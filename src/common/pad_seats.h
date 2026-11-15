// Native split-screen's controller seats (docs/native_splitscreen.md 18.3), the part without SDL or the game: which
// controller is which local player's. Seat k is player k+1's and reads console port k; seat 0 (player 1) also has the
// keyboard and mouse. A seat holds a controller from the START that took it until the player leaves: a controller
// that disconnects leaves its seat waiting (the port stays connected with neutral input, so the title sees no
// removal and the player stays in the match), and the same controller coming back, or else the next free
// controller's START, takes the waiting seat again before any new seat is given. A controller is known by a key the
// window makes stable across reconnects and process starts (its device path, else its GUID and serial; a running
// number keeps two identical controllers apart).
//
// Tested on its own by tests/native_split_input_test.py (no SDL, no game).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace seats {

constexpr int kSeats = 4;

enum class SeatState : uint8_t {
    Empty,    // nobody's
    Held,     // a player's, its controller connected (or seat 0 with the keyboard and mouse only)
    Waiting,  // a player's, its controller disconnected: neutral input until it (or another) comes back
};

inline const char* Name(SeatState s) {
    switch (s) {
        case SeatState::Empty: return "empty";
        case SeatState::Held: return "held";
        case SeatState::Waiting: return "waiting for its controller";
    }
    return "?";
}

struct Seat {
    SeatState state = SeatState::Empty;
    uint32_t device = 0;  // the controller's id while connected (0: none)
    std::string key;      // the controller it holds (kept while it waits); "" for the keyboard-only seat 0
};

// what a controller's START did
enum class Start : uint8_t {
    Own,      // its own seat's START: the player's own button (the game's pause), nothing here
    Resumed,  // took back a seat waiting for a controller (the lowest)
    Joined,   // took the next empty seat (a new player)
    Refused,  // no seat to take (joining closed, or every seat held)
};

inline const char* Name(Start s) {
    switch (s) {
        case Start::Own: return "its own seat's";
        case Start::Resumed: return "a waiting seat taken back";
        case Start::Joined: return "joined a new seat";
        case Start::Refused: return "refused";
    }
    return "?";
}

class Table {
public:
    // seat 0 is always player 1's (the keyboard and mouse), with or without a controller
    Table() { seats_[0].state = SeatState::Held; }

    const Seat& At(int k) const { return seats_[size_t(k)]; }
    int SeatOf(uint32_t device) const {
        if (!device) return -1;
        for (int k = 0; k < kSeats; k++)
            if (seats_[size_t(k)].device == device) return k;
        return -1;
    }
    // the controller seat k reads (0: none: neutral input on a held or waiting seat)
    uint32_t DeviceOf(int k) const { return k >= 0 && k < kSeats ? seats_[size_t(k)].device : 0; }
    // the players: seat k in the game (held or waiting)
    bool InGame(int k) const { return k >= 0 && k < kSeats && seats_[size_t(k)].state != SeatState::Empty; }
    int Players() const {
        int n = 0;
        for (int k = 0; k < kSeats; k++) n += InGame(k);
        return n;
    }
    // bit k: seat k is in the game
    unsigned Mask() const {
        unsigned m = 0;
        for (int k = 0; k < kSeats; k++)
            if (InGame(k)) m |= 1u << k;
        return m;
    }

    // a controller connected: given back to the seat waiting for this key (its seat, -1 none). Another free controller
    // is only listed: it takes a seat with its START
    int Connected(uint32_t device, const std::string& key) {
        Forget(device);
        pads_.push_back({device, key});
        for (int k = 0; k < kSeats; k++) {
            Seat& s = seats_[size_t(k)];
            const bool waits = s.state == SeatState::Waiting || (k == 0 && !s.device);
            if (waits && !key.empty() && s.key == key) {
                Take(k, device);
                return k;
            }
        }
        return -1;
    }

    // a controller disconnected: its seat (-1 none) waits for it, keeping the player in the game
    int Disconnected(uint32_t device) {
        Forget(device);
        int k = SeatOf(device);
        if (k < 0) return -1;
        Seat& s = seats_[size_t(k)];
        s.device = 0;
        // player 1 keeps playing on the keyboard and mouse (its controller's key kept for its return)
        s.state = k == 0 ? SeatState::Held : SeatState::Waiting;
        return k;
    }

