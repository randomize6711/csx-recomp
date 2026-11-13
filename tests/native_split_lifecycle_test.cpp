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
    Departures dep;
    int host = 0, rejoined = 0;
    // player 1 rejoins between frames (native_split.cpp RejoinPlayer1): its client disconnected, then connecting in
    // its next turn (the title's CL_CheckForResend), active after its signon
    Rejoin RejoinP1(bool stale_slot = false) {
        Rejoin r = MayRejoin0(p, dep, server, stale_slot, cls[0]);
        if (r == Rejoin::Allowed) {
            watch[0] = {};
            p.Rejoin0();
        }
        return r;
    }
    void P1Connects(bool ok = true) {
        cls[0] = 2;
        watch[0].Dropped(2);
        cls[0] = ok ? 5 : 1;
    }
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
                if (Player1Exit(p) == P1Exit::Depart) {
                    p.Depart0();
                    ui.Left(0, p);
                    host = SessionHost(p);
                    watch[0] = {};
                } else {
                    p.Return0();
                    watch[0] = {};
                }
                continue;
            }
            Remove(k, left_each);
        }
        // player 1 rejoining: in the match once its client is active (native_split.cpp NativeSplitFrameEnd)
        if (p.At(0).state == State::Connecting && cls[0] == 5 && p.Joined(0)) rejoined++;
        if (end) {
            end = false;
            for (int k = kMaxPlayers - 1; k > 0; k--) Remove(k, 0);
            Shutdown_();
        }
    }
};

// player 1's departure and rejoin in its kept context (docs 15.19; native_split_core.h MayRejoin0, Rejoin0)
static void Player1Rejoin() {
    {  // the rule's reasons, in order
        Players p = With(2);
        Departures d;
        Check(MayRejoin0(p, d, true, false, 1) == Rejoin::NotDeparted, "rejoin: only a departed player 1");
        Check(p.Depart0(), "player 1 departs");
        Check(MayRejoin0(p, d, false, false, 1) == Rejoin::NoServer, "rejoin: not without the server");
        Check(MayRejoin0(p, d, true, true, 1) == Rejoin::StaleSlot, "rejoin: not while a slot names its port");
        Check(MayRejoin0(p, d, true, false, 2) == Rejoin::NotDisconnected, "rejoin: not while its client connects");
        Check(d.Mark(1, p, Leave::Disconnect) && MayRejoin0(p, d, true, false, 1) == Rejoin::DeparturePending,
              "rejoin: not while another's departure is pending");
        d.Clear(1);
        Check(p.Begin(2, 2, "P3") && MayRejoin0(p, d, true, false, 1) == Rejoin::Initialising,
              "rejoin: not while a player initialises");
        p.InitFailed(2);
        Check(MayRejoin0(p, d, true, false, 1) == Rejoin::Allowed, "rejoin allowed: departed, the server, player 2");
        Check(Held(d, p, 0) && HoldsReconnect(true, true, 0, d, p), "departed: its reconnect held");
        Check(p.Rejoin0() && p.At(0).state == State::Connecting && p.At(0).context == 0 && !p.Rejoin0(),
              "player 1 rejoins once, in its own context 0");
        Check(!Held(d, p, 0) && !HoldsReconnect(true, true, 0, d, p) && !DropsLines(d, p, 0),
              "rejoining: its client's own reconnect runs, its lines run");
        Check(ShutdownDecision(true, 0, p) == Shutdown::Refuse && LeaveKeepsShared(true, 0, p),
              "rejoining while player 2 plays: a shutdown in its turn refused, the shared parts kept");
        Check(p.Playing() == 2 && SessionHost(p) == 0, "rejoining counts as playing");
        Check(p.Joined(0) && p.At(0).state == State::Active && !p.Joined(0), "in the match once");
        Check(!p.Rejoin0() && !p.Return0(), "active: no rejoin, no return");
        Players q = With(2);
        Check(!q.Joined(0), "player 1 active from the start: not joined again");
        Players solo;
        Departures d2;
        Check(MayRejoin0(solo, d2, true, false, 1) == Rejoin::NotDeparted && Player1Exit(solo) == P1Exit::TitleOwn,
              "solo: player 1 never departs (its client leaving is the title's own) and never rejoins");
    }
    {  // departs, rejoins, plays: nobody renumbered, the front end and the host stay with player 2 while it plays
        SessionSim s;
        s.watch[0].Dropped(5);
        Check(s.Join(1), "player 2 joins");
        s.LeaveInTurn(0);
        s.FrameEnd();
        Check(s.p.At(0).state == State::Departed && s.ui.driver == 1 && s.host == 1 && s.server,
              "player 1 departs: player 2 drives and hosts, the server stays");
        Check(!s.Join(2), "no player added while player 1 is away");
        Check(s.RejoinP1() == Rejoin::Allowed && s.p.At(0).state == State::Connecting, "player 1 rejoins");
        Check(!s.Join(2), "no player added while player 1 connects");
        s.P1Connects();
        s.FrameEnd();
        Check(s.p.At(0).state == State::Active && s.rejoined == 1 && s.p.At(1).state == State::Active &&
                  s.p.At(1).context == 1 && s.ui.driver == 1 && s.host == 1 && s.server && s.shutdowns == 0,
              "player 1 in the match again: player 2 untouched, still driving");
        Check(s.Join(2) && s.p.At(2).context == 2, "players added again from player 1's match");
    }
    {  // a rejoin whose connect fails while player 2 plays: departed again; then player 2 leaves: the session ends once
        SessionSim s;
        s.watch[0].Dropped(5);
        s.Join(1);
        s.LeaveInTurn(0);
        s.FrameEnd();
        s.RejoinP1();
        s.P1Connects(false);
        s.FrameEnd();
        Check(s.p.At(0).state == State::Departed && s.rejoined == 0 && s.p.At(1).state == State::Active,
              "a failed rejoin: player 1 departed again, player 2 plays on");
        s.LeaveInTurn(1);
        s.FrameEnd();
        Check(!s.server && s.shutdowns == 1 && s.p.At(0).state == State::Active && s.p.Count() == 1,
              "then player 2 leaves: the session ends once, player 1 the title's own");
    }
    {  // player 2 leaves while player 1 is rejoining: the session goes on with player 1; it drives once player 2 is gone
        SessionSim s;
        s.watch[0].Dropped(5);
        s.Join(1);
        s.LeaveInTurn(0);
        s.FrameEnd();
        s.RejoinP1();
        s.LeaveInTurn(1);
        s.FrameEnd();
        Check(s.server && !s.end && s.p.At(1).state == State::Empty && s.p.At(0).state == State::Connecting &&
                  s.ui.driver == 0,
              "player 2 leaves while player 1 rejoins: the server stays, player 1 drives (still connecting)");
        s.P1Connects();
        s.FrameEnd();
        Check(s.p.At(0).state == State::Active && s.server && s.shutdowns == 0, "player 1 in the match, alone");
        Players q = With(1);
        Check(q.Depart0() == true && q.Rejoin0() && Player1Exit(q) == P1Exit::TitleOwn && q.Return0() &&
                  q.At(0).state == State::Active,
              "a rejoin left alone (nobody else plays): the title's own player again");
    }
    {  // repeated departures and rejoins, with two, three and four players
        for (int n = 2; n <= 4; n++) {
            SessionSim s;
            s.watch[0].Dropped(5);
            for (int k = 1; k < n; k++) s.Join(k);
            bool ok = true;
            for (int cycle = 0; cycle < 10; cycle++) {
                s.LeaveInTurn(0);
                s.FrameEnd();
                ok = ok && s.p.At(0).state == State::Departed && s.ui.driver == 1 && s.server;
                ok = ok && s.RejoinP1() == Rejoin::Allowed;
                s.P1Connects();
                s.FrameEnd();
                ok = ok && s.p.At(0).state == State::Active;
                for (int k = 1; k < n; k++) ok = ok && s.p.At(k).state == State::Active && s.p.At(k).context == k;
            }
            ok = ok && s.rejoined == 10 && s.shutdowns == 0 && s.world_frees == 0 && s.p.Count() == n &&
                 s.leaks.leaves == 0;
            Check(ok, n == 2   ? "two players: ten departures and rejoins of player 1, nothing else changes"
                      : n == 3 ? "three players: ten departures and rejoins of player 1, nothing else changes"
                               : "four players: ten departures and rejoins of player 1, nothing else changes");
        }
    }
}

// docs 15.20: the title's temporary removals (0x108BB0's move to the front) told from permanent detachments
static void LinkCycles() {
    {
        LinkTracker t;
        t.Removed(0x909CC0, 0x909E50, kReorderRemoveReturn);
        Check(t.Attached(0x909CC0, 0x909E50, Link::New, kReorderAddReturn) == Link::Cycle && t.Reorders() == 1 &&
                  t.Cycles() == 1 && t.Pending() == 0,
              "0x108BB0's remove (0x108BC5) and add (0x108BD6) of one child: one cycle, a move to the front");
        t.Removed(0x909CC0, 0x909E50, 0x1234);
        Check(t.Attached(0x909CC0, 0x909E50, Link::New, 0x5678) == Link::Cycle && t.Reorders() == 1 && t.Cycles() == 2,
              "another removal re-added to the same parent in its frame: a cycle (not 0x108BB0's)");
        t.Removed(0x909CC0, 0x0A9C1D0, 0x1234);
        Check(t.Attached(0x0BBB000, 0x0A9C1D0, Link::New, 0x5678) == Link::Change && t.Cycles() == 2,
              "re-added to another parent: a parent change");
        Check(t.Attached(0x909CC0, 0x0CCC000, Link::New, 0x5678) == Link::New, "no pending removal: a new attachment");
        Check(t.Attached(0x909CC0, 0x0CCC000, Link::Reattach, 0x5678) == Link::Reattach, "a no-op assignment stays one");
        t.Removed(0x909CC0, 0x0A9C1D0, 0x1234);
        Check(t.Pending() == 1 && t.Settle() == 1 && t.Permanent() == 1 && t.Pending() == 0,
              "not re-added by the frame's end: one permanent detachment");
        Check(t.Settle() == 0 && t.Permanent() == 1, "settled once");
    }
    {  // run 15's frames: the root's 0x909E50 moved to the front every frame, one viewport detached by the cleanup and
       // one detached by the game in a frame of its own
        LinkTracker t;
        uint64_t links[kLinks] = {};
        for (int frame = 0; frame < 300; frame++) {
            t.Removed(0x909CC0, 0x909E50, kReorderRemoveReturn);
            links[int(t.Attached(0x909CC0, 0x909E50, ClassifyLink(0, 0x909CC0, 0), kReorderAddReturn))]++;
            if (frame == 150) t.Removed(0x909CC0, 0x2154000, 0x1AB000);
            links[int(Link::Detach)] += t.Settle();
        }
        Check(links[int(Link::Cycle)] == 300 && links[int(Link::New)] == 0 && links[int(Link::Detach)] == 1 &&
                  t.Reorders() == 300,
              "run 15's pattern: 300 cycles (not 300 attachments and detachments), one permanent detachment");
    }
    Check(std::string(Name(Link::Cycle)).find("temporary") != std::string::npos && kLinks == 7, "the cycle's name, 7 kinds");
}

