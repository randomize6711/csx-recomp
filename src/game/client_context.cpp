// Native split-screen client contexts in the game (docs/native_splitscreen.md, M1; the switch itself:
// client_context_core.h). One context per local player; context 0 is the player the game always had. With one
// context (every game today) the hooks below do nothing.
//
// _Host_Frame (0x5D2E0) takes turns:
//   - each player's input (CL_Move 0x2AC80, run before SV_Frame when this process's server is active, 0x5D3B5..0x5D3C2)
//     so the listen server gets every local player's command in the same frame: hook at 0x5D3C2 (on_false 0x5D3B5);
//   - SV_Frame (0xA1D20) once;
//   - each player's client half (0x5D3F0..0x5D545: packets, prediction, entities, the client game's frame, the
//     screen): hook at 0x5D54A (on_false 0x5D3F0).
// After the last player's turn player 0 is active again, so everything else in the frame (the server, the console,
// the timers) sees the state it always saw.
//
// The regions (Regions below) are one player's client state: each entry gives its evidence. The list grows with
// the audit (tools/state_audit.py); tests/client_context_test.py checks it against the audit's classes: a region
// may not hold data that only the server's code touches.
#include "game/client_context.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/client_context_core.h"
#include "game/net_engine.h"
#include "game/table_fingerprint.h"
#include "kernel/kernel.h"

namespace {

uint8_t* Map(uint32_t va) { return static_cast<uint8_t*>(kernel::HostPtr(va)); }

using Switcher = ctx::Switcher<uint8_t* (*)(uint32_t)>;

bool g_in_turn = false;  // between a turn's start (0x5D3B5, 0x5D3F0) and the end of the last player's turn
bool g_in_server_frame = false;  // between 0x5D3CD and 0x5D3D2 (SV_Frame's call): the built-in server's frame

// the generated per-player objects (tools/region_model.py), and their fingerprint as compiled (table_fingerprint.h;
// tools/check_tables.py compares it with the source tree's table)
struct GenRegion {
    uint32_t va, size;
    const char* what;
};
constexpr GenRegion kGenRegions[] = {
#include "game/client_context_regions.inc"
};
[[gnu::used]] const tablefp::Stamp kRegionsStamp = {"CSXFP1:regions", tablefp::Ranges(kGenRegions),
                                                    uint32_t(sizeof(kGenRegions) / sizeof(kGenRegions[0])),
                                                    tablefp::Bytes(kGenRegions)};

Switcher& Contexts() {
    static Switcher s(csx::ClientContextRegions(), &Map);
    return s;
}

}  // namespace

