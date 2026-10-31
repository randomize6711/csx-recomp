// net_graph (N5E, Options > Interface > Net Graph: Off / Always On / With Scoreboard, pc_settings.ini
// net_graph_mode; Net Graph Detail: net_graph's own level 1..3): the engine's network graph (SCR_DrawNetGraph,
// 0x6E4B0: the frame rate, the bytes in and out, the server's rate, loss and choke, the latency's columns; it draws
// in a match, checked in vk_viewmodel_netgraph) shown always, only while the scoreboard is up, or not (the title's
// own: net_graph 0). The setting sets the cvar when what it wants changes, so net_graph typed in the console still
// works until then; cl_showfps is not touched.
// Its place: the title puts it by net_graphpos (0x6E2D0: 1 the view's bottom right, 2 the middle, else the left)
// over the HUD's bottom row; here (a hook after that, 0x6E613) the whole of it, its backdrop, columns and text as
// measured while it draws, goes in the screen's bottom right corner (over the HUD's corner), a margin inside (the
// view is the player's own: a split-screen player's quarter or half; its coordinates are the frame's units, which its
// fills land in, and its text is given them less the text hook's side column).
// Its size (Net Graph Size, net_graph_size 50 / 75 / 100, 50 by default since 2026-10-31): the engine's columns, one
// unit wide each (net_graphwidth, 300 by default) and their height (value / net_scale, net_scale 5 by default), set
// with net_graph while this setting shows it. The text keeps the console font (8 units a character).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/pc_netgraph_layout.h"
#include "kernel/kernel.h"

std::string PcCurrentScreen();  // pc_browser.cpp
float PcWideSide();             // pc_wide.cpp: the side column on a wider frame (0 at 4:3)

namespace {
using kernel::R32;
int g_set = -1;  // the net_graph level last set here
int g_size = -1;  // the net_graph_size last set
float g_scale = 1;  // the parent transform's scale (Net Graph Size while this setting shows the graph; 1 otherwise)
constexpr uint32_t kNetGraphValue = 0x3787D8 + 0xC;  // the net_graph cvar's value (float)
constexpr uint32_t kGraphHeight = 0x3776BC + 0xC;   // graphheight's (64)
}  // namespace

// on the game thread every frame (pc_menu.cpp's PcMenuFrame)
void PcHudFrame() {
    static unsigned seen = ~0u;
    static std::string mode = "off";
    static int level = 1, size = 50;
    if (pc::Version() != seen) {
        seen = pc::Version();
        mode = pc::Get("net_graph_mode", "off");
        level = std::max(1, std::min(3, std::atoi(pc::Get("net_graph_level", "1").c_str())));
        size = std::max(25, std::min(100, std::atoi(pc::Get("net_graph_size", "50").c_str())));
    }
    int want = mode == "always" ? level : mode == "scoreboard" && PcCurrentScreen() == "Scoreboard" ? level : 0;
    g_scale = mode == "off" ? 1.0f : float(size) / 100.0f;
    if (mode == "off" && g_set < 0) return;  // never set here: the console's own
    if (want > 0 && size != g_size) {  // the engine's own layout (its 300 columns, net_scale 5), scaled as a whole
        g_size = size;
        csx::QueueEngineCommand("net_graphwidth 300; net_scale 5\n");
        LOGI("pc hud: net_graph size %d%%: the engine's layout scaled as one", size);
    }
    if (want == g_set) return;
    g_set = want;
    csx::QueueEngineCommand("net_graph " + std::to_string(want) + "\n");
    LOGI("pc hud: net_graph %d (%s)", want, mode.c_str());
}

namespace {
// the net graph as one parent (src/game/pc_netgraph_layout.h): from its place (0x6E613) to its epilogue (0x6EB50)
// every fill (0x6D7D1, the float rectangle the fill draws) and every string (0x470D0, the text renderer's setup: its
// float position) goes through one transform about the origin, scaled by Net Graph Size, and the glyphs with it
// (the font's scale, +0x10 across and +0x14 down, which the renderer multiplies every glyph and every measured width
// by, scaled while the graph draws and put back after). The text gets a width that cuts nothing (the netgraph's own
// call gave 640 - x, so past x 640 its strings ended in "..."). The box of all of it, after the transform and
// relative to the origin, places the next frames; the largest since the graph's look changed, so numbers changing
// width never push it past the edge (nor make it jitter)
bool g_measuring = false;
pcnetgraph::Transform g_tf;
float g_text_x = 0, g_text_y = 0;  // the string being drawn, transformed
pcnetgraph::Box g_frame, g_extent;
int g_extent_for = -1;  // the look the extent was measured with (net_graph level, graph width, scale)
uint32_t g_font = 0;    // the font the net graph's text is drawn with (seen at the renderer, pc_wide.cpp's hook)
float g_font_scale[2] = {1, 1};
bool g_font_scaled = false;

void ScaleFont(bool on) {
    if (!g_font || on == g_font_scaled) return;
    float* f = static_cast<float*>(kernel::HostPtr(g_font + 0x10));
    if (on) {
        g_font_scale[0] = f[0], g_font_scale[1] = f[1];
        f[0] *= g_tf.scale, f[1] *= g_tf.scale;
    } else {
        f[0] = g_font_scale[0], f[1] = g_font_scale[1];
    }
    g_font_scaled = on;
}
}  // namespace

// the text renderer's font while the net graph draws (pc_wide.cpp PcWideText, the renderer's entry: ecx the font)
void PcNetGraphFont(uint32_t font) {
    if (!g_measuring || !font || font == g_font) return;
    ScaleFont(false);
    g_font = font;
    if (g_tf.scale != 1) ScaleFont(true);  // from its next string (and every frame after)
}

