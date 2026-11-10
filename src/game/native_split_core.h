// Native split-screen's local players without the game (docs/native_splitscreen.md, M2): the slots, each player's
// state through its life, and what a player's initialisation may register. Tested on its own by
// tests/native_split_test.py; src/game/native_split.cpp drives it from the game.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace nsplit {

constexpr int kMaxPlayers = 4;

enum class State {
    Empty,         // no player in this slot
    Initialising,  // its context made, its client's own initialisation running
    Connecting,    // initialised; its client joins the built-in server on its own turn
    Active,        // in the match
    Leaving,       // its server slot dropped, its objects being released
    Departed,      // slot 0 only: the game's own player left while others play (docs 15.12); its context stays the
                   // switcher's first, its client disconnected; the title's own player again when the session ends
};

inline const char* Name(State s) {
    switch (s) {
    case State::Empty: return "empty";
    case State::Initialising: return "initialising";
    case State::Connecting: return "connecting";
    case State::Active: return "active";
    case State::Leaving: return "leaving";
    case State::Departed: return "departed";
    }
    return "?";
}

struct Player {
    State state = State::Empty;
    int context = -1;     // its client context (src/game/client_context.cpp)
    std::string name;     // its userinfo name
};

// the slots: player 0 is the game's own (its context 0, always there); 1..3 the added ones
class Players {
public:
    Players() {
        p_[0] = {State::Active, 0, ""};
    }

    const Player& At(int k) const { return p_[size_t(k)]; }
    int Count() const {
        int n = 0;
        for (const Player& x : p_) n += x.state != State::Empty;
        return n;
    }
    // the players in the match or joining it (Connecting / Active): who keeps the session alive
    int Playing() const {
        int n = 0;
        for (const Player& x : p_) n += x.state == State::Connecting || x.state == State::Active;
        return n;
    }
    // the slot a new player takes (the lowest empty), -1 when full
    int FreeSlot() const {
        for (int k = 1; k < kMaxPlayers; k++)
            if (p_[size_t(k)].state == State::Empty) return k;
        return -1;
    }
    // the transitions; false (and nothing changed) when not allowed from the current state
    bool Begin(int k, int context, const std::string& name) {
        if (k <= 0 || k >= kMaxPlayers || p_[size_t(k)].state != State::Empty || context <= 0) return false;
        p_[size_t(k)] = {State::Initialising, context, name};
        initialising_ = k;
        return true;
    }
    bool Initialised(int k) { return Step(k, State::Initialising, State::Connecting, true); }
    // its initialisation failed its checks: the slot empty again (the caller removes its context), never connecting
    bool InitFailed(int k) {
        if (!Step(k, State::Initialising, State::Empty, true)) return false;
        p_[size_t(k)] = {};
        return true;
    }
    bool Joined(int k) { return Step(k, State::Connecting, State::Active, false); }
    bool Leave(int k) {
        if (k <= 0 || k >= kMaxPlayers) return false;
        State s = p_[size_t(k)].state;
        if (s != State::Connecting && s != State::Active) return false;
        p_[size_t(k)].state = State::Leaving;
        return true;
    }
    bool Released(int k) {
        if (!Step(k, State::Leaving, State::Empty, false)) return false;
        p_[size_t(k)] = {};
        return true;
    }
    // the game's own player (slot 0, context 0) leaves while another plays: Departed (its context kept); false when it
    // is not active. Back to Active when the session ends (the title's own player at its menus again)
    bool Depart0() {
        if (p_[0].state != State::Active) return false;
        p_[0].state = State::Departed;
        return true;
    }
    bool Return0() {
        if (p_[0].state != State::Departed) return false;
        p_[0].state = State::Active;
        return true;
    }
    // a context removed: the players whose contexts came after it move down one (client_context's Remove)
    void ContextRemoved(int context) {
        for (Player& x : p_)
            if (x.state != State::Empty && x.context > context) x.context--;
    }
    // the player whose initialisation is running (its registrations are the first player's: not repeated), -1 none
    int Initialising() const { return initialising_; }
    // a slot's player by its loopback port (the port is the slot)
    int ByPort(int port) const {
        return port >= 0 && port < kMaxPlayers && p_[size_t(port)].state != State::Empty ? port : -1;
    }

private:
    bool Step(int k, State from, State to, bool end_init) {
        if (k <= 0 || k >= kMaxPlayers || p_[size_t(k)].state != from) return false;
        p_[size_t(k)].state = to;
        if (end_init && initialising_ == k) initialising_ = -1;
        return true;
    }
    std::array<Player, kMaxPlayers> p_{};
    int initialising_ = -1;
};

