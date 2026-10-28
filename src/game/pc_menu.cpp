// PC options inside the game's own menus (FINDINGS.md "Original menus and PC Options"). The game's screens are text:
// the engine loads FrontEndUI*.txt (front end) and InGameUI*.txt (pause menu) whole (0xCD9A0: size at 0xCDA2C, read,
// then 0xCDB1A) and parses screens of tables, rows and cells (CellFixed labels, CellList choices) drawn and navigated
// by the game's own UI code with its fonts, images and sounds. The PC screens are declared once below (Screens():
// their rows of settings); from that the hooks add, in memory, a row per screen to each menu's Options screen and the
// screens themselves written in that menu's own format, laid out as its Control Options screen. The screens' commands
// are console commands ("!name" in the text, as the game's own screens call theirs): pc_options_load <screen> on
// entry fills the lists from pc_settings.ini, A runs pc_options_save <screen>, X (where offered) pc_options_defaults
// <screen>, B leaves without saving. A hook in Cmd_ExecuteString serves them before the engine's own command lookup.
// PC Controls' rows also take a pressed key (press-a-key rebinding): Enter or a click on a row's key name makes the
// row wait for a key; its value cell shows "Press a key" in the game's own table (the shown choice's text swapped
// while it waits); the next key, mouse button or wheel notch the window sends becomes the row's choice in its list,
// Escape or the pad's B leave it as it was. The list, Accept, Cancel and Reset stay as they are: Accept saves.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/csx.h"
#include "common/pc_bindings.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/local_server.h"
#include "game/player_slots.h"
#include "kernel/kernel.h"

bool PcNetworkMatch();  // net_session.cpp
std::string PcPartyExtendInGame(const std::string& text);  // pc_party.cpp

namespace {

using kernel::R32;

// ---- the PC screens ------------------------------------------------------------------------------------------------

struct Choice {
    std::string label, value;
};
struct Setting {
    std::string key;    // pc_settings.ini name
    std::string label;  // row text
    std::string help[2];
    std::vector<Choice> choices;
    int def = 0;  // index of the default choice
};
struct PcScreen {
    const char* id;     // the screens are SuperLauncher_PC<id> (front end) and InGame_PC<id> (pause menu)
    const char* title;  // its title and its row in Options
    const char* row_help[2];
    std::vector<Setting> settings;
    bool resettable;     // X restores the defaults (the game's Cancel_Accept_Reset legend)
    bool unique_values;  // no two rows may hold the same non-empty value (key bindings)
    bool hidden = false; // not a row of Options: opened by another screen (the server browser's Filters)
    const char* on_entry = nullptr;   // console commands run after the lists are loaded, and on Accept after the save
    const char* on_accept = nullptr;
    int status_lines = 0;             // BackgroundText lines under a shorter list, set by its commands (Split Screen)
    const char* on_y = nullptr;       // Y opens another screen
    const char* external = nullptr;   // its values are not pc_settings.ini's: "!<external>_load" fills it, its Accept runs
                                      // "!<external>_save" (a profile's settings, the match rules)
};

std::vector<Choice> Numbers(std::initializer_list<const char*> v) {
    std::vector<Choice> c;
    for (const char* s : v) c.push_back({s, s});
    return c;
}

Setting Binding(const pc::Action& a) {
    Setting s{std::string("bind.") + a.id, a.label, {"Click the key or press Enter, then press a key or mouse",
                                                       "button (Escape cancels). The arrows also choose one."}};
    for (const pc::KeyInfo& k : pc::BindableKeys()) {
        if (std::string(k.name) == a.key) s.def = int(s.choices.size());
        s.choices.push_back({k.label, k.name});
    }
    return s;
}

// Resolution's choices: Native (the window's pixels, the display's in fullscreen), the display's modes as SDL3 lists
// them (CSX_DISPLAY_MODES from the window, largest first, the desktop's in CSX_DESKTOP_MODE), a WxH saved by hand that
// is not among them, and the title's own 480 lines. Headless there are no modes: Native draws at 480 lines there.
std::vector<Choice> Resolutions() {
    std::string desktop = std::getenv("CSX_DESKTOP_MODE") ? std::getenv("CSX_DESKTOP_MODE") : "";
    std::vector<Choice> c = {{desktop.empty() ? "Native" : "Native (" + desktop + ")", "native"}};
    std::string modes = std::getenv("CSX_DISPLAY_MODES") ? std::getenv("CSX_DISPLAY_MODES") : "";
    for (size_t at = 0; at < modes.size();) {
        size_t end = modes.find(',', at);
        std::string m = modes.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!m.empty()) c.push_back({m, m});
        at = end == std::string::npos ? modes.size() : end + 1;
    }
    std::string saved = pc::Get("resolution", "native");
    unsigned w = 0, h = 0;
    bool listed = false;
    for (const Choice& x : c) listed = listed || x.value == saved;
    if (!listed && std::sscanf(saved.c_str(), "%ux%u", &w, &h) == 2) c.push_back({saved + " (custom)", saved});
    c.push_back({"Original (480 lines)", "original"});
    return c;
}

