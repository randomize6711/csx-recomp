// tests/native_split_test.py builds and runs this: native split-screen's parts without the game (M2):
// src/game/local_loopback.h, heap_owner.h, native_split_core.h and client_context_core.h's removal
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "game/client_context_core.h"
#include "game/heap_owner.h"
#include "game/intern_pool.h"
#include "game/local_loopback.h"
#include "game/native_split_core.h"
#include "game/shared_once.h"

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
    // allocation identity: a later block at the same address is another allocation (serial, size, site)
    uint64_t first = b.At(0x1000200)->serial;
    b.Freed(0x1000200);
    b.Allocated(0x1000200, 0x40, kProcess, 0x1234);
    Check(b.At(0x1000200)->serial != first && b.At(0x1000200)->site == 0x1234, "a reused address: a new identity");

    // the census (verification only: it changes nothing), at each region's guest alignment, never across its end
    {
        Blocks h;
        h.Allocated(0x8000000, 0x48, kProcess, 0x1A7F91);
        std::vector<Span> spans = {{0, 0x10001, 3}, {3, 0x20000, 8}};
        std::vector<uint8_t> copy(11, 0);
        uint32_t p = 0x8000000;
        std::memcpy(copy.data() + 3, &p, 4);
        std::vector<uint8_t> before = copy;
        std::vector<Pointer> found = HeapPointers(copy, spans, h);
        Check(found.size() == 1 && found[0].offset == 3 && found[0].va == 0x20000 && found[0].block == 0x8000000,
              "a pointer after an odd-sized region found, at its guest address");
        Check(copy == before, "the census changes nothing");
        std::vector<uint8_t> c3(10, 0);
        std::memcpy(c3.data() + 2, &p, 4);  // guest 0x30004 of a region starting at 0x30002
        Check(HeapPointers(c3, {{0, 0x30002, 10}}, h).size() == 1, "a region starting at an odd address");
        std::vector<uint8_t> c4(8, 0);
        std::memcpy(c4.data() + 2, &p, 4);  // straddles the end of a 4-byte region
        Check(HeapPointers(c4, {{0, 0x40000, 4}, {4, 0x50002, 4}}, h).empty(), "never across a region's end");
        Check(HeapPointers(c4, {{6, 0x60000, 16}}, h).empty(), "never past the copy's end");
    }

    // kinds by allocating code
    const SiteRule rules[] = {{0x1BF2A, 0x1BF2B, Kind::SharedRegistry, "cvar"},
                              {0x42000, 0x43000, Kind::SharedScratch, "cvar strings"}};
    Check(KindOfSite(0x1BF2A, rules, 2) == Kind::SharedRegistry && KindOfSite(0x42500, rules, 2) == Kind::SharedScratch &&
              KindOfSite(0x1A7F91, rules, 2) == Kind::Owned,
          "kinds from the allocating code; anything else owned");
    // a block made through an allocation helper takes its caller's site and kind
    {
        Blocks h;
        h.Allocated(0xA000000, 0x20, 1, 0xC8EB6, Kind::Owned);  // malloc's caller: the helper
        Check(h.Reattribute(0xA000000, 0x42150, KindOfSite(0x42150, rules, 2)) &&
                  h.At(0xA000000)->kind == Kind::SharedScratch && h.At(0xA000000)->site == 0x42150 &&
                  h.At(0xA000000)->owner == 1,
              "a cvar string through the general allocator: shared scratch, its owner kept");
        Check(!h.Reattribute(0xA000004, 0x1, Kind::Owned), "not a block's start: nothing changed");
    }
    // frees and destructions, the kind weighed: shared kinds are anyone's, owned ones their owner's
    Block scratch{0x20, 0, 1, 0x42500, Kind::SharedScratch}, mine{0x20, 1, 2, 0x1A7F91, Kind::Owned},
        p1{0x20, 0, 3, 0x1A7F91, Kind::Owned}, process{0x20, kProcess, 4, 0x1A7F91, Kind::Owned};
    Check(!FreeIsForeign(scratch, 1) && MayDestroy(scratch, 1), "shared scratch: player 2 frees player 1's (the title)");
    Check(!FreeIsForeign(mine, 1) && MayDestroy(mine, 1), "player 2: its own");
    Check(FreeIsForeign(p1, 1) && !MayDestroy(p1, 1), "player 2: never player 1's object");
    Check(FreeIsForeign(process, 1) && !MayDestroy(process, 1), "player 2: never the process's object");
    Check(!FreeIsForeign(p1, 0) && !FreeIsForeign(mine, kProcess), "player 1 and the process as the title");
    Check(SkipFree(0, 1) && !SkipFree(1, 1) && !SkipFree(1, kProcess), "the skip: only a player after the first");
    Check(MayDestroy(1, kProcess) && !MayDestroy(Blocks::kNone, 1), "an unknown block is not destroyed by player 2");
    // a leaving player: its blocks counted and handed to the process
    b.Allocated(0x4000000, 0x20, 1);
    b.Allocated(0x4000100, 0x30, 1);
    b.Allocated(0x4000200, 0x10, 0);
    Check(b.Freed(0x4000200) == 0 && b.Freed(0x4000200) == Blocks::kNone, "freed once");
    auto [n, bytes] = b.Orphan(1);
    Check(n == 2 && bytes == 0x50 && b.At(0x4000000)->owner == kProcess, "a leaving player's blocks: counted, orphaned");
}