// a new player's initialisation, in order (native_split.cpp AddPlayer runs these; docs 13.2)
enum class InitStep {
    BootImage,      // its context from the boot image (no heap pointer: checked)
    ClientState,    // CL_Init's per-player state (list heads, the datagram buffer, cls.state), the tempent free list
    Userinfo,       // player 1's userinfo text, then its own name
    Initialize,     // the client game's Initialize (gEngfuncs, its particle manager)
    HudInit,        // the client game's HUD_Init (its HUD and objects; registrations answered by the first player's)
    Check,          // against player 1's initialised state: every object its own or shared, none missing
};
inline std::array<InitStep, 6> InitOrder() {
    return {InitStep::BootImage, InitStep::ClientState, InitStep::Userinfo, InitStep::Initialize, InitStep::HudInit,
            InitStep::Check};
}

// svc_deltadescription's registry slot for a player: player 1 (and the first registration of a name) the shared slot;
// a player after the first, once the name is registered, its own slot (it reads the message, the registry stays)
inline uint32_t DeltaSlot(uint32_t shared_slot, uint32_t shared_value, int player, uint32_t own_slots, uint32_t elem) {
    if (player <= 0 || !shared_slot || !shared_value) return shared_slot;
    return own_slots + uint32_t(player) * elem;
}

// what the client's initialisation registers, while another player's initialisation runs (the first player's
// registrations stand: the commands, cvars and message handlers map names to the same functions for every player)
enum class Registration { Register, ReuseExisting, Skip };
inline Registration RegisterPolicy(bool another_players_init, bool name_exists) {
    if (!another_players_init) return Registration::Register;   // the game's own initialisation: as the title
    return name_exists ? Registration::ReuseExisting : Registration::Register;  // a name only this player's code has
}
// the engine's Cmd_AddCommand (0x3C760) is a fatal Sys_Error after host_initialized: never called again
inline Registration EngineCommandPolicy(bool another_players_init) {
    return another_players_init ? Registration::Skip : Registration::Register;
}

// ---- the process's one-time start-up work (docs 15.7) --------------------------------------------------------------

// A lazily initialised process constant a new player's steps would otherwise set first, in that player's
// initialisation, against the protected start-up state: the timer 0xFC870's function-local static (its guard bit
// 0x5C9660, under it 1 / the performance counter's frequency at 0x5C9640 and the timer's fields cleared). The process
// runs the original guarded block once, at the end of Host_Init, before the initial state is kept and before any
// player is added; a player's later call finds the guard set and skips it, as the title's second call does.
enum class Once { Pending, Ran, AlreadyDone, Failed };

inline const char* Name(Once s) {
    switch (s) {
    case Once::Pending: return "pending";
    case Once::Ran: return "ran";
    case Once::AlreadyDone: return "already done";
    case Once::Failed: return "failed";
    }
    return "?";
}

// what the timer's one-time block left: its guard bit set and a period (a float, its bits) not 0. Before: the guard
// already set means another call did it (nothing run); after a run, a guard still clear or no period is a failure
inline Once TimerOnce(uint32_t guard_before, bool ran, uint32_t guard_after, uint32_t period_after) {
    if (guard_before & 1) return Once::AlreadyDone;
    if (!ran) return Once::Pending;
    return (guard_after & 1) && period_after ? Once::Ran : Once::Failed;
}

// a player may be added only once the process's one-time work is done (fail closed: pending or failed, none is)
inline bool MayAddPlayers(Once timer) { return timer == Once::Ran || timer == Once::AlreadyDone; }

// ---- the session's lifecycle (docs 15) ---------------------------------------------------------------------------

