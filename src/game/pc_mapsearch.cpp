// Map search (N5B; a filter since 2026-10-31): the map lists (Custom Game's SuperLauncher_SinglePlayerMatchMapList,
// which the single player match, Custom Match, Deathmatch and Gun Game all pick their map in; the party's
// SuperLauncher_PCMatchMaps, Peer-to-Peer's and Host LAN Match's) have a search field above the list ("Search maps...")
// that filters it as it is typed, case-insensitively, on each map's name and file name (src/game/pc_mapsearch_filter.h:
// the names indexed once when the menu text is loaded, never a folder scan on a key). The DLC maps (pc_dlc.cpp) and
// the game folder's maps (pc_lobby.cpp) are rows like the title's, so they are searched alike; a list only ever has
// the maps its screen offers.
// - The field: a letter, a digit, - or _ typed on the list starts typing in it (as does a click on it); Backspace,
//   Delete, the arrows, Home and End edit it; Ctrl+A selects it, Ctrl+C / Ctrl+X / Ctrl+V copy, cut and paste;
//   Enter or Escape leave it (Enter keeps the highlighted map; neither starts anything, nor closes the menu); [x]
//   beside it clears it. While typing, keys are the field's, not the menu's or the game's.
// - The list: the table's own rows (+0x60..+0x64, 0x20 bytes a row: their names, previews and values) are kept once
//   and the matching ones written in their place, the selection kept on its map when it still shows (else the first),
//   scrolled into view; "No maps found" on the help line when none does. Up and Down, the wheel, the scrollbar
//   (pc_scroll.cpp) and the mouse work on the rows shown.
// - Choosing a map (A, Enter, a double click: the screen's own command, which ExtendMapSearch has go through
//   "pc_mapsearch accept"): with none shown nothing happens; otherwise the whole list is put back with the selection
//   on that map's own row, then the screen's command runs, so the title's start of the session and the party's map
//   step see the list as they made it.
// - On a controller Y opens the profile keyboard (the title's virtual keyboard screen) for the same search; the
//   list's other buttons are as they were, and the search is never needed to choose a map.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/csx.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/pc_mapsearch_filter.h"
#include "kernel/kernel.h"

std::string PcCurrentScreen();  // pc_browser.cpp
void PcSetScreenLineColour(X86Context& ctx, const char* screen, int index, const std::string& text, float r, float g, float b,
                           float a);
std::string PcKeyboardScreen(const char* name, const char* title, const char* command, const char* hint);
std::string PcEditText(const char* screen);
void PcClearEdit(X86Context& ctx, const char* screen, const std::string& text);
int PcTableShownRows(uint32_t table);  // pc_scroll.cpp
float PcTextUnits(const std::string& s);  // pc_menu.cpp: a text's width in the menu font's units
void PcScrollForget(uint32_t table);   // pc_scroll.cpp: its scrollbar's rows in view dropped