// docs 15.20: what leaving players still hold, kept by the heap's map as blocks are freed, moved or reused, and the
// bound on it
static void LiveRetention() {
    heapown::Blocks b;
    b.Allocated(0x1000, 100, 1, 0x1F9BBE);
    b.Allocated(0x2000, 200, 1, 0x1CFD0);
    b.Allocated(0x3000, 300, 1, 0x1CFD0);
    b.Allocated(0x4000, 400, 0, 0x1CFD0);  // player 1's: never left behind
    Check(b.LeftBehindBytes() == 0 && b.LeftBehindCount() == 0, "nothing left behind before a leave");
    auto [n, bytes] = b.Orphan(1);
    Check(n == 3 && bytes == 600 && b.LeftBehindBytes() == 600 && b.LeftBehindCount() == 3, "a leave: 600 bytes held");
    b.Orphan(1);
    Check(b.LeftBehindBytes() == 600, "orphaned again: counted once");
    b.Freed(0x2000);
    Check(b.LeftBehindBytes() == 400 && b.LeftBehindCount() == 2, "the game frees one: 400 held");
    b.Freed(0x2000);
    Check(b.LeftBehindBytes() == 400, "a second free of it: nothing more");
    b.Moved(0x3000, 0x5000, 500, -1);
    Check(b.LeftBehindBytes() == 600 && b.At(0x5000) && b.At(0x5000)->left_by == 1, "moved and grown: held at its size");
    b.Moved(0x5000, 0x5000, 50, -1);
    Check(b.LeftBehindBytes() == 150, "shrunk in place: held at its size");
    b.Allocated(0x5000, 70, -1, 0x22);  // the heap gave its address out again: it was freed unseen
    Check(b.LeftBehindBytes() == 100 && b.LeftBehindCount() == 1, "its address reused: it was freed, no longer held");
    b.Moved(0x6000, 0x1000, 10, -1);  // an unknown block moved onto a held one's address: that one was freed unseen
    Check(b.LeftBehindBytes() == 0, "a move onto a held block's address: freed, no longer held");
    b.Allocated(0x7000, 80, 2);
    b.Orphan(2);
    b.Moved(0x7000, 0, 90, 2);
    Check(b.LeftBehindBytes() == 80, "a failed realloc: the old block still held");

    LeakBudget lb;
    lb.Left(929355, 929355);
    lb.Observe(929355 - 17703);
    lb.Left(929355, 2 * 929355 - 17703);
    Check(lb.left_behind == 1858710 && lb.outstanding == 1841007 && lb.ReleasedSince() == 17703 && lb.Consistent(),
          "run 15's leaves: 1,858,710 left in all, 17,703 freed by the game since, 1,841,007 held");
    Check(lb.MayJoin() && lb.Projected() == long((lb.limit - lb.outstanding) / (lb.outstanding / 2)),
          "the projection from what is held");
    LeakBudget over;
    over.Left(9u << 20, 9u << 20);
    Check(!over.MayJoin() && over.Projected() == 0, "held over the bound: no join");
    over.Observe(1u << 20);
    Check(over.MayJoin(), "the game freed most of it: joins open (the session's latch is the caller's)");
    LeakBudget bad;
    bad.Left(100, 500);
    Check(!bad.Consistent() && bad.ReleasedSince() == 0 && bad.outstanding == 500,
          "the heap's count above the history: inconsistent, the gate on the larger");
    LeakBudget ab;
    ab.Abandoned(300, 300);
    Check(ab.leaves == 0 && ab.left_behind == 300 && ab.outstanding == 300 && ab.Consistent(),
          "a failed initialisation's leftovers: in both, not a leave");
    LeakBudget sim;
    sim.Left(1000);
    sim.Left(2000);
    Check(sim.outstanding == 3000 && sim.left_behind == 3000, "without a heap: nothing ever freed again");
    Check(LeakBudget{}.limit == size_t(8) << 20, "the bound stays 8 MiB");
}

// docs 15.21: the particle pool's dry run (report only) and the sentence groups' bytes
static void PoolDryRunRules() {
    Check(!AddWaitsForSlot(true, false) && AddWaitsForSlot(true, true) && !AddWaitsForSlot(false, true),
          "docs 15.22: an add waits while a server slot names its port (only while the server runs)");
    Check(PoolVerdict(false, 0, 0) == PoolReclaim::NotFound, "no pool: not found");
    Check(PoolVerdict(true, 0, 0) == PoolReclaim::Unreferenced, "a pool nothing names");
    Check(PoolVerdict(true, 3, 0) == PoolReclaim::OnlyByteData, "a pool named only by bytes");
    Check(PoolVerdict(true, 3, 1) == PoolReclaim::Referenced && PoolVerdict(true, 0, 1) == PoolReclaim::Referenced,
          "one word not known to be bytes: referenced");
    // run 15's three pool roots, each a byte area now
    for (uint32_t at : {0x540830u, 0x5D1DC8u, 0x62DCA4u})
        Check(EvidenceOf(RootKind::StaticWord, at, 0) == Evidence::Unverified, "run 15's pool roots: unverified bytes");
    Check((0x540830u - 0x53FD20u) / 0x34u == 54 && (0x540830u - 0x53FD20u) % 0x34u == 0x18,
          "0x540830: sentence group 54, +0x18 (an index byte)");
    Check(EvidenceOf(RootKind::StaticWord, 0x5425C0, 0) == Evidence::Unclassified &&
              EvidenceOf(RootKind::StaticWord, 0x53FD1C, 0) == Evidence::Unverified &&
              EvidenceOf(RootKind::StaticWord, 0x539D1C, 0) == Evidence::Unclassified,
          "the areas' ends: the groups end at 0x5425C0, the text starts at 0x539D20");
    Check(kPoolSite == 0x1F9BBE && PersistentSiteOf(kPoolSite) != nullptr, "the pool's site, persistent by the title");
    Check(!ReclaimsPool(false, PoolReclaim::OnlyByteData) && !ReclaimsPool(false, PoolReclaim::Unreferenced),
          "reclamation off: never freed, whatever the dry run says");
    Check(ReclaimsPool(true, PoolReclaim::OnlyByteData) && ReclaimsPool(true, PoolReclaim::Unreferenced) &&
              !ReclaimsPool(true, PoolReclaim::Referenced) && !ReclaimsPool(true, PoolReclaim::NotFound),
          "reclamation on (experimental): only a pool named by nothing but bytes");
}

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

// ---- the same-frame reconnect (docs 15.15): the frame's order simulated -----------------------------------------
// SIMULATED, no game: the title's frame as the eleventh run showed it - each player's input turn (its queued lines run
// at 0x5D3B5: a "disconnect" is its CL_Disconnect, cls.state 1), the server's frame (the dropped client's slot
// freed), each player's client turn (CL_CheckForResend at 0x5D487: state 1 with the server active connects again to
// "localhost", state 2 resends), then the frame's end (native_split.cpp NativeSplitFrameEnd). The rules are
// native_split_core.h's; whether the game takes these paths is the runtime test's to show (native_split_2p_leave).

struct FrameOrderSim {
    bool fixed = true;  // false: the rules before the correction (no departure recorded, nothing held)
    bool split = true;  // native split on (false: the title alone)
    Players p;
    SharedUiOwner ui;
    ClientWatch watch[kMaxPlayers];
    Departures dep;
    CommandQueues q;
    uint32_t cls[kMaxPlayers] = {5, 1, 1, 1};
    int userid[kMaxPlayers] = {1, 0, 0, 0};
    int contexts_made = 0, next_userid = 2, reconnects = 0, resends = 0, held = 0, removals[kMaxPlayers] = {};
    int departures = 0, shutdowns = 0, sessions_ended = 0;
    bool server = true, end = false, kick_next[kMaxPlayers] = {};
    std::vector<std::pair<int, std::string>> ran;  // the lines run, by player
    size_t dropped = 0;

