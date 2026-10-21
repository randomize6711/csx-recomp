// Native mouse look: rotations from the PC's mouse added to the title's own view angles, where its stick look adds
// the stick's (FINDINGS.md "Mouse look").
//
// The look routine 0x19E160 integrates the right stick into yaw (float at 0x552C10, degrees, positive to the left as
// in GoldSrc) at 0x19E4C3 and pitch (0x552C14, positive looking down) at 0x19E4DD, through its response curve, turn
// acceleration and aim assist, then clamps the pitch from 0x19E4EB on. The hook at 0x19E4EB adds the mouse's degrees
// after the stick's and before that clamp, so the title's limits hold and the controller path is unchanged. The
// routine returns early where the title takes no look input (menus), and so does the mouse then. A mouse moves by
// distance, not speed: the degrees are applied once, whatever the frame time.
// Every second with mouse motion the log says how it arrived: "mouse look: E motions, D degrees (right, up), moved in
// M of P passes, at most X degrees in one" (passes without motion while it streams in are frames the window thread
// had not read the mouse yet: uneven turning at high frame rates).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/csx.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace csx {

namespace {
constexpr uint32_t kYaw = 0x552C10, kPitch = 0x552C14;
std::atomic<float> g_right{0}, g_up{0};  // degrees not yet applied
std::atomic<unsigned> g_motions{0};        // AddLook calls (motion events) since the last log

void AddTo(uint32_t va, float d) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    f += d;
    std::memcpy(kernel::HostPtr(va), &f, 4);
}
}  // namespace

void AddLook(float right_degrees, float up_degrees) {
    float r = g_right.load(), u = g_up.load();
    while (!g_right.compare_exchange_weak(r, r + right_degrees)) {}
    while (!g_up.compare_exchange_weak(u, u + up_degrees)) {}
    g_motions++;
}

}  // namespace csx

// [[hook]] at 0x19E4EB (config/game.toml): the game thread, once per pass through the look routine
void MouseLookApply(X86Context&, uint8_t*) {
    float right = csx::g_right.exchange(0), up = csx::g_up.exchange(0);
    if (right != 0) csx::AddTo(csx::kYaw, -right);
    if (up != 0) csx::AddTo(csx::kPitch, -up);
    static unsigned passes = 0, moved = 0;
    static double sum_right = 0, sum_up = 0, most = 0, sum_d = 0, sum_d2 = 0;
    static auto start = std::chrono::steady_clock::now();
    passes++;
    if (right != 0 || up != 0) {
        moved++;
        sum_right += right;
        sum_up += up;
        double d = std::hypot(double(right), double(up));
        most = std::max(most, d);
        sum_d += d;
        sum_d2 += d * d;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - start < std::chrono::seconds(1)) return;
    if (unsigned motions = csx::g_motions.exchange(0); motions || moved) {
        // the spread: the standard deviation of a moving pass's turn over its mean (0 for a steady stream at a steady
        // frame rate; batches of motion read late make it larger)
        double mean = moved ? sum_d / moved : 0, var = moved ? std::max(0.0, sum_d2 / moved - mean * mean) : 0;
        LOGI("mouse look: %u motions, %.3f %.3f degrees (right, up), moved in %u of %u passes, %.3f in one on average, "
             "at most %.3f, spread %.2f", motions, sum_right, sum_up, moved, passes, mean, most,
             mean > 0 ? std::sqrt(var) / mean : 0.0);
    }
    passes = moved = 0;
    sum_right = sum_up = most = sum_d = sum_d2 = 0;
    start = now;
}
