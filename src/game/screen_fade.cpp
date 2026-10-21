// The title's screen fade (the ScreenFade message: the flashbang's white-out, FINDINGS.md "Flashbang"), logged.
//
// The server's RadiusFlash (0x155170) sends each player in sight of the grenade a white fade whose hold and fade-out
// times follow the distance and whether the player faced it; the client's handler 0xB6500 keeps it at 0x74A244..
// 0x74A258 against the client time (0x74A290, a double): with FFADE_IN the alpha holds until the hold ends, then falls
// linearly to 0 when the fade ends (0xB6630). Every frame 0x4D860 draws it as a full-screen quad through the GL layer.
// Its timing is the client clock's, so it does not depend on the frame rate; this log shows it.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}
double D(uint32_t va) {
    double d;
    std::memcpy(&d, kernel::HostPtr(va), 8);
    return d;
}
}  // namespace

// [[hook]] at 0x4D86D (config/game.toml), in 0x4D860 after 0xB6630 returned this frame's alpha (eax, 0 when no fade):
// CSX_LOG_FADE=1 logs a fade's start (its colour, alpha and flags, hold end and fade end in client time), its alpha
// ten times a second and its end, with how long it lasted in client and in real time
void ScreenFadeLog(X86Context& ctx, uint8_t*) {
    static const bool log = std::getenv("CSX_LOG_FADE") && *std::getenv("CSX_LOG_FADE") == '1';
    if (!log) return;
    using clock = std::chrono::steady_clock;
    static bool on = false;
    static double start = 0;
    static clock::time_point start_real, last;
    uint32_t alpha = ctx.eax;
    double now = D(0x74A290);
    const uint8_t* c = static_cast<const uint8_t*>(kernel::HostPtr(0x74A254));
    if (alpha && !on) {
        on = true;
        start = now;
        start_real = last = clock::now();
        LOGI("screen fade: started at %.3f s, colour %02X%02X%02X alpha %u, flags %X, held until %.3f s, ends at %.3f s",
             now, c[0], c[1], c[2], c[3], kernel::R32(0x74A258), F(0x74A250), F(0x74A248));
    } else if (alpha && clock::now() - last >= std::chrono::milliseconds(100)) {
        last = clock::now();
        LOGI("screen fade: alpha %u at %.3f s", alpha, now - start);
    } else if (!alpha && on) {
        on = false;
        LOGI("screen fade: ended, lasted %.3f s (%.3f s real)", now - start,
             std::chrono::duration<double>(clock::now() - start_real).count());
    }
}
