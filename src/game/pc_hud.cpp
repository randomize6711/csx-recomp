// net_graph (N5E, Options > Interface > Net Graph: Off / Always On / With Scoreboard, pc_settings.ini
// net_graph_mode; Net Graph Detail: net_graph's own level 1..3): the engine's network graph (SCR_DrawNetGraph,
// 0x6E4B0: the frame rate, the bytes in and out, the server's rate, loss and choke, the latency's columns; it draws
// in a match, checked in vk_viewmodel_netgraph) shown always, only while the scoreboard is up, or not (the title's
// own: net_graph 0). The setting sets the cvar when what it wants changes, so net_graph typed in the console still
// works until then; cl_showfps is not touched.
// Its place: the title puts it by net_graphpos (0x6E2D0: 1 the view's bottom right, 2 the middle, else the left)
// over the HUD's bottom row; here (a hook after that, 0x6E613) the whole of it, its backdrop, columns and text as
// measured while it draws, goes in the screen's bottom right corner (over the HUD's corner), a margin inside (the
// view is the player's own: a split-screen player's quarter or half; on a wider frame the screen's right edge is the
// frame's, past the 640 columns the 2D layout keeps in the middle).
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
    if (mode == "off" && g_set < 0) return;  // never set here: the console's own
    if (want > 0 && size != g_size) {  // before the graph is shown: its columns' width and height
        g_size = size;
        char cmd[96];
        std::snprintf(cmd, sizeof cmd, "net_graphwidth %d; net_scale %g\n", 300 * size / 100, 5.0 * 100.0 / size);
        csx::QueueEngineCommand(cmd);
        LOGI("pc hud: net_graph size %d%% (%s)", size, cmd);
    }
    if (want == g_set) return;
    g_set = want;
    csx::QueueEngineCommand("net_graph " + std::to_string(want) + "\n");
    LOGI("pc hud: net_graph %d (%s)", want, mode.c_str());
}

namespace {
// the net graph's drawn box, measured while SCR_DrawNetGraph draws (from its place, 0x6E613, to its epilogue,
// 0x6EB50): its backdrop and columns (the fill 0x6D770), its text (DrawConsoleString 0x47060's answer), relative
// to the origin it was given. The largest box seen since the graph's look last changed places the next frames, so
// numbers changing width never push it past the edge (nor make it jitter)
bool g_measuring = false;
int32_t g_origin_x = 0, g_origin_y = 0;
pcnetgraph::Box g_frame, g_extent;
int g_extent_for = -1;  // the look the extent was measured with (net_graph level and graph width)
}  // namespace

// [[hook]] at 0x6E613 (config/game.toml) in SCR_DrawNetGraph, its place just computed (0x6E2D0: the graph's x at
// [esp+0x5C], its y at [esp+0x40]; esi the view's rectangle x, y, width, height; ebp the graph's width). All of it
// (backdrop, columns, text: one box, measured as it draws) goes kMargin inside the screen's bottom right corner
// (src/game/pc_netgraph_layout.h). The screen in the 2D layout: on a wider frame the 640 columns sit in the middle
// with a side column either side (pc_wide.cpp), so its right edge is 640 + the side, not the view's own
void PcNetGraphPlace(X86Context& ctx, uint8_t*) {
    int32_t x = int32_t(R32(ctx.esi)), y = int32_t(R32(ctx.esi + 4)), w = int32_t(R32(ctx.esi + 8)),
            h = int32_t(R32(ctx.esi + 0xC)), graph = int32_t(ctx.ebp);
    if (w <= 0 || h <= 0 || graph <= 0) return;
    const float level = kernel::G<float>(kNetGraphValue), height = kernel::G<float>(kGraphHeight);
    const int look = int(level) * 10000 + graph;
    if (look != g_extent_for) {  // another detail or size: measured again
        g_extent_for = look;
        g_extent = pcnetgraph::Box();
    }
    const pcnetgraph::Box rel = g_extent.Valid() ? g_extent : pcnetgraph::Estimate(graph, height, level >= 2.0f ? 46 : 24);
    const pcnetgraph::Screen screen = pcnetgraph::ScreenOf(PcWideSide(), float(x), float(y), float(w), float(h));
    const pcnetgraph::Origin o = pcnetgraph::Place(rel, screen);
    kernel::W32(ctx.esp + 0x5C, uint32_t(o.x));
    kernel::W32(ctx.esp + 0x40, uint32_t(o.y));
    g_origin_x = o.x, g_origin_y = o.y;
    g_frame = pcnetgraph::Box();
    g_measuring = true;
}

// [[hook]] at 0x6D770, the net graph's fill (eax the rectangle: x, y, width, height): its backdrop and columns
void PcNetGraphRect(X86Context& ctx, uint8_t*) {
    if (!g_measuring || !ctx.eax) return;
    float rx = float(int32_t(R32(ctx.eax))), ry = float(int32_t(R32(ctx.eax + 4)));
    float rw = float(int32_t(R32(ctx.eax + 8))), rh = float(int32_t(R32(ctx.eax + 0xC)));
    g_frame.Add(rx - float(g_origin_x), ry - float(g_origin_y), rx + rw - float(g_origin_x), ry + rh - float(g_origin_y));
}

// [[hook]] at 0x4714E, DrawConsoleString's return (x at [esp+0x10], y at [esp+0x14]; eax its answer): the net graph's
// text, a line of the console font (about 12 units high; its lines are 15 apart)
void PcNetGraphText(X86Context& ctx, uint8_t*) {
    if (!g_measuring) return;
    float tx = float(int32_t(R32(ctx.esp + 0x10))), ty = float(int32_t(R32(ctx.esp + 0x14)));
    float end = pcnetgraph::TextEnd(tx, float(int32_t(ctx.eax)));
    LOG_FIRST(6, "pc hud: net_graph text at %.0f %.0f, the renderer's answer %d: its end %.0f", tx, ty, int32_t(ctx.eax), end);
    g_frame.Add(tx - float(g_origin_x), ty - float(g_origin_y), end - float(g_origin_x), ty + 12.0f - float(g_origin_y));
}

// [[hook]] at 0x6EB50, SCR_DrawNetGraph's epilogue (every way out of it): the frame's box joins the extent
void PcNetGraphDone(X86Context&, uint8_t*) {
    if (!g_measuring) return;
    g_measuring = false;
    if (!g_frame.Valid()) return;
    const bool grew = !g_extent.Valid() || g_frame.x1 > g_extent.x1 || g_frame.y1 > g_extent.y1 ||
                      g_frame.x0 < g_extent.x0 || g_frame.y0 < g_extent.y0;
    g_extent.Add(g_frame);
    if (grew)
        LOG_FIRST(16, "pc hud: net_graph drawn %.0f..%.0f x %.0f..%.0f from its origin %d %d", g_extent.x0, g_extent.x1,
                  g_extent.y0, g_extent.y1, g_origin_x, g_origin_y);
}
