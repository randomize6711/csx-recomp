// Split-screen in the menus (docs/splitscreen.md): a "Split Screen" row on the main menu (its last) opening the
// Split Screen screen (pc_menu.cpp's PcScreen "Split": Local Players 1-4, the two-player layout, whether player 1 also
// takes the first controller; PC settings split_*), its status lines showing each player's device as the compositor
// sees them (its State messages), "Press START on a controller to join" for a free slot. Accept saves the settings and
// applies them: under the compositor (this process is split-screen player 1) they go to the compositor, which starts
// or ends players at once; alone in its window, the game restarts as the compositor (back on the main menu).
// Y opens Player Names (SuperLauncher_PCSplitNames): a row a player, A on one opens the profile keyboard (the same
// screen as Join by Address: pad, mouse, or typed on the keyboard) for that player's name, saved as split_name_N in
// pc_settings.ini. A name is 1-20 characters of letters, digits, spaces and - _ . ! ? ( ) [ ] * + = : ' @ # & (no
// quotes, ';', '\\', '%', '$' or control characters: it goes into a console command and a server's userinfo); an
// empty name gives the default back (player 1: its profile's name, players 2-4: "Player N").
#include <cctype>
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
std::string PcKeyboardScreen(const char* name, const char* title, const char* command, const char* hint);
std::string PcEditText(const char* screen);
void PcClearEdit(X86Context& ctx, const char* screen, const std::string& text);
void PcTypeAgain();  // pc_mouse.cpp

namespace {

constexpr char kScreen[] = "SuperLauncher_PCSplit";
constexpr char kNames[] = "SuperLauncher_PCSplitNames";
constexpr char kName[] = "SuperLauncher_PCSplitName";
constexpr int kFirstStatus = 0;  // its BackgroundText lines: the four status lines, the hint, then the title
constexpr size_t kMaxName = 20;
std::string g_shown;
int g_naming = 0;  // the player whose name the keyboard screen edits

// a player's name as set (empty: its default)
std::string SetName(int n) { return pc::Get("split_name_" + std::to_string(n), ""); }
std::string ShownName(int n) {
    std::string v = SetName(n);
    return !v.empty() ? v : n == 1 ? std::string("(profile name)") : "Player " + std::to_string(n);
}

}  // namespace

// a name a player may have: 1-20 characters from the list above, not only spaces (the same check the compositor and
// the network join apply to what they read back)
bool PcSplitNameValid(const std::string& name) {
    if (name.empty() || name.size() > kMaxName || name.find_first_not_of(' ') == std::string::npos) return false;
    for (unsigned char c : name)
        if (!std::isalnum(c) && !std::strchr(" -_.!?()[]*+=:'@#&", c)) return false;
    return true;
}

// this process's name as a split-screen player (player 1: split_name_1 when set; players 2..4: the compositor's
// CSX_PLAYER_NAME, "Player N" when that is not a valid name); "" without split-screen or a name of its own
std::string PcSplitPlayerName() {
    int n = csx::SplitPlayer();
    if (n < 1) return "";
    const char* env = std::getenv("CSX_PLAYER_NAME");
    std::string v = n == 1 ? pc::Get("split_name_1", "") : env ? env : "";
    if (PcSplitNameValid(v)) return v;
    if (!v.empty()) LOGW("split: the name set for player %d is not used (not a valid name)", n);
    return n == 1 ? "" : "Player " + std::to_string(n);
}

