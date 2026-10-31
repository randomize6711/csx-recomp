// The title's Options screen with the PC's categories among its own rows (src/game/pc_menu.cpp; tested on its own by
// tests/pc_options_text_test.py). Text in, text out: no game memory.
#pragma once
#include <algorithm>
#include <cstring>
#include <initializer_list>
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

// The main menu redesign (2026-10-31, docs/menus.md "Main menu"): Instant Action, Multiplayer, Options, Extras (and
// Exit Game, added after by pc_menu.cpp's ExtendExit), in the title's own rows and style:
// - Instant Action: Single Player's row (its help), opening the Instant Action hub (pc_instant.cpp);
// - Multiplayer: Online Play's row (pc_browser.cpp: the title's System Link screen, the PC's online menu);
// - Options: the title's own;
// - Extras: a hub of the title's style (HubScreen) holding Training's and Change Profile's own rows, as they were,
//   and Mods (the PC's Mods screen, not a row of Options since 2026-10-31).
// Single Player's and the party's (Multiplayer, N4.8) rows leave the main menu: Instant Action opens the same match
// setup, the party is the online menu's Split-Screen and Host Game. Returns the text with the main menu laid out and
// the Extras screen added; empty when the menu is not as expected
inline constexpr char kInstantScreen[] = "SuperLauncher_PCInstantAction";
inline constexpr char kExtrasScreen[] = "SuperLauncher_PCExtras";

// a row of a menu table (two tabs deep) holding marker, with its help table: [start, end) in text, or npos
inline std::pair<size_t, size_t> MenuRowOf(const std::string& text, size_t screen, size_t end, const std::string& marker) {
    size_t at = screen == std::string::npos ? screen : text.find(marker, screen);
    if (at == std::string::npos || at > end) return {std::string::npos, std::string::npos};
    size_t start = text.rfind("\n\t\tRow\r\n", at), stop = text.find("\r\n\t\t}\r\n", at);
    if (start == std::string::npos || start < screen || stop == std::string::npos || stop > end)
        return {std::string::npos, std::string::npos};
    return {start + 1, stop + std::strlen("\r\n\t\t}\r\n")};
}

// a row's label, command and help lines rewritten (the help table's lines in order; empty keeps a line)
inline std::string Relabel(std::string row, const std::string& label, const std::string& command,
                           const std::vector<std::string>& help) {
    const size_t cmd = row.find("\t\t\tCommandSelect ");
    if (cmd == std::string::npos) return {};
    size_t at = 0;
    for (const std::string& h : help) {
        at = row.find("CellFixed \"", at);
        if (at == std::string::npos || at > row.find("\t\t\tCommandSelect ")) break;
        size_t e = row.find("\r\n", at);
        if (!h.empty()) row.replace(at, e - at, "CellFixed \"" + h + "\"");
        at = row.find("\r\n", at);
    }
    size_t c = row.find("\t\t\tCommandSelect "), ce = row.find("\r\n", c);
    if (!command.empty()) row.replace(c, ce - c, "\t\t\tCommandSelect \"" + command + "\"");
    size_t l = row.rfind("\t\t\tCellFixed \""), le = row.find("\r\n", l);
    if (l == std::string::npos || l < c) return {};
    row.replace(l, le - l, "\t\t\tCellFixed \"" + label + "\"");
    return row;
}

// a hub of the title's own front end style (its main menu's look: background, highlight, sounds, help lines under
// the list), with rows as written; extra lines (BackgroundImage / BackgroundText) after the title's line, an OnEntry
inline std::string HubScreen(const std::string& name, const std::string& title, const std::string& rows,
                             const std::string& extra = "", const std::string& on_entry = "") {
    return "\r\n" + name + "\r\n{\r\n"
           "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n"
           "\tHighlightSound \"menu/buy_hover.wav\"\r\n\tPieceOffsetDegrees \"90\"\r\n"
           "\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n\tCommandSelect \"RowSelect\"\r\n"
           "\tBackgroundImage \"Back_MainScreen\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
           "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n" +
           (on_entry.empty() ? std::string() : "\tOnEntry \"" + on_entry + "\"\r\n") +
           "\tOnButtonDownB \"back\"\r\n"
           "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t\"" + title + "\"\r\n" + extra +
           "\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n\t}\r\n"
           "\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n\t}\r\n"
           "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
           "\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\tTableVerticalWrap\r\n\t\t\tTableLocation \"80 98\"\r\n"
           "\t\t\tTableSize \"236 252\"\r\n\t\t\tTableRowSpacing \"12\"\r\n\t\t\tNonSelectBG \"0 0 0 0\"\r\n"
           "\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n"
           "\t\t\tHighlightImage \"MainMenu_Highlight 1.0 1.0 1.0 1.0 -40.0 -1.0 240.0 38.0\"\r\n"
           "\t\t}\r\n" + rows + "\t}\r\n}\r\n";
}

