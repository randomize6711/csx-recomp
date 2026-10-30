// The party's way into a match (N4.8, docs/multiplayer_frontend.md): after the party screen (pc_party.cpp) player 1,
// the leader, picks where to play (SuperLauncher_PCPartyDestination: Local Only, Host LAN Match, Server Browser), a map
// (SuperLauncher_PCMatchMaps: the title's own map list, its previews and names, and the maps in the game folder it
// leaves out), a game mode the map has (SuperLauncher_PCMatchModes: Bomb Defusal, Hostage Rescue), the rules (Y: pc_menu
// .cpp's PcScreen "Rules", rulesets saved by name) and the match lobby (SuperLauncher_PCLobby_<map>: the map's preview,
// mode, rules, Local Only or LAN, every member with its team and readiness, the bots and slots). Every member is ready
// again in the lobby (A; up / down its team); player 1 starts. The match line is the lobby's own (map, bots, rules) and
// the title relaunches into it from the lobby: N4.7's local server takes it (RelaunchNotify), offline and private
// (127.0.0.1, a password only the party has) for Local Only, on the network (LAN discovery, never a master server) for
// Host LAN Match. Each member joins its team as it enters (CSX_PARTY_TEAM: the title's own team screen, its row's
// command).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "common/profiles.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "game/pc_party.h"

void PcSetScreenLine(X86Context& ctx, const char* screen, int index, const std::string& text);  // pc_browser.cpp
void PcSetScreenLineColour(X86Context& ctx, const char* screen, int index, const std::string& text, float r, float g,
                           float b, float a);
std::string PcCurrentScreen();
std::string PcKeyboardScreen(const char* name, const char* title, const char* command, const char* hint);
std::string PcEditText(const char* screen);
void PcClearEdit(X86Context& ctx, const char* screen, const std::string& text);
void PcTypeAgain();
std::map<std::string, std::string> PcScreenValues(const std::string& name);  // pc_menu.cpp
void PcSetScreenValues(const std::string& name, const std::map<std::string, std::string>& values);
std::map<std::string, std::string> PcScreenDefaults(const std::string& name);
std::map<std::string, std::vector<std::string>> PcScreenChoices(const std::string& name);
std::string PcNumberValue(const std::string& key, const std::string& value);  // pc_menu.cpp
int PcSelectedRow(const std::string& screen);
std::string PcProbeTable();                                       // pc_party.cpp
std::vector<std::string> PcProbeTake(const std::string& screen);

namespace fs = std::filesystem;

namespace {

constexpr char kDest[] = "SuperLauncher_PCPartyDestination";
constexpr char kMaps[] = "SuperLauncher_PCMatchMaps";
constexpr char kModes[] = "SuperLauncher_PCMatchModes";
constexpr char kRules[] = "SuperLauncher_PCRules";  // pc_menu.cpp's PcScreen "Rules"
constexpr char kSets[] = "SuperLauncher_PCMatchRulesets";
constexpr char kSetName[] = "SuperLauncher_PCMatchRulesetName";
constexpr char kSetDelete[] = "SuperLauncher_PCMatchRulesetDelete";
constexpr char kPassword[] = "SuperLauncher_PCMatchPassword";
constexpr char kLobbyPrefix[] = "SuperLauncher_PCLobby_";
constexpr int kSetRows = 10;

struct MapInfo {
    std::string label, value, kind, image;  // kind "bomb" or "hostage"
};
std::vector<MapInfo> g_maps;  // in the map screen's row order
std::string g_dest = "local";  // "local" or "lan"
int g_map = 0;
std::string g_mode = "bomb";
int g_mode_cursor = 0;
std::string g_message;        // the lobby's passing line
std::string g_lan_password;   // Host LAN Match's (in memory only: never saved or logged)
int g_set_cursor = 0, g_set_top = 0;
std::vector<profiles::Info> g_sets;
bool g_set_renaming = false;
bool g_team_done = false;     // this boot's team joined (CSX_PARTY_TEAM)

std::string Q(const std::string& s) { return "\"" + s + "\""; }

bool RuleKey(const std::string& k) {
    static const std::set<std::string> keys = [] {
        std::set<std::string> s;
        for (auto& [key, v] : PcScreenDefaults(kRules)) s.insert(key);
        return s;
    }();
    return keys.count(k) > 0;
}

profiles::Store Sets() { return profiles::Store(pc::ProfileFolder(), "rulesets", RuleKey); }

const char* ModeName(const std::string& kind) { return kind == "hostage" ? "Hostage Rescue" : "Bomb Defusal"; }

// ---- the rules --------------------------------------------------------------------------------------------------------
// the party's current rules: PC settings party.rule.<cvar> (the default where unset or neither one of the row's values
// nor a number of its range, typed: N5B)
std::map<std::string, std::string> Rules() {
    auto d = PcScreenDefaults(kRules);
    auto choices = PcScreenChoices(kRules);
    for (auto& [k, v] : d) {
        std::string set = pc::GetGlobal("party.rule." + k, "");
        const auto& ok = choices[k];
        if (!set.empty() && std::find(ok.begin(), ok.end(), set) != ok.end()) v = set;
        else if (!set.empty() && PcNumberValue(k, set) == set) v = set;
    }
    return d;
}

void SetRules(const std::map<std::string, std::string>& r, const std::string& ruleset) {
    for (auto& [k, v] : r) pc::Set("party.rule." + k, v);
    pc::Set("party.ruleset", ruleset);
    pc::Save();
}

std::string RulesetName() {
    auto now = Rules();
    std::string id = pc::GetGlobal("party.ruleset", "default");
    if (id != "default" && profiles::IdValid(id)) {
        std::string name = Sets().Name(id);
        if (!name.empty()) {
            auto saved = PcScreenDefaults(kRules);
            for (auto& [k, v] : Sets().Values(id)) saved[k] = v;
            if (saved == now) return name;
        }
    }
    return now == PcScreenDefaults(kRules) ? "Default Rules" : "Custom Rules";
}

std::string Label(const std::string& key, const std::string& value) {
    // the row's own choice label for a value (pc_menu.cpp knows them only by index: read from the defaults' screen)
    static const std::map<std::string, std::map<std::string, std::string>> labels = {
        {"bot_difficulty", {{"0", "Easy"}, {"1", "Normal"}, {"2", "Hard"}, {"3", "Expert"}}},
        {"bot_join_team", {{"any", "both teams"}, {"t", "Terrorists"}, {"ct", "Counter-Terrorists"}}},
        {"pc_gamemode", {{"classic", "Classic"}, {"deathmatch", "Deathmatch"}, {"gungame", "Gun Game"}}},
    };
    auto k = labels.find(key);
    if (k != labels.end())
        if (auto v = k->second.find(value); v != k->second.end()) return v->second;
    return value;
}

int MaxBots() { return 65 - std::max(1, PcPartyCount()); }

// ---- the maps -----------------------------------------------------------------------------------------------------------
// the title's own map list (Single Player's): its rows' names, map values, kinds and preview images, in its order; then
// every map archive in the game folder it leaves out (not the training maps), its kind by its name
std::vector<MapInfo> ReadMaps(const std::string& text) {
    std::vector<MapInfo> out;
    size_t screen = text.find("\nSuperLauncher_SinglePlayerMatchMapList\r\n");
    size_t end = screen == std::string::npos ? screen : text.find("\nSuperLauncher_", screen + 1);
    std::set<std::string> listed;
    for (size_t at = screen; at != std::string::npos && at < end;) {
        size_t kind = text.find("CellFixed \"", at);
        size_t img = kind == std::string::npos ? kind : text.find("HighlightImage \"", kind);
        size_t list = img == std::string::npos ? img : text.find("ListDefault \"", img);
        if (list == std::string::npos || list > end) break;
        auto quoted = [&](size_t from, size_t n) {
            size_t a = text.find('"', from + n - 1) + 1, b = text.find('"', a);
            return text.substr(a, b - a);
        };
        std::string k = quoted(kind, 11), image = quoted(img, 16), label = quoted(list, 13);
        size_t v1 = text.find('"', text.find('"', text.find('"', list) + 1) + 1) + 1;
        std::string value = text.substr(v1, text.find('"', v1) - v1);
        image = image.substr(0, image.find(' '));
        at = list + 1;
        if (value == "random" || value.empty()) continue;
        out.push_back({label, value, k == "Hostage Rescue" ? "hostage" : "bomb", image});
        listed.insert(value);
    }
    const char* game = std::getenv("GAME_DIR");
    std::vector<std::string> more;
    std::error_code ec;
    if (game)
        for (const auto& e : fs::directory_iterator(game, ec)) {
            std::string n = e.path().filename().string(), stem = e.path().stem().string();
            std::string ext = e.path().extension().string();
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".sxwad" || listed.count(stem) || stem.find("training") != std::string::npos) continue;
            if (stem.rfind("de_", 0) == 0 || stem.rfind("cs_", 0) == 0) more.push_back(stem);
        }
    std::sort(more.begin(), more.end());
    for (auto& m : more) {
        std::string label = m.substr(3);
        if (!label.empty()) label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
        out.push_back({label, m, m.rfind("cs_", 0) == 0 ? "hostage" : "bomb", ""});
    }
    return out;
}

