// The server browser in the game's own menus (docs/pc_multiplayer.md "N3"; FINDINGS.md "Original menus"). The
// original System Link menus are the base: System Link becomes Multiplayer (LAN Matches, Internet Matches, Favorites,
// Recent Servers, Join by Address, Host a Match), and the server list is the console's own session list screen
// (Xbox Live's OptiMatch Sessions layout: its background, columns, highlight, details lines and the System Link
// screen's Join / Refresh / Host a Match / Cancel bar), written as menu text at load and drawn, navigated and sounded
// by the game's UI code. Nothing here draws: the list's rows are the table's rows, made with the game's own row and
// cell functions (as its System Link list was filled, 0xF12F0); the title, tabs, status and details are the screen's
// BackgroundText lines with their text and colour set; the keyboard screens (address, password) are its profile
// keyboard. The discovery (src/net/browser.h) runs on a thread of its own; the game thread copies what it found when
// it changed and rebuilds the rows (at most a few times a second, the selection kept on the same server).
//
// Controls: the pad as the console's (A join, X refresh, Y host a match, B back, LT / RT the tabs, Black the sort
// order, White add or remove a favourite, Back the filters); the keyboard as the menus' pad (Enter joins, Escape
// goes back on these screens, F5 refreshes, Page Up / Page Down the tabs); the mouse: a row selects, a double click
// joins, a column's header sorts by it, the tabs and the Sort / Filters / Favorite lines are clickable.
//
// Joining: the browser's answer from the server (signed by its identity) is checked before anything else: the key
// remembered for that address (known_servers.txt) or pinned (net_server_key) must match, or the identity warning
// asks first (Accept trusts the new key, explicitly); a full server, another game version, a server that did not
// answer are messages; a password is asked for on the keyboard. Then the title relaunches into "+connect ADDRESS" as
// its system link join did, with the server's key pinned for that connection (CSX_SERVER_KEY) so the game connection
// must reach the same server the list showed. A join that fails (a wrong password, a kick, a lost server) returns to
// the front end and the browser with a message (net_session.cpp).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/net_engine.h"
#include "kernel/kernel.h"
#include "net/browser.h"
#include "net/query.h"
#include "net/transport.h"

namespace {
using kernel::R32;
using kernel::W32;

// ---- screens --------------------------------------------------------------------------------------------------------
constexpr char kBrowser[] = "SuperLauncher_PCBrowser";
constexpr char kAddress[] = "SuperLauncher_PCAddress";
constexpr char kPassword[] = "SuperLauncher_PCPassword";
constexpr char kMessage[] = "SuperLauncher_PCMessage";
constexpr char kServerMessage[] = "SuperLauncher_PCServerMessage";  // the local server's (N4.7): did not start, stopped
constexpr char kIdentity[] = "SuperLauncher_PCIdentity";
constexpr char kContacting[] = "SuperLauncher_PCContacting";
constexpr char kMultiplayer[] = "SuperLauncher_SystemLink";

// the browser's BackgroundText lines, in the order the screen declares them
enum Line {
    kTitle, kTab0, kTab1, kTab2, kTab3, kStatus, kDetail0,  // eight detail lines: two columns of four
    kSort = kDetail0 + 8, kFilters, kFavorite, kLines
};
constexpr int kTabs = 4;
const char* const kTabNames[kTabs] = {"LAN", "Internet", "Favorites", "Recent"};
const char* const kTabTitles[kTabs] = {"LAN Matches", "Internet Matches", "Favorite Servers", "Recent Servers"};
const uint8_t kTabSources[kTabs] = {net::kLan, net::kInternet, net::kFavorite, net::kRecent};
// where the lines are (menu pixels): the tabs, the details' columns, the actions (also the mouse's targets)
constexpr float kTabX[kTabs] = {75, 135, 222, 322}, kTabY = 104, kStatusX = 420;
constexpr float kDetailX[2] = {70, 330}, kDetailY = 300, kDetailStep = 19;
constexpr float kActionY = 382, kActionX[3] = {70, 270, 370};
// the list
constexpr float kTableX = 65, kTableY = 128, kTableW = 515, kTableH = 160;

// ---- guest functions the list and texts are made with (the System Link list's own, 0xF12F0) --------------------------
constexpr uint32_t kScreens = 0x5523D0, kCurrentScreen = 0x5523D4;
constexpr uint32_t kMalloc = 0x22D86A, kFree = 0x22C609;  // cdecl
constexpr uint32_t kAssign = 0xBC290;     // std::wstring::assign: ecx the string, eax the characters, [esp+4] count
constexpr uint32_t kAddCell = 0xDC440;    // a fixed cell: ecx its colour, edx the row; [esp+4] its text, [esp+8] colour
constexpr uint32_t kRowDestroy = 0xDC2D0; // a row's cells freed: eax the row
constexpr uint32_t kRowsClear = 0xDD520;  // a table's rows freed: ebx the rows vector (table +0x5C)
constexpr uint32_t kRowsPush = 0xDD2F0;   // a row copied onto the rows vector: ecx the row, esi the vector
constexpr uint32_t kCellColour = 0x385554, kCellColour2 = 0x552E88;
constexpr uint32_t kEditBox = 0x388;      // a keyboard screen's edit box; its text a std::wstring at +0x34

struct Regs {
    uint32_t eax = 0, ecx = 0, edx = 0, ebx = 0, esi = 0;
};
// a guest function taking register arguments; the caller's registers are as they were after
uint32_t Call(X86Context& ctx, uint32_t fn, Regs r, std::initializer_list<uint32_t> args = {}) {
    uint32_t s[5] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi};
    ctx.eax = r.eax, ctx.ecx = r.ecx, ctx.edx = r.edx, ctx.ebx = r.ebx, ctx.esi = r.esi;
    uint32_t result = kernel::CallGuest(ctx, fn, args);
    ctx.eax = s[0], ctx.ecx = s[1], ctx.edx = s[2], ctx.ebx = s[3], ctx.esi = s[4];
    return result;
}

