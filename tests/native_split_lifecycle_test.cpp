// tests/native_split_lifecycle_test.py builds and runs this: native split-screen's M2 lifecycle without the game
// (docs/native_splitscreen.md 15.12): src/game/native_split_lifecycle.h (the console's routing, the front end's commands,
// the voice channels, a leaving player's cleanup, the leak bound) and native_split_core.h's session rules (a shutdown
// asked in a turn, player 1's departure, the clients watched), with heap_owner.h's reference plan for the cleanup.
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "game/heap_owner.h"
#include "game/native_split_core.h"
#include "game/native_split_lifecycle.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

using namespace nsplit;

// ---- the routing rules ------------------------------------------------------------------------------------------

static void Routing() {
    // a turn's line is that player's, for every player (player 1 too), kept by the hook (its queue)
    for (int k = 0; k < kMaxPlayers; k++) {
        Route r = RouteText(-1, false, false, true, k);
        Check(r.origin == Origin::Player && r.player == k && !r.native, "a line added in player k's turn: its queue");
    }
    Check(RouteText(-1, false, false, false, -1).origin == Origin::Process && RouteText(-1, false, false, false, -1).native,
          "outside the turns: the process's, the shared buffer");
    Check(RouteText(-1, false, true, false, -1).origin == Origin::Server && RouteText(-1, false, true, false, -1).native,
          "the server's frame: the shared buffer");
    Check(RouteText(-1, true, false, true, 1).origin == Origin::FrontEnd && RouteText(-1, true, false, true, 1).native,
          "the shared front end's scope in a player's turn: the shared buffer, not the player's");
    Check(RouteText(2, false, false, true, 2).origin == Origin::Player && RouteText(2, false, false, true, 2).native &&
              RouteText(2, false, false, true, 2).player == 2,
          "while a player's lines run: its lines' lines run with them (the title's buffer)");
    Check(RouteText(2, true, false, true, 2).origin == Origin::FrontEnd, "the front end's scope first, even while a run");
    Check(RouteText(-1, false, false, true, -1).origin == Origin::Process, "a turn of no known player: the process's");
    // Cbuf_Execute in a turn runs the player's queue; outside, and nested in a run, the title's
    Check(ExecuteRunsQueue(-1, true, 1) && ExecuteRunsQueue(-1, true, 0), "an execute in a turn runs its queue");
    Check(!ExecuteRunsQueue(-1, false, -1) && !ExecuteRunsQueue(1, true, 1), "outside the turns or nested: the title's");
    // the front end's command
    Check(FrontEndCommand("open TeamSelect_Root", true) == FrontEnd::Run, "the driver's xbui line runs");
    Check(FrontEndCommand("open TeamSelect_Root", false) == FrontEnd::Withheld, "another's xbui line is withheld");
    Check(FrontEndCommand("!slot1; wait; xbui open TeamSelect_T", false) == FrontEnd::Run,
          "another's ! line is its own console line (its queue)");
    Check(FrontEndCommand(nullptr, false) == FrontEnd::Withheld, "no text: withheld");
}

static void Queues() {
    CommandQueues q;
    Check(q.Add(1, "slot1\n") && q.Add(1, "impulse 101\n") && q.Insert(1, "exec a.cfg\n"), "lines added and inserted");
    Check(q.Add(2, "slot2\n") && q.Size(2) == 6, "another player's lines apart");
    std::string s = q.Take(1);
    Check(s == "exec a.cfg\nslot1\nimpulse 101\n" && q.Size(1) == 0, "taken in order, the insert first; empty after");
    q.Add(1, "later\n");
    q.Return(1, "wait\nrest\n");
    Check(q.Take(1) == "wait\nrest\nlater\n", "what a wait left goes back before what came meanwhile");
    Check(q.Drop(2) == 6 && q.Size(2) == 0, "a leaving player's lines dropped");
    std::string big(kQueueMax - 1, 'x');
    Check(!q.Add(3, big + "y") && q.Overflows(3) == 1 && q.Size(3) == 0, "a line that would fill it refused");
    Check(q.Add(3, std::string(kQueueMax - 2, 'x')) && !q.Add(3, "yy") && q.Overflows(3) == 2, "the bound, as Cbuf's");
    Check(!q.Add(-1, "x") && !q.Add(kMaxPlayers, "x") && q.Take(9).empty(), "no such player: refused");
}