// [[hook]] at 0x6E613 (config/game.toml) in SCR_DrawNetGraph, its place just computed (0x6E2D0: the graph's x at
// [esp+0x5C], its y at [esp+0x40]; esi the view's rectangle x, y, width, height; ebp the graph's width). All of it
// (backdrop, columns, text: one box, measured as it draws) goes kMargin inside the screen's bottom right corner
// (src/game/pc_netgraph_layout.h). The screen is the view's rectangle, in the frame's units (0 0 854 480 at 16:9),
// the space the fills land in; the text renderer's strings are given their x less the side column its hook adds
void PcNetGraphPlace(X86Context& ctx, uint8_t*) {
    int32_t x = int32_t(R32(ctx.esi)), y = int32_t(R32(ctx.esi + 4)), w = int32_t(R32(ctx.esi + 8)),
            h = int32_t(R32(ctx.esi + 0xC)), graph = int32_t(ctx.ebp);
    if (w <= 0 || h <= 0 || graph <= 0) return;
    const float level = kernel::G<float>(kNetGraphValue), height = kernel::G<float>(kGraphHeight);
    const int look = int(level) * 100000 + graph * 100 + int(g_scale * 100);
    if (look != g_extent_for) {  // another detail or size: measured again
        g_extent_for = look;
        g_extent = pcnetgraph::Box();
    }
    const pcnetgraph::Box rel =
        g_extent.Valid() ? g_extent : pcnetgraph::Estimate(graph, height, level >= 2.0f ? 30 : 24, g_scale);
    const pcnetgraph::Screen screen = pcnetgraph::ScreenOf(float(x), float(y), float(w), float(h));
    LOG_FIRST(2, "pc hud: net_graph in the view %d %d %d %d (the frame's units)", x, y, w, h);
    const pcnetgraph::Origin o = pcnetgraph::Place(rel, screen);
    kernel::W32(ctx.esp + 0x5C, uint32_t(o.x));
    kernel::W32(ctx.esp + 0x40, uint32_t(o.y));
    g_tf = {float(o.x), float(o.y), g_scale};
    g_frame = pcnetgraph::Box();
    g_measuring = true;
    if (g_scale != 1) ScaleFont(true);
}

// [[hook]] at 0x6D7D1 in the net graph's fill (0x6D770: its backdrop, columns and lines), the fill's float
// rectangle pushed for 0x6F550: x0, y0, x1, y1 at [esp]..[esp+0xC]. Transformed and measured
void PcNetGraphRect(X86Context& ctx, uint8_t*) {
    if (!g_measuring) return;
    float* r = static_cast<float*>(kernel::HostPtr(ctx.esp));
    r[0] = g_tf.X(r[0]), r[1] = g_tf.Y(r[1]), r[2] = g_tf.X(r[2]), r[3] = g_tf.Y(r[3]);
    g_frame.Add(std::min(r[0], r[2]) - g_tf.ox, std::min(r[1], r[3]) - g_tf.oy, std::max(r[0], r[2]) - g_tf.ox,
                std::max(r[1], r[3]) - g_tf.oy);
}

// [[hook]] at 0x470D0 in DrawConsoleString (0x47060), the text renderer's setup about to be called (0xBCBC0: x, y
// floats at [esp], [esp+4], ..., the width it may fill at [esp+0x18], 640): the net graph's string transformed, and
// given a width that cuts nothing
void PcNetGraphTextAt(X86Context& ctx, uint8_t*) {
    if (!g_measuring) return;
    float* p = static_cast<float*>(kernel::HostPtr(ctx.esp));
    g_text_x = g_tf.X(p[0]), g_text_y = g_tf.Y(p[1]);  // the frame's units, as the fills
    p[0] = pcnetgraph::TextDrawX(g_text_x, PcWideSide()), p[1] = g_text_y;  // the text hook adds the side back
    kernel::W32(ctx.esp + 0x18, uint32_t(pcnetgraph::kTextWidth));
}

// [[hook]] at 0x4714E, DrawConsoleString's return (eax its answer: the string's width, measured at the font's scale,
// so the transform's): the string's box, a line of the console font (its 0.75 glyphs about 12 units high)
void PcNetGraphText(X86Context& ctx, uint8_t*) {
    if (!g_measuring) return;
    const float end = pcnetgraph::TextEnd(g_text_x, float(int32_t(ctx.eax)));
    LOG_FIRST(6, "pc hud: net_graph text at %.0f %.0f, the renderer's answer %d: its end %.0f", g_text_x, g_text_y,
              int32_t(ctx.eax), end);
    g_frame.Add(g_text_x - g_tf.ox, g_text_y - g_tf.oy, end - g_tf.ox, g_text_y + 12.0f * g_tf.scale - g_tf.oy);
}

// [[hook]] at 0x6EB50, SCR_DrawNetGraph's epilogue (every way out of it): the frame's box joins the extent
void PcNetGraphDone(X86Context&, uint8_t*) {
    if (!g_measuring) return;
    g_measuring = false;
    ScaleFont(false);
    if (!g_frame.Valid()) return;
    const bool grew = !g_extent.Valid() || g_frame.x1 > g_extent.x1 || g_frame.y1 > g_extent.y1 ||
                      g_frame.x0 < g_extent.x0 || g_frame.y0 < g_extent.y0;
    g_extent.Add(g_frame);
    if (grew)
        LOG_FIRST(16, "pc hud: net_graph drawn %.0f..%.0f x %.0f..%.0f from its origin %.0f %.0f at scale %.2f",
                  g_extent.x0, g_extent.x1, g_extent.y0, g_extent.y1, g_tf.ox, g_tf.oy, g_tf.scale);
}
