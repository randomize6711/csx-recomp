// The multiplayer party (N4.8, docs/multiplayer_frontend.md): Main menu > Multiplayer opens the party screen
// (SuperLauncher_PCParty), four panels, one a local player: "PRESS A TO JOIN" until a controller's A or START joins it
// (the compositor's PartyJoin), then its profile, its team and whether it is ready. Player 1 (the party's leader: the
// player whose process runs this front end) drives the screen with its own devices through the title's UI; the other
// members' controllers reach it as party messages (split::Msg::Party, csx::TakePartyInput) and drive only their own
// panel. Player 1 continues when every member is ready. The party itself (members, devices, profiles, teams, ready)
// is the compositor's, so it outlives player 1's relaunches: this file shows it and asks for changes ("party set").
//
// Profiles (src/common/profiles.h): a member picks one of the saved profiles with left / right (one profile is never
// two members'), X opens Profiles (select, create, rename, edit, delete) for that member, which has the menus while it
// is there (the compositor's menu focus: its controller works player 1's front end, player 1's devices wait). Player
// 1 plays as its profile (pc::UseProfile); players 2-4 are started with theirs (CSX_PC_PROFILE).
//
// The game alone in its window (no compositor) restarts as the compositor when Multiplayer is opened, back on this
// screen (CSX_FRONTEND_OPEN).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "common/profiles.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/pc_party.h"

void PcSetScreenLine(X86Context& ctx, const char* screen, int index, const std::string& text);  // pc_browser.cpp
void PcSetScreenLineColour(X86Context& ctx, const char* screen, int index, const std::string& text, float r, float g,
                           float b, float a);
std::string PcCurrentScreen();
std::string PcKeyboardScreen(const char* name, const char* title, const char* command, const char* hint);
std::string PcEditText(const char* screen);
void PcClearEdit(X86Context& ctx, const char* screen, const std::string& text);
void PcTypeAgain();  // pc_mouse.cpp
std::map<std::string, std::string> PcScreenValues(const std::string& name);                           // pc_menu.cpp
void PcSetScreenValues(const std::string& name, const std::map<std::string, std::string>& values);   // pc_menu.cpp
std::vector<int> PcListRows(const std::string& screen);                                              // pc_menu.cpp
bool PcSetListRows(const std::string& screen, const std::vector<int>& picks);                        // pc_menu.cpp
void PcLobbyInput(X86Context& ctx, int n, const std::string& what);                                  // pc_lobby.cpp
int PcSelectedRow(const std::string& screen);                                                        // pc_menu.cpp
void PcSetSelectedRow(const std::string& screen, int row);                                           // pc_menu.cpp

// ---- directions on screens whose lines these commands draw ----------------------------------------------------------
// The title runs only its own table commands for the pad's directions (CommandHighlight), so such a screen has an
// unseen table of three rows, the middle one a list of three choices, kept on the middle row and choice: a move off
// it is up or down, a choice changed left or right (the title's own d-pad, stick, keyboard and sounds), put back each
// frame
std::string PcProbeTable() {
    return "\tPieceOffsetDegrees \"0\"\r\n"
           "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableIncreaseValue\"\r\n\t}\r\n"
           "\tGeneric_4_2\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n"
           "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableDecreaseValue\"\r\n\t}\r\n"
           "\tGeneric_4_4\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n"
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n\t\t\tColumnWidthPercentage \"50\"\r\n\t\t\tColumnWidthPercentage \"50\"\r\n"
           "\t\t\tTableLocation \"700 700\"\r\n\t\t\tTableSize \"10 30\"\r\n\t\t}\r\n"
           "\t\tRow\r\n\t\t{\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellList\r\n\t\t\t{\r\n\t\t\t\tListDefault \" \"\t\"0\"\r\n\t\t\t}\r\n\t\t}\r\n"
           "\t\tRow\r\n\t\t{\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellList\r\n\t\t\t{\r\n\t\t\t\tListOption \" \"\t\"l\"\r\n"
           "\t\t\t\tListDefault \" \"\t\"m\"\r\n\t\t\t\tListOption \" \"\t\"r\"\r\n\t\t\t}\r\n\t\t}\r\n"
           "\t\tRow\r\n\t\t{\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellList\r\n\t\t\t{\r\n\t\t\t\tListDefault \" \"\t\"0\"\r\n\t\t\t}\r\n\t\t}\r\n"
           "\t}\r\n";
}

// the directions taken on the screen since the last call ("up", "down", "left", "right"), the table put back
std::vector<std::string> PcProbeTake(const std::string& screen) {
    std::vector<std::string> out;
    int row = PcSelectedRow(screen);
    if (row < 0) return out;
    std::vector<int> picks = PcListRows(screen);
    if (picks.size() < 3) return out;
    if (row == 0) out.push_back("up");
    else if (row == 2) out.push_back("down");
    if (picks[1] == 0) out.push_back("left");
    else if (picks[1] == 2) out.push_back("right");
    if (row != 1) PcSetSelectedRow(screen, 1);
    if (picks[1] != 1) PcSetListRows(screen, {-1, 1, -1});
    return out;
}

