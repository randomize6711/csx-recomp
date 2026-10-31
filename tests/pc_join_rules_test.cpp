// tests/pc_join_rules_test.py builds and runs this: src/game/pc_join_rules.h on its own
#include <cstdio>

#include "game/pc_join_rules.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    using namespace join;
    // which press joins
    Check(Joins(Where::Menus, true, false), "START in the menus joins");
    Check(!Joins(Where::Menus, false, true), "A in the menus does not (it is the menus')");
    Check(Joins(Where::PartyScreen, false, true) && Joins(Where::PartyScreen, true, false), "the party screen: A or START");
    Check(!Joins(Where::Match, true, true), "never in a match");
    // four at most
    Check(Room(1) && Room(3) && !Room(4), "up to four local players");
    // the popup: profiles, one never two players'
    const std::vector<Profile> saved = {{"aaaa", "Alice"}, {"bbbb", "Bob"}, {"cccc", "Cat"}};
    std::vector<Row> rows = PopupRows(saved, {"aaaa", "", "", ""}, 2);
    Check(rows.size() == 5 && rows[3].kind == Row::New && rows[4].kind == Row::Cancel, "profiles, New Profile, Cancel");
    Check(rows[0].taken_by == 1 && rows[1].taken_by == 0, "player 1's profile marked as theirs");
    Check(Choose(rows, 0) == Pick::TakenByOther, "another player's profile cannot be chosen");
    Check(Choose(rows, 1) == Pick::Chosen, "a free profile is chosen");
    Check(Choose(rows, 3) == Pick::NewProfile, "New Profile");
    Check(Choose(rows, 4) == Pick::Cancelled, "Cancel: the controller does not join");
    Check(Choose(rows, 9) == Pick::Closed, "out of the rows: nothing chosen");
    // a member's own profile is not "taken" from itself (changing profile)
    Check(PopupRows(saved, {"aaaa", "bbbb", "", ""}, 2)[1].taken_by == 0, "its own profile is free to it");
    // at most six profiles in the popup
    std::vector<Profile> many;
    for (int i = 0; i < 10; i++) many.push_back({"id" + std::to_string(i), "P" + std::to_string(i)});
    Check(PopupRows(many, {"", "", "", ""}, 2).size() == size_t(kPopupProfiles + 2), "six profiles shown at most");
    Check(FullRows().size() == 1 && Choose(FullRows(), 0) == Pick::Closed, "all slots taken: a message and OK");
    Check(Selection(7, 5) == 4 && Selection(-1, 5) == 0 && Selection(2, 5) == 2, "the selection held to the rows");
    // the match's local players, from the members
    Check(LocalPlayers({true, false, false, false}) == 1, "one player: a full-screen match");
    Check(LocalPlayers({true, true, false, false}) == 2, "two");
    Check(LocalPlayers({true, true, true, false}) == 3, "three");
    Check(LocalPlayers({true, true, true, true}) == 4, "four");
    // the slots: humans and bots within 65
    Check(MaxBots(1) == 64 && MaxBots(4) == 61, "64 bots alone, 61 with four players");
    Check(ClampBots(64, 4) == 61 && ClampBots(7, 4) == 7 && ClampBots(-1, 2) == 0, "the bots held within the slots");
    // the status lines
    Check(SlotLine(1, true, "Alice") == "P1  Alice" && SlotLine(3, false, "") == "P3  Press START to join" &&
              SlotLine(2, true, "") == "P2  Choosing a profile...", "Instant Action's player status");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
