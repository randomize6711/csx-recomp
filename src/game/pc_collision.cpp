// Custom Game > Player Collision (FINDINGS.md "Player collision"). The player's body meets other players only in the
// movement code: PM_Move (0x1A5B20, pm_shared, the one copy the server's entity interface and the client's
// HUD_PlayerMove both reach) moves a player against pmove's physents (count at +0x240, the array at +0x244, 0xD4 bytes
// each: player at +0x20, info at +0x50), which the engine fills with the world, the solid entities near the move and
// the other players (player != 0; bots are players). Weapon traces, grenades and every other entity's movement go
// through the engine's SV_Move on the edicts' own solidity and hitboxes, not through physents.
//
// Off leaves the other players out of physents at PM_Move's entry, for the server's move of each player and bot and
// for the local client's prediction alike: bodies pass through each other, everything else is unchanged (the world,
// doors, platforms, hostages; shots, grenades, damage, hitboxes). The value comes with the match's command line
// ("+pc_player_collision 0|1", the Custom Options row's choice; src/game/pc_menu.cpp), before anyone spawns, and is
// fixed for the match: a change once a player has moved is refused (no live toggle).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
constexpr uint32_t kNumPhysent = 0x240, kPhysents = 0x244, kPhysentSize = 0xD4, kPhysentPlayer = 0x20;

bool g_collision = true;  // On: the title's own behaviour
bool g_given = false;     // the match's command line set it
bool g_moved = false;     // a player has moved: the match runs and the setting stays as it is
}  // namespace

// "pc_player_collision 0|1" (pc_menu.cpp's PcConsoleCommand): from the match's command line
void PcPlayerCollisionCommand(const std::string& arg) {
    if (arg != "0" && arg != "1") {
        LOGW("pc: pc_player_collision takes 0 or 1, not \"%s\"", arg.c_str());
        return;
    }
    if (g_moved) {
        LOGI("pc: player collision is set at the match's start (Custom Options): stays %s", g_collision ? "on" : "off");
        return;
    }
    g_collision = arg == "1";
    g_given = true;
    LOGI("pc: player collision %s (Custom Options)%s", g_collision ? "on" : "off",
         g_collision ? "" : ": players and bots pass through each other");
}

// [[hook]] at 0x1A5B20 (config/game.toml), PM_Move's entry (the playermove at [esp+4], server at [esp+8]): with
// collision Off the other players' physents are taken out (the world, entry 0, and the rest kept in their order)
void PcPlayerCollisionMove(X86Context& ctx, uint8_t*) {
    if (!g_moved && !g_given) LOGI("pc: player collision on (the title's own)");
    g_moved = true;
    if (g_collision) return;
    uint32_t pm = kernel::R32(ctx.esp + 4);
    uint32_t n = kernel::R32(pm + kNumPhysent), kept = n ? 1 : 0;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t pe = pm + kPhysents + i * kPhysentSize;
        if (kernel::R32(pe + kPhysentPlayer)) continue;
        if (kept != i)
            std::memmove(kernel::HostPtr(pm + kPhysents + kept * kPhysentSize), kernel::HostPtr(pe), kPhysentSize);
        kept++;
    }
    if (kept == n) return;
    kernel::W32(pm + kNumPhysent, kept);
    // logged once for each mover the server moves (pmove's player_index at +0: the player 0, the bots after it) and
    // once for the client's prediction
    static uint64_t logged = 0;
    bool server = kernel::R32(ctx.esp + 8) != 0;
    uint32_t mover = server ? std::min<uint32_t>(kernel::R32(pm), 62) : 63;
    if (!(logged >> mover & 1)) {
        logged |= uint64_t(1) << mover;
        if (server)
            LOGI("pc: player bodies: %u of %u physents (players) left out of client %u's move", n - kept, n,
                 kernel::R32(pm));
        else
            LOGI("pc: player bodies: %u of %u physents (players) left out of the client's prediction", n - kept, n);
    }
}

namespace csx {
// the match's setting, for a network client's signon (net_security.cpp)
bool PlayerCollisionOn() {
    return g_collision;
}
}  // namespace csx