namespace {
using kernel::R32;

constexpr char kKeyboard[] = "SuperLauncher_PCMapSearch";
// the menu font's units to the 640x480 layout's at a BackgroundText's scale 1 ("Map Selection": 11.6 units, 146 wide on
// the screen), and where the map screen's background has its COUNTER-STRIKE logo (Back_Select_A_Map: x 402 to 580)
constexpr float kTitleUnit = 12.6f, kLogoLeft = 402.0f;
constexpr uint32_t kRowSize = 0x20;

struct List {
    const char* screen;
    std::string help;     // its help line as the title wrote it (line 1)
    std::string accept;   // its A command as written ("SinglePlayerStartSession", "pc_lobby map")
    int field_line = -1;  // the BackgroundText lines added: the field, its [x]
    float fx = 0, fy = 0, fw = 0, fh = 0, cx = 0;  // their places (the menu's 640x480): the field, the [x]'s x
    std::vector<std::pair<std::string, std::string>> rows;  // label, map value (the menu text's order)
    std::vector<mapsearch::Entry> entries;
    // the table's rows as the title made them, kept while the list is filtered
    uint32_t table = 0, begin = 0;
    std::vector<uint8_t> saved;
    std::vector<size_t> shown;  // the rows shown (indices into entries), every row when not filtered
    bool filtered = false;
};
List g_lists[] = {{"SuperLauncher_SinglePlayerMatchMapList"}, {"SuperLauncher_PCMatchMaps"}};

std::mutex g_lock;
std::deque<int> g_keys;  // characters, or kKey* (the window thread's)
std::string g_text_mirror;  // the field's text for the window's Ctrl+C / Ctrl+X
std::atomic<bool> g_keys_on{false}, g_editing{false};
std::string g_screen;                // the list being searched
std::string g_pending;               // the keyboard's Done: searched once its list shows again
bool g_pending_set = false;
mapsearch::Field g_field;
bool g_dirty = true;                 // the field's lines to draw again
int g_accepting = 0;                 // frames left of a choice made (the list kept whole until the screen goes)

constexpr int kKeyBackspace = -2, kKeyDelete = -3, kKeyLeft = -4, kKeyRight = -5, kKeyHome = -6, kKeyEnd = -7,
              kKeyEnter = -8, kKeyEscape = -9, kKeySelectAll = -10, kKeyCut = -11;

List* ListOf(const std::string& screen) {
    for (List& l : g_lists)
        if (screen == l.screen) return &l;
    return nullptr;
}

uint32_t TableOf(const std::string& screen) {
    for (uint32_t s = R32(0x5523D0); s; s = R32(s + 0x378)) {
        std::string n;
        for (uint32_t p = R32(s), i = 0; p && i < 128; i++) {
            char c = static_cast<char>(*static_cast<uint8_t*>(kernel::HostPtr(p + i)));
            if (!c) break;
            n += c;
        }
        if (n == screen) return R32(s + 0x380);
    }
    return 0;
}

void SetEditing(bool on) {
    if (g_editing.exchange(on) != on) {
        g_dirty = true;
        LOGI("pc maps: the search field %s", on ? "takes the keys" : "is left");
    }
}

void Mirror() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_text_mirror = g_field.all ? g_field.text : std::string();
}

// the table's rows kept as the title made them (once a visit; 0 when they are not the menu text's rows)
bool Keep(List& l) {
    uint32_t table = TableOf(l.screen);
    if (!table) return false;
    uint32_t b = R32(table + 0x60), e = R32(table + 0x64);
    const size_t n = b && e > b ? (e - b) / kRowSize : 0;
    if (n != l.entries.size()) {
        LOG_FIRST(4, "pc maps: %s has %zu rows, its menu text %zu: searched without filtering", l.screen, n, l.entries.size());
        return false;
    }
    l.table = table, l.begin = b;
    l.saved.assign(static_cast<const uint8_t*>(kernel::HostPtr(b)), static_cast<const uint8_t*>(kernel::HostPtr(b)) + n * kRowSize);
    l.shown.resize(n);
    for (size_t i = 0; i < n; i++) l.shown[i] = i;
    l.filtered = false;
    return true;
}

// the selected row's map (an index into entries), -1 for none
int SelectedEntry(const List& l) {
    if (!l.table) return -1;
    int sel = int(R32(l.table + 0x9C));
    return sel >= 0 && sel < int(l.shown.size()) ? int(l.shown[size_t(sel)]) : -1;
}

// rows written: the given entries in the table, the selection on `entry` (or the first), in view
void Write(List& l, const std::vector<size_t>& rows, int entry) {
    for (size_t k = 0; k < rows.size(); k++)
        std::memcpy(kernel::HostPtr(l.begin + uint32_t(k) * kRowSize), l.saved.data() + rows[k] * kRowSize, kRowSize);
    kernel::W32(l.table + 0x64, l.begin + uint32_t(rows.size()) * kRowSize);
    l.shown = rows;
    PcScrollForget(l.table);
    const int sel = mapsearch::SelectAfter(entry, rows);
    kernel::W32(l.table + 0x9C, uint32_t(std::max(0, sel)));
    kernel::W32(l.table + 0x120, uint32_t(mapsearch::FirstShown(int(R32(l.table + 0x120)), sel, int(rows.size()),
                                                               PcTableShownRows(l.table))));
}

// the whole list again (the selection on its map's own row)
void Restore(List& l) {
    if (!l.filtered || !l.table) return;
    std::vector<size_t> all(l.saved.size() / kRowSize);
    for (size_t i = 0; i < all.size(); i++) all[i] = i;
    Write(l, all, SelectedEntry(l));
    l.filtered = false;
}

void ShowLine(X86Context& ctx, const List& l, const std::string& text) {
    PcSetScreenLineColour(ctx, l.screen, 1, text, 61, 74, 84, 255);
}