    // a player added between frames (native_split.cpp AddPlayer): a fresh context, its client at state 1
    bool Add(int k) {
        if (p.FreeSlot() != k || p.At(0).state != State::Active) return false;
        if (!p.Begin(k, p.Count(), "P")) return false;  // client_context's Add: the next context (they stay compact)
        contexts_made++;
        dep.Clear(k);
        cls[k] = 1;
        p.Initialised(k);
        return true;
    }
    // its own CL_Disconnect in its turn (the disconnect command, or svc_disconnect: kicked)
    void Disconnect(int k, Leave why) {
        Shutdown d = split ? ShutdownDecision(true, k, p) : Shutdown::Proceed;
        cls[k] = 1;
        if (d == Shutdown::Proceed) {
            Shutdown_();
            return;
        }
        if (d == Shutdown::Defer) end = true;
        if (fixed && dep.Mark(k, p, why)) dropped += q.Drop(k);
    }
    void Shutdown_() {
        if (!server) return;
        server = false;
        shutdowns++;
        int n = 0;
        auto order = TeardownOrder(p, 0, n);
        for (int i = 0; i < n; i++) Remove(order[size_t(i)]);
        p.Return0();
        dep.Clear(0);
    }
    void InputTurn(int k) {
        if (fixed && DropsLines(dep, p, k)) {
            dropped += q.Drop(k);
            return;
        }
        std::string lines = q.Take(k);
        size_t at = 0;
        while (at < lines.size()) {
            size_t nl = lines.find('\n', at);
            std::string line = lines.substr(at, nl - at);
            at = nl == std::string::npos ? lines.size() : nl + 1;
            if (fixed && DropsLines(dep, p, k)) {  // the run ends at the departure (its size set to 0)
                dropped += line.size() + 1 + (lines.size() - at);
                break;
            }
            ran.push_back({k, line});
            if (line == "disconnect") Disconnect(k, Leave::Disconnect);
            if (line == "connect localhost") {
                Disconnect(k, Leave::Disconnect);
                cls[k] = 2;
            }
        }
        if (kick_next[k]) {  // svc_disconnect read in its turn
            kick_next[k] = false;
            Disconnect(k, Leave::Kicked);
        }
    }
    void ClientTurn(int k) {
        if (fixed && HoldsReconnect(split, true, k, dep, p)) {
            held++;
            return;
        }
        // CL_CheckForResend
        if (cls[k] == 1 && server) {
            cls[k] = 2;
            reconnects++;
        } else if (cls[k] == 2) {
            resends++;
        }
        // the server's reply and the signon, in one step here: a connecting client becomes active, a new userid
        if (cls[k] == 2 && server) {
            cls[k] = 5;
            userid[k] = next_userid++;
        }
    }
    void Remove(int k) {
        if (!p.Leave(k)) return;
        removals[k]++;
        ui.Left(k, p);
        p.ContextRemoved(p.At(k).context);
        p.Released(k);
        watch[k] = {};
        dep.Clear(k);
        dropped += q.Drop(k);
        userid[k] = 0;
        cls[k] = 1;
        if (p.At(0).state == State::Departed && p.Playing() == 0) end = true;
    }
    void FrameEnd() {
        for (int k = 0; k < kMaxPlayers; k++) {
            State s = p.At(k).state;
            if (s != State::Connecting && s != State::Active) continue;
            bool pending = dep.Pending(k), drop = watch[k].Dropped(cls[k]);
            if (!(fixed ? RemoveAtFrameEnd(pending, drop) : drop)) continue;
            if (k == 0) {
                if (OthersRemain(p, 0)) {
                    if (p.Depart0()) departures++;
                    dep.Clear(0);
                    ui.Left(0, p);
                } else {
                    dep.Clear(0);
                }
                continue;
            }
            Remove(k);
        }
        if (end) {
            end = false;
            for (int k = kMaxPlayers - 1; k > 0; k--) Remove(k);
            Shutdown_();
            sessions_ended++;
        }
        for (int k = 1; k < kMaxPlayers; k++)
            if (p.At(k).state == State::Connecting && cls[k] == 5) p.Joined(k);
    }
    void Frame() {
        std::vector<int> playing;
        for (int k = 0; k < kMaxPlayers; k++)
            if (p.At(k).state != State::Empty && p.At(k).state != State::Leaving) playing.push_back(k);
        for (int k : playing) InputTurn(k);
        for (int k : playing) ClientTurn(k);
        FrameEnd();
    }
    void Frames(int n) {
        for (int i = 0; i < n; i++) Frame();
    }
};

static FrameOrderSim TwoPlaying(bool fixed = true) {
    FrameOrderSim s;
    s.fixed = fixed;
    s.watch[0].Dropped(5);
    s.Add(1);
    s.Frames(2);
    return s;
}

