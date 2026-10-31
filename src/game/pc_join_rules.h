// Local players joining from the menus (src/game/pc_party.cpp's join popup, src/linux/compositor.cpp's PartyJoin,
// src/game/pc_instant.cpp's Start Match; tested on its own by tests/pc_join_rules_test.py). No game memory.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace join {

inline constexpr int kMaxLocal = 4;    // local players: player 1 and up to three more
inline constexpr int kSlots = 65;      // the match's player slots (G8.2B: 64 bots and a player, or fewer bots)

// a free controller's press: whether it joins. On the party's own screens A or START (as before); on any other menu
// screen START only (A there is the menus'); in a match (or no menu) never
enum class Where { Match, Menus, PartyScreen };
inline bool Joins(Where where, bool start, bool a) {
    if (where == Where::PartyScreen) return start || a;
    if (where == Where::Menus) return start;
    return false;
}

// whether another player can join: the local players so far (1..4)
inline bool Room(int players) { return players < kMaxLocal; }

// the most bots a match can have beside its local players, and a bot count held to it
inline int MaxBots(int players) { return std::max(0, kSlots - std::clamp(players, 1, kMaxLocal)); }
inline int ClampBots(int bots, int players) { return std::clamp(bots, 0, MaxBots(players)); }

// the join popup's rows for a member: the saved profiles (each marked with the player who has it, 0 free), then
// "New Profile..." and "Cancel"; at most kPopupProfiles profiles (the rest through New Profile's Profiles screen)
inline constexpr int kPopupProfiles = 6;
struct Row {
    enum Kind { Profile, New, Cancel, Ok } kind;
    std::string id, name;
    int taken_by = 0;  // another player's profile: shown, not chosen
};
struct Profile {
    std::string id, name;
};
inline std::vector<Row> PopupRows(const std::vector<Profile>& profiles, const std::vector<std::string>& player_profiles,
                                  int member) {
    std::vector<Row> rows;
    for (const Profile& p : profiles) {
        if (int(rows.size()) >= kPopupProfiles) break;
        int by = 0;
        for (size_t i = 0; i < player_profiles.size(); i++)
            if (int(i) + 1 != member && !p.id.empty() && player_profiles[i] == p.id) by = int(i) + 1;
        rows.push_back({Row::Profile, p.id, p.name, by});
    }
    rows.push_back({Row::New, "", "New Profile...", 0});
    rows.push_back({Row::Cancel, "", "Cancel", 0});
    return rows;
}
// the popup when every slot is taken: its message's single row
inline std::vector<Row> FullRows() { return {{Row::Ok, "", "OK", 0}}; }

// the popup's selection held to its rows (its table has a fixed number of rows; the rest are not entries)
inline int Selection(int row, int rows) { return rows <= 0 ? 0 : std::clamp(row, 0, rows - 1); }

// what A on a row does
enum class Pick { Chosen, TakenByOther, NewProfile, Cancelled, Closed };
inline Pick Choose(const std::vector<Row>& rows, int row) {
    if (row < 0 || row >= int(rows.size())) return Pick::Closed;
    switch (rows[size_t(row)].kind) {
        case Row::Profile: return rows[size_t(row)].taken_by ? Pick::TakenByOther : Pick::Chosen;
        case Row::New: return Pick::NewProfile;
        case Row::Cancel: return Pick::Cancelled;
        case Row::Ok: return Pick::Closed;
    }
    return Pick::Closed;
}

// the local players a match starts with: player 1 and every member after it (the compositor keeps them in order)
inline int LocalPlayers(const std::vector<bool>& members) {
    int n = 1;
    for (size_t i = 1; i < members.size() && i < size_t(kMaxLocal); i++)
        if (members[i]) n = int(i) + 1;
    return n;
}

// Instant Action's status line for a slot (1..4)
inline std::string SlotLine(int slot, bool in, const std::string& name) {
    const std::string p = "P" + std::to_string(slot) + "  ";
    if (!in) return p + "Press START to join";
    return p + (name.empty() ? std::string("Choosing a profile...") : name);
}

}  // namespace join
