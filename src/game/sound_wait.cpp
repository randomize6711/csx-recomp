// The sound system's 450 ms wait (FINDINGS.md "High population performance"): the game's sound object (0x553ED8) runs
// XACT and DirectSound's work on its own thread (0x103E60: take the sound lock [obj+0xA8], DoWork, release, sleep to a
// 30 ms period). Its stop-everything function (0x106380: round restart 0x146302, map loads and eight other callers)
// first busy-waits a fixed 0.45 s (QueryPerformanceCounter against 0.45 at 0x3668D0) whenever that thread exists,
// checking nothing else, then takes the lock and stops each sound. On the Xbox's one processor spinning was how the
// sound thread got its turns; here it runs on its own core, and the game thread spent the 0.45 s spinning: a 450 ms
// frame at every round restart (the worst frame of every benchmark, 16 bots or 64).
// The wait now ends once the sound thread has started two passes after it began (one whole pass of its work in
// between: what the spin gave it, without waiting out the rest), 30 to 60 ms. The 0.45 s limit stays. With no sound
// thread the original loop runs as it was (it does the work itself). CSX_SOUND_WAIT=original keeps the full wait.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
std::atomic<uint32_t> g_passes{0};
thread_local uint32_t t_start_pass = 0;
thread_local std::chrono::steady_clock::time_point t_start;

bool Original() {
    static const bool on = std::getenv("CSX_SOUND_WAIT") && !std::strcmp(std::getenv("CSX_SOUND_WAIT"), "original");
    return on;
}
}  // namespace

// [[hook]] at 0x103EB9 (config/game.toml): the sound thread's loop, at the start of each pass
void SoundThreadPass(X86Context&, uint8_t*) { g_passes++; }

// [[hook]] at 0x1063C8: the stop-everything function about to take its wait's start time
void SoundWaitBegin(X86Context&, uint8_t*) {
    t_start_pass = g_passes.load();
    t_start = std::chrono::steady_clock::now();
}

// [[hook]] at 0x106408 (the wait loop, not timed out, about to test for the sound thread, ebx = the sound object):
// true ends the wait (0x106444, where the timeout goes) once the thread has started two passes since it began
bool SoundWaitDone(X86Context& ctx, uint8_t*) {
    if (Original() || !kernel::R32(ctx.ebx + 0xA4) || g_passes.load() - t_start_pass < 2) return false;
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
    LOG_FIRST(16, "sound: the stop-everything wait ended after %.1f ms (the sound thread's two passes; was 450 ms)", ms);
    return true;
}
