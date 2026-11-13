// Native split-screen M3.2 (docs/native_splitscreen.md 17): development-only automatic team and class choice for the
// local players, through the game's own menu commands, sent by each player's own client. Pure (no guest memory):
// tests/native_split_view_test.cpp.
//
// The server's menu state (the game DLL's ClientCommand "menuselect", 0x13FB5E, P): a player's menu at CBasePlayer
// +0x364 (1 the team menu a player is put in when it enters, 2 the in-game team menu, 3 the class menu), its team at
// +0x1E0 (0 none, 1 Terrorist, 2 Counter-Terrorist, 3 spectator). "menuselect N" in the team menu (HandleMenu_ChooseTeam
// 0x13DB30; its table 0x13E2F8): 1 Terrorist, 2 Counter-Terrorist, 5 auto-assign, 6 spectate; in the class menu
// (HandleMenu_ChooseAppearance 0x13D8E0): 1..4 a model, above 4 a random one (the game's RANDOM_LONG(1, 4)), after
// which the player spawns as the round's rules allow. Nothing here writes the server's state: the player's own console
// line, run in its own turn, goes to the server as any client's unknown command does (forwarded), and the game's own
// rules decide (a full team refuses, a round in progress waits).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>

namespace nsplit {

// what the server's player shows
struct ServerPlayer {
    bool known = false;  // a server client for this player with a player object
    uint32_t team = 0, menu = 0;
    float health = 0;
    uint32_t deadflag = 0, weapons = 0;
};
constexpr uint32_t kMenuChooseTeam = 1, kMenuInGameTeam = 2, kMenuChooseClass = 3;

// a player's configured choice: team slot (1 T, 2 CT, 5 auto, 6 spectate; 0 none: not automatic), class slot (1..4,
// 5 random)
struct JoinChoice {
    int team = 0, cls = 5;
    bool On() const { return team == 1 || team == 2 || team == 5 || team == 6; }
};

// "T,CT" lists per player, e.g. CSX_NATIVE_SPLIT_TEAMS="0,5": player 1 by its own menus, player 2 auto-assigned; a
// value not in the team menu's slots is 0 (not automatic)
inline int ListValue(const char* list, int index, int fallback) {
    if (!list) return fallback;
    int i = 0;
    const char* p = list;
    while (*p) {
        if (i == index) {
            char* end = nullptr;
            long v = std::strtol(p, &end, 10);
            return end != p ? int(v) : fallback;
        }
        if (*p == ',') i++;
        p++;
    }
    return fallback;
}
inline JoinChoice ChoiceOf(const char* teams, const char* classes, int player) {
    JoinChoice c;
    c.team = ListValue(teams, player, 0);
    if (!(c.team == 1 || c.team == 2 || c.team == 5 || c.team == 6)) c.team = 0;
    c.cls = ListValue(classes, player, 5);
    if (c.cls < 1 || c.cls > 5) c.cls = 5;
    return c;
}

// the next line a player's own client sends (empty: nothing now). `sent_*` the times the line went (frames since);
// a line goes again after `retry` frames without the server's state moving on, at most `tries` times each
enum class JoinStep { Off, NoServerPlayer, Team, Class, Done, Waiting, GaveUp };
inline const char* Name(JoinStep s) {
    switch (s) {
    case JoinStep::Off: return "not automatic";
    case JoinStep::NoServerPlayer: return "no server player yet";
    case JoinStep::Team: return "its team chosen";
    case JoinStep::Class: return "its class chosen";
    case JoinStep::Done: return "on its team";
    case JoinStep::Waiting: return "waiting for the server";
    case JoinStep::GaveUp: return "given up (the server did not take it)";
    }
    return "?";
}
struct JoinState {
    int team_tries = 0, class_tries = 0;
    int since = 0;  // frames since the last line
    bool done = false;
    void Reset() { *this = {}; }
};
inline JoinStep NextJoin(const JoinChoice& c, const ServerPlayer& s, JoinState& st, int retry = 60, int tries = 3) {
    if (!c.On()) return JoinStep::Off;
    if (st.done) return JoinStep::Done;
    if (!s.known) return JoinStep::NoServerPlayer;
    st.since++;
    bool chose_team = s.team == 1 || s.team == 2 || s.team == 3;
    if (chose_team && s.menu != kMenuChooseClass) {
        st.done = true;  // a team (and its class, or spectating): the game's own state from here on
        return JoinStep::Done;
    }
    bool due = st.since > retry;
    if (!chose_team && (s.menu == kMenuChooseTeam || s.menu == kMenuInGameTeam)) {
        if (st.team_tries == 0 || due) {
            if (st.team_tries >= tries) return JoinStep::GaveUp;
            st.team_tries++;
            st.since = 0;
            return JoinStep::Team;
        }
        return JoinStep::Waiting;
    }
    if (chose_team && s.menu == kMenuChooseClass) {
        if (st.class_tries == 0 || due) {
            if (st.class_tries >= tries) return JoinStep::GaveUp;
            st.class_tries++;
            st.since = 0;
            return JoinStep::Class;
        }
        return JoinStep::Waiting;
    }
    return JoinStep::Waiting;  // another menu open (the buy menu, a message): the game's own
}
// who chooses automatically: a player other than the shared front end's driver (whose own team screen shows)
inline bool AutoJoinsFor(int player, int driver) { return player != driver; }

inline std::string JoinLine(JoinStep s, const JoinChoice& c) {
    if (s == JoinStep::Team) return "menuselect " + std::to_string(c.team) + "\n";
    if (s == JoinStep::Class) return "menuselect " + std::to_string(c.cls) + "\n";
    return "";
}
inline const char* TeamName(uint32_t t) {
    return t == 1 ? "Terrorist" : t == 2 ? "Counter-Terrorist" : t == 3 ? "spectator" : "none";
}

}  // namespace nsplit
