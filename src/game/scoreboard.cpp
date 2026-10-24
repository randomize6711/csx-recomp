// The scoreboard for 65 players (FINDINGS.md "Scoreboard"): the original Xbox screen, its fonts, colours and rows, with
// a Ping column and scrolling. The Scoreboard screen (InGameUI*.txt) is a PassiveTable that the HUD's ScorePanel
// fills every frame (FillGrid 0x1B9820: for each sorted row by its kind, 1 a team header 0x1B99C0, 2 the spectators'
// header 0x1B9E00, otherwise a player row 0x1BA000 (a blank spacer row makes nothing there), each a list of cells made
// by 0xDC540 / 0xDC440 from a std::wstring); its own sort is kept (score down, then deaths up, then player number).
// - Columns: the screen's layout gains a sixth column, Ping, between Deaths and the voice icon (bomb icon | Name |
//   Score | Deaths | Ping | voice icon). A player row's Ping cell is the row's Deaths cell made a second time (its
//   code run again, hook 0x1BA4C3) with the text replaced before the cell is made (hook 0x1BA4AA): the same font,
//   colour and dimming as the row's other numbers. A row without a team (an unassigned player) has blank cells; its
//   Ping cell is its third blank cell made again (0x1BA616 / 0x1BA601).
// - Ping: "BOT" for a bot, by the "*bot" key in the player's userinfo (cl.players[i].userinfo, as the PC game reads
//   it: the server adds it to a fake client's userinfo when the engine creates one, ScoreBotKey below); otherwise the ping the server measured and
//   sent (svc_pings, cl.players[i].ping +0x128), "-" until a measurement for that player has arrived (hook 0x31A30
//   marks each slot svc_pings writes, with the player's userid: a new player in the slot is unmeasured again).
// - Rows: the table shows a fixed number of lines (kLines). With more rows, the hook at the row loop (0x1B98A0) shows
//   a window from a scroll offset (scoreboard_rows.h): the team header of the first row stays at the top, the last
//   line tells how many players are below (that row made as a spectators' header, its kind pointed at a 2 and put back
//   at the loop's next step 0x1B9979, the count in its Name cell: 0x1B9ED0). The mouse wheel (3 rows a notch), Page Up /
//   Down, Home and End scroll while the Scoreboard is open and its rows do not fit; otherwise they stay the game's
//   (the wheel's weapon change). The offset is kept while the Scoreboard stays open and starts at the top each time
//   it opens; it is clamped each frame as players come and go.
// CSX_LOG_SCOREBOARD=1 logs the rows when they change (at most twice a second; each marked on screen or scrolled away),
// for tests.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/guest_tables.h"
#include "common/guest_tables_layout.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/player_slots.h"
#include "game/scoreboard_rows.h"
#include "kernel/kernel.h"

using kernel::R32;
using kernel::W32;