// two players built on a synthetic memory as AddPlayer builds them: player 1 by "the process's initialisation" (its
// objects the process's), player 2 from the boot image by its own steps; the reference check as AddPlayer makes it
static std::vector<uint8_t> g_two(0x10000);
static uint8_t* MapTwo(uint32_t va) { return g_two.data() + va; }
static void TwoPlayers() {
    using namespace heapown;
    std::vector<ctx::Region> regions = {{0x1001, 0x13, "odd"}, {0x2000, 0x40, "objects"}};
    ctx::Switcher<uint8_t* (*)(uint32_t)> s(regions, &MapTwo);
    std::vector<Span> spans = {{0, 0x1001, 0x13}, {0x13, 0x2000, 0x40}};
    Blocks heap;
    auto put = [](uint32_t va, uint32_t v) { std::memcpy(g_two.data() + va, &v, 4); };
    auto get = [](uint32_t va) { uint32_t v; std::memcpy(&v, g_two.data() + va, 4); return v; };
    // boot: the static objects constructed, no heap object
    put(0x2000, 0xC0DE);  // a vtable pointer (code, not heap)
    s.CaptureInitial();
    // the process's initialisation: player 1's objects, a registered cvar, its own state
    heap.Allocated(0x900000, 0x48, kProcess, 0x1A7F91);   // the 0x552C70-like object
    heap.Allocated(0x900100, 0x14, kProcess, 0x1BF2A, Kind::SharedRegistry);  // a cvar_t
    put(0x2004, 0x900000);
    put(0x2008, 0x900100);
    put(0x1004, 0x1234);  // plain data
    std::vector<uint8_t> reference(s.Bytes());
    size_t at = 0;
    for (const ctx::Region& r : regions) {
        std::memcpy(reference.data() + at, g_two.data() + r.start, r.size);
        at += r.size;
    }
    std::vector<Pointer> objects = HeapPointers(reference, spans, heap);
    Check(objects.size() == 2, "player 1's initialisation made two objects in its regions");
    // player 2: the boot image (no heap pointer), then its own steps
    int c = s.Add();
    Check(HeapPointers(s.Saved(c), spans, heap).empty(), "the boot image holds no heap pointer: nothing to inherit");
    Check(s.Switch(c) && get(0x2000) == 0xC0DE && get(0x2004) == 0 && get(0x1004) == 0, "player 2 starts at the boot");
    heap.Allocated(0x980000, 0x48, 1, 0x1A7F91);  // its own object (its initialisation, owner player 2)
    put(0x2004, 0x980000);
    put(0x2008, 0x900100);                         // the cvar registration answered with the registered one
    auto check = [&](size_t& own, size_t& missing, size_t& foreign) {
        own = missing = foreign = 0;
        for (const Pointer& p : objects) {
            uint32_t mine = get(p.va);
            const Block* b = mine ? heap.At(heap.Holding(mine)) : nullptr;
            if (!mine) missing++;
            else if (b && (b->kind != Kind::Owned || b->owner == 1)) own++;
            else foreign++;
        }
    };
    size_t own, missing, foreign;
    check(own, missing, foreign);
    Check(own == 2 && missing == 0 && foreign == 0, "player 2: its own object and the shared cvar; nothing missing");
    put(0x2004, 0);  // a step missing
    check(own, missing, foreign);
    Check(missing == 1, "a missing initialisation step fails the check");
    put(0x2004, 0x900000);  // player 1's object (inherited)
    check(own, missing, foreign);
    Check(foreign == 1, "an inherited object of player 1 fails the check");
    put(0x2004, 0x980000);
    Check(s.Switch(0) && get(0x2004) == 0x900000 && get(0x1004) == 0x1234, "player 1's state untouched");
    Check(s.Switch(c) && get(0x2004) == 0x980000, "player 2's own state kept");
    s.Switch(0);
}

// the start-up values check (docs 13.8, 14.6): what the process's start-up set for player 1 that a new player's own
// steps left at the boot value; a link (a function, read-only data or a static object the code takes, its own regions)
// fails, a number is reported, a heap pointer is the object check's
static bool FakeFunction(uint32_t v) { return v == 0x18B7A0 || v == 0x1A1140; }
static void StartupValues() {
    using namespace heapown;
    std::vector<Span> spans = {{0, 0x1001, 0x13}, {0x13, 0x2000, 0x20}};  // an odd start, as the real regions
    std::vector<uint8_t> boot(0x33), reference(0x33);
    auto put = [](std::vector<uint8_t>& c, size_t off, uint32_t v) { std::memcpy(c.data() + off, &v, 4); };
    Blocks heap;
    heap.Allocated(0x900000, 0x40, kProcess, 0xAF8C4);
    static const uint32_t taken[] = {0x32AFF4, 0x371970, 0x5BD8C8};  // sorted: addresses the code takes
    ValueMap map;
    map.function = FakeFunction;
    map.taken = taken;
    map.taken_count = 3;
    std::vector<Span> regions = spans;
    regions.push_back({0x33, 0x749D40, 0x88});  // cl's list heads (they point at cl itself)
    map.regions = &regions;
    put(boot, 0x13 + 0x0, 0xC0DE);       // 0x2000: a vtable set by a static constructor: the same in both
    put(reference, 0x13 + 0x0, 0xC0DE);
    put(reference, 0x13 + 0x4, 0x371970);  // 0x2004: a static link (a cvar's next, a list head)
    put(reference, 0x13 + 0x8, 0x3C);      // 0x2008: a value (the refresh rate)
    put(reference, 0x13 + 0xC, 0x900010);  // 0x200C: a heap pointer (the object check's)
    put(reference, 0x13 + 0x10, 7);        // 0x2010: a value player 2's own steps set again
    put(reference, 0x3, 0x5BD8C8);         // 0x1004 (offset 3: the guest alignment): a link in the odd region
    put(reference, 0x13 + 0x14, 0x2A0000); // 0x2014: a byte count in the image's range (run 5's false link): a number
    put(reference, 0x13 + 0x18, 0x18B7A0); // 0x2018: a function entry (a callback)
    put(reference, 0x13 + 0x1C, 0x749D44); // 0x201C: a list head pointing into its own regions
    std::map<uint32_t, uint32_t> mine = {{0x2010, 7}};
    auto read = [&](uint32_t va) { auto it = mine.find(va); return it == mine.end() ? 0u : it->second; };
    std::vector<StartupGap> gaps = StartupGaps(boot, reference, spans, heap, map, read);
    auto at = [&](uint32_t va) -> const StartupGap* { for (auto& g : gaps) if (g.va == va) return &g; return nullptr; };
    Check(gaps.size() == 6, "six start-up values player 2 lacks (four links, two numbers)");
    Check(at(0x2004) && at(0x2004)->link && at(0x2004)->kind == ValueKind::StaticObject && at(0x1004) &&
              at(0x1004)->link, "a static link it lacks is a link (fails)");
    Check(at(0x2008) && !at(0x2008)->link && at(0x2008)->reference == 0x3C, "a plain value it lacks is reported");
    Check(at(0x2014) && !at(0x2014)->link && at(0x2014)->kind == ValueKind::Number,
          "a count in the image's range names nothing: a number, not a link (run 5's statistics table)");
    Check(at(0x2018) && at(0x2018)->link && at(0x2018)->kind == ValueKind::Function, "a function entry is a link");
    Check(at(0x201C) && at(0x201C)->link && at(0x201C)->kind == ValueKind::Region, "a pointer into its regions is a link");
    Check(!at(0x200C), "a heap pointer is left to the object check");
    Check(!at(0x2000) && !at(0x2010), "the same at boot, or set again by its own steps: not a gap");
    mine[0x2004] = 0x371970;
    mine[0x1004] = 0x5BD8C8;
    mine[0x2008] = 0x3C;
    mine[0x2014] = 0x2A0000;
    mine[0x2018] = 0x18B7A0;
    mine[0x201C] = 0x749D44;
    Check(StartupGaps(boot, reference, spans, heap, map, read).empty(), "every value made again: no gap");
    mine[0x2008] = 0x1E;  // its own, different value (not the boot one): its own, not a gap
    Check(StartupGaps(boot, reference, spans, heap, map, read).empty(), "a different value of its own is not a gap");
    // the classification itself
    Check(Classify(0x32AFF4, map, heap) == ValueKind::ReadOnly, "a taken .rdata address: read-only data");
    Check(Classify(0x32AFF8, map, heap) == ValueKind::Number, "an untaken .rdata address: a number");
    Check(Classify(0x1A1141, map, heap) == ValueKind::Number, "inside a function, not its entry: a number");
    Check(Classify(0x900020, map, heap) == ValueKind::Heap, "inside a heap block: the heap");
    Check(Classify(0xFFFF, map, heap) == ValueKind::Number, "below the image: a number");
    Check(Classify(0x371970, ValueMap{}, heap) == ValueKind::Number, "nothing known of the image: a number");
}