std::string GuestString(uint32_t va, size_t max = 128) {
    std::string s;
    for (size_t i = 0; va && i < max; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}

// a guest block for the texts' characters and a temporary std::wstring (made once, never freed)
uint32_t Scratch(X86Context& ctx) {
    static uint32_t block = 0;
    if (!block) block = kernel::CallGuest(ctx, kMalloc, {0x1000});
    return block;
}

// text (ASCII, cleaned) into a std::wstring of the game's: assign from UTF-16 in the scratch block
void SetWide(X86Context& ctx, uint32_t wstr, const std::string& text) {
    uint32_t buf = Scratch(ctx);
    if (!buf) return;
    std::string t = text.substr(0, 0x7F0);
    for (size_t i = 0; i <= t.size(); i++) {
        uint16_t c = i < t.size() ? static_cast<unsigned char>(t[i]) : 0;
        std::memcpy(static_cast<uint8_t*>(kernel::HostPtr(buf)) + i * 2, &c, 2);
    }
    Call(ctx, kAssign, {buf, wstr}, {uint32_t(t.size())});
}

std::string ReadWide(uint32_t wstr) {
    uint32_t len = R32(wstr + 0x14), cap = R32(wstr + 0x18);
    uint32_t chars = cap >= 8 ? R32(wstr + 4) : wstr + 4;
    std::string s;
    for (uint32_t i = 0; i < len && i < 256 && chars; i++) {
        uint16_t c;
        std::memcpy(&c, kernel::HostPtr(chars + i * 2), 2);
        s += c < 0x80 ? char(c) : '?';
    }
    return s;
}

uint32_t FindScreen(const char* name) {
    for (uint32_t s = R32(kScreens); s; s = R32(s + 0x378))
        if (GuestString(R32(s)) == name) return s;
    return 0;
}
std::string CurrentScreen() {
    uint32_t s = R32(kCurrentScreen);
    return s ? GuestString(R32(s)) : std::string();
}

// a BackgroundText line of a screen: its text and colour (alpha 0 hides it)
void SetLine(X86Context& ctx, uint32_t screen, int i, const std::string& text, float r = 54, float g = 83, float b = 107,
             float a = 255) {
    if (!screen || i >= int(R32(screen + 0x108))) return;
    uint32_t t = R32(screen + 0x10C + 4 * uint32_t(i));
    if (!t) return;
    float c[4] = {r / 255, g / 255, b / 255, text.empty() ? 0.f : a / 255};
    std::memcpy(kernel::HostPtr(t + 0x10), c, sizeof(c));
    SetWide(ctx, t + 0x20, text.empty() ? " " : text);
}

// ---- the discovery thread -----------------------------------------------------------------------------------------------
struct Shared {
    std::mutex lock;
    std::deque<std::function<void(net::Browser&)>> commands;
    std::vector<net::BrowserEntry> entries;
    uint64_t version = 0;
    bool busy = false, listing = false;
    size_t masters = 0, masters_answered = 0;
    net::Browser::Stats stats;
    std::map<std::string, net::Address> resolved;  // saved and typed addresses -> what they resolved to
    std::map<std::string, std::string> unresolved; // address -> why not
};
Shared g_shared;
std::thread g_thread;
std::atomic<bool> g_running{false};

double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

std::vector<uint16_t> LanPorts() {
    std::vector<uint16_t> ports;
    unsigned lo = 27015, hi = 27020;
    std::string spec = pc::Get("net_lan_ports", "27015-27020");
    if (std::sscanf(spec.c_str(), "%u-%u", &lo, &hi) != 2) hi = lo = unsigned(std::atoi(spec.c_str()));
    if (lo == 0 || hi > 65535 || hi < lo || hi - lo > 32) lo = 27015, hi = 27020;
    for (unsigned p = lo; p <= hi; p++) ports.push_back(uint16_t(p));
    uint16_t own = csx::NetServerPort();
    if (std::find(ports.begin(), ports.end(), own) == ports.end()) ports.push_back(own);
    return ports;
}

void ThreadMain(net::BrowserConfig config, std::string masters, std::optional<net::Address> bind) {
    std::string error;
    auto socket = net::OpenUdp(0, error, bind ? &*bind : nullptr, true);
    if (!socket) {
        LOGE("browser: no socket: %s", error.c_str());
        g_running = false;
        return;
    }
    LOGI("browser: discovery on UDP %s, LAN ports %u-%u, %zu broadcast addresses", socket->Local().ToString().c_str(),
         config.lan_ports.front(), config.lan_ports.back(), config.broadcast.size());
    // the master servers named (net_master), resolved here, off the game thread
    for (size_t at = 0; at < masters.size();) {
        size_t end = masters.find_first_of(", ", at);
        std::string m = masters.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!m.empty()) {
            if (auto a = net::Address::Parse(m, net::kMasterPort)) config.masters.push_back(*a);
            else LOGW("browser: the master server %s cannot be resolved", m.c_str());
        }
        at = end == std::string::npos ? masters.size() : end + 1;
    }
    size_t nmasters = config.masters.size();
    net::Browser b(std::move(socket), config, Now);
    uint64_t published = 0;
    while (g_running) {
        std::deque<std::function<void(net::Browser&)>> work;
        {
            std::lock_guard<std::mutex> lock(g_shared.lock);
            work.swap(g_shared.commands);
        }
        for (auto& f : work) f(b);
        b.Pump();
        bool busy = b.Busy();
        if (b.Version() != published || busy != g_shared.busy) {
            published = b.Version();
            std::lock_guard<std::mutex> lock(g_shared.lock);
            g_shared.entries = b.Entries();
            g_shared.version++;
            g_shared.busy = busy;
            g_shared.listing = b.Listing();
            g_shared.masters = nmasters;
            g_shared.masters_answered = b.MastersAnswered();
            g_shared.stats = b.GetStats();
        }
        b.Wait(0.005);
    }
}

void Post(std::function<void(net::Browser&)> f) {
    std::lock_guard<std::mutex> lock(g_shared.lock);
    g_shared.commands.push_back(std::move(f));
}

void StartThread() {
    if (g_running) return;
    net::BrowserConfig c;
    c.lan_ports = LanPorts();
    c.broadcast = net::LanBroadcastAddresses();
    std::optional<net::Address> bind;
    if (const char* b = std::getenv("CSX_NET_BIND")) bind = net::Address::Parse(b, 0);
    g_running = true;
    g_thread = std::thread(ThreadMain, c, pc::Get("net_master", ""), bind);
    std::atexit([] {
        g_running = false;
        if (g_thread.joinable()) g_thread.join();
    });
}

// a saved or typed address: resolved on the thread (a name may take seconds), then queried as that source
void AddAddress(const std::string& text, uint8_t source, const std::string& key) {
    Post([text, source, key](net::Browser& b) {
        auto a = net::Address::Parse(text, csx::NetServerPort());
        std::lock_guard<std::mutex> lock(g_shared.lock);
        if (!a) {
            g_shared.unresolved[text] = "the address cannot be found";
            return;
        }
        g_shared.resolved[text] = *a;
        g_shared.unresolved.erase(text);
        b.Add(*a, source, key);
    });
}

// ---- saved servers (PC settings favorite.N / recent.N) -----------------------------------------------------------------
constexpr size_t kMaxFavorites = 64, kMaxRecent = 16;
std::vector<net::SavedServer> g_favorites, g_recent;
bool g_saved_loaded = false;

std::vector<net::SavedServer> LoadSaved(const char* prefix, size_t max) {
    std::vector<net::SavedServer> v;
    for (size_t i = 1; i <= max; i++) {
        std::string line = pc::Get(std::string(prefix) + "." + std::to_string(i), "");
        if (line.empty()) continue;
        if (auto s = net::ParseSaved(line)) v.push_back(*s);
        else LOGW("browser: %s.%zu is not \"ADDRESS KEY NAME\": ignored", prefix, i);
    }
    return v;
}
void StoreSaved(const char* prefix, const std::vector<net::SavedServer>& v, size_t max) {
    for (size_t i = 1; i <= max; i++) {
        std::string key = std::string(prefix) + "." + std::to_string(i);
        if (i <= v.size()) pc::Set(key, net::FormatSaved(v[i - 1]));
        else if (!pc::Get(key, "").empty()) pc::Erase(key);
    }
    pc::Save();
}
void LoadAllSaved() {
    if (g_saved_loaded) return;
    g_saved_loaded = true;
    g_favorites = LoadSaved("favorite", kMaxFavorites);
    g_recent = LoadSaved("recent", kMaxRecent);
    for (auto& f : g_favorites) AddAddress(f.address, net::kFavorite, f.key);
    for (auto& r : g_recent) AddAddress(r.address, net::kRecent, "");
    LOGI("browser: %zu favorites, %zu recent servers", g_favorites.size(), g_recent.size());
}

// the saved server an entry is (by the address its text resolved to)
int SavedIndex(const std::vector<net::SavedServer>& v, const net::Address& a) {
    std::lock_guard<std::mutex> lock(g_shared.lock);
    for (size_t i = 0; i < v.size(); i++) {
        auto it = g_shared.resolved.find(v[i].address);
        if (it != g_shared.resolved.end() && it->second == a) return int(i);
    }
    return -1;
}

// ---- the browser's state on the game thread ------------------------------------------------------------------------------
struct State {
    int tab = 0;
    std::vector<net::BrowserEntry> entries;
    std::vector<size_t> view;
    std::vector<net::Address> view_addresses;  // the rows' servers in their order
    uint64_t version = 0, shown_version = ~0ull;
    unsigned shown_settings = ~0u;
    int shown_tab = -1;
    double rebuilt_at = -1;
    bool have_selected = false;
    bool chosen = false;      // the player moved the selection: it follows that server; until then, the top row
    net::Address selected;
    int selected_row = -1;
    std::string details_for = "-";  // the entry the details show (address + version)
    bool resort = true;       // the next rebuild sorts again (else the rows keep their order: pings that move by a
                              // millisecond do not reshuffle the list under the pointer)
    bool busy = false, listing = false;
    size_t masters = 0, masters_answered = 0;
    double refreshed_at = -1;
    net::ViewOptions options;
} g;

struct Join {
    bool active = false;
    net::Address address;
    std::string name, key, password;
    bool trusted = false;   // the identity warning was answered with Accept
} g_join;

// a typed address being contacted (Join by Address)
struct Contact {
    bool active = false;
    std::string text;
    double started = 0;
} g_contact;

std::string g_message[3];  // the message screen's lines
bool g_message_closing = false;  // Continue pressed: the message closes once A and B are released
// the screen the Match Server message opened over, which it closes back into (logged)
std::string g_message_under;
std::atomic<bool> g_browser_keys{false};

net::ViewOptions Options(int tab) {
    net::ViewOptions o;
    o.sources = kTabSources[tab];
    std::string sort = pc::Get("browser_sort", "ping");
    o.sort = sort == "players" ? net::SortKey::Players : sort == "name" ? net::SortKey::Name
           : sort == "map" ? net::SortKey::Map : net::SortKey::Ping;
    o.reverse = pc::Get("browser_order", "0") == "1";
    o.hide_full = pc::Get("browser_full", "1") == "0";
    o.hide_empty = pc::Get("browser_empty", "1") == "0";
    std::string pw = pc::Get("browser_password", "any");
    o.password = pw == "hide" ? 1 : pw == "only" ? 2 : 0;
    o.map = pc::Get("browser_map", "");
    std::string loc = pc::Get("browser_location", "any");
    o.location = tab >= 2 ? (loc == "lan" ? 1 : loc == "internet" ? 2 : 0) : 0;
    o.answered_only = tab < 2;  // LAN and internet: the servers that answered; saved servers always listed
    return o;
}

