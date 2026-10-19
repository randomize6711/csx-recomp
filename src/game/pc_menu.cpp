// PC options inside the game's own menus (FINDINGS.md "Original menus and PC Options"). The game's screens are text:
// the engine loads FrontEndUI*.txt (front end) and InGameUI*.txt (pause menu) whole (0xCD9A0: size at 0xCDA2C, read,
// then 0xCDB1A) and parses screens of tables, rows and cells (CellFixed labels, CellList choices) drawn and navigated
// by the game's own UI code with its fonts, images and sounds. The hooks here add, in memory, a "PC Options" row to
// each Options screen and a screen per menu written in that same format, laid out as that menu's Control Options
// screen (SuperLauncher_PCOptions, InGame_PCOptions). The screens' commands are console commands ("!name" in the
// text, as the game's own screens call theirs): pc_options_load <screen> on entry fills the lists from
// pc_settings.ini, A runs pc_options_save <screen>, B leaves without saving. A hook in Cmd_ExecuteString serves them
// before the engine's own command lookup.
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {

using kernel::R32;

struct Choice {
    const char* label;
    const char* value;
};
struct Setting {
    const char* key;    // pc_settings.ini name
    const char* label;  // row text
    const char* help[2];
    std::vector<Choice> choices;
    int def;  // index of the default choice
};

// one row per setting, in screen order
const std::vector<Setting>& Settings() {
    static const std::vector<Setting> s = {
        {"mouse_sensitivity",
         "Mouse Sensitivity",
         {"Set how far the view turns when you move the mouse.", "3 matches the game's original setting."},
         {{"0.5", "0.5"}, {"1", "1"}, {"1.5", "1.5"}, {"2", "2"}, {"2.5", "2.5"}, {"3", "3"}, {"3.5", "3.5"},
          {"4", "4"}, {"5", "5"}, {"6", "6"}, {"7", "7"}, {"8", "8"}, {"10", "10"}},
         5},
        {"mouse_invert",
         "Invert Mouse",
         {"Turn on this option to reverse the vertical look", "of the mouse."},
         {{"Off", "0"}, {"On", "1"}},
         0},
        {"display_mode",
         "Display Mode",
         {"Play in a window or on the full screen.", "F11 also switches between them."},
         {{"Windowed", "windowed"}, {"Fullscreen", "fullscreen"}},
         0},
        {"master_volume",
         "Master Volume",
         {"Set the volume of everything the game plays.", "Sound Options set its parts."},
         {{"0", "0"}, {"10", "10"}, {"20", "20"}, {"30", "30"}, {"40", "40"}, {"50", "50"}, {"60", "60"},
          {"70", "70"}, {"80", "80"}, {"90", "90"}, {"100", "100"}},
         10},
    };
    return s;
}

constexpr const char* kScreen = "SuperLauncher_PCOptions";  // the front end's (FrontEndUI.txt)
constexpr const char* kInGameScreen = "InGame_PCOptions";    // the pause menu's (InGameUI.txt)

std::string Quote(const std::string& s) { return "\"" + s + "\""; }

// the help text block every row of the game's option screens carries (shown at the bottom while it is highlighted)
std::string HelpTable(const char* const* lines, int n, const char* indent) {
    std::string t = std::string(indent) + "Table\r\n" + indent + "{\r\n";
    t += std::string(indent) + "\tLayout\r\n" + indent + "\t{\r\n";
    for (const char* l : {"ColumnWidthPercentage \"100\"", "TableLocation \"75 361\"", "TableSize \"476 40\"",
                          "TableRowSpacing \"1\"", "TableTextScale \"0.75\"", "NonSelectFG \"0.24 0.29 0.33 1\""})
        t += std::string(indent) + "\t\t" + l + "\r\n";
    t += std::string(indent) + "\t}\r\n";
    for (int i = 0; i < n; i++)
        if (lines[i] && *lines[i])
            t += std::string(indent) + "\tRow\r\n" + indent + "\t{\r\n" + indent + "\t\tCellFixed " + Quote(lines[i]) +
                 "\r\n" + indent + "\t}\r\n";
    return t + indent + "}\r\n";
}

// the row added to SuperLauncher_Options, as its other rows are written
std::string OptionsRow() {
    const char* help[2] = {"Set mouse and other options for playing", "on a PC."};
    return "\t\tRow\r\n\t\t{\r\n" + HelpTable(help, 2, "\t\t\t") + "\t\t\tCommandSelect \"open " + kScreen +
           "\"\r\n\t\t\tCellFixed \"PC Options\"\r\n\t\t}\r\n";
}

// SuperLauncher_PCOptions: the game's Control Options screen (backgrounds, sounds, arrows, layout) with these rows
std::string Screen() {
    std::string t = std::string("\r\n") + kScreen + "\r\n{\r\n";
    t += "\tMenuType \"superlauncher\"\r\n"
         "\tEnterSound \"menu/buy_submenu.wav\"\r\n"
         "\tHighlightSound \"menu/buy_hover.wav\"\r\n"
         "\tPieceOffsetDegrees \"0\"\r\n"
         "\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n"
         "\tBackgroundImage \"Back_2ndary.1Pane\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
         "\tBackgroundImage \"Cancel_Accept 200 200 200 255 0 411 640 69\"\r\n"
         "\tOnEntry \"!pc_options_load " + std::string(kScreen) + "\"\r\n"
         "\tOnButtonDownB \"back\"\r\n"
         "\tOnButtonDownA \"!pc_options_save " + kScreen + "; xbui back\"\r\n"
         "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"PC Options\"\r\n"
         "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableIncreaseValue\"\r\n\t}\r\n"
         "\tGeneric_4_2\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n"
         "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableDecreaseValue\"\r\n\t}\r\n"
         "\tGeneric_4_4\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n"
         "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
         "\t\t\tColumnWidthPercentage \"60\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
         "\t\t\tColumnWidthPercentage \"40\"\r\n\t\t\tColumnJustify \"center\"\r\n"
         "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
         "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"500 400\"\r\n"
         "\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
         "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 299.0 2.0 176.0 22.0\"\r\n"
         "\t\t\tNonHighlightImage \"Arrows_Unselected 1.0 1.0 1.0 1.0 299.0 2.0 176.0 22.0\"\r\n"
         "\t\t}\r\n";
    for (const Setting& s : Settings()) {
        t += "\t\tRow\r\n\t\t{\r\n" + HelpTable(s.help, 2, "\t\t\t");
        t += "\t\t\tCellFixed " + Quote(s.label) + "\r\n\t\t\tCellList\r\n\t\t\t{\r\n";
        for (size_t i = 0; i < s.choices.size(); i++)
            t += std::string("\t\t\t\t") + (int(i) == s.def ? "ListDefault " : "ListOption ") +
                 Quote(s.choices[i].label) + "\t\t" + Quote(s.choices[i].value) + "\r\n";
        t += "\t\t\t}\r\n\t\t}\r\n";
    }
    return t + "\t}\r\n}\r\n";
}

// InGame_PCOptions: the pause menu's Control Options screen (its overlay, colours, sounds, arrows) with these rows;
// the in-game option screens carry no help lines
std::string InGameScreen() {
    std::string t = std::string("\r\n") + kInGameScreen + "\r\n{\r\n";
    t += "\tMenuType \"superlauncher\"\r\n"
         "\tEnterSound \"menu/buy_enter.wav\"\r\n"
         "\tSelectSound \"menu/buy_submenu.wav\"\r\n"
         "\tHighlightSound \"menu/buy_hover.wav\"\r\n"
         "\tPieceOffsetDegrees \"0\"\r\n"
         "\tCachedImage \"angleSelect\t\t\t\t255 255 255 255\"\r\n"
         "\tBackgroundImage \"csxbackground\t\t100 100 80 235\t\t0 0 640 480\"\r\n"
         "\tModal \"50\"\r\n"
         "\tBackgroundText \"177 169 54 255 65 45 400 30\"\t\"PC Options\"\r\n"
         "\tOnButtonDownB \"back\"\r\n"
         "\tOnButtonDownA \"!pc_options_save " + std::string(kInGameScreen) + "; xbui back\"\r\n"
         "\tOnEntry \"!pc_options_load " + kInGameScreen + "\"\r\n"
         "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableIncreaseValue\"\r\n\t}\r\n"
         "\tGeneric_4_2\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n"
         "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableDecreaseValue\"\r\n\t}\r\n"
         "\tGeneric_4_4\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n"
         "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
         "\t\t\tColumnWidthPercentage \"60\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
         "\t\t\tColumnWidthPercentage \"40\"\r\n\t\t\tColumnJustify \"center\"\r\n"
         "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.875\"\r\n\t\t\tTableLocation \"75 100\"\r\n"
         "\t\t\tTableRowSpacing \"5\"\r\n\t\t\tTableSize \"475 400\"\r\n"
         "\t\t\tHighlightImage \"Arrows_Selected 1.0 1.0 1.0 1.0 257.0 2.0 218.0 22.0\"\r\n"
         "\t\t}\r\n";
    for (const Setting& s : Settings()) {
        t += "\t\tRow\r\n\t\t{\r\n\t\t\tCellFixed " + Quote(s.label) + "\r\n\t\t\tCellList\r\n\t\t\t{\r\n";
        for (size_t i = 0; i < s.choices.size(); i++)
            t += std::string("\t\t\t\t") + (int(i) == s.def ? "ListDefault " : "ListOption ") +
                 Quote(s.choices[i].label) + "\t\t" + Quote(s.choices[i].value) + "\r\n";
        t += "\t\t\t}\r\n\t\t}\r\n";
    }
    return t + "\t}\r\n}\r\n";
}

// the pause menu's text: a row in its Options screen after Online Options (written as its rows are: centred, a blank
// cell either side) and the screen; empty when its layout is not the one expected
std::string ExtendInGame(const std::string& text) {
    size_t screen = text.find("\nOptions\r\n{");
    size_t online = screen == std::string::npos ? screen : text.find("\"open InGame_OnlineOptions\"", screen);
    size_t end = online == std::string::npos ? online : text.find("\n\t\t}\r\n", online);
    if (end == std::string::npos) return {};
    end += std::strlen("\n\t\t}\r\n");
    std::string row = std::string("\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect   \"open ") + kInGameScreen +
                      "\"\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed \"PC Options\"\r\n\t\t\tCellFixed \" \"\r\n\t\t}\r\n";
    return text.substr(0, end) + row + text.substr(end) + InGameScreen();
}

// the front end text with the row and the screen added; empty when its layout is not the one expected
std::string Extend(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_Options");
    size_t credits = screen == std::string::npos ? screen : text.find("CommandSelect \"!PlayCredits\"", screen);
    // the credits row's own line (two tabs; the rows of its help table have four)
    size_t row = credits == std::string::npos ? credits : text.rfind("\n\t\tRow\r\n", credits);
    if (row == std::string::npos || row < screen) return {};
    row += 1;
    return text.substr(0, row) + OptionsRow() + text.substr(row) + Screen();
}

constexpr uint32_t kExtra = 64 * 1024;  // room added to the load buffer
uint32_t g_pending_size = 0;            // the UI file's own size while its load is under way, 0 otherwise

std::string GuestString(uint32_t va) {
    std::string s;
    for (int i = 0; va && i < 512; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}

// the game's UI objects (stuffUItoPlayerData 0xDAD40 and the selection writer at 0xD7920 read and write them so):
// screens in a list from 0x5523D0 (name pointer at +0, next at +0x378, table at +0x380), a table's rows at +0x60..+0x64
// (32 bytes each), a row's cells at +0xC..+0x10 (0x78 bytes each), a list cell's current choice at +0 and its
// choices at +0xC..+0x10 (0x1C bytes each)
uint32_t FindTable(const char* screen) {
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

void LoadOptions(const std::string& screen) {
    uint32_t table = FindTable(screen.c_str());
    if (!table) return LOGW("pc menu: no %s screen", screen.c_str());
    for (size_t i = 0; i < Settings().size(); i++) {
        const Setting& s = Settings()[i];
        uint32_t cell = ListCell(table, i);
        if (!cell || Choices(cell) != s.choices.size()) {
            LOGW("pc menu: row %zu is not the list expected", i);
            continue;
        }
        std::string v = pc::Get(s.key, s.choices[size_t(s.def)].value);
        uint32_t pick = uint32_t(s.def);
        for (size_t c = 0; c < s.choices.size(); c++)
            if (v == s.choices[c].value) pick = uint32_t(c);
        kernel::W32(cell, pick);
    }
}

void SaveOptions(const std::string& screen) {
    uint32_t table = FindTable(screen.c_str());
    if (!table) return LOGW("pc menu: no %s screen", screen.c_str());
    for (size_t i = 0; i < Settings().size(); i++) {
        const Setting& s = Settings()[i];
        uint32_t cell = ListCell(table, i), pick = cell ? R32(cell) : 0;
        if (cell && Choices(cell) == s.choices.size() && pick < s.choices.size()) pc::Set(s.key, s.choices[pick].value);
    }
    pc::Save();
}

}  // namespace

// [[hook]] at 0xCDA2E (config/game.toml): the file's size was just taken into ebp, the name is at [esp+0x10]; a menu
// file (FrontEndUI*.txt, InGameUI*.txt) gets room for the additions (the buffer is size + 1, the read stops at the file's end)
void PcUiLoadSize(X86Context& ctx, uint8_t*) {
    g_pending_size = 0;
    std::string name = GuestString(R32(ctx.esp + 0x10));
    if (name.rfind("FrontEndUI", 0) != 0 && name.rfind("InGameUI", 0) != 0) return;
    g_pending_size = ctx.ebp;
    ctx.ebp += kExtra;
}

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
    std::string out = name.rfind("FrontEndUI", 0) == 0 ? Extend(text) : ExtendInGame(text);
    if (out.empty() || out.size() > room) {
        LOGW("pc menu: %s left as it is (%s)", name.c_str(), out.empty() ? "layout not recognised" : "no room");
        out = text;
    } else {
        LOGI("pc menu: PC Options added to %s", name.c_str());
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
    std::string screen = R32(0x4253E8) > 1 ? GuestString(R32(0x4251AC)) : std::string(kScreen);  // argument 1
    if (cmd == "pc_options_load") LoadOptions(screen);
    else if (cmd == "pc_options_save") SaveOptions(screen);
    else return false;
    return true;
}