namespace csx {

// One player's client state (sorted by address). (P) proved from the code; see docs/native_splitscreen.md section 4
// and analysis/player_slots_engine_client.md.
// Read by the server's code too (a listen server reads its own player's client state, P): cl.maxclients 0x778A5C,
// cl.playernum 0x771224, cl.frames fields 0x74A27C / 0x74A290. The server runs while player 0 is active, so it reads
// the host player's values, as the title does.
std::vector<ctx::Region> ClientContextRegions() {
    std::vector<ctx::Region> r = {
        // the client game's g_TeamInfo[18] (P: memset 0xC6 dwords; 0x2C x 18; indexed by team, not moved by the player
        // slots work)
        {0x548A28, 0x318, "g_TeamInfo"},
        // the client game's CHud gHUD and the HUD elements it embeds (constructed by 0x1A6BB0 from the C runtime's
        // initializer 0x23EB60, P), up to the next constructed static object 0x569398 (an upper bound, I). Read by the
        // server's code at +0x1BC0 (0x563F90, the player method 0x11D930): player 1's, docs 10.1
        {0x5623D0, 0x6FC8, "gHUD"},
        // the client game's CVoiceStatus g_VoiceStatus (constructed by 0x1B7DD0 from the initializer 0x23ECA0, P), up to
        // the array that follows it at 0x576C78 (0x1CC4A0, P); its per player arrays are in the guest tables below
        {0x56D6E8, 0x9590, "g_VoiceStatus"},
        // client_state_t cl (P: CL_ClearState's rep stosl of 0xDC22 dwords at 0x28E37)
        {0x749D40, 0x37088, "cl"},
        // client_static_t cls: from cls.state (0x7812E0, its first field; &cls is Netchan_Setup's connection status at
        // 0x21973 / 0x21D46 / 0x27402, P) to the end of its last member, the 1 KB buffer at 0x78579C (copied whole at
        // 0x29770, P), at 0x785B9C. After it: separate client globals (0x785B9C..0x785BAC, in the generated list) and
        // cl_visedicts[512] at 0x785BC0..0x7863C0 (filled at 0x1CA01 with its count 0x749D20 < 0x200, P: rebuilt every
        // frame). cls.state is also read by the server's code: player 1's, as for cl.
        {0x7812E0, 0x48BC, "cls"},
    };
    // the rest, from the audit's policy (generated; each entry names its writers)
    for (const GenRegion& g : kGenRegions) r.push_back({g.va, g.size, g.what});
    // the client's per-player arrays the 65 player slots moved into the guest tables (config/guest_tables.toml): their
    // old places in .data (predicted_players 0x3AE3D0, g_PlayerInfoList 0x5487B0, g_PlayerExtraInfo 0x57E978,
    // DM_PlayerState 0x6A38A0, ...) are no longer used. The server's tables (sv_*) and the network's scratch (net_*)
    // and the command line templates are not a player's.
    for (size_t k : {guest_tables::k_cl_frame_playerstates, guest_tables::k_cl_players, guest_tables::k_cl_predicted_players,
                     guest_tables::k_dm_playerstate, guest_tables::k_hud_extra_info, guest_tables::k_hud_info_list,
                     guest_tables::k_hud_is_spectator, guest_tables::k_hud_player_record, guest_tables::k_hud_player_flag,
                     guest_tables::k_hud_spectator_positions, guest_tables::k_hud_player_fov,
                     guest_tables::k_voice_status_masks, guest_tables::k_voice_talk_time, guest_tables::k_voice_labels,
                     guest_tables::k_voice_heads, guest_tables::k_studio_player_state}) {
        const guest_tables::Table& t = guest_tables::kTables[k];
        r.push_back({t.va, t.elem * t.count, t.name});
    }
    std::sort(r.begin(), r.end(), [](const ctx::Region& a, const ctx::Region& b) { return a.start < b.start; });
    if (std::string e = ctx::CheckRegions(r); !e.empty()) {
        LOGE("client context: the region list is wrong (%s): native split-screen off", e.c_str());
        r.clear();
    }
    return r;
}

int ClientContextCount() { return Contexts().Count(); }
int ClientContextActive() { return Contexts().Active(); }

int ClientContextAdd() {
    if (Contexts().Regions().empty()) return -1;  // the list failed its checks: one player only
    int k = Contexts().Add();
    LOGI("client context: player %d added (%zu bytes of client state each)", k + 1, Contexts().Bytes());
    return k;
}

namespace {
double g_switch_ms = 0;
uint64_t g_switches = 0;
// a switch, timed (the frame's turns and the players' additions all switch through here)
bool TimedSwitch(int k) {
    if (k == Contexts().Active()) return Contexts().Switch(k);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = Contexts().Switch(k);
    g_switch_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_switches++;
    return ok;
}
}  // namespace
bool ClientContextSwitch(int k) { return TimedSwitch(k); }
double ClientContextSwitchMs(uint64_t& switches) {
    switches = g_switches;
    return g_switch_ms;
}
bool ClientContextRemove(int k) { return Contexts().Remove(k); }
bool ClientContextHasInitial() { return Contexts().HasInitial(); }
std::vector<uint8_t>& ClientContextInitial() { return Contexts().Initial(); }
std::vector<uint8_t>& ClientContextSaved(int k) { return Contexts().Saved(k); }
uint32_t ClientContextAddressOf(size_t offset) { return Contexts().AddressOf(offset); }
long ClientContextOffsetOf(uint32_t va) { return Contexts().OffsetOf(va); }

bool ClientContextInTurn() { return g_in_turn; }
bool ClientContextInServerFrame() { return g_in_server_frame; }

}  // namespace csx

