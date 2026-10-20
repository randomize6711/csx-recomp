// PC options inside the game's own menus (FINDINGS.md "Original menus and PC Options"). The game's screens are text:
// the engine loads FrontEndUI*.txt (front end) and InGameUI*.txt (pause menu) whole (0xCD9A0: size at 0xCDA2C, read,
// then 0xCDB1A) and parses screens of tables, rows and cells (CellFixed labels, CellList choices) drawn and navigated
// by the game's own UI code with its fonts, images and sounds. The PC screens are declared once below (Screens():
// their rows of settings); from that the hooks add, in memory, a row per screen to each menu's Options screen and the
// screens themselves written in that menu's own format, laid out as its Control Options screen. The screens' commands
// are console commands ("!name" in the text, as the game's own screens call theirs): pc_options_load <screen> on
// entry fills the lists from pc_settings.ini, A runs pc_options_save <screen>, X (where offered) pc_options_defaults
// <screen>, B leaves without saving. A hook in Cmd_ExecuteString serves them before the engine's own command lookup.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/pc_bindings.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

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
};

std::vector<Choice> Numbers(std::initializer_list<const char*> v) {
    std::vector<Choice> c;
    for (const char* s : v) c.push_back({s, s});
    return c;
}

Setting Binding(const pc::Action& a) {
    Setting s{std::string("bind.") + a.id, a.label, {"Choose the key or mouse button for this action.",
                                                       "Menus keep their own keys; Escape opens the game menu."}};
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

std::string Commands(const std::string& name, bool resettable) {
    std::string t = "\tOnEntry \"!pc_options_load " + name + "\"\r\n\tOnButtonDownB \"back\"\r\n"
                    "\tOnButtonDownA \"!pc_options_save " + name + "; xbui back\"\r\n";
    if (resettable) t += "\tOnButtonDownX \"!pc_options_defaults " + name + "\"\r\n";
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
           " 200 200 200 255 0 411 640 69\"\r\n" + Commands(name, s.resettable) +
           "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t" + Quote(s.title) + "\r\n" + Pie() +
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           "\t\t\tColumnWidthPercentage \"60\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
           "\t\t\tColumnWidthPercentage \"40\"\r\n\t\t\tColumnJustify \"center\"\r\n"
           "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
           "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"500 250\"\r\n"
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
           Commands(name, s.resettable) + Pie() +
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           "\t\t\tColumnWidthPercentage \"60\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
           "\t\t\tColumnWidthPercentage \"40\"\r\n\t\t\tColumnJustify \"center\"\r\n"
           "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
           "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"475 300\"\r\n"
           "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 257.0 2.0 218.0 22.0\"\r\n"
           "\t\t}\r\n" + ListRows(s, false) + "\t}\r\n}\r\n";
}

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
        rows += "\t\tRow\r\n\t\t{\r\n" + HelpTable(help, 2, "\t\t\t") + "\t\t\tCommandSelect \"open " +
                FrontEndName(s) + "\"\r\n\t\t\tCellFixed " + Quote(s.title) + "\r\n\t\t}\r\n";
        screens += FrontEndScreen(s);
    }
    return text.substr(0, row) + rows + text.substr(row) + screens;
}

// the pause menu's text: after Online Options in its Options screen a row per screen (centred between blank cells as
// its rows are), then the screens; empty when its layout is not the one expected
std::string ExtendInGame(const std::string& text) {
    size_t screen = text.find("\nOptions\r\n{");
    size_t online = screen == std::string::npos ? screen : text.find("\"open InGame_OnlineOptions\"", screen);
    size_t end = online == std::string::npos ? online : text.find("\n\t\t}\r\n", online);
    if (end == std::string::npos) return {};
    end += std::strlen("\n\t\t}\r\n");
    std::string rows, screens;
    for (const PcScreen& s : Screens()) {
        rows += "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect   \"open " + InGameName(s) +
                "\"\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed " + Quote(s.title) +
                "\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n";
        screens += InGameScreen(s);
    }
    return text.substr(0, end) + rows + text.substr(end) + screens;
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
                LOGI("pc menu: %s is already %s's, %s is left unbound", r.choices[pick].label.c_str(),
                     s.settings[it->second].label.c_str(), r.label.c_str());
                value.clear();
                kernel::W32(cells[i], uint32_t(IndexOf(r, "")));
            }
        }
        pc::Set(r.key, value);
    }
    pc::Save();
}

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
    std::string out = name.rfind("FrontEndUI", 0) == 0 ? ExtendFrontEnd(text) : ExtendInGame(text);
    PcMouseLearnUi(out.empty() ? text : out);
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

// [[hook]] at 0x3CA25 in Cmd_ExecuteString, the command just tokenized (cmd_argc 0x4253E8, cmd_argv 0x4251A8): true
// serves a pc_ command here and leaves the function
bool PcConsoleCommand(X86Context&, uint8_t*) {
    if (!R32(0x4253E8)) return false;
    std::string cmd = GuestString(R32(0x4251A8));
    if (cmd != "pc_options_load" && cmd != "pc_options_save" && cmd != "pc_options_defaults") return false;
    std::string name = R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string();  // argument 1: the screen
    const PcScreen* s = FindScreen(name);
    if (!s) LOGW("pc menu: %s for an unknown screen %s", cmd.c_str(), name.c_str());
    else if (cmd == "pc_options_load") LoadScreen(name, *s);
    else if (cmd == "pc_options_save") SaveScreen(name, *s);
    else DefaultScreen(name, *s);
    return true;
}