// the protected start-up state (docs 14.6): its bytes before a new player's steps; a changed run is found, a run
// never crosses a range, unchanged state reports nothing
static void ProtectedState() {
    using namespace heapown;
    std::map<uint32_t, uint8_t> mem;
    auto read = [&](uint32_t va) { auto it = mem.find(va); return it == mem.end() ? uint8_t(0) : it->second; };
    std::vector<Range> ranges = {{0x552C38, 4}, {0x552C40, 4}, {0x543AD0, 0x1A00}, {0x371178, 4}};
    mem[0x552C40] = 1;     // the material tables' flag, set by player 1's start-up
    mem[0x543AD0] = 'c';   // a table's first name
    std::vector<uint8_t> before = Snapshot(ranges, read);
    Check(before.size() == 4 + 4 + 0x1A00 + 4, "the snapshot holds every protected byte");
    Check(Changed(ranges, before, read).empty(), "nothing changed: nothing reported");
    mem[0x543AD0] = 0;     // the table cleared (rep stosd) and rebuilt by a new player's steps
    mem[0x543AD1] = 0x41;
    mem[0x543AD2] = 0x42;
    mem[0x37117B] = 0x99;  // a cvar's name pointer changed
    std::vector<Range> c = Changed(ranges, before, read);
    Check(c.size() == 2, "two changed runs");
    Check(c.size() == 2 && c[0].va == 0x543AD0 && c[0].size == 3, "the table's run: its address and length");
    Check(c.size() == 2 && c[1].va == 0x37117B && c[1].size == 1, "the cvar link's byte");
    // adjacent ranges: a run ends at its range's end
    std::vector<Range> adj = {{0x1000, 2}, {0x1002, 2}};
    std::vector<uint8_t> b2 = Snapshot(adj, read);
    mem[0x1001] = 1;
    mem[0x1002] = 1;
    std::vector<Range> c2 = Changed(adj, b2, read);
    Check(c2.size() == 2 && c2[0].va == 0x1001 && c2[1].va == 0x1002, "a run never crosses a range");
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
    // a failed initialisation: the slot empty again, never connecting; a later attempt can take it
    Players f;
    Check(f.Begin(1, 1, "Player 2") && f.InitFailed(1) && f.At(1).state == State::Empty && f.Initialising() == -1,
          "a failed initialisation leaves the slot empty");
    Check(!f.InitFailed(1) && !f.Joined(1), "and nothing else follows from it");
    // the initialisation's order: the boot image first, the client's state before its game code, the check last
    auto order = InitOrder();
    Check(order.front() == InitStep::BootImage && order.back() == InitStep::Check, "boot image first, check last");
    auto pos = [&](InitStep x) { for (size_t i = 0; i < order.size(); i++) if (order[i] == x) return int(i); return -1; };
    Check(pos(InitStep::ClientState) < pos(InitStep::Initialize) && pos(InitStep::Userinfo) < pos(InitStep::Initialize) &&
              pos(InitStep::Initialize) < pos(InitStep::HudInit),
          "client state and userinfo before Initialize, Initialize before HUD_Init");
    // the connect's shared registry built once: player 1 and a first registration use the shared slot; a later player,
    // once the name is registered, its own; reconnecting, the same own slot again (its previous description its own)
    const uint32_t own = 0x700D1FF0, elem = 4;
    Check(DeltaSlot(0x6000, 0x9000, 0, own, elem) == 0x6000, "player 1: the shared registry");
    Check(DeltaSlot(0x6000, 0, 1, own, elem) == 0x6000, "a first registration of a name: the shared slot");
    Check(DeltaSlot(0x6000, 0x9000, 1, own, elem) == own + 4 && DeltaSlot(0x6000, 0x9000, 3, own, elem) == own + 12,
          "players 2 and 4: their own slots, the registry untouched");
    Check(DeltaSlot(0x6000, 0x9000, 1, own, elem) == DeltaSlot(0x6000, 0x9000, 1, own, elem), "the same slot on a rejoin");
    Check(DeltaSlot(0, 0, 1, own, elem) == 0, "no slot from the lookup: left as it is");
    // registrations: the game's own initialisation as the title; another player's reuses the first's
    Check(RegisterPolicy(false, true) == Registration::Register, "the game's own: registers");
    Check(RegisterPolicy(true, true) == Registration::ReuseExisting, "another player's: the existing one");
    Check(RegisterPolicy(true, false) == Registration::Register, "a name only it registers: new");
    Check(EngineCommandPolicy(true) == Registration::Skip && EngineCommandPolicy(false) == Registration::Register,
          "Cmd_AddCommand never again after the start");
}

