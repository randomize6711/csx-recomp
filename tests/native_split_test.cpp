// tests/native_split_test.py builds and runs this: native split-screen's parts without the game (M2):
// src/game/local_loopback.h, heap_owner.h, native_split_core.h and client_context_core.h's removal
#include <cstdio>
#include <string>
#include <vector>

#include "game/client_context_core.h"
#include "game/heap_owner.h"
#include "game/local_loopback.h"
#include "game/native_split_core.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

static std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
static std::string Text(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

static void Loopback() {
    using namespace loopnet;
    Channels ch;
    // two logical clients, one server
    auto a = Bytes("p1 move"), b = Bytes("p2 move");
    Check(ch.ToServer(0, a.data(), a.size()) && ch.ToServer(1, b.data(), b.size()), "two clients send");
    int from = -1;
    std::vector<uint8_t> got;
    Check(ch.ServerGet(from, got) && from == 0 && Text(got) == "p1 move", "the server reads player 1's, tagged 0");
    Check(ch.ServerGet(from, got) && from == 1 && Text(got) == "p2 move", "then player 2's, tagged 1");
    Check(!ch.ServerGet(from, got), "nothing more");
    // the server's replies go to the player its address names, never to the other
    auto r1 = Bytes("to p1"), r2 = Bytes("to p2");
    ch.ToClient(1, r2.data(), r2.size());
    ch.ToClient(0, r1.data(), r1.size());
    Check(ch.ClientPending(0) == 1 && ch.ClientPending(1) == 1, "one each");
    Check(ch.ClientGet(1, got) && Text(got) == "to p2", "player 2 gets its own");
    Check(!ch.ClientGet(1, got), "and nothing of player 1's");
    Check(ch.ClientGet(0, got) && Text(got) == "to p1", "player 1 gets its own");
    // no such player
    Check(!ch.ToServer(4, a.data(), a.size()) && !ch.ToClient(-1, a.data(), a.size()), "no player 5 or -1");
    // the title's ring: the newest four kept a side (a player's own oldest dropped, not another player's)
    for (int i = 0; i < 6; i++) {
        auto m = Bytes("p2 #" + std::to_string(i));
        ch.ToServer(1, m.data(), m.size());
    }
    auto keep = Bytes("p1 keep");
    ch.ToServer(0, keep.data(), keep.size());
    size_t mine = 0;
    bool p1_kept = false;
    std::string first_p2;
    while (ch.ServerGet(from, got)) {
        if (from == 1 && first_p2.empty()) first_p2 = Text(got);
        mine += from == 1;
        p1_kept |= from == 0 && Text(got) == "p1 keep";
    }
    Check(mine == 4 && first_p2 == "p2 #2" && p1_kept, "four newest of player 2, player 1's untouched");
    for (int i = 0; i < 6; i++) {
        auto m = Bytes("to p1 #" + std::to_string(i));
        ch.ToClient(0, m.data(), m.size());
    }
    Check(ch.ClientPending(0) == 4 && ch.ClientGet(0, got) && Text(got) == "to p1 #2", "a client's four newest");
    // a player leaving: its packets both ways forgotten, the other's kept
    ch.Clear();
    ch.ToServer(0, a.data(), a.size());
    ch.ToServer(1, b.data(), b.size());
    ch.ToClient(1, r2.data(), r2.size());
    ch.ToClient(0, r1.data(), r1.size());
    ch.Forget(1);
    Check(ch.ServerPending() == 1 && ch.ClientPending(1) == 0 && ch.ClientPending(0) == 1, "player 2's packets gone");
    Check(ch.ServerGet(from, got) && from == 0, "player 1's still there");
    ch.Clear();
    Check(ch.ServerPending() == 0 && ch.ClientPending(0) == 0, "the server shut down: all empty");
    // identities on the loopback: player 1 keeps port 0 (the title's own address), each player its own port
    Check(PortOf(0) == 0 && PortOf(1) == 1 && PlayerOf(1) == 1 && PlayerOf(0) == 0 && PlayerOf(7) == -1, "ports");
    // the address hooks: single player (every loopback port 0) exactly as the title; two players kept apart
    Check(AddressesMatch(1, 0, 0), "single player: the loopback client matches as before");
    Check(!AddressesMatch(1, 0, 1) && AddressesMatch(1, 1, 1), "two loopback players: each matches only itself");
    Check(AddressesMatch(3, 0, 1) && AddressesMatch(2, 5, 6), "other address types: left to the title's own checks");
}

// a server that answers each client with its sender's index, over many frames: no packet crosses players
static void Exchange() {
    loopnet::Channels ch;
    int crossed = 0, answered[2] = {0, 0};
    for (int frame = 0; frame < 200; frame++) {
        for (int k = 0; k < 2; k++) {  // each player's turn: its command
            auto m = Bytes("cmd from " + std::to_string(k));
            ch.ToServer(k, m.data(), m.size());
        }
        int from;
        std::vector<uint8_t> got;
        while (ch.ServerGet(from, got)) {  // the server frame: a reply to the sender
            if (Text(got) != "cmd from " + std::to_string(from)) crossed++;
            auto r = Bytes("snapshot for " + std::to_string(from));
            ch.ToClient(from, r.data(), r.size());
        }
        for (int k = 0; k < 2; k++)  // each player's client half
            while (ch.ClientGet(k, got)) {
                if (Text(got) != "snapshot for " + std::to_string(k)) crossed++;
                answered[k]++;
            }
    }
    Check(crossed == 0 && answered[0] == 200 && answered[1] == 200, "200 frames, two players: nothing crossed");
}

static void Heap() {
    using namespace heapown;
    Blocks b;
    b.Allocated(0x1000000, 0x100, kProcess);
    b.Allocated(0x1000200, 0x40, kProcess);
    Check(b.Holding(0x1000000) == 0x1000000 && b.Holding(0x10000FF) == 0x1000000 && !b.Holding(0x1000100),
          "a block holds its bytes only");
    Check(b.Holding(0x1000230) == 0x1000200 && !b.Holding(0xFFFFFF), "the second block, nothing below");
    // the scrub: a new player's copy loses player 1's heap pointers (inner ones too), keeps everything else
    std::vector<uint8_t> copy(32, 0);
    uint32_t vals[8] = {0x1000000, 7, 0x1000210, 0x2000000, 0x1000100, 0, 0x10000FC, 12};
    std::memcpy(copy.data(), vals, sizeof vals);
    std::vector<Cleared> cleared = Scrub(copy, b);
    Check(cleared.size() == 3, "three heap pointers found (two block starts, one inside)");
    uint32_t after[8];
    std::memcpy(after, copy.data(), sizeof after);
    Check(after[0] == 0 && after[2] == 0 && after[6] == 0 && after[1] == 7 && after[3] == 0x2000000 && after[4] == 0x1000100,
          "cleared exactly those");
    // the new player's initialisation replaces one with its own; the others it leaves are shared objects: back
    uint32_t own = 0x3000000;
    std::memcpy(copy.data(), &own, 4);
    auto any = [](uint32_t) { return true; };
    Check(RestoreUnreplaced(copy, cleared, b, any) == 2, "two restored");
    std::memcpy(after, copy.data(), sizeof after);
    Check(after[0] == own && after[2] == 0x1000210 && after[6] == 0x10000FC, "its own kept, the shared ones back");
    // the M2 run's case: the snapshot's pointers go stale (player 1's code frees and reuses their blocks before player 2
    // is added): a freed block's pointer stays empty, and so does one whose address another allocation took since
    {
        Blocks h;
        h.Allocated(0x5000000, 0x40, kProcess);  // a HUD list node
        h.Allocated(0x5000100, 0x40, kProcess);  // the object at 0x552CA8
        h.Allocated(0x5000200, 0x40, kProcess);  // a registered cvar
        std::vector<uint8_t> snap(12);
        uint32_t v[3] = {0x5000000, 0x5000100, 0x5000200};
        std::memcpy(snap.data(), v, 12);
        std::vector<Cleared> at_snapshot = Scrub(snap, h);  // when the snapshot is taken
        Check(at_snapshot.size() == 3, "all three found while current");
        h.Freed(0x5000000);                         // player 1 frees its node
        h.Freed(0x5000100);                         // and its object...
        h.Allocated(0x5000100, 0x80, kProcess);     // ...and the address is reused by an unrelated block
        std::vector<uint8_t> player2 = snap;        // player 2's copy: all three cleared
        auto any = [](uint32_t) { return true; };
        Check(RestoreUnreplaced(player2, at_snapshot, h, any) == 1, "only the cvar still alive is given back");
        uint32_t w[3];
        std::memcpy(w, player2.data(), 12);
        Check(w[0] == 0 && w[1] == 0 && w[2] == 0x5000200, "a freed block's pointer and a reused address stay empty");
    }
    // only shared kinds are given back: a live block of player 1 that is not a registry entry or a cvar stays empty
    {
        Blocks h;
        h.Allocated(0x6000000, 0x14, kProcess, 0x1BF2A);  // a registered cvar
        h.Allocated(0x6000100, 0x440, kProcess, 0x1BAE6C);  // player 1's mutable object
        std::vector<uint8_t> snap(8);
        uint32_t v[2] = {0x6000000, 0x6000100};
        std::memcpy(snap.data(), v, 8);
        std::vector<Cleared> c = Scrub(snap, h);
        Check(c.size() == 2 && c[0].site == 0x1BF2A && c[1].size == 0x440, "the cleared pointers keep size and site");
        auto shared = [](uint32_t site) { return site == 0x1BF2A; };
        std::vector<uint8_t> p2 = snap;
        Check(RestoreUnreplaced(p2, c, h, shared) == 1, "only the cvar comes back");
        uint32_t w[2];
        std::memcpy(w, p2.data(), 8);
        Check(w[0] == 0x6000000 && w[1] == 0, "player 1's mutable object is never shared");
        // the same address, the same serial but another size or site is not the same allocation
        Blocks g;
        g.Allocated(0x7000000, 0x20, kProcess, 0x1BF2A);
        std::vector<uint8_t> s2(4);
        uint32_t x = 0x7000000;
        std::memcpy(s2.data(), &x, 4);
        std::vector<Cleared> c2 = Scrub(s2, g);
        c2[0].size = 0x30;
        Check(!StillAlive(c2[0], g), "a different size: not the same allocation");
    }
    // the safety net: player 2..4 never frees a block that is not its own
    Check(SkipFree(kProcess, 1) && SkipFree(0, 1) && SkipFree(2, 1), "player 2: not the process's, not another's");
    Check(!SkipFree(1, 1) && !SkipFree(kProcess, 0) && !SkipFree(1, kProcess) && !SkipFree(Blocks::kNone, 1),
          "its own, player 1's own frees, frees outside the turns, unknown blocks: done");
    // frees: whose block, by whom
    Check(!FreeIsForeign(1, 1) && FreeIsForeign(0, 1) && FreeIsForeign(1, 0), "a player frees only its own");
    Check(FreeIsForeign(kProcess, 1) && !FreeIsForeign(kProcess, 0) && !FreeIsForeign(kProcess, kProcess),
          "the process's initialisation: freed by player 1 or the process only");
    Check(!FreeIsForeign(Blocks::kNone, 1), "unknown blocks are not reported");
    // outside the players' turns (the server frame, the console) a free is not attributed: a resource a player's turn
    // loaded (a precached model) is freed by the server or a map change
    Check(!FreeIsForeign(1, kProcess), "a free outside the turns is not reported");
    b.Allocated(0x4000000, 0x20, 1);
    b.Allocated(0x4000100, 0x30, 1);
    b.Allocated(0x4000200, 0x10, 0);
    Check(b.Freed(0x4000200) == 0 && b.Freed(0x4000200) == Blocks::kNone, "freed once");
    auto [n, bytes] = b.Orphan(1);
    Check(n == 2 && bytes == 0x50 && b.At(0x4000000)->owner == kProcess, "a leaving player's blocks: counted, orphaned");
    b.OwnAll(kProcess);
    Check(b.At(0x1000000)->owner == kProcess, "the snapshot owns all");
}

static void Lifecycle() {
    using namespace nsplit;
    Players p;
    Check(p.Count() == 1 && p.At(0).state == State::Active && p.At(0).context == 0, "player 1 is the game's own");
    Check(p.FreeSlot() == 1 && p.Initialising() == -1, "slot 2 free");
    Check(!p.Begin(0, 1, "x") && !p.Begin(1, 0, "x"), "not player 1's slot, not context 0");
    Check(p.Begin(1, 1, "Player 2") && p.Initialising() == 1 && p.At(1).state == State::Initialising, "player 2 begins");
    Check(!p.Begin(1, 2, "again") && !p.Joined(1) && !p.Leave(1), "nothing else while initialising");
    Check(p.Initialised(1) && p.Initialising() == -1 && p.At(1).state == State::Connecting, "initialised: connecting");
    Check(p.Joined(1) && p.At(1).state == State::Active && p.Count() == 2, "joined");
    Check(p.ByPort(1) == 1 && p.ByPort(0) == 0 && p.ByPort(2) == -1, "players by port");
    // a third, then the second leaves: the third's context moves down
    Check(p.Begin(2, 2, "Player 3") && p.Initialised(2) && p.Joined(2), "player 3");
    Check(!p.Released(1) && p.Leave(1) && p.At(1).state == State::Leaving, "player 2 leaves");
    p.ContextRemoved(1);
    Check(p.Released(1) && p.At(1).state == State::Empty && p.At(2).context == 1 && p.Count() == 2,
          "released; player 3's context renumbered");
    // reconnection: the slot is taken again
    Check(p.FreeSlot() == 1 && p.Begin(1, 2, "Player 2") && p.Initialised(1) && p.Joined(1), "player 2 back");
    Check(p.FreeSlot() == 3 && p.Begin(3, 3, "Player 4") && p.FreeSlot() == -1, "four players, full");
    Check(std::string(Name(State::Connecting)) == "connecting", "state names");
    // registrations: the game's own initialisation as the title; another player's reuses the first's
    Check(RegisterPolicy(false, true) == Registration::Register, "the game's own: registers");
    Check(RegisterPolicy(true, true) == Registration::ReuseExisting, "another player's: the existing one");
    Check(RegisterPolicy(true, false) == Registration::Register, "a name only it registers: new");
    Check(EngineCommandPolicy(true) == Registration::Skip && EngineCommandPolicy(false) == Registration::Register,
          "Cmd_AddCommand never again after the start");
}

static std::vector<uint8_t> g_mem(0x4000);
static uint8_t* Map(uint32_t va) { return g_mem.data() + va; }

static void Contexts() {
    std::vector<ctx::Region> regions = {{0x100, 0x10, "a"}, {0x200, 0x20, "b"}};
    ctx::Switcher<uint8_t* (*)(uint32_t)> s(regions, &Map);
    Check(s.OffsetOf(0x100) == 0 && s.OffsetOf(0x10F) == 0xF && s.OffsetOf(0x200) == 0x10 && s.OffsetOf(0x150) == -1,
          "offsets in a copy");
    Check(s.AddressOf(0) == 0x100 && s.AddressOf(0x10) == 0x200 && s.AddressOf(0x2F) == 0x21F && s.AddressOf(0x30) == 0,
          "addresses of a copy's bytes");
    g_mem[0x100] = 1;
    s.CaptureInitial();
    Check(s.Add() == 1 && s.Add() == 2 && s.Add() == 3, "three more");
    s.Saved(2)[0] = 22;
    s.Saved(3)[0] = 33;
    Check(!s.Remove(0) && s.Switch(2) && !s.Remove(2), "neither 0 nor the active one");
    Check(s.Remove(1) && s.Active() == 1 && s.Count() == 3, "removing 1: the active one (was 2) is now 1");
    Check(g_mem[0x100] == 22 && s.Switch(2) && g_mem[0x100] == 33, "the others' states kept");
    Check(s.Switch(0) && g_mem[0x100] == 1, "player 1's too");
}

#include <chrono>
// the switch's cost on this machine, with the game's region layout (152 regions, 1 060 577 bytes): reported, not
// checked (a machine's speed is not a test's business)
static void SwitchCost() {
    static std::vector<uint8_t> big(0x200000);
    std::vector<ctx::Region> regions;
    uint32_t at = 0x1000, total = 0;
    for (int i = 0; i < 152; i++) {
        uint32_t n = i == 0 ? 0x3FBB0 : i == 1 ? 0x37088 : i == 2 ? 0x3E80C : i == 3 ? 0x1E1C8 : 0x400 + uint32_t(i) * 37;
        if (total + n > 1060577) n = 1060577 - total;
        if (!n) break;
        regions.push_back({at, n, "r"});
        at += n + 0x40;
        total += n;
    }
    auto map = [](uint32_t va) { return big.data() + va; };
    ctx::Switcher<decltype(map)> s(regions, map);
    s.CaptureInitial();
    s.Add();
    const int n = 400;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) s.Switch(i % 2 ? 0 : 1);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
    std::printf("switch cost: %zu regions, %zu bytes a player: %.3f ms a switch (copy out + copy in); two players "
                "switch four times a frame: %.3f ms a frame\n",
                regions.size(), s.Bytes(), ms, ms * 4);
}

int main() {
    SwitchCost();
    Loopback();
    Exchange();
    Heap();
    Lifecycle();
    Contexts();
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
