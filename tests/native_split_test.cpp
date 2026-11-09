// tests/native_split_test.py builds and runs this: native split-screen's parts without the game (M2):
// src/game/local_loopback.h, heap_owner.h, native_split_core.h and client_context_core.h's removal
#include <cstdio>
#include <cstring>
#include <map>
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
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