namespace {

constexpr char kParty[] = "SuperLauncher_PCParty";
constexpr char kProfiles[] = "SuperLauncher_PCProfiles";
constexpr char kName[] = "SuperLauncher_PCProfileName";
constexpr char kDelete[] = "SuperLauncher_PCProfileDelete";
constexpr char kEdit[] = "SuperLauncher_PCProfile";  // pc_menu.cpp's PcScreen "Profile": its settings
constexpr int kLines = 6;                            // a panel's BackgroundText lines (after the title's)
constexpr int kListRows = 10;                        // Profiles' rows shown at once

const char* const kTeams[] = {"Auto-Select", "Terrorist", "Counter-Terrorist"};

struct Member {
    bool in = false;            // a member of the party
    std::string pad = "0";      // "1" its controller, "lost" unplugged, "0" none
    std::string profile;        // its profile's id ("" none)
    int team = 0;
    bool ready = false;
};
Member g_m[4];
Member g_solo;                  // player 1's panel while the game is alone in its window (no compositor's party)
int g_count = 1, g_kbm = 1, g_focus = 0;
std::string g_note[4];          // a panel's passing message ("That profile is player 2's.")
std::string g_shown;            // what the panels show now (redrawn when it changes)
bool g_party_on = false;        // the compositor was told a party screen is shown
bool g_pad_applied = false;     // the profile's controller options given to the title this boot (a relaunch: again)

// the Profiles screen: whose, the cursor, the list it shows, what the keyboard is for
int g_prof_member = 0, g_prof_cursor = 0, g_prof_top = 0;
std::vector<profiles::Info> g_prof_list;
enum class Naming { None, Create, Rename } g_naming = Naming::None;
std::string g_edit_profile;     // the profile Profiles > Y edits

profiles::Store Store() { return profiles::Store(pc::ProfileFolder()); }

std::string Q(const std::string& s) { return "\"" + s + "\""; }

bool PartyScreen(const std::string& s) {
    return s == kParty || s == kProfiles || s == kName || s == kDelete || s == kEdit ||
           s.rfind("SuperLauncher_PCParty", 0) == 0 || s.rfind("SuperLauncher_PCLobby", 0) == 0 ||
           s.rfind("SuperLauncher_PCMatch", 0) == 0 || s == "SuperLauncher_PCRules" || s == "SuperLauncher_PCSplit";
}

// the compositor's party ("member N pad P profile ID team T ready R" lines, "kbm N", "focus N", "players N")
void ReadState() {
    std::string state = csx::SplitState();
    for (auto& m : g_m) m = {};
    g_count = 1;
    g_m[0].in = true;
    for (size_t at = 0; at < state.size();) {
        size_t e = state.find('\n', at);
        std::string l = state.substr(at, e == std::string::npos ? std::string::npos : e - at);
        at = e == std::string::npos ? state.size() : e + 1;
        int n = 0, team = 0, ready = 0;
        char pad[8] = {}, id[24] = {};
        if (std::sscanf(l.c_str(), "member %d pad %7s profile %23s team %d ready %d", &n, pad, id, &team, &ready) == 5 &&
            n >= 1 && n <= 4) {
            Member& m = g_m[n - 1];
            m.in = true;
            m.pad = pad;
            m.profile = profiles::IdValid(id) ? id : "";
            m.team = std::clamp(team, 0, 2);
            m.ready = ready != 0;
            g_count = std::max(g_count, n);
        } else if (std::sscanf(l.c_str(), "kbm %d", &n) == 1) {
            g_kbm = n;
        } else if (std::sscanf(l.c_str(), "focus %d", &n) == 1) {
            g_focus = n;
        }
    }
    // without a compositor (the game alone in its window, the headless harness): player 1 alone, its panel kept here
    if (!csx::SplitPlayer()) {
        g_m[0] = g_solo;
        g_m[0].in = true;
        g_m[0].pad = "0";
    }
}

void Tell(int n) {
    const Member& m = g_m[n - 1];
    if (!csx::SplitPlayer() && n == 1) g_solo = m;  // the game alone: its own panel's state
    // the slot's profile is remembered for the next party (its own again when it is still free)
    if (!m.profile.empty() && pc::GetGlobal("party.profile_" + std::to_string(n), "") != m.profile) {
        pc::Set("party.profile_" + std::to_string(n), m.profile);
        pc::Save();
    }
    if (!csx::SplitPlayer()) return;
    csx::SplitSend(csx::kSplitSetup, "party set " + std::to_string(n) + " profile " + (m.profile.empty() ? "-" : m.profile) +
                                         " team " + std::to_string(m.team) + " ready " + (m.ready ? "1" : "0"));
}

// player 1 plays as its member's profile (its settings, its name)
void UseOwnProfile() {
    if (g_m[0].profile == pc::ActiveProfile()) return;
    pc::UseProfile(g_m[0].profile);
    g_pad_applied = false;  // its controller options into the title's
}

bool Taken(const std::string& id, int except) {
    for (int i = 0; i < 4; i++)
        if (i + 1 != except && g_m[i].in && g_m[i].profile == id) return true;
    return false;
}

std::string ProfileName(const std::string& id) {
    std::string n = id.empty() ? "" : Store().Name(id);
    return n;
}

// the button prompts of a member's device: the controller's, or the keyboard's for the keyboard's member (the menus'
// keys, src/linux/window_input.cpp: Space is A, Escape (or Left Ctrl) B, R X, Q Y, Tab Back, Enter START)
std::string Prompt(int n, const char* pad) {
    if (n != g_kbm || g_m[n - 1].pad == "1") return pad;
    std::string p = pad;
    static const std::pair<const char*, const char*> kKeys[] = {{"A ", "SPACE "}, {"B ", "ESC "}, {"X ", "R "},
                                                                {"Y ", "Q "}, {"BACK ", "TAB "}};
    std::string out;
    for (size_t at = 0; at < p.size();) {
        bool done = false;
        for (auto& [b, k] : kKeys)
            if ((at == 0 || p[at - 1] == ' ') && p.compare(at, std::strlen(b), b) == 0) {
                out += k;
                at += std::strlen(b);
                done = true;
                break;
            }
        if (!done) out += p[at++];
    }
    return out;
}

// ---- the party screen ---------------------------------------------------------------------------------------------------
// the panels' places (640x480): 2x2
int PanelX(int i) { return i % 2 ? 336 : 64; }
int PanelY(int i) { return i < 2 ? 92 : 238; }

std::string PartyScreenText() {
    std::string t = std::string("\r\n") + kParty + "\r\n{\r\n"
        "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
        "\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n";
    for (int i = 0; i < 4; i++)  // each panel's box: the title's white box, faint
        t += "\tBackgroundImage \"whitebox 255 255 255 46 " + std::to_string(PanelX(i) - 8) + " " +
             std::to_string(PanelY(i) - 6) + " 256 136\"\r\n";
    t += "\tOnEntry \"!pc_party enter\"\r\n\tCommandSimulate1 \"!pc_party frame\"\r\n"
         "\tOnButtonDownA \"!pc_party 1 a\"\r\n\tOnButtonDownB \"!pc_party 1 b\"\r\n"
         "\tOnButtonDownX \"!pc_party 1 x\"\r\n\tOnButtonDownY \"!pc_party 1 y\"\r\n"
         "\tOnButtonDownStart \"!pc_party 1 start\"\r\n\tOnButtonDownBack \"!pc_party 1 back\"\r\n"
         "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"Multiplayer\"\r\n";
    for (int i = 0; i < 4; i++)
        for (int l = 0; l < kLines; l++) {
            char b[160];
            std::snprintf(b, sizeof b, "\tBackgroundText \"54 83 107 255 %d %d 240 24 %s\"\t\" \"\r\n", PanelX(i),
                          PanelY(i) + l * 20, l == 0 ? "0.8" : "0.7");
            t += b;
        }
    t += "\tBackgroundText \"54 83 107 255 70 384 500 24 0.7\"\t\" \"\r\n";  // the leader's hint line
    return t + PcProbeTable() + "}\r\n";
}

struct Line {
    std::string text;
    float r = 54, g = 83, b = 107;
};

std::vector<Line> PanelLines(int i) {
    int n = i + 1;
    const Member& m = g_m[i];
    std::vector<Line> l(kLines);
    l[0] = {"PLAYER " + std::to_string(n) + (n == 1 ? "  -  PARTY LEADER" : ""), 30, 45, 60};
    if (!m.in) {
        l[2] = {"PRESS A TO JOIN", 120, 120, 120};
        l[3] = {csx::SplitPlayer() ? "(A or START on a free controller)" : "(the game restarts in split screen)", 120, 120, 120};
        return l;
    }
    std::string dev = m.pad == "1" ? "Controller" : m.pad == "lost" ? "CONTROLLER DISCONNECTED" : "";
    if (n == g_kbm) dev = dev.empty() || m.pad == "lost" ? "Keyboard and Mouse" + std::string(m.pad == "lost" ? " (controller lost)" : "")
                                                          : "Keyboard, Mouse and Controller";
    if (dev.empty()) dev = "No controller: plug one in";
    l[1] = {dev, m.pad == "lost" ? 160.f : 54.f, m.pad == "lost" ? 40.f : 83.f, m.pad == "lost" ? 40.f : 107.f};
    std::string pname = ProfileName(m.profile);
    l[2] = {pname.empty() ? "<  Choose a profile  >" : "<  " + pname + "  >", 20, 30, 40};
    l[3] = {std::string("Team: ") + kTeams[m.team] + (m.ready ? "" : "   (Up/Down)")};
    if (m.ready) l[4] = {"READY", 20, 110, 30};
    else l[4] = {pname.empty() ? "NOT READY: choose a profile" : "NOT READY", 150, 60, 30};
    if (!g_note[i].empty()) l[4] = {g_note[i], 150, 60, 30};
    if (g_focus == n) l[5] = {"Editing its profile...", 54, 83, 107};
    else if (m.ready) l[5] = {Prompt(n, n == 1 ? "A Continue  B Not ready" : "B Not ready")};
    else l[5] = {Prompt(n, n == 1 ? "A Ready  X Profiles  BACK Options" : "A Ready  B Leave  X Profiles")};
    return l;
}

void ShowParty(X86Context& ctx, bool force = false) {
    ReadState();
    std::string all;
    std::vector<std::vector<Line>> panels;
    for (int i = 0; i < 4; i++) {
        panels.push_back(PanelLines(i));
        for (auto& l : panels.back()) all += l.text + "|";
    }
    bool everyone = true;
    for (int i = 0; i < g_count; i++) everyone = everyone && g_m[i].ready;
    std::string hint = g_focus ? "Player " + std::to_string(g_focus) + " is editing a profile."
                       : everyone ? Prompt(1, "Everyone is ready: A Continue.")
                                  : "Every player picks a profile and gets ready; player 1 then continues.";
    all += hint;
    if (!force && all == g_shown) return;
    g_shown = all;
    for (int i = 0; i < 4; i++)
        for (int l = 0; l < kLines; l++) {
            const Line& x = panels[size_t(i)][size_t(l)];
            PcSetScreenLineColour(ctx, kParty, 1 + i * kLines + l, x.text, x.r, x.g, x.b, 255);
        }
    PcSetScreenLine(ctx, kParty, 1 + 4 * kLines, hint);
}

// a member's left / right: the next profile no other member has (a fresh party picks the first free one with A)
void CycleProfile(int n, int step) {
    Member& m = g_m[n - 1];
    std::vector<profiles::Info> list = Store().List();
    std::vector<std::string> free;
    for (auto& p : list)
        if (!Taken(p.id, n)) free.push_back(p.id);
    if (free.empty()) {
        g_note[n - 1] = list.empty() ? "No profiles yet: X creates one." : "Every profile is another player's.";
        return;
    }
    auto it = std::find(free.begin(), free.end(), m.profile);
    int at = it == free.end() ? (step > 0 ? -1 : 0) : int(it - free.begin());
    at = (at + step + int(free.size())) % int(free.size());
    m.profile = free[size_t(at)];
    g_note[n - 1].clear();
    Tell(n);
    if (n == 1) UseOwnProfile();
}

void OpenProfiles(int n);

bool EveryoneReady() {
    for (int i = 0; i < g_count; i++)
        if (!g_m[i].ready || g_m[i].profile.empty()) return false;
    return true;
}

// a member's button on its panel (player 1's from this screen's handlers, the others' from the compositor)
void PanelInput(X86Context& ctx, int n, const std::string& b) {
    ReadState();
    if (n < 1 || n > 4) return;
    Member& m = g_m[n - 1];
    if (b == "joined" || b == "pad on" || b == "pad off") {
        g_note[n - 1].clear();
        if (b == "joined" && m.profile.empty()) {  // its slot's last profile, while it is free
            std::string id = pc::GetGlobal("party.profile_" + std::to_string(n), "");
            if (!id.empty() && Store().Exists(id) && !Taken(id, n)) {
                m.profile = id;
                Tell(n);
            }
        }
        return ShowParty(ctx, true);
    }
    if (!m.in || g_focus) return;  // a member editing its profile has the menus: the panels wait
    if (b == "left" || b == "right") {
        if (!m.ready) CycleProfile(n, b == "right" ? 1 : -1);
    } else if (b == "up" || b == "down") {
        if (!m.ready) {
            m.team = (m.team + (b == "down" ? 1 : 2)) % 3;
            Tell(n);
        }
    } else if (b == "a" || b == "start") {
        if (m.ready) {
            if (n == 1) {
                if (EveryoneReady()) {
                    LOGI("party: player 1 continues with %d players", g_count);
                    csx::QueueEngineCommand("xbui open SuperLauncher_PCPartyDestination\n");
                } else {
                    g_note[0] = "Waiting for every player to be ready.";
                }
            }
        } else if (m.profile.empty()) {
            CycleProfile(n, 1);  // the first free profile, or a note that there is none
            if (g_m[n - 1].profile.empty() && Store().List().empty()) OpenProfiles(n);
        } else {
            m.ready = true;
            g_note[n - 1].clear();
            LOGI("party: player %d ready (%s, %s)", n, ProfileName(m.profile).c_str(), kTeams[m.team]);
            Tell(n);
        }
    } else if (b == "b") {
        if (m.ready) {
            m.ready = false;
            Tell(n);
        } else if (n > 1) {
            LOGI("party: player %d leaves", n);
            g_note[n - 1].clear();
            csx::SplitSend(csx::kSplitSetup, "party leave " + std::to_string(n));
        } else {
            csx::QueueEngineCommand("xbui back\n");
        }
    } else if (b == "x") {
        if (!m.ready) OpenProfiles(n);
    } else if (b == "back" && n == 1) {
        csx::QueueEngineCommand("xbui open SuperLauncher_PCSplit\n");  // Split Screen Options (layout, player 1's controller)
        return;
    }
    ShowParty(ctx, true);
}

// ---- Profiles (for one member) --------------------------------------------------------------------------------------------
std::string ProfilesScreenText() {
    std::string t = std::string("\r\n") + kProfiles + "\r\n{\r\n"
        "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
        "\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n"
        "\tBackgroundImage \"whitebox 255 255 255 46 72 88 496 270\"\r\n"
        "\tOnEntry \"!pc_party prof show\"\r\n"
        "\tOnButtonDownA \"!pc_party prof a\"\r\n\tOnButtonDownB \"!pc_party prof b\"\r\n"
        "\tOnButtonDownX \"!pc_party prof x\"\r\n\tOnButtonDownY \"!pc_party prof y\"\r\n"
        "\tOnButtonDownBack \"!pc_party prof delete\"\r\n"
        "\tBackgroundText \"54 83 107 255 65 45 500 30\"\t\"Profiles\"\r\n";
    for (int r = 0; r < kListRows + 1; r++)
        t += "\tBackgroundText \"54 83 107 255 90 " + std::to_string(96 + r * 23) + " 460 24 0.8\"\t\" \"\r\n";
    t += "\tBackgroundText \"54 83 107 255 70 370 520 24 0.7\"\t\" \"\r\n";
    return t + PcProbeTable() + "}\r\n";
}

// a profile's deletion asked first (Back on Profiles): A deletes, B keeps it
std::string DeleteScreenText() {
    return std::string("\r\n") + kDelete + "\r\n{\r\n"
        "\tmenutype \"generic\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tModal \"200\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Cancel_Accept 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnEntry \"!pc_party prof delete_show\"\r\n"
        "\tOnButtonDownA \"!pc_party prof delete_yes\"\r\n\tOnButtonDownB \"!pc_party prof delete_no\"\r\n"
        "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"Delete Profile\"\r\n"
        "\tBackgroundText \"54 83 107 255 80 190 520 30 0.8\"\t\" \"\r\n"
        "\tBackgroundText \"54 83 107 255 80 216 520 30 0.8\"\t\" \"\r\n"
        "\tBackgroundText \"54 83 107 255 80 242 520 30 0.8\"\t\" \"\r\n}\r\n";
}

void OpenProfiles(int n) {
    g_prof_member = n;
    g_prof_cursor = 0;
    g_prof_top = 0;
    // the member has the menus while it is in Profiles (and the keyboard, its settings screen)
    if (n > 1) csx::SplitSend(csx::kSplitSetup, "party focus " + std::to_string(n));
    LOGI("party: player %d opens Profiles", n);
    csx::QueueEngineCommand("xbui open " + std::string(kProfiles) + "\n");
}

void ShowProfiles(X86Context& ctx) {
    ReadState();
    g_prof_list = Store().List();
    int rows = int(g_prof_list.size()) + 1;  // the profiles, then Create New Profile
    g_prof_cursor = std::clamp(g_prof_cursor, 0, rows - 1);
    if (g_prof_cursor < g_prof_top) g_prof_top = g_prof_cursor;
    if (g_prof_cursor >= g_prof_top + kListRows + 1) g_prof_top = g_prof_cursor - kListRows;
    PcSetScreenLine(ctx, kProfiles, 0, "Profiles  -  Player " + std::to_string(g_prof_member));
    for (int r = 0; r < kListRows + 1; r++) {
        int i = g_prof_top + r;
        std::string text;
        float c[3] = {54, 83, 107};
        if (i < int(g_prof_list.size())) {
            const auto& p = g_prof_list[size_t(i)];
            text = p.name;
            for (int k = 0; k < 4; k++)
                if (g_m[k].in && g_m[k].profile == p.id) text += k + 1 == g_prof_member ? "   (yours)" : "   (player " + std::to_string(k + 1) + ")";
        } else if (i == int(g_prof_list.size())) {
            text = "Create New Profile";
        }
        if (i == g_prof_cursor && !text.empty()) {
            text = ">  " + text;
            c[0] = 15, c[1] = 20, c[2] = 25;
        }
        PcSetScreenLineColour(ctx, kProfiles, 1 + r, text, c[0], c[1], c[2], 255);
    }
    std::string hint = g_prof_cursor < int(g_prof_list.size()) ? "A Select  X Rename  Y Settings  BACK Delete  B Done"
                                                                 : "A Create  B Done";
    PcSetScreenLine(ctx, kProfiles, 2 + kListRows, Prompt(g_prof_member, hint.c_str()));
}

void CloseProfiles() {
    csx::SplitSend(csx::kSplitSetup, "party focus 0");
    csx::QueueEngineCommand("xbui back\n");
}

void ProfilesInput(X86Context& ctx, const std::string& b) {
    ReadState();
    LOGI("party: Profiles (player %d): %s at row %d", g_prof_member, b.c_str(), g_prof_cursor);
    int rows = int(g_prof_list.size()) + 1;
    bool on_profile = g_prof_cursor < int(g_prof_list.size());
    const std::string id = on_profile ? g_prof_list[size_t(g_prof_cursor)].id : "";
    Member& m = g_m[std::clamp(g_prof_member, 1, 4) - 1];
    if (b == "up") g_prof_cursor = (g_prof_cursor + rows - 1) % rows;
    else if (b == "down") g_prof_cursor = (g_prof_cursor + 1) % rows;
    else if (b == "a") {
        if (!on_profile) {
            g_naming = Naming::Create;
            csx::QueueEngineCommand("xbui open " + std::string(kName) + "\n");
            return;
        }
        if (Taken(id, g_prof_member)) {
            PcSetScreenLine(ctx, kProfiles, 2 + kListRows, "That profile is another player's.");
            return;
        }
        m.profile = id;
        Tell(g_prof_member);
        if (g_prof_member == 1) UseOwnProfile();
        LOGI("party: player %d plays as profile %s (%s)", g_prof_member, id.c_str(), ProfileName(id).c_str());
        return CloseProfiles();
    } else if (b == "b") {
        return CloseProfiles();
    } else if (b == "x" && on_profile) {
        g_naming = Naming::Rename;
        csx::QueueEngineCommand("xbui open " + std::string(kName) + "\n");
        return;
    } else if (b == "y" && on_profile) {
        g_edit_profile = id;
        csx::QueueEngineCommand("xbui open " + std::string(kEdit) + "\n");
        return;
    } else if (b == "delete" && on_profile) {
        if (Taken(id, 0) && !(m.profile == id)) {
            PcSetScreenLine(ctx, kProfiles, 2 + kListRows, "That profile is another player's: it is not deleted.");
            return;
        }
        csx::QueueEngineCommand("xbui open " + std::string(kDelete) + "\n");
        return;
    }
    ShowProfiles(ctx);
}

void DeleteInput(X86Context& ctx, const std::string& b) {
    if (g_prof_cursor >= int(g_prof_list.size())) return csx::QueueEngineCommand("xbui back\n");
    profiles::Info p = g_prof_list[size_t(g_prof_cursor)];
    if (b == "delete_show") {
        PcSetScreenLine(ctx, kDelete, 1, "Delete the profile \"" + p.name + "\"?");
        PcSetScreenLine(ctx, kDelete, 2, "Its settings are lost. A deletes it, B keeps it.");
        return;
    }
    if (b == "delete_yes") {
        std::string error;
        if (Store().Remove(p.id, error)) {
            LOGI("party: profile %s (%s) deleted", p.id.c_str(), p.name.c_str());
            for (int k = 0; k < 4; k++)
                if (g_m[k].in && g_m[k].profile == p.id) {
                    g_m[k].profile.clear();
                    g_m[k].ready = false;
                    Tell(k + 1);
                    if (k == 0) UseOwnProfile();
                }
        } else {
            LOGW("party: profile %s not deleted: %s", p.id.c_str(), error.c_str());
        }
    }
    csx::QueueEngineCommand("xbui back\n");
}

// the keyboard's Done: a new profile (the PC's current settings its first values) or a new name
void NameDone(X86Context& ctx) {
    std::string name = PcEditText(kName);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    name.erase(0, std::min(name.size(), name.find_first_not_of(' ')));
    std::string error;
    if (g_naming == Naming::Create) {
        std::map<std::string, std::string> first;
        for (const char* k : {"mouse_sensitivity", "mouse_zoom_sensitivity", "mouse_invert", "mouse_raw", "fov", "prompt_style"})
            if (std::string v = pc::GetGlobal(k, ""); !v.empty()) first[k] = v;
        std::string id = Store().Create(name, first, error);
        if (id.empty()) {
            PcSetScreenLine(ctx, kName, 1, error);
            PcTypeAgain();
            return;
        }
        LOGI("party: profile %s created (%zu characters)", id.c_str(), name.size());
        // a member without a profile takes the one it made
        Member& m = g_m[std::clamp(g_prof_member, 1, 4) - 1];
        if (m.profile.empty()) {
            m.profile = id;
            Tell(g_prof_member);
            if (g_prof_member == 1) UseOwnProfile();
        }
    } else if (g_naming == Naming::Rename && g_prof_cursor < int(g_prof_list.size())) {
        if (!Store().Rename(g_prof_list[size_t(g_prof_cursor)].id, name, error)) {
            PcSetScreenLine(ctx, kName, 1, error);
            PcTypeAgain();
            return;
        }
        LOGI("party: profile %s renamed", g_prof_list[size_t(g_prof_cursor)].id.c_str());
    }
    g_naming = Naming::None;
    csx::QueueEngineCommand("xbui back\n");
}

void NameOpen(X86Context& ctx) {
    std::string now = g_naming == Naming::Rename && g_prof_cursor < int(g_prof_list.size()) ? g_prof_list[size_t(g_prof_cursor)].name : "";
    PcClearEdit(ctx, kName, now);
    PcSetScreenLine(ctx, kName, 1, g_naming == Naming::Rename ? "The profile's new name, up to 20 characters."
                                                               : "The new profile's name, up to 20 characters.");
}

// the profile's settings screen (pc_menu.cpp's PcScreen "Profile", its rows the profile-scoped settings): loaded from
// the profile Profiles > Y chose, saved back to it on Accept (the PC's value for a setting the profile has none of)
void EditLoad() {
    if (!profiles::IdValid(g_edit_profile)) return;
    auto values = Store().Values(g_edit_profile);
    for (const char* k : {"mouse_sensitivity", "mouse_zoom_sensitivity", "mouse_invert", "mouse_raw", "fov", "prompt_style"})
        if (!values.count(k))
            if (std::string v = pc::GetGlobal(k, ""); !v.empty()) values[k] = v;
    PcSetScreenValues(kEdit, values);
}

void EditSave() {
    if (!profiles::IdValid(g_edit_profile)) return;
    auto values = Store().Values(g_edit_profile);
    for (auto& [k, v] : PcScreenValues(kEdit)) values[k] = v;
    std::string error;
    if (Store().Save(g_edit_profile, values, error)) LOGI("party: profile %s's settings saved", g_edit_profile.c_str());
    else LOGW("party: profile %s's settings not saved: %s", g_edit_profile.c_str(), error.c_str());
    if (g_edit_profile == pc::ActiveProfile()) pc::UseProfile(g_edit_profile);  // player 1 plays with them at once
}

// ---- the title's Controller Options, a profile's (pad.*) ------------------------------------------------------------------
// The title keeps them in its own profile (its Xbox save, made fresh on each start's disk; players 2-4 have none): a
// process playing as a profile puts the profile's values into the title's screen of them and runs what its Accept
// runs, so the title applies them itself; a player's Accept on that screen saves them back to its profile. The two
// screens' rows by position (the front end's has no thumbstick and button configuration).
constexpr char kPadFront[] = "SuperLauncher_ControllerOptions";
constexpr char kPadGame[] = "InGame_ControllerOptions";
const char* const kPadFrontKeys[] = {"pad.invert", "pad.sensitivity", "pad.fast_look", "pad.left_hand", "pad.vibration", "pad.hints"};
const char* const kPadGameKeys[] = {"pad.invert", "pad.sensitivity", "pad.fast_look", "pad.left_hand", "pad.vibration",
                                    "pad.thumbstick", "pad.buttons", "pad.hints"};

std::vector<std::string> PadKeys(const std::string& screen) {
    if (screen == kPadFront) return {std::begin(kPadFrontKeys), std::end(kPadFrontKeys)};
    return {std::begin(kPadGameKeys), std::end(kPadGameKeys)};
}

void ApplyControllerOptions() {
    if (g_pad_applied || pc::ActiveProfile().empty()) return;
    for (const char* screen : {kPadGame, kPadFront}) {
        std::vector<int> rows = PcListRows(screen);
        std::vector<std::string> keys = PadKeys(screen);
        if (rows.size() < keys.size()) continue;  // not loaded (the other menu's)
        g_pad_applied = true;
        std::vector<int> picks(rows.size(), -1);
        int set = 0;
        for (size_t i = 0; i < keys.size(); i++)
            if (std::string v = pc::Get(keys[i], ""); !v.empty()) {
                picks[i] = std::atoi(v.c_str());
                set++;
            }
        if (!set) return;
        PcSetListRows(screen, picks);
        csx::QueueEngineCommand(screen == std::string(kPadGame) ? "stuffUItoPlayerData;SetCurrentControllerOptions\n"
                                                                 : "stuffUItoPlayerData\n");
        LOGI("party: the profile's controller options (%d) given to the title (%s)", set, screen);
        return;
    }
}

void CaptureControllerOptions(const std::string& screen) {
    std::vector<int> rows = PcListRows(screen);
    std::vector<std::string> keys = PadKeys(screen);
    if (rows.size() < keys.size()) return;
    for (size_t i = 0; i < keys.size(); i++)
        if (rows[i] >= 0) pc::Set(keys[i], std::to_string(rows[i]));
    pc::Save();
    LOGI("party: controller options saved%s", pc::ActiveProfile().empty() ? "" : " to the profile");
}

// a Controller Options screen's Accept also saves to the profile (the front end's and the pause menu's)
std::string CaptureOnAccept(const std::string& text, const char* screen, const char* accept) {
    size_t at = text.find(std::string("\n") + screen + "\r\n");
    size_t a = at == std::string::npos ? at : text.find(accept, at);
    if (a == std::string::npos) {
        LOGW("party: %s not as expected: its Accept does not save to the profile", screen);
        return text;
    }
    std::string with = accept;
    with.replace(with.find("xbui back"), 9, std::string("pc_party pad_capture ") + screen + "; xbui back");
    return text.substr(0, a) + with + text.substr(a + std::strlen(accept));
}

// split_name_2..4 (N4.6's Player Names) become profiles once, the first time the party screen opens; the settings
// stay as they are (nothing overwritten)
void Migrate() {
    if (pc::GetGlobal("profiles_migrated", "0") == "1") return;
    profiles::Store store = Store();
    std::set<std::string> have;
    for (auto& p : store.List()) have.insert(p.name);
    for (int n = 1; n <= 4; n++) {
        std::string name = pc::GetGlobal("split_name_" + std::to_string(n), "");
        if (name.empty() || !profiles::NameValid(name) || have.count(name)) continue;
        std::string error, id = store.Create(name, {}, error);
        if (!id.empty()) {
            have.insert(name);
            LOGI("party: profile %s made from split_name_%d", id.c_str(), n);
        }
    }
    pc::Set("profiles_migrated", "1");
    pc::Save();
}

}  // namespace