// the rows the field's search shows, and what the lines say
void Apply(X86Context& ctx, List& l) {
    const std::string query = mapsearch::Trim(g_field.text);
    if (l.table) {
        const int entry = SelectedEntry(l);
        if (query.empty()) {
            Restore(l);
        } else {
            Write(l, mapsearch::Filter(l.entries, query), entry);
            l.filtered = true;
        }
    }
    if (query.empty()) ShowLine(ctx, l, l.help);
    else if (l.table && l.shown.empty()) ShowLine(ctx, l, "No maps found");
    else if (l.table) ShowLine(ctx, l, std::to_string(l.shown.size()) + " of " + std::to_string(l.entries.size()) + " maps");
    else ShowLine(ctx, l, "Search: " + query);  // not filterable: as before, the help line only
    LOGI("pc maps: \"%s\" shows %zu of %zu maps", query.c_str(), l.table ? l.shown.size() : l.entries.size(), l.entries.size());
    g_dirty = true;
}

void DrawField(X86Context& ctx, const List& l) {
    if (!g_dirty || l.field_line < 0) return;
    g_dirty = false;
    const bool editing = g_editing;
    const std::string shown = g_field.Shown(editing);
    if (g_field.text.empty() && !editing) PcSetScreenLineColour(ctx, l.screen, l.field_line, shown, 120, 132, 142, 255);
    else PcSetScreenLineColour(ctx, l.screen, l.field_line, shown, 54, 83, 107, 255);
    PcSetScreenLineColour(ctx, l.screen, l.field_line + 1, g_field.text.empty() ? " " : "[x]", 54, 83, 107, 255);
}

// a screen's rows from the menu text: one ListDefault a row ("label" "value")
std::vector<std::pair<std::string, std::string>> RowsOf(const std::string& text, size_t from, size_t to) {
    std::vector<std::pair<std::string, std::string>> out;
    for (size_t at = text.find("ListDefault", from); at != std::string::npos && at < to; at = text.find("ListDefault", at + 1)) {
        size_t a = text.find('"', at), b = a == std::string::npos ? a : text.find('"', a + 1);
        size_t c = b == std::string::npos ? b : text.find('"', b + 1), d = c == std::string::npos ? c : text.find('"', c + 1);
        if (d == std::string::npos || d > to) break;
        out.push_back({text.substr(a + 1, b - a - 1), text.substr(c + 1, d - c - 1)});
    }
    return out;
}
}  // namespace

