// CSX_LOG_SERVERTIME=1 (FINDINGS.md "Bot count", for benchmarks): the server's share of each frame, logged once a
// second. The host frame times its parts for host_speeds with Sys_FloatTime (0xAC240, the stamps at 0x43E0D0..F8,
// printed by 0x5CFF0): the server frame is the call at 0x5D3CD (SV_Frame 0xA1D20: the clients' commands and so every
// player's and bot's move, the game DLL's frame with the bots' thinking and navigation, entity physics, the messages
// to the clients on the loopback). Within it PM_Move (0x1A5B20, its one ret at 0x1A5B7B) is the movement of each
// player and bot (the server's moves; the client's prediction runs outside the server frame). The rest of the server
// frame is the bots' AI and navigation, the game rules and the network messages, which share functions and are not
// told apart here.
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "core/log.h"
#include "cpu/x86_context.h"

namespace {
using Clock = std::chrono::steady_clock;

bool Enabled() {
    static const bool on = std::getenv("CSX_LOG_SERVERTIME") && *std::getenv("CSX_LOG_SERVERTIME") == '1';
    return on;
}

Clock::time_point g_frame_start, g_move_start, g_second = Clock::now();
int g_depth = 0;  // PM_Move entered and not left (it does not recurse; a guard all the same)
double g_frame_ms = 0, g_frame_max = 0, g_move_ms = 0;
unsigned g_frames = 0, g_moves = 0;
bool g_in_frame = false;

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
}  // namespace

// [[hook]] at 0x5D3CD (config/game.toml): the host frame about to run the server frame
void ServerFrameBegin(X86Context&, uint8_t*) {
    if (!Enabled()) return;
    g_in_frame = true;
    g_frame_start = Clock::now();
}

// [[hook]] at 0x5D3D2: the server frame returned; once a second the second's frames and moves are logged
void ServerFrameEnd(X86Context&, uint8_t*) {
    if (!Enabled() || !g_in_frame) return;
    g_in_frame = false;
    auto now = Clock::now();
    double ms = Ms(now - g_frame_start);
    g_frame_ms += ms;
    g_frame_max = ms > g_frame_max ? ms : g_frame_max;
    g_frames++;
    if (now - g_second < std::chrono::seconds(1)) return;
    g_second = now;
    if (g_frames)
        LOGI("server timing: %u frames, server frame %.3f ms mean, %.3f max; player moves %.3f ms a frame (%u moves); "
             "the rest (bots' AI and navigation, game rules, network) %.3f ms a frame",
             g_frames, g_frame_ms / g_frames, g_frame_max, g_move_ms / g_frames, g_moves,
             (g_frame_ms - g_move_ms) / g_frames);
    g_frame_ms = g_frame_max = g_move_ms = 0;
    g_frames = g_moves = 0;
}

// [[hook]] at 0x1A5B24 (PM_Move's second instruction: its entry has the player collision hook)
void ServerMoveBegin(X86Context&, uint8_t*) {
    if (!Enabled() || !g_in_frame || g_depth++) return;
    g_move_start = Clock::now();
}

// [[hook]] at 0x1A5B7B: PM_Move's ret
void ServerMoveEnd(X86Context&, uint8_t*) {
    if (!Enabled() || !g_in_frame || !g_depth || --g_depth) return;
    g_move_ms += Ms(Clock::now() - g_move_start);
    g_moves++;
}