const char* SortName(net::SortKey k) {
    switch (k) {
    case net::SortKey::Players: return "Players";
    case net::SortKey::Name: return "Server Name";
    case net::SortKey::Map: return "Map";
    default: return "Ping";
    }
}

std::string Num(int n) { return std::to_string(n); }

const net::BrowserEntry* Selected() {
    if (!g.have_selected) return nullptr;
    for (const auto& e : g.entries)
        if (e.address == g.selected) return &e;
    return nullptr;
}

// the row cells of an entry: name, players, map, ping
std::array<std::string, 4> Cells(const net::BrowserEntry& e) {
    std::array<std::string, 4> c;
    std::string name = e.have_info ? e.info.name : "";
    if (name.empty()) {
        int f = SavedIndex(g_favorites, e.address);
        name = f >= 0 && !g_favorites[size_t(f)].name.empty() ? g_favorites[size_t(f)].name : e.address.ToString();
    }
    if (e.have_info && e.info.password) name = "* " + name;
    if (!e.problem.empty()) name = "! " + name;
    c[0] = name;
    c[1] = e.have_info ? Num(e.info.Total()) + "/" + Num(e.info.max_players) : "-";
    c[2] = e.have_info ? e.info.map : e.state == net::BrowserEntry::State::NoAnswer ? "No answer" : "...";
    c[3] = e.ping >= 0 ? Num(e.ping) : "-";
    return c;
}

// the table's rows made again: one per entry of the view (or one line saying why there are none)
void BuildRows(X86Context& ctx, uint32_t table) {
    uint32_t rows = table + 0x5C;
    Call(ctx, kRowsClear, {.ebx = rows});
    uint32_t scratch = Scratch(ctx);
    if (!scratch) return;
    uint32_t wstr = scratch + 0xF00;  // a temporary std::wstring (0x1C bytes) after the characters
    auto add_row = [&](const std::array<std::string, 4>& cells) {
        uint32_t row = kernel::CallGuest(ctx, kMalloc, {0x20});
        if (!row) return;
        std::memset(kernel::HostPtr(row), 0, 0x20);
        for (const std::string& text : cells) {
            std::memset(kernel::HostPtr(wstr), 0, 0x1C);
            W32(wstr + 0x18, 7);
            SetWide(ctx, wstr, text);
            Call(ctx, kAddCell, {.ecx = kCellColour, .edx = row}, {wstr, kCellColour2});
            if (R32(wstr + 0x18) >= 8) kernel::CallGuest(ctx, kFree, {R32(wstr + 4)});
        }
        Call(ctx, kRowsPush, {.ecx = row, .esi = rows});
        Call(ctx, kRowDestroy, {.eax = row});
        kernel::CallGuest(ctx, kFree, {row});
    };
    for (size_t i : g.view) add_row(Cells(g.entries[i]));
    if (g.view.empty()) {
        std::string why = g.busy ? "Searching..." : g.tab == 1 && g.masters == 0 ? "No master server is set."
                        : g.tab <= 1 ? "No servers found." : g.tab == 2 ? "No favorites yet." : "No recent servers.";
        add_row({why, "", "", ""});
    }
}

// the selection kept on the same server when the rows change; the scroll keeps it in view
void PlaceSelection(uint32_t table) {
    int count = int(g.view.size()), sel = 0;
    for (int i = 0; i < count; i++)
        if (g.have_selected && g.entries[g.view[size_t(i)]].address == g.selected) sel = i;
    if (count == 0) sel = 0;
    W32(table + 0x9C, uint32_t(sel));
    float scale, top, bottom;
    std::memcpy(&scale, kernel::HostPtr(table + 0x12C), 4);
    std::memcpy(&top, kernel::HostPtr(table + 0x30), 4);
    std::memcpy(&bottom, kernel::HostPtr(table + 0x38), 4);
    int height = int(float(int(R32(0x37A81C))) * scale) + int(R32(table + 0x128));
    int shown = height > 0 ? std::max(1, int(bottom - top) / height - 1) : 1;  // a row's height goes to the header
    int first = int(R32(table + 0x120));
    if (sel < first) first = sel;
    if (sel >= first + shown) first = sel - shown + 1;
    first = std::max(0, std::min(first, std::max(0, count - shown)));
    W32(table + 0x120, uint32_t(first));
    g.selected_row = sel;
}

void ShowTabs(X86Context& ctx, uint32_t screen) {
    SetLine(ctx, screen, kTitle, kTabTitles[g.tab]);
    for (int t = 0; t < kTabs; t++) {
        bool on = t == g.tab;
        // the current tab dark and in brackets, the others the menus' blue-grey (gold does not show on this art)
        if (on) SetLine(ctx, screen, kTab0 + t, std::string("[") + kTabNames[t] + "]", 24, 29, 33);
        else SetLine(ctx, screen, kTab0 + t, kTabNames[t], 54, 83, 107);
    }
}

void ShowActions(X86Context& ctx, uint32_t screen) {
    SetLine(ctx, screen, kSort, std::string("Sort: ") + SortName(g.options.sort) + (g.options.reverse ? " (reversed)" : ""));
    bool filtered = g.options.hide_full || g.options.hide_empty || g.options.password || !g.options.map.empty() ||
                    g.options.location;
    SetLine(ctx, screen, kFilters, filtered ? "Filters (on)" : "Filters");
    const net::BrowserEntry* e = Selected();
    bool fav = e && (e->sources & net::kFavorite);
    SetLine(ctx, screen, kFavorite, e ? (fav ? "Remove Favorite" : "Add to Favorites") : "");
}

void ShowStatus(X86Context& ctx, uint32_t screen) {
    std::string s;
    size_t answered = 0;
    for (size_t i : g.view) answered += g.entries[i].have_info;
    if (g.tab == 1 && g.masters == 0) s = "No master server set";
    else if (g.listing) s = "Getting the server list... " + Num(int(g.entries.size())) + " servers so far";
    else if (g.busy) s = "Searching... " + Num(int(answered)) + (answered == 1 ? " server" : " servers");
    else s = Num(int(g.view.size())) + (g.view.size() == 1 ? " server" : " servers");
    SetLine(ctx, screen, kStatus, s);
}

void ShowDetails(X86Context& ctx, uint32_t screen) {
    const net::BrowserEntry* e = Selected();
    std::string key = e ? e->address.ToString() + "/" + Num(int(g.version)) : "none";
    if (key == g.details_for) return;
    g.details_for = key;
    std::string l[8];
    if (e && e->have_info) {
        const net::ServerInfo& s = e->info;
        std::string mode = net::GameMode(s.map);
        l[0] = "Game Type: " + (mode.empty() ? std::string("Other") : mode);
        l[4] = "Players: " + Num(s.humans) + " + " + Num(s.bots) + (s.bots == 1 ? " bot" : " bots") + " of " +
               Num(s.max_players);
        l[1] = std::string("Dedicated Server: ") + (s.dedicated ? "Yes" : "No");
        l[5] = std::string("Password: ") + (s.password ? "Yes" : "No");
        l[2] = "Round Time: " + Num(s.round_seconds / 60) + (s.round_seconds % 60 ? ":" + Num(s.round_seconds % 60 / 10) +
               Num(s.round_seconds % 10) : "") + " min" + (s.time_limit ? ", map " + Num(s.time_limit) + " min" : "");
        l[6] = std::string("Player Collision: ") + (s.collision ? "On" : "Off") + (s.friendly_fire ? ", Friendly Fire" : "");
        l[3] = "Address: " + e->address.ToString() + (s.lan_only ? " (LAN only)" : "");
        l[7] = e->problem.empty() ? "Ping: " + (e->ping >= 0 ? Num(e->ping) + " ms" : std::string("-"))
                                  : "Warning: the server's identity changed";
    } else if (e) {
        l[0] = "Address: " + e->address.ToString();
        l[4] = e->state == net::BrowserEntry::State::NoAnswer ? "The server does not answer." : "Waiting for an answer...";
    }
    for (int i = 0; i < 8; i++) SetLine(ctx, screen, kDetail0 + i, l[i]);
}

