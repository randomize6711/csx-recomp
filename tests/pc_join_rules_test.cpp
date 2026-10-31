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
    // which press joins: START on Instant Action, never A there (the menus'), nowhere else
    Check(Joins(Where::InstantAction, true, false), "START on Instant Action joins");
    Check(!Joins(Where::InstantAction, false, true), "A on Instant Action is the menus'");
    Check(Joins(Where::PartyScreen, false, true) && Joins(Where::PartyScreen, true, false), "the party screen: A or START");
    Check(!Joins(Where::Elsewhere, true, true), "other menus and matches: never");
    // four at most
    Check(Room(1) && Room(3) && !Room(4), "up to four local players");
    // the slot's profile: its own last one when free, else a guest; no saved profile touched
    Check(SlotProfile("bbbb", true, {"aaaa", "", ""}) == "bbbb", "the slot's remembered profile, free");
    Check(SlotProfile("aaaa", true, {"aaaa", "", ""}).empty(), "another player has it: a guest");
    Check(SlotProfile("cccc", false, {"aaaa"}).empty(), "a profile that no longer exists: a guest");
    Check(SlotProfile("", true, {"aaaa"}).empty(), "none remembered: a guest");
    Check(GuestName(3) == "Guest 3", "a guest's name");
    // the match's local players, from the members (1: full screen; 2-4: the split layouts)
    Check(LocalPlayers({true, false, false, false}) == 1, "one player");
    Check(LocalPlayers({true, true, false, false}) == 2, "two");
    Check(LocalPlayers({true, true, true, false}) == 3, "three");
    Check(LocalPlayers({true, true, true, true}) == 4, "four");
    // the slots: humans and bots within 65
    Check(MaxBots(1) == 64 && MaxBots(4) == 61, "64 bots alone, 61 with four players");
    Check(ClampBots(64, 4) == 61 && ClampBots(7, 4) == 7 && ClampBots(-1, 2) == 0, "the bots held within the slots");
    // the panel's lines
    Check(SlotLine(1, true, "Alice") == "P1  -  Ready (Alice)", "player 1 and its profile");
    Check(SlotLine(2, true, "") == "P2  -  Ready (Guest 2)", "a guest");
    Check(SlotLine(3, false, "") == "P3  -  Press Start to Join", "a free slot");
    Check(SlotLine(2, true, "Bob", true) == "P2  -  Controller disconnected", "a disconnected controller");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