// the front end's party screens (added at load by pc_menu.cpp's PcUiLoadText)
std::string ExtendParty(const std::string& text) {
    // the main menu's Multiplayer row (System Link's, renamed by pc_browser.cpp) opens the party first; the server
    // browser is one of its destinations
    std::string out = text;
    const std::string row = "CommandSelect \"!SystemLinkSetup;xbui open SuperLauncher_SystemLink\"";
    size_t main = out.find("\nSuperLauncher_MainScreen\r\n");
    size_t at = main == std::string::npos ? main : out.find(row, main);
    if (at == std::string::npos) {
        LOGW("party: the main menu not laid out as expected: Multiplayer opens the server browser as before");
        return text;
    }
    out.replace(at, row.size(), "CommandSelect \"open " + std::string(kParty) + "\"");
    out = CaptureOnAccept(out, kPadFront, "\"!stuffUItoPlayerData; xbui back\"");
    return out + PartyScreenText() + ProfilesScreenText() + DeleteScreenText() +
           PcKeyboardScreen(kName, "Profile Name", "pc_party name_done", "Up to 20 characters.");
}

std::string PcLobbyPauseMenu(const std::string& text);  // pc_lobby.cpp

std::string PcPartyExtendInGame(const std::string& text) {
    return PcLobbyPauseMenu(CaptureOnAccept(text, kPadGame, "\"!stuffUItoPlayerData;SetCurrentControllerOptions;xbui back\""));
}