// what the thread found, copied when it changed
void TakeSnapshot() {
    std::lock_guard<std::mutex> lock(g_shared.lock);
    if (g_shared.version == g.version) return;
    g.version = g_shared.version;
    g.entries = g_shared.entries;
    g.busy = g_shared.busy;
    g.listing = g_shared.listing;
    g.masters = g_shared.masters;
    g.masters_answered = g_shared.masters_answered;
}

// a favourite whose key is now proven at another address (it moved): its address follows (the key proves it is the same
// server; nothing is trusted on an address alone)
void FollowMovedFavorites() {
    bool changed = false;
    for (auto& f : g_favorites) {
        if (f.key.empty()) continue;
        const net::BrowserEntry* at = nullptr;
        bool answered_there = false;
        {
            std::lock_guard<std::mutex> lock(g_shared.lock);
            auto it = g_shared.resolved.find(f.address);
            for (const auto& e : g.entries) {
                if (it != g_shared.resolved.end() && e.address == it->second && e.have_info && e.key == f.key)
                    answered_there = true;
                if (e.have_info && e.key == f.key && e.problem.empty() && !(e.sources & net::kFavorite)) at = &e;
            }
        }
        if (answered_there || !at) continue;
        LOGI("browser: favorite \"%s\" moved from %s to %s (the same identity key)", f.name.c_str(), f.address.c_str(),
             at->address.ToString().c_str());
        f.address = at->address.ToString();
        AddAddress(f.address, net::kFavorite, f.key);
        changed = true;
    }
    if (changed) StoreSaved("favorite", g_favorites, kMaxFavorites);
}

// the list shown again when anything it depends on changed (at most four times a second while it changes)
void Update(X86Context& ctx, bool force) {
    uint32_t screen = FindScreen(kBrowser);
    if (!screen) return;
    uint32_t table = R32(screen + 0x380);
    TakeSnapshot();
    unsigned settings = pc::Version();
    bool tab_changed = g.shown_tab != g.tab;
    if (tab_changed || settings != g.shown_settings) {
        g.options = Options(g.tab);
        g.resort = true;
        ShowTabs(ctx, screen);
    }
    // the selection follows the table (the d-pad, the mouse): the server under it
    if (table && !tab_changed && g.selected_row >= 0) {
        int sel = int(R32(table + 0x9C));
        if (sel != g.selected_row && sel >= 0 && size_t(sel) < g.view.size()) {
            g.selected = g.entries[g.view[size_t(sel)]].address;
            g.have_selected = true;
            g.chosen = true;
            g.selected_row = sel;
            g.details_for = "-";
        }
    }
    double now = Now();
    bool changed = force || tab_changed || settings != g.shown_settings || g.version != g.shown_version;
    if (changed && (force || tab_changed || now - g.rebuilt_at >= 0.25) && table) {
        FollowMovedFavorites();
        std::vector<size_t> view = net::View(g.entries, g.options);
        // the same servers as shown: their order kept unless a sort, a refresh or the tab asked for it again
        if (!g.resort && view.size() == g.view_addresses.size()) {
            std::map<uint64_t, size_t> at;
            for (size_t i : view) at[g.entries[i].address.Key()] = i;
            std::vector<size_t> kept;
            for (const net::Address& a : g.view_addresses)
                if (auto it = at.find(a.Key()); it != at.end()) kept.push_back(it->second);
            if (kept.size() == view.size()) view = kept;
        }
        bool reordered = g.resort || view.size() != g.view.size();
        g.resort = false;
        g.view = view;
        g.view_addresses.clear();
        for (size_t i : g.view) g.view_addresses.push_back(g.entries[i].address);
        if (tab_changed) g.chosen = false;
        if (!g.have_selected || !g.chosen) {
            g.have_selected = !g.view.empty();
            if (g.have_selected) g.selected = g.entries[g.view[0]].address;
        }
        bool still = false;
        for (size_t i : g.view) still = still || g.entries[i].address == g.selected;
        if (!still && !g.view.empty()) {
            // the selected server left the view: the row now at its place
            size_t at = std::min<size_t>(size_t(std::max(0, g.selected_row)), g.view.size() - 1);
            g.selected = g.entries[g.view[at]].address;
        }
        g.have_selected = !g.view.empty();
        BuildRows(ctx, table);
        PlaceSelection(table);
        if (reordered) {  // the rows for the log (and the tests): when servers came, went or the order changed
            std::string rows;
            for (size_t i : g.view) {
                auto c = Cells(g.entries[i]);
                rows += " [" + c[0] + " | " + g.entries[i].address.ToString() + " | " + c[1] + " | " + c[2] + " | " + c[3] + "]";
            }
            LOGI("browser: %s: %zu rows%s", kTabTitles[g.tab], g.view.size(), rows.c_str());
        }
        g.rebuilt_at = now;
        g.shown_version = g.version;
        g.shown_settings = settings;
        g.shown_tab = g.tab;
        g.details_for = "-";
    }
    ShowStatus(ctx, screen);
    ShowActions(ctx, screen);
    ShowDetails(ctx, screen);
}

void Refresh() {
    g.refreshed_at = Now();
    g.resort = true;
    int tab = g.tab;
    Post([tab](net::Browser& b) {
        if (tab == 0) b.RefreshLan();
        else if (tab == 1) b.RefreshInternet();
        else b.RefreshSource(kTabSources[tab]);
    });
    LOGI("browser: %s refreshed", kTabNames[tab]);
}

void SetTab(int tab) {
    g.tab = (tab % kTabs + kTabs) % kTabs;
    Post([tab = g.tab](net::Browser& b) { b.WatchLan(tab == 0); });
    Refresh();
    LOGI("browser: %s", kTabTitles[g.tab]);
}

void Open(const std::string& screen) { csx::QueueEngineCommand("xbui open " + screen + "\n"); }

void Message(const std::string& a, const std::string& b = "", const std::string& c = "") {
    g_message[0] = a;
    g_message[1] = b;
    g_message[2] = c;
    LOGI("browser: message: %s %s %s", a.c_str(), b.c_str(), c.c_str());
    Open(kMessage);
}

// a password the engine's userinfo can carry (net_engine.cpp's rule): printable, no spaces, quotes, ';' or '\'
bool PasswordOk(const std::string& p) {
    bool ok = !p.empty() && p.size() <= 32;
    for (unsigned char c : p) ok = ok && c > 0x20 && c < 0x7F && c != '"' && c != ';' && c != '\\';
    return ok;
}

void AddRecent(const net::Address& a, const std::string& key, const std::string& name) {
    std::string text = a.ToString();
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(), [&](const net::SavedServer& s) { return s.address == text; }),
                   g_recent.end());
    g_recent.insert(g_recent.begin(), {text, key, name});
    if (g_recent.size() > kMaxRecent) g_recent.resize(kMaxRecent);
    StoreSaved("recent", g_recent, kMaxRecent);
}

// every check passed: the title relaunches into the server (net_session.cpp), its key pinned for the connection
void Connect() {
    std::string addr = g_join.address.ToString();
    net::KnownServers known((app::ExeDir() / "known_servers.txt").string());
    std::string error;
    if (known.Check(addr, g_join.key) != net::KnownServers::Result::Match && !known.Remember(addr, g_join.key, error))
        LOGW("browser: the server's key not remembered: %s", error.c_str());
    AddRecent(g_join.address, g_join.key, g_join.name);
#ifdef _WIN32
    _putenv_s("CSX_SERVER_KEY", g_join.key.c_str());
    _putenv_s("CSX_NET_PASSWORD", g_join.password.c_str());
    _putenv_s("CSX_BROWSER_JOIN", addr.c_str());
    _putenv_s("CSX_BROWSER_TAB", std::to_string(g.tab).c_str());
#else
    setenv("CSX_SERVER_KEY", g_join.key.c_str(), 1);
    if (g_join.password.empty()) unsetenv("CSX_NET_PASSWORD");
    else setenv("CSX_NET_PASSWORD", g_join.password.c_str(), 1);
    setenv("CSX_BROWSER_JOIN", addr.c_str(), 1);
    setenv("CSX_BROWSER_TAB", std::to_string(g.tab).c_str(), 1);
#endif
    LOGI("browser: joining \"%s\" at %s (key %s%s)", g_join.name.c_str(), addr.c_str(), g_join.key.c_str(),
         g_join.password.empty() ? "" : ", with a password");
    g_join.active = false;
    csx::RequestJoin(addr);
}

