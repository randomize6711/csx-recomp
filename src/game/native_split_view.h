// Native split-screen M3 (docs/native_splitscreen.md 16): which local players draw this frame, where each one's view
// goes in the window, and which view ends the window's frame. Pure (no guest memory): tests/native_split_view_test.cpp.
//
// Every player that draws runs the title's own SCR_UpdateScreen in its own turn, into the one back buffer, with its own
// client state (its camera, HUD, view model, fades: all in its regions), and its Swap is one view of the window frame
// (d3d::SetSplitView): the host places the finished frame in that player's viewport and presents the window once, at
// the last view. The world is simulated once (the server frame), each player's view drawn once.
#pragma once
#include <array>
#include <cstdint>

#include "game/native_split_core.h"

namespace nsplit {

// a viewport in fractions of the window
struct Rect {
    float x = 0, y = 0, w = 1, h = 1;
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// the layout of n views (1..4): one, the whole window; two, top and bottom (vertical: side by side); three and four, a
// 2x2 grid (three leave the last quarter empty, so every view has the same size and aspect: the title's one device has
// one back buffer size)
inline std::array<Rect, kMaxPlayers> Layout(int n, bool vertical) {
    std::array<Rect, kMaxPlayers> r{};
    if (n <= 1) return r;
    if (n == 2) {
        if (vertical) {
            r[0] = {0, 0, 0.5f, 1};
            r[1] = {0.5f, 0, 0.5f, 1};
        } else {
            r[0] = {0, 0, 1, 0.5f};
            r[1] = {0, 0.5f, 1, 0.5f};
        }
        return r;
    }
    r[0] = {0, 0, 0.5f, 0.5f};
    r[1] = {0.5f, 0, 0.5f, 0.5f};
    r[2] = {0, 0.5f, 0.5f, 0.5f};
    r[3] = {0.5f, 0.5f, 0.5f, 0.5f};
    return r;
}

// the view size of the configured layout (the players asked for, CSX_NATIVE_SPLIT): the back buffer's aspect is chosen
// for it once at the boot, and the internal resolution follows it, whoever is away
inline Rect ConfiguredView(int wanted, bool vertical) { return Layout(wanted, vertical)[0]; }

// the display aspect of one view of the configured layout in a window of `window_aspect`
inline float ViewAspect(float window_aspect, int wanted, bool vertical) {
    Rect v = ConfiguredView(wanted, vertical);
    return window_aspect * v.w / v.h;
}

// who draws: a player in its match or connecting to it (its loading screen is its own); a departed player 1, an
// initialising player and an empty slot draw nothing
inline bool Draws(State s) { return s == State::Active || s == State::Connecting; }

// one player's view of this window frame
struct View {
    bool draws = false;  // false: its turn draws nothing (SCR_UpdateScreen skipped)
    int index = 0;       // its place among this frame's views
    int views = 0;       // the views of this frame
    bool first = false, last = false;
    Rect rect;
};

// the view of the player in context `context` (contexts run their turns in order 0, 1, ...); `context_of[k]` is player
// k's context (-1 none). The views are laid out for the players that draw now: alone, a player gets the whole window
inline View ViewOf(const Players& p, const std::array<int, kMaxPlayers>& context_of, int context, bool vertical) {
    View v;
    int me = -1;
    for (int k = 0; k < kMaxPlayers; k++)
        if (context_of[k] == context && p.At(k).state != State::Empty) me = k;
    // the drawing players in turn order (by context)
    std::array<int, kMaxPlayers> order{};
    int n = 0;
    for (int c = 0; c < kMaxPlayers; c++)
        for (int k = 0; k < kMaxPlayers; k++)
            if (context_of[k] == c && Draws(p.At(k).state)) order[size_t(n++)] = k;
    v.views = n;
    if (me < 0 || !Draws(p.At(me).state)) return v;
    for (int i = 0; i < n; i++)
        if (order[size_t(i)] == me) v.index = i;
    v.draws = true;
    v.first = v.index == 0;
    v.last = v.index == n - 1;
    v.rect = Layout(n, vertical)[size_t(v.index)];
    return v;
}

// a draw of the shared front end (the console, the game menus, the front end's own screens) in a player's turn: once a
// window frame, in the turn of the player driving the shared front end, when several views draw (one view: as the title)
inline bool DrawsShared(int views, int turn_player, int driver) { return views <= 1 || turn_player < 0 || turn_player == driver; }

// a VGUI panel traverse in a player's turn: another player's viewport (the root panel is the process's, every player's
// viewport its child) is not painted in this player's view (its paint draws the 3D view and the HUD of whoever's turn
// it is). Not a viewport, or this player's own: painted
inline bool PaintsPanel(bool in_turn, bool is_viewport, uint32_t panel, uint32_t own_viewport) {
    return !in_turn || !is_viewport || panel == own_viewport;
}

// the pad each player reads in its turn (the title's one active pad index, set for that turn): player 1 keeps the
// title's own choice (the controller that pressed START, with the keyboard and mouse on port 0); player k (2..4) the
// window's port k-1 (its (k-1)th other controller)
inline uint32_t PadPort(int player, uint32_t title_active) { return player <= 0 ? title_active : uint32_t(player); }

// input for an added player's turn (the look routine, the pad's buttons as keys): only with its own controller routed
// (CSX_NATIVE_SPLIT_INPUT), and never in another player's initialisation or outside the turns
inline bool AddedPlayerReadsPad(bool routed, bool in_turn) { return routed && in_turn; }

}  // namespace nsplit