    // a controller's START. `join_open`: a new seat may be given (Instant Action, or a match with room); `limit` the
    // seats a match may have (its layout's players). A waiting seat is taken back first, whatever join_open says
    Start Pressed(uint32_t device, bool join_open, int limit, int* seat_out = nullptr) {
        int own = SeatOf(device);
        if (seat_out) *seat_out = own;
        if (own >= 0) return Start::Own;
        if (!IsConnected(device)) return Start::Refused;  // not a connected controller
        for (int k = 0; k < kSeats; k++) {
            if (seats_[size_t(k)].state != SeatState::Waiting) continue;
            Take(k, device);
            if (seat_out) *seat_out = k;
            return Start::Resumed;
        }
        if (!join_open) return Start::Refused;
        for (int k = 1; k < std::min(limit, kSeats); k++) {
            if (seats_[size_t(k)].state != SeatState::Empty) continue;
            Take(k, device);
            if (seat_out) *seat_out = k;
            return Start::Joined;
        }
        return Start::Refused;
    }

    // seat 0's own controller (Split Screen > Player 1 > Also a Controller): the first free one. -1 none taken
    int GiveP1Controller() {
        if (seats_[0].device) return 0;
        for (const Pad& f : pads_)
            if (SeatOf(f.device) < 0) {
                Take(0, f.device);
                return 0;
            }
        return -1;
    }

    // a player left (its pause menu, BACK in the party, the match's end for it): its seat empty, its controller free.
    // Seat 0 keeps the keyboard and mouse and lets its controller go
    void Leave(int k) {
        if (k < 0 || k >= kSeats) return;
        Seat& s = seats_[size_t(k)];
        s.device = 0;
        s.key.clear();
        s.state = k == 0 ? SeatState::Held : SeatState::Empty;
    }

    // the seats across a process start (the match's relaunch): "k=key" for every seat with a controller, ';' between.
    // Keys are the window's (printable, no ';' or '=')
    std::string Save() const {
        std::string out;
        for (int k = 0; k < kSeats; k++) {
            const Seat& s = seats_[size_t(k)];
            if (s.state == SeatState::Empty) continue;
            if (k == 0 && s.key.empty()) continue;
            if (!out.empty()) out += ';';
            out += std::to_string(k) + "=" + s.key;
        }
        return out;
    }
    // restored at the start: every listed seat waits for its controller (the window's connection events give them
    // back). Seats 1..3 named with an empty key wait for any START. Returns the seats restored
    int Load(const std::string& text) {
        int n = 0;
        size_t i = 0;
        while (i < text.size()) {
            size_t j = text.find(';', i);
            if (j == std::string::npos) j = text.size();
            std::string item = text.substr(i, j - i);
            i = j + 1;
            size_t eq = item.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            char* end = nullptr;
            long k = std::strtol(item.c_str(), &end, 10);
            if (end != item.c_str() + eq || k < 0 || k >= kSeats) continue;
            Seat& s = seats_[size_t(k)];
            s.key = item.substr(eq + 1);
            s.device = 0;
            s.state = SeatState::Waiting;
            n++;
        }
        return n;
    }

    // the connected controllers, in connection order (held by a seat or not)
    struct Pad {
        uint32_t device;
        std::string key;
    };
    const std::vector<Pad>& Connected() const { return pads_; }
    bool IsConnected(uint32_t device) const {
        for (const Pad& f : pads_)
            if (f.device == device) return true;
        return false;
    }
    // a connected controller no seat holds (the next to join), in connection order; 0 none
    uint32_t FirstUnseated() const {
        for (const Pad& f : pads_)
            if (SeatOf(f.device) < 0) return f.device;
        return 0;
    }

private:
    void Take(int k, uint32_t device) {
        Seat& s = seats_[size_t(k)];
        std::string key;
        for (const Pad& f : pads_)
            if (f.device == device) key = f.key;
        s.device = device;
        s.key = key;
        s.state = SeatState::Held;
    }
    void Forget(uint32_t device) {
        for (size_t i = 0; i < pads_.size(); i++)
            if (pads_[i].device == device) {
                pads_.erase(pads_.begin() + long(i));
                return;
            }
    }

    std::array<Seat, kSeats> seats_{};
    std::vector<Pad> pads_;  // every connected controller (held or not), in connection order
};

}  // namespace seats
