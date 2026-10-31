// tests/pc_options_text_test.py builds and runs this: src/game/pc_options_text.h on its own, on menu text written in
// the title's format (not the title's own text)
#include <cstdio>
#include <string>
#include <vector>

#include "game/pc_options_text.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

static std::string Row(const std::string& command, const std::string& label) {
    return "\t\tRow\r\n\t\t{\r\n\t\t\tCommandSelect \"" + command + "\"\r\n\t\t\tCellFixed \"" + label + "\"\r\n\t\t}\r\n";
}

static std::string Screen(const std::string& name, const std::string& rows, bool front_end) {
    std::string layout = front_end ? "\t\t\tTableRowSpacing \"12\"\r\n\t\t\tHighlightImage \"MainMenu_Highlight 1.0 1.0 1.0 1.0 "
                                     "-40.0 -1.0 240.0 38.0\"\r\n"
                                   : "\t\t\tTableSize \"640 300\"\r\n";
    return "\r\n" + name + "\r\n{\r\n\tBackgroundText \"x\"\t\"Options\"\r\n\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n" + layout +
           "\t\t}\r\n" + rows + "\t}\r\n}\r\n";
}

// the labels of a screen's rows, in order
static std::vector<std::string> Labels(const std::string& text, const std::string& name) {
    std::vector<std::string> out;
    size_t at = text.find("\n" + name + "\r\n{"), end = text.find("\n}", at + 1);
    for (size_t l = text.find("CellFixed \"", at); l != std::string::npos && l < end; l = text.find("CellFixed \"", l + 1)) {
        size_t q = l + 11, e = text.find('"', q);
        out.push_back(text.substr(q, e - q));
    }
    return out;
}

int main() {
    using pcmenu::OptionsRow;
    const std::string controller = "\r\nSuperLauncher_ControllerOptions\r\n{\r\n\tBackgroundText \"x\"\t\"Control Options\"\r\n}\r\n";
    std::string fe = Screen("SuperLauncher_Options",
                            Row("open SuperLauncher_ControllerOptions", "Control Options") +
                                Row("!pushSoundtracksToUI; xbui open SuperLauncher_Sound", "Sound Options") +
                                Row("open SuperLauncher_OnlineOptions", "Online Options") + Row("!PlayCredits", "View Credits"),
                            true) +
                     controller;
    std::vector<OptionsRow> pc;
    for (auto [id, label] : std::vector<std::pair<const char*, const char*>>{{"Video", "Video"},
                                                                             {"Mouse", "Mouse"},
                                                                             {"Interface", "Interface"},
                                                                             {"Controls", "Keybinds"},
                                                                             {"Multiplayer", "Multiplayer"},
                                                                             {"Mods", "Mods"}})
        pc.push_back({std::string("pc:") + id, Row(std::string("open SuperLauncher_PC") + id, label)});
    std::string out = pcmenu::ExtendOptions(fe, pc, true);
    std::vector<std::string> want = {"Video", "Sound Options", "Interface", "Mouse", "Controller",
                                     "Keybinds", "Multiplayer", "Mods", "View Credits"};
    Check(Labels(out, "SuperLauncher_Options") == want, "front end: the categories in order, no Online Options");
    Check(out.find("PCOptions") == std::string::npos && out.find("PC Options") == std::string::npos, "no PC Options");
    Check(out.find("TableRowSpacing \"4\"") != std::string::npos && out.find("240.0 30.0") != std::string::npos,
          "front end: nine rows fit above the help lines");
    Check(out.find("\"Controller\"\r\n}") != std::string::npos, "the Controller screen's own title");
    Check(out.find("xbui open SuperLauncher_Sound") != std::string::npos, "the title's Sound Options row kept as written");
    int keybinds = 0;
    for (size_t at = out.find("Keybinds"); at != std::string::npos; at = out.find("Keybinds", at + 1)) keybinds++;
    Check(keybinds == 1, "Keybinds once");

    std::string ig = Screen("Options",
                            Row("open InGame_ControllerOptions", "Control Options") + Row("open InGame_OnlineOptions", "Online Options"),
                            false);
    std::vector<OptionsRow> pc_ig;
    for (auto [id, label] : std::vector<std::pair<const char*, const char*>>{
             {"Video", "Video"}, {"Mouse", "Mouse"}, {"Sound", "Sound Options"}, {"Interface", "Interface"},
             {"Controls", "Keybinds"}, {"Multiplayer", "Multiplayer"}, {"Mods", "Mods"}})
        pc_ig.push_back({std::string("pc:") + id, Row(std::string("open InGame_PC") + id, label)});
    std::string out_ig = pcmenu::ExtendOptions(ig, pc_ig, false);
    std::vector<std::string> want_ig = {"Video", "Sound Options", "Interface", "Mouse", "Controller", "Keybinds", "Multiplayer", "Mods"};
    Check(Labels(out_ig, "Options") == want_ig, "pause menu: the categories in order, Sound Options of its own");
    Check(out_ig.find("TableSize \"640 300\"") != std::string::npos, "pause menu: its layout unchanged");

    Check(pcmenu::ExtendOptions("no options here", pc, true).empty(), "a menu not as expected: empty (left as it is)");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