// the built-in server and its world are the session's, not a client's: they end only when the session's host leaves
// (the local player who started the server: its window, menus and console start and end matches; player 1 in M2, the
// only one with them). Another local player's client disconnecting (a kick, the server's disconnect message, a
// stuffed "disconnect") leaves the server running for the others.
struct Session {
    int host = 0;  // the local player whose disconnect ends the match (the server's starter)
    bool DisconnectEndsServer(int player) const { return player == host; }
};

// another local player than `k` keeps the session alive (joining or in the match)
inline bool OthersRemain(const Players& p, int k) {
    for (int j = 0; j < kMaxPlayers; j++)
        if (j != k && (p.At(j).state == State::Connecting || p.At(j).state == State::Active)) return true;
    return false;
}

// a request to shut the built-in server down (Host_ShutdownServer 0x5C9D0: the disconnect command, Host_EndGame,
// Host_Error, a gamedir mismatch, a map change), decided by where it is made (docs 15.12):
//  - outside the players' turns: the process's (the console, the menus, a map change, the host's frame) - it goes on,
//    the other local players removed first (TeardownOrder);
//  - in a player's turn while another local player plays: refused - that player's client leaves alone (its own
//    CL_Disconnect: its server slot dropped by its "dropclient"), the server and world stay for the others;
//  - in player 1's turn, alone: the title's own (it goes on);
//  - in another player's turn, the last one playing: deferred - the session ends as the process at the frame's end,
//    from player 1's context (the switcher's first), after that player is removed: never inside a player's context
//    the world is freed in; an unknown player's turn is refused (fail closed)
enum class Shutdown { Proceed, Refuse, Defer };
inline const char* Name(Shutdown s) {
    switch (s) {
    case Shutdown::Proceed: return "goes on";
    case Shutdown::Refuse: return "refused";
    case Shutdown::Defer: return "deferred to the frame's end";
    }
    return "?";
}
inline Shutdown ShutdownDecision(bool in_turn, int acting, const Players& p) {
    if (!in_turn) return Shutdown::Proceed;
    if (acting < 0 || acting >= kMaxPlayers) return Shutdown::Refuse;
    if (OthersRemain(p, acting)) return Shutdown::Refuse;
    return acting == 0 ? Shutdown::Proceed : Shutdown::Defer;
}
// a client's own disconnect (CL_Disconnect 0x28F20) in its turn while another local player plays: the shared parts it
// would end (the server, the one sound system's sounds, the renderer's list, the LostHost screen) are kept
inline bool LeaveKeepsShared(bool in_turn, int acting, const Players& p) { return in_turn && OthersRemain(p, acting); }

// a player's client watched between frames: cls.state's highest value seen since it joined; dropped back to
// disconnected (1) or below after it was connecting (2) or more: its client left (a kick, the server's disconnect, its
// own disconnect command, a connection that failed or timed out)
constexpr uint32_t kClsStateDisconnected = 1, kClsStateConnecting = 2;
struct ClientWatch {
    uint32_t best = 0;
    // the state seen now; true once, when the client has left
    bool Dropped(uint32_t state) {
        if (state > best) best = state;
        if (best >= kClsStateConnecting && state <= kClsStateDisconnected) {
            best = 0;
            return true;
        }
        return false;
    }
};

// the local player whose leaving ends the server (StartsModuleValidation; the window's): the lowest one playing,
// player 1 while it plays
inline int SessionHost(const Players& p) {
    for (int k = 0; k < kMaxPlayers; k++)
        if (p.At(k).state == State::Connecting || p.At(k).state == State::Active) return k;
    return 0;
}

// before the server shuts down (its world, hunk and models freed): the other local players removed first, the highest
// slot first, so none is left pointing into a freed world. Only from the host's own context (the switcher's state is
// the host's live one); in another player's turn nothing is removed (the caller reports it)
inline std::array<int, kMaxPlayers> TeardownOrder(const Players& p, int host, int& n) {
    std::array<int, kMaxPlayers> out{};
    n = 0;
    for (int k = kMaxPlayers - 1; k >= 0; k--) {
        if (k == host) continue;
        State s = p.At(k).state;
        if (s == State::Connecting || s == State::Active) out[size_t(n++)] = k;
    }
    return out;
}