// ---- the screens' text --------------------------------------------------------------------------------------------------
std::string Head(const char* name, const char* type = "superlauncher") {
    return std::string("\r\n") + name + "\r\n{\r\n\tMenuType " + Q(type) +
           "\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
           "\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n";
}

std::string Help(const std::string& line) {
    return "\t\t\tTable\r\n\t\t\t{\r\n\t\t\t\tLayout\r\n\t\t\t\t{\r\n\t\t\t\t\tColumnWidthPercentage \"100\"\r\n"
           "\t\t\t\t\tTableLocation \"75 361\"\r\n\t\t\t\t\tTableSize \"476 40\"\r\n\t\t\t\t\tTableRowSpacing \"1\"\r\n"
           "\t\t\t\t\tTableTextScale \"0.75\"\r\n\t\t\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n\t\t\t\t}\r\n"
           "\t\t\t\tRow\r\n\t\t\t\t{\r\n\t\t\t\t\tCellFixed " + Q(line) + "\r\n\t\t\t\t}\r\n\t\t\t}\r\n";
}

// a dummy table for the screens whose lines are drawn by these commands (the title's superlauncher screens have one)
std::string NoTable() {
    return "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\tTableLocation \"700 700\"\r\n"
           "\t\t\tTableSize \"10 10\"\r\n\t\t}\r\n\t\tRow\r\n\t\t{\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n\t}\r\n";
}


// where the party plays: the title's main menu look (its highlight, a help line a row)
std::string DestinationText() {
    std::string t = Head(kDest) + "\tPieceOffsetDegrees \"90\"\r\n\tCommandSelect \"RowSelect\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnButtonDownB \"back\"\r\n\tOnEntry \"!pc_lobby dest_show\"\r\n"
        "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"Multiplayer\"\r\n"
        "\tBackgroundText \"54 83 107 255 330 110 260 24 0.7\"\t\" \"\r\n"
        "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n\t}\r\n"
        "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n\t}\r\n"
        "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\tTableVerticalWrap\r\n"
        "\t\t\tTableLocation \"80 106\"\r\n\t\t\tTableSize \"236 200\"\r\n\t\t\tTableRowSpacing \"12\"\r\n"
        "\t\t\tNonSelectBG \"0 0 0 0\"\r\n\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
        "\t\t\tHighlightImage \"MainMenu_Highlight 1.0 1.0 1.0 1.0 -40.0 -1.0 240.0 38.0\"\r\n\t\t}\r\n";
    struct Row {
        const char *label, *help, *cmd;
    } rows[] = {
        {"Local Only", "Split screen on this PC: no one else can see or join.", "!pc_lobby dest local"},
        {"Host LAN Match", "Players on your network can find and join the match.", "!pc_lobby dest lan"},
        {"Server Browser", "Join a match on your network or the internet.", "!pc_lobby dest browser"},
    };
    for (auto& r : rows)
        t += "\t\tRow\r\n\t\t{\r\n" + Help(r.help) + "\t\t\tCommandSelect " + Q(r.cmd) + "\r\n\t\t\tCellFixed " + Q(r.label) +
             "\r\n\t\t}\r\n";
    return t + "\t}\r\n}\r\n";
}