const std::vector<PcScreen>& Screens() {
    static const std::vector<PcScreen> s = [] {
        std::vector<PcScreen> v;
        v.push_back({"Options",
                     "PC Options",
                     {"Set mouse and other options for playing", "on a PC."},
                     {
                         {"mouse_sensitivity",
                          "Mouse Sensitivity",
                          {"Set how far the view turns when you move the mouse.",
                           "3 matches the game's original setting."},
                          Numbers({"0.5", "1", "1.5", "2", "2.5", "3", "3.5", "4", "5", "6", "7", "8", "10"}),
                          5},
                         {"mouse_invert",
                          "Invert Mouse",
                          {"Turn on this option to reverse the vertical look", "of the mouse."},
                          {{"Off", "0"}, {"On", "1"}},
                          0},
                         {"mouse_raw",
                          "Raw Mouse Input",
                          {"Use the mouse's own movement, without the desktop's",
                           "pointer acceleration. Off uses the desktop's."},
                          {{"On", "1"}, {"Off", "0"}},
                          0},
                         {"fov",
                          "Field of View",
                          {"Set how wide the view is. 90 is the game's original;",
                           "zoomed weapons keep their own."},
                          {{"90 (Original)", "90"}, {"95", "95"}, {"100", "100"}, {"105", "105"}, {"110", "110"},
                           {"115", "115"}, {"120", "120"}},
                          0},
                         {"aspect",
                          "Aspect Ratio",
                          {"Set the screen's shape; Automatic follows the display.",
                           "Takes effect the next time the game starts."},
                          {{"Automatic", "auto"}, {"4:3 (Original)", "4:3"}, {"16:10", "16:10"}, {"16:9", "16:9"},
                           {"21:9", "21:9"}},
                          0},
                         {"resolution",
                          "Resolution",
                          {"Set the size the game is drawn at; Native matches",
                           "the screen. The menus keep their layout."},
                          Resolutions(),
                          0},
                         {"frame_rate",
                          "Frame Rate Limit",
                          {"Set how many frames are drawn each second.", "30 is the game's original rate."},
                          {{"30 (Original)", "30"}, {"60", "60"}, {"72", "72"}, {"100", "100"}, {"120", "120"},
                           {"144", "144"}, {"165", "165"}, {"240", "240"}},
                          0},
                         {"vsync",
                          "VSync",
                          {"Wait for the display's refresh before each frame.",
                           "Off lets the frame rate pass the refresh rate."},
                          {{"On", "1"}, {"Off", "0"}},
                          0},
                         {"xbox_effects",
                          "Xbox Graphical Effects",
                          {"Turn the original's Auto Exposure and Bloom",
                           "on or off together; Off keeps their own settings."},
                          {{"On", "1"}, {"Off", "0"}},
                          0},
                         {"auto_exposure",
                          "Auto Exposure",
                          {"Let the brightness follow what you look at, as the",
                           "original does. Off shows it unadjusted (darker)."},
                          {{"On", "1"}, {"Off", "0"}},
                          0},
                         {"bloom",
                          "Bloom",
                          {"Let bright light glow, as the original does.",
                           "Off draws without the glow."},
                          {{"On", "1"}, {"Off", "0"}},
                          0},
                         {"display_mode",
                          "Display Mode",
                          {"Play in a window or on the full screen.", "F11 also switches between them."},
                          {{"Windowed", "windowed"}, {"Fullscreen", "fullscreen"}},
                          0},
                         {"master_volume",
                          "Master Volume",
                          {"Set the volume of everything the game plays.", "Sound Options set its parts."},
                          Numbers({"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"}),
                          10},
                         // N4.7 (src/game/local_server.cpp): a match from the menus in csx-server, or in the game
                         {"server_process",
                          "Match Server",
                          {"Separate runs a match's server (bots, rules) in its",
                           "own process: smoother play. In Game is the original."},
                          {{"Separate", "1"}, {"In Game", "0"}},
                          0},
                     },
                     true,
                     false});
        PcScreen controls{"Controls",
                          "PC Controls",
                          {"Choose the keys and mouse buttons for moving,", "shooting and the other actions."},
                          {},
                          true,
                          true};
        for (const pc::Action& a : pc::Actions()) controls.settings.push_back(Binding(a));
        v.push_back(controls);
        // the server browser's filters (src/game/pc_browser.cpp reads them: browser_*), opened from its screen
        std::vector<Choice> maps = {{"Any", ""}};
        for (const char* m : {"cs_havana", "cs_italy", "cs_miami", "cs_militia", "cs_office", "de_airstrip", "de_aztec",
                              "de_cbble", "de_chateau", "de_corruption", "de_dust", "de_dust2", "de_fastline", "de_inferno",
                              "de_piranesi", "de_prodigy", "de_stadium", "de_tides", "de_truth", "de_vostok"})
            maps.push_back({m, m});
        PcScreen filters{"Filters",
                         "Server Filters",
                         {"", ""},
                         {
                             {"browser_sort",
                              "Sort By",
                              {"Choose the order of the server list.", "Players: the fullest servers first."},
                              {{"Ping", "ping"}, {"Players", "players"}, {"Server Name", "name"}, {"Map", "map"}},
                              0},
                             {"browser_order",
                              "Order",
                              {"Reversed turns the chosen order around.", ""},
                              {{"Normal", "0"}, {"Reversed", "1"}},
                              0},
                             {"browser_full",
                              "Full Servers",
                              {"Show or hide servers with no free slot.", ""},
                              {{"Show", "1"}, {"Hide", "0"}},
                              0},
                             {"browser_empty",
                              "Empty Servers",
                              {"Show or hide servers with no players or bots.", ""},
                              {{"Show", "1"}, {"Hide", "0"}},
                              0},
                             {"browser_password",
                              "Password Protected",
                              {"Show servers that ask for a password,", "hide them, or show only them."},
                              {{"Show", "any"}, {"Hide", "hide"}, {"Only", "only"}},
                              0},
                             {"browser_map", "Map", {"Show servers on one map only, or on Any map.", ""}, maps, 0},
                             {"browser_location",
                              "Location",
                              {"Favorites and Recent: servers on your network (LAN),", "on the internet, or Any."},
                              {{"Any", "any"}, {"LAN", "lan"}, {"Internet", "internet"}},
                              0},
                         },
                         true,
                         false,
                         true};
        v.push_back(filters);
        // split-screen's options (src/game/pc_split.cpp, docs/splitscreen.md), opened from the party screen (BACK):
        // the players are the party's (N4.8)
        PcScreen split{"Split",
                       "Split Screen Options",
                       {"", ""},
                       {
                           {"split_layout",
                            "Two Players",
                            {"Split the screen top and bottom, or side by side.", ""},
                            {{"Top and Bottom", "horizontal"}, {"Side by Side", "vertical"}},
                            0},
                           {"split_p1_pad",
                            "Player 1",
                            {"Player 1 plays with the keyboard and mouse, and can take", "the first controller too (all players on controllers)."},
                            {{"Keyboard and Mouse", "0"}, {"Also a Controller", "1"}},
                            0},
                       },
                       true,
                       false,
                       true,
                       "pc_split show",
                       "pc_split apply",
                       5};
        v.push_back(split);
        // a profile's own settings (N4.8, src/game/pc_party.cpp: Profiles > Y), loaded from and saved to that profile;
        // the PC Options rows (their choices as there) and the title's Controller Options (pad.*: their rows' values)
        PcScreen profile{"Profile", "Profile Settings", {"", ""}, {}, true, false, true};
        profile.external = "pc_party edit";
        for (const char* key : {"mouse_sensitivity", "mouse_invert", "mouse_raw", "fov"})
            for (const Setting& r : v[0].settings)
                if (r.key == key) profile.settings.push_back(r);
        const std::vector<Choice> off_on = {{"Off", "0"}, {"On", "1"}};
        profile.settings.push_back({"pad.invert", "Inverted Look", {"The controller's vertical look reversed.", ""}, off_on, 0});
        profile.settings.push_back({"pad.sensitivity", "Look Sensitivity", {"How fast the aiming thumbstick turns.", ""},
                                    {{"Very Low", "0"}, {"Low", "1"}, {"Medium", "2"}, {"High", "3"}, {"Very High", "4"}}, 1});
        profile.settings.push_back({"pad.fast_look", "Fast Look", {"Faster turning while the aiming thumbstick is held.", ""}, off_on, 0});
        profile.settings.push_back({"pad.left_hand", "Left-Hand Weapons", {"The weapon shown at the left of the screen.", ""}, off_on, 0});
        profile.settings.push_back({"pad.vibration", "Damage Vibration", {"The controller vibrates on damage.", ""}, off_on, 1});
        profile.settings.push_back({"pad.hints", "Automatic Hints", {"Help text shown in the game.", ""}, off_on, 1});
        v.push_back(profile);
        // the match rules (N4.8, src/game/pc_lobby.cpp: the game mode screen's Y), the server's own cvars only (no
        // maximum money: the game has none); loaded from and saved to the party's rules, Y the rulesets
        auto list = [](std::initializer_list<std::pair<const char*, const char*>> v) {
            std::vector<Choice> c;
            for (auto& [l, x] : v) c.push_back({l, x});
            return c;
        };
        std::vector<Choice> bots;
        for (int n = 0; n <= 16; n++) bots.push_back({std::to_string(n), std::to_string(n)});
        for (int n = 20; n <= 60; n += 4) bots.push_back({std::to_string(n), std::to_string(n)});
        bots.push_back({"61", "61"});
        PcScreen rules{"Rules", "Custom Rules", {"", ""}, {}, true, false, true};
        rules.external = "pc_lobby rules";
        rules.on_y = "open SuperLauncher_PCMatchRulesets";
        rules.settings = {
            {"bot_quota", "Bots", {"Computer players (at most 65 with the local players).", ""}, bots, 7},
            {"bot_difficulty", "Bot Difficulty", {"How well the bots play.", ""},
             list({{"Easy", "0"}, {"Normal", "1"}, {"Hard", "2"}, {"Expert", "3"}}), 1},
            {"bot_join_team", "Bot Team", {"The team the bots join.", ""},
             list({{"Both", "any"}, {"Terrorist", "t"}, {"Counter-Terrorist", "ct"}}), 0},
            {"mp_friendlyfire", "Friendly Fire", {"Shots hurt teammates.", ""}, off_on, 0},
            {"mp_roundtime", "Round Time", {"Minutes a round lasts.", ""},
             list({{"1 min", "1"}, {"1.5 min", "1.5"}, {"2 min", "2"}, {"2.5 min", "2.5"}, {"3 min", "3"}, {"4 min", "4"},
                   {"5 min", "5"}, {"6 min", "6"}, {"7 min", "7"}, {"8 min", "8"}, {"9 min", "9"}}), 6},
            {"mp_freezetime", "Freeze Time", {"Seconds before a round's players may move.", ""},
             list({{"None", "0"}, {"3 s", "3"}, {"6 s", "6"}, {"10 s", "10"}, {"15 s", "15"}}), 2},
            {"mp_buytime", "Buy Time", {"Minutes the buy menu stays open in a round.", ""},
             list({{"15 s", "0.25"}, {"30 s", "0.5"}, {"1 min", "1"}, {"1.5 min", "1.5"}, {"2 min", "2"}, {"3 min", "3"}}), 3},
            {"mp_startmoney", "Starting Money", {"Each player's money at the start.", ""},
             list({{"$800", "800"}, {"$1000", "1000"}, {"$2000", "2000"}, {"$4000", "4000"}, {"$6000", "6000"},
                   {"$8000", "8000"}, {"$10000", "10000"}, {"$16000", "16000"}}), 0},
            {"mp_maxrounds", "Rounds", {"The match ends after this many rounds.", ""},
             list({{"No Limit", "0"}, {"5", "5"}, {"7", "7"}, {"10", "10"}, {"12", "12"}, {"15", "15"}, {"20", "20"},
                   {"30", "30"}}), 2},
            {"mp_winlimit", "Win Limit", {"The match ends when a team wins this many rounds.", ""},
             list({{"No Limit", "0"}, {"4", "4"}, {"7", "7"}, {"10", "10"}, {"15", "15"}}), 1},
            {"mp_timelimit", "Time Limit", {"The match ends after this many minutes.", ""},
             list({{"No Limit", "0"}, {"10 min", "10"}, {"20 min", "20"}, {"30 min", "30"}, {"45 min", "45"},
                   {"60 min", "60"}}), 5},
            {"mp_limitteams", "Team Balance", {"How many more players one team may have.", ""},
             list({{"Off", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}}), 2},
            {"mp_autoteambalance", "Auto Team Balance", {"Players moved to even the teams between rounds.", ""}, off_on, 1},
            {"mp_c4timer", "C4 Timer", {"Seconds before a planted bomb explodes.", ""},
             list({{"35 s", "35"}, {"45 s", "45"}, {"60 s", "60"}, {"90 s", "90"}}), 1},
            {"pc_player_collision", "Player Collision", {"Players and bots block each other.", ""}, list({{"On", "1"}, {"Off", "0"}}), 0},
            {"allow_spectators", "Spectators", {"Dead players may join the spectators.", ""}, off_on, 0},
        };
        v.push_back(rules);
        return v;
    }();
    return s;
}