// the secure-module validation (0x4E50B0.., one machine for the process: the server's module download and every
// client's validation share it; dormant unless the server is secure): only the session's host starts it. An added
// player's server is this process, the same executable, so its own request would only reset the shared machine
// (player 1's validation, or a server download in progress)
inline bool StartsModuleValidation(const Session& s, int player) { return player == s.host; }

// whose allocation (or free) this is: a player's slot, or -1 the process. The shared front end's scope and a
// shared-once operation are the process's whoever runs them; then the player initialising; then the player whose turn
// it is; else (between the turns: the server, the console, the front end's own frame) the process
inline int AllocationOwner(bool ui_scope, bool world_running, int init_owner, bool in_turn, int turn_player) {
    if (ui_scope || world_running) return -1;
    if (init_owner > 0) return init_owner;
    if (in_turn) return turn_player >= 0 ? turn_player : -1;
    return -1;
}

// ---- a player's activation (docs 15.10) ---------------------------------------------------------------------------

// the activation check 0x269A0 (_Host_Frame's call at 0x5D3A6, its only caller): cls.state 3 (connected) or 4 (after
// the signon's 0x2D800) with cls.signon [0x784800] == 2 (the signon complete) becomes 5 (active); any other state or an
// incomplete signon is left as it is (0x269A5..0x269BE). The game runs the real function; this is its rule, which
// tests/region_model_test.py checks against the generated code
constexpr uint32_t kClsConnected = 3, kClsSignonDone = 4, kClsActiveState = 5, kSignonsComplete = 2;
inline uint32_t Activation(uint32_t state, uint32_t signon) {
    return (state == kClsConnected || state == kClsSignonDone) && signon == kSignonsComplete ? kClsActiveState : state;
}

// _Host_Frame's per-player prelude: 0x269A0 and the client's CL_UpdateClientData 0x1CC10 (HUD_UpdateClientData: the
// client game's view angles and field of view, only in state 5) run at 0x5D3A6 / 0x5D3B0, before the players' turns,
// so in player 1's context only. Player 1 (context 0) keeps that original call; every other context runs both, in that
// order, once a frame, at the start of its own input turn (0x5D3B5): after its previous frame's packets, before its
// CL_Move, as player 1's are. Never twice for one context: context 0's turn start runs nothing
inline bool PreludeInTurn(int context) { return context > 0; }

// ---- the shared front end (docs 15.10) ----------------------------------------------------------------------------

// The front end (the Xbox UI's screens: the scoreboard, the team and buy menus) is one for the process until M4: one
// screen list (0x5523D0), one command queue. A client game reaches it from its own turn: its scoreboard's rows are the
// shared "Scoreboard" screen's list (0x1B91E0 takes it by name, 0x1B9820 frees and rebuilds its rows) and its menus are
// "xbui" commands queued to the shared console (0xCA6F0). One local player drives it: player 1 while it is in the
// session, then the lowest remaining player (the shared UI never depends on one player's lifetime). The driver's turn
// runs the maintenance as the process (its elements are the process's, not the driver's); another player's turn does
// not run it at all; outside the players' turns it runs as the title's
enum class SharedUi { AsTitle, AsShared, Withheld };
inline const char* Name(SharedUi s) {
    switch (s) {
        case SharedUi::AsTitle: return "as the title";
        case SharedUi::AsShared: return "as the process, by its driver";
        case SharedUi::Withheld: return "withheld";
    }
    return "?";
}
struct SharedUiOwner {
    int driver = 0;  // the local player (slot) whose turns maintain the shared front end
    SharedUi Decide(bool in_turn, int acting) const {
        if (!in_turn) return SharedUi::AsTitle;  // the process's own frame: the front end's code as before
        return acting == driver ? SharedUi::AsShared : SharedUi::Withheld;  // a turn of no known player: withheld too
    }
    // player `k` leaves (before its slot is released): the driver handed to the lowest remaining active player, else
    // the lowest connecting one, else back to slot 0. The new driver
    int Left(int k, const Players& p) {
        if (k != driver) return driver;
        int best = -1;
        for (State want : {State::Active, State::Connecting})
            for (int j = 0; j < kMaxPlayers && best < 0; j++)
                if (j != k && p.At(j).state == want) best = j;
        driver = best >= 0 ? best : 0;
        return driver;
    }
};

}  // namespace nsplit