// the maps: the title's own Map Selection screen (its background, previews, kinds), its rows from ReadMaps
std::string MapsText() {
    std::string t = Head(kMaps) + "\tPieceOffsetDegrees \"90\"\r\n"
        "\tBackgroundImage \"Back_Select_A_Map\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnButtonDownA \"!pc_lobby map\"\r\n\tOnButtonDownB \"back\"\r\n"
        "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"Map Selection\"\r\n"
        "\tBackgroundText \"61 74 84 255 80 370 476 40 0.75\"\t\"Select the map the party plays on.\"\r\n"
        "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n\t}\r\n"
        "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n\t}\r\n"
        "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\tColumnJustify \"center\"\r\n"
        "\t\t\tTableVerticalWrap\r\n\t\t\tTableRowSpacing \"12\"\r\n\t\t\tTableLocation \"75 135\"\r\n"
        "\t\t\tTableSize \"200 180\"\r\n\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
        "\t\t\tHighlightImage \"Secondary_Highlight 1.0 1.0 1.0 1.0 -7.0 -2.0 211.0 35.0\"\r\n\t\t}\r\n";
    for (const MapInfo& m : g_maps) {
        t += "\t\tRow\r\n\t\t{\r\n\t\t\tTable\r\n\t\t\t{\r\n\t\t\t\tLayout\r\n\t\t\t\t{\r\n"
             "\t\t\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\t\t\tColumnJustify \"center\"\r\n"
             "\t\t\t\t\tTableLocation \"325 290\"\r\n\t\t\t\t\tTableSize \"250 40\"\r\n"
             "\t\t\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n\t\t\t\t}\r\n\t\t\t\tRow\r\n\t\t\t\t{\r\n"
             "\t\t\t\t\tCellFixed " + Q(ModeName(m.kind)) + "\r\n\t\t\t\t}\r\n\t\t\t}\r\n";
        if (!m.image.empty()) t += "\t\t\tHighlightImage \"" + m.image + " 255 255 255 255 350 125 200 150\"\r\n";
        t += "\t\t\tCellList\r\n\t\t\t{\r\n\t\t\t\tListDefault " + Q(m.label) + "\t\t\t" + Q(m.value) + "\r\n\t\t\t}\r\n\t\t}\r\n";
    }
    return t + "\t}\r\n}\r\n";
}

// lines drawn by the commands (title, a list with a cursor, its hints): Game Mode, Rulesets
std::string ListText(const char* name, const char* title, int rows, const std::string& cmd, const char* image = nullptr) {
    std::string t = Head(name) +
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n"
        "\tBackgroundImage \"whitebox 255 255 255 46 72 88 496 270\"\r\n";
    if (image) t += std::string("\tBackgroundImage \"") + image + "\"\r\n";
    t += "\tOnEntry \"!" + cmd + " show\"\r\n\tOnButtonDownA \"!" + cmd + " a\"\r\n\tOnButtonDownB \"!" + cmd + " b\"\r\n"
         "\tOnButtonDownX \"!" + cmd + " x\"\r\n\tOnButtonDownY \"!" + cmd + " y\"\r\n\tOnButtonDownBack \"!" + cmd + " delete\"\r\n"
         "\tOnButtonDownStart \"!" + cmd + " start\"\r\n"
         "\tBackgroundText \"54 83 107 255 65 45 500 30\"\t" + Q(title) + "\r\n";
    for (int r = 0; r < rows; r++)
        t += "\tBackgroundText \"54 83 107 255 90 " + std::to_string(96 + r * 23) + " 460 24 0.8\"\t\" \"\r\n";
    t += "\tBackgroundText \"54 83 107 255 70 370 520 24 0.7\"\t\" \"\r\n";
    return t + PcProbeTable() + "}\r\n";
}

std::string ConfirmText(const char* name, const char* title, const std::string& cmd) {
    return std::string("\r\n") + name + "\r\n{\r\n"
        "\tmenutype \"generic\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tModal \"200\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Cancel_Accept 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnEntry \"!" + cmd + " delete_show\"\r\n\tOnButtonDownA \"!" + cmd + " delete_yes\"\r\n"
        "\tOnButtonDownB \"!" + cmd + " delete_no\"\r\n"
        "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t" + Q(title) + "\r\n"
        "\tBackgroundText \"54 83 107 255 80 190 520 30 0.8\"\t\" \"\r\n"
        "\tBackgroundText \"54 83 107 255 80 216 520 30 0.8\"\t\" \"\r\n}\r\n";
}

// the lobby, one screen a map (its preview a fixed image of the screen): lines drawn by ShowLobby
constexpr int kLobbyLines = 14;
std::string LobbyText(const MapInfo& m) {
    std::string name = kLobbyPrefix + m.value;
    std::string t = Head(name.c_str()) +
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n"
        "\tBackgroundImage \"whitebox 255 255 255 46 56 84 528 290\"\r\n";
    if (!m.image.empty()) t += "\tBackgroundImage \"" + m.image + " 255 255 255 255 380 92 196 147\"\r\n";
    t += "\tOnEntry \"!pc_lobby show\"\r\n\tCommandSimulate1 \"!pc_lobby frame\"\r\n"
         "\tOnButtonDownA \"!pc_lobby 1 a\"\r\n\tOnButtonDownB \"!pc_lobby 1 b\"\r\n"
         "\tOnButtonDownX \"!pc_lobby 1 x\"\r\n\tOnButtonDownY \"!pc_lobby 1 y\"\r\n"
         "\tOnButtonDownStart \"!pc_lobby 1 start\"\r\n"
         "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"Match Lobby\"\r\n";
    for (int l = 1; l < kLobbyLines; l++) {
        int y = l <= 3 ? 92 + (l - 1) * 22 : l <= 7 ? 172 + (l - 4) * 22 : 262 + (l - 8) * 20;
        int w = l <= 3 ? 300 : 520;
        t += "\tBackgroundText \"54 83 107 255 66 " + std::to_string(y) + " " + std::to_string(w) + " 24 " +
             (l <= 7 ? "0.75" : "0.65") + "\"\t\" \"\r\n";
    }
    return t + PcProbeTable() + "}\r\n";
}