std::string FrontEndName(const PcScreen& s) { return std::string("SuperLauncher_PC") + s.id; }
std::string InGameName(const PcScreen& s) { return std::string("InGame_PC") + s.id; }

// the screen of either menu by its name
const PcScreen* FindScreen(const std::string& name) {
    for (const PcScreen& s : Screens())
        if (name == FrontEndName(s) || name == InGameName(s)) return &s;
    return nullptr;
}

// ---- the text, in the game's own format -----------------------------------------------------------------------------

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// the help text block every row of the front end's option screens carries (shown at the bottom while highlighted)
std::string HelpTable(const std::string* lines, int n, const char* indent) {
    std::string t = std::string(indent) + "Table\r\n" + indent + "{\r\n";
    t += std::string(indent) + "\tLayout\r\n" + indent + "\t{\r\n";
    for (const char* l : {"ColumnWidthPercentage \"100\"", "TableLocation \"75 361\"", "TableSize \"476 40\"",
                          "TableRowSpacing \"1\"", "TableTextScale \"0.75\"", "NonSelectFG \"0.24 0.29 0.33 1\""})
        t += std::string(indent) + "\t\t" + l + "\r\n";
    t += std::string(indent) + "\t}\r\n";
    for (int i = 0; i < n; i++)
        if (!lines[i].empty())
            t += std::string(indent) + "\tRow\r\n" + indent + "\t{\r\n" + indent + "\t\tCellFixed " + Quote(lines[i]) +
                 "\r\n" + indent + "\t}\r\n";
    return t + indent + "}\r\n";
}

std::string Pie() {
    return "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableIncreaseValue\"\r\n\t}\r\n"
           "\tGeneric_4_2\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n"
           "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableDecreaseValue\"\r\n\t}\r\n"
           "\tGeneric_4_4\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n";
}

std::string ListRows(const PcScreen& s, bool help) {
    std::string t;
    for (const Setting& r : s.settings) {
        t += "\t\tRow\r\n\t\t{\r\n";
        if (help) t += HelpTable(r.help, 2, "\t\t\t");
        t += "\t\t\tCellFixed " + Quote(r.label) + "\r\n\t\t\tCellList\r\n\t\t\t{\r\n";
        for (size_t i = 0; i < r.choices.size(); i++)
            t += std::string("\t\t\t\t") + (int(i) == r.def ? "ListDefault " : "ListOption ") +
                 Quote(r.choices[i].label) + "\t\t" + Quote(r.choices[i].value) + "\r\n";
        t += "\t\t\t}\r\n\t\t}\r\n";
    }
    return t;
}

