// src/game/scoreboard_rows.h on its own (tests/scoreboard_rows_test.py builds and runs it): the scoreboard's window of
// rows for empty, small and full player lists, one team, scrolling to both ends, clamping when players leave, the
// team header kept in sight, the "more below" line, every player reachable; the Ping text; the userinfo lookup.
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "game/scoreboard_rows.h"

using scoreboard::Row;

namespace {
int g_failed = 0, g_checks = 0;

void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// the ScorePanel's rows for teams of the given sizes: a header and its players per team, a blank row between teams
std::vector<Row> Rows(const std::vector<int>& teams) {
    std::vector<Row> r;
    for (size_t t = 0; t < teams.size(); t++) {
        if (t) r.push_back(Row::Blank);
        r.push_back(Row::Team);
        for (int i = 0; i < teams[t]; i++) r.push_back(Row::Player);
    }
    return r;
}

int Lines(const std::vector<Row>& rows, const scoreboard::View& v) {
    int n = v.indicator >= 0;
    for (size_t i = 0; i < rows.size(); i++) n += v.shown[i] && rows[i] != Row::Blank;
    return n;
}

// scrolled from the top to the bottom a wheel notch at a time (3 rows, or what the window holds besides its header
// and indicator when that is less, as scoreboard.cpp's page step): every player is shown at some offset, no
// window holds more lines than the table, the window ends at the last row at the largest offset
void Reach(const std::vector<Row>& rows, int lines, const std::string& name) {
    int players = 0;
    for (Row r : rows) players += r == Row::Player;
    std::set<int> seen;
    int max_off = scoreboard::MaxOffset(rows, lines);
    int step = std::min(3, std::max(1, lines - 3));
    for (int off = 0; off <= max_off + 6; off += step) {
        scoreboard::View v = scoreboard::Window(rows, lines, off);
        Check(Lines(rows, v) <= lines, name + ": no more lines than the table at offset " + std::to_string(off));
        Check(v.offset <= max_off, name + ": offset clamped");
        for (size_t i = 0; i < rows.size(); i++)
            if (v.shown[i] && rows[i] == Row::Player) seen.insert(int(i));
        // the first shown player's team header is shown (kept in sight)
        for (size_t i = 0; i < rows.size(); i++) {
            if (!v.shown[i] || rows[i] != Row::Player) continue;
            int h = scoreboard::HeaderOf(rows, int(i));
            Check(h < 0 || v.shown[size_t(h)], name + ": the first player's team header in sight at " + std::to_string(off));
            break;
        }
        if (v.indicator >= 0) {
            Check(v.indicator > v.offset && !v.shown[size_t(v.indicator)], name + ": the indicator after the window");
            int below = 0;
            for (size_t i = size_t(v.indicator); i < rows.size(); i++) below += rows[i] == Row::Player;
            Check(v.players_below == below, name + ": the indicator's count");
        }
    }
    Check(int(seen.size()) == players, name + ": every player reachable (" + std::to_string(seen.size()) + " of " +
                                           std::to_string(players) + ")");
    scoreboard::View end = scoreboard::Window(rows, lines, 1 << 20);
    Check(rows.empty() || end.shown.back(), name + ": the bottom shows the last row");
    Check(end.indicator < 0, name + ": nothing below at the bottom");
    if (players) Check(end.last_player == players, name + ": the bottom shows the last player");
}
}  // namespace

