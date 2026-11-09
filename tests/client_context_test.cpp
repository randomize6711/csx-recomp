// tests/client_context_test.py builds and runs this: src/game/client_context_core.h on a synthetic memory
#include <cstdio>
#include <vector>

#include "game/client_context_core.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

static std::vector<uint8_t> g_mem(0x10000);
static uint8_t* Map(uint32_t va) { return g_mem.data() + va; }

static void Fill(uint32_t start, uint32_t size, uint8_t v) {
    for (uint32_t i = 0; i < size; i++) g_mem[start + i] = uint8_t(v + i);
}
static bool Holds(uint32_t start, uint32_t size, uint8_t v) {
    for (uint32_t i = 0; i < size; i++)
        if (g_mem[start + i] != uint8_t(v + i)) return false;
    return true;
}

int main() {
    using namespace ctx;
    // region lists
    Check(CheckRegions({{0x100, 0x10, "a"}, {0x110, 0x10, "b"}}).empty(), "adjacent regions are fine");
    Check(!CheckRegions({{0x100, 0x20, "a"}, {0x110, 0x10, "b"}}).empty(), "overlap refused");
    Check(!CheckRegions({{0x200, 0x10, "a"}, {0x100, 0x10, "b"}}).empty(), "unsorted refused");
    Check(!CheckRegions({{0x100, 0, "a"}}).empty(), "empty refused");
    Check(!CheckRegions({{0xFFFFFFF0u, 0x20, "a"}}).empty(), "past 4 GB refused");

    std::vector<Region> regions = {{0x1000, 0x40, "one"}, {0x2000, 0x100, "two"}, {0x3001, 0x3, "odd"}};
    Switcher<uint8_t* (*)(uint32_t)> s(regions, &Map);
    Check(s.Count() == 1 && s.Active() == 0 && s.Bytes() == 0x143, "one context, its size");
    // one context: switching to itself copies nothing
    Fill(0x1000, 0x40, 1);
    Check(s.Switch(0) && s.Switches() == 0 && Holds(0x1000, 0x40, 1), "one context: no copy");
    Check(!s.Switch(1) && !s.Switch(-1), "no such context");

    // player 0's state, then a second player made from it, then given its own
    Fill(0x1000, 0x40, 10);
    Fill(0x2000, 0x100, 20);
    Fill(0x3001, 0x3, 30);
    Fill(0x0F00, 0x100, 99);  // outside every region: shared, never copied
    Check(s.Add() == 1 && s.Count() == 2, "a second context");
    Check(s.Switch(1) && s.Active() == 1, "switch to player 2");
    Check(Holds(0x1000, 0x40, 10) && Holds(0x2000, 0x100, 20), "player 2 starts as a copy");
    Fill(0x1000, 0x40, 50);
    Fill(0x2000, 0x100, 60);
    Fill(0x3001, 0x3, 70);
    Fill(0x0F00, 0x100, 77);  // shared memory changed by player 2's turn
    Check(s.Switch(0), "back to player 1");
    Check(Holds(0x1000, 0x40, 10) && Holds(0x2000, 0x100, 20) && Holds(0x3001, 0x3, 30), "player 1's state back");
    Check(Holds(0x0F00, 0x100, 77), "shared memory is not switched");
    Check(g_mem[0x3000] == 0 && g_mem[0x3004] == 0, "bytes beside a region untouched");
    Check(s.Switch(1) && Holds(0x1000, 0x40, 50) && Holds(0x2000, 0x100, 60) && Holds(0x3001, 0x3, 70),
          "player 2's own state back");

    // the frame's turns: 0, 1, 2 then done
    Check(NextTurn(0, 1) == -1, "one player: no second turn");
    Check(NextTurn(0, 3) == 1 && NextTurn(1, 3) == 2 && NextTurn(2, 3) == -1, "three players' turns");

    // removing: never the active one
    Check(!s.RemoveLast(), "the active context is not removed");
    Check(s.Switch(0) && s.RemoveLast() && s.Count() == 1, "the last one removed");
    Check(s.Add() == 1 && s.Switch(1) && Holds(0x1000, 0x40, 10), "a new one copies the active player's state");

    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