// per-player connection buffers: cls.datagram's buffer is inside cls (the per-player region), at the same address for
// every player; a switch keeps each player's bytes
static std::vector<uint8_t> g_cls(0x2000);
static uint8_t* MapCls(uint32_t va) { return g_cls.data() + va; }
static void ConnectionBuffers() {
    ctx::Switcher<uint8_t* (*)(uint32_t)> s({{0x1000, 0x400, "cls"}}, &MapCls);
    s.CaptureInitial();
    std::memset(g_cls.data() + 0x1100, 'A', 64);  // player 1's datagram
    int c = s.Add();
    Check(s.Switch(c) && g_cls[0x1100] == 0, "player 2's buffer starts empty");
    std::memset(g_cls.data() + 0x1100, 'B', 64);
    Check(s.Switch(0) && g_cls[0x1100] == 'A' && g_cls[0x113F] == 'A', "player 1's datagram kept");
    Check(s.Switch(c) && g_cls[0x1100] == 'B', "player 2's kept");
    s.Switch(0);
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

// docs 15: the world's shared-once operations, a connection's and the session's lifecycle. A simulated client connect
// drives sharedonce::World exactly as native_split.cpp's hooks do (Begin; Run: the whole operation as the process,
// Finished; PerPlayerOnly: the player's own part; Inner / Title: as the title)
namespace sim {
struct Engine {
    sharedonce::World world;
    heapown::Blocks heap;
    int builds = 0;                 // the shared part (lightmaps, decals, particles) built
    int decal_pool_clears = 0;      // R_DecalInit: harmful on a world another player uses
    int view_resets[nsplit::kMaxPlayers] = {};  // the per-player part (shake, fade, cl_entities[0])
    int material_loads = 0;
    std::string materials_of;       // the list's map
    uint32_t next_block = 0x100000;
    bool fail_next = false;         // the next shared run is cut short (its host call unwinds)
    std::vector<uint32_t> shared_blocks;  // the world's allocations
    // the owner a hook gives an allocation: a shared run's is the process's, else the acting player's
    int Owner(int player) const { return world.Running() ? heapown::kProcess : player; }
    // R_NewMap through the hook (0x4F8D0)
    sharedonce::Action NewMap(int player, sharedonce::Generation g) {
        sharedonce::Action a = world.Begin(sharedonce::Op::WorldRenderer, g);
        switch (a) {
        case sharedonce::Action::Run: {
            struct Run {
                Engine& e;
                bool ok = false;
                ~Run() { e.world.Finished(sharedonce::Op::WorldRenderer, ok); }
            } run{*this};
            // the hook's own call of R_NewMap: the hook again (Inner), then the whole function
            if (world.Begin(sharedonce::Op::WorldRenderer, g) != sharedonce::Action::Inner) return a;
            uint32_t b = next_block;
            next_block += 0x100;
            heap.Allocated(b, 0x100, Owner(player), 0x52AB3, heapown::Kind::Owned);  // a lightmap buffer
            shared_blocks.push_back(b);
            if (fail_next) {
                fail_next = false;
                return a;  // cut short: run.ok stays false
            }
            builds++;
            decal_pool_clears++;
            view_resets[player]++;
            run.ok = true;
            break;
        }
        case sharedonce::Action::PerPlayerOnly:
            view_resets[player]++;
            break;
        case sharedonce::Action::Inner:
        case sharedonce::Action::Title:
            builds++;
            decal_pool_clears++;
            view_resets[player]++;
            break;
        }
        return a;
    }
    // the HUD's map poll (0x1AE5FA): the material list by the map's name
    sharedonce::Action Materials(int player, const std::string& map) {
        sharedonce::Action a = world.Begin(sharedonce::Op::Materials, {0, 0, map});
        if (a == sharedonce::Action::Run) {
            uint32_t b = next_block;
            next_block += 0x4C;
            heap.Allocated(b, 0x4C, Owner(player), 0x1DBA29, heapown::Kind::Owned);
            shared_blocks.push_back(b);
            material_loads++;
            materials_of = map;
            world.Finished(sharedonce::Op::Materials, true);
        } else if (a == sharedonce::Action::Title) {
            material_loads++;
        }
        return a;
    }
};
}  // namespace sim

static void SharedOnce() {
    using sharedonce::Action;
    using sharedonce::Generation;
    const Generation de_dust{7, 0x6E8900, ""}, de_dust_again{8, 0x6E8900, ""}, cs_office{9, 0x6E8A40, ""};
    // player 1 connects: the world built once, as the process
    sim::Engine e;
    Check(e.NewMap(0, de_dust) == Action::Run, "shared once: the first client of a world builds it");
    Check(e.builds == 1 && e.view_resets[0] == 1, "shared once: the build includes the first client's own part");
    Check(!e.world.Running(), "shared once: no run left in progress");
    // player 2 joins the same world: its own part only, nothing shared rebuilt
    Check(e.NewMap(1, de_dust) == Action::PerPlayerOnly, "shared once: player 2 joining does not rebuild the world");
    Check(e.builds == 1 && e.decal_pool_clears == 1, "shared once: player 1's decal pool not cleared by player 2");
    Check(e.view_resets[1] == 1, "shared once: player 2's own part still runs (its view reset, its entity 0)");
    Check(e.NewMap(2, de_dust) == Action::PerPlayerOnly && e.NewMap(3, de_dust) == Action::PerPlayerOnly &&
              e.builds == 1 && e.view_resets[2] == 1 && e.view_resets[3] == 1,
          "shared once: players 3 and 4 the same; built exactly once");
    Check(e.world.Runs(sharedonce::Op::WorldRenderer) == 1 && e.world.Skipped(sharedonce::Op::WorldRenderer) == 3,
          "shared once: counted: one build, three players' parts");
    // the shared allocation is the process's: no player's free of it is the title's, none orphaned by a leave
    Check(e.heap.At(e.shared_blocks[0]) && e.heap.At(e.shared_blocks[0])->owner == heapown::kProcess,
          "shared once: a build's allocation is the process's, not the building player's");
    auto orphaned = e.heap.Orphan(0);
    Check(orphaned.first == 0, "shared once: player 1 leaving orphans nothing of the world");
    Check(e.world.Done(sharedonce::Op::WorldRenderer, de_dust), "shared once: the world stays built when a player leaves");
    // a new map generation (a restart of the same map: the spawn count moved; the same model pointer): built again
    Check(e.NewMap(1, de_dust_again) == Action::Run && e.builds == 2,
          "shared once: a new generation of the same map is built again (by whichever player gets there first)");
    Check(e.NewMap(0, de_dust_again) == Action::PerPlayerOnly && e.builds == 2 && e.view_resets[0] == 2,
          "shared once: player 1 coming second to the new generation does its own part only");
    Check(e.NewMap(0, cs_office) == Action::Run && e.builds == 3, "shared once: a different map is built");
    // the old generation is not taken as built once another one is
    Check(!e.world.Done(sharedonce::Op::WorldRenderer, de_dust_again), "shared once: one generation at a time");
    // the world's end (the server shut down): everything forgotten, built again for the next world
    e.world.Invalidate();
    Check(e.NewMap(0, cs_office) == Action::Run && e.builds == 4,
          "shared once: after the server's shutdown the same generation is built again");
    // a run cut short: not recorded; the next caller of the generation builds it (never a half-built world skipped)
    e.world.Invalidate();
    e.fail_next = true;
    Check(e.NewMap(1, cs_office) == Action::Run && e.builds == 4, "shared once: a failed run builds nothing");
    Check(!e.world.Running() && e.world.Failed(sharedonce::Op::WorldRenderer) == 1,
          "shared once: a failed run is finished as failed (its in-progress state cleared)");
    Check(!e.world.Done(sharedonce::Op::WorldRenderer, cs_office), "shared once: a failed run is not taken as built");
    Check(e.NewMap(0, cs_office) == Action::Run && e.builds == 5, "shared once: after a failure the next caller builds");
    // no generation (no world model): as the title, nothing recorded
    Check(e.NewMap(0, Generation{3, 0, ""}) == Action::Title && !e.world.Running(),
          "shared once: no world: run as the title, nothing recorded");
    Check(e.world.Done(sharedonce::Op::WorldRenderer, cs_office), "shared once: a title run leaves the record alone");
    // Finished without a run (an unpaired call) changes nothing
    e.world.Finished(sharedonce::Op::WorldRenderer, true);
    Check(e.world.Runs(sharedonce::Op::WorldRenderer) == 5 && e.builds == 6,
          "shared once: an unpaired Finished counts nothing (five recorded builds and one title run so far)");
    // the operations are independent: the signon's UI unload has its own record
    sharedonce::World w;
    Check(w.Begin(sharedonce::Op::WorldRenderer, de_dust) == Action::Run, "shared once: begin one operation");
    Check(w.Begin(sharedonce::Op::SignonUi, de_dust) == Action::Run, "shared once: another runs alongside");
    Check(w.Begin(sharedonce::Op::WorldRenderer, de_dust) == Action::Inner,
          "shared once: its own call inside a run is the inner one (the title's function runs)");
    w.Finished(sharedonce::Op::SignonUi, true);
    Check(w.Running(), "shared once: still running while one operation is");
    w.Finished(sharedonce::Op::WorldRenderer, true);
    Check(!w.Running() && w.Begin(sharedonce::Op::SignonUi, de_dust) == Action::PerPlayerOnly,
          "shared once: the UI unload done once for the generation");
    // the material list (texture-reload ownership): by the map's name; a reload only for a different map
    sim::Engine m;
    Check(m.Materials(0, "de_dust") == Action::Run && m.material_loads == 1, "materials: the first poll loads the list");
    Check(m.Materials(1, "de_dust") == Action::PerPlayerOnly && m.material_loads == 1,
          "materials: player 2's first poll of the same map does not reload it");
    Check(m.Materials(1, "cs_office") == Action::Run && m.material_loads == 2 && m.materials_of == "cs_office",
          "materials: a different map is never skipped (player 2 first at the new map loads it)");
    Check(m.Materials(0, "cs_office") == Action::PerPlayerOnly && m.material_loads == 2,
          "materials: player 1 then finds it loaded");
    Check(m.Materials(0, "de_dust") == Action::Run && m.material_loads == 3, "materials: back to the first map: loaded");
    Check(m.Materials(0, "") == Action::Title && m.material_loads == 4, "materials: no name: as the title");
    for (uint32_t b : m.shared_blocks)
        Check(m.heap.At(b) && m.heap.At(b)->owner == heapown::kProcess, "materials: the list's nodes are the process's");
    // a player's later free of the world's block is foreign (refused, as any player's own code freeing it); the
    // process (a shared run, or outside the turns) may free any
    Check(heapown::FreeIsForeign(heapown::kProcess, 1), "ownership: player 2 freeing the world's block is refused");
    Check(!heapown::FreeIsForeign(0, heapown::kProcess), "ownership: a shared run may free player 1's old block");
    Check(!heapown::FreeIsForeign(1, heapown::kProcess), "ownership: a shared run may free player 2's old block");
}

static void SessionLifecycle() {
    nsplit::Session s;
    // a second local client's disconnect never ends the server and world; the host's does
    Check(!s.DisconnectEndsServer(1) && !s.DisconnectEndsServer(3), "session: an added player's disconnect keeps the server");
    Check(s.DisconnectEndsServer(0), "session: the host's disconnect ends the match");
    nsplit::Session other{2};
    Check(other.DisconnectEndsServer(2) && !other.DisconnectEndsServer(0), "session: the host is explicit, not slot 0");
    // the module validation: one machine, only the host starts it
    Check(nsplit::StartsModuleValidation(s, 0) && !nsplit::StartsModuleValidation(s, 1),
          "session: only the host starts the secure-module validation (the transfer machine is not reset by player 2)");
    // the teardown before the world is freed: the other players, highest slot first, the host never
    nsplit::Players p;
    p.Begin(1, 1, "p2");
    p.Initialised(1);
    p.Joined(1);
    p.Begin(2, 2, "p3");
    p.Initialised(2);  // still connecting
    p.Begin(3, 3, "p4");
    p.InitFailed(3);   // failed its initialisation: nothing to remove
    int n = 0;
    auto order = nsplit::TeardownOrder(p, s.host, n);
    Check(n == 2 && order[0] == 2 && order[1] == 1, "teardown: connecting and active players, highest slot first");
    for (int i = 0; i < n; i++) Check(p.Leave(order[size_t(i)]) && p.Released(order[size_t(i)]), "teardown: each leaves");
    order = nsplit::TeardownOrder(p, s.host, n);
    Check(n == 0 && p.Count() == 1 && p.At(0).state == nsplit::State::Active, "teardown: only the host is left");
    // no invalid transitions: a player is not removed twice, the host is never removed, an initialising one is not
    Check(!p.Leave(1) && !p.Leave(0), "teardown: no second removal, never the host's slot");
    p.Begin(1, 1, "p2");
    Check(!p.Leave(1), "teardown: a player still initialising is not removed (its initialisation ends it)");
    order = nsplit::TeardownOrder(p, s.host, n);
    Check(n == 0, "teardown: an initialising player is not in the teardown");
    Check(p.InitFailed(1) && p.Count() == 1, "teardown: a failed initialisation empties its slot");
    // another host: player 1 is torn down like any other
    nsplit::Players q;
    q.Begin(1, 1, "p2");
    q.Initialised(1);
    q.Joined(1);
    order = nsplit::TeardownOrder(q, 1, n);
    Check(n == 1 && order[0] == 0, "teardown: with another host, player 1 is removed first");
}

// the timer's once-per-process block (docs 15.7): FC870 as the code has it (its guard 0x5C9660, the period 0x5C9640
// = C / the Xbox's constant frequency 0x2BB5C755, its fields 0x5C9648..0x5C9658 cleared; after the block, at
// FC8CC, the timing itself) and the hook at FC8CC that ends the process's own call there
namespace timer {
struct Sim {
    std::map<uint32_t, uint8_t> mem;
    bool block_only = false;  // NativeSplitTimerBlockOnly
    int blocks = 0;           // the guarded block's runs
    uint32_t R(uint32_t va) const {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            auto it = mem.find(va + uint32_t(i));
            v |= uint32_t(it == mem.end() ? 0 : it->second) << (8 * i);
        }
        return v;
    }
    void W(uint32_t va, uint32_t v) {
        for (int i = 0; i < 4; i++) mem[va + uint32_t(i)] = uint8_t(v >> (8 * i));
    }
    uint8_t Byte(uint32_t va) const {
        auto it = mem.find(va);
        return it == mem.end() ? 0 : it->second;
    }
    void Call(uint32_t ebx, uint32_t arg) {
        if (!(R(0x5C9660) & 1)) {  // FC87B .. FC8C6
            W(0x5C9660, R(0x5C9660) | 1);
            for (uint32_t va : {0x5C9640u, 0x5C9648u, 0x5C964Cu, 0x5C9650u, 0x5C9654u, 0x5C9658u}) W(va, 0);
            float period = 1000.0f / float(0x2BB5C755);
            uint32_t bits;
            std::memcpy(&bits, &period, 4);
            W(0x5C9640, bits);
            blocks++;
        }
        if (block_only) return;  // the hook at FC8CC: on to the epilogue FC954
        W(0x5BD9D8, arg);        // FC90E
        if (ebx != 0x270F) {     // FC920 .. FC944 (simplified: the start time kept, the timer running)
            W(0x5C9650, 0);
            W(0x5C9654, 0);
            W(0x5C9648, 0x1234 + arg);
            W(0x5C9658, 1);
        }
        W(0x56169C, ebx);  // FC94E
    }
    // the process's call at the end of Host_Init (native_split.cpp NativeSplitReference)
    nsplit::Once ProcessOnce() {
        uint32_t guard = R(0x5C9660);
        bool ran = false;
        if (!(guard & 1)) {
            block_only = true;
            Call(0, 0);
            block_only = false;
            ran = true;
        }
        return nsplit::TimerOnce(guard, ran, R(0x5C9660), R(0x5C9640));
    }
};
}  // namespace timer

