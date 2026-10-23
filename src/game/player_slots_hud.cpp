// 65 player slots (G8.2B): the client HUD's hooks. Its per player tables moved to guest tables sized for the HUD's
// MAX_PLAYERS of N + 1 (config/player_slots.toml); these hooks check the player indices the server's messages carry
// where the original writes a table with them unchecked (a malformed or hostile message must not write past a
// table), clear what the original's unrolled stores cannot reach, and read and write the voice masks, which are 3
// dwords now (VoiceMask, vban).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "common/guest_tables.h"
#include "common/guest_tables_layout.h"
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
constexpr uint32_t kHudMax = N + 1;  // the HUD's MAX_PLAYERS: its tables hold [kHudMax + 1]
constexpr uint32_t kDwords = (N + 31) / 32;

uint32_t Table(size_t k) { return guest_tables::Production(k).va; }

// a refused index is logged once per kind
bool Refuse(const char* what, uint32_t index) {
    static uint64_t seen = 0;
    uint64_t bit = uint64_t(1) << (std::hash<const char*>()(what) % 64);
    if (!(seen & bit)) LOGW("player slots: %s names player %u (at most %u): ignored", what, index, N);
    seen |= bit;
    return false;
}

// the user message reader's state (READ_* in the client DLL: read count, buffer, bad read, size)
constexpr uint32_t kReadCount = 0x548F3C, kReadBuf = 0x548F40, kBadRead = 0x548F44, kReadSize = 0x548F48;

uint32_t ReadBytes(uint32_t n) {
    uint32_t at = R32(kReadCount), size = R32(kReadSize), buf = R32(kReadBuf);
    if (at + n > size) {
        W32(kBadRead, 1);
        return 0xFFFFFFFFu;
    }
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) v |= uint32_t(*static_cast<uint8_t*>(kernel::HostPtr(buf + at + i))) << (8 * i);
    W32(kReadCount, at + n);
    return v;
}

void BeginRead(uint32_t buf, uint32_t size) {
    W32(kReadCount, 0);
    W32(kBadRead, 0);
    W32(kReadSize, size);
    W32(kReadBuf, buf);
}

}  // namespace

// [[hook]] at 0x1A7423 (the per player field of view message, esi = the player, 0 = everyone): an index past the
// table is refused (false: 0x1A742E, nothing stored)
bool SlotsHudFovIndex(X86Context& ctx, uint8_t*) { return ctx.esi <= kHudMax || Refuse("the field of view message", ctx.esi); }

// [[hook]] at 0x1A73D3 (the same message's other form, g_PlayerExtraInfo[esi] +0x44): false: 0x1A73E0
bool SlotsHudFovExtraIndex(X86Context& ctx, uint8_t*) {
    return ctx.esi <= kHudMax || Refuse("the field of view message", ctx.esi);
}

// [[hook]] at 0x1A7469 (the field of view for everyone: the original's 17 stores, eax = the moved table, ebx = the
// value): the entries past them
void SlotsHudFovAll(X86Context& ctx, uint8_t*) {
    for (uint32_t i = 17; i <= kHudMax; i++) W32(ctx.eax + 4 * i, ctx.ebx);
}

// [[hook]] at 0x1D59F0 (the Radar message, esi = player * 0x4C): an index past the table is refused (0x1D59FE)
bool SlotsHudRadarIndex(X86Context& ctx, uint8_t*) {
    uint32_t i = ctx.esi / 0x4C;
    return i <= kHudMax || Refuse("the Radar message", i);
}

// [[hook]] at 0x1D6C9D (DeathMsg, the killer in ebp, the victim at [esp+0x18], -1 when missing, 0 the world): a
// player past the slots drops the message (0x1D724E: handled, nothing shown)
bool SlotsHudDeathMsg(X86Context& ctx, uint8_t*) {
    uint32_t killer = ctx.ebp, victim = R32(ctx.esp + 0x18);
    auto ok = [](uint32_t v) { return v == 0xFFFFFFFFu || v == 0xFF || v <= N; };
    if (ok(killer) && ok(victim)) return true;
    return Refuse("DeathMsg", ok(killer) ? victim : killer);
}

