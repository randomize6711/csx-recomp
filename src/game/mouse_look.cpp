// Native mouse look: rotations from the PC's mouse added to the title's own view angles, where its stick look adds
// the stick's (FINDINGS.md "Mouse look").
//
// The look routine 0x19E160 integrates the right stick into yaw (float at 0x552C10, degrees, positive to the left as
// in GoldSrc) at 0x19E4C3 and pitch (0x552C14, positive looking down) at 0x19E4DD, through its response curve, turn
// acceleration and aim assist, then clamps the pitch from 0x19E4EB on. The hook at 0x19E4EB adds the mouse's degrees
// after the stick's and before that clamp, so the title's limits hold and the controller path is unchanged. The
// routine returns early where the title takes no look input (menus), and so does the mouse then. A mouse moves by
// distance, not speed: the degrees are applied once, whatever the frame time.
#include <atomic>
#include <cstring>

#include "common/csx.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace csx {

namespace {
constexpr uint32_t kYaw = 0x552C10, kPitch = 0x552C14;
std::atomic<float> g_right{0}, g_up{0};  // degrees not yet applied

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
}

}  // namespace csx

// [[hook]] at 0x19E4EB (config/game.toml): the game thread, once per pass through the look routine
void MouseLookApply(X86Context&, uint8_t*) {
    float right = csx::g_right.exchange(0), up = csx::g_up.exchange(0);
    if (right != 0) csx::AddTo(csx::kYaw, -right);
    if (up != 0) csx::AddTo(csx::kPitch, -up);
}