// a frame of _Host_Frame as native_split.cpp drives it: the shared buffer run once before the turns (player 1's
// context, outside the turns), each player's queue at its own input turn; each line run in a context
struct FrameSim {
    CommandQueues q;
    std::string shared;
    std::vector<std::pair<int, std::string>> ran;  // (context, line)
    void Add(bool in_turn, int turn_player, bool server, const std::string& line) {
        Route r = RouteText(-1, false, server, in_turn, turn_player);
        if (r.native)
            shared += line;
        else
            q.Add(r.player, line);
    }
    void RunLines(int context, std::string lines) {
        size_t at = 0;
        while (at < lines.size()) {
            size_t e = lines.find('\n', at);
            ran.push_back({context, lines.substr(at, e - at)});
            at = e + 1;
        }
    }
    void Frame(int players) {
        RunLines(0, shared);  // 0x5D3AB, context 0, outside the turns
        shared.clear();
        for (int c = 0; c < players; c++) RunLines(c, q.Take(c));  // each input turn (context = slot here)
    }
};

static void OnceAndPerPlayer() {
    for (int n : {1, 2, 4}) {
        FrameSim f;
        f.Add(false, -1, false, "map de_dust2\n");        // the console: the process's
        f.Add(false, -1, true, "changelevel2 x\n");       // the server's
        for (int k = 0; k < n; k++) f.Add(true, k, false, "cmd p" + std::to_string(k) + "\n");  // stufftext, each
        f.Frame(n);
        int process = 0, server = 0;
        bool own = true;
        for (auto& [c, l] : f.ran) {
            process += l == "map de_dust2";
            server += l == "changelevel2 x";
            if (l.rfind("cmd p", 0) == 0) own = own && c == l[5] - '0';
        }
        Check(process == 1 && server == 1, "the process's and the server's lines run once a frame, whatever the players");
        Check(own, "each player's line runs in its own context only");
        Check(f.ran.size() == size_t(2 + n), "nothing run twice");
        f.ran.clear();
        f.Frame(n);
        Check(f.ran.empty(), "nothing again the next frame");
    }
    // the old routing for contrast: every line in player 1's context
    FrameSim old;
    old.shared = "cmd p1\n";
    old.Frame(2);
    Check(old.ran.size() == 1 && old.ran[0].first == 0, "before: player 2's stuffed line ran as player 1's");
}

// ---- the session -------------------------------------------------------------------------------------------------

static Players With(int n) {
    Players p;
    for (int k = 1; k < n; k++) {
        p.Begin(k, k, "Player");
        p.Initialised(k);
        p.Joined(k);
    }
    return p;
}

static void Shutdowns() {
    Players two = With(2), one = With(1), four = With(4);
    Check(ShutdownDecision(false, -1, two) == Shutdown::Proceed, "outside the turns: the process's shutdown goes on");
    Check(ShutdownDecision(true, 1, two) == Shutdown::Refuse, "player 2's turn while player 1 plays: refused");
    Check(ShutdownDecision(true, 0, two) == Shutdown::Refuse, "player 1's turn while player 2 plays: refused");
    Check(ShutdownDecision(true, 0, one) == Shutdown::Proceed, "player 1 alone: the title's own");
    Check(ShutdownDecision(true, 3, four) == Shutdown::Refuse && ShutdownDecision(true, -1, four) == Shutdown::Refuse,
          "player 4 of four refused; an unknown turn refused (fail closed)");
    Players p2only = With(2);
    p2only.Depart0();
    Check(ShutdownDecision(true, 1, p2only) == Shutdown::Defer, "player 2 the last playing: deferred (from player 1's context)");
    Check(LeaveKeepsShared(true, 1, two) && LeaveKeepsShared(true, 0, two) && !LeaveKeepsShared(true, 0, one) &&
              !LeaveKeepsShared(false, 0, two) && !LeaveKeepsShared(true, 1, p2only),
          "a client's own disconnect keeps the shared parts only while another plays");
    Check(OthersRemain(four, 2) && !OthersRemain(one, 0), "others remain");
}

static void Departure() {
    Players p = With(3);
    SharedUiOwner ui;
    Check(SessionHost(p) == 0, "player 1 the host while it plays");
    Check(p.Depart0() && p.At(0).state == State::Departed && !p.Depart0(), "player 1 departs once");
    Check(ui.Left(0, p) == 1 && SessionHost(p) == 1, "the front end and the host go to player 2, the lowest playing");
    Check(p.At(1).context == 1 && p.At(2).context == 2 && p.At(0).context == 0, "nobody renumbered, player 1's context kept");
    Check(p.Playing() == 2 && p.Count() == 3 && p.FreeSlot() == 3, "two play; slot 0 still held (no new player in it)");
    Check(ui.Decide(true, 1) == SharedUi::AsShared && ui.Decide(true, 0) == SharedUi::Withheld,
          "the new driver runs the front end; the departed player 1's context does not");
    Check(p.Leave(1) && ui.Left(1, p) == 2 && (p.Released(1), SessionHost(p) == 2), "then player 2 leaves: player 3 drives");
    Check(p.Return0() && p.At(0).state == State::Active && !p.Return0(), "the session's end: player 1 the title's again");
    Players q;
    Check(!q.Leave(0) && !q.Return0(), "player 1 never leaves through Leave (it departs); returns only when departed");
}