void PcPartyCommand(X86Context& ctx, const std::vector<std::string>& a) {
    std::string w1 = a.size() > 1 ? a[1] : "", w2 = a.size() > 2 ? a[2] : "";
    if (w1 == "enter") {
        // (the game alone in its window stays alone until another controller asks to join: PcPartyMenuFrame)
        Migrate();
        ReadState();
        // each member's profile: the one it plays as, else its slot's last one while it is free, else the only one
        std::vector<profiles::Info> list = Store().List();
        for (int n = 1; n <= 4; n++) {
            Member& m = g_m[n - 1];
            if (!m.in || !m.profile.empty()) continue;
            std::string id = n == 1 ? pc::ActiveProfile() : "";
            if (id.empty()) id = pc::GetGlobal("party.profile_" + std::to_string(n), "");
            if (id.empty() && n == 1 && list.size() == 1) id = list[0].id;
            if (!id.empty() && Store().Exists(id) && !Taken(id, n)) {
                m.profile = id;
                Tell(n);
                if (n == 1) UseOwnProfile();
            }
        }
        g_shown.clear();
        return ShowParty(ctx, true);
    }
    if (w1 == "frame") return ShowParty(ctx);
    if (w1 == "prof") {
        if (w2 == "show") return ShowProfiles(ctx);
        if (w2.rfind("delete_", 0) == 0) return DeleteInput(ctx, w2);
        return ProfilesInput(ctx, w2);
    }
    if (w1 == "pad_capture") return CaptureControllerOptions(w2);
    if (w1 == "name_done") return NameDone(ctx);
    if (w1 == "name_open") return NameOpen(ctx);
    if (w1 == "edit_load") return EditLoad();
    if (w1 == "edit_save") return EditSave();
    if (w1 == "1") return PanelInput(ctx, 1, w2);
}