// ---- the screens' logic -------------------------------------------------------------------------------------------------
const MapInfo* CurrentMap() { return g_map >= 0 && g_map < int(g_maps.size()) ? &g_maps[size_t(g_map)] : nullptr; }

std::vector<std::string> ModesOf(const MapInfo& m) { return {m.kind}; }

void ShowModes(X86Context& ctx) {
    const MapInfo* m = CurrentMap();
    if (!m) return;
    std::vector<std::string> all = {"bomb", "hostage"}, have = ModesOf(*m);
    PcSetScreenLine(ctx, kModes, 1, "Map: " + m->label);
    for (int i = 0; i < 2; i++) {
        bool on = std::find(have.begin(), have.end(), all[size_t(i)]) != have.end();
        std::string text = std::string(i == g_mode_cursor ? ">  " : "") + ModeName(all[size_t(i)]) + (on ? "" : "   (not on this map)");
        float c = i == g_mode_cursor ? 15.f : on ? 54.f : 140.f;
        PcSetScreenLineColour(ctx, kModes, 3 + i, text, c, c + (on ? 5 : 0), c + (on ? 10 : 0), 255);
    }
    PcSetScreenLine(ctx, kModes, 6, "Rules: " + RulesetName());
    PcSetScreenLine(ctx, kModes, 9, PcPartyPrompt(1, "A Select  Y Custom Rules  B Back"));
}

void ModesInput(X86Context& ctx, const std::string& b) {
    const MapInfo* m = CurrentMap();
    if (!m) return;
    if (b == "up" || b == "down") g_mode_cursor = 1 - g_mode_cursor;
    else if (b == "y") return csx::QueueEngineCommand(std::string("xbui open ") + kRules + "\n");
    else if (b == "b") return csx::QueueEngineCommand("xbui back\n");
    else if (b == "a" || b == "start") {
        std::string kind = g_mode_cursor ? "hostage" : "bomb";
        auto have = ModesOf(*m);
        if (std::find(have.begin(), have.end(), kind) == have.end()) {
            PcSetScreenLine(ctx, kModes, 9, std::string(ModeName(kind)) + " is not played on " + m->label + ".");
            return;
        }
        g_mode = kind;
        LOGI("lobby: %s, %s, %s", m->value.c_str(), ModeName(kind), RulesetName().c_str());
        // everyone confirms again in the lobby
        auto members = PcPartyMembers();
        for (int n = 1; n <= 4; n++)
            if (members[size_t(n - 1)].in) PcPartySetMember(n, members[size_t(n - 1)].team, false);
        g_message.clear();
        return csx::QueueEngineCommand("xbui open " + std::string(kLobbyPrefix) + m->value + "\n");
    }
    ShowModes(ctx);
}

// Rulesets: Default Rules, the saved ones, then Save Current Rules As
void ShowSets(X86Context& ctx) {
    g_sets = Sets().List();
    int rows = int(g_sets.size()) + 2;
    g_set_cursor = std::clamp(g_set_cursor, 0, rows - 1);
    if (g_set_cursor < g_set_top) g_set_top = g_set_cursor;
    if (g_set_cursor >= g_set_top + kSetRows) g_set_top = g_set_cursor - kSetRows + 1;
    std::string now = RulesetName();
    for (int r = 0; r < kSetRows; r++) {
        int i = g_set_top + r;
        std::string text = i == 0 ? "Default Rules" : i <= int(g_sets.size()) ? g_sets[size_t(i - 1)].name
                         : i == int(g_sets.size()) + 1 ? "Save Current Rules As..." : "";
        if (!text.empty() && text == now && i <= int(g_sets.size())) text += "   (in use)";
        float c = i == g_set_cursor ? 15.f : 54.f;
        PcSetScreenLineColour(ctx, kSets, 1 + r, text.empty() ? "" : (i == g_set_cursor ? ">  " : "") + text, c, c + 5, c + 10, 255);
    }
    bool saved = g_set_cursor >= 1 && g_set_cursor <= int(g_sets.size());
    PcSetScreenLine(ctx, kSets, 1 + kSetRows,
                    PcPartyPrompt(1, saved ? "A Load  X Rename  BACK Delete  B Back" : g_set_cursor == 0 ? "A Load  B Back" : "A Save  B Back"));
}

