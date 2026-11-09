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

#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/client_context_core.h"
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
    return {
        // predicted_players[17] {movetype, solid, usehull, active, origin, angles} (P: 0x25EEC, 0x26054, loops
        // `cmp $0x11` 0x25FE5 / 0x26276; 0x28 x 17, the next variable at 0x3AE680)
        {0x3AE3D0, 0x2A8, "predicted_players"},
        // the client game's g_PlayerInfoList[18] hud_player_info_t (P: GetPlayerInfo loop 0x1D5A10; 0x14 x 18)
        {0x5487B0, 0x168, "g_PlayerInfoList"},
        // the client game's g_TeamInfo[18] (P: memset 0xC6 dwords; 0x2C x 18)
        {0x548A28, 0x318, "g_TeamInfo"},
        // the client game's g_PlayerExtraInfo[18] (P: memset 0x156 dwords at 0x1B926D; 0x4C x 18)
        {0x57E978, 0x558, "g_PlayerExtraInfo"},
        // the engine's studio player state cache DM_PlayerState[17] (P layout: 0x20C x 17, 7 `imul $0x20c`)
        {0x6A38A0, 0x22CC, "DM_PlayerState"},
        // client_state_t cl (P: CL_ClearState's rep stosl of 0xDC22 dwords at 0x28E37)
        {0x749D40, 0x37088, "cl"},
    };
}

int ClientContextCount() { return Contexts().Count(); }
int ClientContextActive() { return Contexts().Active(); }

int ClientContextAdd() {
    int k = Contexts().Add();
    LOGI("client context: player %d added (%zu bytes of client state each)", k + 1, Contexts().Bytes());
    return k;
}

bool ClientContextSwitch(int k) { return Contexts().Switch(k); }

}  // namespace csx

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
