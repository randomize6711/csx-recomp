// The loading screen's progress bar (the bug-fix pass's issue 10). The title draws it (0xC2BC0: a bar from x 74 to the
// end x it is given, 640x480; the end kept at 0x5520AC for its redraws) with the end at fraction x K1 + K2 (0xC3269: K1 at 0x366BBC, K2 at 0x366BB8), the
// fraction being what has loaded over what the title expects to load (0x37A5B8 / 0x526AE0). A load past that count (a
// match through the local server, imported maps, mods) took the fraction past 1 and the bar past its frame: the end
// is held at the full bar's (fraction 1). The bar still moves as before up to there.
// On a wide frame its frame (a screen rectangle, pc_wide.cpp's PcWideRect2D) moves to the centred 640 columns but the
// bar's own vertices (x 74 at 0xC2F13, the end x through 0xC2F82) are in a 640 wide space stretched over the whole
// frame: the bar was drawn left of its frame. Both ends go where their frame is: (x + side) x 640 / the frame's width
// (PcWideSide: the side column).
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
constexpr uint32_t kScale = 0x366BBC, kStart = 0x366BB8;

float GuestFloat(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}
}  // namespace

float PcWideSide();  // pc_wide.cpp

namespace {
// a 640x480 x where its frame is on a frame side wider at each side (the bar's space stretched over the whole frame)
float Placed(float x, float side) { return side > 0 ? (x + side) * 640.0f / (640.0f + 2.0f * side) : x; }
}  // namespace

// [[hook]] at 0xC2F69 (config/game.toml) in the bar's drawing: its end x in eax (the value it was given; the one kept
// for the redraws is left as it is)
void PcLoadingBar(X86Context& ctx, uint8_t*) {
    int32_t full = int32_t(GuestFloat(kScale) + GuestFloat(kStart));
    int32_t end = int32_t(ctx.eax);
    if (full > 74 && full <= 640 && end > full) {
        LOG_FIRST(4, "pc loading: the bar's end %d held at the full bar's %d (loaded past the expected count)", end, full);
        end = full;
    }
    float side = PcWideSide();
    if (side > 0) LOG_FIRST(1, "pc loading: the bar moved %.0f to its frame in the centred 640 columns", side);
    ctx.eax = uint32_t(int32_t(Placed(float(end), side) + 0.5f));
}

// [[hook]] at 0xC2F18: the bar's start x (74.0 in edi) for both its start vertices, moved as its end
void PcLoadingBarStart(X86Context& ctx, uint8_t*) {
    float x = Placed(74.0f, PcWideSide());
    std::memcpy(&ctx.edi, &x, 4);
}
