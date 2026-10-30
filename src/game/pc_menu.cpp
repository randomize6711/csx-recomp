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
#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "common/mods.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/local_server.h"
#include "game/net_engine.h"
#include "game/player_slots.h"
#include "kernel/kernel.h"

bool PcNetworkMatch();  // net_session.cpp
void PcSetScreenLineColour(X86Context& ctx, const char* screen, int index, const std::string& text, float r, float g, float b,
                           float a);  // pc_browser.cpp
std::string PcPartyExtendInGame(const std::string& text);  // pc_party.cpp
std::string ExtendMapSearch(const std::string& text);  // pc_mapsearch.cpp
std::string ExtendDlcMaps(const std::string& text);  // pc_dlc.cpp
std::string ExtendPrompts(const std::string& text);  // pc_prompts.cpp
std::string PcPromptName(const std::string& slot);
void PcPromptsFrame(X86Context& ctx);
bool PcRadioClientCommand(const std::string& name);  // pc_radio.cpp
void PcServerFeatures(const std::string& args);
void PcBuyCommand(const std::string& cmd);  // pc_buy.cpp
std::string PcBuyAlias(const std::string& cmd);
void PcBuyItem(const std::string& item);
void PcBuyFrame(X86Context& ctx);
void PcHudFrame();  // pc_hud.cpp
void PcMapSearchCommand(X86Context& ctx, const std::string& what);

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
    std::vector<const char*> opens = {};  // a hub (N5A): no settings, a row opening each of these screens (by id)
    bool panel = false;  // the highlighted row's description beside the list (N5B: the graphics options)
};

// the controller's extra actions' buttons (N7, src/linux/window_input.cpp Poll)
std::vector<Choice> PadExtraChoices() {
    return {{"None", "none"},
            {"Back + Up", "back_up"},
            {"Back + Down", "back_down"},
            {"Back + Left", "back_left"},
            {"Back + Right", "back_right"},
            {"Share / Capture", "share"},
            {"Paddle 1", "paddle1"},
            {"Paddle 2", "paddle2"},
            {"Paddle 3", "paddle3"},
            {"Paddle 4", "paddle4"},
            {"Touchpad", "touchpad"}};
}

std::vector<Choice> Numbers(std::initializer_list<const char*> v) {
    std::vector<Choice> c;
    for (const char* s : v) c.push_back({s, s});
    return c;
}

// ---- details and typed numbers (N5B) ----------------------------------------------------------------------------
// A setting's longer description (its Details screen, Y on its row; paragraphs, wrapped there) and, for a number, the
// range a typed value may have (a value typed on the keyboard while the row is highlighted: kept as the row's own
// "(custom)" choice when it is none of the list's). By key: the rows a profile and the match rules copy have them too.
struct Extra {
    const char* key;
    std::vector<std::string> details;
    double lo = 0, hi = 0;  // a number's range (hi > lo), inclusive
    bool integer = false;
};

const Extra* ExtraOf(const std::string& key) {
    static const std::vector<Extra> extras = {
        {"display_mode",
         {"Windowed keeps the game in a window you can move and resize. Fullscreen covers the whole display.",
          "F11 or Alt+Enter switches between the two at any time; the choice is saved."}},
        {"resolution",
         {"Native draws at the window's own pixels (the display's in Fullscreen). The display's modes are listed largest "
          "first; Original draws the title's 480 lines and scales them up.",
          "The menus and the HUD keep their layout at every size. A change takes effect at once."}},
        {"aspect",
         {"Automatic follows the window's shape. 4:3 is the original's shape; the wider shapes show more at the sides.",
          "Takes effect the next time the game starts."}},
        {"frame_rate",
         {"The most frames drawn each second. 30 is the original's rate; movement keeps the same speed at every limit.",
          "With VSync On the display's refresh rate limits the frame rate too."},
         30,
         1000,
         true},
        {"vsync",
         {"On waits for the display's refresh before showing each frame: no tearing, and no more frames than the display "
          "shows.",
          "Off shows each frame as soon as it is ready, up to the Frame Rate Limit; tearing may show."}},
        {"fov",
         {"How wide the view is, in degrees. 90 is the original's. A scoped weapon's zoom keeps its own view.",
          "Wider views show more at the edges of the screen and make things in the middle look smaller."},
         60,
         130,
         true},
        {"viewmodel_fov",
         {"The first-person weapon's own field of view, in degrees (at 4:3, as Field of View). Lower draws it larger "
          "and closer; the world's view does not change. Same as Field of View draws it with the world's, as the "
          "original does (a wide Field of View then shows it smaller)."},
         50,
         130,
         true},
        {"xbox_effects",
         {"On uses Auto Exposure and Bloom as they are set below. Off turns both off without changing their own "
          "settings, for the plain look in one step."}},
        {"auto_exposure",
         {"The original's adaptive brightness (cl_dynamicgamma): the picture brightens or darkens with what you look at, "
          "as on the Xbox.",
          "Off uses the fixed brightness of the title's own settings, which is darker in most places."}},
        {"bloom",
         {"The original's glow around bright light (cl_hdrbloom); each map sets its own strength.",
          "On most maps it adds little: mainly a bright sky glows. Off skips its passes."}},
        {"mouse_sensitivity",
         {"How far the view turns for each step the mouse moves. 3 matches the original's default.",
          "A scoped weapon turns at the same sensitivity."},
         0.1,
         20},
        {"mouse_zoom_sensitivity",
         {"Through a scope (the AWP, Scout, SG 550, G3SG1, AUG and SG 552 zoomed) the view turns this share of Mouse "
          "Sensitivity: below 1 for finer aim at long range. 1 turns as without a scope, as the original does."},
         0.05,
         5},
        {"mouse_invert", {"On: moving the mouse forward looks down, as a flight stick does."}},
        {"mouse_raw",
         {"On reads the mouse's own movement, so the desktop's pointer speed and acceleration do not change your aim.",
          "Off moves the view as the desktop moves its pointer, acceleration included."}},
        {"master_volume",
         {"Scales everything the game plays: effects, voices, ambient sound and music. The title's own Options > Sound "
          "sets each of them (saved with the profile, as on the Xbox)."},
         0,
         100,
         true},
        {"audio_device",
         {"The sound card or headset the game plays through. System Default follows the system's choice, also when it "
          "changes. A change takes effect at once."}},
        {"audio_background",
         {"On: the game keeps playing its sound while another window has the keyboard. Off silences it until the game's "
          "window is in front again (in split screen: the shared window)."}},
        {"net_graph_mode",
         {"The engine's network graph (net_graph): the frame rate, the data in and out each second, the server's "
          "update rate, lost and choked packets and a column a packet for its latency. Always On keeps it in the bottom "
          "right corner, With Scoreboard only while the scoreboard is up. Off leaves net_graph to the console."}},
        {"net_graph_level",
         {"Graph: the columns and the main numbers (net_graph 1). Graph and Details: more numbers (2). Numbers: the "
          "numbers alone (3)."}},
        {"prompt_style",
         {"Automatic shows the buttons of the device used last: the keyboard's keys, or the controller's (an Xbox, "
          "PlayStation, Switch, Steam or Wii controller is recognised by its make; any other shows Xbox 360 buttons).",
          "A choice keeps those buttons whatever is used. Each profile keeps its own."}},
        {"server_process",
         {"Separate runs a match started from the menus (Custom Game, Local Only) in its own server process, csx-server: "
          "the bots and the rules run there and the game only plays, which keeps play smooth with many bots.",
          "In Game runs the server inside the game, as the original does."}},
        {"menu_smooth_scroll",
         {"On lets a list that is longer than its screen glide by a row when the highlight moves past its edge.",
          "Off moves it a whole row at once, as the original does."}},
        {"net_update_rate",
         {"The snapshots a second this PC asks a server for (cl_updaterate). Our servers send 30 by default and up to "
          "60; a server may send fewer than asked."}, 10, 100, true},
        {"net_cmd_rate",
         {"How many times a second this PC sends your moves to a server (cl_cmdrate). More can make movement and shots "
          "reach the server sooner, at the cost of upload."}, 10, 100, true},
        {"net_rate",
         {"The most bytes a second a server may send this PC (rate). Large matches (many players and bots) need more; "
          "too little shows as choppy players."}, 10000, 1000000, true},
        // the match rules (pc_lobby.cpp; the dedicated server's own ranges, src/server/server_config.cpp)
        {"bot_quota", {"Bots that join the match. With the local players at most 65 take part."}, 0, 61, true},
        {"mp_roundtime", {"Minutes a round lasts before the time runs out."}, 1, 9},
        {"mp_freezetime", {"Seconds at a round's start before anyone may move."}, 0, 60, true},
        {"mp_buytime", {"Minutes from a round's start in which players may buy in their buy zone."}, 0.25, 9},
        {"mp_startmoney", {"Each player's money when they join and when the match starts."}, 800, 16000, true},
        {"mp_maxrounds", {"The match ends after this many rounds; 0 for no limit."}, 0, 999, true},
        {"mp_winlimit", {"The match ends when a team has won this many rounds; 0 for no limit."}, 0, 999, true},
        {"mp_timelimit", {"The match ends after this many minutes; 0 for no limit."}, 0, 999, true},
        {"mp_limitteams", {"How many more players one team may have than the other; 0 turns the limit off."}, 0, 32, true},
        {"mp_c4timer", {"Seconds a planted bomb takes to explode."}, 10, 90, true},
        {"pc_gravity",
         {"The match's gravity (the server's sv_gravity): 800 is the original's. Lower makes jumps higher and falls "
          "slower, higher the reverse. The server's value counts: it is sent to every player."}, 50, 4000, true},
        {"pc_fraglimit", {"Deathmatch: the match ends when a player has this many kills (frags); 0 for no limit. The "
                          "time limit still applies."}, 0, 999, true},
        {"pc_gamemode",
         {"Classic: the original rounds, objectives and economy.",
          "Deathmatch: no rounds. A dead player spawns again after 2.5 seconds with $16000 and 2 seconds of protection "
          "(a glow); the buy zone opens at each spawn. The match ends at the time or frag limit.",
          "Gun Game: no rounds, no buying. Each kill moves you to the next weapon of 23 (pistols, shotguns, SMGs, "
          "rifles, the AWP, the M249, the knife last); a knife kill at the last level wins.",
          "Bomb and hostage objectives still play out in Deathmatch and Gun Game but end no round."}},
        {"pc_player_collision",
         {"On: players and bots block each other, as in the original. Off: their bodies pass through each other and "
          "nobody can stand on another player.",
          "Unchanged either way: shots, the knife, grenades and their blasts, hitboxes and damage; walls, doors and "
          "hostages still block.",
          "Set for the whole match from its start. It applies where this PC runs the server (Custom Game, Local Only "
          "and LAN matches) and is sent to the players who join; another server keeps its own."}},
    };
    for (const Extra& e : extras)
        if (key == e.key) return &e;
    return nullptr;
}

