// 65 player slots (G8.2B): the game DLL's player masks and tables that the original keeps in 32 bits or for 17
// players, in host code (docs/player_slots_plan.md; the operand patches are config/player_slots_patches.toml).
//
// - CVoiceGameMgr (voice_gamemgr.cpp of the HL SDK, 0x102C80..0x103370): its masks are arrays of one-dword bit
//   vectors indexed with a x4 scale, which no operand patch can widen, so ClientConnected, PlayerHasBlockedPlayer,
//   ClientCommand and Update are replaced here with the same logic on 65-bit masks kept in host memory (nothing else
//   reads them). Init stays the original (patched: N players, the VoiceMask message's size). VoiceMask carries the
//   two masks as 3 dwords each; "vban" takes up to 3 hex dwords.
// - StatShot (the shot statistics, 0x153800..0x1539E0): the damage per player grows to N (patched allocation), its
//   reset clears all of it (hook), AddDamage refuses an index past N (hook), and Send writes the enemy and team
//   masks as 3 dwords each (the original's 16-bit shorts dropped players 17 and up).
// - trigger_hurt's per-tick player mask (pev->impulse, 1 << (entindex - 1): players 33 and up alias 1..32) is a
//   65-bit set per trigger in host memory.
// - Team sizes: a team was full at its map's spawn point count; the counts the game rules keep for that test are
//   set to the slot count once counted (the map's own points are reused by the game's spawn choice).
#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/player_slots.h"
#include "kernel/kernel.h"

using kernel::CallGuest;
using kernel::R32;
using kernel::W32;

namespace {

constexpr uint32_t N = slots::kMaxPlayers;
constexpr uint32_t kDwords = (N + 31) / 32;
using Mask = std::bitset<N>;

// the engine's functions for the game DLL (enginefuncs_t, copied to 0x527A80..)
constexpr uint32_t kMessageBegin = 0x527B58, kMessageEnd = 0x527B5C, kWriteByte = 0x527B60, kWriteLong = 0x527B6C;
constexpr uint32_t kCvarGetFloat = 0x527B84, kEntIndex = 0x527BBC, kEntOfIndex = 0x527BC0, kCmdArgv = 0x527BEC,
                   kCmdArgc = 0x527BF0, kSetClientListening = 0x527CD8;
constexpr uint32_t kGlobals = 0x527CE8, kGlobalsMaxClients = 0x90;
constexpr uint32_t kMsgOne = 1;

uint32_t Call(X86Context& ctx, uint32_t slot, std::initializer_list<uint32_t> args) {
    return CallGuest(ctx, R32(slot), args);
}

// a float a guest call returned on the x87 stack
float PopFloat(X86Context& ctx) {
    float v = float(ctx.st[ctx.top & 7]);
    ctx.top = (ctx.top + 1) & 7;
    return v;
}

std::string GuestString(uint32_t va, size_t max = 256) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < max; i++) {
        char c = char(*static_cast<uint8_t*>(kernel::HostPtr(va + uint32_t(i))));
        if (!c) break;
        s += c;
    }
    return s;
}

// CBaseEntity* -> edict_t* (pev at +4, pev->pContainingEntity at +0x208)
uint32_t EdictOf(uint32_t entity) { return entity ? R32(R32(entity + 4) + 0x208) : 0; }

// the player CBasePlayer* in slot i (0-based) when it is a valid player, else 0 (CVoiceGameMgr's own test: the
// edict in use, its private data, IsPlayer at vtable +0xAC)
uint32_t Player(X86Context& ctx, uint32_t i) {
    uint32_t g = R32(kGlobals);
    if (!g || i + 1 > R32(g + kGlobalsMaxClients)) return 0;
    uint32_t e = Call(ctx, kEntOfIndex, {i + 1});
    if (!e || R32(e)) return 0;
    uint32_t p = R32(e + 0x7C);
    if (!p) return 0;
    uint32_t saved = ctx.ecx;
    ctx.ecx = p;
    uint32_t is = CallGuest(ctx, R32(R32(p) + 0xAC), {});
    ctx.ecx = saved;
    return is ? p : 0;
}

void WriteMask(X86Context& ctx, const Mask& m) {
    for (uint32_t d = 0; d < kDwords; d++) {
        uint32_t v = 0;
        for (uint32_t b = 0; b < 32 && d * 32 + b < N; b++) v |= uint32_t(m[d * 32 + b]) << b;
        Call(ctx, kWriteLong, {v});
    }
}

// --- CVoiceGameMgr ---
struct VoiceGame {
    Mask mod_enable, want_mod_enable;
    Mask ban[N], sent_ban[N], sent_rules[N];
} g_voice;

bool Same(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
    return true;
}

uint32_t ClientIndex(X86Context& ctx, uint32_t edict) { return Call(ctx, kEntIndex, {edict}) - 1; }

// the CVoiceGameMgr's fields: +0x8 VoiceMask message id, +0xC ReqState id, +0x10 the helper, +0x14 max players,
// +0x18 the update interval
uint32_t MaxPlayers(uint32_t self) { return std::min<uint32_t>(R32(self + 0x14), N); }

}  // namespace