std::string Commands(const std::string& name, const PcScreen& s) {
    if (s.external)  // a profile's settings (pc_party.cpp), the match rules (pc_lobby.cpp)
        return "\tOnEntry \"!" + std::string(s.external) + "_load\"\r\n\tOnButtonDownB \"back\"\r\n"
               "\tOnButtonDownA \"!" + std::string(s.external) + "_save; xbui back\"\r\n\tOnButtonDownX \"!pc_options_defaults " + name +
               "\"\r\n" + (s.on_y ? "\tOnButtonDownY \"" + std::string(s.on_y) + "\"\r\n" : std::string());
    std::string entry = s.on_entry ? std::string("; ") + s.on_entry : "", accept = s.on_accept ? std::string("; ") + s.on_accept : "";
    std::string t = "\tOnEntry \"!pc_options_load " + name + entry + "\"\r\n\tOnButtonDownB \"back\"\r\n"
                    "\tOnButtonDownA \"!pc_options_save " + name + accept + "; xbui back\"\r\n";
    if (s.resettable) t += "\tOnButtonDownX \"!pc_options_defaults " + name + "\"\r\n";
    if (s.on_y && name.rfind("SuperLauncher_", 0) == 0) t += "\tOnButtonDownY \"" + std::string(s.on_y) + "\"\r\n";
    return t;
}

// a screen's status lines (BackgroundText, set by its commands): under its list, above the help lines
std::string StatusLines(const PcScreen& s) {
    std::string t;
    for (int i = 0; i < s.status_lines; i++)
        t += "\tBackgroundText \"54 83 107 255 80 " + std::to_string(250 + 20 * i) + " 480 30 0.75\"\t\" \"\r\n";
    return t;
}

// the front end's screen: its Control Options screen (backgrounds, sounds, arrows, layout, help lines); the list
// scrolls inside the table's area above the help lines
std::string FrontEndScreen(const PcScreen& s) {
    std::string name = FrontEndName(s);
    return "\r\n" + name + "\r\n{\r\n"
           "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n"
           "\tHighlightSound \"menu/buy_hover.wav\"\r\n\tPieceOffsetDegrees \"0\"\r\n"
           "\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n"
           "\tBackgroundImage \"Back_2ndary.1Pane\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
           "\tBackgroundImage \"" + std::string(s.resettable ? "Cancel_Accept_Reset" : "Cancel_Accept") +
           " 200 200 200 255 0 411 640 69\"\r\n" + Commands(name, s) + StatusLines(s) +
           "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t" + Quote(s.title) + "\r\n" + Pie() +
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           "\t\t\tColumnWidthPercentage \"60\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
           "\t\t\tColumnWidthPercentage \"40\"\r\n\t\t\tColumnJustify \"center\"\r\n"
           "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
           "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"500 " + std::string(s.status_lines ? "140" : "250") + "\"\r\n"
           "\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
           "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 299.0 2.0 176.0 22.0\"\r\n"
           "\t\t\tNonHighlightImage \"Arrows_Unselected 1.0 1.0 1.0 1.0 299.0 2.0 176.0 22.0\"\r\n"
           "\t\t}\r\n" + ListRows(s, true) + "\t}\r\n}\r\n";
}

// the pause menu's screen: its Control Options screen (the overlay, colours, sounds, arrows; no help lines)
std::string InGameScreen(const PcScreen& s) {
    std::string name = InGameName(s);
    return "\r\n" + name + "\r\n{\r\n"
           "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_enter.wav\"\r\n"
           "\tSelectSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
           "\tPieceOffsetDegrees \"0\"\r\n\tCachedImage \"angleSelect\t\t\t\t255 255 255 255\"\r\n"
           "\tBackgroundImage \"csxbackground\t\t100 100 80 235\t\t0 0 640 480\"\r\n\tModal \"50\"\r\n"
           "\tBackgroundText \"177 169 54 255 65 45 400 30\"\t" + Quote(s.title) + "\r\n" +
           Commands(name, s) + Pie() +
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           "\t\t\tColumnWidthPercentage \"60\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
           "\t\t\tColumnWidthPercentage \"40\"\r\n\t\t\tColumnJustify \"center\"\r\n"
           "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
           "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"475 300\"\r\n"
           "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 257.0 2.0 218.0 22.0\"\r\n"
           "\t\t}\r\n" + ListRows(s, false) + "\t}\r\n}\r\n";
}

// Custom Game > Player Collision (src/game/pc_collision.cpp): a last row in the single player match's Custom Options,
// written as its rows are. The screen's choices are the match's command line (0xE2270 joins the table's values when
// Match Options is Custom), so the row's "+pc_player_collision" reaches the match before anyone spawns. It goes last:
// the screen's own code reaches its nine rows by position (0xE1DE0). The game keeps the screen's choices from one
// front end to the next (after a match the next match's command line still has them), this row's with them; Reset
// (X) gives On, the default, as for the other rows
std::string CollisionRow() {
    static const std::string help[2] = {"Choose whether players and bots block each other.",
                                        "Off lets them pass through; shots still hit."};
    return "\t\tRow\r\n\t\t{\r\n" + HelpTable(help, 2, "\t\t\t") +
           "\r\n\t\t\tCellFixed \"Player Collision\"\r\n\t\t\tCellList\r\n\t\t\t{\r\n"
           "\t\t\t\tListDefault \"On\"\t\t\t\"+pc_player_collision 1\"\r\n"
           "\t\t\t\tListOption \"Off\"\t\t\t\"+pc_player_collision 0\"\r\n\t\t\t}\r\n\t\t}\r\n";
}

// the single player Custom Options screen's table with the Player Collision row after its last; the text unchanged
// when the screen is not laid out as expected
std::string ExtendCustomOptions(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_SinglePlayerMatchCustomOptions\r\n");
    size_t next = screen == std::string::npos ? screen : text.find("\nSuperLauncher_", screen + 1);
    // the screen ends "\t\t}\r\n\t}\r\n}\r\n": the last row, the table, the screen
    size_t end = next == std::string::npos ? next : text.rfind("\t}\r\n}\r\n", next);
    if (end == std::string::npos || end < screen || text.find("+mp_c4timer", screen) > end) {
        LOGW("pc menu: the single player Custom Options not as expected, no Player Collision row");
        return text;
    }
    return text.substr(0, end) + CollisionRow() + text.substr(end);
}

// Custom Game > Number of Bots (FINDINGS.md "Bot count"): the single player match's Match Options list "1 bot" to
// "11 bots" (+bot_quota N on the match's command line, as its other rows). With the 65 player slots (G8.2B) 64 bots fit
// with the player. The list gains "0 bots" before its first choice and after its last "12 bots" to "16 bots", then
// "20 bots" to "64 bots" in fours (21 steps from the default to 64, not 57); the screen's code that reads it by
// position (0xE1D10, with the bots on one team at most 8) is replaced by PcBotCountLimit for the longer list
constexpr int kMaxBots = int(slots::kMaxPlayers) - 1;

std::vector<int> BotCounts() {
    std::vector<int> n;
    for (int i = 0; i <= 16; i++) n.push_back(i);
    for (int i = 20; i <= kMaxBots; i += 4) n.push_back(i);
    return n;
}