namespace {

// what the status lines say: the compositor's state ("slot N TEXT" lines), else how it will start
std::vector<std::string> StatusText() {
    std::vector<std::string> l(5);
    l[4] = "Press Y for the players' names.";
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
                l[size_t(n)] = (SetName(n + 1).empty() ? "Player " + std::to_string(n + 1) : SetName(n + 1)) + ": " +
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
    for (int i = 0; i < 5; i++) PcSetScreenLine(ctx, kScreen, kFirstStatus + i, lines[size_t(i)]);
}

void ShowNames(X86Context& ctx) {
    for (int i = 0; i < 4; i++) PcSetScreenLine(ctx, kNames, i, ShownName(i + 1));
}

std::string Q(const std::string& s) { return "\"" + s + "\""; }

// Player Names: the PC screens' plain panel with System Link's table and highlight (Back / Select), a row a player
// with its name beside it
std::string NamesScreen() {
    std::string t = std::string("\r\n") + kNames + "\r\n{\r\n"
        "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
        "\tPieceOffsetDegrees \"90\"\r\n\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n\tCommandSelect \"RowSelect\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnEntry \"!pc_split names_show\"\r\n\tOnButtonDownB \"back\"\r\n";
    for (int i = 0; i < 4; i++)  // the names (BackgroundText 0..3), beside the rows
        t += "\tBackgroundText \"54 83 107 255 330 " + std::to_string(106 + 73 * i / 2) + " 300 30 0.875\"\t\" \"\r\n";
    t += "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"Player Names\"\r\n"
         "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n\t}\r\n"
         "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n\t}\r\n"
         "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\tTableVerticalWrap\r\n"
         "\t\t\tTableLocation \"80 98\"\r\n\t\t\tTableSize \"236 300\"\r\n\t\t\tTableRowSpacing \"12\"\r\n"
         "\t\t\tNonSelectBG \"0 0 0 0\"\r\n\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
         "\t\t\tHighlightImage \"MainMenu_Highlight 1.0 1.0 1.0 1.0 -40.0 -1.0 240.0 38.0\"\r\n\t\t}\r\n";
    for (int n = 1; n <= 4; n++) {
        std::string help = n == 1 ? "Player 1's name in split screen (empty: the profile's)." : "The name player " + std::to_string(n) + " plays under.";
        t += "\t\tRow\r\n\t\t{\r\n\t\t\tTable\r\n\t\t\t{\r\n\t\t\t\tLayout\r\n\t\t\t\t{\r\n"
             "\t\t\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\t\t\tTableLocation \"75 361\"\r\n\t\t\t\t\tTableSize \"476 40\"\r\n"
             "\t\t\t\t\tTableRowSpacing \"1\"\r\n\t\t\t\t\tTableTextScale \"0.75\"\r\n\t\t\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
             "\t\t\t\t}\r\n\t\t\t\tRow\r\n\t\t\t\t{\r\n\t\t\t\t\tCellFixed " + Q(help) + "\r\n\t\t\t\t}\r\n\t\t\t}\r\n"
             "\t\t\tCommandSelect \"!pc_split name_open " + std::to_string(n) + "; xbui open " + kName + "\"\r\n"
             "\t\t\tCellFixed \"Player " + std::to_string(n) + "\"\r\n\t\t}\r\n";
    }
    return t + "\t}\r\n}\r\n";
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
    return text.substr(0, row) + add + text.substr(row) + NamesScreen() +
           PcKeyboardScreen(kName, "Player Name", "pc_split name", "Up to 20 characters.");
}

void PcSplitCommand(X86Context& ctx, const std::vector<std::string>& a) {
    std::string cmd = a.size() > 1 ? a[1] : "";
    if (cmd == "show") {
        g_shown.clear();
        Show(ctx);
        return;
    }
    if (cmd == "names_show") return ShowNames(ctx);
    if (cmd == "name_open") {  // the keyboard screen opens with the player's name as set (the first key replaces it)
        g_naming = std::atoi(a.size() > 2 ? a[2].c_str() : "0");
        if (g_naming < 1 || g_naming > 4) g_naming = 0;
        if (g_naming) {
            PcClearEdit(ctx, kName, SetName(g_naming));
            PcSetScreenLine(ctx, kName, 1, "Player " + std::to_string(g_naming) + "'s name, up to 20 characters.");
        }
        return;
    }
    if (cmd == "name") {  // Done: checked, saved, back to Player Names
        if (!g_naming) return;
        std::string name = PcEditText(kName);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        name.erase(0, name.find_first_not_of(' ') == std::string::npos ? name.size() : name.find_first_not_of(' '));
        std::string key = "split_name_" + std::to_string(g_naming);
        if (name.empty()) {
            pc::Erase(key);
            LOGI("split: player %d's name back to its default", g_naming);
        } else if (PcSplitNameValid(name)) {
            pc::Set(key, name);
            LOGI("split: player %d's name set (%zu characters)", g_naming, name.size());
        } else {
            LOGW("split: player %d's name refused (1-20 letters, digits, spaces and - _ . ! ? ( ) [ ] * + = : ' @ # &)", g_naming);
            PcSetScreenLine(ctx, kName, 1, "A character in that name cannot be used.");
            PcTypeAgain();
            return;  // the keyboard stays: the name is changed or cleared there
        }
        pc::Save();
        g_naming = 0;
        csx::QueueEngineCommand("xbui back\n");
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
