// Map search (N5B): the map lists (Custom Game's SuperLauncher_SinglePlayerMatchMapList, the party's
// SuperLauncher_PCMatchMaps) find a map by its name. On the keyboard, letters and digits typed there make a search
// (Backspace takes one back): the highlight goes to the first map whose name or file name holds it, the list scrolled
// to show it, and the screen's help line says what is searched and what it found. On a controller Y opens the profile
// keyboard (the title's virtual keyboard screen) for the same search, Done finds it. The arrows, A, B and the mouse
// stay as they were; the screen's other keys are not taken (on these screens letters are the search's, not the pad's
// W A S D, Q and R). The rows are read from the menu text as it is loaded (one ListDefault a row, in order).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/csx.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

std::string PcCurrentScreen();  // pc_browser.cpp
void PcSetScreenLineColour(X86Context& ctx, const char* screen, int index, const std::string& text, float r, float g, float b,
                           float a);
std::string PcKeyboardScreen(const char* name, const char* title, const char* command, const char* hint);
std::string PcEditText(const char* screen);
void PcClearEdit(X86Context& ctx, const char* screen, const std::string& text);
int PcTableShownRows(uint32_t table);  // pc_scroll.cpp

namespace {
using kernel::R32;

constexpr char kKeyboard[] = "SuperLauncher_PCMapSearch";
struct List {
    const char* screen;
    std::string help;                                  // its help line as the title wrote it
    std::vector<std::pair<std::string, std::string>> rows;  // label, map value
};
List g_lists[] = {{"SuperLauncher_SinglePlayerMatchMapList", {}, {}}, {"SuperLauncher_PCMatchMaps", {}, {}}};

std::mutex g_lock;
std::deque<int> g_keys;  // characters, or -2 Backspace (the window thread's)
std::atomic<bool> g_keys_on{false};
std::string g_query, g_screen;     // the search and the screen it is for
std::string g_pending;             // the keyboard's Done: searched once its list shows again
bool g_shown_default = true;

List* ListOf(const std::string& screen) {
    for (List& l : g_lists)
        if (screen == l.screen) return &l;
    return nullptr;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
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

void ShowLine(X86Context& ctx, const List& l, const std::string& text) {
    PcSetScreenLineColour(ctx, l.screen, 1, text, 61, 74, 84, 255);
}

// the highlight on the first row holding the search, the list scrolled to it; the help line says so
void Find(X86Context& ctx, List& l, const std::string& query) {
    if (query.empty()) {
        ShowLine(ctx, l, l.help);
        g_shown_default = true;
        return;
    }
    g_shown_default = false;
    std::string q = Lower(query);
    uint32_t table = TableOf(l.screen);
    for (size_t i = 0; i < l.rows.size() && table; i++) {
        if (Lower(l.rows[i].first).find(q) == std::string::npos && Lower(l.rows[i].second).find(q) == std::string::npos)
            continue;
        uint32_t row = uint32_t(i), first = R32(table + 0x120);
        int shown = std::max(1, PcTableShownRows(table));
        if (row < first) first = row;
        else if (row >= first + uint32_t(shown)) first = row - uint32_t(shown) + 1;
        kernel::W32(table + 0x9C, row);
        kernel::W32(table + 0x120, first);
        ShowLine(ctx, l, "Search: " + query + "   " + l.rows[i].first);
        LOGI("pc maps: \"%s\" found %s (row %zu)", query.c_str(), l.rows[i].second.c_str(), i);
        return;
    }
    ShowLine(ctx, l, "Search: " + query + "   no map has it");
    LOGI("pc maps: \"%s\" found no map", query.c_str());
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

// the front end's text: both map lists take Y (the search keyboard) and their rows are read; the keyboard screen added
std::string ExtendMapSearch(const std::string& text) {
    std::string out = text;
    for (List& l : g_lists) {
        size_t screen = out.find(std::string("\n") + l.screen + "\r\n");
        size_t end = screen == std::string::npos ? screen : out.find("\n}", screen + 1);
        size_t a = end == std::string::npos ? end : out.find("\tOnButtonDownA", screen);
        if (a == std::string::npos || a > end) {
            LOGW("pc maps: %s not as expected: no search there", l.screen);
            continue;
        }
        size_t help = out.find("BackgroundText", out.find("BackgroundText", screen) + 1);  // the second: the help line
        if (help < end) {
            size_t q1 = out.find('"', out.find('"', out.find('"', help) + 1) + 1), q2 = out.find('"', q1 + 1);
            l.help = out.substr(q1 + 1, q2 - q1 - 1);
        }
        out.insert(a, "\tOnButtonDownY \"!pc_mapsearch open\"\r\n");
        end = out.find("\n}", screen + 1);
        l.rows = RowsOf(out, screen, end);
        LOGI("pc maps: %s, %zu maps searchable", l.screen, l.rows.size());
    }
    return out + PcKeyboardScreen(kKeyboard, "Find a Map", "pc_mapsearch done", "Part of the map's name.");
}

// "pc_mapsearch open" (Y on a map list), "pc_mapsearch done" (the keyboard's Done)
void PcMapSearchCommand(X86Context& ctx, const std::string& what) {
    if (what == "open") {
        g_screen = PcCurrentScreen();
        PcClearEdit(ctx, kKeyboard, g_query);
        csx::QueueEngineCommand(std::string("xbui open ") + kKeyboard + "\n");
    } else if (what == "done") {
        g_pending = PcEditText(kKeyboard);
        while (!g_pending.empty() && g_pending.back() == ' ') g_pending.pop_back();
        csx::QueueEngineCommand("xbui back\n");
    }
}

// on the game thread every frame (pc_menu.cpp's PcMenuFrame)
void PcMapSearchFrame(X86Context& ctx) {
    std::string screen = PcCurrentScreen();
    List* l = ListOf(screen);
    bool on = l != nullptr && !csx::ConsoleOpen();
    if (on != g_keys_on.exchange(on)) LOGI("pc maps: letters %s", on ? "search the list" : "are the pad's again");
    std::deque<int> keys;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        keys.swap(g_keys);
    }
    if (screen == kKeyboard) return;
    if (!l) {
        if (!screen.empty()) {  // another screen: the next list starts afresh
            g_query.clear();
            g_pending.clear();
            g_screen.clear();
        }
        return;
    }
    if (screen != g_screen) {  // a list opened: its search starts empty
        g_screen = screen;
        g_query.clear();
        if (!g_shown_default) Find(ctx, *l, "");
    }
    if (!g_pending.empty()) {  // back from the keyboard
        g_query = std::exchange(g_pending, std::string());
        Find(ctx, *l, g_query);
    }
    bool changed = false;
    for (int k : keys) {
        if (k == -2) {
            if (!g_query.empty()) g_query.pop_back();
        } else if (g_query.size() < 24) {
            g_query += char(k);
        }
        changed = true;
    }
    if (changed) Find(ctx, *l, g_query);
}

namespace csx {
bool MapSearchKeys() { return g_keys_on; }
void MapSearchInput(int c) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_keys.push_back(c);
}
}  // namespace csx