// a number typed for a row: its range's text, and the value as saved ("" when it is not one of the range)
bool Numeric(const std::string& key) {
    const Extra* e = ExtraOf(key);
    return e && e->hi > e->lo;
}
std::string RangeText(const Extra& e) {
    char b[64];
    std::snprintf(b, sizeof b, "%g to %g", e.lo, e.hi);
    return b;
}
std::string NumberValue(const std::string& key, const std::string& typed) {
    const Extra* e = ExtraOf(key);
    if (!e || !(e->hi > e->lo) || typed.empty() || typed.size() > 8) return {};
    size_t dots = 0, digits = 0;
    for (char c : typed) {
        if (c == '.') dots++;
        else if (c >= '0' && c <= '9') digits++;
        else return {};
    }
    if (!digits || dots > (e->integer ? 0u : 1u)) return {};
    double v = std::strtod(typed.c_str(), nullptr);
    if (!(v >= e->lo && v <= e->hi)) return {};
    char b[32];
    std::snprintf(b, sizeof b, e->integer ? "%.0f" : "%.3f", v);
    std::string s = b;  // without a decimal part's trailing zeros
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

constexpr char kCustomLabel[] = "Your own value.";  // never shown: the arrows pass it until a number is typed

// a row's list length in the game: its choices and, for a number, the typed value's
size_t Listed(const Setting& r) { return r.choices.size() + (Numeric(r.key) ? 1 : 0); }

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
    std::vector<Choice> c = {{"Native", "native"}};  // (the display's size is in its Details: a long label overran its box)
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
    if (!listed && std::sscanf(saved.c_str(), "%ux%u", &w, &h) == 2) c.push_back({saved + "*", saved});  // * not listed
    c.push_back({"Original (480)", "original"});
    return c;
}

// Output Device's choices (N5E, src/linux/sdl_audio.cpp): the system's default, the playback devices SDL listed at
// the start (CSX_AUDIO_DEVICES), and a device saved that is not connected now
std::vector<Choice> AudioDevices() {
    std::vector<Choice> c = {{"System Default", ""}};
    std::string names = std::getenv("CSX_AUDIO_DEVICES") ? std::getenv("CSX_AUDIO_DEVICES") : "";
    for (size_t at = 0; at < names.size();) {
        size_t end = names.find('|', at);
        std::string n = names.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!n.empty()) c.push_back({n, n});
        at = end == std::string::npos ? names.size() : end + 1;
    }
    std::string saved = pc::Get("audio_device", "");
    bool listed = false;
    for (const Choice& x : c) listed = listed || x.value == saved;
    if (!listed) c.push_back({saved + " (not connected)", saved});
    return c;
}