// the front end's text: both map lists get the search field's lines above their table, Y (the search keyboard), their
// A through "pc_mapsearch accept", and their rows read; the keyboard screen added
std::string ExtendMapSearch(const std::string& text) {
    std::string out = text;
    for (List& l : g_lists) {
        size_t screen = out.find(std::string("\n") + l.screen + "\r\n");
        size_t end = screen == std::string::npos ? screen : out.find("\n}", screen + 1);
        size_t a = end == std::string::npos ? end : out.find("\tOnButtonDownA \"", screen);
        size_t table = end == std::string::npos ? end : out.find("\n\tTable\r\n", screen);
        if (a == std::string::npos || a > end || table == std::string::npos || table > end) {
            LOGW("pc maps: %s not as expected: no search there", l.screen);
            continue;
        }
        size_t help = out.find("BackgroundText", out.find("BackgroundText", screen) + 1);  // the second: the help line
        if (help < end) {
            size_t q1 = out.find('"', out.find('"', out.find('"', help) + 1) + 1), q2 = out.find('"', q1 + 1);
            l.help = out.substr(q1 + 1, q2 - q1 - 1);
        }
        // A: the screen's command, run by the search once a shown map is chosen
        size_t c0 = a + std::strlen("\tOnButtonDownA \""), c1 = out.find('"', c0);
        l.accept = out.substr(c0, c1 - c0);
        if (!l.accept.empty() && l.accept[0] == '!') l.accept.erase(0, 1);
        out.replace(a, c1 + 1 - a, "\tOnButtonDownA \"!pc_mapsearch accept\"");
        out.insert(a, "\tOnButtonDownY \"!pc_mapsearch open\"\r\n");
        // the field: in the header, on the title's line between the title and the background's COUNTER-STRIKE logo
        // (x 402 on in the 640x480 layout, which the 2D layout keeps at every aspect ratio), compact, with a
        // blue-grey border (two whitebox rectangles: the border, the field inside it) and its [x] at its right end.
        // When the title leaves no room before the logo, above the list instead (its first row stays where it is)
        table = out.find("\n\tTable\r\n", screen);
        float title_x = 65, title_y = 45;
        std::string title;
        if (size_t t = out.find("BackgroundText \"", screen); t < table) {
            std::sscanf(out.c_str() + t, "BackgroundText \"%*f %*f %*f %*f %f %f", &title_x, &title_y);
            size_t q1 = out.find('"', out.find('"', t + 16) + 1), q2 = q1 == std::string::npos ? q1 : out.find('"', q1 + 1);
            if (q2 != std::string::npos && q2 < table) title = out.substr(q1 + 1, q2 - q1 - 1);
        }
        const mapsearch::Header header = mapsearch::PlaceInHeader(title_x, title_y, PcTextUnits(title) * kTitleUnit, kLogoLeft);
        if (!header.in_header) {
            size_t loc = out.find("TableLocation \"", table);
            float tx = 75, ty = 135;
            if (loc != std::string::npos) std::sscanf(out.c_str() + loc, "TableLocation \"%f %f\"", &tx, &ty);
            l.fx = tx + 5, l.fy = ty - 32, l.fw = 165, l.fh = 22;
        } else {
            l.fx = header.x, l.fy = header.y, l.fw = header.w, l.fh = header.h;
        }
        l.cx = l.fx + l.fw - 22;
        int lines = 0;
        for (size_t b = out.find("BackgroundText", screen); b != std::string::npos && b < table; b = out.find("BackgroundText", b + 1))
            lines++;
        l.field_line = lines;
        char field[400];
        std::snprintf(field, sizeof field,
                      "\tBackgroundImage \"whitebox 110 128 146 255 %.0f %.0f %.0f %.0f\"\r\n"
                      "\tBackgroundImage \"whitebox 236 239 242 235 %.0f %.0f %.0f %.0f\"\r\n"
                      "\tBackgroundText \"120 132 142 255 %.0f %.0f %.0f %.0f 0.7\"\t\"Search maps...\"\r\n"
                      "\tBackgroundText \"54 83 107 255 %.0f %.0f 20 %.0f 0.7\"\t\" \"\r\n",
                      l.fx, l.fy, l.fw, l.fh, l.fx + 1, l.fy + 1, l.fw - 2, l.fh - 2, l.fx + 6, l.fy + 1, l.fw - 30, l.fh,
                      l.cx, l.fy + 1, l.fh);
        out.insert(table + 1, field);
        LOGI("pc maps: %s's search field %s at %.0f %.0f (%.0f x %.0f)", l.screen,
             header.in_header ? "in the header" : "above the list (no room in the header)", l.fx, l.fy, l.fw, l.fh);
        end = out.find("\n}", screen + 1);
        l.rows = RowsOf(out, screen, end);
        l.entries = mapsearch::Index(l.rows);
        LOGI("pc maps: %s, %zu maps searchable (field line %d, A runs %s)", l.screen, l.rows.size(), l.field_line,
             l.accept.c_str());
    }
    return out + PcKeyboardScreen(kKeyboard, "Find a Map", "pc_mapsearch done", "Part of the map's name.");
}

// "pc_mapsearch open" (Y on a map list), "pc_mapsearch done" (the keyboard's Done), "pc_mapsearch accept" (A, Enter
// or a double click on a map list: the map chosen)
void PcMapSearchCommand(X86Context& ctx, const std::string& what) {
    if (what == "open") {
        g_screen = PcCurrentScreen();
        SetEditing(false);
        PcClearEdit(ctx, kKeyboard, g_field.text);
        csx::QueueEngineCommand(std::string("xbui open ") + kKeyboard + "\n");
    } else if (what == "done") {
        g_pending = PcEditText(kKeyboard);
        while (!g_pending.empty() && g_pending.back() == ' ') g_pending.pop_back();
        g_pending_set = true;
        csx::QueueEngineCommand("xbui back\n");
    } else if (what == "accept") {
        List* l = ListOf(PcCurrentScreen());
        if (!l) return;
        if (g_editing) {  // the search field's Enter: it is left, the highlighted map kept, nothing started
            SetEditing(false);
            return;
        }
        if (l->filtered && l->shown.empty()) {
            LOGI("pc maps: no map shown: nothing chosen");
            return;
        }
        const int entry = SelectedEntry(*l);
        Restore(*l);
        g_accepting = 30;
        LOGI("pc maps: %s chosen (%s)", entry >= 0 && entry < int(l->entries.size()) ? l->entries[size_t(entry)].value.c_str() : "?",
             l->accept.c_str());
        csx::QueueEngineCommand(l->accept + "\n");
    }
}

