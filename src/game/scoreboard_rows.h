// The scoreboard's rows on screen (src/game/scoreboard.cpp; FINDINGS.md "Scoreboard"): which of the ScorePanel's
// sorted rows (team headers, players, blank spacers) the table shows, from a scroll offset, and the ping cell's text.
// Pure functions, tested on their own by tests/scoreboard_rows_test.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scoreboard {

// a ScorePanel row: a team's header (or the spectators' header: both start a section), a player, or a blank row
// (the panel's spacer, which FillGrid makes nothing of: it takes no line)
enum class Row : uint8_t { Team, Player, Blank };

struct View {
    std::vector<bool> shown;  // per row: drawn
    int offset = 0;           // the first row of the window, after clamping
    int indicator = -1;       // the row turned into the "more below" line (the first row after the window), -1 none
    int players_below = 0;    // players after the window (the indicator's count)
    int first_player = 0;     // 1-based numbers of the first and last players shown among all players (0: none)
    int last_player = 0;
};

// the team header a row belongs under: the last Team row before it, -1 when none
inline int HeaderOf(const std::vector<Row>& rows, int row) {
    for (int i = row - 1; i >= 0; i--)
        if (rows[size_t(i)] == Row::Team) return i;
    return -1;
}

// the rows a window starting at `offset` shows in `capacity` lines: the section header of the first row first when the
// window starts below it (it stays in sight while its players scroll), then the rows in order (a blank row takes no
// line); when rows remain after it, its last line becomes the indicator: that row is drawn as a header row with the
// count of players below instead
inline View Window(const std::vector<Row>& rows, int capacity, int offset) {
    View v;
    const int n = int(rows.size());
    v.shown.assign(rows.size(), false);
    if (capacity < 2) capacity = 2;
    auto build = [&](int off) {
        View w;
        w.shown.assign(rows.size(), false);
        w.offset = off;
        int used = 0, last = -1;  // lines used, the last row shown that takes a line
        if (off > 0 && off < n && rows[size_t(off)] != Row::Team) {
            int h = HeaderOf(rows, off);
            if (h >= 0) w.shown[size_t(h)] = true, used++, last = h;
        }
        int i = off;
        for (; i < n; i++) {
            if (rows[size_t(i)] == Row::Blank) {
                w.shown[size_t(i)] = true;
                continue;
            }
            if (used == capacity) break;
            w.shown[size_t(i)] = true, used++, last = i;
        }
        bool more = false;
        for (int j = i; j < n && !more; j++) more = rows[size_t(j)] != Row::Blank;
        if (more && last >= 0) {  // more below: the window's last line is the indicator
            w.shown[size_t(last)] = false;
            w.indicator = last;
            for (int j = last; j < n; j++) w.players_below += rows[size_t(j)] == Row::Player;
        }
        return w;
    };
    int lines = 0;
    for (Row r : rows) lines += r != Row::Blank;
    if (lines <= capacity) {
        v = build(0);
    } else {
        // the largest offset whose window reaches the last row: past it the window only shrinks
        int max_off = n - 1;
        while (max_off > 0 && build(max_off - 1).indicator < 0) max_off--;
        if (offset > max_off) offset = max_off;
        if (offset < 0) offset = 0;
        v = build(offset);
    }
    int player = 0;
    for (int i = 0; i < n; i++) {
        if (rows[size_t(i)] != Row::Player) continue;
        player++;
        if (!v.shown[size_t(i)]) continue;
        if (!v.first_player) v.first_player = player;
        v.last_player = player;
    }
    return v;
}

// the largest useful offset (the window then ends at the last row)
inline int MaxOffset(const std::vector<Row>& rows, int capacity) {
    return Window(rows, capacity, int(rows.size())).offset;
}

// the Ping column: BOT for a bot (the server's "*bot" key in its userinfo), "-" until the server has sent a measured
// ping for the player, the measured milliseconds otherwise
inline std::string PingText(bool bot, bool measured, int ping) {
    if (bot) return "BOT";
    if (!measured || ping < 0) return "-";
    return std::to_string(ping > 9999 ? 9999 : ping);
}

// the indicator's text: how many players are below the window and how to reach them
inline std::string MoreText(int players_below) {
    return "v  " + std::to_string(players_below) + " more  (wheel / Page Down)";
}

// a key of an info string ("\key\value\key\value"), empty when absent (the engine's Info_ValueForKey)
inline std::string InfoValue(const std::string& info, const std::string& key) {
    size_t i = 0;
    while (i < info.size()) {
        if (info[i] == '\\') i++;
        size_t k = info.find('\\', i);
        if (k == std::string::npos) return {};
        size_t e = info.find('\\', k + 1);
        if (e == std::string::npos) e = info.size();
        if (info.compare(i, k - i, key) == 0) return info.substr(k + 1, e - k - 1);
        i = e;
    }
    return {};
}

}  // namespace scoreboard
