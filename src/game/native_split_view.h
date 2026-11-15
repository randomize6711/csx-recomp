// Native split-screen M3 (docs/native_splitscreen.md 16): which local players draw this frame, where each one's view
// goes in the window, and which view ends the window's frame. Pure (no guest memory): tests/native_split_view_test.cpp.
//
// Every player that draws runs the title's own SCR_UpdateScreen in its own turn, into the one back buffer, with its own
// client state (its camera, HUD, view model, fades: all in its regions), and its Swap is one view of the window frame
// (d3d::SetSplitView): the host places the finished frame in that player's viewport and presents the window once, at
// the last view. The world is simulated once (the server frame), each player's view drawn once.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
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

// ---- the split views' field of view (docs 17) ----
// The title's FOV is horizontal degrees for a 4:3 view; pc_wide makes it Hor+ (the 4:3 view's vertical angle kept) for
// the frame's aspect. A split view's frame is wider than the window (two top and bottom at 16:9: 32:9), where Hor+
// reaches 139 degrees and stretches the sides. The split policy ("window", the default): the horizontal angle a
// player has across what the window shows of its view is at most the one the whole window would have (Hor+ at the
// window's aspect, about 106 degrees at 16:9 for 90): the view is a strip of the full-window view, its vertical angle
// narrower. "wide" keeps Hor+ at the view's aspect (the earlier behaviour). Alone, a player's frame is cropped to the
// window's aspect (FrameRect), so its shown part is the window's: the same horizontal angle as one player alone in a
// window of that aspect. All in degrees; fov the title's (4:3 horizontal), frame / shown / window aspects w:h
inline double HorPlusDeg(double fov, double aspect) {
    const double rad = 3.14159265358979323846 / 180.0;
    if (!(fov > 0 && fov < 180) || !(aspect > 0)) return fov;
    return 2.0 * std::atan(std::tan(fov * rad / 2.0) * aspect * 0.75) / rad;
}
// the horizontal angle across the whole frame; shown: the aspect of the part of the frame the window shows (the frame's
// own when it is fitted whole, the viewport's when its sides are cut)
inline double SplitFrameFov(double fov, double frame, double shown, double window, bool wide_policy) {
    const double rad = 3.14159265358979323846 / 180.0;
    if (!(frame > 0 && shown > 0 && window > 0)) return HorPlusDeg(fov, frame);
    double target = wide_policy ? shown : std::min(shown, window);
    double across_shown = HorPlusDeg(fov, target);  // across what the window shows
    double t = std::tan(across_shown * rad / 2.0) * frame / shown;
    return 2.0 * std::atan(t) / rad;
}
// what the window shows of a frame of aspect `frame` in a viewport of aspect `viewport` (split::FrameRect: fitted whole,
// or, wider than a viewport at least 4:3, cut at the sides to the viewport's aspect)
inline double ShownAspect(double frame, double viewport) {
    if (frame > viewport && viewport >= 4.0 / 3.0 - 0.001) return viewport;
    return frame;
}
// the vertical angle of a horizontal one at an aspect
inline double VerticalDeg(double hfov, double aspect) {
    const double rad = 3.14159265358979323846 / 180.0;
    return 2.0 * std::atan(std::tan(hfov * rad / 2.0) / aspect) / rad;
}

// the pad each player reads in its turn (the title's one active pad index, set for that turn): player 1 keeps the
// title's own choice (the controller that pressed START, with the keyboard and mouse on port 0); player k (2..4) the
// window's port k-1 (its (k-1)th other controller)
inline uint32_t PadPort(int player, uint32_t title_active) { return player <= 0 ? title_active : uint32_t(player); }

// input for an added player's turn (the look routine, the pad's buttons as keys): only with its own controller routed
// (CSX_NATIVE_SPLIT_INPUT), and never in another player's initialisation or outside the turns
inline bool AddedPlayerReadsPad(bool routed, bool in_turn) { return routed && in_turn; }

// ---- the shared front end's input (docs 17.9) ----
// The title feeds its one front end from the pad it reads: the look routine (0x19E160, in every player's input turn)
// copies the pad bridge's buttons (0x5439B8) into the front end's button snapshot (0x5523C0 now, 0x5523C4 the frame
// before) and the active pad's left stick into the menus' direction (0xCA670: 0x552DD0 / 0x552DD4); the pie menus
// (0xCD470 the slice the direction points at, 0xCD680 A or START to choose it), drawn and run in the driver's turn,
// read them. The bridge (0x19DFD0) hands each new press and release to the screen on top (0xCB170 / 0xCB540: its
// button commands, the page and the cheat keys). One front end for the process: only the player driving it feeds it.
// Run 18: player 2's routed look ran after player 1's and left its idle buttons in the snapshot at every frame's end,
// so the team screen's choice (A) never saw player 1's press (its D-pad still turned the direction: player 2's pass
// reads player 1's copy before replacing it). Another player's controller goes to its own game only (its look, its
// buttons as its keys), also while the driver has a screen open. player < 0: not a local player's (as the title)
inline bool FeedsSharedFrontEnd(int player, int driver, bool own_menus = false) {
    return own_menus || player < 0 || player == driver;
}

// the window's keyboard and mouse are player 1's (port 0, docs 17.12). They act on the shared front end's screen (the
// menus' pad and pointer: pc_keys' menu mode, the PC screens' typing, key capture, number rows, the browser's and the map
// lists' keys) only while player 1 drives it; another driver's screen (player 2's buy menu after player 1's departure
// and rejoin, run 20) leaves them player 1's game's, and that screen to its driver's controller. Not native
// split-screen (the title alone, the two-process split-screen: each process's own front end): always, as before
inline bool PcInputDrivesMenus(bool native_split, int driver, bool own_menus = false) {
    return !native_split || FeedsSharedFrontEnd(0, driver, own_menus);
}
// docs 18.5: the in-game menus (0xCD7D0) drawn in a player's pass: its own with own menus, else the driver's once
inline bool DrawsMenus(int views, int turn_player, int driver, bool own_menus) {
    return (own_menus && turn_player >= 0) || DrawsShared(views, turn_player, driver);
}

// a model of what the front end reads (tests/native_split_view_test.cpp): one look routine pass of `player` with its
// bridge mask and its left stick (the title's 0xCA670 also takes the D-pad's up / down / left / right over the stick)
struct FrontEndInput {
    uint32_t now = 0, before = 0;  // 0x5523C0, 0x5523C4
    float dx = 0, dy = 0;          // 0x552DD0, 0x552DD4
    int fed_by = -1;               // whose controller the snapshot is (diagnostics)
    void Look(int player, int driver, uint32_t mask, float stick_x, float stick_y) {
        if (!FeedsSharedFrontEnd(player, driver)) return;  // NativeSplitLookMenu: 0x19E1AE -> 0x19E1F9
        dx = stick_x;
        dy = stick_y;
        if (now & 0x100) dy = 1;  // 0xCA670 reads the snapshot before this pass's copy: the D-pad of the frame before
        else if (now & 0x200) dy = -1;
        else if (now & 0x400) dx = -1;
        else if (now & 0x800) dx = 1;
        before = now;
        now = mask;
        fed_by = player;
    }
};

}  // namespace nsplit
