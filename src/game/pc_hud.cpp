// net_graph (N5E, PC Options > HUD and Menus > Net Graph: Off / Always On / With Scoreboard, pc_settings.ini
// net_graph_mode; Net Graph Detail: net_graph's own level 1..3): the engine's network graph (SCR_DrawNetGraph,
// 0x6E4B0: the frame rate, the bytes in and out, the server's rate, loss and choke, the latency's columns; it draws
// in a match, checked in vk_viewmodel_netgraph) shown always, only while the scoreboard is up, or not (the title's
// own: net_graph 0). The setting sets the cvar when what it wants changes, so net_graph typed in the console still
// works until then; cl_showfps is not touched.
// Its place: the title puts it by net_graphpos (0x6E2D0: 1 the view's bottom right, 2 the middle, else the left)
// over the HUD's bottom row; here (a hook after that, 0x6E613) it goes in the view's bottom right corner above the
// HUD's money, health and ammunition.
#include <algorithm>
#include <cstdlib>
#include <string>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

std::string PcCurrentScreen();  // pc_browser.cpp

namespace {
using kernel::R32;
int g_set = -1;  // the net_graph level last set here
}  // namespace

// on the game thread every frame (pc_menu.cpp's PcMenuFrame)
void PcHudFrame() {
    static unsigned seen = ~0u;
    static std::string mode = "off";
    static int level = 1;
    if (pc::Version() != seen) {
        seen = pc::Version();
        mode = pc::Get("net_graph_mode", "off");
        level = std::max(1, std::min(3, std::atoi(pc::Get("net_graph_level", "1").c_str())));
    }
    int want = mode == "always" ? level : mode == "scoreboard" && PcCurrentScreen() == "Scoreboard" ? level : 0;
    if (mode == "off" && g_set < 0) return;  // never set here: the console's own
    if (want == g_set) return;
    g_set = want;
    csx::QueueEngineCommand("net_graph " + std::to_string(want) + "\n");
    LOGI("pc hud: net_graph %d (%s)", want, mode.c_str());
}

// [[hook]] at 0x6E613 (config/game.toml) in SCR_DrawNetGraph, its place just computed (0x6E2D0: the graph's x at
// [esp+0x5C], its y at [esp+0x40]; esi the view's rectangle x, y, width, height; ebp the graph's width): the view's
// bottom right corner, above the HUD's bottom row
void PcNetGraphPlace(X86Context& ctx, uint8_t*) {
    int32_t x = int32_t(R32(ctx.esi)), y = int32_t(R32(ctx.esi + 4)), w = int32_t(R32(ctx.esi + 8)),
            h = int32_t(R32(ctx.esi + 0xC)), graph = int32_t(ctx.ebp);
    if (w <= 0 || h <= 0 || graph <= 0) return;
    // its text runs about 60 units past the graph's width; the HUD's bottom row is the last 40 or so
    kernel::W32(ctx.esp + 0x5C, uint32_t(x + w - graph - 70));
    kernel::W32(ctx.esp + 0x40, uint32_t(y + h - 150));
}