namespace {
constexpr uint32_t kRowLast = 0x19C;       // ScorePanel: the last row's index (rows - 1)
constexpr uint32_t kSortedPlayer = 0x294;  // ScorePanel: [row] the player (or team) of each row
constexpr uint32_t kRowKind = 0x3C4;       // ScorePanel: [row] 0 a player, 1 a team header, 2 the spectators', 3 blank
constexpr uint32_t kMalloc = 0x22D86A;     // the CRT's malloc (size), cdecl: the game frees the strings it makes
constexpr uint32_t kFree = 0x22C609;       // and its free (pointer), cdecl, as the game's string code calls it
constexpr uint32_t kCurrentScreen = 0x5523D4;
constexpr uint32_t kPlayerInfo = 0x24C;    // cl.players' stride: userid +0, userinfo +4, name +0x104, ping +0x128
constexpr uint32_t kExtraInfo = 0x4C;      // g_PlayerExtraInfo's stride: frags +0, deaths +2, team +0x2A
constexpr int kLines = 25;                 // the table's lines at the layout's text scale (ExtendScoreboard)
constexpr int kWheelRows = 3;

using Clock = std::chrono::steady_clock;

bool LogOn() {
    static const bool on = std::getenv("CSX_LOG_SCOREBOARD") && *std::getenv("CSX_LOG_SCOREBOARD") == '1';
    return on;
}

int Lines() {
    static const int n = [] {
        const char* s = std::getenv("CSX_SCOREBOARD_LINES");  // for measuring the layout
        return s && std::atoi(s) >= 4 ? std::atoi(s) : kLines;
    }();
    return n;
}

struct State {
    int offset = 0;
    Clock::time_point seen;
    scoreboard::View view;
    std::vector<scoreboard::Row> rows;
    bool overflow = false;
    uint32_t two = 0;          // a guest dword holding 2 (the spectators' header kind), for the indicator
    uint32_t saved_kind = 0;   // the loop's row kind pointer while the indicator row is made
    bool indicator_now = false;
    bool ping_pass = false;    // the Deaths (or blank) cell is being made again as the Ping cell
    bool team_pass = false;    // the team header's blank cell is being made again
    uint32_t team_edi = 0;
    int spec_pass = 0;         // the spectators' header's blank cells made again so far
    uint32_t ping_player = 0;
    std::string last_log;
    Clock::time_point logged;
} g;

std::atomic<int> g_scroll{0};  // rows to scroll, from the key loop (pc_keys.cpp); INT_MIN / INT_MAX: top / bottom
std::atomic<bool> g_open_overflow{false};
uint32_t g_measured[slots::kMaxPlayers] = {};  // per cl.players slot: the userid + 1 a ping was measured for

uint32_t ClPlayer(uint32_t index) {  // 1-based player number to its player_info_t
    const auto& t = guest_tables::Production(guest_tables::k_cl_players);
    return index >= 1 && index <= t.count ? t.va + (index - 1) * kPlayerInfo : 0;
}

uint32_t Extra(uint32_t index) {
    const auto& t = guest_tables::Production(guest_tables::k_hud_extra_info);
    return index < t.count ? t.va + index * kExtraInfo : 0;
}

std::string GuestText(uint32_t va, size_t max) {
    std::string s;
    for (size_t i = 0; va && i < max; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}

bool IsBot(uint32_t index) {
    uint32_t p = ClPlayer(index);
    return p && std::atoi(scoreboard::InfoValue(GuestText(p + 4, 256), "*bot").c_str()) > 0;
}

std::string PingOf(uint32_t index) {
    uint32_t p = ClPlayer(index);
    if (!p) return "-";
    uint32_t userid = R32(p);
    bool measured = userid && g_measured[index - 1] == userid + 1;
    return scoreboard::PingText(IsBot(index), measured, int32_t(R32(p + 0x128)));
}

// the cell's std::wstring (MSVC: +4 up to 7 UTF-16 characters in place, or a pointer from 8 on; +0x14 length, +0x18
// capacity) set to text: a longer text in a block of the game's heap (its malloc), which the game frees with the
// string, as it frees one it made; a string already on the heap is freed first (its free)
void SetCellText(X86Context& ctx, uint32_t str, const std::string& text) {
    if (R32(str + 0x18) >= 8) {
        kernel::CallGuest(ctx, kFree, {R32(str + 4)});
        W32(str + 0x18, 7);
    }
    uint32_t n = uint32_t(text.size()), chars = str + 4;
    if (n >= 8) {
        chars = kernel::CallGuest(ctx, kMalloc, {(n + 1) * 2});
        if (!chars) {
            n = 0, chars = str + 4;
        } else {
            W32(str + 4, chars);
            W32(str + 0x18, n);
        }
    }
    for (uint32_t i = 0; i <= n; i++) {
        uint16_t c = i < n ? static_cast<unsigned char>(text[i]) : 0;
        std::memcpy(static_cast<uint8_t*>(kernel::HostPtr(chars)) + i * 2, &c, 2);
    }
    W32(str + 0x14, n);
}

void LogShown(uint32_t panel) {
    const scoreboard::View& v = g.view;
    int players = int(std::count(g.rows.begin(), g.rows.end(), scoreboard::Row::Player));
    std::string s = "scoreboard: " + std::to_string(players) + " players in " + std::to_string(g.rows.size()) +
                    " rows, " + std::to_string(Lines()) + " lines, offset " + std::to_string(v.offset) + ": players " +
                    std::to_string(v.first_player) + "-" + std::to_string(v.last_player) + " of " +
                    std::to_string(players) + ", " + std::to_string(v.players_below) + " below";
    // every row, " | " before one on screen and " ~ " before one off it (scrolled away), the indicator as "more"
    std::string rows;
    for (size_t r = 0; r < g.rows.size(); r++) {
        uint32_t who = R32(panel + kSortedPlayer + uint32_t(r) * 4);
        bool on = v.shown[r] && int(r) != v.indicator;
        if (int(r) == v.indicator) rows += " | more " + std::to_string(v.players_below);
        if (g.rows[r] == scoreboard::Row::Blank) continue;
        rows += on ? " | " : " ~ ";
        if (g.rows[r] == scoreboard::Row::Team && R32(panel + kRowKind + uint32_t(r) * 4) == 2) {
            rows += "spectators";
        } else if (g.rows[r] == scoreboard::Row::Team) {
            rows += "team " + std::to_string(R32(0x548A28 + who * 0x2C + 0x28)) + " (" +
                    std::to_string(int16_t(kernel::R16(0x548A28 + who * 0x2C + 0x1C))) + ")";
        } else {
            uint32_t e = Extra(who), p = ClPlayer(who);
            rows += std::to_string(who) + " \"" + (p ? GuestText(p + 0x104, 32) : std::string()) + "\" t" +
                    std::to_string(e ? int16_t(kernel::R16(e + 0x2A)) : 0) + " " +
                    std::to_string(e ? int16_t(kernel::R16(e)) : 0) + " " +
                    std::to_string(e ? int16_t(kernel::R16(e + 2)) : 0) + " ping=" + PingOf(who) +
                    (IsBot(who) ? " bot" : " human");
        }
    }
    auto now = Clock::now();
    if (s + rows == g.last_log || now - g.logged < std::chrono::milliseconds(500)) return;
    g.last_log = s + rows;
    g.logged = now;
    LOGI("%s;%s", s.c_str(), rows.c_str());
}
}  // namespace

// [[hook]] at 0x1B98A0 (FillGrid's row loop, ebp = the panel, [esp+0x44] = the row, [esp+0x10] its kind's pointer):
// true makes the row; false skips it (0x1B9979)
bool ScoreRowShown(X86Context& ctx, uint8_t*) {
    uint32_t panel = ctx.ebp;
    int row = int(R32(ctx.esp + 0x44));
    if (row == 0) {
        auto now = Clock::now();
        if (now - g.seen > std::chrono::milliseconds(500)) g.offset = 0;  // just opened: from the top
        g.seen = now;
        int n = int(R32(panel + kRowLast)) + 1;
        n = std::clamp(n, 0, 4 * int(slots::kMaxPlayers));
        g.rows.resize(size_t(n));
        for (int r = 0; r < n; r++) {
            uint32_t kind = R32(panel + kRowKind + uint32_t(r) * 4);
            // a player row whose player has no name makes nothing (the row builder's own test, 0x1BA055)
            uint32_t who = R32(panel + kSortedPlayer + uint32_t(r) * 4);
            const auto& info = guest_tables::Production(guest_tables::k_hud_info_list);
            bool named = who && who < info.count && R32(info.va + who * 0x14);
            g.rows[size_t(r)] = kind == 1 || kind == 2 ? scoreboard::Row::Team
                              : kind == 0 && named     ? scoreboard::Row::Player
                                                       : scoreboard::Row::Blank;
        }
        int step = g_scroll.exchange(0);
        if (step == INT32_MIN) g.offset = 0;
        else if (step == INT32_MAX) g.offset = n;
        else g.offset += step;
        g.view = scoreboard::Window(g.rows, Lines(), g.offset);
        g.offset = g.view.offset;
        g.overflow = n > Lines();
        g_open_overflow = g.overflow;
        if (!g.two) {
            g.two = kernel::CallGuest(ctx, kMalloc, {4});
            if (g.two) W32(g.two, 2);
        }
        if (LogOn()) LogShown(panel);
    }
    if (row < 0 || row >= int(g.view.shown.size())) return false;
    if (row == g.view.indicator && g.two) {
        g.saved_kind = R32(ctx.esp + 0x10);
        W32(ctx.esp + 0x10, g.two);
        g.indicator_now = true;
        return true;
    }
    return g.view.shown[size_t(row)];
}

// [[hook]] at 0x1B9979 (the row loop's next step, which advances [esp+0x10]): the indicator row's kind pointer put
// back
void ScoreRowNext(X86Context& ctx, uint8_t*) {
    if (!g.indicator_now) return;
    W32(ctx.esp + 0x10, g.saved_kind);
    g.indicator_now = false;
}

// [[hook]] at 0x1B9E63 (the spectators' header's title cell about to be made, [esp] = its string): the title moves
// to the Name column (its own cell is the first, narrow column, which cut it to "Sp")
void ScoreSpectatorTitle(X86Context& ctx, uint8_t*) { SetCellText(ctx, R32(ctx.esp), ""); }

// [[hook]] at 0x1B9ED0 (the spectators' header's Name cell about to be made, [esp] = its string): "Spectators", or
// for the indicator the count below
void ScoreSpectatorName(X86Context& ctx, uint8_t*) {
    SetCellText(ctx, R32(ctx.esp), g.indicator_now ? scoreboard::MoreText(g.view.players_below) : "Spectators");
}

// [[hook]] at 0x1BA4C3 (a player row's Deaths cell made; [esp+0x280] the panel, [esp+0x288] the row): true runs the
// Deaths cell's code again (0x1BA43E) as the Ping cell, once
bool ScorePingCellAgain(X86Context& ctx, uint8_t*) {
    if (g.ping_pass) return g.ping_pass = false;
    g.ping_player = R32(R32(ctx.esp + 0x280) + kSortedPlayer + R32(ctx.esp + 0x288) * 4);
    return g.ping_pass = true;
}

// [[hook]] at 0x1BA616 (an unassigned player's second blank cell made): true makes it again (0x1BA5C0) as the Ping
// cell, once
bool ScorePingBlankAgain(X86Context& ctx, uint8_t*) { return ScorePingCellAgain(ctx, nullptr); }

// [[hook]] at 0x1BA4AA and 0x1BA601 (the Deaths / blank cell about to be made, [esp] = its string): on the second
// run the player's ping
void ScorePingCellText(X86Context& ctx, uint8_t*) {
    if (g.ping_pass) SetCellText(ctx, R32(ctx.esp), PingOf(g.ping_player));
}

// [[hook]] at 0x79F91 (SV_CreateFakeClient, its userinfo set, ebx = the client_t, about to mark it to be sent and
// read): a bot's userinfo gains "*bot\1", as the PC engine gives its fake clients (CS's game DLL also asks for it at
// 0x162C97, which this build's bot path does not reach). The engine's own fake client flag (FL_FAKECLIENT 0x2000 in
// the edict's flags, set here) is where it comes from; SV_FullClientUpdate sends the userinfo (only "_" keys are left
// out), so every client, local or remote, knows its bots. A client cannot set a * key itself ("Can't set * keys").
void ScoreBotKey(X86Context& ctx, uint8_t*) {
    uint32_t info = ctx.ebx + 0x4BC8, edict = R32(ctx.ebx + 0x4B9C);
    if (!edict || !(R32(edict + 0x224) & 0x2000)) return;
    std::string text = GuestText(info, 256);
    if (!scoreboard::InfoValue(text, "*bot").empty() || text.size() + 7 >= 256) return;
    text += "\\*bot\\1";
    std::memcpy(kernel::HostPtr(info), text.c_str(), text.size() + 1);
}

// A row is drawn only when it has a cell for each column (the original's spectators' header, 4 cells for 5 columns,
// never showed): the team header (5 cells) makes its blank Deaths-side cell once more, the spectators' header (4)
// its blank third cell twice more, for the six columns.

// [[hook]] at 0x1B9CD5 (the team header's fourth cell about to be made, edi = its team): the team kept for the replay,
// which the block overwrites (edi = 7)
void ScoreTeamCellSave(X86Context& ctx, uint8_t*) {
    if (!g.team_pass) g.team_edi = ctx.edi;
}

// [[hook]] at 0x1B9D59 (the team header's fourth cell made): true makes it again (0x1B9CD5), once
bool ScoreTeamCellAgain(X86Context& ctx, uint8_t*) {
    if (g.team_pass) return g.team_pass = false;
    ctx.edi = g.team_edi;
    return g.team_pass = true;
}

// [[hook]] at 0x1B9F5A (the spectators' header's third cell made): true makes it again (0x1B9EF4), twice
bool ScoreSpectatorCellAgain(X86Context&, uint8_t*) {
    if (g.spec_pass < 2) return ++g.spec_pass, true;
    g.spec_pass = 0;
    return false;
}

// [[hook]] at 0x31A30 (svc_pings stored a ping, the slot in esi): the slot's player has a measured ping
void ScorePingMeasured(X86Context& ctx, uint8_t*) {
    uint32_t p = ClPlayer(ctx.esi + 1);
    if (p && ctx.esi < slots::kMaxPlayers) g_measured[ctx.esi] = R32(p) + 1;
}

// the key loop (pc_keys.cpp): a key the Scoreboard takes while it is open and its rows do not fit (wheel 239 / 240,
// Page Down 149, Page Up 150, Home 151, End 152); true when taken (its release too)
bool ScoreboardKey(int key, bool down) {
    uint32_t screen = R32(kCurrentScreen);
    if (!screen || GuestText(R32(screen), 16) != "Scoreboard" || !g_open_overflow) return false;
    int page = std::max(1, Lines() - 3);
    int step = 0;
    switch (key) {
        case 240: step = -kWheelRows; break;
        case 239: step = kWheelRows; break;
        case 150: step = -page; break;
        case 149: step = page; break;
        case 151: step = INT32_MIN; break;
        case 152: step = INT32_MAX; break;
        default: return false;
    }
    if (!down) return true;
    if (step == INT32_MIN || step == INT32_MAX) {
        g_scroll = step;
    } else {
        int cur = g_scroll.load();
        if (cur != INT32_MIN && cur != INT32_MAX) g_scroll = cur + step;
    }
    return true;
}

// the Scoreboard screen's table with the Ping column (pc_menu.cpp, InGameUI*.txt as it loads); the text unchanged
// when the screen is not as expected
std::string ExtendScoreboard(const std::string& text) {
    size_t screen = text.find("\nScoreboard\r\n{");
    size_t table = screen == std::string::npos ? screen : text.find("\tPassiveTable\r\n", screen);
    size_t end = table == std::string::npos ? table : text.find("\r\n\t}\r\n}\r\n", table);
    size_t next = screen == std::string::npos ? screen : text.find("\nSpectator\r\n", screen);
    if (end == std::string::npos || (next != std::string::npos && end > next)) {
        LOGW("scoreboard: the Scoreboard screen's table not found, left as it is");
        return text;
    }
    end += std::strlen("\r\n\t}\r\n");
    const char* cols[][2] = {{"5", "center"}, {"43", "truncated"}, {"13", "center"}, {"13", "center"},
                             {"16", "center"}, {"10", "center"}};
    std::string layout;
    for (auto& c : cols)
        layout += std::string("\t\t\tColumnWidthPercentage \"") + c[0] + "\"\r\n\t\t\tColumnJustify \"" + c[1] + "\"\r\n";
    std::string t = "\tPassiveTable\r\n\t{\r\n\t\tLayout\r\n\t\t{\r\n" + layout +
                    "\t\t\tTableLocation \"40 28\"\r\n\t\t\tTableTextScale \"0.62\"\r\n\t\t\tTableSize \"560 440\"\r\n"
                    "\t\t\tTableVerticalWrap\r\n\t\t\tDisableFG \"0.2 0.7 0.3 1.0\"\r\n\t\t}\r\n\r\n"
                    "\t\tHeader\r\n\t\t{\r\n\t\t\tCellFixed \" \"\r\n\t\t\tCellFixed \"Name\"\r\n"
                    "\t\t\tCellFixed \"Score\"\r\n\t\t\tCellFixed \"Deaths\"\r\n\t\t\tCellFixed \"Ping\"\r\n"
                    "\t\t\tCellFixed \" \"\r\n\t\t}\r\n\r\n\t\tRow\r\n\t\t{\r\n";
    for (int i = 0; i < 6; i++) t += "\t\t\tCellFixed \" \"\r\n";
    t += "\t\t}\r\n\t}\r\n";
    LOGI("scoreboard: the Scoreboard screen has a Ping column and %d lines", Lines());
    return text.substr(0, table) + t + text.substr(end);
}