// [[hook]] at 0x1D823C and 0x1D8344 (two messages that mark a player talking: esi / edi): false skips the store
// (0x1D825B / 0x1D8363)
bool SlotsHudTalkIndexEsi(X86Context& ctx, uint8_t*) { return ctx.esi <= kHudMax || Refuse("a talk message", ctx.esi); }
bool SlotsHudTalkIndexEdi(X86Context& ctx, uint8_t*) { return ctx.edi <= kHudMax || Refuse("a talk message", ctx.edi); }

// [[hook]] at 0x1B9354 (ScorePanel::SortTeams, esi = the panel): its has-been-sorted flags (moved after its fields,
// N + 2 bytes: config/player_slots.toml) cleared whole
void SlotsScoreSortedClear(X86Context& ctx, uint8_t*) {
    constexpr uint32_t kSorted = 0x294 + 2 * (N + 11) * 4;
    std::memset(kernel::HostPtr(ctx.esi + kSorted), 0, 0x44);
}

// --- CVoiceStatus ---

namespace {

// its five bit vectors, 3 dwords each (voice_status_masks): +0x24 speaking, +0x28 audible, +0x2C voice enabled,
// +0x30 server banned, +0x958C seen
constexpr uint32_t kAudible = 0xC, kServerBanned = 0x24;
constexpr uint32_t kLevelName = 0x543210, kCvarGetFloat = 0x543124, kServerCmd = 0x543134,
                   kServerCmdUnreliable = 0x54326C, kGetPlayerInfo = 0x543260, kClientTime = 0x5431C0;
constexpr uint32_t kBanManager = 0x7CCC, kIsBanned = 0x1DE320;

float PopFloat(X86Context& ctx) {
    float v = float(ctx.st[ctx.top & 7]);
    ctx.top = (ctx.top + 1) & 7;
    return v;
}

// guest stack room for the strings and the player info the calls take, below the caller's frame
struct Scratch {
    X86Context& ctx;
    uint32_t saved, at;
    explicit Scratch(X86Context& c) : ctx(c), saved(c.esp), at((c.esp - 0x800) & ~0xFu) { ctx.esp = at; }
    ~Scratch() { ctx.esp = saved; }
    uint32_t Text(uint32_t offset, const char* s) {
        std::strcpy(static_cast<char*>(kernel::HostPtr(at + offset)), s);
        return at + offset;
    }
};

}  // namespace

// CVoiceStatus::HandleVoiceMaskMsg (0x1B8C80, ecx = the message, edx = its size, ebx = this): the audible and server
// banned masks, 3 dwords each, then the server's voice mod flag (a byte; -1 when the message ends before it)
X86_FUNC(sub_001B8C80) {
    (void)base;
    BeginRead(ctx.ecx, ctx.edx);
    uint32_t masks = Table(guest_tables::k_voice_status_masks);
    for (uint32_t d = 0; d < kDwords; d++) W32(masks + kAudible + 4 * d, ReadBytes(4));
    for (uint32_t d = 0; d < kDwords; d++) W32(masks + kServerBanned + 4 * d, ReadBytes(4));
    uint32_t b = ReadBytes(1);
    W32(ctx.ebx + 0x1C, b);
    ctx.esp += 4;
}

// CVoiceStatus::UpdateServerState (0x1B8850, [esp+4] = this, [esp+8] = force): tells the server whether voice is on
// ("VModEnable") and which players this client banned ("vban" with 3 hex dwords), each when it changed or forced
X86_FUNC(sub_001B8850) {
    (void)base;
    uint32_t self = R32(ctx.esp + 4);
    bool force = (R32(ctx.esp + 8) & 0xFF) != 0;
    {
        Scratch s(ctx);
        uint32_t level = CallGuest(ctx, R32(kLevelName), {});
        if (level && *static_cast<char*>(kernel::HostPtr(level))) {
            CallGuest(ctx, R32(kCvarGetFloat), {s.Text(0x700, "voice_modenable")});
            uint32_t mod = PopFloat(ctx) != 0.0f ? 1 : 0;
            if (force || R32(self + 0x1C) != mod) {
                W32(self + 0x1C, mod);
                char cmd[32];
                std::snprintf(cmd, sizeof cmd, "VModEnable %u", mod);
                CallGuest(ctx, R32(kServerCmd), {s.Text(0x600, cmd)});
            }
            uint32_t banned[kDwords] = {}, info = s.at + 0x100;
            for (uint32_t i = 0; i < N; i++)
                if (CallGuest(ctx, R32(kGetPlayerInfo), {i + 1, info}) &&
                    CallGuest(ctx, kIsBanned, {self + kBanManager, info}))
                    banned[i / 32] |= 1u << (i % 32);
            uint32_t masks = Table(guest_tables::k_voice_status_masks);
            bool changed = false;
            for (uint32_t d = 0; d < kDwords; d++) changed |= banned[d] != R32(masks + kServerBanned + 4 * d);
            if (changed || force) {
                char cmd[64];
                std::snprintf(cmd, sizeof cmd, "vban %x %x %x", banned[0], banned[1], banned[2]);
                CallGuest(ctx, R32(kServerCmdUnreliable), {s.Text(0x600, cmd)});
            }
            CallGuest(ctx, R32(kClientTime), {});
            float t = PopFloat(ctx);
            std::memcpy(kernel::HostPtr(self + 0x18), &t, 4);
        }
    }
    ctx.esp += 12;
}

