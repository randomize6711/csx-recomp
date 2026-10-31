// Local players joining Instant Action (src/game/pc_party.cpp, src/linux/compositor.cpp's PartyJoin,
// src/game/pc_instant.cpp's panel and Start Match; tested on its own by tests/pc_join_rules_test.py). No game memory.
// No popup: a free controller's START on Instant Action takes the next slot at once, with the profile that slot had
// last when it is free, else as a guest; the panel shows it.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace join {

inline constexpr int kMaxLocal = 4;    // local players: player 1 and up to three more
inline constexpr int kSlots = 65;      // the match's player slots (G8.2B: 64 bots and a player, or fewer bots)

// a free controller's press: whether it joins. On Instant Action START (A there is the menus'); on the party's own
// screens A or START (as before, Host Game's party); anywhere else (other menus, a match) never
enum class Where { Elsewhere, InstantAction, PartyScreen };
inline bool Joins(Where where, bool start, bool a) {
    if (where == Where::PartyScreen) return start || a;
    if (where == Where::InstantAction) return start;
    return false;
}

// whether another player can join: the local players so far (1..4)
inline bool Room(int players) { return players < kMaxLocal; }

// the profile a joining slot plays as: the one that slot had last (remembered), when it exists and no other player
// has it; "" otherwise (a guest: no saved profile is touched)
inline std::string SlotProfile(const std::string& remembered, bool exists, const std::vector<std::string>& others) {
    if (remembered.empty() || !exists) return {};
    for (const std::string& o : others)
        if (o == remembered) return {};
    return remembered;
}
inline std::string GuestName(int slot) { return "Guest " + std::to_string(slot); }

// the most bots a match can have beside its local players, and a bot count held to it
inline int MaxBots(int players) { return std::max(0, kSlots - std::clamp(players, 1, kMaxLocal)); }
inline int ClampBots(int bots, int players) { return std::clamp(bots, 0, MaxBots(players)); }

// the local players a match starts with: player 1 and every member after it (the compositor keeps them in order)
inline int LocalPlayers(const std::vector<bool>& members) {
    int n = 1;
    for (size_t i = 1; i < members.size() && i < size_t(kMaxLocal); i++)
        if (members[i]) n = int(i) + 1;
    return n;
}

// Instant Action's Local Players line for a slot (1..4): its player and profile, or how to join
inline std::string SlotLine(int slot, bool in, const std::string& name, bool controller_lost = false) {
    const std::string p = "P" + std::to_string(slot) + "  -  ";
    if (!in) return p + "Press Start to Join";
    if (controller_lost) return p + "Controller disconnected";
    return p + "Ready (" + (name.empty() ? GuestName(slot) : name) + ")";
}

}  // namespace join