static void ProcessTimer() {
    using heapown::Range;
    const std::vector<Range> prot = {{0x5C9640, 4}, {0x5C9660, 4}};  // client_context_protected.inc's two entries
    // the title's order (one player, or native split before this): the first call, wherever it comes, runs the block
    timer::Sim title;
    title.Call(5, 77);
    // native split: the process first, then (later) the same first call of the title's
    timer::Sim ns;
    nsplit::Once once = ns.ProcessOnce();
    Check(once == nsplit::Once::Ran && ns.blocks == 1, "the block runs once, as the process, at the end of Host_Init");
    Check(nsplit::MayAddPlayers(once), "players may be added once it ran");
    Check(ns.R(0x5BD9D8) == 0 && ns.R(0x56169C) == 0 && ns.R(0x5C9658) == 0,
          "the process's call stops after the block: no timing, no time stored");
    Check((ns.R(0x5C9660) & 1) && ns.R(0x5C9640) != 0, "the guard set and the period computed");
    // a new player's initialisation calls the timer: the protected state is as it was, the block not run again
    auto read = [&](uint32_t va) { return ns.Byte(va); };
    std::vector<uint8_t> before = heapown::Snapshot(prot, read);
    ns.Call(5, 77);
    Check(heapown::Changed(prot, before, read).empty(), "player 2's call changes no protected timer byte");
    Check(ns.blocks == 1, "the block ran once for the process (not again in player 2's initialisation)");
    // the same state as the title's own first call: the original calculation, the shared fields alike
    bool same = true;
    for (uint32_t va = 0x5C9640; va < 0x5C9664; va++) same &= ns.Byte(va) == title.Byte(va);
    for (uint32_t va : {0x5BD9D8u, 0x56169Cu}) same &= ns.R(va) == title.R(va);
    Check(same, "process first, then the call: the timer's state is the title's after its first call");
    // a second process start-up step (a second reference: never in one process, but it must not run the block again)
    Check(ns.ProcessOnce() == nsplit::Once::AlreadyDone && ns.blocks == 1, "the guard already set: nothing run");
    // without it (the sixth run, docs 15.6): player 2's initialisation made the first call; the check finds it
    timer::Sim old;
    auto read_old = [&](uint32_t va) { return old.Byte(va); };
    std::vector<uint8_t> b0 = heapown::Snapshot(prot, read_old);
    old.Call(5, 77);
    std::vector<Range> c = heapown::Changed(prot, b0, read_old);
    Check(c.size() == 2 && c[0].va == 0x5C9640 && c[1].va == 0x5C9660,
          "the first call in a player's initialisation changes both protected values (still a failure)");
    Check(!nsplit::MayAddPlayers(nsplit::Once::Pending), "no player before the process's step (pending)");
    // a run that left the guard clear, or no period, fails closed
    Check(nsplit::TimerOnce(0, true, 0, 0x3A800000) == nsplit::Once::Failed, "the guard still clear: failed");
    Check(nsplit::TimerOnce(0, true, 1, 0) == nsplit::Once::Failed, "no period: failed");
    Check(!nsplit::MayAddPlayers(nsplit::Once::Failed), "failed: no player is added");
}