static void SameFrameReconnect() {
    std::printf("note: SameFrameReconnect: simulated frames (no game); the runtime path is native_split_2p_leave's\n");
    {  // before the correction: the eleventh run's failure, reproduced
        FrameOrderSim s = TwoPlaying(false);
        Check(s.p.At(1).state == State::Active && s.userid[1] == 2, "(unfixed) player 2 in the match, userid 2");
        s.q.Add(1, "disconnect\n");
        s.Frames(5);
        Check(s.removals[1] == 0 && s.p.At(1).state == State::Active && s.userid[1] == 3 && s.reconnects == 2,
              "(unfixed) CL_CheckForResend reconnects it in place (userid 3): never removed - the eleventh run");
    }
    {  // player 2 disconnects at the start of its turn; CL_CheckForResend later in the same frame
        FrameOrderSim s = TwoPlaying();
        int before = s.reconnects;
        s.q.Add(1, "disconnect\n");
        s.InputTurn(0);
        s.InputTurn(1);
        Check(s.dep.Pending(1) && s.dep.Why(1) == Leave::Disconnect && s.cls[1] == 1,
              "its own disconnect at its turn's start: its departure recorded, cls.state 1");
        Check(!s.dep.Pending(0) && s.p.At(0).state == State::Active, "player 1 not marked");
        s.ClientTurn(0);
        s.ClientTurn(1);
        Check(s.held == 1 && s.reconnects == before && s.cls[1] == 1,
              "CL_CheckForResend later in the frame: held, no reconnect, still disconnected");
        s.FrameEnd();
        Check(s.removals[1] == 1 && s.p.At(1).state == State::Empty && !s.dep.Pending(1),
              "the frame's end removes it once; the record cleared");
        s.Frames(3);
        Check(s.removals[1] == 1, "removed exactly once (later frames nothing)");
        Check(s.p.At(0).state == State::Active && s.cls[0] == 5 && s.userid[0] == 1 && s.server && s.shutdowns == 0,
              "player 1 active throughout, the server kept");
        // an explicit rejoin: a fresh context, the normal connect and signon
        int ctx_before = s.contexts_made;
        Check(s.Add(1) && s.p.At(1).context == 1 && s.contexts_made == ctx_before + 1 && !s.dep.Pending(1) &&
                  s.q.Size(1) == 0,
              "a rejoin: a new context in the empty slot, nothing pending, no queued lines");
        s.Frames(2);
        Check(s.p.At(1).state == State::Active && s.userid[1] == 3 && s.held == 1,
              "it connects as the title's (not held) and joins with a new userid");
    }
    {  // repeated departure requests, and lines after the disconnect
        FrameOrderSim s = TwoPlaying();
        s.q.Add(1, "disconnect\nsay after\ndisconnect\n");
        s.InputTurn(1);
        Check(s.ran.size() == 1 && s.ran[0].second == "disconnect", "the lines after its disconnect in the run: dropped");
        s.Disconnect(1, Leave::Kicked);
        Check(s.dep.Repeats() == 1 && s.dep.Why(1) == Leave::Disconnect, "a repeated request: counted, the first cause kept");
        s.q.Add(1, "connect localhost\n");
        s.InputTurn(1);
        Check(s.q.Size(1) == 0 && s.ran.size() == 1, "a held player's later lines never run");
        s.ClientTurn(1);
        s.FrameEnd();
        s.FrameEnd();
        Check(s.removals[1] == 1 && s.q.Size(1) == 0, "removed once, its queue empty");
    }
    {  // a kicked player (svc_disconnect: its CL_Disconnect in its turn)
        FrameOrderSim s = TwoPlaying();
        s.kick_next[1] = true;
        s.Frame();
        Check(s.removals[1] == 1 && s.reconnects == 1 && s.server && s.p.At(0).state == State::Active,
              "a kicked player 2: held, removed, the server kept");
    }
    {  // normal retries when no departure is pending
        FrameOrderSim s;
        s.watch[0].Dropped(5);
        s.Add(1);
        s.server = false;  // its first connect waits (no answer yet): state 2 resent each frame
        s.cls[1] = 2;
        s.Frames(3);
        Check(s.resends == 3 && s.held == 0 && s.removals[1] == 0, "a connecting player's retries: the title's, not held");
        Players p = With(2);
        Departures d;
        Check(!HoldsReconnect(true, true, 1, d, p) && !HoldsReconnect(true, true, 0, d, p),
              "nobody held without a departure");
        d.Mark(1, p, Leave::Disconnect);
        Check(HoldsReconnect(true, true, 1, d, p) && !HoldsReconnect(true, true, 0, d, p) &&
                  !HoldsReconnect(true, false, 1, d, p) && !HoldsReconnect(false, true, 1, d, p) &&
                  !HoldsReconnect(true, true, -1, d, p),
              "held only in the departing player's own turn, with native split on");
    }
    {  // solo: the title's own
        FrameOrderSim s;
        s.split = false;
        s.watch[0].Dropped(5);
        s.q.Add(0, "disconnect\n");
        s.Frame();
        Check(s.shutdowns == 1 && !s.server && s.dep.Marks() == 0 && s.held == 0, "solo: its disconnect shuts the server, nothing held");
        FrameOrderSim t;
        t.watch[0].Dropped(5);
        t.q.Add(0, "disconnect\n");
        t.Frame();
        Check(t.shutdowns == 1 && t.dep.Marks() == 0 && t.held == 0, "native split with player 1 alone: the title's own");
    }
    {  // player 1 departs while player 2 plays: no reconnect in place, ever, while the others play
        FrameOrderSim s = TwoPlaying();
        s.q.Add(0, "disconnect\n");
        s.Frame();
        Check(s.departures == 1 && s.p.At(0).state == State::Departed && s.cls[0] == 1 && s.server,
              "player 1 departs: the server kept");
        s.Frames(10);
        Check(s.cls[0] == 1 && s.userid[0] == 1 && s.held >= 11 && s.p.At(1).state == State::Active &&
                  s.p.At(1).context == 1 && s.ui.driver == 1,
              "departed player 1 never reconnects in place; player 2 plays on, drives, not renumbered");
        s.q.Add(1, "disconnect\n");
        s.Frame();
        Check(s.sessions_ended == 1 && s.shutdowns == 1 && s.p.At(0).state == State::Active && s.p.Count() == 1 &&
                  !s.server,
              "the final player leaves: the session ends once; player 1 the title's own again");
        int held = s.held;
        s.Frames(2);
        Check(s.held == held && s.dep.Marks() == 2, "after the session's end player 1 is not held (no server to rejoin)");
    }
    {  // four players: 2 and 4 depart in one frame, 3 stays, a repeat ignored
        FrameOrderSim s;
        s.watch[0].Dropped(5);
        for (int k = 1; k < 4; k++) {
            s.Add(k);
            s.Frames(1);
        }
        Check(s.p.Playing() == 4, "four players playing");
        s.q.Add(1, "disconnect\n");
        s.q.Add(3, "disconnect\ndisconnect\n");
        s.Frame();
        Check(s.removals[1] == 1 && s.removals[3] == 1 && s.removals[2] == 0 && s.p.At(2).state == State::Active &&
                  s.p.At(2).context == 1 && s.p.Playing() == 2 && s.server,
              "four: 2 and 4 removed once each, 3 plays on (its context moves down, its slot kept)");
        Check(s.Add(1) && s.p.At(1).context == 2 && !s.dep.Pending(1), "a new player in slot 1: a fresh context");
    }
    {  // a disconnect during an incomplete initialisation / signon
        FrameOrderSim s;
        s.watch[0].Dropped(5);
        s.Add(1);
        s.server = false;  // no signon yet: it stays connecting
        s.cls[1] = 2;
        s.Frame();
        Check(s.p.At(1).state == State::Connecting, "player 2 connecting");
        s.server = true;
        s.q.Add(1, "disconnect\n");
        s.InputTurn(1);
        Check(s.dep.Pending(1), "a connecting player's disconnect: recorded");
        s.ClientTurn(1);
        s.FrameEnd();
        Check(s.removals[1] == 1 && s.p.At(1).state == State::Empty, "removed, never joined");
        Players p;
        p.Begin(1, 1, "P");
        Departures d;
        Check(!d.Mark(1, p, Leave::Disconnect) && !d.Mark(0, With(1), Leave::None), "an initialising player is never marked");
        Players left = With(2);
        left.Leave(1);
        Check(!d.Mark(1, left, Leave::Disconnect) && !d.Mark(5, left, Leave::Disconnect), "nor a leaving or unknown slot");
    }
    {  // an ordinary temporary state never removes: a map change's reconnect (state 3 again), connecting, signon
        FrameOrderSim s = TwoPlaying();
        s.cls[1] = 3;
        s.FrameEnd();
        s.cls[1] = 4;
        s.FrameEnd();
        s.cls[1] = 5;
        s.FrameEnd();
        Check(s.removals[1] == 0 && s.dep.Marks() == 0, "states 3 and 4 between frames: not removed");
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

// ---- the cleanup's guest calls and repeated leaves (docs 15.16; simulated: no game, no guest code) ----------------

static void CleanupCalls() {
    for (size_t i = 0; i < kCleanupCallCount; i++)
        Check(size_t(kCleanupCalls[i].id) == i, "the contract table in CleanupFn's order (Contract() indexes it)");
    const CleanupCall& d = Contract(CleanupFn::Destroy552CA8);
    Check(d.fn == 0x1BAD90 && d.regs == kRegEsi && d.args == 0, "0x1BAD90: esi the object, nothing on the stack");
    Check(MeetsContract(d, kRegEsi, 0) && MeetsContract(d, kRegEsi | kRegEdi, 0), "esi set: met (edi too is harmless)");
    Check(!MeetsContract(d, kRegEdi, 0), "the twelfth run's call (edi only): refused");
    Check(!MeetsContract(d, kRegEsi, 1), "an argument too many: refused");
    const CleanupCall& v = Contract(CleanupFn::Delete552C70);
    Check(v.regs == kRegEcx && v.args == 1 && v.pops == 4 && v.vtable == 0x35EB78, "thiscall: ecx, the delete flag, ret 4");
    Check(!MeetsContract(v, 0, 1) && MeetsContract(v, kRegEcx, 1), "a thiscall without ecx: refused");
    Check(VtableMatches(v, 0x35EB78) && !VtableMatches(v, 0x361C8C) && !VtableMatches(v, 0),
          "a vtable call only on its constructor's vtable");
    Check(VtableMatches(Contract(CleanupFn::Free), 0x12345678), "a direct call has no vtable to match");
    for (size_t i = 0; i < kCleanupCallCount; i++) {
        int bits = 0;
        for (uint8_t r = kCleanupCalls[i].regs; r; r &= uint8_t(r - 1)) bits++;
        Check(bits <= 1, "every cleanup call takes at most one register (the object)");
    }
    Release ok[8] = {Release::Release, Release::None, Release::None, Release::None,
                     Release::None,    Release::None, Release::None, Release::Release};
    Check(ChildrenReleasable(ok, 8), "0x552C70's objects absent or its own: it is deleted");
    for (Release r : {Release::NotOwn, Release::SharedKind, Release::Referenced}) {
        Release one[8] = {Release::Release, r, Release::None, Release::None,
                          Release::None,    Release::None, Release::None, Release::None};
        Check(!ChildrenReleasable(one, 8), "one of its objects kept: 0x552C70 kept whole");
    }
}

// ---- the VGUI hierarchy's shared parent (docs 15.17) ----------------------------------------------------------------
// A model of the guest's words and blocks: the engine's root panel (the process's, made at its start) with its
// children vector at +0x2C (4 reserved: the 16-byte buffer from 0xB52DA), each local player's viewport (0x552B7C)
// set as its child (setParent -> 0xB5220 -> 0xB52B0), removeChild (0x10ADC0 -> 0xB51B0) as the cleanup's detach.
// Simulated: these are the rules native_split.cpp applies, not a run of the title's code.
namespace vgui {
struct World {
    heapown::Blocks b;
    std::map<uint32_t, uint32_t> mem;
    uint32_t next = 0x02000000;
    uint64_t foreign = 0, process_grows = 0, frees = 0;
    bool fixed = true;  // the grow's ownership rule on (false: the run-13 build, the acting owner's)
    uint32_t R(uint32_t va) const {
        auto it = mem.find(va);
        return it == mem.end() ? 0 : it->second;
    }
    uint32_t Alloc(uint32_t size, int owner, uint32_t site) {
        uint32_t va = next;
        next += (size + 0xFF) & ~0xFFu;
        b.Allocated(va, size, owner, site);
        return va;
    }
    // a panel: its vtable (one with the Panel's setParent / removeChild, as every class the title builds on it)
    uint32_t Panel(int owner, uint32_t vtable = kViewportVtable) {
        uint32_t p = Alloc(0xA84, owner, 0x1BAF1A);
        mem[p] = vtable;
        mem[p + kPanelChildren] = 0, mem[p + kPanelChildren + 4] = 4;
        mem[p + kPanelChildren + 8] = Alloc(16, owner, 0xB52DA);
        return p;
    }
    // 0xB52B0 from `acting`'s turn: its new buffer, its old one freed (a foreign free refused, as NativeSplitHeapFree)
    void Grow(uint32_t hdr, int32_t wanted, int acting) {
        int32_t cap = int32_t(R(hdr + 4));
        if (wanted <= cap) return;
        uint32_t start = b.Holding(hdr);
        const heapown::Block* hb = start ? b.At(start) : nullptr;
        int as = acting;
        if (fixed && VectorGrowOwner(wanted, cap, hb != nullptr, hb ? hb->owner : heapown::Blocks::kNone,
                                     heapown::IsImageAddress(hdr), false, acting) == GrowAs::Process) {
            as = heapown::kProcess;
            process_grows++;
        }
        int32_t n = cap ? cap : 1;
        while (n < wanted) n *= 2;
        uint32_t nb = Alloc(uint32_t(n) * 4, as, 0xB52DA), old = R(hdr + 8);
        for (int32_t i = 0; i < int32_t(R(hdr)); i++) mem[nb + 4 * uint32_t(i)] = R(old + 4 * uint32_t(i));
        mem[hdr + 4] = uint32_t(n), mem[hdr + 8] = nb;
        const heapown::Block* ob = b.At(old);
        bool refused = ob && ((as >= 0 && heapown::SkipFree(ob->owner, as)) || (as < 0 && fixed && ob->owner >= 0));
        if (ob && heapown::FreeIsForeign(*ob, as)) foreign++;
        if (!refused && ob) b.Freed(old), frees++;
    }
    // setParent(child, parent) from `acting`'s turn: off its old parent, appended once (0xB5220)
    void SetParent(uint32_t child, uint32_t parent, int acting) {
        if (uint32_t old = R(child + kPanelParent)) RemoveChild(old, child);
        mem[child + kPanelParent] = parent;
        uint32_t hdr = parent + kPanelChildren;
        for (uint32_t i = 0; i < R(hdr); i++)
            if (R(R(hdr + 8) + 4 * i) == child) return;
        Grow(hdr, int32_t(R(hdr)) + 1, acting);
        mem[R(hdr + 8) + 4 * R(hdr)] = child;
        mem[hdr]++;
    }
    // removeChild 0x10ADC0: the child's parent and +0x3C cleared, 0xB51B0 (entries moved down; nothing freed)
    void RemoveChild(uint32_t parent, uint32_t child) {
        mem[child + kPanelParent] = 0, mem[child + 0x3C] = 0;
        uint32_t hdr = parent + kPanelChildren, data = R(hdr + 8), n = R(hdr);
        for (uint32_t i = 0; i < n; i++)
            if (R(data + 4 * i) == child) {
                for (uint32_t j = i; j + 1 < n; j++) mem[data + 4 * j] = R(data + 4 * (j + 1));
                mem[hdr]--;
                return;
            }
    }
    PanelView View(uint32_t vp) const {
        PanelView v;
        const heapown::Block* x = vp ? b.At(vp) : nullptr;
        v.known = x != nullptr;
        v.owner = x ? x->owner : heapown::Blocks::kNone;
        v.panel = x && R(vp) == kViewportVtable;
        v.parent = x ? R(vp + kPanelParent) : 0;
        if (v.parent) {
            const heapown::Block* pb = b.At(b.Holding(v.parent));
            v.parent_owner = pb ? pb->owner : heapown::Blocks::kNone;
            v.parent_panel = R(v.parent) != 0;
            v.count = int32_t(R(v.parent + kPanelChildren)), v.capacity = int32_t(R(v.parent + kPanelChildren + 4));
            uint32_t data = R(v.parent + kPanelChildren + 8);
            v.data_ok = data && b.At(data) && b.At(data)->size >= uint32_t(v.capacity) * 4;
            for (int32_t i = 0; i < v.count; i++) v.listed += R(data + 4 * uint32_t(i)) == vp;
        }
        return v;
    }
    // the cleanup's detach (DetachViewport): DetachCheck, then removeChild
    Detach Leave(uint32_t vp, int k) {
        Detach d = DetachCheck(vp, View(vp), k);
        if (d == Detach::Detach) RemoveChild(R(vp + kPanelParent), vp);
        return d;
    }
    std::vector<ChildFinding> Stale(uint32_t parent) const {
        return StaleChildren(parent, b, [&](uint32_t va) { return R(va); });
    }
    bool Lists(uint32_t parent, uint32_t child) const {
        for (uint32_t i = 0; i < R(parent + kPanelChildren); i++)
            if (R(R(parent + kPanelChildren + 8) + 4 * i) == child) return true;
        return false;
    }
};
}  // namespace vgui

static void SharedParentRules() {
    // whose a grown vector's buffer is
    Check(VectorGrowOwner(5, 4, true, heapown::kProcess, false, false, 1) == GrowAs::Process,
          "a process block's vector grown in player 2's turn: as the process");
    Check(VectorGrowOwner(4, 4, true, heapown::kProcess, false, false, 1) == GrowAs::Acting,
          "no grow (within the capacity): the title's code as it is");
    Check(VectorGrowOwner(5, 4, true, 1, false, false, 1) == GrowAs::Acting, "its own block's vector: its own buffer");
    Check(VectorGrowOwner(5, 4, true, 0, false, false, 1) == GrowAs::Acting,
          "another player's block's vector: not reclassified (the guard reports a foreign free)");
    Check(VectorGrowOwner(5, 4, false, -2, true, false, 1) == GrowAs::Process, "a process-wide static vector: the process's");
    Check(VectorGrowOwner(5, 4, false, -2, true, true, 1) == GrowAs::Acting, "a static in the players' regions: its own");
    Check(VectorGrowOwner(5, 4, false, -2, false, false, 1) == GrowAs::Acting, "a stack vector: the acting owner's");
    Check(VectorGrowOwner(5, 4, true, heapown::kProcess, false, false, -1) == GrowAs::Acting,
          "outside the turns (and solo, native split off: no hook at all): the process already");
    Check(VectorGrowOwner(5, 4, true, heapown::kProcess, false, false, 0) == GrowAs::Process,
          "player 1's turn too (its allocations in turns are its own since docs 15.10)");
    // the detach's check, case by case
    PanelView v;
    Check(DetachCheck(0, v, 1) == Detach::None, "no viewport: nothing");
    v.known = true, v.owner = 0;
    Check(DetachCheck(0x02000000, v, 1) == Detach::NotOwn, "another's viewport: not touched");
    v.owner = 1;
    Check(DetachCheck(0x02000000, v, 1) == Detach::NotPanel, "not a panel: not called");
    v.panel = true;
    Check(DetachCheck(0x02000000, v, 1) == Detach::NoParent, "no parent: nothing to detach (already detached)");
    v.parent = 0x00909CC0, v.parent_owner = 1;
    Check(DetachCheck(0x02000000, v, 1) == Detach::OwnParent, "its own parent: its own tree, left as it is");
    v.parent_owner = heapown::kProcess;
    Check(DetachCheck(0x02000000, v, 1) == Detach::ParentNotPanel, "a parent that is not a panel: not called");
    v.parent_panel = true, v.count = 5, v.capacity = 4, v.data_ok = true;
    Check(DetachCheck(0x02000000, v, 1) == Detach::BadList, "a count past the capacity: not called");
    v.count = 2, v.data_ok = false;
    Check(DetachCheck(0x02000000, v, 1) == Detach::BadList, "a children buffer that is no block: not called");
    v.data_ok = true;
    Check(DetachCheck(0x02000000, v, 1) == Detach::NotListed, "not among the children: not called");
    v.listed = 2;
    Check(DetachCheck(0x02000000, v, 1) == Detach::Duplicate, "listed twice: not called (removeChild takes one)");
    v.listed = 1;
    Check(DetachCheck(0x02000000, v, 1) == Detach::Detach, "its own panel, once among a process panel's children: detached");
    // its contract
    const CleanupCall& c = Contract(CleanupFn::DetachChild);
    Check(c.fn == 0x10ADC0 && c.regs == kRegEcx && c.args == 1 && c.pops == 4 && c.slot == 0x4C && !c.vtable && !c.frees,
          "removeChild: ecx the parent, the child pushed, ret 4, through slot 0x4C, no free");
    Check(SlotMatches(c, 0x10ADC0) && !SlotMatches(c, 0x10AD00) && !SlotMatches(c, 0),
          "called only when the parent's vtable has removeChild at 0x4C");
    Check(MeetsContract(c, kRegEcx, 1) && !MeetsContract(c, kRegEcx, 0) && !MeetsContract(c, kRegEsi, 1),
          "the parent in ecx and exactly the child: met; anything else refused");
    Check(SlotMatches(Contract(CleanupFn::Delete552C70), 0), "the other contracts have no slot (their vtable exact)");
    Check(IsPanelVtable(kPanelSetParent, kPanelRemoveChild) && !IsPanelVtable(kPanelSetParent, 0),
          "a panel: both of the Panel's methods in its vtable");
}

static void SharedParentCycles() {
    // the run-13 build: player 2's third join grows the root's 4 children in its turn, the process's buffer refused
    {
        vgui::World w;
        w.fixed = false;
        uint32_t root = w.Panel(heapown::kProcess, 0x3447E4), other = w.Panel(heapown::kProcess, 0x3447E4);
        w.SetParent(other, root, heapown::kProcess);
        w.SetParent(w.Panel(heapown::kProcess), root, heapown::kProcess);  // player 1's (the process's start)
        for (int join = 0; join < 3; join++) {
            uint32_t vp = w.Panel(1);
            w.SetParent(vp, root, 1);
            if (join < 2) w.b.Orphan(1);  // its leave, not detached (the run-13 build)
        }
        Check(w.foreign == 1, "the run-13 build: the third join's grow frees the process's buffer (a foreign free)");
    }
    // the fix: ten leaves and rejoins of player 2
    vgui::World w;
    uint32_t root = w.Panel(heapown::kProcess, 0x3447E4), other = w.Panel(heapown::kProcess, 0x3447E4);
    w.SetParent(other, root, heapown::kProcess);
    uint32_t p1 = w.Panel(heapown::kProcess);  // player 1's viewport: made at the process's start (the process's)
    w.SetParent(p1, root, heapown::kProcess);
    uint32_t first_buf = w.R(root + kPanelChildren + 8);
    bool ok = true, once = true;
    std::vector<uint32_t> gone;
    for (int cycle = 0; cycle < 10; cycle++) {
        uint32_t vp = w.Panel(1);
        w.SetParent(vp, root, 1);
        ok &= w.Lists(root, vp) && w.Lists(root, p1) && w.R(root + kPanelChildren) == 3;
        Detach d = w.Leave(vp, 1);
        once &= d == Detach::Detach && w.Leave(vp, 1) == Detach::NoParent;  // a second detach does nothing
        w.b.Orphan(1);
        gone.push_back(vp);
        ok &= !w.Lists(root, vp) && w.Lists(root, p1) && w.R(vp + kPanelParent) == 0;
    }
    Check(ok, "each cycle: player 2's viewport a child while it plays, off the root at its leave; player 1's stays");
    Check(once, "detached exactly once (a second detach finds no parent)");
    Check(w.foreign == 0 && w.process_grows == 0 && w.R(root + kPanelChildren + 8) == first_buf,
          "ten cycles: the root's 4 children never grown (no free at all, none foreign)");
    Check(w.Stale(root).empty(), "no stale child: no departed player's viewport, no duplicate");
    for (uint32_t vp : gone) Check(w.b.At(vp) && w.b.At(vp)->left_by == 1, "each viewport left behind, never deleted");
    // four players: the root's 4 children grown at the fourth's join, as the process
    vgui::World f;
    uint32_t r4 = f.Panel(heapown::kProcess, 0x3447E4);
    f.SetParent(f.Panel(heapown::kProcess, 0x3447E4), r4, heapown::kProcess);
    uint32_t v1 = f.Panel(heapown::kProcess);
    f.SetParent(v1, r4, heapown::kProcess);
    uint32_t vps[4] = {v1, 0, 0, 0};
    for (int k = 1; k < 4; k++) f.SetParent(vps[k] = f.Panel(k), r4, k);
    const heapown::Block* buf = f.b.At(f.R(r4 + kPanelChildren + 8));
    Check(f.process_grows == 1 && f.foreign == 0 && buf && buf->owner == heapown::kProcess && f.frees == 1,
          "four players: the fifth child grows the root's vector as the process (its old buffer freed by its owner)");
    Check(f.R(r4 + kPanelChildren) == 5, "four viewports and the engine's own child: five children");
    Check(f.Leave(vps[2], 2) == Detach::Detach && !f.Lists(r4, vps[2]) && f.Lists(r4, vps[1]) && f.Lists(r4, vps[3]) &&
              f.Lists(r4, v1),
          "player 3 leaves: only its viewport off the root; players 1, 2 and 4 keep theirs");
    f.b.Orphan(2);
    Check(f.Leave(vps[1], 2) == Detach::NotOwn, "player 3's cleanup never touches player 2's viewport");
    for (int k = 2; k < 4; k++) f.SetParent(f.Panel(k), r4, k);  // player 3 back, and a rejoin of player 4's slot
    Check(f.foreign == 0 && f.Stale(r4).empty(), "rejoins over a grown vector: no foreign free, no stale child");
    // player 1 departs while player 2 plays: its context (and viewport, the process's) kept on the root
    vgui::World g;
    uint32_t rg = g.Panel(heapown::kProcess, 0x3447E4), g1 = g.Panel(heapown::kProcess), g2 = g.Panel(1);
    g.SetParent(g1, rg, heapown::kProcess);
    g.SetParent(g2, rg, 1);
    Check(g.Leave(g1, 0) == Detach::NotOwn && g.Lists(rg, g1) && g.Lists(rg, g2),
          "player 1's departure: its viewport (the process's) not detached; player 2's untouched");
    Check(g.Stale(rg).empty(), "player 1's kept viewport is no stale child (it is no departed player's block)");
    // the stale-parent detection: a viewport left on the root, a freed child, a duplicate entry
    vgui::World s;
    uint32_t rs = s.Panel(heapown::kProcess, 0x3447E4), s2 = s.Panel(1), s3 = s.Panel(2);
    s.SetParent(s2, rs, 1);
    s.SetParent(s3, rs, 2);
    s.b.Orphan(1);  // player 2 left without the detach
    auto st = s.Stale(rs);
    Check(st.size() == 1 && st[0].child == s2 && st[0].left_by == 1 && !st[0].duplicate,
          "a departed player's viewport kept on the process's panel: reported [own:stale-child]");
    s.b.Freed(s3);
    st = s.Stale(rs);
    Check(st.size() == 2 && st[1].child == s3 && st[1].left_by == -2, "a freed child still listed: reported");
    uint32_t data = s.R(rs + kPanelChildren + 8);
    s.mem[data + 4] = s2;  // the same panel twice
    st = s.Stale(rs);
    Check(st.size() == 2 && st[1].duplicate, "a panel listed twice: reported");
    vgui::World e;
    uint32_t re = e.Panel(heapown::kProcess, 0x3447E4);
    e.mem[re + kPanelChildren] = 9;  // a count past the capacity: not read
    Check(e.Stale(re).empty(), "a vector that is not sane is not read");
}

// three leaves and rejoins of player 2 over one heap: the run-12 pattern (a process-wide vector grown in its turn)
static void RepeatedLeaves() {
    heapown::Blocks b;
    std::map<uint32_t, uint32_t> mem;  // guest words
    const uint32_t kVec = 0x5620A8, kProc = 0x00C93FF0, kP1 = 0x01000000;
    b.Allocated(kProc, 64, heapown::kProcess, 0xB52DA);  // the process's vector buffer (its initialisation)
    b.Allocated(kP1, 32, 0, 0x1000);                       // player 1's object
    mem[kVec + 8] = kProc;
    Players p = With(2);
    size_t left_total = 0, blocked = 0, foreign_done = 0, removals = 0;
    LeakBudget lb;
    uint32_t next = 0x02000000;
    for (int cycle = 0; cycle < 3; cycle++) {
        // its turn: its objects, one named by nobody, one by the shared data, one grown vector buffer
        uint32_t own = next, named = next + 0x100, buf = next + 0x200;
        next += 0x1000;
        b.Allocated(own, 16, 1, 0x2E2E0);
        b.Allocated(named, 16, 1, 0x1A6E0F);
        mem[0x36E000 + 4 * uint32_t(cycle)] = named;
        // the vector's grow in its turn: its new buffer, the old one's free refused when not its own
        uint32_t old = mem[kVec + 8];
        b.Allocated(buf, 128, 1, 0xB52DA);
        mem[kVec + 8] = buf;
        const heapown::Block* ob = b.At(old);
        if (ob && heapown::SkipFree(ob->owner, 1)) {
            blocked++;
            Check(cycle == 0 ? ob->left_by == -2 : ob->left_by == 1,
                  "the refused buffer: the process's own first, then one an earlier player 2 left behind");
        } else if (ob) {
            b.Freed(old);
        }
        // the leave: removed exactly once
        Check(p.Leave(1) && !p.Leave(1), "a leaving player is removed once (a second Leave does nothing)");
        removals++;
        auto plan = heapown::PlanRollback(
            b, 1,
            [&](auto f) {
                for (auto& [a, v] : mem)
                    if (a < 0x01000000) f(a, v);
            },
            [&](uint32_t va) { auto it = mem.find(va); return it == mem.end() ? 0u : it->second; });
        size_t total = 0, released = 0, kept = 0;
        b.ForEach([&](uint32_t, const heapown::Block& x) {
            if (x.owner == 1) total += x.size;
        });
        std::set<uint32_t> keep;
        for (auto& r : plan.keep) keep.insert(r.va), kept += r.size;
        for (auto& r : plan.release) {
            // its own frees only (as NativeSplitHeapFree): never another owner's block
            const heapown::Block* x = b.At(r.va);
            if (!x || heapown::FreeIsForeign(*x, 1)) {
                foreign_done++;
                continue;
            }
            released += x->size;
            b.Freed(r.va);
        }
        Check(keep.count(named) && keep.count(buf) && !keep.count(own), "the named and the vector's buffer kept; its own freed");
        auto [n, bytes] = b.Orphan(1);
        Check(bytes == kept && released + bytes == total, "released + left behind = everything it owned (accounting)");
        Check(b.At(buf) && b.At(buf)->owner == heapown::kProcess && b.At(buf)->left_by == 1,
              "the kept buffer handed to the process, marked left behind by player 2");
        lb.Left(bytes);
        left_total += bytes;
        Check(b.At(kP1) && b.At(kP1)->owner == 0 && b.At(kProc) && b.At(kProc)->owner == heapown::kProcess,
              "player 1's object and the process's buffer untouched by the cleanup");
        p.Released(1);
        // the rejoin: a fresh context (no heap pointer from the boot image), then its initialisation
        Check(p.Begin(1, 1, "Player 2") && p.Initialised(1) && p.Joined(1), "player 2 joins again in the slot");
        std::vector<heapown::Span> spans = {{0, 0x750000, 16}};
        std::map<uint32_t, uint32_t> fresh;
        auto read = [&](uint32_t va) { auto it = fresh.find(va); return it == fresh.end() ? 0u : it->second; };
        Check(heapown::LeftBehindNames(spans, b, read).empty(), "a fresh context names nothing left behind");
        fresh[0x750004] = named + 4;  // a stale name of what an earlier player 2 left
        auto stale = heapown::LeftBehindNames(spans, b, read);
        Check(stale.size() == 1 && stale[0].va == 0x750004 && stale[0].block == named, "a stale name found: its word and block");
        fresh[0x750004] = kP1;
        Check(heapown::LeftBehindNames(spans, b, read).empty(), "player 1's object is not a left-behind one");
    }
    Check(removals == 3 && blocked == 3 && foreign_done == 0, "three cycles: every foreign free refused, none done");
    Check(lb.leaves == 3 && lb.left_behind == left_total && lb.MayJoin(), "what was left behind summed over the leaves");
    // a double free: a block freed, then freed again before any allocation reuses the address
    heapown::Blocks d;
    d.Allocated(0x3000000, 8, 1);
    Check(d.Freed(0x3000000) == 1 && d.WasFreed(0x3000000) && !d.At(0x3000000), "freed once: remembered");
    d.Allocated(0x3000000, 8, 1);
    Check(!d.WasFreed(0x3000000), "the address allocated again: a later free is not a double one");
    Check(d.Moved(0x3000000, 0x3001000, 16, 1) && d.WasFreed(0x3000000) && !d.WasFreed(0x3001000),
          "a moved block's old address freed, its new one live");
    Check(!d.WasFreed(0x3002000), "an address never known: not a double free");
}


// ---- retention, focus and the cleanup's destructors (docs 15.18) -------------------------------------------------
// Simulated: the rules native_split.cpp applies, fed modelled guest words and blocks; no title code runs.
static void FocusAndLinks() {
    // a focus field cleared only for a panel of the leaving player (players 2..4)
    Check(ClearFocus(true, 1, 1), "player 2's viewport as App's mouse focus at its leave: cleared");
    Check(!ClearFocus(true, 0, 1), "player 1's panel: kept");
    Check(!ClearFocus(true, heapown::kProcess, 1), "the process's panel: kept");
    Check(!ClearFocus(false, 1, 1), "a value of the leaving player's that is no panel: not touched");
    Check(!ClearFocus(true, 0, 0), "player 1's departure keeps its context: nothing cleared");
    Check(sizeof kAppFocus / sizeof kAppFocus[0] == 5 && kAppFocus[2].offset == 0x1C && kApp + 0x1C == 0x384B74,
          "the five focus fields; the mouse focus at 0x384B74 (run 14's word)");
    // App modelled: its focus fields, a tick setting the mouse focus to the topmost viewport (setMouseFocus 0x10A400
    // calls the old focus's exit), the leave clearing what names the leaving player's panel
    vgui::World w;
    uint32_t root = w.Panel(heapown::kProcess, 0x3447E4), p1 = w.Panel(heapown::kProcess), p2 = w.Panel(1);
    w.SetParent(p1, root, heapown::kProcess);
    w.SetParent(p2, root, 1);
    std::map<uint32_t, uint32_t> app;  // offset -> value
    app[0x1C] = p2, app[0x18] = p1, app[0x14] = p2, app[0x20] = 0, app[0x24] = root;
    std::vector<std::pair<uint32_t, int>> exits;  // the panel whose exit ran, from whose code
    auto set_focus = [&](uint32_t next) {
        if (app[0x1C] == next) return;
        uint32_t old = app[0x1C];
        app[0x1C] = next, app[0x18] = old;
        if (old) exits.push_back({old, w.b.At(old) ? w.b.At(old)->owner : -2});
    };
    auto leave = [&](int k, bool clear) {
        w.Leave(p2, k);
        if (clear)
            for (const FocusField& f : kAppFocus) {
                uint32_t v = app[f.offset];
                const heapown::Block* b = v ? w.b.At(w.b.Holding(v)) : nullptr;
                if (v && ClearFocus(w.R(v) != 0, b ? b->owner : heapown::Blocks::kNone, k)) app[f.offset] = 0;
            }
        w.b.Orphan(k);
    };
    {
        auto saved = app;
        leave(1, false);  // the run-14 build: the focus left on player 2's viewport
        set_focus(p1);    // the next tick (player 1's turn)
        Check(exits.size() == 1 && exits[0].first == p2 && w.b.At(p2)->left_by == 1,
              "without the clear: the next tick calls the departed viewport's exit (a left-behind object's code)");
        app = saved;
    }
    vgui::World w2;
    w = w2;
    root = w.Panel(heapown::kProcess, 0x3447E4), p1 = w.Panel(heapown::kProcess), p2 = w.Panel(1);
    w.SetParent(p1, root, heapown::kProcess);
    w.SetParent(p2, root, 1);
    app.clear();
    app[0x1C] = p2, app[0x18] = p1, app[0x14] = p2, app[0x24] = root;
    exits.clear();
    leave(1, true);
    set_focus(p1);
    Check(app[0x14] == 0 && app[0x1C] == p1 && app[0x18] == 0 && app[0x24] == root,
          "with the clear: the key focus and mouse focus off player 2's panel; the process's root kept");
    Check(exits.empty(), "the next tick calls no departed object's code (the old focus 0, as removeSurfaceBase leaves it)");

    // links classified at a call's entry
    Check(ClassifyLink(0, 0x909CC0, 0) == Link::New, "no parent, not listed: a new attachment");
    Check(ClassifyLink(0x909CC0, 0x909CC0, 1) == Link::Reattach, "already its child: a re-attachment, no change");
    Check(ClassifyLink(0x2000000, 0x909CC0, 0) == Link::Change, "from another parent: a parent change");
    Check(ClassifyLink(0x909CC0, 0, 0) == Link::Detach && ClassifyLink(0, 0, 0) == Link::Nothing,
          "setParent(0): a detachment (nothing when it had none)");
    Check(ClassifyLink(0, 0x909CC0, 1) == Link::Duplicate && ClassifyLink(0x909CC0, 0x909CC0, 2) == Link::Duplicate,
          "listed without being its child, or twice: a duplicate attempt");
    Check(PartOfLink(0x10ACD0) && PartOfLink(0x10AD17) && PartOfLink(0x10AD99) && !PartOfLink(0x10ADC0),
          "removeChild from setParent / addChild / insertChildAt: part of their link, not a detachment");
    // run 14's pattern: player 1's turn re-adds the engine's child ~15 times a second: no structural change
    uint64_t links[6] = {};
    links[int(ClassifyLink(0, 0x909CC0, 0))]++;
    for (int i = 0; i < 1600; i++) links[int(ClassifyLink(0x909CC0, 0x909CC0, 1))]++;
    links[int(ClassifyLink(0, 0x909CC0, 0))]++;      // player 2's viewport
    links[int(Link::Detach)]++;                       // its detach by the cleanup
    Check(links[int(Link::New)] == 2 && links[int(Link::Reattach)] == 1600 && links[int(Link::Detach)] == 1 &&
              links[int(Link::Change)] == 0 && links[int(Link::Duplicate)] == 0,
          "1600 repeated adds are re-attachments; the structural count is 2 attachments and 1 detachment");
}

static void RetentionRules() {
    // the roots' evidence
    Check(EvidenceOf(RootKind::StaticWord, 0x384B74, 0) == Evidence::Verified, "App's mouse focus: a verified field");
    Check(EvidenceOf(RootKind::StaticWord, 0x5622B0, 0) == Evidence::Verified, "the intern pool's search key: verified");
    Check(EvidenceOf(RootKind::StaticWord, 0x62DCA4, 0) == Evidence::Unverified,
          "run 14's third leave: a word of the 32 KB message buffer's bytes");
    Check(EvidenceOf(RootKind::StaticWord, 0x5D1DC8, 0) == Evidence::Unverified,
          "run 14's second leave: a word of the 64x64 texture's texels");
    Check(EvidenceOf(RootKind::StaticWord, 0x540978, 0) == Evidence::Unverified &&
              (0x540978u - 0x53FD20u) % 0x34u == 0x28,
          "run 14's first leave: sentence group 60's +0x28, an index byte (docs 15.21; was unclassified)");
    Check(EvidenceOf(RootKind::ProcessBlock, 0, 0x10829B) == Evidence::Unverified, "a whole file's bytes (0x108230)");
    Check(EvidenceOf(RootKind::ProcessBlock, 0, 0x66943) == Evidence::Unclassified, "the model loader's block: unclassified");
    Check(EvidenceOf(RootKind::SharedKind, 0, 0) == Evidence::Verified, "a shared kind: by its kind's rule");
    Check(!StaticAreaOf(0x5D34C8) && StaticAreaOf(0x5D34C7) && !StaticAreaOf(0x635BD4) && StaticAreaOf(0x635BD3),
          "the byte areas end where their buffers end (0x2000, 0x8000 bytes)");
    Check(PersistentSiteOf(0x1F9BBE) && PersistentSiteOf(0x1F9EE6) && PersistentSiteOf(0x1BAF1A) &&
              !PersistentSiteOf(0xB52DA) && !PersistentSiteOf(0),
          "persistent by the original: the particle pool, its CMiniMem, a viewport; nothing else");
    // categories: each block once
    Check(Categorize(true, true, true) == Retained::Persistent && Categorize(false, false, true) == Retained::Unreferenced &&
              Categorize(false, true, true) == Retained::Verified && Categorize(false, true, false) == Retained::Conservative,
          "persistent first, then named by nobody, then verified reach, else conservative");
    // reachability from verified roots, counted once: a verified root and an unverified one both reach a shared block
    heapown::Blocks b;
    std::map<uint32_t, uint32_t> mem;
    auto rd = [&](uint32_t va) { auto it = mem.find(va); return it == mem.end() ? 0u : it->second; };
    uint32_t vp = 0x2000000, child = 0x2001000, pool = 0x2100000, tail = 0x2002000, other = 0x2003000;
    b.Allocated(vp, 0xA84, 1, 0x1BAF1A);
    b.Allocated(child, 0x40, 1, 0x1B4DB1);
    b.Allocated(pool, 637500, 1, 0x1F9BBE);
    b.Allocated(tail, 0x10, 1, 0xB52DA);
    b.Allocated(other, 0x10, 1, 0x1A6E0F);
    b.Allocated(0x1000000, 0x800, heapown::kProcess, 0x10829B);  // a file buffer of the process's
    mem[vp + 0x40] = child, mem[child + 4] = tail;
    mem[0x384B74] = vp;                 // App's mouse focus (verified)
    mem[0x62DCA4] = pool + 0x1234;      // message bytes naming the pool's inside (unverified, interior)
    mem[0x1000000 + 0x7F0] = other;     // the file buffer's never written tail
    mem[0x1000000 + 0x7F4] = child;     // and the same child a second time
    auto plan = heapown::PlanRollback(
        b, 1, [&](auto f) { for (auto& [a, v] : mem) if (a < 0x1000000) f(a, v); }, rd);
    std::map<uint32_t, const heapown::RollbackItem*> by;
    for (auto& r : plan.keep) by[r.va] = &r;
    Check(plan.keep.size() == 5 && plan.release.empty(), "every one of its blocks named: all five kept");
    Check(by.count(pool) && by[pool]->named_at == 0x62DCA4 && by[pool]->named_value == pool + 0x1234,
          "the pool kept through the message buffer's word: an interior pointer (+0x1234) recorded");
    Check(by.count(vp) && by[vp]->named_value == vp, "the viewport named directly");
    auto reach = heapown::ReachableFrom(b, 1, {vp}, rd);
    Check(reach.size() == 3 && reach.count(vp) && reach.count(child) && reach.count(tail) && !reach.count(pool),
          "from App's focus alone: the viewport, its child and that child's buffer");
    size_t total = 0, counted = 0;
    size_t cat[5] = {};
    b.ForEach([&](uint32_t va, const heapown::Block& x) {
        if (x.owner != 1) return;
        total++;
        std::set<uint32_t> kept;
        for (auto& r : plan.keep) kept.insert(r.va);
        cat[int(Categorize(PersistentSiteOf(x.site) != nullptr, kept.count(va) > 0, reach.count(va) > 0))]++;
    });
    for (size_t c : cat) counted += c;
    Check(counted == total && cat[int(Retained::Persistent)] == 2 && cat[int(Retained::Verified)] == 2 &&
              cat[int(Retained::Conservative)] == 1,
          "each block counted once: 2 persistent (viewport, pool), 2 verified (child, buffer), 1 conservative");
    // the retention bound's projection (run 14: 929355, 929371, 929355 bytes)
    LeakBudget lb;
    Check(lb.Projected() == -1, "no leave yet: nothing to project");
    for (size_t x : {929355u, 929371u, 929355u}) lb.Left(x);
    Check(lb.left_behind == 2788081 && lb.Projected() == 6, "run 14: 2788081 bytes in 3 leaves: about 6 more leaves");
    for (int i = 0; i < 6; i++) lb.Left(929360);
    Check(lb.MayJoin() && lb.Projected() == 0, "nine leaves: still within 8 MiB, none more projected");
    lb.Left(929360);
    Check(!lb.MayJoin() && lb.Projected() == 0, "the tenth: past the bound, joins stop (the bound unchanged)");
    Check(LeakBudget{}.limit == size_t(8) << 20, "the bound stays 8 MiB");
}

// the particle manager's pool (CMiniMem 0x1F9B90) modelled from its constructor: why its destructor is never called
static void ParticlePool() {
    heapown::Blocks b;
    const uint32_t count = 0x7530 >> 4, elem = 320, pool = 0x2079010;
    uint32_t size = count * (elem + 0x14);
    Check(size == 637500, "30000 >> 4 entries of 320 + 0x14 bytes: run 14's 637500-byte block");
    b.Allocated(pool, size, 1, 0x1F9BBE);
    // each entry (16 bytes at the pool's start) keeps at +4 a place in the same block (0x1F9C10..0x1F9C25)
    size_t interior = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t data = pool + (count << 4) + elem * i;
        interior += b.Holding(data) == pool && data != pool;
    }
    Check(interior == count, "every entry's +4 is inside the pool, never a block start");
    // the destructor 0x1F9D10 -> each entry's 0x1F9B30 frees its +4: 1875 frees of no block start
    size_t not_start = 0;
    for (uint32_t i = 0; i < count; i++) not_start += b.At(pool + (count << 4) + elem * i) == nullptr;
    Check(not_start == count, "its destructor would free 1875 places inside a block: never called; the pool kept");
    Check(PersistentSiteOf(0x1F9BBE) != nullptr, "reported as persistent by the original, not as a leak to reclaim");
    // a fresh context's join: the singleton (0x552CE4, in each player's regions) 0, so a new pool on first use
    std::map<int, uint32_t> singleton;  // per join
    uint32_t next = 0x3000000;
    for (int join = 0; join < 3; join++) {
        if (!singleton[join]) {
            b.Allocated(next, size, 1, 0x1F9BBE);
            singleton[join] = next;
            next += 0x100000;
        }
        b.Orphan(1);
    }
    size_t pools = 0;
    b.ForEach([&](uint32_t, const heapown::Block& x) { pools += x.site == 0x1F9BBE && x.left_by == 1; });
    Check(pools == 4, "each rejoin's fresh context makes its own pool: one more 637500-byte block each leave");
}

static void DestructorSteps() {
    Check(AfterCall(false, Post::Keeps, true) == Step::Skipped, "not called (its contract refused): skipped");
    Check(AfterCall(true, Post::Keeps, true) == Step::Returned, "0x1BAD90 returned, its object still allocated: returned");
    Check(AfterCall(true, Post::Keeps, false) == Step::Failed, "0x1BAD90 returned, its object gone: failed");
    Check(AfterCall(true, Post::Deletes, false) == Step::Returned, "a deleting destructor freed its object: returned");
    Check(AfterCall(true, Post::Deletes, true) == Step::Failed, "a deleting destructor left its object: failed");
    Check(AfterCall(true, Post::NotABlock, false) == Step::Returned, "a static slot's free: returned");
    Check(std::string(Name(Step::NotConsidered)) != Name(Step::Skipped) &&
              std::string(Name(Step::Returned)) != Name(Step::Failed),
          "four distinct outcomes, each logged by its name");
    const CleanupCall& d = Contract(CleanupFn::Destroy552CA8);
    Check(d.fn == 0x1BAD90 && d.regs == kRegEsi && d.args == 0, "the contract unchanged: esi the object, no argument");
    // the four outcomes over a modelled cleanup: no object; another's object; its own (returned); one it frees itself
    heapown::Blocks b;
    b.Allocated(0x2000000, 64, 1);
    b.Allocated(0x2100000, 64, 0);
    auto outcome = [&](uint32_t obj, bool frees_itself) {
        if (!obj) return Step::NotConsidered;
        const heapown::Block* x = b.At(obj);
        if (ReleaseCheck(x != nullptr, x ? x->owner : -2, true, 1, false) != Release::Release) return Step::Skipped;
        if (frees_itself) b.Freed(obj);
        return AfterCall(true, Post::Keeps, b.At(obj) != nullptr);
    };
    Check(outcome(0, false) == Step::NotConsidered, "0x552CA8 empty: not considered");
    Check(outcome(0x2100000, false) == Step::Skipped && b.At(0x2100000), "player 1's object: skipped, kept");
    Check(outcome(0x2000000, false) == Step::Returned, "its own: invoked and returned (its free follows)");
    Check(outcome(0x2000000, true) == Step::Failed, "an object the destructor itself freed: failed, not freed again");
}

static void GrowLeftBehind() {
    Check(VectorGrowOwner(5, 4, true, heapown::kProcess, false, false, 1, true) == GrowAs::Acting,
          "a vector in a block a departed player left behind: not grown as the process (the guard reports its free)");
    Check(VectorGrowOwner(5, 4, true, heapown::kProcess, false, false, 1, false) == GrowAs::Process,
          "the process's own block: as the process (unchanged)");
    // three and four players over the root, its old buffer freed by its owner each time
    for (int players = 3; players <= 4; players++) {
        vgui::World f;
        uint32_t r = f.Panel(heapown::kProcess, 0x3447E4);
        f.SetParent(f.Panel(heapown::kProcess, 0x3447E4), r, heapown::kProcess);
        f.SetParent(f.Panel(heapown::kProcess), r, heapown::kProcess);
        for (int k = 1; k < players; k++) f.SetParent(f.Panel(k), r, k);
        const heapown::Block* buf = f.b.At(f.R(r + kPanelChildren + 8));
        Check(f.foreign == 0 && buf && buf->owner == heapown::kProcess &&
                  f.process_grows == (players == 4 ? 1u : 0u),
              players == 3 ? "three players: four children, no grow" : "four players: one grow, as the process");
    }
}

// docs 15.23: player 1's memory over its departures, the rejoin's bound, the timed leave once
static void Player1Memory() {
    using namespace nsplit;
    Player1Retention r;
    Check(r.Growth() == 0 && r.NextCycle() == 0, "no departure: no growth");
    r.Departed(425755);
    Check(r.baseline == 425755 && r.Growth() == 0 && r.NextCycle() == 0, "first departure: the baseline, no growth");
    r.Departed(539834);
    Check(r.Growth() == 539834 - 425755 && r.NextCycle() == 539834 - 425755, "run 16's second departure: its growth");
    r.Departed(636210);
    Check(r.Growth() == 636210 - 425755 && r.NextCycle() == 636210 - 539834, "third: growth over the first, cycle over the last");
    r.Departed(600000);
    Check(r.Growth() == 600000 - 425755 && r.NextCycle() == 0, "the game freed some: growth falls, no cycle growth");
    r.Departed(100);
    Check(r.Growth() == 0, "below the baseline: no growth (never negative)");

    LeakBudget b;
    Check(b.MayJoin() && b.MayRejoin(0), "empty budget: join and rejoin");
    b.dormant = b.limit;
    Check(b.MayJoin() && !b.MayRejoin(1), "growth at the bound: a join may, a rejoin adding more may not");
    b.dormant = b.limit + 1;
    Check(!b.MayJoin(), "growth above the bound closes joins");
    b.dormant = 0;
    b.Left(size_t(5) << 20);
    Check(b.MayRejoin(size_t(3) << 20) && !b.MayRejoin((size_t(3) << 20) + 1), "leaves and the next cycle summed");
    b.dormant = size_t(2) << 20;
    Check(b.MayJoin() && b.MayRejoin(size_t(1) << 20) && !b.MayRejoin((size_t(1) << 20) + 1), "leaves + growth + cycle within the bound");
    Check(b.limit == size_t(8) << 20, "the bound stays 8 MiB");
    // simulated cycles at run 16's rate: a rejoin is refused before the bound would be passed, never after
    Player1Retention c;
    LeakBudget d;
    size_t held = 425755;
    int rejoins = 0;
    for (int i = 0; i < 1000; i++) {
        c.Departed(held);
        d.dormant = c.Growth();
        if (!d.MayRejoin(c.NextCycle())) break;
        rejoins++;
        held += 100000;
    }
    Check(d.outstanding + d.dormant <= d.limit && rejoins > 50 && rejoins < 100,
          "at 100 KB a cycle about 80 rejoins, then refused within the bound");

    Check(DiagLeaveDue(0, 1) && !DiagLeaveDue(1, 1), "a timed leave once by default");
    Check(DiagLeaveDue(1, 2) && !DiagLeaveDue(2, 2), "twice when asked");
    Check(DiagLeaveDue(0, 0) && DiagLeaveDue(100, 0), "0: after every join (the repetition asked for)");

    std::map<uint32_t, SiteSum> m;
    m[0x100] = {0x100, 2, 50};
    m[0x200] = {0x200, 1, 900};
    m[0x300] = {0x300, 9, 900};
    m[0x50] = {0x50, 1, 10};
    std::vector<SiteSum> top = TopSites(m, 3);
    Check(top.size() == 3 && top[0].site == 0x200 && top[1].site == 0x300 && top[2].site == 0x100,
          "sites by bytes, ties by address, the first n");
    Check(TopSites({}, 8).empty(), "no sites");
}

int main() {
    Player1Memory();
    Player1Rejoin();
    LinkCycles();
    LiveRetention();
    PoolDryRunRules();
    Routing();
    Queues();
    OnceAndPerPlayer();
    Shutdowns();
    Departure();
    Watch();
    Sessions();
    SameFrameReconnect();
    CleanupRules();
    CleanupCalls();
    RepeatedLeaves();
    SharedParentRules();
    SharedParentCycles();
    Voice();
    FocusAndLinks();
    RetentionRules();
    ParticlePool();
    DestructorSteps();
    GrowLeftBehind();
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