const std::vector<PcScreen>& Screens() {
    static const std::vector<PcScreen> s = [] {
        std::vector<PcScreen> v;
        // PC Options (N5A): a hub of the title's style with a screen a group; the settings' keys as before
        v.push_back({"Video",
                     "Video",
                     {"Set the display mode, resolution, shape and", "frame rate."},
                     {
                         {"display_mode",
                          "Display Mode",
                          {"Play in a window or on the full screen.", "F11 also switches between them."},
                          {{"Windowed", "windowed"}, {"Fullscreen", "fullscreen"}},
                          0},
                         {"resolution",
                          "Resolution",
                          {"Set the size the game is drawn at; Native matches",
                           "the screen. The menus keep their layout."},
                          Resolutions(),
                          0},
                         {"aspect",
                          "Aspect Ratio",
                          {"Set the screen's shape; Automatic follows the display.",
                           "Takes effect the next time the game starts."},
                          {{"Automatic", "auto"}, {"4:3 (Original)", "4:3"}, {"16:10", "16:10"}, {"16:9", "16:9"},
                           {"21:9", "21:9"}},
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
                     },
                     true,
                     false,
                     true});
        v.back().panel = true;  // Video
        v.push_back({"Graphics",
                     "Advanced Graphics",
                     {"Set the field of view and the original's", "graphical effects."},
                     {
                         {"fov",
                          "Field of View",
                          {"Set how wide the view is. 90 is the game's original;",
                           "zoomed weapons keep their own."},
                          {{"90 (Original)", "90"}, {"95", "95"}, {"100", "100"}, {"105", "105"}, {"110", "110"},
                           {"115", "115"}, {"120", "120"}},
                          0},
                         {"xbox_effects",
                          "Xbox Visual Effects",
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
                         // N5E (src/game/pc_viewmodel.cpp)
                         {"viewmodel_fov",
                          "Weapon Field of View",
                          {"Set how wide the weapon in your hands is drawn.",
                           "Same draws it as the world, as the original does."},
                          {{"Same as FOV", "0"}, {"60", "60"}, {"65", "65"}, {"70", "70"}, {"75", "75"},
                           {"80", "80"}, {"85", "85"}, {"90", "90"}, {"95", "95"}, {"100", "100"}, {"110", "110"}},
                          0},
                     },
                     true,
                     false,
                     true});
        v.back().panel = true;  // Graphics
        v.push_back({"Mouse",
                     "Mouse",
                     {"Set the mouse's sensitivity and how it", "moves the view."},
                     {
                         {"mouse_sensitivity",
                          "Mouse Sensitivity",
                          {"Set how far the view turns when you move the mouse.",
                           "3 matches the game's original setting."},
                          Numbers({"0.5", "1", "1.5", "2", "2.5", "3", "3.5", "4", "5", "6", "7", "8", "10"}),
                          5},
                         // N5C (src/game/mouse_look.cpp ZoomScale)
                         {"mouse_zoom_sensitivity",
                          "Scoped Sensitivity",
                          {"How fast the view turns through a scope, as a share",
                           "of Mouse Sensitivity. 1 turns as without a scope."},
                          Numbers({"0.25", "0.5", "0.75", "1", "1.25", "1.5", "2"}),
                          3},
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
                     },
                     true,
                     false,
                     true});
        // N5D (src/game/pc_prompts.cpp, linux/pad_style.h)
        v.push_back({"Controller",
                     "Controller",
                     {"Set which buttons the menus show, and the", "controller's flashlight and spray."},
                     {
                         {"prompt_style",
                          "Button Prompts",
                          {"Show the buttons of the controller in use, or of", "one you choose."},
                          {{"Automatic", "auto"}, {"Keyboard/Mouse", "pc"}, {"Xbox (Original)", "xbox"},
                           {"Xbox 360", "x360"}, {"Xbox Series X|S", "series"}, {"PlayStation 4", "ps"},
                           {"DualSense", "dualsense"}, {"Nintendo Switch", "switch"}, {"Steam Controller", "steam"},
                           {"Wii Remote", "wii"}},
                          0},
                         // N7 (src/linux/window_input.cpp Poll): actions the original's controller has no
                         // button for, on a chord or a newer controller's extra button
                         {"pad_flashlight",
                          "Flashlight",
                          {"The flashlight's button. Back with the D-pad keeps",
                           "every original button's action."},
                          PadExtraChoices(),
                          1},
                         {"pad_spray",
                          "Spray Logo",
                          {"The spray's button. Back with the D-pad keeps",
                           "every original button's action."},
                          PadExtraChoices(),
                          2},
                     },
                     true,
                     false,
                     true});
        v.push_back({"Audio",
                     "Audio",
                     {"Set the volume of the game's sound.", ""},
                     {
                         {"master_volume",
                          "Master Volume",
                          {"Set the volume of everything the game plays.", "Options > Sound sets its parts."},
                          Numbers({"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"}),
                          10},
                         // N5E (src/linux/sdl_audio.cpp)
                         {"audio_device",
                          "Output Device",
                          {"Choose where the game's sound plays.", "System Default follows the system."},
                          AudioDevices(),
                          0},
                         {"audio_background",
                          "Audio in the Background",
                          {"On keeps the sound playing while another window", "is in front. Off silences it then."},
                          {{"On", "1"}, {"Off", "0"}},
                          0},
                     },
                     true,
                     false,
                     true});
        v.push_back({"Gameplay",
                     "Gameplay",
                     {"Set how some of the game's effects look", "on this PC."},
                     {
                         // N7 (src/game/pc_gameplay.cpp)
                         {"flashbang_colour",
                          "Flashbang Effect",
                          {"White is the original's blinding flash. Black blinds",
                           "as long, with a dark screen instead."},
                          {{"White", "white"}, {"Black", "black"}},
                          0},
                         {"death_camera",
                          "Death Camera",
                          {"Original shows your body from behind. First Person",
                           "keeps your own eyes as you fall, then the original's."},
                          {{"Original", "original"}, {"First Person", "first_person"}},
                          0},
                     },
                     true,
                     false,
                     true});
        v.push_back({"Hud",
                     "HUD and Menus",
                     {"Set how the menus and the game's displays", "behave."},
                     {
                         // N5A (src/game/pc_scroll.cpp)
                         {"menu_smooth_scroll",
                          "Smooth Menu Scrolling",
                          {"Let long lists glide to the next rows. Off moves",
                           "them a whole row at once, as the original does."},
                          {{"On", "1"}, {"Off", "0"}},
                          0},
                         // N5E (src/linux/window_input.cpp Poll)
                         // N5E (src/game/pc_hud.cpp)
                         {"net_graph_mode",
                          "Net Graph",
                          {"Show the network graph in the bottom right corner:",
                           "always, with the scoreboard, or not."},
                          {{"Off", "off"}, {"Always On", "always"}, {"With Scoreboard", "scoreboard"}},
                          0},
                         {"net_graph_level",
                          "Net Graph Detail",
                          {"How much the network graph shows.", ""},
                          {{"Graph", "1"}, {"Graph and Details", "2"}, {"Numbers", "3"}},
                          0},
                         // N10 (src/game/pc_hud_layout.cpp): F7 in a match edits each element's place
                         {"hud_scale",
                          "HUD Scale",
                          {"The size of the HUD's displays (F7 in a match moves",
                           "and sizes each one)."},
                          {{"75%", "75"}, {"90%", "90"}, {"100%", "100"}, {"115%", "115"}, {"130%", "130"}, {"150%", "150"}},
                          2},
                         {"hud_scale_split",
                          "HUD Scale (Split Screen)",
                          {"The HUD's size in a split-screen player's view.", ""},
                          {{"75%", "75"}, {"90%", "90"}, {"100%", "100"}, {"115%", "115"}, {"130%", "130"}, {"150%", "150"}},
                          2},
                         {"scoreboard_mode",
                          "Scoreboard",
                          {"Hold shows the scoreboard while its button is held;",
                           "Toggle shows it from one press to the next."},
                          {{"Hold", "hold"}, {"Toggle", "toggle"}},
                          0},
                     },
                     true,
                     false,
                     true});
        v.push_back({"Multiplayer",
                     "Multiplayer",
                     {"Set where matches run and how much this PC", "sends and receives online."},
                     {
                         // N4.7 (src/game/local_server.cpp): a match from the menus in csx-server, or in the game
                         {"server_process",
                          "Match Server",
                          {"Separate runs a match's server (bots, rules) in its",
                           "own process: smoother play. In Game is the original."},
                          {{"Separate", "1"}, {"In Game", "0"}},
                          0},
                         // N1 (src/game/net_engine.cpp: rate, cl_updaterate, cl_cmdrate when joining a server)
                         {"net_update_rate",
                          "Updates per Second",
                          {"How many updates a second to ask a server for.",
                           "Takes effect when you next join a server."},
                          Numbers({"20", "30", "60"}),
                          1},
                         {"net_cmd_rate",
                          "Commands per Second",
                          {"How many times a second your moves are sent.",
                           "Takes effect when you next join a server."},
                          Numbers({"30", "60", "100"}),
                          1},
                         {"net_rate",
                          "Bandwidth",
                          {"The most bytes a second a server may send you.",
                           "Takes effect when you next join a server."},
                          {{"25 KB/s", "25000"}, {"50 KB/s", "50000"}, {"100 KB/s", "100000"}, {"200 KB/s", "200000"},
                           {"500 KB/s", "500000"}},
                          2},
                     },
                     true,
                     false,
                     true});
        // N8 (src/common/mods.h): the installed mods, read once; changes take effect at the next start
        {
            PcScreen mods{"Mods", "Mods", {"Turn installed mods on or off. Changes take", "effect when the game next starts."}, {},
                          true, false, true};
            mods.settings.push_back({"mods_original", "Looks",
                                     {"Mods uses the mods turned on below. Original", "Xbox turns every mod off at once."},
                                     {{"Mods", "0"}, {"Original Xbox", "1"}},
                                     0});
            for (const csx::mods::Mod& m : csx::mods::Scan()) {
                std::string help1 = m.problems.empty() ? m.description.substr(0, 50)
                                                       : "Cannot load: " + m.problems[0].substr(0, 36);
                std::string help2 = std::string(m.kind == "content" ? "Content: every player in a match needs it."
                                                                    : "Cosmetic: changes only this PC's looks.") +
                                    (m.author.empty() ? "" : " By " + m.author.substr(0, 20) + ".");
                mods.settings.push_back({"mod." + m.id, m.name + " " + m.version, {help1, help2},
                                         {{"Off", "0"}, {"On", "1"}}, 0});
                mods.settings.push_back({"mod_order." + m.id, "  Load Order",
                                         {"Where its files go among the other mods':", "a later mod's file wins."},
                                         {{"First", "10"}, {"Early", "30"}, {"Normal", "50"}, {"Late", "70"}, {"Last", "90"}},
                                         2});
            }
            v.push_back(mods);
        }
        v.push_back({"Options",
                     "PC Options",
                     {"Set mouse and other options for playing", "on a PC."},
                     {},
                     false,
                     false,
                     false,
                     nullptr,
                     nullptr,
                     0,
                     nullptr,
                     nullptr,
                     {"Video", "Graphics", "Mouse", "Controls", "Controller", "Audio", "Gameplay", "Hud", "Multiplayer",
                      "Mods"}});
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
                            {{"Keyboard/Mouse", "0"}, {"Also a Controller", "1"}},
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
        for (const char* key : {"mouse_sensitivity", "mouse_zoom_sensitivity", "mouse_invert", "mouse_raw", "fov", "prompt_style"})
            for (const PcScreen& from : v)  // PC Options' screens (N5A: Mouse, Graphics)
                for (const Setting& r : from.settings)
                    if (r.key == key && !from.external) profile.settings.push_back(r);
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
            {"pc_player_collision", "Player Collision",
             {"On: players and bots block each other. Off: their", "bodies pass through; shots and grenades still hit."},
             list({{"On", "1"}, {"Off", "0"}}), 0},
            {"allow_spectators", "Spectators", {"Dead players may join the spectators.", ""}, off_on, 0},
            {"pc_gamemode", "Game Mode",
             {"Classic: rounds. Deathmatch: respawn, no rounds.", "Gun Game: a new weapon for each kill, the knife last."},
             list({{"Classic", "classic"}, {"Deathmatch", "deathmatch"}, {"Gun Game", "gungame"}}), 0},
            {"pc_fraglimit", "Frag Limit", {"Deathmatch: the match ends at this many frags.", ""},
             list({{"No Limit", "0"}, {"10", "10"}, {"20", "20"}, {"30", "30"}, {"50", "50"}, {"100", "100"}}), 0},
            {"mp_flashlight", "Flashlights", {"Players may use their flashlights.", ""}, off_on, 1},
            {"pc_gravity", "Gravity", {"How strongly players fall (800 the original's).", ""},
             list({{"Very Low", "200"}, {"Low", "500"}, {"Normal", "800"}, {"High", "1200"}}), 2},
        };
        v.push_back(rules);
        return v;
    }();
    return s;
}

std::string FrontEndName(const PcScreen& s) { return std::string("SuperLauncher_PC") + s.id; }
std::string InGameName(const PcScreen& s) { return std::string("InGame_PC") + s.id; }

// the screen of either menu by its name
const PcScreen* FindScreenById(const std::string& id) {
    for (const PcScreen& s : Screens())
        if (id == s.id) return &s;
    return nullptr;
}

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
        // a number's own typed value (N5B): one more choice, passed over by the arrows until a value is typed; its
        // label (15 characters: the game keeps it in a heap block with room for "16000 (custom)") is written then
        if (Numeric(r.key)) t += "\t\t\t\tListOption " + Quote(kCustomLabel) + "\t\t\"custom\"\r\n";
        t += "\t\t\t}\r\n\t\t}\r\n";
    }
    return t;
}