static void Watch() {
    ClientWatch w;
    Check(!w.Dropped(1) && !w.Dropped(1), "a new player waiting, disconnected: not dropped");
    Check(!w.Dropped(2) && !w.Dropped(3) && !w.Dropped(4) && !w.Dropped(5), "connects and becomes active");
    Check(w.Dropped(1) && !w.Dropped(1), "kicked (cls.state back to 1): dropped once");
    ClientWatch f;
    Check(!f.Dropped(2) && f.Dropped(1), "a connect that failed: dropped");
    ClientWatch r;
    r.Dropped(5);
    r.Dropped(1);
    Check(!r.Dropped(1) && !r.Dropped(2) && r.Dropped(0), "after a drop it watches anew (a rejoin)");
}

// ---- a session simulated: the server, the world, the players' joins and leaves -----------------------------------

struct SessionSim {
    Players p;
    SharedUiOwner ui;
    ClientWatch watch[kMaxPlayers];
    uint32_t cls[kMaxPlayers] = {5, 1, 1, 1};
    bool server = true, end = false;
    int world = 1, shutdowns = 0, world_frees = 0;
    LeakBudget leaks;
    bool add_failed = false;
    // a player's client leaves in its own turn (its CL_Disconnect: its shutdown decided)
    void LeaveInTurn(int k) {
        Shutdown d = ShutdownDecision(true, k, p);
        if (d == Shutdown::Proceed) Shutdown_();
        if (d == Shutdown::Defer) end = true;
        cls[k] = 1;
    }
    void Shutdown_() {
        if (!server) return;
        shutdowns++;
        world_frees++;
        server = false;
        int n = 0;
        auto order = TeardownOrder(p, 0, n);
        for (int i = 0; i < n; i++) Remove(order[size_t(i)], 0);
        p.Return0();
        ui.driver = 0;
    }
    void Remove(int k, size_t left) {
        if (!p.Leave(k)) return;
        ui.Left(k, p);
        p.ContextRemoved(p.At(k).context);
        p.Released(k);
        watch[k] = {};
        leaks.Left(left);
        if (!leaks.MayJoin()) add_failed = true;
        if (p.At(0).state == State::Departed && p.Playing() == 0) end = true;
    }
    bool Join(int k, bool init_ok = true) {
        // players are added from player 1's match (native_split.cpp: its cls.state active in context 0)
        if (add_failed || !leaks.MayJoin() || p.FreeSlot() != k || p.At(0).state != State::Active) return false;
        p.Begin(k, p.Count(), "P");
        if (!init_ok) {
            p.InitFailed(k);
            add_failed = true;
            return false;
        }
        p.Initialised(k);
        cls[k] = 5;
        watch[k].Dropped(2);
        p.Joined(k);
        return true;
    }
    void FrameEnd(size_t left_each = 0) {
        for (int k = 0; k < kMaxPlayers; k++) {
            State s = p.At(k).state;
            if (s != State::Connecting && s != State::Active) continue;
            if (!watch[k].Dropped(cls[k])) continue;
            if (k == 0) {
                if (OthersRemain(p, 0)) {
                    p.Depart0();
                    ui.Left(0, p);
                }
                continue;
            }
            Remove(k, left_each);
        }
        if (end) {
            end = false;
            for (int k = kMaxPlayers - 1; k > 0; k--) Remove(k, 0);
            Shutdown_();
        }
    }
};