// a click on a map list (pc_mouse.cpp, the menu's 640x480 units): the field starts typing, its [x] clears it; true
// when the click was the search's. A click anywhere else leaves the field
bool PcMapSearchClick(X86Context& ctx, const std::string& screen, float x, float y) {
    List* l = ListOf(screen);
    if (!l || l->field_line < 0) return false;
    if (y >= l->fy && y < l->fy + l->fh && x >= l->cx - 2 && x < l->fx + l->fw + 2 && !g_field.text.empty()) {
        g_field.Clear();
        Mirror();
        Apply(ctx, *l);
        SetEditing(true);
        return true;
    }
    if (y >= l->fy && y < l->fy + l->fh && x >= l->fx - 4 && x < l->fx + l->fw) {
        SetEditing(true);
        g_field.End();
        return true;
    }
    SetEditing(false);
    return false;
}

// whether a screen is a map list (pc_scroll.cpp: its scrollbar)
bool PcMapSearchList(const std::string& screen) { return ListOf(screen) != nullptr; }

// on the game thread every frame (pc_menu.cpp's PcMenuFrame)
void PcMapSearchFrame(X86Context& ctx) {
    std::string screen = PcCurrentScreen();
    List* l = ListOf(screen);
    bool on = l != nullptr && !csx::ConsoleOpen();
    if (on != g_keys_on.exchange(on)) LOGI("pc maps: keys %s", on ? "search the list" : "are the pad's again");
    std::deque<int> keys;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        keys.swap(g_keys);
    }
    if (screen == kKeyboard) return;
    if (!l) {
        if (!screen.empty()) {  // another screen: the lists whole again, the next starts afresh
            for (List& each : g_lists) Restore(each);
            g_field.Clear();
            Mirror();
            g_pending.clear();
            g_pending_set = false;
            g_screen.clear();
            g_accepting = 0;
            SetEditing(false);
        }
        return;
    }
    if (screen != g_screen) {  // a list opened: its search starts empty
        for (List& each : g_lists) Restore(each);
        g_screen = screen;
        g_field.Clear();
        Mirror();
        SetEditing(false);
        g_accepting = 0;
        Keep(*l);
        ShowLine(ctx, *l, l->help);
        g_dirty = true;
    }
    if (g_accepting > 0) {  // a map chosen: the list stays whole while its command takes the screen away
        g_accepting--;
        DrawField(ctx, *l);
        return;
    }
    if (g_pending_set) {  // back from the keyboard
        g_pending_set = false;
        g_field.Clear();
        g_field.Insert(std::exchange(g_pending, std::string()));
        Mirror();
        Apply(ctx, *l);
    }
    bool changed = false;
    for (int k : keys) {
        const std::string before = g_field.text;
        if (k > 0) {
            if (!g_editing) SetEditing(true);
            g_field.Insert(std::string(1, char(k)));
        } else if (k == kKeyBackspace) {
            if (!g_editing) SetEditing(true), g_field.End();
            g_field.Backspace();
        } else if (k == kKeyDelete) {
            g_field.Delete();
        } else if (k == kKeyLeft) {
            g_field.Left();
        } else if (k == kKeyRight) {
            g_field.Right();
        } else if (k == kKeyHome) {
            g_field.Home();
        } else if (k == kKeyEnd) {
            g_field.End();
        } else if (k == kKeySelectAll) {
            g_field.SelectAll();
        } else if (k == kKeyCut) {
            g_field.Cut();
        } else if (k == kKeyEnter || k == kKeyEscape) {
            SetEditing(false);
        }
        g_dirty = true;
        changed |= g_field.text != before;
    }
    if (!keys.empty()) Mirror();
    if (changed) Apply(ctx, *l);
    DrawField(ctx, *l);
}

namespace csx {
bool MapSearchKeys() { return g_keys_on; }
bool MapSearchEditing() { return g_keys_on && g_editing; }
void MapSearchInput(int c) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_keys.push_back(c);
}
std::string MapSearchSelection() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_text_mirror;
}
}  // namespace csx