// a row's Details (N5B): Y, or BACK where Y opens another screen (the match rules' Rulesets)
bool DetailsOnBack(const std::string& name, const PcScreen& s) { return s.on_y && name.rfind("SuperLauncher_", 0) == 0; }
std::string DetailsButton(const std::string& name, const PcScreen& s) {
    return std::string("\tOnButtonDown") + (DetailsOnBack(name, s) ? "Back" : "Y") + " \"!pc_details " + name + "\"\r\n";
}

std::string Commands(const std::string& name, const PcScreen& s) {
    if (s.external)  // a profile's settings (pc_party.cpp), the match rules (pc_lobby.cpp)
        return "\tOnEntry \"!" + std::string(s.external) + "_load\"\r\n\tOnButtonDownB \"back\"\r\n"
               "\tOnButtonDownA \"!" + std::string(s.external) + "_save; xbui back\"\r\n\tOnButtonDownX \"!pc_options_defaults " + name +
               "\"\r\n" + (s.on_y ? "\tOnButtonDownY \"" + std::string(s.on_y) + "\"\r\n" : std::string()) + DetailsButton(name, s);
    std::string entry = s.on_entry ? std::string("; ") + s.on_entry : "", accept = s.on_accept ? std::string("; ") + s.on_accept : "";
    std::string t = "\tOnEntry \"!pc_options_load " + name + entry + "\"\r\n\tOnButtonDownB \"back\"\r\n"
                    "\tOnButtonDownA \"!pc_options_save " + name + accept + "; xbui back\"\r\n";
    if (s.resettable) t += "\tOnButtonDownX \"!pc_options_defaults " + name + "\"\r\n";
    if (s.on_y && name.rfind("SuperLauncher_", 0) == 0) t += "\tOnButtonDownY \"" + std::string(s.on_y) + "\"\r\n";
    return t + DetailsButton(name, s);
}

// a screen's status lines (BackgroundText, set by its commands): under its list, above the help lines
std::string StatusLines(const PcScreen& s) {
    std::string t;
    for (int i = 0; i < s.status_lines; i++)
        t += "\tBackgroundText \"54 83 107 255 80 " + std::to_string(250 + 20 * i) + " 480 30 0.75\"\t\" \"\r\n";
    return t;
}

// a panel's lines (N5B): BackgroundText at the list's left, after the title and the hint (filled by HintFrame), its
// text wrapped to the lines' width (kPanelUnits, Units: the font's widths, measured on the screens)
constexpr int kPanelLines = 13;
constexpr float kPanelUnits = 17.5f;  // 160 wide at 0.6: about 14.3 x 0.6 a unit
std::string PanelLines(const char* colour) {
    std::string t;
    for (int i = 0; i < kPanelLines; i++)
        t += std::string("\tBackgroundText \"") + colour + " 62 " + std::to_string(102 + 17 * i) + " 160 17 0.6\"\t\" \"\r\n";
    return t;
}

// the list's columns: the label, then the value with its arrows (2026-10-30: wider value columns; a 60/40 split cut
// values such as "Native (1920x1080)" and the panel's labels)
std::string Columns(const PcScreen& s) {
    const char* label = s.panel ? "55" : "52";
    const char* value = s.panel ? "45" : "48";
    return std::string("\t\t\tColumnWidthPercentage \"") + label + "\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
           "\t\t\tColumnWidthPercentage \"" + value + "\"\r\n\t\t\tColumnJustify \"center\"\r\n";
}