// the join of a server that answered: the checks, then the password or the connection
void JoinEntry(const net::BrowserEntry& e) {
    if (!e.have_info) {
        if (e.state == net::BrowserEntry::State::NoAnswer)
            return Message("The server at " + e.address.ToString() + " does not answer.",
                           "It may be off, or a firewall may block UDP port " + Num(e.address.port) + ".");
        return Message("Still waiting for the server to answer.", "Try again in a moment.");
    }
    const net::ServerInfo& s = e.info;
    if (s.protocol != 47) return Message("The server runs another version of the game.", "(protocol " + Num(s.protocol) + ")");
    if (s.Full()) return Message("The server is full.", Num(s.Total()) + " of " + Num(s.max_players) + " player slots are taken.");
    // the whole local party goes (N4.8): a slot for every member, or none of them
    int party = csx::SplitPlayer() ? csx::SplitPlayers() : 1;
    if (party > 1 && int(s.max_players) - int(s.Total()) < party)
        return Message("The server has room for " + Num(int(s.max_players) - int(s.Total())) + " more players.",
                       "Your party has " + Num(party) + ": choose a server with more free slots.");
    g_join = {};
    g_join.active = true;
    g_join.address = e.address;
    g_join.name = s.name;
    g_join.key = e.key;
    std::string addr = e.address.ToString();
    // a pinned key (net_server_key) must be the server's; else the remembered one, or the favourite's
    std::string pinned = pc::Get("net_server_key", "");
    if (!pinned.empty() && pinned != e.key) {
        g_join.active = false;
        return Message("The server's identity is not the pinned key.", "Pinned (net_server_key): " + pinned.substr(0, 32) + "...",
                       "The server: " + e.key.substr(0, 32) + "...");
    }
    net::KnownServers known((app::ExeDir() / "known_servers.txt").string());
    std::string old;
    bool changed = known.Check(addr, e.key, &old) == net::KnownServers::Result::Changed;
    if (!changed && !e.problem.empty()) {
        changed = true;
        old = e.expected_key;
    }
    if (changed) {
        g_message[0] = old;
        g_message[1] = e.key;
        LOGW("browser: %s's identity key changed (remembered %s, now %s): asking", addr.c_str(), old.c_str(), e.key.c_str());
        return Open(kIdentity);
    }
    if (s.password) return Open(kPassword);
    Connect();
}

// ---- the keyboard screens' text -------------------------------------------------------------------------------------------
std::string EditText(const char* screen_name) {
    uint32_t screen = FindScreen(screen_name);
    uint32_t box = screen ? R32(screen + kEditBox) : 0;
    return box ? ReadWide(box + 0x34) : std::string();
}
void ClearEdit(X86Context& ctx, const char* screen_name, const std::string& text = "") {
    uint32_t screen = FindScreen(screen_name);
    uint32_t box = screen ? R32(screen + kEditBox) : 0;
    if (!box) return;
    SetWide(ctx, box + 0x34, text);
    W32(box + 0x70, uint32_t(text.size()));  // the cursor at the end
    // +0x6C set: the first key typed replaces the text shown (as the profile name's default is replaced)
    static_cast<uint8_t*>(kernel::HostPtr(box + 0x6C))[0] = text.empty() ? 0 : 1;
}

void ShowMessageScreen(X86Context& ctx, const char* name) {
    uint32_t s = FindScreen(name);
    if (!s) return;
    if (std::string(name) == kIdentity) {
        SetLine(ctx, s, 1, "This server's identity key has changed.");
        SetLine(ctx, s, 2, "It may be another server using its address, or its");
        SetLine(ctx, s, 3, "owner made a new key. Accept only if you trust the change.");
        SetLine(ctx, s, 4, "Remembered: " + g_message[0].substr(0, 32));
        SetLine(ctx, s, 5, "            " + g_message[0].substr(32));
        SetLine(ctx, s, 6, "Now:        " + g_message[1].substr(0, 32));
        SetLine(ctx, s, 7, "            " + g_message[1].substr(32));
        return;
    }
    if (std::string(name) == kContacting) {
        SetLine(ctx, s, 1, "Contacting " + g_contact.text + "...");
        return;
    }
    for (int i = 0; i < 3; i++) SetLine(ctx, s, 1 + i, g_message[i]);
}

void LeaveBrowser() {
    Post([](net::Browser& b) { b.WatchLan(false); });
}

// a typed address being contacted: answered, refused or timed out
void ContactFrame() {
    if (!g_contact.active) return;
    TakeSnapshot();
    std::optional<net::Address> a;
    std::string why;
    {
        std::lock_guard<std::mutex> lock(g_shared.lock);
        if (auto it = g_shared.resolved.find(g_contact.text); it != g_shared.resolved.end()) a = it->second;
        if (auto it = g_shared.unresolved.find(g_contact.text); it != g_shared.unresolved.end()) why = it->second;
    }
    if (!why.empty()) {
        g_contact.active = false;
        csx::QueueEngineCommand("xbui back\n");
        return Message("Cannot find the server " + g_contact.text + ".", "Check the address and try again.");
    }
    for (const auto& e : g.entries) {
        if (!a || !(e.address == *a)) continue;
        if (e.have_info || e.state == net::BrowserEntry::State::NoAnswer) {
            g_contact.active = false;
            csx::QueueEngineCommand("xbui back\n");
            return JoinEntry(e);
        }
    }
    if (Now() - g_contact.started > 6) {
        g_contact.active = false;
        csx::QueueEngineCommand("xbui back\n");
        Message("No server answered at " + g_contact.text + ".", "It may be off, or a firewall may block it.");
    }
}

}  // namespace