// a hub row: its two help lines (under the list, as the title's rows have them), its command, its label
inline std::string HubRow(const std::string& label, const std::string& help1, const std::string& help2,
                          const std::string& command) {
    std::string t = "\t\tRow\r\n\t\t{\r\n\t\t\tTable\r\n\t\t\t{\r\n\t\t\t\tLayout\r\n\t\t\t\t{\r\n"
                    "\t\t\t\t\tColumnWidthPercentage \"100\"\r\n\t\t\t\t\tTableLocation \"75 361\"\r\n"
                    "\t\t\t\t\tTableSize \"476 40\"\r\n\t\t\t\t\tTableRowSpacing \"1\"\r\n\t\t\t\t\tTableTextScale \"0.75\"\r\n"
                    "\t\t\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n\t\t\t\t}\r\n";
    for (const std::string& h : {help1, help2})
        t += "\t\t\t\tRow\r\n\t\t\t\t{\r\n\t\t\t\t\tCellFixed \"" + (h.empty() ? std::string(" ") : h) + "\"\r\n\t\t\t\t}\r\n";
    return t + "\t\t\t}\r\n\t\t\tCommandSelect \"" + command + "\"\r\n\t\t\tCellFixed \"" + label + "\"\r\n\t\t}\r\n";
}

inline std::string MainMenuLayout(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_MainScreen\r\n");
    size_t end = screen == std::string::npos ? screen : text.find("\n}", screen + 1);
    const auto training = MenuRowOf(text, screen, end, "CommandSelect \"open SuperLauncher_Training\"");
    const auto single = MenuRowOf(text, screen, end, "CommandSelect \"open SuperLauncher_SinglePlayerMatch\"");
    const auto online = MenuRowOf(text, screen, end, "CellFixed \"Online Play\"");
    const auto party = MenuRowOf(text, screen, end, "CellFixed \"Multiplayer\"");
    const auto options = MenuRowOf(text, screen, end, "CommandSelect \"open SuperLauncher_Options\"");
    const auto profile = MenuRowOf(text, screen, end, "CellFixed \"Change Profile\"");
    for (const auto& r : {training, single, online, party, options, profile})
        if (r.first == std::string::npos) return {};
    auto row = [&](const std::pair<size_t, size_t>& r) { return text.substr(r.first, r.second - r.first); };
    const std::string instant =
        Relabel(row(single), "Instant Action", std::string("open ") + kInstantScreen,
                {"", "opponents: maps, modes, bots and rules."});
    const std::string multiplayer =
        Relabel(row(online), "Multiplayer", "", {"Server browser, split-screen, hosting, Direct-IP."});
    const std::string extras = Relabel(row(training), "Extras", std::string("open ") + kExtrasScreen,
                                       {"Training, your profile and mods."});
    if (instant.empty() || multiplayer.empty() || extras.empty()) return {};
    // the main table's rows: from its first row to its last, replaced
    const size_t first = std::min({training.first, single.first, online.first, party.first, options.first, profile.first});
    const size_t last = std::max({training.second, single.second, online.second, party.second, options.second, profile.second});
    const std::string between = text.substr(first, last - first);
    // (anything else between them, a row the title or another step added, stays after Options)
    std::string others = between;
    for (const auto& r : {training, single, online, party, options, profile}) {
        const std::string t = row(r);
        size_t at = others.find(t);
        if (at != std::string::npos) others.erase(at, t.size());
    }
    const std::string rows = instant + multiplayer + row(options) + others + extras;
    const std::string hub = HubScreen(kExtrasScreen, "Extras",
                                      row(training) + row(profile) +
                                          HubRow("Mods", "Turn installed mods on or off. Changes take",
                                                 "effect when the game next starts.", "open SuperLauncher_PCMods"));
    return text.substr(0, first) + rows + text.substr(last) + hub;
}

}  // namespace pcmenu