static void Sessions() {
    {  // player 2 leaves while player 1 plays
        SessionSim s;
        s.watch[0].Dropped(5);
        s.Join(1);
        s.LeaveInTurn(1);
        s.FrameEnd();
        Check(s.server && s.world_frees == 0 && s.p.At(1).state == State::Empty && s.p.At(0).state == State::Active,
              "player 2 leaves: the server and world stay, player 1 plays on");
    }
    {  // player 1 leaves while player 2 plays, then player 2: the session ends once, as the process
        SessionSim s;
        s.watch[0].Dropped(5);
        s.Join(1);
        s.LeaveInTurn(0);
        s.FrameEnd();
        Check(s.server && s.world_frees == 0 && s.p.At(0).state == State::Departed && s.ui.driver == 1 &&
                  s.p.At(1).state == State::Active && s.p.At(1).context == 1,
              "player 1 leaves: the server and world stay, player 2 drives, nobody renumbered");
        s.LeaveInTurn(1);
        Check(s.server && s.end, "player 2 the last: its shutdown deferred, not in its context");
        s.FrameEnd();
        Check(!s.server && s.shutdowns == 1 && s.p.At(0).state == State::Active && s.p.Count() == 1 && s.ui.driver == 0,
              "the session ends once at the frame's end; player 1 the title's own again");
    }
    {  // four players: 2 and 4 leave, 3 stays with 1; player 1 leaves; 3 drives
        SessionSim s;
        s.watch[0].Dropped(5);
        Check(s.Join(1) && s.Join(2) && s.Join(3), "four players");
        s.LeaveInTurn(1);
        s.LeaveInTurn(3);
        s.FrameEnd();
        Check(s.server && s.p.Playing() == 2 && s.p.At(2).context == 1, "two leave; player 3's context moves down, its slot kept");
        s.LeaveInTurn(0);
        s.FrameEnd();
        Check(s.server && s.ui.driver == 2 && s.p.At(0).state == State::Departed, "player 1 departs: player 3 drives");
        Check(!s.Join(1), "no player joins while player 1 is away (it adds players from its match)");
    }
    {  // repeated joins and leaves: bounded by what they leave behind
        SessionSim s;
        s.watch[0].Dropped(5);
        s.leaks.limit = 10000;
        int joins = 0;
        for (int i = 0; i < 100; i++) {
            if (!s.Join(1)) break;
            joins++;
            s.LeaveInTurn(1);
            s.FrameEnd(3000);
        }
        Check(joins == 4 && s.add_failed && s.leaks.left_behind == 12000, "joins stop once the residue passes the bound");
        Check(s.server && s.p.At(0).state == State::Active, "the session itself goes on");
        SessionSim clean;
        clean.watch[0].Dropped(5);
        int n = 0;
        for (int i = 0; i < 50 && clean.Join(1); i++, n++) {
            clean.LeaveInTurn(1);
            clean.FrameEnd(0);
        }
        Check(n == 50 && !clean.add_failed, "with nothing left behind, joins and leaves go on");
    }
    {  // a failed initialisation: the slot empty again, no further player
        SessionSim s;
        Check(!s.Join(1, false) && s.p.At(1).state == State::Empty && s.add_failed && !s.Join(1),
              "a partial initialisation fails: not added, no retry this session");
        Check(s.server && s.world_frees == 0, "the server and world untouched by it");
    }
    {  // a kick of player 2 (svc_disconnect: its own CL_Disconnect in its turn) and the process's shutdown of all
        SessionSim s;
        s.watch[0].Dropped(5);
        s.Join(1);
        s.Join(2);
        s.LeaveInTurn(2);  // kicked
        s.FrameEnd();
        Check(s.server && s.p.Playing() == 2, "a kicked player 3 removed, the server kept");
        s.Shutdown_();  // the host's menu: "disconnect" outside the turns
        Check(!s.server && s.shutdowns == 1 && s.p.Count() == 1 && s.p.At(1).state == State::Empty,
              "the process's shutdown removes the others first, once");
    }
}

// ---- a leaving player's cleanup ------------------------------------------------------------------------------------

