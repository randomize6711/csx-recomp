// net_graph (N5E, Options > Interface > Net Graph: Off / Always On / With Scoreboard, pc_settings.ini
// net_graph_mode; Net Graph Detail: net_graph's own level 1..3): the engine's network graph (SCR_DrawNetGraph,
// 0x6E4B0: the frame rate, the bytes in and out, the server's rate, loss and choke, the latency's columns; it draws
// in a match, checked in vk_viewmodel_netgraph) shown always, only while the scoreboard is up, or not (the title's
// own: net_graph 0). The setting sets the cvar when what it wants changes, so net_graph typed in the console still
// works until then; cl_showfps is not touched.
// Its place: the title puts it by net_graphpos (0x6E2D0: 1 the view's bottom right, 2 the middle, else the left)
// over the HUD's bottom row; here (a hook after that, 0x6E613) it goes in the view's bottom right corner, over the
// HUD's corner: the whole graph, its backdrop and its text kKeep inside the view's right and bottom edges (the view
// is the player's own: a split-screen player's quarter or half; widescreen's columns are the view's).
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
#include "kernel/kernel.h"

std::string PcCurrentScreen();  // pc_browser.cpp

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

// [[hook]] at 0x6E613 (config/game.toml) in SCR_DrawNetGraph, its place just computed (0x6E2D0: the graph's x at
// [esp+0x5C], its y at [esp+0x40]; esi the view's rectangle x, y, width, height; ebp the graph's width). The whole of
// it goes in the view's bottom right corner (the user's choice: over the HUD's corner, kKeep inside the edges). Drawn
// from that place (0x6E613 on): its backdrop from x - 5, y - graphheight - 5 (graphheight 64, cvar 0x3776BC), the graph's
// width + 10 wide and graphheight + 34 high, so its bottom is y + 29; the text from x, its longest line about 24
// characters at net_graph 1 (in, out, loss and choke), about 46 at 2 and 3 (the server's sv line)
void PcNetGraphPlace(X86Context& ctx, uint8_t*) {
    int32_t x = int32_t(R32(ctx.esi)), y = int32_t(R32(ctx.esi + 4)), w = int32_t(R32(ctx.esi + 8)),
            h = int32_t(R32(ctx.esi + 0xC)), graph = int32_t(ctx.ebp);
    if (w <= 0 || h <= 0 || graph <= 0) return;
    constexpr int32_t kKeep = 8, kChar = 8, kBelow = 29;  // the margin; a character; the backdrop's bottom below y
    float level = kernel::G<float>(kNetGraphValue);
    float height = kernel::G<float>(kGraphHeight);
    int32_t text = (level >= 2.0f ? 46 : 24) * kChar;
    int32_t right = std::max(graph + 5, text);  // the block's right edge past x
    int32_t top = int32_t(height) + 5;          // its top above y
    kernel::W32(ctx.esp + 0x5C, uint32_t(std::max(x + kKeep + 5, x + w - kKeep - right)));
    kernel::W32(ctx.esp + 0x40, uint32_t(std::max(y + kKeep + top, y + h - kKeep - kBelow)));
}