// the tempent pool's free list (docs 15.6): CL_TempEntInit (0x3A8A0) as the code has it, in a new player's own copy:
// its pool's links its own, player 1's untouched
static void TempEntPool() {
    constexpr uint32_t kPool = 0x3AEF00, kEnd = 0x4251A8, kFirst = 0x3AEF2C, kLast = 0x424A3C, kStride = 0x790;
    auto init = [&](std::vector<uint8_t>& m) {  // m: the pool's bytes, guest address kPool + offset
        std::fill(m.begin(), m.end(), 0);       // rep stosd 0x1D8A8 dwords
        auto w = [&](uint32_t va, uint32_t v) { std::memcpy(m.data() + (va - kPool), &v, 4); };
        for (uint32_t a = kFirst; a < kLast; a += kStride) w(a, a + 0x764);
        w(kLast, 0);
        w(0x4251A0, kPool);
        w(0x4251A4, 0);
    };
    std::vector<uint8_t> p1(kEnd - kPool), p2(kEnd - kPool, 0);
    init(p1);
    std::vector<uint8_t> p1_before = p1;
    init(p2);  // the new player's ClientState step, in its own context
    Check(p1 == p1_before, "player 2's pool initialisation leaves player 1's pool as it was");
    Check(p2 == p1, "player 2's pool links as player 1's start-up linked its own (250 links, none lacking)");
    uint32_t link = 0;
    std::memcpy(&link, p2.data() + (kFirst - kPool), 4);
    Check(link == kFirst + 0x764 && link >= kPool && link < kEnd, "a link names an entry of the same pool");
    Check((kLast - kFirst) / kStride == 249, "250 entries linked (the last one 0)");
}

// ---- docs 15.8: the string-intern pool, realloc's moves, a refused player's rollback --------------------------------

// a guest memory of a few ranges (sparse), and a heap that hands out blocks at increasing addresses
struct GuestMem {
    std::map<uint32_t, uint8_t> m;
    uint8_t Rd(uint32_t va) const {
        auto it = m.find(va);
        return it == m.end() ? 0 : it->second;
    }
    void W8(uint32_t va, uint8_t v) { m[va] = v; }
    void W16(uint32_t va, uint16_t v) { W8(va, uint8_t(v)), W8(va + 1, uint8_t(v >> 8)); }
    void W32(uint32_t va, uint32_t v) { W16(va, uint16_t(v)), W16(va + 2, uint16_t(v >> 16)); }
    uint32_t R32(uint32_t va) const { return Rd(va) | Rd(va + 1) << 8 | Rd(va + 2) << 16 | uint32_t(Rd(va + 3)) << 24; }
    uint16_t R16(uint32_t va) const { return uint16_t(Rd(va) | Rd(va + 1) << 8); }
    internpool::ReadByte Reader() const {
        return [this](uint32_t va) { return Rd(va); };
    }
};

