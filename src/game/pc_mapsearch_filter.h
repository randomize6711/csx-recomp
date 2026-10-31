// The map lists' search (src/game/pc_mapsearch.cpp; tested on its own by tests/pc_mapsearch_filter_test.py): the
// maps' names indexed once per list (each row's label and map file name, lower case), the rows a search shows, the
// selection kept across a change of the search, and the search field's text editing. No game memory.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mapsearch {

inline std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// a list's maps as the menu text has them (label, map value: "Dust II", "de_dust2"), with their search key made once
struct Entry {
    std::string label, value, key;
};
inline std::vector<Entry> Index(const std::vector<std::pair<std::string, std::string>>& rows) {
    std::vector<Entry> out;
    out.reserve(rows.size());
    for (const auto& [label, value] : rows) out.push_back({label, value, Lower(label) + '\n' + Lower(value)});
    return out;
}

// the search as typed: spaces at the ends do not count
inline std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// the rows the search shows, in the list's order: every row for an empty search; else those whose name or file name
// holds it, case-insensitively ("dust" shows de_dust and de_dust2)
inline std::vector<size_t> Filter(const std::vector<Entry>& entries, const std::string& query) {
    std::vector<size_t> out;
    const std::string q = Lower(Trim(query));
    out.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); i++)
        if (q.empty() || entries[i].key.find(q) != std::string::npos) out.push_back(i);
    return out;
}

// the selection after the rows shown change: the same map when it is still shown, else the first shown, -1 when
// none is (a list row: an index into `shown`)
inline int SelectAfter(int selected_entry, const std::vector<size_t>& shown) {
    if (shown.empty()) return -1;
    for (size_t i = 0; i < shown.size(); i++)
        if (int(shown[i]) == selected_entry) return int(i);
    return 0;
}

// the first row shown so the selection is in view (rows: how many the table shows)
inline int FirstShown(int first, int selected, int count, int rows) {
    if (count <= 0 || selected < 0) return 0;
    rows = std::max(1, rows);
    if (selected < first) first = selected;
    if (selected >= first + rows) first = selected - rows + 1;
    return std::clamp(first, 0, std::max(0, count - rows));
}

// the search field's text: a cursor, and everything selected (Ctrl+A) so the next key replaces or deletes it
struct Field {
    std::string text;
    size_t cursor = 0;
    bool all = false;
    static constexpr size_t kMax = 32;

    void Insert(const std::string& s) {
        if (all) Clear();
        for (char c : s) {
            if (text.size() >= kMax) break;
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) continue;
            text.insert(cursor++, 1, c);
        }
    }
    void Backspace() {
        if (all) return Clear();
        if (cursor > 0) text.erase(--cursor, 1);
    }
    void Delete() {
        if (all) return Clear();
        if (cursor < text.size()) text.erase(cursor, 1);
    }
    void Left() { all = false, cursor = cursor > 0 ? cursor - 1 : 0; }
    void Right() { all = false, cursor = std::min(text.size(), cursor + 1); }
    void Home() { all = false, cursor = 0; }
    void End() { all = false, cursor = text.size(); }
    void SelectAll() { all = !text.empty(); }
    std::string Cut() {
        std::string s = all ? text : std::string();
        if (all) Clear();
        return s;
    }
    void Clear() { text.clear(), cursor = 0, all = false; }
    // as shown in the field: the placeholder when empty and not typing, a caret where it types
    std::string Shown(bool editing) const {
        if (text.empty() && !editing) return "Search maps...";
        if (!editing) return text;
        if (all) return "[" + text + "]";
        return text.substr(0, cursor) + "|" + text.substr(cursor);
    }
};

}  // namespace mapsearch
