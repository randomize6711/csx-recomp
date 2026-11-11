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

int main() {
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
    Voice();
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