// the title's pool, as 0x115C70 builds it and 0x115D10 appends to it (the tree's balance left out: the root stays the
// first node); buffers moved as realloc may move them, tracked in `blocks` as the heap hooks would
struct PoolSim {
    GuestMem& g;
    heapown::Blocks& blocks;
    uint32_t heap = 0x01000000;  // the next block's address
    static constexpr uint32_t P = internpool::kPool;
    PoolSim(GuestMem& gm, heapown::Blocks& b, uint32_t nodes) : g(gm), blocks(b) {
        g.W32(P + internpool::kLess, internpool::kLessInsensitive);
        uint32_t n = Alloc(nodes * 12, heapown::kProcess);
        g.W32(P + internpool::kNodes, n);
        g.W32(P + internpool::kNodesAlloc, nodes);
        g.W32(P + internpool::kNodesGrow, 0);
        g.W16(P + internpool::kRoot, 0xFFFF), g.W16(P + internpool::kCount, 0);
        g.W16(P + internpool::kFree, 0xFFFF), g.W16(P + internpool::kNext, 0);
        g.W32(P + internpool::kStrings, 0), g.W32(P + internpool::kStringsAlloc, 0);
        g.W32(P + internpool::kStringsGrow, 0x100), g.W32(P + internpool::kStringsSize, 0);
    }
    uint32_t Alloc(uint32_t size, int owner) {
        uint32_t va = heap;
        heap += (size + 0xFFF) & ~0xFFFu;
        blocks.Allocated(va, size, owner);
        return va;
    }
    // realloc: a new block, the bytes copied, the old freed (the heap's own move: only Moved records it)
    uint32_t Realloc(uint32_t old, uint32_t old_size, uint32_t size, int mover) {
        uint32_t va = heap;
        heap += (size + 0xFFF) & ~0xFFFu;
        for (uint32_t k = 0; k < old_size; k++) g.W8(va + k, g.Rd(old + k));
        blocks.Moved(old, va, size, mover);
        return va;
    }
    uint16_t Intern(const std::string& s, int who) {
        uint16_t next = g.R16(P + internpool::kNext);
        uint32_t nodes = g.R32(P + internpool::kNodes), str = g.R32(P + internpool::kStrings);
        for (uint16_t i = 0; i < next; i++) {  // the find (0x115DC0)
            uint32_t off = g.R32(nodes + i * 12 + 8);
            std::string t;
            for (uint32_t k = off; g.Rd(str + k); k++) t.push_back(char(g.Rd(str + k)));
            if (t == s) return i;
        }
        // the strings: reserve at the end (0x113460 / 0x113A40), copied with the NUL
        uint32_t size = g.R32(P + internpool::kStringsSize), alloc = g.R32(P + internpool::kStringsAlloc);
        uint32_t need = uint32_t(s.size()) + 2;
        if (size + need > alloc) {
            uint32_t grown = alloc;
            while (size + need > grown) grown += 0x100;
            str = str ? Realloc(str, alloc, grown, who) : Alloc(grown, who);
            g.W32(P + internpool::kStrings, str);
            g.W32(P + internpool::kStringsAlloc, grown);
        }
        for (size_t k = 0; k <= s.size(); k++) g.W8(str + size + uint32_t(k), k < s.size() ? uint8_t(s[k]) : 0);
        g.W32(P + internpool::kStringsSize, size + need);
        // the node (0x115FC0: the next one, the nodes doubled when used up)
        uint32_t nalloc = g.R32(P + internpool::kNodesAlloc);
        if (next == nalloc) {
            nodes = Realloc(nodes, nalloc * 12, nalloc * 2 * 12, who);
            g.W32(P + internpool::kNodes, nodes);
            g.W32(P + internpool::kNodesAlloc, nalloc * 2);
        }
        g.W32(nodes + next * 12 + 8, size);
        g.W16(P + internpool::kNext, uint16_t(next + 1));
        g.W16(P + internpool::kCount, uint16_t(g.R16(P + internpool::kCount) + 1));
        if (next == 0) g.W16(P + internpool::kRoot, 0);
        return next;
    }
};

static void InternPool() {
    GuestMem g;
    heapown::Blocks blocks;
    PoolSim pool(g, blocks, 4);
    // the process's start-up: a few paths (the process's own)
    uint16_t a = pool.Intern("maps/de_dust2.bsp", heapown::kProcess), b = pool.Intern("sound/a.wav", heapown::kProcess);
    Check(a == 0 && b == 1 && pool.Intern("maps/de_dust2.bsp", heapown::kProcess) == 0, "an index per string, found again");
    auto block = [&](uint32_t va) {
        const heapown::Block* bl = blocks.At(va);
        return bl ? internpool::BlockInfo{true, bl->owner, bl->size} : internpool::BlockInfo{};
    };
    auto reader = g.Reader();
    internpool::State before = internpool::Capture(reader);
    Check(before.complete && before.entries.size() == 2 && before.entries[1].text == "sound/a.wav",
          "the pool captured: its nodes and strings");
    // legitimate growth in player 2's steps (slot 1): in place, then with both buffers moved (realloc)
    pool.Intern("xbox_cs_ForceFeedbackEffects.dat", 1);
    auto in_place = internpool::Check(before, internpool::Capture(reader), block);
    Check(in_place.empty(), "growth: a string appended is no problem");
    uint32_t str0 = g.R32(internpool::kPool + internpool::kStrings), nodes0 = g.R32(internpool::kPool + internpool::kNodes);
    for (int i = 0; i < 20; i++) pool.Intern("cstrike/file_" + std::to_string(i) + "_with_a_long_name.txt", 1);
    internpool::State grown = internpool::Capture(reader);
    Check(grown.h.strings != str0 && grown.h.nodes != nodes0, "the strings' buffer and the node array moved");
    auto moved = internpool::Check(before, grown, block);
    Check(moved.empty(), "growth with both buffers moved: every invariant holds");
    Check(grown.entries[0].text == "maps/de_dust2.bsp" && grown.entries[1].text == "sound/a.wav" &&
              pool.Intern("sound/a.wav", 1) == 1,
          "the indices given before name the same strings after the moves");
    Check(blocks.At(grown.h.strings) && blocks.At(grown.h.strings)->owner == heapown::kProcess &&
              blocks.At(grown.h.nodes)->owner == heapown::kProcess && !blocks.At(str0),
          "a moved buffer stays the process's (realloc keeps the owner), the old address no block");
    // each broken invariant is found
    auto broken = [&](auto mutate, const char* what) {
        GuestMem saved = g;
        heapown::Blocks saved_blocks = blocks;
        mutate();
        auto p = internpool::Check(before, internpool::Capture(reader), block);
        Check(!p.empty(), what);
        g = saved;
        blocks = saved_blocks;
    };
    constexpr uint32_t P = internpool::kPool;
    broken([&] { g.W32(P + internpool::kLess, 0x123456); }, "the less function changed: a problem");
    broken([&] { g.W32(P + internpool::kLess, internpool::kLessSensitive); }, "the other less function: a problem");
    broken([&] { g.W32(P + internpool::kStringsGrow, 0x200); }, "the string grow changed: a problem");
    broken([&] { g.W32(P + internpool::kNodesGrow, 8); }, "the node grow changed: a problem");
    broken([&] { g.W32(P + internpool::kStringsSize, 4); }, "the strings shrank: a problem");
    broken([&] { g.W16(P + internpool::kCount, 1); }, "a node removed (count below next): a problem");
    broken([&] { g.W16(P + internpool::kFree, 0); }, "a free list (a removal): a problem");
    broken([&] { g.W32(P + internpool::kStringsSize, g.R32(P + internpool::kStringsAlloc) + 1); },
           "more string bytes than allocated: a problem");
    broken([&] { g.W32(grown.h.nodes + 1 * 12 + 8, 0); }, "an earlier index renamed (its offset changed): a problem");
    broken([&] { g.W8(grown.h.strings + before.entries[1].offset, 'X'); }, "an earlier index's string overwritten: a problem");
    broken([&] { g.W32(grown.h.nodes + 5 * 12 + 8, 0x7FFFFFF); }, "an offset outside the strings: a problem");
    broken([&] { blocks.Moved(grown.h.strings, grown.h.strings, 0x40, heapown::kProcess); }, "a buffer smaller than allocated");
    broken([&] {
        heapown::Block b2 = *blocks.At(grown.h.strings);
        blocks.Freed(grown.h.strings);
        blocks.Allocated(grown.h.strings, b2.size, 1);
    }, "the strings' buffer a player's block (its leaving would free it): a problem");
    broken([&] { blocks.Freed(grown.h.nodes); }, "the node array no known block: a problem");
    broken([&] { g.W16(P + internpool::kRoot, 0x7000); }, "the root no node: a problem");
}