static void CleanupRules() {
    auto order = CleanupOrder();
    auto at = [&](Cleanup c) {
        for (int i = 0; i < kCleanupSteps; i++)
            if (order[size_t(i)] == c) return i;
        return -1;
    };
    Check(at(Cleanup::ServerSlot) < at(Cleanup::Entities) && at(Cleanup::Channels) < at(Cleanup::Entities),
          "nothing reaches it before its objects go");
    Check(at(Cleanup::SharedUi) < at(Cleanup::Plan) && at(Cleanup::Plan) < at(Cleanup::Entities),
          "the front end handed over and the plan made before any release");
    Check(at(Cleanup::ClientObjects) < at(Cleanup::HudList) && at(Cleanup::Residual) < at(Cleanup::Context) &&
              at(Cleanup::Context) < at(Cleanup::Slot),
          "the objects before the list they hang on; the context and slot last");
    Check(ReleaseCheck(true, 1, true, 1, false) == Release::Release, "its own, unreferenced: released");
    Check(ReleaseCheck(true, 0, true, 1, false) == Release::NotOwn && ReleaseCheck(true, -1, true, 1, false) == Release::NotOwn,
          "player 1's or the process's: kept");
    Check(ReleaseCheck(true, 1, false, 1, false) == Release::SharedKind, "a shared kind: kept");
    Check(ReleaseCheck(true, 1, true, 1, true) == Release::Referenced, "named by another: kept");
    Check(ReleaseCheck(false, -2, false, 1, false) == Release::None, "no block: nothing");

    // the plan over a heap: player 2's objects, one named by the shared data, one by player 1's block, one by a kept one
    heapown::Blocks b;
    std::map<uint32_t, uint32_t> mem;  // va -> word
    b.Allocated(0x100000, 16, 1);      // its entities, named by nobody else
    b.Allocated(0x200000, 16, 1);      // named by the shared data
    b.Allocated(0x300000, 16, 1);      // named by the kept one (+0)
    b.Allocated(0x400000, 16, 0);      // player 1's, naming 0x500000
    b.Allocated(0x500000, 16, 1);
    b.Allocated(0x600000, 16, 1, 0, heapown::Kind::SharedRegistry);
    mem[0x200000] = 0x300000;
    mem[0x400004] = 0x500008;
    std::map<uint32_t, uint32_t> shared = {{0x36D000, 0x200004}};
    auto plan = heapown::PlanRollback(
        b, 1, [&](auto f) { for (auto& [a, v] : shared) f(a, v); },
        [&](uint32_t va) { auto it = mem.find(va); return it == mem.end() ? 0u : it->second; });
    std::set<uint32_t> keep, release;
    for (auto& r : plan.keep) keep.insert(r.va);
    for (auto& r : plan.release) release.insert(r.va);
    Check(release == std::set<uint32_t>{0x100000}, "only the unreferenced block of its own is released");
    Check(keep.count(0x200000) && keep.count(0x300000) && keep.count(0x500000) && keep.count(0x600000) && !keep.count(0x400000),
          "named by the shared data, through a kept block, by player 1's block, a shared kind: kept; player 1's not its");
    // the frees its destructors make: the keep set refused, each block freed at most once
    std::set<uint32_t> freed;
    int refused = 0;
    for (uint32_t va : {0x100000u, 0x200000u, 0x100000u, 0x500000u, 0x400000u}) {
        const heapown::Block* blk = b.At(va);
        if (!blk) continue;  // gone: never freed twice
        if (keep.count(va) || heapown::SkipFree(blk->owner, 1)) {
            refused++;
            continue;
        }
        freed.insert(va);
        b.Freed(va);
    }
    Check(freed == std::set<uint32_t>{0x100000} && refused == 3, "one free done once; the named and player 1's refused");
    auto [n, bytes] = b.Orphan(1);
    Check(n == 4 && bytes == 64, "what it still owns left behind (reported), never freed under another");
    LeakBudget lb;
    lb.Left(bytes);
    Check(lb.MayJoin() && lb.leaves == 1, "a small residue: joins go on");
}

// ---- the voice channels -------------------------------------------------------------------------------------------

static void Voice() {
    Check(VoiceDecide(false, 0) == SharedUi::AsTitle && VoiceDecide(false, 2) == SharedUi::AsTitle,
          "outside the turns (Host_Shutdown's release): as the title");
    Check(VoiceDecide(true, 0) == SharedUi::AsShared, "context 0's turn: the process's scope");
    for (int c = 1; c < kMaxPlayers; c++) Check(VoiceDecide(true, c) == SharedUi::Withheld, "another context: not run");
    // five channels, codecs allocated in the voice scope (the process's): a player leaving frees none
    struct Channel {
        int entity = -1;
        int owner = -2;
    } ch[5];
    auto feed = [&](int context, int speaker) {
        if (VoiceDecide(true, context) != SharedUi::AsShared) return false;
        for (Channel& c : ch)
            if (c.entity == speaker || c.entity < 0) {
                c.entity = speaker;
                c.owner = AllocationOwner(true, false, -1, true, context);
                return true;
            }
        return false;
    };
    int fed = 0;
    for (int frame = 0; frame < 3; frame++)
        for (int c = 0; c < 4; c++) fed += feed(c, 7);
    Check(fed == 3, "the same speaker's data added once a frame (context 0's turn), not once per player");
    Check(ch[0].entity == 7 && ch[0].owner == heapown::kProcess && ch[1].entity == -1, "one channel, the process's");
    Players p = With(2);
    p.Leave(1);
    p.Released(1);
    Check(ch[0].owner == heapown::kProcess, "player 2 leaving owns no channel to release");
}

int main() {
    Routing();
    Queues();
    OnceAndPerPlayer();
    Shutdowns();
    Departure();
    Watch();
    Sessions();
    CleanupRules();
    Voice();
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