void SetsInput(X86Context& ctx, const std::string& b) {
    int rows = int(g_sets.size()) + 2;
    if (b == "up") g_set_cursor = (g_set_cursor + rows - 1) % rows;
    else if (b == "down") g_set_cursor = (g_set_cursor + 1) % rows;
    else if (b == "b") return csx::QueueEngineCommand("xbui back\n");
    else if (b == "a") {
        if (g_set_cursor == 0) {
            SetRules(PcScreenDefaults(kRules), "default");
            LOGI("lobby: Default Rules loaded");
        } else if (g_set_cursor <= int(g_sets.size())) {
            const auto& set = g_sets[size_t(g_set_cursor - 1)];
            auto r = PcScreenDefaults(kRules);
            for (auto& [k, v] : Sets().Values(set.id)) r[k] = v;
            SetRules(r, set.id);
            LOGI("lobby: ruleset %s loaded", set.id.c_str());
        } else {
            g_set_renaming = false;
            return csx::QueueEngineCommand(std::string("xbui open ") + kSetName + "\n");
        }
        PcSetScreenValues(kRules, Rules());  // the rules screen under it shows them
        return csx::QueueEngineCommand("xbui back\n");
    } else if (b == "x" && g_set_cursor >= 1 && g_set_cursor <= int(g_sets.size())) {
        g_set_renaming = true;
        return csx::QueueEngineCommand(std::string("xbui open ") + kSetName + "\n");
    } else if (b == "delete" && g_set_cursor >= 1 && g_set_cursor <= int(g_sets.size())) {
        return csx::QueueEngineCommand(std::string("xbui open ") + kSetDelete + "\n");
    } else if (b == "delete_show" && g_set_cursor >= 1 && g_set_cursor <= int(g_sets.size())) {
        PcSetScreenLine(ctx, kSetDelete, 1, "Delete the ruleset \"" + g_sets[size_t(g_set_cursor - 1)].name + "\"?");
        PcSetScreenLine(ctx, kSetDelete, 2, "A deletes it, B keeps it.");
        return;
    } else if (b == "delete_yes" && g_set_cursor >= 1 && g_set_cursor <= int(g_sets.size())) {
        std::string error, id = g_sets[size_t(g_set_cursor - 1)].id;
        if (Sets().Remove(id, error)) {
            LOGI("lobby: ruleset %s deleted", id.c_str());
            if (pc::GetGlobal("party.ruleset", "") == id) pc::Set("party.ruleset", "custom");
        }
        return csx::QueueEngineCommand("xbui back\n");
    } else if (b == "delete_no") {
        return csx::QueueEngineCommand("xbui back\n");
    }
    ShowSets(ctx);
}

void SetNameDone(X86Context& ctx) {
    std::string name = PcEditText(kSetName), error;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    name.erase(0, std::min(name.size(), name.find_first_not_of(' ')));
    if (g_set_renaming && g_set_cursor >= 1 && g_set_cursor <= int(g_sets.size())) {
        if (!Sets().Rename(g_sets[size_t(g_set_cursor - 1)].id, name, error)) {
            PcSetScreenLine(ctx, kSetName, 1, error);
            return PcTypeAgain();
        }
        LOGI("lobby: ruleset %s renamed (%zu characters)", g_sets[size_t(g_set_cursor - 1)].id.c_str(), name.size());
    } else {
        std::string id = Sets().Create(name, Rules(), error);
        if (id.empty()) {
            PcSetScreenLine(ctx, kSetName, 1, error);
            return PcTypeAgain();
        }
        pc::Set("party.ruleset", id);
        pc::Save();
        LOGI("lobby: ruleset %s saved (%zu characters)", id.c_str(), name.size());
    }
    csx::QueueEngineCommand("xbui back\n");
}

// ---- the lobby ----------------------------------------------------------------------------------------------------------
bool EveryoneReady() {
    for (auto& m : PcPartyMembers())
        if (m.in && (!m.ready || m.profile.empty())) return false;
    return true;
}

std::string LobbyName() {
    const MapInfo* m = CurrentMap();
    return m ? kLobbyPrefix + m->value : std::string();
}

void ShowLobby(X86Context& ctx) {
    const MapInfo* map = CurrentMap();
    if (!map) return;
    std::string s = LobbyName();
    const char* n = s.c_str();
    auto r = Rules();
    auto members = PcPartyMembers();
    int count = PcPartyCount();
    int bots = std::min(std::atoi(r["bot_quota"].c_str()), MaxBots());
    PcSetScreenLine(ctx, n, 1, map->label + "  -  " + ModeName(g_mode));
    PcSetScreenLine(ctx, n, 2, "Rules: " + RulesetName());
    if (g_dest == "lan")
        PcSetScreenLineColour(ctx, n, 3, std::string("LAN: players on your network can join") +
                                             (g_lan_password.empty() ? "" : " (password)"), 30, 100, 40, 255);
    else
        PcSetScreenLineColour(ctx, n, 3, "LOCAL ONLY: only the players on this PC", 120, 70, 20, 255);
    for (int i = 0; i < 4; i++) {
        const PcPartyMember& m = members[size_t(i)];
        std::string line;
        if (m.in)
            line = "Player " + std::to_string(i + 1) + ": " + (m.name.empty() ? "(no profile)" : m.name) + "   " +
                   PcPartyTeamName(m.team) + "   " + (m.ready ? "READY" : "not ready") + (i == 0 ? "   (leader)" : "");
        PcSetScreenLineColour(ctx, n, 4 + i, line, m.ready ? 20.f : 120.f, m.ready ? 100.f : 60.f, m.ready ? 30.f : 30.f, 255);
    }
    PcSetScreenLine(ctx, n, 8, "Bots: " + std::to_string(bots) + " (" + Label("bot_difficulty", r["bot_difficulty"]) + ", " +
                                   Label("bot_join_team", r["bot_join_team"]) + ")    Slots: " + std::to_string(count) +
                                   " on this PC + " + std::to_string(bots) + " bots of 65");
    PcSetScreenLine(ctx, n, 9, Label("pc_gamemode", r["pc_gamemode"]) + "   Round " + r["mp_roundtime"] + " min   Freeze " + r["mp_freezetime"] + " s   Buy " +
                                   r["mp_buytime"] + " min   Money $" + r["mp_startmoney"]);
    PcSetScreenLine(ctx, n, 10, "Rounds " + (r["mp_maxrounds"] == "0" ? std::string("no limit") : r["mp_maxrounds"]) +
                                    "   Wins " + (r["mp_winlimit"] == "0" ? std::string("no limit") : r["mp_winlimit"]) +
                                    "   Friendly fire " + (r["mp_friendlyfire"] == "1" ? "On" : "Off") + "   Collision " +
                                    (r["pc_player_collision"] == "0" ? "Off" : "On"));
    std::string hint = PcPartyFocus() ? "Player " + std::to_string(PcPartyFocus()) + " is editing a profile."
                       : EveryoneReady() ? PcPartyPrompt(1, "Everyone is ready: START starts the match.  X Rules  B Back")
                                         : PcPartyPrompt(1, g_dest == "lan" ? "A Ready  Up/Down Team  X Rules  Y Password  B Back"
                                                                            : "A Ready  Up/Down Team  X Rules  B Back");
    PcSetScreenLine(ctx, n, 11, hint);
    PcSetScreenLine(ctx, n, 12, g_message);
}

