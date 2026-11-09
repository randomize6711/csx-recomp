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

}  // namespace nsplit
