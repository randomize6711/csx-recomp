// Frame rate (FINDINGS.md "Frame rate"): Options > PC Options > Frame Rate Limit. The title presented every second
// vertical blank (30 frames per second) and the window paces to that by default; GoldSrc's own cap is fps_max (cvar
// 0x377D08, value 0x377D14, default 72; Host_FilterTime 0x5CD6B, which also holds loading at 30). A chosen limit
// sets both the window's pacing (app::SetFrameRate) and fps_max.
// The movement command's msec (CL_Move 0x2AE..: (int)(host_frametime x 1000), host_frametime the double at
// 0x6CE0E0) drops the fraction of a millisecond every frame, and the player's movement runs on those milliseconds:
// at 30 frames per second 33 of 33.3 ms (247.5 units/s for 250), at 72 13 of 13.9 ms (234), at 144 6 of 6.9 (216).
// Above the original 30 the fraction is carried to the next frame, so the milliseconds add up to the time that passed
// and the speed is the same at every rate; at 30 the title's own rounding is kept.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app/app.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kFpsMax = 0x377D14;     // fps_max's value (float)
constexpr uint32_t kFrameTime = 0x6CE0E0;  // host_frametime (double, seconds)

unsigned g_rate = 0;   // the chosen limit, 0 while none is set (FPS from the environment, fps_max the game's)
double g_carry = 0;    // the fraction of a millisecond the last movement command left out

float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}

}  // namespace

// on the game thread every frame (pc_keys.cpp's PcKeysFrame): the setting applied when it changes, fps_max kept at it
void PcFrameRateFrame(X86Context&) {
    static unsigned seen = ~0u;
    if (pc::Version() != seen) {
        seen = pc::Version();
        unsigned rate = unsigned(std::max(0, std::atoi(pc::Get("frame_rate", "").c_str())));
        if (rate != g_rate) {
            g_rate = rate;
            g_carry = 0;
            if (rate) app::SetFrameRate(rate);
            LOGI("pc frame: limit %u frames per second%s", rate, rate > 30 ? ", movement time carried" : "");
        }
    }
    if (g_rate && F(kFpsMax) != float(g_rate)) {
        float v = float(g_rate);
        std::memcpy(kernel::HostPtr(kFpsMax), &v, 4);
    }
}

// [[hook]] at 0x2AEDB (config/game.toml): CL_Move storing the movement command's msec (al, after the clamp to 255);
// above 30 frames per second the fraction of a millisecond is carried to the next command
void PcMoveMsec(X86Context& ctx, uint8_t*) {
    if (g_rate <= 30) return;
    double ms;
    std::memcpy(&ms, kernel::HostPtr(kFrameTime), 8);
    double exact = std::clamp(ms * 1000.0 + g_carry, 0.0, 255.0);
    uint32_t msec = uint32_t(exact);
    g_carry = exact - double(msec);
    ctx.eax = (ctx.eax & ~0xFFu) | msec;
}
