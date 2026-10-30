// Smooth scrolling of the menus' lists (N5A, PC Options > Game > Smooth Menu Scrolling, pc_settings.ini
// "menu_smooth_scroll", On by default). The title's table draw (0xCD030) shows the rows from its first shown one
// (table +0x120) to the last that fit, each at (row - first) x the row step (the font height x the text scale + the
// row spacing) below the table's top (+0x30, a float, plus a header's height), drawn by 0xCCD20; a selection past the
// shown rows moves the first one by a whole row at once. Here, when a table's first shown row changes by one or two
// rows, its rows are drawn from where they were and ease to their place over about a tenth of a second: an offset
// (the change x the row step, shrinking each frame) added to the rows' top, one more row drawn on the side the
// movement uncovers, and the rows clipped to the table's area below its header with the D3D scissor (the title's
// D3DDevice_SetScissors, 0x3067B0) while they are drawn. Larger jumps (a list wrapping from its last row to its first)
// are not eased. The rows' positions for the mouse (pc_mouse.cpp) are the table's own: the easing is only drawn.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;
using kernel::W32;

constexpr uint32_t kSetScissors = 0x3067B0;  // D3DDevice_SetScissors(count, exclusive, rects)
constexpr uint32_t kPresentWidth = 0x553620;  // the back buffer's width in the title's pixels (wider than 640: widescreen)

struct Ease {
    int first = -1;      // the first shown row last drawn
    float offset = 0;    // pixels still to go (positive: the rows are drawn lower than their place)
    std::chrono::steady_clock::time_point at{};
};
std::map<uint32_t, Ease> g_tables;  // by table address (the menus' tables live as long as their screen)
bool g_clipped = false;             // the scissor is set for the rows being drawn

bool Enabled() {
    static unsigned seen = ~0u;
    static bool on = true;
    if (pc::Version() != seen) {
        seen = pc::Version();
        on = pc::Get("menu_smooth_scroll", "1") != "0";
    }
    return on;
}

float AsFloat(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
}  // namespace

// [[hook]] at 0xCD15E in the table draw (0xCD030), before its rows' loop: esi the table, edi the first row to draw,
// ebx the row step (pixels), [esp+0x10] the header's height (pixels, the rows' offset), [esp+0x28] the last row to draw
void PcScrollRows(X86Context& ctx, uint8_t*) {
    g_clipped = false;
    if (!Enabled()) return;
    const uint32_t table = ctx.esi;
    const int first = int(R32(table + 0x120));
    const int step = int(ctx.ebx);
    const int rows = R32(table + 0x60) ? int((R32(table + 0x64) - R32(table + 0x60)) >> 5) : 0;
    Ease& e = g_tables[table];
    auto now = std::chrono::steady_clock::now();
    if (e.first >= 0 && first != e.first && std::abs(first - e.first) <= 2 && step > 0) e.offset += float((first - e.first) * step);
    else if (first != e.first) e.offset = 0;
    if (e.offset != 0) {
        double dt = std::chrono::duration<double>(now - e.at).count();
        e.offset *= float(std::exp(-std::min(dt, 0.1) / 0.035));  // about 0.1 s to settle
        if (std::fabs(e.offset) < 0.5f) e.offset = 0;
    }
    e.first = first;
    e.at = now;
    if (e.offset == 0 || step <= 0) return;
    const int off = int(std::lround(e.offset));
    if (off == 0) return;
    const uint32_t header = R32(ctx.esp + 0x10);
    W32(ctx.esp + 0x10, uint32_t(int(header) + off));
    if (off > 0 && first > 0) ctx.edi = uint32_t(first - 1);  // the row above comes into view
    int last = int(R32(ctx.esp + 0x28));
    if (off < 0 && last + 1 < rows) W32(ctx.esp + 0x28, uint32_t(last + 1));  // the row below
    // the rows clipped to the table below its header: the title's pixels (as the menus are drawn), the whole width
    float top = AsFloat(R32(table + 0x30)) + float(int(header)), bottom = AsFloat(R32(table + 0x38));
    uint32_t width = std::max<uint32_t>(640, R32(kPresentWidth));
    uint32_t rect = ctx.esp - 0x400;  // a D3DRECT below the stack in use (SetScissors reads it at once)
    W32(rect, 0);
    W32(rect + 4, uint32_t(std::max(0.0f, top)));
    W32(rect + 8, width);
    W32(rect + 12, uint32_t(std::max(top, bottom)));
    kernel::CallGuest(ctx, kSetScissors, {1, 0, rect});
    g_clipped = true;
}

// [[hook]] at 0xCD1E2 in the table draw, after its rows' loop: the scissor taken off again
void PcScrollRowsDone(X86Context& ctx, uint8_t*) {
    if (!g_clipped) return;
    g_clipped = false;
    kernel::CallGuest(ctx, kSetScissors, {0, 0, 0});
}
