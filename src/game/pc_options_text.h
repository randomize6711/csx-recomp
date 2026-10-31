// The title's Options screen with the PC's categories among its own rows (src/game/pc_menu.cpp; tested on its own by
// tests/pc_options_text_test.py). Text in, text out: no game memory.
#pragma once
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pcmenu {

// the title's Options screen (front end SuperLauncher_Options, pause menu Options) with the PC's categories among its
// own rows, in OptionsOrder (since 2026-10-31 there is no PC Options: every category is a row here). The title's rows
// are kept as written (their commands, their help), Control Options named Controller as the other categories are;
// Online Options (Xbox Live's status and the Xbox voice: none on a PC) is left out. The front end's rows are closer
// together so that the nine fit above the help lines as the title's six did. Empty when the screen is not as expected
struct OptionsRow {
    std::string key;   // "pc:<id>" or "title:<what>"
    std::string text;  // the row as written
};

inline std::string OptionsKey(const std::string& row) {
    if (row.find("ControllerOptions\"") != std::string::npos) return "title:controller";
    if (row.find("SuperLauncher_Sound\"") != std::string::npos) return "title:sound";
    if (row.find("OnlineOptions\"") != std::string::npos) return "title:online";
    if (row.find("!PlayCredits") != std::string::npos) return "title:credits";
    return "title:other";
}

// the categories' order: the PC's and the title's (a screen not named here goes after them, before View Credits)
inline const char* const kOptionsOrder[] = {"pc:Video",    "title:sound",   "pc:Sound",          "pc:Interface", "pc:Mouse",
                                     "title:controller", "pc:Controls", "pc:Multiplayer", "pc:Mods"};

inline std::string ExtendOptions(const std::string& text, const std::vector<OptionsRow>& pc_rows, bool front_end) {
    size_t screen = text.find(front_end ? "\nSuperLauncher_Options\r\n{" : "\nOptions\r\n{");
    size_t end = screen == std::string::npos ? screen : text.find("\n}", screen + 1);
    size_t table = screen == std::string::npos ? screen : text.find("\n\tTable\r\n", screen);
    size_t layout_end = table == std::string::npos ? table : text.find("\n\t\t}\r\n", table);
    if (layout_end == std::string::npos || layout_end > end) return {};
    size_t first = text.find("\n\t\tRow\r\n", layout_end);
    size_t table_end = text.find("\n\t}", layout_end);
    if (first == std::string::npos || table_end == std::string::npos || first > table_end || table_end > end) return {};
    std::vector<OptionsRow> rows;
    for (size_t at = first + 1; at < table_end + 1;) {
        size_t next = text.find("\n\t\tRow\r\n", at);
        size_t stop = next == std::string::npos || next > table_end ? table_end + 1 : next + 1;
        std::string row = text.substr(at, stop - at);
        std::string key = OptionsKey(row);
        if (key == "title:controller")
            for (const char* label : {"CellFixed \"Control Options\""})
                if (size_t l = row.find(label); l != std::string::npos) row.replace(l, std::strlen(label), "CellFixed \"Controller\"");
        rows.push_back({key, row});
        at = stop;
    }
    std::vector<OptionsRow> all = rows;
    all.insert(all.end(), pc_rows.begin(), pc_rows.end());
    std::string out;
    std::vector<bool> used(all.size(), false);
    auto take = [&](const std::string& key) {
        for (size_t i = 0; i < all.size(); i++)
            if (!used[i] && all[i].key == key) {
                used[i] = true;
                out += all[i].text;
            }
    };
    for (const char* key : kOptionsOrder) take(key);
    for (size_t i = 0; i < all.size(); i++)
        if (!used[i] && all[i].key != "title:credits" && all[i].key != "title:online") {
            used[i] = true;
            out += all[i].text;
        }
    take("title:credits");
    std::string head = text.substr(0, first + 1);
    if (front_end) {  // 28 units a row (the title's 36): nine rows from 98 end at 350, above the help lines at 361
        for (auto [from, to] : {std::pair<const char*, const char*>{"TableRowSpacing \"12\"", "TableRowSpacing \"4\""},
                                {"MainMenu_Highlight 1.0 1.0 1.0 1.0 -40.0 -1.0 240.0 38.0",
                                 "MainMenu_Highlight 1.0 1.0 1.0 1.0 -40.0 -1.0 240.0 30.0"}}) {
            size_t at = head.find(from, table);
            if (at != std::string::npos && at < layout_end) head.replace(at, std::strlen(from), to);
        }
    }
    std::string tail = text.substr(table_end + 1);
    // the Controller screen's own title as its row's name
    const std::string ctrl = front_end ? "\nSuperLauncher_ControllerOptions\r\n{" : "\nInGame_ControllerOptions\r\n{";
    if (size_t c = tail.find(ctrl); c != std::string::npos) {
        size_t title = tail.find("\"Control Options\"", c);
        size_t c_end = tail.find("\n}", c + 1);
        if (title != std::string::npos && title < c_end) tail.replace(title, std::strlen("\"Control Options\""), "\"Controller\"");
    } else if (size_t c2 = head.find(ctrl); c2 != std::string::npos) {
        size_t title = head.find("\"Control Options\"", c2);
        size_t c_end = head.find("\n}", c2 + 1);
        if (title != std::string::npos && title < c_end) head.replace(title, std::strlen("\"Control Options\""), "\"Controller\"");
    }
    return head + out + tail;
}

}  // namespace pcmenu