static void ReallocMoves() {
    heapown::Blocks b;
    b.Allocated(0x1000, 0x40, heapown::kProcess, 0x1234, heapown::Kind::Owned);
    uint64_t serial = b.At(0x1000)->serial;
    Check(b.Moved(0x1000, 0x5000, 0x80, 1) && !b.At(0x1000) && b.At(0x5000), "a move: the old address gone, the new one known");
    Check(b.At(0x5000)->owner == heapown::kProcess && b.At(0x5000)->site == 0x1234 && b.At(0x5000)->size == 0x80 &&
              b.At(0x5000)->serial != serial,
          "the object keeps its owner and site (not the mover's), its new size, a new serial");
    Check(b.Moved(0x5000, 0, 0x100, 1) && b.At(0x5000) && b.At(0x5000)->size == 0x80, "a failed realloc changes nothing");
    Check(b.Moved(0x5000, 0x5000, 0x100, 1) && b.At(0x5000)->size == 0x100, "in place: resized");
    Check(!b.Moved(0x9000, 0xA000, 0x20, 2) && b.At(0xA000) && b.At(0xA000)->owner == 2,
          "an unknown old block: the new one allocated as malloc's would be (the mover's)");
    Check(b.Holding(0x5000 + 0xFF) == 0x5000 && b.Holding(0x5000 + 0x100) == 0, "the moved block's extent");
}

static void Rollback() {
    // the shared data (.data: 0x400000..0x400100) and the heap: the process's pool buffer, player 1's object, and
    // player 2's (slot 1) refused initialisation's blocks
    GuestMem g;
    heapown::Blocks b;
    constexpr uint32_t kData = 0x400000, kDataEnd = 0x400100;
    constexpr uint32_t POOL = 0x1000000, A1 = 0x1010000, B1 = 0x1020000, B2 = 0x1030000, B3 = 0x1040000,
                       B4 = 0x1050000, B5 = 0x1060000, B6 = 0x1070000, B7 = 0x1080000, B8 = 0x1090000;
    b.Allocated(POOL, 0x100, heapown::kProcess);
    b.Allocated(A1, 0x40, 0);
    for (uint32_t va : {B1, B2, B3, B5, B6, B7, B8}) b.Allocated(va, 0x40, 1);
    b.Allocated(B4, 0x40, 1, 0x2C716, heapown::Kind::SharedRegistry);  // a user message entry it registered
    g.W32(kData + 0x10, POOL);       // the pool names its buffer (shared)
    g.W32(kData + 0x20, A1);         // player 1's state names its object
    g.W32(kData + 0x30, B1);         // a registration of player 2's object in the shared data
    g.W32(A1 + 0x8, B2);             // player 1's object names one of player 2's
    g.W32(B1 + 0x4, B3);             // a kept block of its own names another
    g.W32(kData + 0x40, B7 + 0x18);  // an interior pointer
    g.W32(B5 + 0x0, B6);             // B5 -> B6, nobody names B5
    g.W32(POOL + 0x20, 0);
    g.W32(B8 + 0x0, B5);             // B8 -> B5 -> B6: an unnamed chain
    auto shared = [&](auto f) {
        for (uint32_t va = kData; va < kDataEnd; va += 4) f(va, g.R32(va));
    };
    auto rd = [&](uint32_t va) { return g.R32(va); };
    heapown::RollbackPlan plan = heapown::PlanRollback(b, 1, shared, rd);
    auto has = [](const std::vector<heapown::RollbackItem>& v, uint32_t va) {
        return std::any_of(v.begin(), v.end(), [&](const heapown::RollbackItem& r) { return r.va == va; });
    };
    auto reason = [&](uint32_t va) -> std::string {
        for (const heapown::RollbackItem& r : plan.keep)
            if (r.va == va) return r.reason;
        return "";
    };
    Check(plan.release.size() == 3 && has(plan.release, B5) && has(plan.release, B6) && has(plan.release, B8),
          "releasable: only the failed player's blocks nothing that stays names");
    Check(plan.keep.size() == 5 && reason(B1) == "named by the shared data" && reason(B7) == "named by the shared data",
          "kept: named by the shared data (an interior pointer too)");
    Check(reason(B2) == "named by another owner's block", "kept: named by player 1's object");
    Check(reason(B3) == "named by a kept block of its own", "kept: named by a kept block");
    Check(reason(B4).find("shared kind") != std::string::npos, "kept: a shared registry entry, whoever allocated it");
    Check(!has(plan.release, POOL) && !has(plan.keep, POOL) && !has(plan.release, A1) && !has(plan.keep, A1),
          "the process's and player 1's blocks are never in the plan");
    Check(plan.ReleaseBytes() == 3 * 0x40, "the bytes releasable");
    // the release (as native_split.cpp's: a block freed only while it is still the failed player's): each once
    int frees = 0;
    auto release = [&](const heapown::RollbackPlan& p) {
        for (const heapown::RollbackItem& r : p.release) {
            const heapown::Block* bl = b.At(r.va);
            if (!bl || bl->owner != 1) continue;
            b.Freed(r.va);
            frees++;
        }
    };
    release(plan);
    release(plan);  // the same plan again: nothing freed twice
    Check(frees == 3 && !b.At(B5) && !b.At(B6) && !b.At(B8), "released once each, never twice");
    Check(b.At(POOL) && b.At(A1) && b.At(B1) && b.At(B2) && b.At(B3) && b.At(B4) && b.At(B7),
          "the shared pool, player 1's object and every kept block survive");
    heapown::RollbackPlan again = heapown::PlanRollback(b, 1, shared, rd);
    Check(again.release.empty() && again.keep.size() == 5, "a second plan: nothing more to release");
    // a refused initialisation that grew the shared pool: its moved buffer stays the process's, so it is not the
    // failed player's to release, and survives
    b.Moved(POOL, 0x10A0000, 0x200, 1);
    g.W32(kData + 0x10, 0x10A0000);
    heapown::RollbackPlan after_growth = heapown::PlanRollback(b, 1, shared, rd);
    Check(!has(after_growth.release, 0x10A0000) && !has(after_growth.keep, 0x10A0000) &&
              b.At(0x10A0000)->owner == heapown::kProcess,
          "the pool grown by the failed player: still the process's, never released");
    // then the player's leftovers are handed to the process (Orphan): no later free is taken for that player's
    auto [n, bytes] = b.Orphan(1);
    Check(n == 5 && bytes == 5 * 0x40, "the kept blocks handed to the process (5, 320 bytes)");
}

int main() {
    SwitchCost();
    Loopback();
    Exchange();
    Heap();
    TwoPlayers();
    StartupValues();
    ProtectedState();
    ConnectionBuffers();
    Lifecycle();
    Contexts();
    SharedOnce();
    SessionLifecycle();
    ProcessTimer();
    TempEntPool();
    InternPool();
    ReallocMoves();
    Rollback();
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