// the list's table: the whole width, or beside a panel (narrower, a smaller text, its arrows moved with its value
// column); the arrows' image spans the value column (Columns)
std::string TableLayout(const PcScreen& s, bool front_end) {
    if (!s.panel)
        return front_end ? std::string("\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
                                       "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"500 ") + (s.status_lines ? "140" : "250") + "\"\r\n"
                                       "\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
                                       "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 262.0 2.0 236.0 22.0\"\r\n"
                                       "\t\t\tNonHighlightImage \"Arrows_Unselected 1.0 1.0 1.0 1.0 262.0 2.0 236.0 22.0\"\r\n"
                         : "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
                           "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"475 300\"\r\n"
                           "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 249.0 2.0 224.0 22.0\"\r\n";
    std::string t = "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.8\"\r\n\t\t\tTableLocation \"232 100\"\r\n"
                    "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"384 250\"\r\n";
    if (front_end)
        return t + "\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
                   "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 213.0 2.0 169.0 22.0\"\r\n"
                   "\t\t\tNonHighlightImage \"Arrows_Unselected 1.0 1.0 1.0 1.0 213.0 2.0 169.0 22.0\"\r\n";
    return t + "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 213.0 2.0 169.0 22.0\"\r\n";
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
           "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t" + Quote(s.title) + "\r\n" +
           "\tBackgroundText \"54 83 107 255 72 423 250 20 0.7\"\t\" \"\r\n" +  // the hint (N5B: HintFrame)
           (s.panel ? PanelLines("54 83 107 255") : std::string()) + Pie() +
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           + Columns(s) + TableLayout(s, true) +
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
           "\tBackgroundText \"177 169 54 255 75 412 500 20 0.7\"\t\" \"\r\n" +  // the hint (N5B: HintFrame)
           (s.panel ? PanelLines("255 255 255 255") : std::string()) +
           Commands(name, s) + Pie() +
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           + Columns(s) + TableLayout(s, false) +
           "\t\t}\r\n" + ListRows(s, false) + "\t}\r\n}\r\n";
}

// a hub row's name: the screen's title (PC Controls is the Keyboard category there, N5B)
std::string HubLabel(const PcScreen& c) { return std::string(c.id) == "Controls" ? "Keyboard" : c.title; }

// a hub (N5A: PC Options): the title's own Options screens, front end and pause menu, with a row opening each of its
// screens (the front end's rows carry their help lines)
std::string HubFrontEnd(const PcScreen& s) {
    std::string rows;
    for (const char* id : s.opens)
        if (const PcScreen* c = FindScreenById(id)) {
            std::string help[2] = {c->row_help[0], c->row_help[1]};
            rows += "\t\tRow\r\n\t\t{\r\n" + HelpTable(help, 2, "\t\t\t") + "\t\t\tCommandSelect \"open " +
                    FrontEndName(*c) + "\"\r\n\t\t\tCellFixed " + Quote(HubLabel(*c)) + "\r\n\t\t}\r\n";
        }
    return "\r\n" + FrontEndName(s) + "\r\n{\r\n"
           "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n"
           "\tHighlightSound \"menu/buy_hover.wav\"\r\n\tPieceOffsetDegrees \"90\"\r\n"
           "\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n\tCommandSelect \"RowSelect\"\r\n"
           "\tBackgroundImage \"Back_MainScreen\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
           "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n"
           "\tOnButtonDownB \"back\"\r\n"
           "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t" + Quote(s.title) + "\r\n"
           "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n\t}\r\n"
           "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n\t}\r\n"
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           "\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\tTableVerticalWrap\r\n\t\t\tTableLocation \"80 98\"\r\n"
           "\t\t\tTableSize \"236 252\"\r\n\t\t\tTableRowSpacing \"12\"\r\n\t\t\tNonSelectBG \"0 0 0 0\"\r\n"
           "\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
           "\t\t\tHighlightImage \"MainMenu_Highlight 1.0 1.0 1.0 1.0 -40.0 -1.0 240.0 38.0\"\r\n"
           "\t\t}\r\n" + rows + "\t}\r\n}\r\n";
}

std::string HubInGame(const PcScreen& s) {
    std::string rows;
    for (const char* id : s.opens)
        if (const PcScreen* c = FindScreenById(id))
            rows += "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect   \"open " + InGameName(*c) +
                    "\"\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed " + Quote(HubLabel(*c)) + "\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n";
    return "\r\n" + InGameName(s) + "\r\n{\r\n"
           "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_enter.wav\"\r\n"
           "\tSelectSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
           "\tPieceOffsetDegrees \"90\"\r\n\tCachedImage \"angleSelect\t\t\t\t255 255 255 255\"\r\n"
           "\tCommandSelect \"RowSelect\"\r\n"
           "\tBackgroundImage \"csxbackground\t\t100 100 80 235\t\t0 0 640 480\"\r\n\tModal \"50\"\r\n"
           "\tOnButtonDownA \" \"\r\n\tOnButtonDownStart \"back\"\r\n\tOnButtonDownB \"back\"\r\n"
           "\tBackgroundText \"177 169 54 255 65 45 400 30\"\t" + Quote(s.title) + "\r\n"
           "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n\t}\r\n"
           "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n\t}\r\n"
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           "\t\t\tColumnWidthPercentage \"10\"\r\n\t\t\tColumnJustify \"center\"\r\n"
           "\t\t\tColumnWidthPercentage \"80\"\r\n\t\t\tColumnJustify \"center\"\r\n"
           "\t\t\tColumnWidthPercentage \"10\"\r\n\t\t\tColumnJustify \"center\"\r\n"
           "\t\t\tTableVerticalWrap\r\n\t\t\tTableLocation \"0 150\"\r\n\t\t\tTableSize \"640 300\"\r\n"
           "\t\t\tNonSelectBG \"0 0 0 0\"\r\n\t\t\tSelectBG \"0 0 100 40\"\r\n"
           "\t\t\tHighlightImage \"Secondary_Highlight 1.0 1.0 1.0 1.0 205.0 -1.0 230.0 34.0\"\r\n"
           "\t\t}\r\n\t\tHeader\r\n\t\t{\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n" +
           rows + "\t}\r\n}\r\n";
}

// a screen a hub opens (shown only from there)
bool HubChild(const PcScreen& s) {
    for (const PcScreen& h : Screens())
        for (const char* id : h.opens)
            if (std::string(id) == s.id) return true;
    return false;
}

// a row's Details (N5B): a screen of text lines in the menu's style (the front end's message screens, the pause menu's
// overlay), filled on entry for the row Y (BACK) was pressed on; A or B goes back to its screen as it was
constexpr char kDetailsFrontEnd[] = "SuperLauncher_PCDetails", kDetailsInGame[] = "InGame_PCDetails";
constexpr int kDetailLines = 13;

std::string DetailsScreen(bool front_end) {
    std::string t = std::string("\r\n") + (front_end ? kDetailsFrontEnd : kDetailsInGame) + "\r\n{\r\n";
    if (front_end)
        t += "\tmenutype \"generic\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tModal \"200\"\r\n"
             "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
             "\tBackgroundImage \"Back 200 200 200 255 0 411 640 69\"\r\n";
    else
        t += "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_enter.wav\"\r\n"
             "\tSelectSound \"menu/buy_submenu.wav\"\r\n\tPieceOffsetDegrees \"0\"\r\n"
             "\tBackgroundImage \"csxbackground\t\t100 100 80 235\t\t0 0 640 480\"\r\n\tModal \"50\"\r\n";
    t += "\tOnEntry \"!pc_details show\"\r\n\tOnButtonDownA \"back\"\r\n\tOnButtonDownB \"back\"\r\n";
    const char* title = front_end ? "54 83 107 255" : "177 169 54 255";
    const char* body = front_end ? "54 83 107 255" : "255 255 255 255";
    t += std::string("\tBackgroundText \"") + title + " 65 45 500 30\"\t\"Details\"\r\n";
    for (int i = 0; i < kDetailLines; i++)
        t += std::string("\tBackgroundText \"") + body + " 75 " + std::to_string(92 + 22 * i) + " 500 22 0.75\"\t\" \"\r\n";
    if (!front_end) t += std::string("\tBackgroundText \"") + title + " 75 412 500 20 0.7\"\t\"B Back\"\r\n";
    return t + "}\r\n";
}

// Custom Game > Player Collision (src/game/pc_collision.cpp): a last row in the single player match's Custom Options,
// written as its rows are. The screen's choices are the match's command line (0xE2270 joins the table's values when
// Match Options is Custom), so the row's "+pc_player_collision" reaches the match before anyone spawns. It goes last:
// the screen's own code reaches its nine rows by position (0xE1DE0). The game keeps the screen's choices from one
// front end to the next (after a match the next match's command line still has them), this row's with them; Reset
// (X) gives On, the default, as for the other rows
std::string CollisionRow() {
    static const std::string help[2] = {"On: players and bots block each other. Off: their",
                                        "bodies pass through; shots and grenades still hit."};
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

// Custom Game > Game Mode (src/game/pc_modes.cpp, N6C-E) and Gravity (pc_gameplay.cpp, N7): last rows in the single
// player Match Options, after Match Options (the screen's code reaches its four rows by position), their choices on
// the match's command line as the other rows' ("+pc_gamemode ...", "+pc_gravity ..."), whatever Match Options is.
// The mode's choices also allow the flashlight (mp_flashlight 1: the game's cvar is 0, the title had no binding for it)
std::string ExtendGameMode(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_SinglePlayerMatchOptions\r\n");
    size_t next = screen == std::string::npos ? screen : text.find("\nSuperLauncher_", screen + 1);
    size_t end = next == std::string::npos ? next : text.rfind("\t}\r\n}\r\n", next);
    if (end == std::string::npos || end < screen || text.find("\"Match Options\"", screen) > end) {
        LOGW("pc menu: the single player Match Options not as expected, no Game Mode row");
        return text;
    }
    static const std::string help[2] = {"Classic: rounds. Deathmatch: respawn, no rounds.",
                                        "Gun Game: a new weapon for each kill, the knife last."};
    std::string row = "\t\tRow\r\n\t\t{\r\n" + HelpTable(help, 2, "\t\t\t") +
                      "\r\n\t\t\tCellFixed \"Game Mode\"\r\n\t\t\tCellList\r\n\t\t\t{\r\n"
                      "\t\t\t\tListDefault \"Classic\"\t\t\"+pc_gamemode classic +mp_flashlight 1\"\r\n"
                      "\t\t\t\tListOption \"Deathmatch\"\t\"+pc_gamemode deathmatch +mp_flashlight 1\"\r\n"
                      "\t\t\t\tListOption \"Gun Game\"\t\t\"+pc_gamemode gungame +mp_flashlight 1\"\r\n\t\t\t}\r\n\t\t}\r\n";
    // Gravity (src/game/pc_gameplay.cpp, N7): the server's sv_gravity
    static const std::string gravity_help[2] = {"How strongly players fall. Normal is the original's.",
                                                "Lower lets everyone jump higher and fall slower."};
    row += "\t\tRow\r\n\t\t{\r\n" + HelpTable(gravity_help, 2, "\t\t\t") +
           "\r\n\t\t\tCellFixed \"Gravity\"\r\n\t\t\tCellList\r\n\t\t\t{\r\n"
           "\t\t\t\tListOption \"Very Low\"\t\t\"+pc_gravity 200\"\r\n"
           "\t\t\t\tListOption \"Low\"\t\t\t\"+pc_gravity 500\"\r\n"
           "\t\t\t\tListDefault \"Normal\"\t\t\"+pc_gravity 800\"\r\n"
           "\t\t\t\tListOption \"High\"\t\t\t\"+pc_gravity 1200\"\r\n\t\t\t}\r\n\t\t}\r\n";
    return text.substr(0, end) + row + text.substr(end);
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
    return ExtendGameMode(out);
}

}  // namespace

// one of PC Options' screens (any PcScreen: the hub, the settings, PC Controls, Rules, a profile's, Filters), front end
// or in game (pc_browser.cpp: not a list of servers or party actions)
bool PcSettingsScreen(const std::string& name) { return FindScreen(name) != nullptr; }

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
        screens += s.opens.empty() ? FrontEndScreen(s) : HubFrontEnd(s);
    }
    screens += DetailsScreen(true);
    return text.substr(0, row) + rows + text.substr(row) + screens;
}