// ---- the menu text ---------------------------------------------------------------------------------------------------------
namespace {

std::string Q(const std::string& s) { return "\"" + s + "\""; }

std::string Text(float x, float y, const std::string& s, float scale = 0.75f, const char* colour = "54 83 107 255") {
    char b[160];
    std::snprintf(b, sizeof(b), "\tBackgroundText \"%s %.0f %.0f 400 30 %.3g\"\t", colour, x, y, scale);
    return b + Q(s) + "\r\n";
}

std::string HelpRow(const std::string& line1, const std::string& line2 = "") {
    std::string t = "\t\t\tTable\r\n\t\t\t{\r\n\t\t\t\tLayout\r\n\t\t\t\t{\r\n\t\t\t\t\tColumnWidthPercentage \"100\"\r\n"
                    "\t\t\t\t\tTableLocation \"75 361\"\r\n\t\t\t\t\tTableSize \"476 40\"\r\n\t\t\t\t\tTableRowSpacing \"1\"\r\n"
                    "\t\t\t\t\tTableTextScale \"0.75\"\r\n\t\t\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n\t\t\t\t}\r\n";
    for (const std::string& l : {line1, line2})
        if (!l.empty()) t += "\t\t\t\tRow\r\n\t\t\t\t{\r\n\t\t\t\t\tCellFixed " + Q(l) + "\r\n\t\t\t\t}\r\n";
    return t + "\t\t\t}\r\n";
}

std::string MenuRow(const std::string& label, const std::string& command, const std::string& h1, const std::string& h2 = "") {
    return "\t\tRow\r\n\t\t{\r\n" + HelpRow(h1, h2) + "\t\t\tCommandSelect " + Q(command) + "\r\n\t\t\tCellFixed " +
           Q(label) + "\r\n\t\t}\r\n";
}

std::string Pie(bool tabs) {
    return std::string("\tGeneric_4_1\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveUp\"\r\n\t}\r\n\tGeneric_4_2\r\n\t{\r\n") +
           (tabs ? "\t\tCommandHighlight\t\"!pc_browser tab -1\"\r\n" : "") +
           "\t}\r\n\tGeneric_4_3\r\n\t{\r\n\t\tCommandHighlight\t\"tableMoveDown\"\r\n\t}\r\n\tGeneric_4_4\r\n\t{\r\n" +
           (tabs ? "\t\tCommandHighlight\t\"!pc_browser tab 1\"\r\n" : "") + "\t}\r\n";
}

// System Link's rows: the PC's ways into a match, then the console's own Host a Match
std::string MultiplayerTable() {
    return MenuRow("LAN Matches", "!pc_browser open 0; xbui open SuperLauncher_PCBrowser",
                   "Find and join a match on your local network.") +
           MenuRow("Internet Matches", "!pc_browser open 1; xbui open SuperLauncher_PCBrowser",
                   "Find and join a match listed by a master server.") +
           MenuRow("Favorites", "!pc_browser open 2; xbui open SuperLauncher_PCBrowser",
                   "Join one of the servers you saved.") +
           MenuRow("Recent Servers", "!pc_browser open 3; xbui open SuperLauncher_PCBrowser",
                   "Join a server you played on lately.") +
           MenuRow("Join by Address", "!pc_browser address_open; xbui open SuperLauncher_PCAddress",
                   "Type a server's address (and port) to join it.") +
           MenuRow("Host a Match", "open SuperLauncher_LocalServerOptions",
                   "Create and host your own match for your network.");
}

std::string BrowserScreen() {
    std::string t = std::string("\r\n") + kBrowser + "\r\n{\r\n"
        "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
        "\tPieceOffsetDegrees \"90\"\r\n\tCachedImage \"whitebox\t\t\t\t255 255 255 255\"\r\n"
        "\tBackgroundImage \"Back_XBL_OptimatchResults\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"Cancel_Join_RefreshResults_HostAMatch 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnEntry \"!pc_browser enter\"\r\n"
        "\tOnButtonDownA \"!pc_browser join\"\r\n\tOnButtonDownStart \"!pc_browser join\"\r\n"
        "\tOnButtonDownX \"!pc_browser refresh\"\r\n"
        "\tOnButtonDownY \"!pc_browser leave; xbui swap SuperLauncher_LocalServerOptions\"\r\n"
        "\tOnButtonDownB \"!pc_browser leave; xbui back\"\r\n"
        "\tOnButtonDownBack \"open SuperLauncher_PCFilters\"\r\n"
        "\tOnButtonDownWhite \"!pc_browser favorite\"\r\n\tOnButtonDownBlack \"!pc_browser sort\"\r\n"
        "\tOnButtonDownLT \"!pc_browser tab -1\"\r\n\tOnButtonDownRT \"!pc_browser tab 1\"\r\n"
        "\tCommandSimulate1 \"!pc_browser frame\"\r\n";
    // the lines, in Line's order
    t += Text(65, 45, "LAN Matches", 1.0f);
    for (int i = 0; i < kTabs; i++) t += Text(kTabX[i], kTabY, kTabNames[i]);
    t += Text(kStatusX, kTabY, " ");  // the status
    for (int i = 0; i < 8; i++) t += Text(kDetailX[i / 4], kDetailY + kDetailStep * float(i % 4), " ");
    for (int i = 0; i < 3; i++) t += Text(kActionX[i], kActionY, " ");
    t += Pie(true);
    char layout[512];
    std::snprintf(layout, sizeof(layout),
                  "\tTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n"
                  "\t\t\tColumnWidthPercentage \"49\"\r\n\t\t\tColumnJustify \"truncated\"\r\n"
                  "\t\t\tColumnWidthPercentage \"15\"\r\n\t\t\tColumnJustify \"center\"\r\n"
                  "\t\t\tColumnWidthPercentage \"21\"\r\n\t\t\tColumnJustify \"center\"\r\n"
                  "\t\t\tColumnWidthPercentage \"15\"\r\n\t\t\tColumnJustify \"center\"\r\n"
                  "\t\t\tTableVerticalWrap\r\n\t\t\tTableTextScale \"0.75\"\r\n\t\t\tTableLocation \"%.0f %.0f\"\r\n"
                  "\t\t\tTableSize \"%.0f %.0f\"\r\n\t\t\tTableRowSpacing \"4\"\r\n",
                  kTableX, kTableY, kTableW, kTableH);
    t += layout;
    t += "\t\t\tHighlightImage \"Secondary_Highlight 1.0 1.0 1.0 1.0 -8.0 -1.0 540.0 27.0\"\r\n"
         "\t\t\tNonSelectFG \"0.24 0.29 0.33 1\"\r\n\t\t\tHeaderFG \"0.24 0.29 0.33 1\"\r\n\t\t}\r\n"
         "\t\tHeader\r\n\t\t{\r\n\t\t\tCellFixed \"Server Name\"\r\n\t\t\tCellFixed \"Players\"\r\n"
         "\t\t\tCellFixed \"Map\"\r\n\t\t\tCellFixed \"Ping\"\r\n\t\t}\r\n"
         "\t\tRow\r\n\t\t{\r\n\t\t\tCellFixed \"Please wait...\"\r\n\t\t\tCellFixed \"....\"\r\n"
         "\t\t\tCellFixed \"Searching\"\r\n\t\t\tCellFixed \"....\"\r\n\t\t}\r\n\t}\r\n}\r\n";
    return t;
}

// the profile keyboard, for a server's address or its password
std::string KeyboardScreen(const char* name, const char* title, const char* command, const char* hint) {
    return std::string("\r\n") + name + "\r\n{\r\n"
        "\tMenuType \"superlauncher\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tHighlightSound \"menu/buy_hover.wav\"\r\n"
        "\tErrorSound \"menu/buy_error.wav\"\r\n"
        "\tBackgroundImage \"CSX_Keyboard_base\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tCommandSelect \"!" + command + "\"\r\n\tOnButtonDownA \" \"\r\n\tOnButtonDownStart \"!" + command + "\"\r\n"
        "\tOnButtonDownB \"!pc_browser keyboard_back; xbui back\"\r\n"
        "\tBackgroundText\t\"54 83 107 255 65 45 400 30\"\t" + Q(title) + "\r\n" +
        Text(75, 360, hint) +
        "\tSelectBox0 \"CSX_Done_Highlight      255 255 255 255 | 62 11 52 15\"\r\n"
        "\tSelectBox1 \"CSX_s_Key_Highlight     255 255 255 255 | 13 13 11 13\"\r\n"
        "\tSelectBox2 \"CSX_Arrow_Highlight     255 255 255 255 | 27 13 26 12\"\r\n"
        "\tSelectBox3 \"CSX_Backspace_Highlight 255 255 255 255 | 55 13 53 13\"\r\n"
        "\tSelectBox4 \"CSX_Space_Highlight     255 255 255 255 | 19 13 18 12\"\r\n"
        "\tEditBox\t\t\t\"54 83 107 0 255 255 255 255 110 113 460 26  1\" \"*\"\r\n"
        "\tVirtualKeyboard\t\"54 83 107 0 255 255 255 255  70 151 500 200 1\"\r\n"
        "\tBackgroundImage \"Back_Select 200 200 200 255 0 411 640 69\"\r\n}\r\n";
}

// a message box as the console's (HostFull, LostHost): its lines centred, Continue or Cancel / Accept
std::string MessageScreen(const char* name, const char* title, int lines, const char* legend, const char* a,
                          const char* b, bool left = false) {
    std::string t = std::string("\r\n") + name + "\r\n{\r\n"
        "\tmenutype \"generic\"\r\n\tEnterSound \"menu/buy_submenu.wav\"\r\n\tModal \"200\"\r\n"
        "\tBackgroundImage \"Back_2ndary.1Pane.NoHelp\t\t200 200 200 255\t\t0   0 640 480\"\r\n"
        "\tBackgroundImage \"" + std::string(legend) + " 200 200 200 255 0 411 640 69\"\r\n"
        "\tOnEntry \"!pc_browser show " + name + "\"\r\n"
        "\tOnButtonDownA \"" + a + "\"\r\n\tOnButtonDownB \"" + b + "\"\r\n"
        "\tBackgroundText \"54 83 107 255 65 45 400 30\"\t" + Q(title) + "\r\n";
    for (int i = 0; i < lines; i++) {
        char b2[128];
        std::snprintf(b2, sizeof(b2), "\tBackgroundText \"54 83 107 255 %d %d 520 30 0.8\"\t\" \"\r\n", left ? 80 : 80,
                      (left ? 140 : 190) + i * 26);
        t += b2;
    }
    return t + "}\r\n";
}

}  // namespace