// CVoiceGameMgr::ClientConnected (0x102E30): eax = the client's edict
X86_FUNC(sub_00102E30) {
    (void)base;
    uint32_t i = ClientIndex(ctx, ctx.eax);
    if (i < N) {
        g_voice.want_mod_enable.set(i);
        g_voice.sent_rules[i].reset();
        g_voice.sent_ban[i].reset();
    }
    ctx.esp += 4;
}

// CVoiceGameMgr::PlayerHasBlockedPlayer (0x102EC0): eax = the receiver, edi = the sender (CBasePlayer*), [esp+4] =
// this; al = whether the receiver banned the sender
X86_FUNC(sub_00102EC0) {
    (void)base;
    uint32_t self = R32(ctx.esp + 4), receiver = ctx.eax, sender = ctx.edi;
    bool banned = false;
    if (receiver && sender) {
        uint32_t r = ClientIndex(ctx, EdictOf(receiver)), s = ClientIndex(ctx, EdictOf(sender)), max = MaxPlayers(self);
        banned = r < max && s < max && g_voice.ban[r][s];
    }
    ctx.eax = (ctx.eax & 0xFFFFFF00u) | (banned ? 1u : 0u);
    ctx.esp += 8;
}

// CVoiceGameMgr::ClientCommand (0x102F40): eax = the player, ecx = the command, [esp+4] = this; al = handled.
// "vban M0 [M1 [M2]]" (hex dwords: the players this client banned), "VModEnable 0|1"
X86_FUNC(sub_00102F40) {
    (void)base;
    uint32_t self = R32(ctx.esp + 4);
    std::string cmd = GuestString(ctx.ecx);
    uint32_t i = ClientIndex(ctx, EdictOf(ctx.eax));
    bool handled = false;
    if (i >= MaxPlayers(self)) {
        handled = true;  // the original: "invalid client", handled
    } else if (Same(cmd, "vban") && Call(ctx, kCmdArgc, {}) >= 2) {
        uint32_t argc = Call(ctx, kCmdArgc, {});
        Mask m;
        for (uint32_t a = 1; a < argc && a <= kDwords; a++) {
            uint32_t v = uint32_t(std::strtoul(GuestString(Call(ctx, kCmdArgv, {a})).c_str(), nullptr, 16));
            for (uint32_t b = 0; b < 32 && (a - 1) * 32 + b < N; b++) m[(a - 1) * 32 + b] = v >> b & 1;
        }
        g_voice.ban[i] = m;
        handled = true;
    } else if (Same(cmd, "VModEnable") && Call(ctx, kCmdArgc, {}) >= 2) {
        g_voice.mod_enable[i] = std::atoi(GuestString(Call(ctx, kCmdArgv, {1})).c_str()) != 0;
        g_voice.want_mod_enable.reset(i);
        handled = true;
    }
    ctx.eax = (ctx.eax & 0xFFFFFF00u) | (handled ? 1u : 0u);
    ctx.esp += 8;
}

// CVoiceGameMgr::UpdateMasks (0x1030E0, ebx = this): for each player, ReqState while the client has not said whether
// voice is on, the game rules' hearing mask (sv_alltalk or the helper's CanPlayerHearPlayer), VoiceMask when it or
// the ban mask changed, and the engine's listen bits
X86_FUNC(sub_001030E0) {
    (void)base;
    uint32_t self = ctx.ebx;
    W32(self + 0x18, R32(0x365DE8));  // the update interval restarts (the constant the original loads)
    Call(ctx, kCvarGetFloat, {0x343340});  // "sv_alltalk"
    bool all_talk = PopFloat(ctx) != 0.0f;
    uint32_t max = MaxPlayers(self), helper = R32(self + 0x10);
    for (uint32_t i = 0; i < max; i++) {
        uint32_t player = Player(ctx, i);
        if (!player) continue;
        uint32_t edict = EdictOf(player);
        if (g_voice.want_mod_enable[i]) {
            Call(ctx, kMessageBegin, {kMsgOne, R32(self + 0xC), 0, edict});
            Call(ctx, kMessageEnd, {});
        }
        Mask rules;
        if (g_voice.mod_enable[i])
            for (uint32_t j = 0; j < max; j++) {
                uint32_t other = Player(ctx, j);
                if (!other) continue;
                bool hear = all_talk;
                if (!hear && helper) {
                    uint32_t saved = ctx.ecx;
                    ctx.ecx = helper;
                    hear = (CallGuest(ctx, R32(R32(helper) + 4), {player, other}) & 0xFF) != 0;
                    ctx.ecx = saved;
                }
                rules[j] = hear;
            }
        if (rules != g_voice.sent_rules[i] || g_voice.ban[i] != g_voice.sent_ban[i]) {
            g_voice.sent_rules[i] = rules;
            g_voice.sent_ban[i] = g_voice.ban[i];
            Call(ctx, kMessageBegin, {kMsgOne, R32(self + 0x8), 0, edict});
            WriteMask(ctx, rules);
            WriteMask(ctx, g_voice.ban[i]);
            Call(ctx, kMessageEnd, {});
        }
        for (uint32_t j = 0; j < max; j++)
            Call(ctx, kSetClientListening, {i + 1, j + 1, rules[j] && !g_voice.ban[i][j] ? 1u : 0u});
    }
    ctx.esp += 4;
}

