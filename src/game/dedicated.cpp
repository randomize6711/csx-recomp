// The dedicated server mode (docs/pc_multiplayer.md "N2"; csx-server, src/server): the engine loaded as GoldSrc's
// dedicated server, which this title never did. CEngine::Load's first argument is g_bIsDedicatedServer (0x5EF670,
// stored at 0xAD569); its one caller passed 0 (0xADA64). With 1, SV_SetMaxclients leaves cls.state 0 (ca_dedicated,
// 0xA19DA) and Host_Init skips the palette, video, draw, renderer and client initialisation (0x5DBEC -> 0x5DD18):
// no local client, so no player slot is the host's; the game DLL's IsDedicatedServer is true.
// The host frame then runs at a fixed tick (kTickRate a second), sleeping between frames instead of spinning, not
// tied to rendering (there is none) or the CPU's speed.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"

namespace {
constexpr double kTickRate = 30;  // the console's and N1's server rate: gameplay timing unchanged (FINDINGS "N2 timing")
}

namespace csx {
namespace {
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_finished{false};
}
bool Dedicated() {
    static const bool on = std::getenv("CSX_DEDICATED") && *std::getenv("CSX_DEDICATED") == '1';
    return on;
}
uint64_t ServerFrames() { return g_frames.load(std::memory_order_relaxed); }
void SetServerFinished() { g_finished = true; }
bool ServerFinished() { return g_finished; }
}  // namespace csx

// [[hook]] at 0xADA66 in the title's boot, CEngine::Load's call with its three arguments pushed ([esp] the dedicated
// flag): 1 for the dedicated server
void DedicatedLoad(X86Context& ctx, uint8_t*) {
    if (!csx::Dedicated()) return;
    LOGI("dedicated: the engine loads as a dedicated server (no client)");
    kernel::W32(ctx.esp, 1);
}

// [[hook]] at 0x5DD18 in Host_Init, where its jump past the client's initialisation lands (only a dedicated server
// gets here: the client's path jumps on at 0x5DD16). The title's game code is one image, client and server parts
// linked together, and code both use (the movement and weapon code: 0x15007E) calls the engine through the client
// part's copy of the engine functions (0x5430E8), which ClientDLL_Init (0x1C5D0, "cl_dlls\client.dll", at 0x5DC70 in
// the skipped part) fills: without it the first such call traps. It runs here as the client's path runs it, after
// PM_Init (0x74830) of the movement struct it hands the client part; then the client part is marked not loaded
// (0x55184C, which 0x1C6F0 set): the engine never calls its HUD, frame or input functions, which need a client. The
// palette, video, drawing, sound, voice and the client itself stay uninitialised
void DedicatedInit(X86Context& ctx, uint8_t*) {
    if (!csx::Dedicated()) return;
    constexpr uint32_t kPmInit = 0x74830, kClientMove = 0x78A6E0, kClientDllInit = 0x1C5D0, kClientDllLoaded = 0x55184C;
    kernel::CallGuest(ctx, kPmInit, {kClientMove});
    kernel::CallGuest(ctx, kClientDllInit, {});
    kernel::W32(kClientDllLoaded, 0);
    LOGI("dedicated: the game code's engine functions set, its client part not run");
}

// [[hook]] at 0xADCE5 in the title's start of a match, its Cvar_DirectSet (0x41F90) of "name" (0x371B90) to the
// profile's name: the local player's name, a cvar CL_Init registers. A dedicated server has no local player and that
// cvar no zone string (setting it frees one: a crash in the allocator, 0xC8690): true skips the call (0xADCEA pops
// its arguments)
bool DedicatedNoPlayerName(X86Context&, uint8_t*) {
    return csx::Dedicated();
}

// [[hook]] at 0x5E01F in Host_Shutdown: with cls.state ca_dedicated it calls a function of GoldSrc's dedicated
// launcher (0x5EF12C), which this title never had and nothing sets: true skips the call while it is not set
bool DedicatedShutdownExport(X86Context&, uint8_t*) {
    return kernel::R32(0x5EF12C) == 0;
}

// [[hook]] at 0x202C32 in the image's startup, the title's main (0xBF470) returned: its quit is over (Host_Shutdown
// told the players) and XAPI would go back to the dashboard (0x20364D, a launch that is refused: not a recompiled
// image). csx-server exits instead of leaving the process parked (a map change relaunches inside main, 0xBF266)
void DedicatedQuit(X86Context&, uint8_t*) {
    if (csx::Dedicated()) csx::SetServerFinished();
}

// [[hook]] at 0x5D5D0, Host_Frame's entry (the title's main loop calls it through CEngine::Frame): the admin actions
// due (server_admin.cpp), then the dedicated server's tick. Each frame starts at its deadline, one tick after the
// previous one's; a frame that ran long moves the schedule on instead of running the missed ticks back to back
void DedicatedTick(X86Context& ctx, uint8_t*) {
    csx::RunAdminActions(ctx);  // a listen server's too
    if (!csx::Dedicated()) return;
    csx::g_frames.fetch_add(1, std::memory_order_relaxed);
    using clock = std::chrono::steady_clock;
    static const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / kTickRate));
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (next < now - period) next = now;
    // the wait answers server queries as they arrive (server_query.cpp), so a browser's ping is not the tick's
    if (next > now) csx::ServerQueryWait(std::chrono::duration<double>(next - now).count());
    next += period;
}