// the front end's text with the multiplayer screens: System Link renamed Multiplayer with the PC's rows, and the
// browser's screens added; the text unchanged when System Link is not laid out as expected
std::string ExtendMultiplayer(const std::string& text) {
    size_t screen = text.find("\nSuperLauncher_SystemLink\r\n");
    size_t table = screen == std::string::npos ? screen : text.find("\tTable\r\n", screen);
    size_t layout_end = table == std::string::npos ? table : text.find("\t\t}\r\n", text.find("Layout", table));
    size_t next = screen == std::string::npos ? screen : text.find("\nSuperLauncher_", screen + 1);
    size_t table_end = next == std::string::npos ? next : text.rfind("\t}\r\n}\r\n", next);
    size_t title = screen == std::string::npos ? screen : text.find("\"System Link\"", screen);
    size_t main_row = text.find("CellFixed \"System Link\"");
    size_t main_help = text.find("\"Compete with other players on System Link.\"");
    if (layout_end == std::string::npos || table_end == std::string::npos || title > table || main_row == std::string::npos ||
        main_help == std::string::npos || table_end < layout_end) {
        LOGW("browser: System Link not laid out as expected: no PC multiplayer menus");
        return text;
    }
    layout_end += std::strlen("\t\t}\r\n");
    std::string out = text.substr(0, title) + "\"Multiplayer\"" + text.substr(title + 13, layout_end - title - 13) +
                      MultiplayerTable() + text.substr(table_end);
    out.replace(out.find("CellFixed \"System Link\""), std::strlen("CellFixed \"System Link\""), "CellFixed \"Multiplayer\"");
    std::string help = "\"Compete with other players on System Link.\"";
    out.replace(out.find(help), help.size(), "\"Play with others on your network or the internet.\"");
    out += BrowserScreen();
    out += KeyboardScreen(kAddress, "Join by Address", "pc_browser address", "A server's address, with its port if not 27015.");
    out += KeyboardScreen(kPassword, "Server Password", "pc_browser password", "This server asks for a password.");
    // Continue closes them once A and B are released (PcBrowserMenuFrame): closed on the press, the screen under them
    // took the same held A as its own press (the main menu's first row opened, N4.7)
    out += MessageScreen(kMessage, "Multiplayer", 3, "Continue", "!pc_browser message_done", "!pc_browser message_done");
    out += MessageScreen(kServerMessage, "Match Server", 3, "Continue", "!pc_browser message_done", "!pc_browser message_done");
    out += MessageScreen(kIdentity, "Server Identity", 7, "Cancel_Accept", "!pc_browser trust", "!pc_browser untrust; xbui back",
                         true);
    out += MessageScreen(kContacting, "Join by Address", 1, "Cancel", "!pc_browser contact_cancel; xbui back",
                         "!pc_browser contact_cancel; xbui back");
    return out;
}

// the profile keyboard screen and its edit box's text, for other PC screens (pc_split.cpp's player names)
std::string PcKeyboardScreen(const char* name, const char* title, const char* command, const char* hint) {
    return KeyboardScreen(name, title, command, hint);
}
std::string PcEditText(const char* screen) { return EditText(screen); }
void PcClearEdit(X86Context& ctx, const char* screen, const std::string& text) { ClearEdit(ctx, screen, text); }

// a BackgroundText line of any screen (pc_split.cpp's status lines)
void PcSetScreenLine(X86Context& ctx, const char* screen, int index, const std::string& text) {
    SetLine(ctx, FindScreen(screen), index, text);
}
// the same with its colour (0-255; alpha 0 hides it): the party's panels (pc_party.cpp)
void PcSetScreenLineColour(X86Context& ctx, const char* screen, int index, const std::string& text, float r, float g, float b,
                           float a) {
    SetLine(ctx, FindScreen(screen), index, text, r, g, b, a);
}
std::string PcCurrentScreen() { return CurrentScreen(); }

// the browser's mouse targets (pc_mouse.cpp): the tabs, the header's columns, the action lines; true when (x, y) hit
// one (its command queued)
bool PcBrowserClick(const std::string& screen, float x, float y) {
    if (screen != kBrowser) return false;
    for (int t = 0; t < kTabs; t++)
        if (x >= kTabX[t] - 4 && x < kTabX[t] + 90 && y >= kTabY - 2 && y < kTabY + 18) {
            csx::QueueEngineCommand("pc_browser tab_to " + std::to_string(t) + "\n");
            return true;
        }
    if (y >= kActionY - 2 && y < kActionY + 18) {
        const char* cmd[3] = {"pc_browser sort", "xbui open SuperLauncher_PCFilters", "pc_browser favorite"};
        for (int i = 2; i >= 0; i--)
            if (x >= kActionX[i] - 4) {
                csx::QueueEngineCommand(std::string(cmd[i]) + "\n");
                return true;
            }
    }
    // the header row: its columns sort (49 / 15 / 21 / 15 percent of the table: the art's dividers)
    if (y >= kTableY + 4 && y < kTableY + 30 && x >= kTableX && x < kTableX + kTableW) {  // the header: 136 to 158
        float f = (x - kTableX) / kTableW;
        const char* key = f < 0.49f ? "name" : f < 0.64f ? "players" : f < 0.85f ? "map" : "ping";
        csx::QueueEngineCommand(std::string("pc_browser sort_by ") + key + "\n");
        return true;
    }
    return false;
}

bool PcBrowserScreen(const std::string& screen) {
    return screen.rfind("SuperLauncher_PC", 0) == 0 && screen != "SuperLauncher_PCOptions" &&
           screen != "SuperLauncher_PCControls";
}

// [[hook]] at 0xD04CD in the keyboard's insert (0xD04A0: cmpl $0xf, 0x48(%esi) - the profile name's 15 characters):
// the address and password screens take more (63 and 32), a split-screen player's name 20; true continues at the
// insert (0xD04D3)
bool PcEditBoxRoom(X86Context& ctx, uint8_t*) {
    std::string s = CurrentScreen();
    uint32_t limit = s == kAddress ? 63 : s == kPassword || s == "SuperLauncher_PCMatchPassword" ? 32
                   : s == "SuperLauncher_PCSplitName" || s == "SuperLauncher_PCProfileName" || s == "SuperLauncher_PCMatchRulesetName" ? 20
                   : 0;
    return limit && R32(ctx.esi + 0x48) < limit;
}

// the console commands of the browser's screens (pc_menu.cpp's PcConsoleCommand hands them here)
void PcBrowserCommand(X86Context& ctx, const std::vector<std::string>& a) {
    std::string cmd = a.size() > 1 ? a[1] : "";
    std::string arg = a.size() > 2 ? a[2] : "";
    StartThread();
    LoadAllSaved();
    if (cmd == "open") {
        g.tab = std::clamp(std::atoi(arg.c_str()), 0, kTabs - 1);
        g.shown_tab = -1;
        g.have_selected = false;
        SetTab(g.tab);
    } else if (cmd == "enter") {
        Update(ctx, true);
    } else if (cmd == "frame") {
        Update(ctx, false);
    } else if (cmd == "refresh") {
        Refresh();
    } else if (cmd == "tab") {
        SetTab(g.tab + (std::atoi(arg.c_str()) < 0 ? -1 : 1));
        Update(ctx, true);
    } else if (cmd == "tab_to") {
        SetTab(std::atoi(arg.c_str()));
        Update(ctx, true);
    } else if (cmd == "sort" || cmd == "sort_by") {
        static const char* keys[] = {"ping", "players", "name", "map"};
        std::string now = pc::Get("browser_sort", "ping");
        if (cmd == "sort_by" && now == arg) {
            pc::Set("browser_order", pc::Get("browser_order", "0") == "1" ? "0" : "1");  // the same column: reversed
        } else {
            std::string next = arg;
            if (cmd == "sort") {
                int i = 0;
                while (i < 4 && now != keys[i]) i++;
                next = keys[(i + 1) % 4];
            }
            pc::Set("browser_sort", next);
            pc::Set("browser_order", "0");
        }
        pc::Save();
        g.resort = true;
        Update(ctx, true);
    } else if (cmd == "favorite") {
        const net::BrowserEntry* e = Selected();
        if (!e) return;
        net::Address addr = e->address;
        int i = SavedIndex(g_favorites, addr);
        if (e->sources & net::kFavorite) {
            if (i >= 0) g_favorites.erase(g_favorites.begin() + i);
            Post([addr](net::Browser& b) { b.Remove(addr, net::kFavorite); });
            LOGI("browser: %s removed from the favorites", addr.ToString().c_str());
        } else if (g_favorites.size() < kMaxFavorites) {
            // the key the server proved, so the favourite names that identity, not only the address
            net::SavedServer s{addr.ToString(), e->key, e->have_info ? e->info.name : ""};
            g_favorites.push_back(s);
            {
                std::lock_guard<std::mutex> lock(g_shared.lock);
                g_shared.resolved[s.address] = addr;
            }
            Post([addr, key = e->key](net::Browser& b) { b.Add(addr, net::kFavorite, key); });
            LOGI("browser: %s added to the favorites (key %s)", addr.ToString().c_str(), e->key.empty() ? "unknown" : e->key.c_str());
        }
        StoreSaved("favorite", g_favorites, kMaxFavorites);
    } else if (cmd == "join") {
        const net::BrowserEntry* e = Selected();
        if (!e) return Message("Choose a server first.");
        JoinEntry(*e);
    } else if (cmd == "leave") {
        LeaveBrowser();
    } else if (cmd == "address_open") {
        ClearEdit(ctx, kAddress, pc::Get("browser_last_address", ""));
    } else if (cmd == "address") {
        std::string text = EditText(kAddress);
        text.erase(0, text.find_first_not_of(' '));
        while (!text.empty() && text.back() == ' ') text.pop_back();
        bool ok = !text.empty() && text.size() <= 63 &&
                  text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") == std::string::npos;
        if (!ok) {
            csx::QueueEngineCommand("xbui back\n");
            return Message("That is not an address.", "Type a name or numbers, like 192.168.1.20:27015.");
        }
        pc::Set("browser_last_address", text);
        pc::Save();
        LOGI("browser: contacting %s", text.c_str());
        g_contact = {true, text, Now()};
        AddAddress(text, net::kDirect, "");
        csx::QueueEngineCommand("xbui back\n");
        Open(kContacting);
    } else if (cmd == "contact_cancel") {
        g_contact.active = false;
    } else if (cmd == "password") {
        std::string pw = EditText(kPassword);
        if (!g_join.active) return;
        if (!PasswordOk(pw))
            return Message("That password cannot be sent.", "At most 32 characters; no spaces, quotes, ';' or '\\'.");
        g_join.password = pw;
        ClearEdit(ctx, kPassword);
        csx::QueueEngineCommand("xbui back\n");
        Connect();
    } else if (cmd == "keyboard_back") {
        if (CurrentScreen() == kPassword) g_join.active = false;
        ClearEdit(ctx, kPassword);
    } else if (cmd == "trust") {
        // the player accepted the new key: remembered for the address (and the favourite), then the join goes on
        if (!g_join.active) return;
        net::KnownServers known((app::ExeDir() / "known_servers.txt").string());
        std::string error;
        if (!known.Remember(g_join.address.ToString(), g_join.key, error)) LOGW("browser: %s", error.c_str());
        int i = SavedIndex(g_favorites, g_join.address);
        if (i >= 0) {
            g_favorites[size_t(i)].key = g_join.key;
            StoreSaved("favorite", g_favorites, kMaxFavorites);
            Post([a = g_join.address, k = g_join.key](net::Browser& b) {
                b.Add(a, net::kFavorite, k);
                b.Refresh(a);
            });
        }
        LOGW("browser: the player trusted %s's new identity key %s", g_join.address.ToString().c_str(), g_join.key.c_str());
        csx::QueueEngineCommand("xbui back\n");
        // its answer's flags still apply: a password asked, then the connection
        bool password = false;
        for (const auto& e : g.entries)
            if (e.address == g_join.address) password = e.have_info && e.info.password;
        if (password) Open(kPassword);
        else Connect();
    } else if (cmd == "untrust") {
        LOGI("browser: the changed identity of %s was not trusted: not joined", g_join.address.ToString().c_str());
        g_join.active = false;
    } else if (cmd == "show") {
        ShowMessageScreen(ctx, arg.c_str());
    } else if (cmd == "message_done") {
        LOGI("browser: the message's Continue (%s): it closes once A and B are released", CurrentScreen().c_str());
        g_message_closing = true;
    } else {
        LOGW("browser: unknown command %s", cmd.c_str());
    }
}