// --- StatShot ---

namespace {
constexpr uint32_t kMsgStatShot = 0x552A4C;
constexpr uint32_t kDamage = 0x10;  // damage to player 1 .. N
}  // namespace

// [[hook]] at 0x153804 (StatShot reset, eax = this): the damage of every player cleared (the original's 17 stores
// follow)
void SlotsStatShotReset(X86Context& ctx, uint8_t*) {
    std::memset(kernel::HostPtr(ctx.eax + kDamage), 0, 4 * N);
}

// [[hook]] at 0x1538CE (StatShot AddDamage, eax = the victim's entity index): an index past the N players is not
// counted (the original indexes its block with it unchecked); false skips the add (0x1538D8)
bool SlotsStatShotIndex(X86Context& ctx, uint8_t*) { return ctx.eax >= 1 && ctx.eax <= N; }

// StatShot Send (0x1538E0, ecx = this, the player at +4, its weapon at +8): the hit masks of enemies (damage > 0)
// and teammates (damage < 0) as 3 dwords each, the damage dealt and taken and the weapon as bytes
X86_FUNC(sub_001538E0) {
    (void)base;
    uint32_t self = ctx.ecx, player = R32(self + 4);
    uint32_t saved = ctx.ecx;
    ctx.ecx = player;
    uint32_t skip = CallGuest(ctx, R32(R32(player) + 0x158), {});  // the original's test (a bot's shots)
    ctx.ecx = saved;
    if (!skip) {
        Mask enemy, team;
        int32_t dealt = 0, taken = 0;
        for (uint32_t k = 0; k < N; k++) {
            int32_t d = int32_t(R32(self + kDamage + 4 * k));
            if (d > 0) enemy.set(k), dealt += d;
            else if (d < 0) team.set(k), taken -= d;
        }
        Call(ctx, kMessageBegin, {kMsgOne, R32(kMsgStatShot), 0, EdictOf(player)});
        WriteMask(ctx, enemy);
        WriteMask(ctx, team);
        Call(ctx, kWriteByte, {uint32_t(dealt) & 0xFF});
        Call(ctx, kWriteByte, {uint32_t(taken) & 0xFF});
        Call(ctx, kWriteByte, {R32(self + 8) & 0xFF});
        Call(ctx, kMessageEnd, {});
        W32(self + 4, 0);
    }
    ctx.esp += 4;
}

// --- trigger_hurt ---

namespace {
std::unordered_map<uint32_t, Mask> g_hurt;  // a trigger's pev -> the players it touched this damage tick

uint32_t TouchingPlayer(X86Context& ctx) { return ClientIndex(ctx, EdictOf(ctx.edi)); }
}  // namespace

// [[hook]] at 0x16B9B2 (CBaseTrigger::HurtTouch, the same damage tick, edi = the player, esi = the trigger): true
// when this player was already hurt this tick (0x16BADB, no damage), else marks it (false: 0x16BA22, damage)
bool SlotsHurtTouched(X86Context& ctx, uint8_t*) {
    uint32_t i = TouchingPlayer(ctx);
    if (i >= N) return false;
    Mask& m = g_hurt[R32(ctx.esi + 4)];
    if (m[i]) return true;
    m.set(i);
    return false;
}

// [[hook]] at 0x16B9DD (a new damage tick): the trigger's set starts empty
void SlotsHurtNewTick(X86Context& ctx, uint8_t*) { g_hurt[R32(ctx.esi + 4)].reset(); }

// [[hook]] at 0x16B9F5 (a new damage tick, the toucher is a player): it is marked
void SlotsHurtMark(X86Context& ctx, uint8_t*) {
    uint32_t i = TouchingPlayer(ctx);
    if (i < N) g_hurt[R32(ctx.esi + 4)].set(i);
}

// --- team sizes ---

// [[hook]] at 0x14837E (CHalfLifeMultiplay::Think, the map's spawn points just counted into +0x5C (Terrorist) and
// +0x60 (Counter-Terrorist)): those counts are what TeamFull and its three inlined copies compare a team's players
// with; a team with points is now full only when the slots are (the points are reused), one without stays closed
void SlotsSpawnCounts(X86Context& ctx, uint8_t*) {
    uint32_t t = R32(ctx.esi + 0x5C), ct = R32(ctx.esi + 0x60);
    LOGI("player slots: the map has %u Terrorist and %u Counter-Terrorist spawn points; teams take up to %u players", t,
         ct, N);
    if (t) W32(ctx.esi + 0x5C, N);
    if (ct) W32(ctx.esi + 0x60, N);
}