std::string RandomPassword() {
    std::random_device rd;
    char b[33];
    std::snprintf(b, sizeof b, "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    return b;
}

void SetEnv(const char* name, const std::string& value) {
#ifndef _WIN32
    if (value.empty()) unsetenv(name);
    else setenv(name, value.c_str(), 1);
#endif
}

// the match: its line (the map, the bots within the slots the party leaves, the rules), where it is played (Local Only:
// a password at random that only the party's processes have), each member's team, then the title's relaunch
void StartMatch(X86Context& ctx) {
    const MapInfo* map = CurrentMap();
    if (!map) return;
    if (PcPartyFocus()) {
        g_message = "A player is editing a profile: the match waits.";
        return ShowLobby(ctx);
    }
    if (!EveryoneReady()) {
        g_message = "Every player must be ready.";
        return ShowLobby(ctx);
    }
    auto r = Rules();
    int bots = std::clamp(std::atoi(r["bot_quota"].c_str()), 0, MaxBots());
    std::string line = "+map " + map->value + " +wait +bot_quota " + std::to_string(bots);
    for (const char* k : {"bot_difficulty", "bot_join_team", "mp_friendlyfire", "mp_roundtime", "mp_freezetime",
                          "mp_buytime", "mp_startmoney", "mp_maxrounds", "mp_winlimit", "mp_timelimit", "mp_limitteams",
                          "mp_autoteambalance", "mp_c4timer", "allow_spectators", "pc_player_collision", "pc_gamemode",
                          "pc_fraglimit", "pc_gravity", "mp_flashlight"})
        line += std::string(" +") + k + " " + r[k];
    std::string password = g_dest == "lan" ? g_lan_password : RandomPassword();
    SetEnv("CSX_PARTY_MATCH", std::to_string(PcPartyCount()));
    SetEnv("CSX_PARTY_HOST", g_dest == "lan" ? "1" : "0");
    SetEnv("CSX_PARTY_PASSWORD", password);
    SetEnv("CSX_NET_PASSWORD", password);  // player 1's own join of it (players 2-4 get it from the compositor)
    SetEnv("CSX_PARTY_TEAM", std::to_string(PcPartyMembers()[0].team));
    SetEnv("CSX_PARTY_LOBBY", LobbyName());
    SetEnv("CSX_PARTY_MODE", g_mode);
    LOGI("lobby: starting %s (%s, %s) for %d local players and %d bots: %s", map->value.c_str(), ModeName(g_mode),
         g_dest == "lan" ? "LAN" : "Local Only", PcPartyCount(), bots, RulesetName().c_str());
    csx::RequestMatch(line);
}

}  // namespace

void PcLobbyInput(X86Context& ctx, int n, const std::string& b) {
    auto members = PcPartyMembers();
    if (n < 1 || n > 4 || !members[size_t(n - 1)].in) return;
    PcPartyMember m = members[size_t(n - 1)];
    g_message.clear();
    if (b == "up" || b == "down") {
        if (!m.ready) PcPartySetMember(n, (m.team + (b == "down" ? 1 : 2)) % 3, false);
    } else if (b == "a") {
        if (!m.ready) PcPartySetMember(n, m.team, true);
    } else if (b == "start") {
        if (!m.ready) PcPartySetMember(n, m.team, true);
        else if (n == 1) return StartMatch(ctx);
    } else if (b == "b") {
        if (m.ready) PcPartySetMember(n, m.team, false);
        else if (n == 1) return csx::QueueEngineCommand("xbui back\n");
    } else if (b == "x" && n == 1) {
        return csx::QueueEngineCommand(std::string("xbui open ") + kRules + "\n");
    } else if (b == "y" && n == 1 && g_dest == "lan") {
        return csx::QueueEngineCommand(std::string("xbui open ") + kPassword + "\n");
    }
    ShowLobby(ctx);
}

// the front end's lobby screens (added at load by pc_menu.cpp's PcUiLoadText)
std::string ExtendLobby(const std::string& text) {
    g_maps = ReadMaps(text);
    if (g_maps.empty()) {
        LOGW("lobby: the title's map list not found: no party matches");
        return text;
    }
    std::string out = text + DestinationText() + MapsText() + ListText(kModes, "Game Mode", 8, "pc_lobby mode") +
                      ListText(kSets, "Rulesets", kSetRows, "pc_lobby sets") + ConfirmText(kSetDelete, "Delete Ruleset", "pc_lobby sets") +
                      PcKeyboardScreen(kSetName, "Ruleset Name", "pc_lobby set_name", "Up to 20 characters.") +
                      PcKeyboardScreen(kPassword, "Match Password", "pc_lobby password", "Up to 32 letters and digits; empty for none.");
    for (const MapInfo& m : g_maps) out += LobbyText(m);
    LOGI("lobby: %zu maps (%zu from the title's list)", g_maps.size(),
         size_t(std::count_if(g_maps.begin(), g_maps.end(), [](const MapInfo& m) { return !m.image.empty(); })));
    return out;
}

