// Instant Action (2026-10-31, docs/menus.md "Main menu"): the main menu's way into a match against bots, a hub of the
// title's front end style (pcmenu::HubScreen) over the title's own match setup screens, which keep every setting:
//   Start Match        the title's SinglePlayerStartSession (0xE2270: the match's line from the screens below, the map
//                      the map list's selection), with Single Player's choice on Custom so its Match Options count
//   Select Map         the title's map list (with its search, pc_mapsearch.cpp); choosing a map returns here
//   Game Mode          the title's Match Options, on its Game Mode row (pc_menu.cpp's ExtendGameMode)
//   Bot Settings       the same screen on Number of Bots (Bot Skill Level, Bot Team below; Y the bots' weapons)
//   Advanced Settings  the title's Custom Options (time and round limits, money, friendly fire, collision); its
//                      Accept sets Match Options' rules to Custom, so they count
// Match Options' and Custom Options' Accept return here (they went on to the map list before). The hub shows the
// match as set (map, mode, bots, rules) in its preview panel, from those screens' own tables. A match's end brings
// the title back to the screen it started from, Single Player's (its difficulties): that screen is the hub's now.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "common/csx.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/pc_options_text.h"
#include "kernel/kernel.h"

void PcSetScreenLine(X86Context& ctx, const char* screen, int index, const std::string& text);  // pc_browser.cpp
void PcSetSelectedRow(const std::string& screen, int row);                                       // pc_menu.cpp
uint32_t PcListCell(const std::string& screen, size_t row);                                     // pc_menu.cpp
int PcBotCountAt(int index);                                                                     // pc_menu.cpp
std::string PcMapListSelected();                                                                 // pc_mapsearch.cpp
void PcMapSearchChooseOnly();                                                                    // pc_mapsearch.cpp

namespace {
using kernel::R32;

constexpr char kMatch[] = "SuperLauncher_SinglePlayerMatch";               // Beginner .. Expert, Custom (row 4)
constexpr char kOptions[] = "SuperLauncher_SinglePlayerMatchOptions";      // its rows below
constexpr char kCustom[] = "SuperLauncher_SinglePlayerMatchCustomOptions";
constexpr char kMaps[] = "SuperLauncher_SinglePlayerMatchMapList";
constexpr int kMatchCustom = 4;
// Match Options' rows: the title's four, then ExtendGameMode's
constexpr int kRowBots = 0, kRowSkill = 1, kRowTeam = 2, kRowRules = 3, kRowMode = 4;
constexpr int kRulesCustom = 4;

int Choice(int row) {
    uint32_t cell = PcListCell(kOptions, size_t(row));
    return cell ? int(R32(cell)) : -1;
}

std::string Name(int index, std::initializer_list<const char*> names) {
    int i = 0;
    for (const char* n : names)
        if (i++ == index) return n;
    return "?";
}

// the preview panel's lines (the hub's BackgroundText 1..4)
void Show(X86Context& ctx) {
    const std::string map = PcMapListSelected();
    const int bots = PcBotCountAt(Choice(kRowBots));
    PcSetScreenLine(ctx, pcmenu::kInstantScreen, 1, "Map: " + (map.empty() ? std::string("?") : map));
    PcSetScreenLine(ctx, pcmenu::kInstantScreen, 2, "Mode: " + Name(Choice(kRowMode), {"Classic", "Deathmatch", "Gun Game"}));
    PcSetScreenLine(ctx, pcmenu::kInstantScreen, 3,
                    "Bots: " + (bots < 0 ? std::string("?") : std::to_string(bots)) + ", " +
                        Name(Choice(kRowSkill), {"Beginner", "Standard", "Professional", "Expert"}));
    PcSetScreenLine(ctx, pcmenu::kInstantScreen, 4,
                    "Rules: " + Name(Choice(kRowRules), {"Beginner", "Standard", "Professional", "Expert", "Custom"}));
}

// a screen's A command replaced (inside that screen's block); false when it is not as expected
bool ReplaceAccept(std::string& text, const char* screen, const std::string& from, const std::string& to) {
    size_t at = text.find(std::string("\n") + screen + "\r\n");
    size_t end = at == std::string::npos ? at : text.find("\n}", at + 1);
    size_t a = at == std::string::npos ? at : text.find("OnButtonDownA \"" + from + "\"", at);
    if (a == std::string::npos || a > end) return false;
    text.replace(a, std::strlen("OnButtonDownA \"") + from.size() + 1, "OnButtonDownA \"" + to + "\"");
    return true;
}
}  // namespace