// on the menu frame (pc_menu.cpp's PcMenuFrame): the compositor told whether a party screen is shown, the other
// members' input applied, the keyboard screen filled when it opens, a screen asked for at boot opened
void PcPartyMenuFrame(X86Context& ctx) {
    ApplyControllerOptions();
    std::string screen = PcCurrentScreen();
    bool on = PartyScreen(screen);
    // the game alone in its window: another controller's A or START (window_input.cpp) restarts it as the
    // compositor, back on the party screen, where that controller joins with A again
    csx::SetPartyScreenOpen(on && !csx::SplitPlayer());
    if (csx::TakePartyJoinRequest() && on && !csx::SplitPlayer()) {
#ifndef _WIN32
        setenv("CSX_FRONTEND_OPEN", kParty, 1);
#endif
        LOGI("party: another controller asks to join: split screen starts");
        if (!csx::SplitStart(1)) {
#ifndef _WIN32
            unsetenv("CSX_FRONTEND_OPEN");
#endif
        }
        return;
    }
    if (on != g_party_on && csx::SplitPlayer() == 1) {
        csx::SplitSend(csx::kSplitSetup, on ? "party on" : "party off");
        g_party_on = on;
    }
    static std::string last;
    if (screen != last) {
        if (screen == kName) NameOpen(ctx);
        if (screen == kParty && last == kProfiles) ShowParty(ctx, true);
        if (screen == kProfiles && (last == kName || last == kEdit || last == kDelete)) ShowProfiles(ctx);
        last = screen;
    }
    // player 1's own directions (or a member's with the menus) on these screens (a screen just opened starts on its
    // table's first row: not a press)
    static std::string probed;
    if (screen != kParty && screen != kProfiles) {
        probed.clear();  // not one of these screens: the title's own table is left alone
    } else if (screen != probed) {
        if (PcSelectedRow(screen) >= 0) {  // once its table is there
            probed = screen;
            PcProbeTake(screen);
        }
    } else if (screen == kParty) {
        for (const std::string& d : PcProbeTake(screen)) PanelInput(ctx, 1, d);
    } else if (screen == kProfiles) {
        for (const std::string& d : PcProbeTake(screen)) ProfilesInput(ctx, d);
    }
    for (const std::string& in : csx::TakePartyInput()) {
        int n = std::atoi(in.c_str());
        std::string what = in.substr(std::min(in.size(), in.find(' ') + 1));
        if (screen == kParty) PanelInput(ctx, n, what);
        else if (screen.rfind("SuperLauncher_PCLobby", 0) == 0) PcLobbyInput(ctx, n, what);
        else if (what == "joined" || what == "pad on" || what == "pad off") ReadState();
    }
    if (const char* open = std::getenv("CSX_FRONTEND_OPEN");
        open && *open && screen.rfind("SuperLauncher_MainScreen", 0) == 0) {
        LOGI("party: opening %s", open);
        csx::QueueEngineCommand(std::string("xbui open ") + open + "\n");
#ifndef _WIN32
        unsetenv("CSX_FRONTEND_OPEN");
#endif
    }
}