std::string ExtendBotCount(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_SinglePlayerMatchOptions\r\n");
    size_t next = screen == std::string::npos ? screen : text.find("\nSuperLauncher_", screen + 1);
    const std::string help = "CellFixed \"Select how many bots you want to play with.\"\r\n\t\t\t\t}\r\n";
    const std::string first = "\t\t\t\tListOption \"1 bot\"\t\t\t\"+wait +bot_quota 1\"\r\n";
    const std::string last = "\t\t\t\tListOption \"11 bots\"\t\t\"+wait +bot_quota 11\"\r\n";
    size_t h = screen == std::string::npos ? screen : text.find(help, screen);
    size_t f = h == std::string::npos ? h : text.find(first, h);
    size_t l = f == std::string::npos ? f : text.find(last, f);
    if (l == std::string::npos || l > next) {
        LOGW("pc menu: the single player Match Options not as expected, Number of Bots left as it is");
        return text;
    }
    std::string more;
    for (int n : BotCounts())
        if (n > 11)
            more += "\t\t\t\tListOption \"" + std::to_string(n) + " bots\"\t\t\"+wait +bot_quota " + std::to_string(n) +
                    "\"\r\n";
    std::string out = text.substr(0, l + last.size()) + more + text.substr(l + last.size());
    out.insert(f, "\t\t\t\tListOption \"0 bots\"\t\t\t\"+wait +bot_quota 0\"\r\n");
    out.insert(h + help.size(), "\t\t\t\tRow\r\n\t\t\t\t{\r\n\t\t\t\t\tCellFixed \"" + std::to_string(kMaxBots) +
                                    " bots and you fill the game's " + std::to_string(slots::kMaxPlayers) +
                                    " player slots.\"\r\n\t\t\t\t}\r\n");
    return out;
}

}  // namespace

// [[hook]] at 0xE1D2B (config/game.toml) in the single player Match Options' simulate (0xE1D10), where it has the
// table (ecx) and the Bot Team row's choice (edi: 0 both teams, 1 or 2 one team): the original then caps Number of
// Bots at its eighth choice ("8 bots") and disables the ninth to fifteenth while the bots join one team (a team was
// full at its map's spawn points, 8 on the smallest maps), enables them otherwise (the list's enable bytes at its cell
// +0x29 + choice). The 65 slots took that team limit away (G8.2B) and Bot Team's one-team choices already turn off
// auto team balance and mp_limitteams, so every choice of the list ExtendBotCount makes stays enabled with any Bot
// Team (29 choices, within the cell's 0x27 enable bytes); true leaves the function (0xE1D63) without the original's cap
bool PcBotCountLimit(X86Context& ctx, uint8_t*) {
    uint32_t list = R32(R32(ctx.ecx + 0x60) + 0xC) + 0x78;  // row 0's cells: the label, then the list
    for (size_t i = 0; i < BotCounts().size() && i < 0x27; i++)
        static_cast<uint8_t*>(kernel::HostPtr(list + 0x29 + uint32_t(i)))[0] = 1;
    return true;
}