void PcLobbyCommand(X86Context& ctx, const std::vector<std::string>& a) {
    std::string w1 = a.size() > 1 ? a[1] : "", w2 = a.size() > 2 ? a[2] : "";
    if (w1 == "dest") {
        if (w2 == "browser") {
            // the whole party into a server: the browser's join checks its free slots (pc_browser.cpp)
            SetEnv("CSX_PARTY_MATCH", std::to_string(PcPartyCount()));
            return csx::QueueEngineCommand("SystemLinkSetup\nxbui open SuperLauncher_SystemLink\n");
        }
        g_dest = w2 == "lan" ? "lan" : "local";
        LOGI("lobby: %s", g_dest == "lan" ? "Host LAN Match" : "Local Only");
        return csx::QueueEngineCommand(std::string("xbui open ") + kMaps + "\n");
    }
    if (w1 == "dest_show") return PcSetScreenLine(ctx, kDest, 1, std::to_string(PcPartyCount()) + " local player" + (PcPartyCount() > 1 ? "s" : ""));
    if (w1 == "map") {
        int row = PcSelectedRow(kMaps);
        if (row < 0 || row >= int(g_maps.size())) return;
        g_map = row;
        g_mode_cursor = g_maps[size_t(row)].kind == "hostage" ? 1 : 0;
        LOGI("lobby: map %s (%s)", g_maps[size_t(row)].value.c_str(), ModeName(g_maps[size_t(row)].kind));
        return csx::QueueEngineCommand(std::string("xbui open ") + kModes + "\n");
    }
    if (w1 == "mode") return w2 == "show" ? ShowModes(ctx) : ModesInput(ctx, w2);
    if (w1 == "sets") return w2 == "show" ? ShowSets(ctx) : SetsInput(ctx, w2);
    if (w1 == "set_name") return SetNameDone(ctx);
    if (w1 == "rules_load") return PcSetScreenValues(kRules, Rules());
    if (w1 == "rules_save") {
        auto now = PcScreenValues(kRules);
        if (now.empty()) return;
        std::string before = RulesetName();
        SetRules(now, pc::GetGlobal("party.ruleset", "default"));
        LOGI("lobby: rules saved (%s, was %s)", RulesetName().c_str(), before.c_str());
        return;
    }
    if (w1 == "password") {
        std::string pw = PcEditText(kPassword);
        if (!pw.empty() && (pw.size() > 32 || pw.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)) {
            PcSetScreenLine(ctx, kPassword, 1, "Letters and digits only, up to 32.");
            return PcTypeAgain();
        }
        g_lan_password = pw;
        LOGI("lobby: the LAN match %s a password", pw.empty() ? "has no" : "has");
        return csx::QueueEngineCommand("xbui back\n");
    }
    if (w1 == "return") {  // the leader's Return to Lobby (its pause menu): the party back on this match's lobby
        if (const char* lobby = std::getenv("CSX_PARTY_LOBBY"); lobby && *lobby) SetEnv("CSX_FRONTEND_OPEN", lobby);
        SetEnv("CSX_PARTY_RETURN", "1");
        LOGI("lobby: the party returns to the lobby");
        return csx::RequestFrontEnd();
    }
    if (w1 == "quit_game") {
        LOGI("lobby: the party's leader quits the game");
        return csx::AppQuit();
    }
    if (w1 == "show" || w1 == "frame") {
        // back from a match (Return to Lobby): this lobby's map, mode and place again; everyone readies again
        std::string screen = PcCurrentScreen();
        for (size_t i = 0; i < g_maps.size(); i++)
            if (screen == kLobbyPrefix + g_maps[i].value) g_map = int(i);
        if (const char* r = std::getenv("CSX_PARTY_RETURN"); r && *r == '1') {
            SetEnv("CSX_PARTY_RETURN", "");
            if (const char* h = std::getenv("CSX_PARTY_HOST")) g_dest = *h == '1' ? "lan" : "local";
            if (const char* m = std::getenv("CSX_PARTY_MODE")) g_mode = m;
            auto members = PcPartyMembers();
            for (int n = 1; n <= 4; n++)
                if (members[size_t(n - 1)].in) PcPartySetMember(n, members[size_t(n - 1)].team, false);
            g_message = "Back from the match: everyone readies again.";
        }
        return ShowLobby(ctx);
    }
    if (w1 == "1") return PcLobbyInput(ctx, 1, w2);
}

// ---- the pause menu by player (N4.8) ---------------------------------------------------------------------------------
// InGame_RootSinglePlayer (every PC match's) rebuilt for the player whose process loads it: split-screen players 2-4
// get no match-wide action (no Change Map, Restart Match or Friends List; Quit leaves their own match: Leave Match);
// player 1 of a party match (CSX_PARTY_LOBBY) also gets Return to Lobby, End Match (the title's Quit) and Quit Game.
// Anyone else keeps the menu as it is.
namespace {
struct PauseRow {
    std::string command, label;
};

std::string RowText(const PauseRow& r) {
    return "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect   " + Q(r.command) + "\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed " +
           Q(r.label) + "\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n";
}

std::string Confirm(const char* name, const char* title, const char* line1, const char* line2, const char* yes) {
    return std::string("\r\n") + name + "\r\n{\r\n"
        "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_enter.wav\"\r\n\tModal \"50\"\r\n"
        "\tBackgroundImage \"csxbackground\t\t100 100 80 235\t\t0 0 640 480\"\r\n"
        "\tOnButtonDownA " + Q(yes) + "\r\n\tOnButtonDownB \"back\"\r\n"
        "\tBackgroundText \"177 169 54 255 65 45 400 30\"\t" + Q(title) + "\r\n"
        "\tBackgroundText \"255 255 255 255 100 200 440 30 0.8\"\t" + Q(line1) + "\r\n"
        "\tBackgroundText \"255 255 255 255 100 230 440 30 0.8\"\t" + Q(line2) + "\r\n" + NoTable() + "}\r\n";
}
}  // namespace

