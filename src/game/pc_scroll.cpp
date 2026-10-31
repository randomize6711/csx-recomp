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
//
// The scrollbar (2026-10-31): a PC settings list (pc_menu.cpp's screens, the Controller rows in the title's Control
// Options too) with more rows than it shows gets a bar at its right edge, drawn after its rows (the engine's FillRGBA,
// 0x476C0: a track and a thumb in the menu's colour, the front end's blue, the pause menu's gold). The thumb's length
// is the shown rows' share of all rows, its place the first shown row's. pc_mouse.cpp clicks and drags it and turns
// the wheel over such a list into it (PcScrollbarFirst, PcScrollView): they move only the rows in view (the list's
// first shown row, +0x120, held at the draw until the selection moves), never the selection; the keys and the pad
// move the selection, which the list keeps in view as before.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

bool PcSettingsScreen(const std::string& name);  // pc_menu.cpp

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
std::map<uint32_t, int> g_shown;    // by table address: how many rows its last draw showed (pc_mapsearch.cpp)

// the scrollbar: drawn for the list being drawn when it is a PC settings list with rows out of view
struct Bar {
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // the track, the menu's 640x480 coordinates
    int rows = 0, shown = 0, first = 0;
};
std::map<uint32_t, Bar> g_bars;  // by table address, from its last draw (none: no bar)
struct View {
    int first = 0;      // the first row shown, as the scrollbar or the wheel put it
    uint32_t sel = 0;   // the selection then: a move of it (the keys, the pad, a click on a row) hands the view back
};
std::map<uint32_t, View> g_views;  // by table address
uint32_t g_drawing = 0;          // the table whose rows are being drawn
int g_step = 0;                  // its row step (pixels)
constexpr uint32_t kCurrentScreen = 0x5523D4, kFillRGBA = 0x476C0, kGlEnable = 0x6A89E8, kGlBlend = 0x0BE2;
constexpr float kBarGap = 4, kBarWidth = 5;

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
    g_shown[ctx.esi] = int(R32(ctx.esp + 0x28)) - int(ctx.edi) + 1;
    g_drawing = ctx.esi;
    g_step = int(ctx.ebx);
    // the rows in view the scrollbar or the wheel chose, while the selection stays where it was
    if (auto v = g_views.find(ctx.esi); v != g_views.end()) {
        if (R32(ctx.esi + 0x9C) != v->second.sel) {
            g_views.erase(v);
        } else {
            const int shown = int(R32(ctx.esp + 0x28)) - int(ctx.edi);
            W32(ctx.esi + 0x120, uint32_t(v->second.first));
            ctx.edi = uint32_t(v->second.first);
            W32(ctx.esp + 0x28, uint32_t(v->second.first + shown));
        }
    }
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

namespace {
std::string ScreenName(uint32_t screen) {
    std::string out;
    for (uint32_t p = screen ? R32(screen) : 0, i = 0; p && i < 64; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(p))[i];
        if (!c) break;
        out += c;
    }
    return out;
}

void Fill(X86Context& ctx, float x, float y, float w, float h, const uint8_t c[3], uint32_t alpha) {
    X86Context saved = ctx;
    ctx.esp -= 0x200;
    kernel::CallGuest(ctx, kFillRGBA, {uint32_t(int32_t(std::lround(x))), uint32_t(int32_t(std::lround(y))),
                                       uint32_t(std::max(1L, std::lround(w))), uint32_t(std::max(1L, std::lround(h))),
                                       c[0], c[1], c[2], alpha});
    ctx = saved;
}

// the bar of the list just drawn, when it is the current screen's PC settings list and has rows out of view
void DrawBar(X86Context& ctx) {
    const uint32_t table = g_drawing, screen = R32(kCurrentScreen);
    g_drawing = 0;
    if (!table || !screen || R32(screen + 0x380) != table) return;
    std::string name = ScreenName(screen);
    if (!PcSettingsScreen(name)) return;
    Bar b;
    b.rows = R32(table + 0x60) ? int((R32(table + 0x64) - R32(table + 0x60)) >> 5) : 0;
    b.shown = g_shown[table];
    b.first = int(R32(table + 0x120));
    if (b.rows <= b.shown || b.shown <= 0 || g_step <= 0) {
        g_bars.erase(table);
        return;
    }
    b.x0 = AsFloat(R32(table + 0x34)) + kBarGap;
    b.x1 = b.x0 + kBarWidth;
    b.y0 = AsFloat(R32(table + 0x30));
    b.y1 = b.y0 + float(b.shown * g_step);
    g_bars[table] = b;
    const bool in_game = name.rfind("InGame_", 0) == 0;
    static const uint8_t blue[3] = {54, 83, 107}, gold[3] = {177, 169, 54};
    const uint8_t* c = in_game ? gold : blue;
    const float h = b.y1 - b.y0, thumb = std::max(8.0f, h * float(b.shown) / float(b.rows));
    const float top = b.y0 + (h - thumb) * float(b.first) / float(std::max(1, b.rows - b.shown));
    Fill(ctx, b.x0, b.y0, kBarWidth, h, c, 70);       // the track
    Fill(ctx, b.x0, top, kBarWidth, thumb, c, 230);  // the thumb
    X86Context saved = ctx;  // FillRGBA leaves blending off: the menu draws on with it on
    ctx.esp -= 0x100;
    kernel::CallGuest(ctx, R32(kGlEnable), {kGlBlend});
    ctx = saved;
}
}  // namespace

// [[hook]] at 0xCD1E2 in the table draw, after its rows' loop: the scissor taken off again, the scrollbar drawn
void PcScrollRowsDone(X86Context& ctx, uint8_t*) {
    if (g_clipped) {
        g_clipped = false;
        kernel::CallGuest(ctx, kSetScissors, {0, 0, 0});
    }
    DrawBar(ctx);
}

// a click or a drag on a list's scrollbar (pc_mouse.cpp): the first row to show for the pointer's height (the thumb's
// middle there), -1 off the bar (dragging: the height alone counts, wherever the pointer went)
int PcScrollbarFirst(uint32_t table, float x, float y, bool dragging) {
    auto it = g_bars.find(table);
    if (it == g_bars.end()) return -1;
    const Bar& b = it->second;
    if (!dragging && !(x >= b.x0 - 3 && x < b.x1 + 4 && y >= b.y0 && y < b.y1)) return -1;
    float f = std::clamp((y - b.y0) / std::max(1.0f, b.y1 - b.y0), 0.0f, 1.0f);
    return std::clamp(int(std::lround(f * float(b.rows) - float(b.shown) / 2.0f)), 0, b.rows - b.shown);
}

// the rows in view set (the scrollbar, the wheel over a list that has one), the selection unchanged; by wheel notches
// from the rows in view when relative; false for a list with no scrollbar
bool PcScrollView(uint32_t table, int first, bool relative) {
    auto it = g_bars.find(table);
    if (it == g_bars.end()) return false;
    const Bar& b = it->second;
    int from = int(R32(table + 0x120));
    int to = std::clamp(relative ? from + first : first, 0, std::max(0, b.rows - b.shown));
    g_views[table] = {to, R32(table + 0x9C)};
    W32(table + 0x120, uint32_t(to));
    return true;
}

// how many rows a table's last draw showed (0 before its first), for moving its highlight into view
int PcTableShownRows(uint32_t table) {
    auto it = g_shown.find(table);
    return it == g_shown.end() ? 0 : it->second;
}