// the hub's screen (pc_menu.cpp's ExtendMainMenu adds it with the main menu)
std::string PcInstantScreens() {
    using pcmenu::HubRow;
    const std::string rows =
        HubRow("Start Match", "Start a match with the map and settings shown.", "", "!pc_instant start") +
        HubRow("Select Map", "Choose the map: type to search the list.", "", "!pc_instant map") +
        HubRow("Game Mode", "Classic, Deathmatch or Gun Game.", "", "!pc_instant mode") +
        HubRow("Bot Settings", "How many bots, their skill and their team.", "Y there sets the bots' weapons.",
               "!pc_instant bots") +
        HubRow("Advanced Settings", "Time and round limits, money, friendly fire", "and the other match rules.",
               "!pc_instant rules");
    // the preview: the lobby's translucent panel on the right, the match as set
    const std::string preview =
        "\tBackgroundImage \"whitebox 255 255 255 46 340 112 250 118\"\r\n"
        "\tBackgroundText \"54 83 107 255 352 122 236 24 0.8\"\t\" \"\r\n"
        "\tBackgroundText \"54 83 107 255 352 148 236 24 0.8\"\t\" \"\r\n"
        "\tBackgroundText \"54 83 107 255 352 174 236 24 0.8\"\t\" \"\r\n"
        "\tBackgroundText \"54 83 107 255 352 200 236 24 0.8\"\t\" \"\r\n";
    return pcmenu::HubScreen(pcmenu::kInstantScreen, "Instant Action", rows, preview, "!pc_instant show");
}

// the title's match screens' Accept back to the hub: Match Options went on (to Custom Options or the map list),
// Custom Options to the map list
std::string PcInstantRewire(const std::string& text) {
    std::string out = text;
    if (!ReplaceAccept(out, kOptions, "!SinglePlayerMatchOptionsAccept", "!pc_instant back"))
        LOGW("pc instant: %s not as expected: its Accept left as it was", kOptions);
    if (!ReplaceAccept(out, kCustom, "open SuperLauncher_SinglePlayerMatchMapList", "!pc_instant custom"))
        LOGW("pc instant: %s not as expected: its Accept left as it was", kCustom);
    return out;
}

std::string PcCurrentScreen();  // pc_browser.cpp

// on the game thread every frame (pc_menu.cpp's PcMenuFrame): the title's Single Player screen (reached only by the
// title itself now: back from a match, the screen the match was started from) gives way to the hub, Back from which
// is the main menu as before
void PcInstantFrame(X86Context&) {
    static std::string last;
    const std::string screen = PcCurrentScreen();
    if (screen == last) return;
    last = screen;
    if (screen != kMatch) return;
    LOGI("pc instant: back from a match: the Instant Action hub instead of Single Player's screen");
    csx::QueueEngineCommand(std::string("xbui back\nxbui open ") + pcmenu::kInstantScreen + "\n");
}

// "pc_instant show|start|map|mode|bots|rules|back|custom" (the hub's rows and its screens' Accept)
void PcInstantCommand(X86Context& ctx, const std::string& what) {
    if (what == "show") return Show(ctx);
    if (what == "start") {
        PcSetSelectedRow(kMatch, kMatchCustom);  // Match Options (bots, mode, rules) count, not a difficulty preset
        LOGI("pc instant: start: %s, mode %d, bots %d, rules %d", PcMapListSelected().c_str(), Choice(kRowMode),
             PcBotCountAt(Choice(kRowBots)), Choice(kRowRules));
        return csx::QueueEngineCommand("SinglePlayerStartSession\n");
    }
    if (what == "map") {
        PcMapSearchChooseOnly();
        LOGI("pc instant: Select Map (choosing returns here)");
        return csx::QueueEngineCommand(std::string("xbui open ") + kMaps + "\n");
    }
    if (what == "mode" || what == "bots") {
        PcSetSelectedRow(kOptions, what == "mode" ? kRowMode : kRowBots);
        return csx::QueueEngineCommand(std::string("xbui open ") + kOptions + "\n");
    }
    if (what == "rules") return csx::QueueEngineCommand(std::string("xbui open ") + kCustom + "\n");
    if (what == "custom") {  // Custom Options accepted: Match Options' rules are those
        if (uint32_t cell = PcListCell(kOptions, kRowRules)) kernel::W32(cell, uint32_t(kRulesCustom));
        LOGI("pc instant: the match's rules are Custom Options'");
        return csx::QueueEngineCommand("xbui back\n");
    }
    if (what == "back") {
        LOGI("pc instant: Match Options accepted: back to the hub");
        return csx::QueueEngineCommand("xbui back\n");
    }
}
