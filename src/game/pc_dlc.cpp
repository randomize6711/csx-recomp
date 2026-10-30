// Maps the title's lists leave out (N6A, N6B; docs/n6_dlc.md, docs/n6_maps.md):
//   - the official downloadable maps, Inferno (de_inferno) and Office (cs_office). Microsoft's Xbox Live packages for
//     them hold no map: they are unlock tokens (contentmeta "Inferno Map", "Office Map"); the maps, their previews
//     (launcher.xpr's de_inferno / cs_office) and loading screens are on the game disc. The title lists them only
//     when the packages are installed (its Custom Game map list and the pause menu's Change Map list hold 18 maps
//     without them); here they go in the title's alphabetical places, with their previews;
//   - imported maps (tools/import_map.py: an archive and a report in the game folder the game runs from, its overlay,
//     common/map_imports.cpp, which links only maps whose report says they are playable): at the lists' ends, before
//     Random, with the random map's preview (an imported map has none of its own), named from their file names.
// Each row is made from a row of the same kind already there (Fastline's for a bomb map, Havana's for a hostage map:
// the kind's label, the layout, the preview's place). The party's map list (pc_lobby.cpp) reads the title's, so it
// shows them too; the map search (pc_mapsearch.cpp) reads it after this.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"

namespace {

struct Map {
    std::string value, label, like, after, preview;  // after "": the list's end (before Random); preview "": the map's
};

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// the game folder's maps (lower case) and the imported ones' reports
std::vector<std::string> GameFiles() {
    std::vector<std::string> out;
    const char* game = std::getenv("GAME_DIR");
    std::error_code ec;
    if (game)
        for (const auto& e : std::filesystem::directory_iterator(game, ec)) out.push_back(Lower(e.path().filename().string()));
    return out;
}

bool Has(const std::vector<std::string>& files, const std::string& name) {
    return std::find(files.begin(), files.end(), Lower(name)) != files.end();
}

std::vector<Map> Maps() {
    std::vector<Map> out;
    std::vector<std::string> files = GameFiles();
    if (Has(files, "de_inferno.sxwad")) out.push_back({"de_inferno", "Inferno", "Fastline", "Havana", ""});
    if (Has(files, "cs_office.sxwad")) out.push_back({"cs_office", "Office", "Havana", "Militia", ""});
    const char* game = std::getenv("GAME_DIR");
    for (const std::string& f : files) {
        if (f.size() < 6 || f.compare(f.size() - 5, 5, ".json") != 0) continue;
        std::string name = f.substr(0, f.size() - 5);
        if (!Has(files, name + ".sxwad")) continue;
        std::ifstream in(std::filesystem::path(game) / f);
        std::string report((std::istreambuf_iterator<char>(in)), {});
        if (report.find("\"playable\": true") == std::string::npos) continue;
        bool hostage = report.find("\"kind\": \"hostage\"") != std::string::npos;
        // "de_sienna_cz" -> "Sienna CZ"
        std::string label = name.size() > 3 && name[2] == '_' ? name.substr(3) : name;
        for (char& c : label) c = c == '_' ? ' ' : c;
        if (label.size() >= 3 && label.compare(label.size() - 3, 3, " cz") == 0) label.replace(label.size() - 2, 2, "CZ");
        if (!label.empty()) label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
        out.push_back({name, label, hostage ? "Havana" : "Fastline", "", "random_map"});
    }
    return out;
}

// a screen's Row block holding needle: [start, end) (from its "\t\tRow" line to after its closing "\t\t}" line and
// its line break)
bool RowAround(const std::string& text, size_t screen, size_t end, const std::string& needle, size_t& a, size_t& b) {
    size_t at = text.find(needle, screen);
    if (at == std::string::npos || at > end) return false;
    a = text.rfind("\n\t\tRow\r\n", at);
    b = text.find("\r\n\t\t}\r\n", at);
    if (a == std::string::npos || a < screen || b == std::string::npos || b > end) return false;
    a += 1;
    b += 8;  // past the row's "\t\t}\r\n": a row inserted there keeps this one's end whole for the next search
    return true;
}

void Replace(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

// a list screen with the maps' rows: key(label) finds a row by its shown name; the last row (Random's in Custom Game,
// the last map's in Change Map) is where a map with no place of its own goes before (Custom Game) or after
std::string AddRows(const std::string& text, const char* screen_name, std::string (*key)(const std::string&),
                    bool before_last, const std::vector<Map>& maps, int& added) {
    std::string out = text;
    size_t screen = out.find(std::string("\n") + screen_name + "\r\n");
    if (screen == std::string::npos) return out;
    for (const Map& m : maps) {
        size_t end = out.find("\n}", screen + 1);
        if (out.find("\"" + m.value + "\"", screen) < end || out.find(" " + m.value + ";", screen) < end) continue;
        size_t a, b, c, d;
        if (!RowAround(out, screen, end, key(m.like), a, b)) continue;
        size_t at;
        if (!m.after.empty()) {
            if (!RowAround(out, screen, end, key(m.after), c, d)) continue;
            at = d;
        } else {  // the end of the list: its last Row block
            size_t last = out.rfind("\n\t\tRow\r\n", end);
            if (last == std::string::npos || last < screen) continue;
            if (before_last) {
                at = last + 1;
            } else {
                size_t close = out.find("\r\n\t\t}\r\n", last);
                if (close == std::string::npos || close > end) continue;
                at = close + 8;
            }
        }
        std::string row = out.substr(a, b - a);
        std::string like_value = m.like == "Fastline" ? "de_fastline" : "cs_havana";
        Replace(row, "\"" + m.like + "\"", "\"" + m.label + "\"");
        if (!m.preview.empty()) Replace(row, "HighlightImage \"" + like_value + " ", "HighlightImage \"" + m.preview + " ");
        Replace(row, like_value, m.value);
        out.insert(at, row);
        added++;
    }
    return out;
}

std::string TitleRow(const std::string& label) { return "ListDefault \"" + label + "\""; }  // Custom Game's list
std::string ChangeMapRow(const std::string& label) { return "CellFixed \"" + label + "\""; }  // the pause menu's

}  // namespace

// the front end's text (Custom Game's map list) and the pause menu's (Change Map), the maps added
std::string ExtendDlcMaps(const std::string& text) {
    std::vector<Map> maps = Maps();
    if (maps.empty()) return text;
    int added = 0;
    std::string out = AddRows(text, "SuperLauncher_SinglePlayerMatchMapList", TitleRow, true, maps, added);
    out = AddRows(out, "ChangeMaps", ChangeMapRow, false, maps, added);
    std::string names;
    for (const Map& m : maps) names += (names.empty() ? "" : ", ") + m.label;
    if (added) LOGI("pc maps: listed %s (%d rows: the official downloadable maps on the disc, imported maps)", names.c_str(), added);
    return out;
}