// the main menu's Exit Game (N5A): a last row (the title's art has room for a seventh, empty there), after Change
// Profile, written as the title's rows are, and its confirmation (A quits: csx::AppQuit, as N4.8's Quit Game; B
// back); the text unchanged when the main menu is not as expected
std::string ExtendExit(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_MainScreen\r\n");
    size_t end = screen == std::string::npos ? screen : text.find("\n}", screen + 1);
    size_t label = screen == std::string::npos ? screen : text.find("CellFixed \"Change Profile\"", screen);
    size_t row_end = label == std::string::npos || label > end ? std::string::npos : text.find("\r\n\t\t}\r\n", label);
    if (row_end == std::string::npos || row_end > end) {
        LOGW("pc menu: the main menu not as expected, no Exit Game row");
        return text;
    }
    row_end += 6;  // after the row's closing line
    const std::string help[2] = {"Close Counter-Strike.", ""};
    std::string row = "\t\tRow\r\n\t\t{\r\n" + HelpTable(help, 1, "\t\t\t") +
                      "\t\t\tCommandSelect \"open SuperLauncher_PCConfirmExit\"\r\n\t\t\tCellFixed \"Exit Game\"\r\n\t\t}\r\n";
    std::string confirm =
        "\r\nSuperLauncher_PCConfirmExit\r\n{\r\n"
        "\tmenutype \"generic\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tModal \"200\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Cancel_Accept 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnButtonDownA \"!pc_exit\"\r\n\tOnButtonDownB \"back\"\r\n"
        "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"Exit Game\"\r\n"
        "\tBackgroundText \"54 83 107 255 80 190 520 30 0.8\"\t\"Close Counter-Strike?\"\r\n"
        "\tBackgroundText \"54 83 107 255 80 216 520 30 0.8\"\t\"A closes the game, B goes back.\"\r\n}\r\n";
    return text.substr(0, row_end) + row + text.substr(row_end) + confirm;
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
        if (s.hidden) {
            if (HubChild(s)) screens += InGameScreen(s);  // PC Options' screens (N5A)
            continue;
        }
        rows += "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect   \"open " + InGameName(s) +
                "\"\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed " + Quote(s.title) +
                "\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n";
        screens += s.opens.empty() ? InGameScreen(s) : HubInGame(s);
    }
    screens += DetailsScreen(false);
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

// a number's typed value in its list (N5B): the choice after the list's own holds it, its label written in place (the
// game's UTF-16 block, never past its capacity: the game frees what it made), its enable byte (cell +0x29 + index:
// the arrows pass over a choice whose byte is 0) set only while it holds a value
std::map<uint32_t, std::string> g_custom;  // list cell -> the typed value its last choice holds

void EnableChoice(uint32_t cell, size_t index, bool on) {
    if (index < 0x27) *static_cast<uint8_t*>(kernel::HostPtr(cell + 0x29 + uint32_t(index))) = on ? 1 : 0;
}

void ClearCustom(uint32_t cell, const Setting& r) {
    if (!Numeric(r.key)) return;
    EnableChoice(cell, r.choices.size(), false);
    g_custom.erase(cell);
}

bool SetCustom(uint32_t cell, const Setting& r, const std::string& value) {
    size_t n = r.choices.size();
    uint32_t choice = R32(cell + 0xC) + uint32_t(n) * 0x1C, capacity = R32(choice + 0x18);
    std::string label = value + " (custom)";
    if (label.size() > capacity) label = value;
    if (!Numeric(r.key) || Choices(cell) != n + 1 || capacity < 8 || label.size() > capacity) {
        LOGW("pc menu: %s cannot show %s", r.label.c_str(), value.c_str());
        return false;
    }
    uint32_t text = R32(choice + 4);
    for (size_t i = 0; i <= label.size(); i++) {
        uint16_t c = i < label.size() ? static_cast<unsigned char>(label[i]) : 0;
        std::memcpy(static_cast<uint8_t*>(kernel::HostPtr(text)) + i * 2, &c, 2);
    }
    kernel::W32(choice + 0x14, uint32_t(label.size()));
    EnableChoice(cell, n, true);
    kernel::W32(cell, uint32_t(n));
    if (g_custom[cell] != value) LOGI("pc menu: %s shows %s (its own value)", r.label.c_str(), value.c_str());
    g_custom[cell] = value;
    return true;
}

// the value a row's list shows ("" for none)
std::string CellValue(uint32_t cell, const Setting& r) {
    uint32_t pick = R32(cell);
    if (pick < r.choices.size()) return r.choices[pick].value;
    auto it = g_custom.find(cell);
    return pick == r.choices.size() && it != g_custom.end() ? it->second : std::string();
}

// a row's list set to a value: its choice (for a number also one equal to it: "3.0" is "3"), a number of its range as
// the typed value, otherwise the row's default
void SetCell(uint32_t cell, const Setting& r, const std::string& value) {
    ClearCustom(cell, r);
    std::string number = NumberValue(r.key, value);
    int at = -1;
    for (size_t c = 0; c < r.choices.size() && at < 0; c++)
        if (r.choices[c].value == value || (!number.empty() && NumberValue(r.key, r.choices[c].value) == number)) at = int(c);
    if (at >= 0) kernel::W32(cell, uint32_t(at));
    else if (number.empty() || !SetCustom(cell, r, number)) kernel::W32(cell, uint32_t(r.def));
}

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
        if (!cell || Choices(cell) != Listed(s.settings[i])) {
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
        SetCell(cells[i], r, pc::Get(r.key, r.choices[size_t(r.def)].value));
    }
}

