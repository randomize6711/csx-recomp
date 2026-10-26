// Split-screen in the menus (docs/splitscreen.md): a "Split Screen" row on the main menu (its last) opening the
// Split Screen screen (pc_menu.cpp's PcScreen "Split": Local Players 1-4, the two-player layout, whether player 1 also
// takes the first controller; PC settings split_*), its status lines showing each player's device as the compositor
// sees them (its State messages), "Press START on a controller to join" for a free slot. Accept saves the settings and
// applies them: under the compositor (this process is split-screen player 1) they go to the compositor, which starts
// or ends players at once; alone in its window, the game restarts as the compositor (back on the main menu).
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"

void PcSetScreenLine(X86Context& ctx, const char* screen, int index, const std::string& text);  // pc_browser.cpp
std::string PcCurrentScreen();

namespace {

constexpr char kScreen[] = "SuperLauncher_PCSplit";
constexpr int kFirstStatus = 0;  // its BackgroundText lines: the four status lines, then the title
std::string g_shown;

// what the status lines say: the compositor's state ("slot N TEXT" lines), else how it will start
std::vector<std::string> StatusText() {
    std::vector<std::string> l(4);
    std::string state = csx::SplitState();
    if (csx::SplitPlayer() == 0) {
        l[0] = "Accept starts split screen: player 1 on the keyboard and";
        l[1] = "mouse, players 2 to 4 on controllers in the order they";
        l[2] = "connect. A controller's START also joins a player.";
        return l;
    }
    size_t at = 0;
    while (at < state.size()) {
        size_t end = state.find('\n', at);
        std::string line = state.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.rfind("slot ", 0) == 0 && line.size() > 7) {
            int n = line[5] - '1';
            std::string what = line.substr(7);
            if (n >= 0 && n < 4)
                l[size_t(n)] = "Player " + std::to_string(n + 1) + ": " +
                               (what == "join" ? std::string("press START on a controller to join") : what == "-" ? std::string("off") : what);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return l;
}

void Show(X86Context& ctx) {
    auto lines = StatusText();
    std::string all;
    for (auto& l : lines) all += l + "\n";
    if (all == g_shown) return;
    g_shown = all;
    for (int i = 0; i < 4; i++) PcSetScreenLine(ctx, kScreen, kFirstStatus + i, lines[size_t(i)]);
}

}  // namespace

// the main menu's Split Screen row, its last (after Change Profile: every original row keeps its place), written as
// its rows are (with help); the text unchanged when the main menu is not laid out as expected
std::string ExtendSplitMenu(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_MainScreen\r\n");
    size_t last = screen == std::string::npos ? screen : text.find("CellFixed \"Change Profile\"", screen);
    size_t row = last == std::string::npos ? last : text.find("\n\t\t}\r\n", last);
    if (row == std::string::npos) {
        LOGW("split: the main menu not laid out as expected: no Split Screen row");
        return text;
    }
    row += std::strlen("\n\t\t}\r\n");  // after the Change Profile row's closing brace
    std::string add = "\t\tRow\r\n\t\t{\r\n\t\t\tTable\r\n\t\t\t{\r\n\t\t\t\tLayout\r\n\t\t\t\t{\r\n"
                      "\t\t\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\t\t\tTableLocation \"75 361\"\r\n"
                      "\t\t\t\t\tTableSize \"476 40\"\r\n\t\t\t\t\tTableRowSpacing \"1\"\r\n\t\t\t\t\tTableTextScale \"0.75\"\r\n"
                      "\t\t\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n\t\t\t\t}\r\n\t\t\t\tRow\r\n\t\t\t\t{\r\n"
                      "\t\t\t\t\tCellFixed \"Play with up to four players on this PC.\"\r\n\t\t\t\t}\r\n\t\t\t}\r\n"
                      "\t\t\tCommandSelect \"open SuperLauncher_PCSplit\"\r\n\t\t\tCellFixed \"Split Screen\"\r\n\t\t}\r\n";
    return text.substr(0, row) + add + text.substr(row);
}

void PcSplitCommand(X86Context& ctx, const std::vector<std::string>& a) {
    std::string cmd = a.size() > 1 ? a[1] : "";
    if (cmd == "show") {
        g_shown.clear();
        Show(ctx);
        return;
    }
    if (cmd != "apply") return;
    int players = std::atoi(pc::Get("split_players", "1").c_str());
    std::string layout = pc::Get("split_layout", "horizontal"), pad = pc::Get("split_p1_pad", "0");
    LOGI("split: %d local players, layout %s, player 1 %s", players, layout.c_str(),
         pad == "1" ? "with a controller too" : "on the keyboard and mouse");
    if (csx::SplitPlayer() == 1) {
        setenv("CSX_SPLIT_PLAYERS", std::to_string(players).c_str(), 1);
        csx::SplitSend(csx::kSplitSetup, "players " + std::to_string(players) + " layout " + layout + " p1pad " + pad);
        return;
    }
    if (players > 1 && !csx::SplitStart(players)) LOGW("split: split screen needs the game's window (not started)");
}

// on the menu frame (pc_menu.cpp's PcMenuFrame): the status lines follow the compositor's state while shown
void PcSplitMenuFrame(X86Context& ctx) {
    if (PcCurrentScreen() == kScreen) Show(ctx);
}