int main() {
    const int lines = 26;
    // empty, one player, the original's sizes, 32, 64 and the player
    for (auto teams : std::vector<std::vector<int>>{{}, {0, 0}, {1, 0}, {8, 8}, {9, 8}, {16, 16}, {16, 17}, {32, 33},
                                                    {64, 1}, {1, 64}, {65, 0}, {0, 65}, {65}, {40, 25}}) {
        std::string name = "teams";
        for (int t : teams) name += " " + std::to_string(t);
        std::vector<Row> rows = Rows(teams);
        Reach(rows, lines, name);
        scoreboard::View top = scoreboard::Window(rows, lines, 0);
        int need = 0;
        for (Row r : rows) need += r != Row::Blank;
        bool fits = need <= lines;
        Check(fits == (top.indicator < 0), name + ": the indicator only when the rows do not fit");
        if (fits) {
            for (size_t i = 0; i < rows.size(); i++) Check(top.shown[i], name + ": every row shown when they fit");
            Check(scoreboard::MaxOffset(rows, lines) == 0, name + ": no scrolling when they fit");
        }
    }
    // the edges: an offset below 0 is the top, past the end the bottom
    std::vector<Row> big = Rows({32, 33});
    Check(scoreboard::Window(big, lines, -5).offset == 0, "a negative offset is the top");
    Check(scoreboard::Window(big, lines, 1000).offset == scoreboard::MaxOffset(big, lines), "a large offset is the bottom");
    // players leave while scrolled to the bottom: the offset is clamped, the window still ends at the last row
    int off = scoreboard::MaxOffset(big, lines);
    std::vector<Row> fewer = Rows({10, 12});
    scoreboard::View after = scoreboard::Window(fewer, lines, off);
    Check(after.offset == 0 && after.indicator < 0, "players left: clamped to what fits");
    std::vector<Row> some_left = Rows({30, 20});
    scoreboard::View clamped = scoreboard::Window(some_left, lines, off);
    Check(clamped.offset == scoreboard::MaxOffset(some_left, lines), "players left: clamped to the new bottom");
    Check(clamped.last_player == 50, "players left: the last player still shown");
    // a team change: the same count in other teams, the window stays within bounds
    scoreboard::View moved = scoreboard::Window(Rows({33, 32}), lines, off);
    Check(moved.last_player == 65 && Lines(Rows({33, 32}), moved) <= lines, "a team change keeps the bottom");
    // the first page of 65 players: the header and 24 players, then the indicator with the rest
    scoreboard::View first = scoreboard::Window(Rows({32, 33}), lines, 0);
    Check(first.first_player == 1 && first.last_player == 24, "the first page (26 lines): players 1-24 (" +
                                                                   std::to_string(first.last_player) + ")");
    Check(first.players_below == 41, "the first page: 41 below (" + std::to_string(first.players_below) + ")");
    // a spectators section (its header a section header like a team's) after both teams
    std::vector<Row> spec = Rows({30, 30});
    spec.push_back(Row::Blank);
    spec.push_back(Row::Team);
    for (int i = 0; i < 5; i++) spec.push_back(Row::Player);
    Reach(spec, lines, "two teams and 5 spectators");
    // blank rows take no line: 25 players and a header with blanks between fit 26 lines
    std::vector<Row> blanks = {Row::Team};
    for (int i = 0; i < 25; i++) blanks.push_back(Row::Player), blanks.push_back(Row::Blank);
    scoreboard::View b = scoreboard::Window(blanks, lines, 0);
    Check(b.indicator < 0 && b.last_player == 25, "blank rows take no line");
    // a small table still makes progress
    Reach(Rows({32, 33}), 4, "a four-line table");

    // the Ping column
    Check(scoreboard::PingText(true, true, 5) == "BOT", "a bot is BOT, even with a ping");
    Check(scoreboard::PingText(true, false, 0) == "BOT", "a bot is BOT before any ping");
    Check(scoreboard::PingText(false, false, 0) == "-", "a human not yet measured");
    Check(scoreboard::PingText(false, true, 0) == "0", "a measured 0 ms");
    Check(scoreboard::PingText(false, true, 37) == "37", "a measured 37 ms");
    Check(scoreboard::PingText(false, true, 4095) == "4095", "the largest ping svc_pings carries");
    Check(scoreboard::PingText(false, true, -1) == "-", "a negative ping is not a measurement");
    Check(scoreboard::MoreText(41).find("41 more") != std::string::npos, "the indicator names the count");

    // the userinfo's *bot key
    Check(scoreboard::InfoValue("\\name\\Tim\\*bot\\1\\model\\gign", "*bot") == "1", "*bot in the middle");
    Check(scoreboard::InfoValue("\\*bot\\1", "*bot") == "1", "*bot alone");
    Check(scoreboard::InfoValue("\\name\\*bot\\model\\gign", "*bot") == "", "a name of *bot is not the key");
    Check(scoreboard::InfoValue("\\name\\Tim\\model\\gign", "*bot") == "", "a human has none");
    Check(scoreboard::InfoValue("", "*bot") == "", "an empty userinfo");
    Check(scoreboard::InfoValue("\\name\\Tim\\bot\\1", "*bot") == "", "bot without the star is not *bot");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