void DefaultScreen(const std::string& name, const PcScreen& s) {
    std::vector<uint32_t> cells = Cells(name, s);
    for (size_t i = 0; i < cells.size(); i++) {
        ClearCustom(cells[i], s.settings[i]);
        kernel::W32(cells[i], uint32_t(s.settings[i].def));
    }
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
        if (pick >= Listed(r)) continue;
        std::string value = CellValue(cells[i], r);
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
    static uint32_t buffer = 0;  // 64 characters (with its end): "Press a key", a number being typed, its range
    uint32_t n = std::min<uint32_t>(uint32_t(std::strlen(text)), 63);
    if (!buffer) buffer = kernel::CallGuest(ctx, kMalloc, {64 * 2});
    if (!buffer) return;
    for (uint32_t i = 0; i <= n; i++) {
        uint16_t c = i < n ? static_cast<unsigned char>(text[i]) : 0;
        std::memcpy(static_cast<uint8_t*>(kernel::HostPtr(buffer)) + i * 2, &c, 2);
    }
    kernel::W32(choice + 4, buffer);
    kernel::W32(choice + 0x14, n);
    kernel::W32(choice + 0x18, 63);  // the block's room: from 8 on the string is read through the pointer, shorter in place
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


// ---- Details and typed numbers (N5B) --------------------------------------------------------------------------------

// a text's width in the menu font's units (an average letter 1; measured on the screens: about 14.3 x the text scale
// a unit, as wide as the game draws before it cuts a cell's text)
float Units(const std::string& s) {
    float u = 0;
    for (char c : s) {
        if (c == ' ') u += 0.5f;
        else if (std::strchr("il.,:;!'|", c)) u += 0.45f;
        else if (std::strchr("fjrtI1()[]-", c)) u += 0.6f;
        else if (std::strchr("mwMW%", c)) u += 1.45f;
        else if (c >= 'A' && c <= 'Z') u += 1.15f;
        else if (c >= '0' && c <= '9') u += 0.9f;
        else u += 1.0f;
    }
    return u;
}

// text in lines of at most width units (the font's, Units), broken between words
std::vector<std::string> Wrap(const std::string& text, float width) {
    std::vector<std::string> out;
    std::string line;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find(' ', at);
        std::string word = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && Units(line + " " + word) > width) {
            out.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

bool InGameScreenName(const std::string& name) { return name.rfind("InGame_", 0) == 0; }

// what a row's Details shows: its help, its longer description, how to type a number
std::vector<std::string> DetailLines(const Setting& r, float width = 44) {
    std::vector<std::string> out;
    auto add = [&out, width](const std::string& p) {
        for (const std::string& l : Wrap(p, width)) out.push_back(l);
    };
    const Extra* e = ExtraOf(r.key);
    if (!e || e->details.empty()) add(r.help[0] + (r.help[1].empty() ? "" : " " + r.help[1]));
    else  // the longer description says what the help says
        for (const std::string& p : e->details) {
            if (!out.empty()) out.push_back("");
            add(p);
        }
    if (Numeric(r.key)) {
        out.push_back("");
        add("Your own value: type a number from " + RangeText(*e) +
            " while the row is highlighted, then Enter (Escape cancels). Accept saves it.");
    }
    if (width == 54 && out.size() > size_t(kDetailLines)) out.resize(size_t(kDetailLines));
    return out;
}

struct Details {
    bool pending = false, opened = false;  // its screen's rows to be put back once it is shown again
    std::string screen, label;
    std::vector<std::string> lines, values;  // the rows' values when Y was pressed (unsaved choices kept)
    uint32_t row = 0;
};
Details g_details;

void OpenDetails(const std::string& name) {
    const PcScreen* s = FindScreen(name);
    uint32_t table = s ? FindTable(name) : 0;
    std::vector<uint32_t> cells = table ? Cells(name, *s) : std::vector<uint32_t>();
    uint32_t row = table ? R32(table + 0x9C) : 0;
    if (row >= cells.size()) return;
    const Setting& r = s->settings[row];
    g_details = {true, false, name, r.label, DetailLines(r), {}, row};
    for (size_t i = 0; i < cells.size(); i++) g_details.values.push_back(CellValue(cells[i], s->settings[i]));
    LOGI("pc menu: details of %s (%s)", r.label.c_str(), name.c_str());
    csx::QueueEngineCommand(std::string("xbui open ") + (InGameScreenName(name) ? kDetailsInGame : kDetailsFrontEnd) + "\n");
}

void ShowDetails(X86Context& ctx) {
    std::string name = ScreenName();
    bool front_end = !InGameScreenName(name);
    float t[3] = {54, 83, 107}, b[3] = {54, 83, 107};
    if (!front_end) {
        t[0] = 177, t[1] = 169, t[2] = 54;
        b[0] = b[1] = b[2] = 255;
    }
    PcSetScreenLineColour(ctx, name.c_str(), 0, g_details.label, t[0], t[1], t[2], 255);
    for (int i = 0; i < kDetailLines; i++)
        PcSetScreenLineColour(ctx, name.c_str(), 1 + i, size_t(i) < g_details.lines.size() ? g_details.lines[size_t(i)] : "",
                              b[0], b[1], b[2], 255);
}

// back from Details: the screen's rows as they were (its entry loaded the saved values), the same row highlighted
void DetailsFrame(const std::string& name) {
    if (!g_details.pending) return;
    if (name == kDetailsFrontEnd || name == kDetailsInGame) {
        g_details.opened = true;
        return;
    }
    if (name == g_details.screen && g_details.opened) {
        const PcScreen* s = FindScreen(name);
        std::vector<uint32_t> cells = s ? Cells(name, *s) : std::vector<uint32_t>();
        for (size_t i = 0; i < cells.size() && cells.size() == g_details.values.size(); i++)
            SetCell(cells[i], s->settings[i], g_details.values[i]);
        if (uint32_t table = FindTable(name)) kernel::W32(table + 0x9C, g_details.row);
        LOGI("pc menu: back from details, %s as it was", name.c_str());
    }
    if (!name.empty() && (name != g_details.screen || g_details.opened)) g_details.pending = false;
}

// the hint under a PC screen's list (BackgroundText after its title): Details' button, and how to type a number on a
// number's row
struct Hint {
    std::string screen, text;
    uint32_t row = ~0u;
};
Hint g_hint;

void HintFrame(X86Context& ctx, const std::string& name, const PcScreen* s, uint32_t row) {
    if (!s || s->settings.empty() || !s->opens.empty()) return;
    std::string text = PcPromptName(DetailsOnBack(name, *s) ? "back" : "y") + " Details";
    if (row < s->settings.size() && Numeric(s->settings[row].key)) {
        // within the hint's box (250 wide at 0.7: about 24 units), left of the help bar's buttons
        const Extra* e = ExtraOf(s->settings[row].key);
        char range[48];
        std::snprintf(range, sizeof range, "%g-%g", e->lo, e->hi);
        // (left of the help bar's buttons: when it would not fit, the panel and the Details say how to type it)
        if (!s->panel && Units(text) + Units(range) + 4.5f <= 20) text += std::string("    Type ") + range;
    }
    if (g_hint.screen == name && g_hint.row == row && g_hint.text == text) return;
    g_hint = {name, text, row};
    bool in_game = InGameScreenName(name);
    int hint = in_game ? 1 : s->status_lines + 1;
    if (in_game) PcSetScreenLineColour(ctx, name.c_str(), hint, text, 177, 169, 54, 255);
    else PcSetScreenLineColour(ctx, name.c_str(), hint, text, 54, 83, 107, 255);
    if (!s->panel || row >= s->settings.size()) return;
    // the panel: the row's name, then its description (as its Details, in the narrower lines)
    const Setting& r = s->settings[row];
    std::vector<std::string> lines = {r.label, ""};
    for (const std::string& l : DetailLines(r, kPanelUnits)) lines.push_back(l);
    if (lines.size() > size_t(kPanelLines)) {  // more than the panel holds: the rest is in its Details
        lines.resize(size_t(kPanelLines) - 1);
        lines.push_back("(more in Details)");
    }
    float c = in_game ? 255 : 54, g = in_game ? 255 : 83, b = in_game ? 255 : 107;
    for (int i = 0; i < kPanelLines; i++)
        PcSetScreenLineColour(ctx, name.c_str(), hint + 1 + i, size_t(i) < lines.size() ? lines[size_t(i)] : "",
                              i == 0 && in_game ? 177 : c, i == 0 && in_game ? 169 : g, i == 0 && in_game ? 54 : b, 255);
}

// a number typed for the highlighted row: shown in its list (its shown choice's text swapped, as a key capture's),
// Enter keeps it (one of the list's choices or the row's own "(custom)" one), Escape cancels; a value outside the
// range is refused and the range shown there for two seconds
struct Typing {
    enum Mode { Off, On, Refused } mode = Off;
    std::string screen, text;
    uint32_t row = 0, cell = 0, shown = 0;
    uint8_t saved[0x1C] = {};
    std::chrono::steady_clock::time_point until{};
};
Typing g_typing;
std::atomic<bool> g_number_row{false}, g_number_typing{false};
std::deque<int> g_number_keys;  // under g_bind_lock

void EndTyping(const char* how) {
    if (g_typing.mode == Typing::Off) return;
    std::memcpy(kernel::HostPtr(g_typing.shown), g_typing.saved, sizeof g_typing.saved);
    g_typing.mode = Typing::Off;
    if (how) LOGI("pc menu: typing %s", how);
}

void StartTyping(const std::string& name, const PcScreen& s, uint32_t row) {
    EndTyping(nullptr);
    std::vector<uint32_t> cells = Cells(name, s);
    if (row >= cells.size()) return;
    uint32_t cell = cells[row], pick = R32(cell);
    if (pick >= Choices(cell)) return;
    g_typing.mode = Typing::On;
    g_typing.screen = name;
    g_typing.text.clear();
    g_typing.row = row;
    g_typing.cell = cell;
    g_typing.shown = R32(cell + 0xC) + pick * 0x1C;
    std::memcpy(g_typing.saved, kernel::HostPtr(g_typing.shown), sizeof g_typing.saved);
    LOGI("pc menu: typing a value for %s", s.settings[row].label.c_str());
}

void TypeKey(X86Context& ctx, const PcScreen& s, int c) {
    const Setting& r = s.settings[g_typing.row];
    if (c == csx::kNumberCancel) return EndTyping("cancelled");
    if (c == csx::kNumberEnter) {
        std::string value = NumberValue(r.key, g_typing.text);
        if (value.empty()) {
            LOGI("pc menu: %s: \"%s\" refused (%s)", r.label.c_str(), g_typing.text.c_str(), RangeText(*ExtraOf(r.key)).c_str());
            g_typing.mode = Typing::Refused;
            g_typing.until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            ShowChoice(ctx, g_typing.shown, ("Use " + RangeText(*ExtraOf(r.key))).c_str());
            return;
        }
        EndTyping(nullptr);
        SetCell(g_typing.cell, r, value);
        LOGI("pc menu: %s typed %s (saved with Accept)", r.label.c_str(), value.c_str());
        return;
    }
    if (c == csx::kNumberBackspace) {
        if (!g_typing.text.empty()) g_typing.text.pop_back();
    } else if (g_typing.text.size() < 8) {
        g_typing.text += char(c);
    }
    ShowChoice(ctx, g_typing.shown, (g_typing.text + "_").c_str());
}

void TypingFrame(X86Context& ctx, const std::string& name, const PcScreen* s, uint32_t row, std::deque<int>& typed) {
    bool console = csx::ConsoleOpen();
    bool numeric = s && !s->unique_values && !console && row < s->settings.size() && Numeric(s->settings[row].key);
    g_number_row = numeric;
    if (g_typing.mode != Typing::Off && (name != g_typing.screen || row != g_typing.row || console))
        EndTyping(g_typing.mode == Typing::On ? "ended (the row or the screen was left)" : nullptr);
    if (g_typing.mode == Typing::Refused && std::chrono::steady_clock::now() >= g_typing.until) EndTyping(nullptr);
    for (int c : typed) {
        if (g_typing.mode != Typing::On) {
            if (c <= 0 || !numeric) continue;
            StartTyping(name, *s, row);
            if (g_typing.mode != Typing::On) continue;
        }
        TypeKey(ctx, *s, c);
    }
    g_number_typing = g_typing.mode == Typing::On;
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
bool NumberRowSelected() { return g_number_row; }
bool NumberTyping() { return g_number_typing; }
void NumberInput(int c) {
    std::lock_guard<std::mutex> lock(g_bind_lock);
    g_number_keys.push_back(c);
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
        std::string value = CellValue(cells[i], s->settings[i]);
        if (R32(cells[i]) < Listed(s->settings[i])) out[s->settings[i].key] = value;
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
        SetCell(cells[i], r, it == values.end() ? r.choices[size_t(r.def)].value : it->second);
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

// a typed number's value as saved for a setting's key, "" when the key takes none or it is not one of its range (the
// match rules' checks, pc_lobby.cpp)
std::string PcNumberValue(const std::string& key, const std::string& value) { return NumberValue(key, value); }

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
void PcMapSearchFrame(X86Context& ctx);    // pc_mapsearch.cpp

void PcMenuFrame(X86Context& ctx) {
    PcBrowserMenuFrame(ctx);
    PcSplitMenuFrame(ctx);
    PcPartyMenuFrame(ctx);
    PcLobbyMenuFrame(ctx);
    PcMapSearchFrame(ctx);
    PcPromptsFrame(ctx);
    PcBuyFrame(ctx);
    PcHudFrame();
    bool start;
    std::deque<int> keys, typed;
    {
        std::lock_guard<std::mutex> lock(g_bind_lock);
        start = std::exchange(g_start, false);
        keys.swap(g_bind_keys);
        typed.swap(g_number_keys);
    }
    std::string name = ScreenName();
    if (g_capture.active && (name != g_capture.screen || csx::ConsoleOpen())) EndCapture("ended (screen left)");
    g_row_selected = PcBindingScreen(name) && !csx::ConsoleOpen();
    if (start && g_row_selected) StartCapture(ctx);
    for (int k : keys)
        if (g_capture.active) CaptureKey(k);
    // N5B: Details, the hint line, a typed number
    const PcScreen* s = FindScreen(name);
    uint32_t table = s && !s->settings.empty() ? FindTable(name) : 0;
    uint32_t row = table ? R32(table + 0x9C) : ~0u;
    DetailsFrame(name);
    HintFrame(ctx, name, table ? s : nullptr, row);
    TypingFrame(ctx, name, table ? s : nullptr, row, typed);
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
void PcGameModeCommand(const std::string& arg);         // pc_modes.cpp
void PcFragLimitCommand(const std::string& arg);
void PcGunGameOrderCommand(const std::string& arg);
void PcGravityCommand(const std::string& arg);  // pc_gameplay.cpp
void PcRotationCommand(const std::string& arg);  // pc_modes.cpp
void PcInviteCommand();                          // pc_invite.cpp
void PcHudEditCommand();                         // pc_hud_layout.cpp

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
    std::string out = name.rfind("FrontEndUI", 0) == 0 ? ExtendPrompts(ExtendExit(ExtendFrontEnd(ExtendMapSearch(ExtendLobby(ExtendParty(ExtendSplitMenu(ExtendMultiplayer(ExtendBotCount(ExtendCustomOptions(ExtendDlcMaps(text)))))))))))
                                                       : ExtendPrompts(ExtendInGame(ExtendDlcMaps(text)));
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
    // the title's own end of a system link session (the pause menu's Quit / Leave Match): a split player's choice
    // (N4.8.1); the title's command runs as before
    if (cmd == "SystemLinkClientEndSession") {
        csx::SplitLeaveChosen();
        return false;
    }
    if (cmd == "pc_exit") {  // the main menu's Exit Game, confirmed (N5A)
        LOGI("pc menu: Exit Game");
        csx::AppQuit();
        return true;
    }
    if (cmd == "csx_radio") {  // N5C (pc_radio.cpp): sent on only to a server that serves it
        return PcRadioClientCommand(R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string());
    }
    if (cmd == "pc_server_mods") {  // the server's signon (N8): its content mods, which this PC must have the same of
        std::string theirs = R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string("none");
        std::string mine = csx::mods::ContentSignature();
        if (mine.empty()) mine = "none";
        if (theirs == mine) {
            LOGI("mods: the server's content mods match this PC's (%s)", mine.c_str());
        } else {
            LOGW("mods: the server's content mods (%s) are not this PC's (%s): disconnected", theirs.c_str(),
                 mine.c_str());
            csx::QueueEngineCommand("disconnect\n");
        }
        return true;
    }
    if (cmd == "pc_server_features") {  // the server's signon (N5C, pc_radio.cpp)
        std::string args;
        for (uint32_t i = 1; i < R32(0x4253E8) && i < 8; i++) args += GuestString(R32(0x4251A8 + 4 * i)) + " ";
        PcServerFeatures(args);
        return true;
    }
    if (cmd == "buyitem" || !PcBuyAlias(cmd).empty()) {  // one item (N5C, pc_buy.cpp): CS 1.6's aliases too
        PcBuyItem(cmd == "buyitem" ? (R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string()) : PcBuyAlias(cmd));
        return true;
    }
    if (cmd == "autobuy" || cmd == "rebuy") {  // N5C (pc_buy.cpp)
        PcBuyCommand(cmd);
        return true;
    }
    if (cmd == "pc_mapsearch") {  // a map list's search keyboard (N5B, pc_mapsearch.cpp)
        PcMapSearchCommand(ctx, R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string());
        return true;
    }
    if (cmd == "pc_details") {  // Y (BACK) on a PC screen's row: its Details; "show" fills that screen (N5B)
        std::string arg = R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string();
        if (arg == "show") ShowDetails(ctx);
        else OpenDetails(arg);
        return true;
    }
    if (cmd == "pc_gamemode" || cmd == "pc_fraglimit" || cmd == "pc_gungame_order") {  // pc_modes.cpp (N6C-E)
        std::string arg = R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string();
        for (uint32_t i = 2; cmd == "pc_gungame_order" && i < R32(0x4253E8) && i < 40; i++)
            arg += " " + GuestString(R32(0x4251A8 + 4 * i));
        if (cmd == "pc_gamemode") PcGameModeCommand(arg);
        else if (cmd == "pc_fraglimit") PcFragLimitCommand(arg);
        else PcGunGameOrderCommand(arg);
        return true;
    }
    if (cmd == "admin" && csx::Dedicated() && R32(0x748648) == 1) {  // the dedicated server's console (N9: the GUI)
        std::string text = "admin";
        for (uint32_t i = 1; i < R32(0x4253E8) && i < 16; i++) text += " " + GuestString(R32(0x4251A8 + 4 * i));
        return csx::AdminCommand(ctx, 0, text);
    }
    if (cmd == "pc_hud_edit") {  // pc_hud_layout.cpp (N10)
        PcHudEditCommand();
        return true;
    }
    if (cmd == "pc_invite") {  // pc_invite.cpp (N9)
        PcInviteCommand();
        return true;
    }
    if (cmd == "pc_rotation") {  // pc_modes.cpp (N9)
        std::string arg;
        for (uint32_t i = 1; i < R32(0x4253E8) && i < 40; i++) arg += GuestString(R32(0x4251A8 + 4 * i)) + " ";
        PcRotationCommand(arg);
        return true;
    }
    if (cmd == "pc_gravity") {  // pc_gameplay.cpp (N7)
        PcGravityCommand(R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string());
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