namespace {

// the diagnostic guard (CSX_CTX_GUARD=1; docs/native_splitscreen.md 10.5): the per-player regions copied before SV_Frame
// and compared after it, so a server frame that writes into a player's client state is logged (region, offset, the
// value before and after) instead of silently changing player 1's copy. Off by default: the frame as before.
struct Guard {
    bool on = false;
    std::vector<uint8_t> before;
    uint64_t frames = 0, dirty = 0;
    double copy_ms = 0, compare_ms = 0;  // totals, for the averages logged
    std::vector<int> reports;            // per region, writes reported so far
    std::chrono::steady_clock::time_point last_summary{};
};
Guard& TheGuard() {
    static Guard g = [] {
        Guard x;
        const char* v = std::getenv("CSX_CTX_GUARD");
        x.on = v && *v == '1' && !csx::Dedicated();
        return x;
    }();
    return g;
}
double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

// [[hook]] at 0x5D3CD in _Host_Frame, before SV_Frame's call: the guard's copy of every per-player region
void ClientContextsGuardBefore(X86Context&, uint8_t*) {
    g_in_server_frame = true;  // the console lines the server adds are the process's (native_split.cpp's routing)
    Guard& g = TheGuard();
    if (!g.on) return;
    const auto& regions = Contexts().Regions();
    if (regions.empty()) return;
    auto t0 = std::chrono::steady_clock::now();
    g.before.resize(Contexts().Bytes());
    size_t at = 0;
    for (const ctx::Region& r : regions) {
        std::memcpy(g.before.data() + at, Map(r.start), r.size);
        at += r.size;
    }
    g.copy_ms += Ms(t0, std::chrono::steady_clock::now());
}

// [[hook]] at 0x5D3D2 in _Host_Frame, after SV_Frame: what the server frame wrote into a player's client state
void ClientContextsGuardAfter(X86Context&, uint8_t*) {
    g_in_server_frame = false;
    Guard& g = TheGuard();
    if (!g.on || g.before.empty()) return;
    const auto& regions = Contexts().Regions();
    auto t0 = std::chrono::steady_clock::now();
    g.reports.resize(regions.size());
    bool dirty = false;
    size_t at = 0;
    for (size_t i = 0; i < regions.size(); i++) {
        const ctx::Region& r = regions[i];
        const uint8_t* live = Map(r.start);
        const uint8_t* old = g.before.data() + at;
        at += r.size;
        if (std::memcmp(live, old, r.size) == 0) continue;
        dirty = true;
        if (g.reports[i] >= 5) continue;
        g.reports[i]++;
        uint32_t off = 0;
        while (off < r.size && live[off] == old[off]) off++;
        uint32_t last = r.size;
        while (last > off && live[last - 1] == old[last - 1]) last--;
        uint32_t was = 0, now = 0;
        std::memcpy(&was, old + (off & ~3u), std::min<uint32_t>(4, r.size - (off & ~3u)));
        std::memcpy(&now, live + (off & ~3u), std::min<uint32_t>(4, r.size - (off & ~3u)));
        LOGW("client context guard: the server frame wrote %s at 0x%08X (+0x%X, %u bytes changed in the span to +0x%X): "
             "0x%08X -> 0x%08X [own:guard-write]",
             r.what, r.start + off, off, last - off, last, was, now);
    }
    g.frames++;
    g.dirty += dirty;
    auto t1 = std::chrono::steady_clock::now();
    g.compare_ms += Ms(t0, t1);
    if (g.last_summary == std::chrono::steady_clock::time_point{} || t1 - g.last_summary > std::chrono::seconds(30)) {
        g.last_summary = t1;
        LOGI("client context guard: %llu server frames checked, %llu wrote client state; %zu bytes a check, copy %.3f ms "
             "and compare %.3f ms on average",
             (unsigned long long)g.frames, (unsigned long long)g.dirty, Contexts().Bytes(),
             g.frames ? g.copy_ms / double(g.frames) : 0.0, g.frames ? g.compare_ms / double(g.frames) : 0.0);
    }
}

// [[hook]] at 0x1CB29 in ClientDLL_HudVidInit (0x1CAC0), just before the client game's HUD_VidInit ([0x7D6CA8]): which
// player's HUD is initialised (log only; M2 checks that every player initialises its own)
void ClientContextsHudInit(X86Context&, uint8_t*) {
    LOG_FIRST(8, "client context: the HUD's video initialisation for player %d (of %d)", Contexts().Active() + 1,
              Contexts().Count());
}

bool NativeSplitOn();                                     // native_split.cpp
void NativeSplitReference(X86Context& ctx, const std::vector<uint8_t>& state);  // native_split.cpp

// [[hook]] at 0x202C1E in the title's startup (0x202BC8), after the C runtime's static constructors (0x206E5B) and
// before main (0xBF470): the per-player regions as a new player's context starts them (the boot image: the client
// game's static objects constructed, no heap object made, nothing initialised by the engine). With native
// split-screen only. A new player is then built by explicit steps in its own context (native_split.cpp,
// docs/native_splitscreen.md 13.2): it inherits nothing of player 1's.
void ClientContextsInitial(X86Context&, uint8_t*) {
    if (csx::Dedicated() || !NativeSplitOn()) return;
    Switcher& s = Contexts();
    if (s.Regions().empty()) return;
    auto t0 = std::chrono::steady_clock::now();
    s.CaptureInitial();
    LOGI("client context: %zu regions, %zu bytes a player; the boot image kept (%.3f ms)%s", s.Regions().size(),
         s.Bytes(), Ms(t0, std::chrono::steady_clock::now()), TheGuard().on ? "; the server write guard is on" : "");
    LOGI("client context: generated regions %u, %u bytes, fingerprint %016llX (tools/check_tables.py)",
         kRegionsStamp.count, kRegionsStamp.bytes, (unsigned long long)kRegionsStamp.hash);
}

// [[hook]] at 0x5DD94 at the end of Host_Init (0x5D9E0), as it sets host_initialized (0x551D7C): player 1's regions
// once the process is initialised and before any connection, a reference only (native_split.cpp checks a new
// player's initialised state against it: every object player 1's initialisation made must be the new player's own).
// With native split-screen only.
void ClientContextsReference(X86Context& ctx, uint8_t*) {
    if (csx::Dedicated() || !NativeSplitOn() || Contexts().Regions().empty()) return;
    std::vector<uint8_t> state(Contexts().Bytes());
    size_t at = 0;
    for (const ctx::Region& r : Contexts().Regions()) {
        std::memcpy(state.data() + at, Map(r.start), r.size);
        at += r.size;
    }
    NativeSplitReference(ctx, state);
}

void NativeSplitClientTurn(X86Context& ctx);  // native_split.cpp: the turn's view of the window frame (M3)
void NativeSplitClientTurnEnd();               // native_split.cpp
void NativeSplitPadTurnBegin();                // native_split.cpp: an added player's own controller in its turn
void NativeSplitPadTurnEnd();                  // native_split.cpp

// [[hook]] at 0x5D3F0 in _Host_Frame: a player's client half starts (with native split-screen: its view, its pad)
void ClientContextsTurnBegin(X86Context& ctx, uint8_t*) {
    g_in_turn = true;
    if (NativeSplitOn()) {
        NativeSplitClientTurn(ctx);
        NativeSplitPadTurnBegin();
    }
}

void NativeSplitFrameEnd(X86Context& ctx);     // native_split.cpp
void NativeSplitTurnPrelude(X86Context& ctx);  // native_split.cpp
void NativeSplitAborted(int context);          // native_split.cpp

// [[hook]] at 0x5D3B5 in _Host_Frame: a player's input turn starts (its CL_Move). Every context but player 1's first
// runs the frame's per-player prelude the title runs before the turns, in player 1's context only (0x5D3A6 / 0x5D3B0:
// the activation check, the client data update; native_split.cpp, docs 15.10); with native split-screen every
// player, player 1 too, then runs its own console lines (docs 15.12). Nothing more without native split-screen
void ClientContextsMoveBegin(X86Context& ctx, uint8_t*) {
    g_in_turn = true;
    g_in_server_frame = false;
    if (NativeSplitOn()) {
        NativeSplitPadTurnBegin();
        NativeSplitTurnPrelude(ctx);
    }
}

// [[hook]] at 0x5D2F4 in _Host_Frame and 0x5D5F0 in Host_Frame, the `test eax, eax` after their setjmp (0x6CE000,
// 0x6CE060): eax not 0 is a frame cut short by Host_EndGame (0x5BCF1, 0x5BD1C) or Host_Error (0x5BE1B), whose longjmp
// unwinds whatever turn was running. With native split-screen, a turn of another player than player 1 would leave its
// context live for the next frame (its prelude and the console buffer then run as that player): player 1's context
// is made live again and the turns' state cleared (docs 15.12). Nothing else changes: the frame returns as the title's
void ClientContextsAbortLanding(X86Context& ctx, uint8_t*) {
    if (!ctx.eax) return;
    g_in_server_frame = false;
    Switcher& s = Contexts();
    int was = s.Active();
    bool in_turn = g_in_turn;
    if (NativeSplitOn()) NativeSplitPadTurnEnd();
    g_in_turn = false;
    if (s.Count() > 1 && was != 0) csx::ClientContextSwitch(0);
    if (NativeSplitOn() && (was != 0 || in_turn)) NativeSplitAborted(was);
}

// [[hook]] at 0x5D3C2 in _Host_Frame, after a player's CL_Move: the next player's turn (false: back to 0x5D3B5), or
// player 0 again and on to SV_Frame (true)
bool ClientContextsMoveNext(X86Context&, uint8_t*) {
    if (NativeSplitOn()) NativeSplitPadTurnEnd();
    Switcher& s = Contexts();
    LOG_FIRST(1, "client context: the players' input turns run (%d player%s)", s.Count(), s.Count() == 1 ? "" : "s");
    if (s.Count() > 1) {
        int next = ctx::NextTurn(s.Active(), s.Count());
        if (next >= 0) return !csx::ClientContextSwitch(next);
        csx::ClientContextSwitch(0);
    }
    g_in_turn = false;
    return true;
}

// [[hook]] at 0x5D54A in _Host_Frame, after a player's client half: the next player's turn (false: back to 0x5D3F0),
// or player 0 again and on to the frame's end (true)
bool ClientContextsFrameNext(X86Context& ctx, uint8_t*) {
    if (NativeSplitOn()) {
        NativeSplitPadTurnEnd();
        NativeSplitClientTurnEnd();
    }
    Switcher& s = Contexts();
    LOG_FIRST(1, "client context: the players' client turns run (%d player%s)", s.Count(), s.Count() == 1 ? "" : "s");
    if (s.Count() > 1) {
        int next = ctx::NextTurn(s.Active(), s.Count());
        if (next >= 0) return !csx::ClientContextSwitch(next);
        csx::ClientContextSwitch(0);
    }
    g_in_turn = false;
    NativeSplitFrameEnd(ctx);  // players added or removed between frames (nothing without native split-screen)
    return true;
}
