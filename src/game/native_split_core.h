// Native split-screen's local players without the game (docs/native_splitscreen.md, M2): the slots, each player's
// state through its life, and what a player's initialisation may register. Tested on its own by
// tests/native_split_test.py; src/game/native_split.cpp drives it from the game.
#pragma once

#include <array>
#include <string>

namespace nsplit {

constexpr int kMaxPlayers = 4;

enum class State {
    Empty,         // no player in this slot
    Initialising,  // its context made, its client's own initialisation running
    Connecting,    // initialised; its client joins the built-in server on its own turn
    Active,        // in the match
    Leaving,       // its server slot dropped, its objects being released
};

inline const char* Name(State s) {
    switch (s) {
    case State::Empty: return "empty";
    case State::Initialising: return "initialising";
    case State::Connecting: return "connecting";
    case State::Active: return "active";
    case State::Leaving: return "leaving";
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

}  // namespace nsplit
