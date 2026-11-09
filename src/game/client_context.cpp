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
#include <cstdlib>
#include <string>
#include <vector>

#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/client_context_core.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"

namespace {

uint8_t* Map(uint32_t va) { return static_cast<uint8_t*>(kernel::HostPtr(va)); }

using Switcher = ctx::Switcher<uint8_t* (*)(uint32_t)>;

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
        // 0x21973 / 0x21D46 / 0x27402, P) to its last referenced fields 0x785B9C..0x785BCC (CL_* code 0x26290, 0x27CE0,
        // 0x285F0, P); the 1 KB buffer at 0x78579C (copied whole at 0x29770) inside. The end is the last field seen,
        // not a proved sizeof (I). cls.state is also read by the server's code: player 1's, as for cl.
        {0x7812E0, 0x48F0, "cls"},
    // the rest, from the audit's policy (generated; each entry names its writers)
#include "game/client_context_regions.inc"
    };
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

bool ClientContextSwitch(int k) { return Contexts().Switch(k); }

}  // namespace csx

// [[hook]] at 0x202C1E in the title's startup (0x202BC8), after the C runtime's static constructors (0x206E5B) and
// before main (0xBF470): the per-player regions as a new context starts them (the client game's objects constructed,
// nothing initialised by the engine yet)
void ClientContextsInitial(X86Context&, uint8_t*) {
    if (csx::Dedicated()) return;  // no client in a dedicated server
    Switcher& s = Contexts();
    if (s.Regions().empty()) return;
    s.CaptureInitial();
    LOGI("client context: %zu regions, %zu bytes a player; the initial state kept", s.Regions().size(), s.Bytes());
}

// [[hook]] at 0x5D3C2 in _Host_Frame, after a player's CL_Move: the next player's turn (false: back to 0x5D3B5), or
// player 0 again and on to SV_Frame (true)
bool ClientContextsMoveNext(X86Context&, uint8_t*) {
    Switcher& s = Contexts();
    LOG_FIRST(1, "client context: the players' input turns run (%d player%s)", s.Count(), s.Count() == 1 ? "" : "s");
    if (s.Count() == 1) return true;
    int next = ctx::NextTurn(s.Active(), s.Count());
    if (next >= 0) return !s.Switch(next);
    s.Switch(0);
    return true;
}

// [[hook]] at 0x5D54A in _Host_Frame, after a player's client half: the next player's turn (false: back to 0x5D3F0),
// or player 0 again and on to the frame's end (true)
bool ClientContextsFrameNext(X86Context&, uint8_t*) {
    Switcher& s = Contexts();
    LOG_FIRST(1, "client context: the players' client turns run (%d player%s)", s.Count(), s.Count() == 1 ? "" : "s");
    if (s.Count() == 1) return true;
    int next = ctx::NextTurn(s.Active(), s.Count());
    if (next >= 0) return !s.Switch(next);
    s.Switch(0);
    return true;
}
