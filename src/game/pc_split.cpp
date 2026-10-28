// Split-screen in the menus (docs/splitscreen.md): a "Split Screen" row on the main menu (its last) opening the
// Split Screen screen (pc_menu.cpp's PcScreen "Split": Local Players 1-4, the two-player layout, whether player 1 also
// takes the first controller; PC settings split_*), its status lines showing each player's device as the compositor
// sees them (its State messages), "Press START on a controller to join" for a free slot. Accept saves the settings and
// applies them: under the compositor (this process is split-screen player 1) they go to the compositor, which starts
// or ends players at once; alone in its window, the game restarts as the compositor (back on the main menu).
// N4.8: the screen is Split Screen Options, opened from Multiplayer's party screen (pc_party.cpp), whose profiles
// replace N4.6's Player Names; a name is still 1-20 characters of letters, digits, spaces and - _ . ! ? ( ) [ ] * + =
// : ' @ # & (it goes into a console command and a server's userinfo).
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
std::string PcPartyProfileName();  // pc_party.cpp

namespace {

constexpr char kScreen[] = "SuperLauncher_PCSplit";
constexpr int kFirstStatus = 0;  // its BackgroundText lines: the four status lines, the hint, then the title
constexpr size_t kMaxName = 20;
std::string g_shown;

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
    // player 1: its profile's name (N4.8), else split_name_1
    std::string profile = n == 1 ? PcPartyProfileName() : "";
    std::string v = n == 1 ? (!profile.empty() ? profile : pc::Get("split_name_1", "")) : env ? env : "";
    if (PcSplitNameValid(v)) return v;
    if (!v.empty()) LOGW("split: the name set for player %d is not used (not a valid name)", n);
    return n == 1 ? "" : "Player " + std::to_string(n);
}

namespace {

// what the status lines say: the compositor's state ("slot N TEXT" lines), else how it will start
std::vector<std::string> StatusText() {
    std::vector<std::string> l(5);
    l[4] = "";
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
    for (int i = 0; i < 5; i++) PcSetScreenLine(ctx, kScreen, kFirstStatus + i, lines[size_t(i)]);
}


}  // namespace

// N4.8: Multiplayer's party screen (pc_party.cpp) replaces the main menu's Split Screen row (its options are opened
// from the party screen, BACK) and the profiles replace Player Names: the front end's text is left as it is
std::string ExtendSplitMenu(const std::string& text) { return text; }

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