std::string PcLobbyPauseMenu(const std::string& text) {
    int player = csx::SplitPlayer();
    bool leader = player <= 1 && std::getenv("CSX_PARTY_LOBBY") && *std::getenv("CSX_PARTY_LOBBY");
    if (player < 2 && !leader) return text;
    size_t screen = text.find("\nInGame_RootSinglePlayer\r\n");
    size_t next = screen == std::string::npos ? screen : text.find("\nInGame_", screen + 1);
    size_t first = screen == std::string::npos ? screen : text.find("\t\tRow\r\n", screen);
    size_t table_end = next == std::string::npos ? next : text.rfind("\t}\r\n}\r\n", next);
    if (first == std::string::npos || table_end == std::string::npos || first > table_end) {
        LOGW("lobby: the pause menu not as expected: left as it is");
        return text;
    }
    std::vector<PauseRow> rows;
    for (size_t at = first; at < table_end;) {
        size_t r = text.find("\t\tRow\r\n", at);
        if (r == std::string::npos || r >= table_end) break;
        size_t c = text.find("CommandSelect", r), close = text.find("\r\n\t\t}\r\n", r);
        size_t q1 = text.find('"', c), q2 = text.find('"', q1 + 1);
        size_t l = text.find("CellFixed \"", text.find("CellFixed \"", c) + 1);
        size_t l1 = text.find('"', l), l2 = text.find('"', l1 + 1);
        if (c == std::string::npos || close == std::string::npos || l2 > close) break;
        rows.push_back({text.substr(q1 + 1, q2 - q1 - 1), text.substr(l1 + 1, l2 - l1 - 1)});
        at = close + 7;
    }
    std::vector<PauseRow> out;
    PauseRow quit;
    for (const PauseRow& r : rows) {
        if (r.command.find("ChangeMaps") != std::string::npos || r.command.find("ConfirmRestartMatch") != std::string::npos) {
            if (leader) out.push_back(r);
        } else if (r.command.find("LiveAware") != std::string::npos) {
            continue;  // Xbox Live's friends: none on the PC
        } else if (r.command.find("ConfirmQuit") != std::string::npos) {
            quit = r;
        } else {
            out.push_back({r.command, r.label == "Options" ? "Player Settings" : r.label});
        }
    }
    if (leader) {
        // Player Settings before the match's actions, as the brief's order: Resume, Statistics, Teams, Settings, ...
        auto settings = std::find_if(out.begin(), out.end(), [](const PauseRow& r) { return r.label == "Player Settings"; });
        if (settings != out.end()) {
            PauseRow ps = *settings;
            out.erase(settings);
            auto at = std::find_if(out.begin(), out.end(), [](const PauseRow& r) { return r.command.find("ChangeMaps") != std::string::npos ||
                                                                                     r.command.find("Restart") != std::string::npos; });
            out.insert(at, ps);
        }
        out.push_back({"open InGame_PCConfirmLobby", "Return to Lobby"});
        if (!quit.command.empty()) out.push_back({quit.command, "End Match"});
        out.push_back({"open InGame_PCConfirmQuitGame", "Quit Game"});
    } else if (!quit.command.empty()) {
        out.push_back({quit.command, "Leave Match"});
    }
    std::string body;
    for (const PauseRow& r : out) body += RowText(r);
    std::string result = text.substr(0, first) + body + text.substr(table_end);
    if (leader)
        result += Confirm("InGame_PCConfirmLobby", "Return to Lobby", "The match ends for every player.",
                          "A returns the party to the lobby, B keeps playing.", "!pc_lobby return") +
                  Confirm("InGame_PCConfirmQuitGame", "Quit Game", "The match ends and the game closes.",
                          "A quits, B keeps playing.", "!pc_lobby quit_game");
    LOGI("lobby: the pause menu for %s: %zu rows", leader ? "the party's leader" : ("player " + std::to_string(player)).c_str(), out.size());
    return result;
}

// every frame (pc_menu.cpp's PcMenuFrame), in every player's process: a party match's member joins its team when
// the title's team screen first opens (the team the lobby showed; Auto-Select as the screen's own first row)
void PcLobbyMenuFrame(X86Context& ctx) {
    static std::string last;
    std::string screen = PcCurrentScreen();
    // player 1's directions on the screens these commands draw (a screen just opened: its first row is no press)
    static std::string probed;
    if (screen != kModes && screen != kSets && screen.rfind(kLobbyPrefix, 0) != 0) {
        probed.clear();  // not one of these screens: the title's own table is left alone
    } else if (screen != probed) {
        if (PcSelectedRow(screen) >= 0) {  // once its table is there
            probed = screen;
            PcProbeTake(screen);
        }
    } else if (screen == kModes)
        for (const std::string& d : PcProbeTake(screen)) ModesInput(ctx, d);
    else if (screen == kSets)
        for (const std::string& d : PcProbeTake(screen)) SetsInput(ctx, d);
    else if (screen.rfind(kLobbyPrefix, 0) == 0)
        for (const std::string& d : PcProbeTake(screen)) PcLobbyInput(ctx, 1, d);
    if (screen == last) return;
    last = screen;
    if (screen == kPassword) PcClearEdit(ctx, kPassword, g_lan_password);
    if (screen == kSetName) {
        PcClearEdit(ctx, kSetName, g_set_renaming && g_set_cursor >= 1 && g_set_cursor <= int(g_sets.size()) ? g_sets[size_t(g_set_cursor - 1)].name : "");
        PcSetScreenLine(ctx, kSetName, 1, "The ruleset's name, up to 20 characters.");
    }
    if (screen == kSets) ShowSets(ctx);
    if (screen == kModes) ShowModes(ctx);
    const char* team = std::getenv("CSX_PARTY_TEAM");
    if (!team || g_team_done || screen != "TeamSelect_RootSinglePlayer") return;
    g_team_done = true;
    int t = std::atoi(team);
    const char* slot = t == 1 ? "slot1" : t == 2 ? "slot2" : "slot5";  // the screen's rows: Terrorist, CT, Auto-Select
    LOGI("lobby: joining %s (the party's team)", t == 1 ? "the Terrorists" : t == 2 ? "the Counter-Terrorists" : "a team (Auto-Select)");
    csx::QueueEngineCommand(std::string(slot) + "\nxbui exit\n");
}