namespace {

// the front end text: before View Credits in SuperLauncher_Options a row per screen (as its rows are written, with
// help), then the screens; empty when its layout is not the one expected
std::string ExtendFrontEnd(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_Options");
    size_t credits = screen == std::string::npos ? screen : text.find("CommandSelect \"!PlayCredits\"", screen);
    // the credits row's own line (two tabs; the rows of its help table have four)
    size_t row = credits == std::string::npos ? credits : text.rfind("\n\t\tRow\r\n", credits);
    if (row == std::string::npos || row < screen) return {};
    row += 1;
    std::string rows, screens;
    for (const PcScreen& s : Screens()) {
        std::string help[2] = {s.row_help[0], s.row_help[1]};
        if (s.hidden) {
            screens += FrontEndScreen(s);
            continue;
        }
        rows += "\t\tRow\r\n\t\t{\r\n" + HelpTable(help, 2, "\t\t\t") + "\t\t\tCommandSelect \"open " +
                FrontEndName(s) + "\"\r\n\t\t\tCellFixed " + Quote(s.title) + "\r\n\t\t}\r\n";
        screens += FrontEndScreen(s);
    }
    return text.substr(0, row) + rows + text.substr(row) + screens;
}

// the pause menu's text: after Online Options in its Options screen a row per screen (centred between blank cells as
// its rows are), then the screens; empty when its layout is not the one expected
// the Single Player pause menu (every PC match's: the title picks it by the game mode, 0xCA8F0) with the title's own
// Change Teams row after Match Statistics, as its system link menus have it (InGame_RootHost / RootClient: the team
// screen TeamSelect_StartMenu, the confirmation InGame_ConfirmSwitchTeams, then switch_to_terrorist /
// _counter_terrorist / _spectator / _auto, which the server's game code handles with its own team rules). Only in
// network and split-screen matches (PcNetworkMatch), not in an offline match in the game's own local server (a
// client of it, but the Single Player match the listen server ran: its pause menu as it was; a hosted one keeps
// CSX_MENU_HOST); unchanged when the menu is not laid out as expected
std::string ExtendPauseTeams(const std::string& text) {
    const char* host = std::getenv("CSX_MENU_HOST");
    bool offline_local = csx::LocalServerRunning() && csx::SplitPlayer() == 0 && !(host && *host == '1');
    if (!PcNetworkMatch() || offline_local) return text;
    size_t screen = text.find("\nInGame_RootSinglePlayer\r\n");
    size_t next = screen == std::string::npos ? screen : text.find("\nInGame_", screen + 1);
    size_t stats = screen == std::string::npos ? screen : text.find("\"open InGame_StatsDisplay\"", screen);
    size_t row = stats == std::string::npos ? stats : text.find("\n\t\t}\r\n", stats);
    if (row == std::string::npos || row > next || text.find("TeamSelect_StartMenu\r\n") == std::string::npos) {
        LOGW("pc menu: the Single Player pause menu not as expected, no Change Teams row");
        return text;
    }
    row += std::strlen("\n\t\t}\r\n");
    LOGI("pc menu: Change Teams in the pause menu (a network or split-screen match)");
    return text.substr(0, row) +
           "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect   \"open TeamSelect_StartMenu\"\r\n\t\t\tCellFixed \" \"\r\n"
           "\t\t\tCellFixed \"Change Teams\"\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n" +
           text.substr(row);
}

std::string ExtendInGame(const std::string& text) {
    size_t screen = text.find("\nOptions\r\n{");
    size_t online = screen == std::string::npos ? screen : text.find("\"open InGame_OnlineOptions\"", screen);
    size_t end = online == std::string::npos ? online : text.find("\n\t\t}\r\n", online);
    if (end == std::string::npos) return {};
    end += std::strlen("\n\t\t}\r\n");
    std::string rows, screens;
    for (const PcScreen& s : Screens()) {
        if (s.hidden) continue;
        rows += "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect   \"open " + InGameName(s) +
                "\"\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed " + Quote(s.title) +
                "\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n";
        screens += InGameScreen(s);
    }
    return PcPartyExtendInGame(ExtendPauseTeams(text.substr(0, end) + rows + text.substr(end) + screens));
}

constexpr uint32_t kExtra = 512 * 1024;  // room added to the load buffer (the controls screens' lists are long)
uint32_t g_pending_size = 0;             // the UI file's own size while its load is under way, 0 otherwise

std::string GuestString(uint32_t va) {
    std::string s;
    for (int i = 0; va && i < 512; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}

// ---- the game's UI objects ---------------------------------------------------------------------------------------
// (stuffUItoPlayerData 0xDAD40 and the selection writer at 0xD7920 read and write them so): screens in a list from
// 0x5523D0 (name pointer at +0, next at +0x378, table at +0x380), a table's rows at +0x60..+0x64 (32 bytes each), a
// row's cells at +0xC..+0x10 (0x78 bytes each), a list cell's current choice at +0 and its choices at +0xC..+0x10
// (0x1C bytes each). Only these few functions know the layout.

uint32_t FindTable(const std::string& screen) {
    for (uint32_t s = R32(0x5523D0); s; s = R32(s + 0x378))
        if (GuestString(R32(s)) == screen) return R32(s + 0x380);
    return 0;
}
uint32_t ListCell(uint32_t table, size_t row) {
    uint32_t rows = R32(table + 0x60), end = R32(table + 0x64);
    if (rows + (row + 1) * 0x20 > end) return 0;
    uint32_t r = rows + uint32_t(row) * 0x20, cells = R32(r + 0xC);
    return cells + 2 * 0x78 <= R32(r + 0x10) ? cells + 0x78 : 0;  // cell 1: the list beside the label
}
uint32_t Choices(uint32_t cell) { return (R32(cell + 0x10) - R32(cell + 0xC)) / 0x1C; }

// the list cells of a PC screen's rows, empty when the screen is not the one declared
std::vector<uint32_t> Cells(const std::string& name, const PcScreen& s) {
    uint32_t table = FindTable(name);
    std::vector<uint32_t> cells;
    if (!table) {
        LOGW("pc menu: no %s screen", name.c_str());
        return {};
    }
    for (size_t i = 0; i < s.settings.size(); i++) {
        uint32_t cell = ListCell(table, i);
        if (!cell || Choices(cell) != s.settings[i].choices.size()) {
            LOGW("pc menu: %s row %zu is not the list expected", name.c_str(), i);
            return {};
        }
        cells.push_back(cell);
    }
    return cells;
}

int IndexOf(const Setting& r, const std::string& value) {
    for (size_t c = 0; c < r.choices.size(); c++)
        if (r.choices[c].value == value) return int(c);
    return r.def;
}

// a label (Latin-1, the menus' text) for the log
std::string Utf8(const std::string& latin1) {
    std::string out;
    for (unsigned char c : latin1) {
        if (c < 0x80) out += char(c);
        else out += {char(0xC0 | c >> 6), char(0x80 | (c & 0x3F))};
    }
    return out;
}

void LoadScreen(const std::string& name, const PcScreen& s) {
    std::vector<uint32_t> cells = Cells(name, s);
    for (size_t i = 0; i < cells.size(); i++) {
        const Setting& r = s.settings[i];
        kernel::W32(cells[i], uint32_t(IndexOf(r, pc::Get(r.key, r.choices[size_t(r.def)].value))));
    }
}

void DefaultScreen(const std::string& name, const PcScreen& s) {
    std::vector<uint32_t> cells = Cells(name, s);
    for (size_t i = 0; i < cells.size(); i++) kernel::W32(cells[i], uint32_t(s.settings[i].def));
    if (!cells.empty()) LOGI("pc menu: %s shows its defaults", name.c_str());
}

// the rows' choices saved; with unique values a value chosen twice stays with the first row and the later row is
// cleared (shown as its empty choice, "None", from then on)
void SaveScreen(const std::string& name, const PcScreen& s) {
    std::vector<uint32_t> cells = Cells(name, s);
    if (cells.empty()) return;
    std::map<std::string, size_t> taken;
    for (size_t i = 0; i < cells.size(); i++) {
        const Setting& r = s.settings[i];
        uint32_t pick = R32(cells[i]);
        if (pick >= r.choices.size()) continue;
        std::string value = r.choices[pick].value;
        if (s.unique_values && !value.empty()) {
            auto [it, fresh] = taken.emplace(value, i);
            if (!fresh) {
                LOGI("pc menu: %s is already %s's, %s is left unbound", Utf8(r.choices[pick].label).c_str(),
                     s.settings[it->second].label.c_str(), r.label.c_str());
                value.clear();
                kernel::W32(cells[i], uint32_t(IndexOf(r, "")));
            }
        }
        pc::Set(r.key, value);
    }
    pc::Save();
}

// ---- press-a-key rebinding ----------------------------------------------------------------------------------------
// A list cell's choices are the game's std::wstring (MSVC, 0x1C bytes: +4 up to 7 UTF-16 characters in place, or
// from 8 on a pointer to them, +0x14 the length, +0x18 the capacity); while a row waits, its shown choice points at
// "Press a key" (UTF-16 in a block of the game's heap, made once) and is put back byte for byte afterwards, so the
// game never frees or grows a string it did not make.

constexpr uint32_t kCurrentScreen = 0x5523D4;
constexpr uint32_t kMalloc = 0x22D86A;  // the CRT's malloc (size), cdecl
constexpr char kPressAKey[] = "Press a key";

struct Capture {
    bool active = false;
    std::string screen;
    size_t row = 0;
    uint32_t cell = 0, shown = 0;  // the list cell, the choice whose text is swapped
    uint8_t saved[0x1C] = {};
};
Capture g_capture;
std::atomic<bool> g_capturing{false}, g_row_selected{false};
std::mutex g_bind_lock;
bool g_start = false;
std::deque<int> g_bind_keys;

void ShowChoice(X86Context& ctx, uint32_t choice, const char* text) {
    static uint32_t buffer = 0;
    uint32_t n = uint32_t(std::strlen(text));
    if (!buffer) buffer = kernel::CallGuest(ctx, kMalloc, {(n + 1) * 2});
    if (!buffer) return;
    for (uint32_t i = 0; i <= n; i++) {
        uint16_t c = static_cast<unsigned char>(text[i]);
        std::memcpy(static_cast<uint8_t*>(kernel::HostPtr(buffer)) + i * 2, &c, 2);
    }
    kernel::W32(choice + 4, buffer);
    kernel::W32(choice + 0x14, n);
    kernel::W32(choice + 0x18, n);
}

void EndCapture(const char* how) {
    if (!g_capture.active) return;
    std::memcpy(kernel::HostPtr(g_capture.shown), g_capture.saved, sizeof g_capture.saved);
    g_capture.active = false;
    g_capturing = false;
    LOGI("pc menu: key capture %s", how);
}

std::string ScreenName() {
    uint32_t screen = R32(kCurrentScreen);
    return screen ? GuestString(R32(screen)) : std::string();
}

void StartCapture(X86Context& ctx) {
    std::string name = ScreenName();
    const PcScreen* s = FindScreen(name);
    uint32_t table = s && s->unique_values ? R32(R32(kCurrentScreen) + 0x380) : 0;
    if (!table || g_capture.active) return;
    size_t row = R32(table + 0x9C);
    std::vector<uint32_t> cells = Cells(name, *s);
    if (row >= cells.size()) return;
    uint32_t cell = cells[row], pick = R32(cell);
    if (pick >= Choices(cell)) return;
    g_capture = {true, name, row, cell, R32(cell + 0xC) + pick * 0x1C, {}};
    std::memcpy(g_capture.saved, kernel::HostPtr(g_capture.shown), sizeof g_capture.saved);
    ShowChoice(ctx, g_capture.shown, kPressAKey);
    g_capturing = true;
    LOGI("pc menu: %s waits for a key", s->settings[row].label.c_str());
}

// the pressed key (engine number) as the row's choice; a key the list does not hold leaves the wait on
void CaptureKey(int key) {
    const PcScreen* s = FindScreen(g_capture.screen);
    if (!s) return EndCapture("ended (no screen)");
    if (key < 0) return EndCapture("cancelled");
    const Setting& r = s->settings[g_capture.row];
    for (size_t c = 0; c < r.choices.size(); c++) {
        const pc::KeyInfo* k = pc::FindKey(r.choices[c].value);
        if (!k || k->number != key || r.choices[c].value.empty()) continue;
        EndCapture("done");
        kernel::W32(g_capture.cell, uint32_t(c));
        LOGI("pc menu: %s set to %s (saved with Accept)", r.label.c_str(), Utf8(r.choices[c].label).c_str());
        return;
    }
    LOGI("pc menu: key %d cannot be bound, still waiting", key);
}

}  // namespace

namespace csx {
bool BindRowSelected() { return g_row_selected; }
bool BindCapturing() { return g_capturing; }
void BindStart() {
    std::lock_guard<std::mutex> lock(g_bind_lock);
    g_start = true;
}
void BindInput(int key) {
    std::lock_guard<std::mutex> lock(g_bind_lock);
    g_bind_keys.push_back(key);
}
}  // namespace csx

// a PC screen's rows as values (key -> the chosen choice's value) and set from values (a key not given: its
// default), for screens whose values are not pc_settings.ini's (pc_party.cpp's profile settings)
std::map<std::string, std::string> PcScreenValues(const std::string& name) {
    std::map<std::string, std::string> out;
    const PcScreen* s = FindScreen(name);
    if (!s) return out;
    std::vector<uint32_t> cells = Cells(name, *s);
    for (size_t i = 0; i < cells.size(); i++) {
        uint32_t pick = R32(cells[i]);
        if (pick < s->settings[i].choices.size()) out[s->settings[i].key] = s->settings[i].choices[pick].value;
    }
    return out;
}

void PcSetScreenValues(const std::string& name, const std::map<std::string, std::string>& values) {
    const PcScreen* s = FindScreen(name);
    if (!s) return;
    std::vector<uint32_t> cells = Cells(name, *s);
    for (size_t i = 0; i < cells.size(); i++) {
        const Setting& r = s->settings[i];
        auto it = values.find(r.key);
        kernel::W32(cells[i], uint32_t(it == values.end() ? r.def : IndexOf(r, it->second)));
    }
}

// a screen's table's highlighted row (the title's own map list, N4.8's copy of it), -1 when not loaded; and set
int PcSelectedRow(const std::string& screen) {
    uint32_t table = FindTable(screen);
    return table ? int(R32(table + 0x9C)) : -1;
}

void PcSetSelectedRow(const std::string& screen, int row) {
    if (uint32_t table = FindTable(screen)) kernel::W32(table + 0x9C, uint32_t(row));
}

// a PC screen's defaults (key -> value) and every value a row may have (the match rules' checks, pc_lobby.cpp)
std::map<std::string, std::string> PcScreenDefaults(const std::string& name) {
    std::map<std::string, std::string> out;
    if (const PcScreen* s = FindScreen(name))
        for (const Setting& r : s->settings) out[r.key] = r.choices[size_t(r.def)].value;
    return out;
}

std::map<std::string, std::vector<std::string>> PcScreenChoices(const std::string& name) {
    std::map<std::string, std::vector<std::string>> out;
    if (const PcScreen* s = FindScreen(name))
        for (const Setting& r : s->settings)
            for (const Choice& c : r.choices) out[r.key].push_back(c.value);
    return out;
}

// any screen's list rows by position (the title's own: Controller Options, N4.8's profiles): the choice index of each
// row's list (-1 a row without one), and set so; empty / false when the screen is not loaded
std::vector<int> PcListRows(const std::string& screen) {
    std::vector<int> out;
    uint32_t table = FindTable(screen);
    if (!table) return out;
    for (size_t row = 0; row < 64; row++) {
        uint32_t rows = R32(table + 0x60), end = R32(table + 0x64);
        if (rows + (row + 1) * 0x20 > end) break;
        uint32_t cell = ListCell(table, row);
        out.push_back(cell && Choices(cell) ? int(R32(cell)) : -1);
    }
    return out;
}

bool PcSetListRows(const std::string& screen, const std::vector<int>& picks) {
    uint32_t table = FindTable(screen);
    if (!table) return false;
    for (size_t row = 0; row < picks.size(); row++) {
        uint32_t cell = ListCell(table, row);
        if (!cell || picks[row] < 0) continue;
        if (uint32_t(picks[row]) < Choices(cell)) kernel::W32(cell, uint32_t(picks[row]));
    }
    return true;
}

// a PC binding screen of either menu (pc_mouse.cpp: a click on a row's key name waits for a key there)
bool PcBindingScreen(const std::string& name) {
    const PcScreen* s = FindScreen(name);
    return s && s->unique_values;
}

// on the game thread every frame (pc_keys.cpp's PcKeysFrame): the capture started, fed or ended with its screen
void PcBrowserMenuFrame(X86Context& ctx);  // pc_browser.cpp
void PcSplitMenuFrame(X86Context& ctx);    // pc_split.cpp
void PcPartyMenuFrame(X86Context& ctx);    // pc_party.cpp
void PcLobbyMenuFrame(X86Context& ctx);    // pc_lobby.cpp

void PcMenuFrame(X86Context& ctx) {
    PcBrowserMenuFrame(ctx);
    PcSplitMenuFrame(ctx);
    PcPartyMenuFrame(ctx);
    PcLobbyMenuFrame(ctx);
    bool start;
    std::deque<int> keys;
    {
        std::lock_guard<std::mutex> lock(g_bind_lock);
        start = std::exchange(g_start, false);
        keys.swap(g_bind_keys);
    }
    std::string name = ScreenName();
    if (g_capture.active && (name != g_capture.screen || csx::ConsoleOpen())) EndCapture("ended (screen left)");
    g_row_selected = PcBindingScreen(name) && !csx::ConsoleOpen();
    if (start && g_row_selected) StartCapture(ctx);
    for (int k : keys)
        if (g_capture.active) CaptureKey(k);
}

namespace {

}  // namespace

// [[hook]] at 0xCDA2E (config/game.toml): the file's size was just taken into ebp, the name is at [esp+0x10]; a menu
// file (FrontEndUI*.txt, InGameUI*.txt) gets room for the additions (the buffer is size + 1, the read stops at the
// file's end)
void PcUiLoadSize(X86Context& ctx, uint8_t*) {
    g_pending_size = 0;
    std::string name = GuestString(R32(ctx.esp + 0x10));
    if (name.rfind("FrontEndUI", 0) != 0 && name.rfind("InGameUI", 0) != 0) return;
    g_pending_size = ctx.ebp;
    ctx.ebp += kExtra;
}

void PcMouseLearnUi(const std::string& text);  // pc_mouse.cpp
std::string ExtendScoreboard(const std::string& text);  // scoreboard.cpp
std::string ExtendMultiplayer(const std::string& text);  // pc_browser.cpp
void PcBrowserCommand(X86Context& ctx, const std::vector<std::string>& args);  // pc_browser.cpp
void PcSplitCommand(X86Context& ctx, const std::vector<std::string>& args);    // pc_split.cpp
std::string ExtendSplitMenu(const std::string& text);                          // pc_split.cpp
std::string ExtendParty(const std::string& text);                              // pc_party.cpp
std::string ExtendLobby(const std::string& text);                              // pc_lobby.cpp
void PcLobbyCommand(X86Context& ctx, const std::vector<std::string>& args);    // pc_lobby.cpp
void PcPartyCommand(X86Context& ctx, const std::vector<std::string>& args);    // pc_party.cpp
void PcPlayerCollisionCommand(const std::string& arg);  // pc_collision.cpp

// [[hook]] at 0xCDB1A: read and closed, esi = the buffer (ebp + 1 bytes, the file's text first); the extended text
// replaces it and ebp becomes its length (the size the caller is told)
void PcUiLoadText(X86Context& ctx, uint8_t*) {
    if (!g_pending_size) return;
    uint32_t size = std::exchange(g_pending_size, 0u), room = ctx.ebp;
    std::string text(static_cast<const char*>(kernel::HostPtr(ctx.esi)), size);
    std::string name = GuestString(R32(ctx.esp + 0x10));
    if (const char* d = std::getenv("CSX_UI_DUMP"); d && *d == '1') {  // the game's menu text as loaded, for reading
        std::filesystem::create_directories(app::ExeDir() / "uidump");
        std::ofstream(app::ExeDir() / "uidump" / name, std::ios::binary) << text;
    }
    std::string out = name.rfind("FrontEndUI", 0) == 0 ? ExtendFrontEnd(ExtendLobby(ExtendParty(ExtendSplitMenu(ExtendMultiplayer(ExtendBotCount(ExtendCustomOptions(text)))))))
                                                       : ExtendInGame(text);
    if (name.rfind("InGameUI", 0) == 0 && !out.empty()) out = ExtendScoreboard(out);
    PcMouseLearnUi(out.empty() ? text : out);
    if (const char* d = std::getenv("CSX_UI_DUMP"); d && *d == '1' && !out.empty())  // and as extended
        std::ofstream(app::ExeDir() / "uidump" / (name + ".pc"), std::ios::binary) << out;
    if (out.empty() || out.size() > room) {
        LOGW("pc menu: %s left as it is (%s)", name.c_str(), out.empty() ? "layout not recognised" : "no room");
        out = text;
    } else {
        LOGI("pc menu: PC screens added to %s", name.c_str());
    }
    std::memcpy(kernel::HostPtr(ctx.esi), out.data(), out.size());
    static_cast<char*>(kernel::HostPtr(ctx.esi))[out.size()] = 0;
    ctx.ebp = uint32_t(out.size());
}

// [[hook]] at 0xDADC5, 0xDB809 and 0xDB925: the profile's walks over every screen's list rows (stuffUItoPlayerData
// 0xDAD40 and the two that read the profile back, 0xDB7xx / 0xDB8xx), esi the screen's name, just before they compare
// it with "PlayerMapVoting" (the one screen they skip). Each list row takes one byte of a 128-byte block (the save's
// on the stack at esp+0x2C, then the profile at 0x552FC9): the PC screens' lists are no profile setting (their values
// are in pc_settings.ini) and with the server browser's filters they ran past the 128 bytes (the stack cookie check
// ended the title at the main menu). PC screens are skipped as that one is: true jumps to the skip (0xDAE95, 0xDB8C0,
// 0xDB9EF). Their place after every original screen kept the original rows' bytes where they were.
bool PcProfileSkipScreen(X86Context& ctx, uint8_t*) {
    std::string name = GuestString(ctx.esi);
    return name.rfind("SuperLauncher_PC", 0) == 0 || name.rfind("InGame_PC", 0) == 0;
}

// [[hook]] at 0xDCB43 and 0xDCBCB (on true to 0xDCB4A / 0xDCBD2): the table's list steps (tableIncreaseValue 0xDCAD0,
// tableDecreaseValue 0xDCB60, the pad's right and left) pass over a choice whose enable byte (cell +0x29 + index, edx
// the index) is 0. The bytes hold 0x27 choices, as many as the original lists have; a longer list (PC Controls' keys,
// over 100) reads the cell's other fields and the next cell there and skipped every fourth key: from 0x27 on a choice
// is taken as enabled
bool PcListChoiceEnabled(X86Context& ctx, uint8_t*) { return ctx.edx >= 0x27; }

// [[hook]] at 0x3CA25 in Cmd_ExecuteString, the command just tokenized (cmd_argc 0x4253E8, cmd_argv 0x4251A8): true
// serves a pc_ command here and leaves the function
bool PcConsoleCommand(X86Context& ctx, uint8_t*) {
    if (!R32(0x4253E8)) return false;
    std::string cmd = GuestString(R32(0x4251A8));
    if (cmd == "pc_split") {
        std::vector<std::string> args;
        for (uint32_t i = 0; i < R32(0x4253E8) && i < 8; i++) args.push_back(GuestString(R32(0x4251A8 + 4 * i)));
        PcSplitCommand(ctx, args);
        return true;
    }
    if (cmd == "pc_lobby") {
        std::vector<std::string> args;
        for (uint32_t i = 0; i < R32(0x4253E8) && i < 8; i++) args.push_back(GuestString(R32(0x4251A8 + 4 * i)));
        PcLobbyCommand(ctx, args);
        return true;
    }
    if (cmd == "pc_party") {
        std::vector<std::string> args;
        for (uint32_t i = 0; i < R32(0x4253E8) && i < 8; i++) args.push_back(GuestString(R32(0x4251A8 + 4 * i)));
        PcPartyCommand(ctx, args);
        return true;
    }
    if (cmd == "pc_browser") {
        std::vector<std::string> args;
        for (uint32_t i = 0; i < R32(0x4253E8) && i < 8; i++) args.push_back(GuestString(R32(0x4251A8 + 4 * i)));
        PcBrowserCommand(ctx, args);
        return true;
    }
    if (cmd == "pc_player_collision") {
        PcPlayerCollisionCommand(R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string());
        return true;
    }
    if (cmd != "pc_options_load" && cmd != "pc_options_save" && cmd != "pc_options_defaults") return false;
    std::string name = R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string();  // argument 1: the screen
    const PcScreen* s = FindScreen(name);
    if (!s) LOGW("pc menu: %s for an unknown screen %s", cmd.c_str(), name.c_str());
    else if (cmd == "pc_options_load") LoadScreen(name, *s);
    else if (cmd == "pc_options_save") SaveScreen(name, *s);
    else DefaultScreen(name, *s);
    return true;
}