// ---- for the lobby (pc_lobby.cpp) -------------------------------------------------------------------------------------
std::vector<PcPartyMember> PcPartyMembers() {
    ReadState();
    std::vector<PcPartyMember> out(4);
    for (int i = 0; i < 4; i++) {
        out[size_t(i)] = {g_m[i].in, g_m[i].pad == "1", g_m[i].profile, ProfileName(g_m[i].profile), g_m[i].team, g_m[i].ready};
    }
    return out;
}

int PcPartyCount() {
    ReadState();
    return g_count;
}

int PcPartyFocus() {
    ReadState();
    return g_focus;
}

void PcPartySetMember(int n, int team, bool ready) {
    ReadState();
    if (n < 1 || n > 4 || !g_m[n - 1].in) return;
    g_m[n - 1].team = std::clamp(team, 0, 2);
    g_m[n - 1].ready = ready;
    Tell(n);
}

const char* PcPartyTeamName(int team) { return kTeams[std::clamp(team, 0, 2)]; }

std::string PcPartyPrompt(int n, const char* pad) {
    ReadState();
    return Prompt(n, pad);
}

// the name this process's player plays under in a party match: its profile's (player 1: the one in use; players 2-4:
// the compositor gives it as CSX_PLAYER_NAME), "" without one
std::string PcPartyProfileName() {
    std::string id = pc::ActiveProfile();
    return id.empty() ? std::string() : Store().Name(id);
}