// --- StatShot (client) ---

// the StatShot message's handler (0x1A5FC0, [esp+8] = size, [esp+0xC] = the message): the enemy and team hit masks
// (3 dwords each), the damage dealt and taken and the weapon, passed to the statistics tracker as before (it counts
// a hit when a mask is not zero; each mask is passed folded into one non-zero dword)
X86_FUNC(sub_001A5FC0) {
    (void)base;
    BeginRead(R32(ctx.esp + 0xC), R32(ctx.esp + 8));
    uint32_t enemy = 0, team = 0;
    bool bad = false;
    for (uint32_t d = 0; d < kDwords; d++) {
        uint32_t v = ReadBytes(4);
        bad |= R32(kBadRead) != 0;
        enemy |= v;
    }
    for (uint32_t d = 0; d < kDwords; d++) team |= ReadBytes(4);
    uint32_t dealt = ReadBytes(1), taken = ReadBytes(1), weapon = ReadBytes(1);
    if (bad) enemy = team = 0xFFFFFFFFu;  // as the original passes a short it could not read
    uint32_t tracker = R32(0x552C98);
    if (tracker) {
        uint32_t saved = ctx.ecx;
        ctx.ecx = tracker;
        CallGuest(ctx, R32(R32(tracker) + 0xC), {weapon, enemy, team, dealt, taken});
        ctx.ecx = saved;
    }
    ctx.eax = 1;
    ctx.esp += 4;
}

// [[hook]] at 0x190E53 (StudioDrawPlayer, ecx = the drawn player's entity number - 1): its per player state
// (g_StudioRenderer +0x40B0, [17] in the original, which players 18 and up wrote past) is kept in studio_player_state;
// an entity number past it skips the state (0x190E7E) instead of indexing out of the table
bool SlotsStudioPlayerIndex(X86Context& ctx, uint8_t*) {
    return ctx.ecx < guest_tables::Production(guest_tables::k_studio_player_state).count;
}

// --- the scoreboard's pages ---

namespace {
constexpr uint32_t kPageRows = 20;       // the rows the scoreboard's list shows (the HUD's 640x480 layout)
constexpr double kPageSeconds = 5.0;     // a page shown this long before the next
std::chrono::steady_clock::time_point g_scores_seen, g_page_start;
uint32_t g_page = 0;
}  // namespace

// [[hook]] at 0x1B98A0 (ScorePanel::FillGrid's row loop, ebp = the panel, [esp+0x44] = the row): with 65 players the
// rows (two team headers, the players, spacers) pass what the list shows (about 20; it does not scroll), so the
// scoreboard pages: 20 rows at a time, the next page every 5 s while it stays open, the first page each time it opens;
// a row off the page is skipped (false: 0x1B9979, the loop's next row)
bool SlotsScoreRowShown(X86Context& ctx, uint8_t*) {
    auto now = std::chrono::steady_clock::now();
    uint32_t row = R32(ctx.esp + 0x44), rows = R32(ctx.ebp + 0x19C) + 1;
    if (row == 0) {
        if (now - g_scores_seen > std::chrono::milliseconds(500)) g_page = 0, g_page_start = now;  // just opened
        g_scores_seen = now;
        uint32_t pages = (rows + kPageRows - 1) / kPageRows;
        if (pages <= 1) {
            g_page = 0;
        } else if (std::chrono::duration<double>(now - g_page_start).count() >= kPageSeconds) {
            g_page = (g_page + 1) % pages;
            g_page_start = now;
        }
        g_page %= std::max<uint32_t>(pages, 1);
    }
    return row / kPageRows == g_page;
}