namespace csx {
bool BrowserKeys() { return g_browser_keys; }
}  // namespace csx

// every menu frame (pc_menu.cpp's PcMenuFrame): a typed address's contact; the way back into the browser after a join
// that ended (net_session.cpp left a message: the front end opens Multiplayer, the browser and the message)
void PcBrowserMenuFrame(X86Context& ctx) {
    (void)ctx;
    // (and the party's screens, N4.8: Escape goes back there too)
    std::string now = CurrentScreen();
    g_browser_keys = (PcBrowserScreen(now) || (now.rfind("SuperLauncher_PCParty", 0) == 0 && now != "SuperLauncher_PCPartyName") ||
                      now == "SuperLauncher_PCProfiles" || now.rfind("SuperLauncher_PCLobby", 0) == 0 ||
                      now == "SuperLauncher_PCRules" || now == "SuperLauncher_PCProfile" ||
                      now.rfind("SuperLauncher_PCMatch", 0) == 0) &&
                     !csx::ConsoleOpen() && !csx::TextEntryOpen();
    ContactFrame();
    if (g_message_closing) {
        // released for 150 ms: the title's menus read the pad on their own a frame or more later, and a screen that
        // opens while they still see A down takes it as a new press
        static auto released_since = std::chrono::steady_clock::time_point{};
        bool held = false;
        for (int port = 0; port < 4; port++) {
            input::Pad p = input::Poll(port);
            held = held || (p.connected && (p.analog[0] || p.analog[1]));
        }
        auto now = std::chrono::steady_clock::now();
        if (held || released_since == std::chrono::steady_clock::time_point{}) released_since = held ? decltype(now){} : now;
        if (!held && now - released_since >= std::chrono::milliseconds(150)) {
            released_since = {};
            g_message_closing = false;
            LOGI("browser: the message closed (%s)%s%s", CurrentScreen().c_str(), g_message_under.empty() ? "" : ", back to ",
                 g_message_under.c_str());
            csx::QueueEngineCommand("xbui back\n");
            g_message_under.clear();
        }
    }
    // the match's local server did not start, or stopped by itself (net_session.cpp): why, on the first front end
    // screen (the main screen, or the one the title's own return restores)
    // once the front end has settled (1.5 s after its first screen: its boot opens the main screen again over anything
    // opened sooner)
    static auto front_end_since = std::chrono::steady_clock::time_point{};
    bool front_end = CurrentScreen().rfind("SuperLauncher_", 0) == 0;
    if (front_end && front_end_since == std::chrono::steady_clock::time_point{}) front_end_since = std::chrono::steady_clock::now();
    bool settled = front_end && std::chrono::steady_clock::now() - front_end_since > std::chrono::milliseconds(1500);
    if (const char* m = std::getenv("CSX_LOCAL_SERVER_MESSAGE"); m && *m && settled) {
        std::string message = m;
#ifdef _WIN32
        _putenv_s("CSX_LOCAL_SERVER_MESSAGE", "");
#else
        unsetenv("CSX_LOCAL_SERVER_MESSAGE");
#endif
        LOGI("browser: the match server's message: %s", message.c_str());
        for (int i = 0; i < 3; i++) {
            size_t cut = message.size() > 52 ? message.rfind(' ', 52) : std::string::npos;
            g_message[i] = cut == std::string::npos || i == 2 ? message.substr(0, 60) : message.substr(0, cut);
            message = cut == std::string::npos || i == 2 ? "" : message.substr(cut + 1);
        }
        g_message_under = CurrentScreen();
        csx::QueueEngineCommand(std::string("xbui open ") + kServerMessage + "\n");
    }
    static bool returned = false;
    if (returned) return;
    std::string screen = CurrentScreen();
    // split-screen player 1 is in the menus, whichever front end screen it is back on (the title's last menu, the
    // party's lobby): the other local players' match is over (the compositor ends them; those who left it leave the
    // party, N4.8.1)
    static bool told = false;
    if (!told && screen.rfind("SuperLauncher_", 0) == 0 && csx::SplitPlayer() == 1) {
        told = true;
        csx::SplitSend(csx::kSplitMatch, "menu");
    }
    if (screen != "SuperLauncher_MainScreen") return;
    returned = true;
    const char* join = std::getenv("CSX_BROWSER_JOIN");
    if (!join) return;
    std::string message = std::getenv("CSX_BROWSER_MESSAGE") ? std::getenv("CSX_BROWSER_MESSAGE") : "";
    int tab = std::getenv("CSX_BROWSER_TAB") ? std::atoi(std::getenv("CSX_BROWSER_TAB")) : 0;
    // the join's settings end with it (a later match from the menus is not that server's)
#ifdef _WIN32
    for (const char* v : {"CSX_BROWSER_JOIN", "CSX_BROWSER_MESSAGE", "CSX_SERVER_KEY", "CSX_NET_PASSWORD", "CSX_BROWSER_TAB"})
        _putenv_s(v, "");
#else
    for (const char* v : {"CSX_BROWSER_JOIN", "CSX_BROWSER_MESSAGE", "CSX_SERVER_KEY", "CSX_NET_PASSWORD", "CSX_BROWSER_TAB"})
        unsetenv(v);
#endif
    LOGI("browser: back from %s%s%s", join, message.empty() ? "" : ": ", message.c_str());
    csx::QueueEngineCommand("SystemLinkSetup\nxbui open SuperLauncher_SystemLink\npc_browser open " +
                            std::to_string(std::clamp(tab, 0, kTabs - 1)) + "\nxbui open SuperLauncher_PCBrowser\n");
    if (!message.empty()) {
        size_t cut = message.size() > 52 ? message.rfind(' ', 52) : std::string::npos;
        if (cut == std::string::npos) g_message[0] = message, g_message[1].clear();
        else g_message[0] = message.substr(0, cut), g_message[1] = message.substr(cut + 1);
        g_message[2].clear();
        csx::QueueEngineCommand(std::string("xbui open ") + kMessage + "\n");
    }
}
