// Native split-screen M3 (docs/native_splitscreen.md 16) without the game or a GPU: the views' layout
// (native_split_view.h), who draws and in which viewport as players depart and rejoin, which view starts and ends the
// window frame, the shared front end drawn once, another player's viewport not painted, the controllers' ports, the
// back buffer's aspect, and a view's placement in the window clipped to its viewport (split_ipc.h PlaceInViewport).
// Simulated frames: the presents are counted by a model of d3d8.cpp's Swap (SDK patch 0047), not drawn.
#include <cmath>
#include <cstdio>
#include <vector>

#include "game/native_split_join.h"
#include "game/native_split_view.h"
#include "linux/split_ipc.h"

using namespace nsplit;

static int checks = 0, failed = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL: %s\n", what);
    }
}
static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// the layouts: cover the window, no overlap, equal sizes
static void Layouts() {
    auto one = Layout(1, false);
    Check(one[0] == Rect{0, 0, 1, 1}, "one view: the whole window");
    auto h = Layout(2, false);
    Check(h[0] == (Rect{0, 0, 1, 0.5f}) && h[1] == (Rect{0, 0.5f, 1, 0.5f}), "two: player 1 above, player 2 below, full width");
    auto v = Layout(2, true);
    Check(v[0] == (Rect{0, 0, 0.5f, 1}) && v[1] == (Rect{0.5f, 0, 0.5f, 1}), "two vertical: left and right, full height");
    for (int n = 2; n <= 4; n++)
        for (int vert = 0; vert < 2; vert++) {
            auto r = Layout(n, vert != 0);
            float area = 0;
            int count = n == 2 ? 2 : 4;
            for (int i = 0; i < count; i++) area += r[size_t(i)].w * r[size_t(i)].h;
            bool overlap = false;
            for (int i = 0; i < count; i++)
                for (int j = i + 1; j < count; j++) {
                    const Rect &a = r[size_t(i)], &b = r[size_t(j)];
                    if (a.x < b.x + b.w - 1e-6f && b.x < a.x + a.w - 1e-6f && a.y < b.y + b.h - 1e-6f && b.y < a.y + a.h - 1e-6f)
                        overlap = true;
                }
            bool same = true;
            for (int i = 1; i < count; i++) same = same && Near(r[size_t(i)].w, r[0].w) && Near(r[size_t(i)].h, r[0].h);
            Check(Near(area, 1) && !overlap && same, "a layout covers the window once, its views the same size");
        }
    Check(Near(ViewAspect(16.0f / 9.0f, 2, false), 32.0f / 9.0f), "two top and bottom at 16:9: 32:9 views");
    Check(Near(ViewAspect(16.0f / 9.0f, 2, true), 8.0f / 9.0f), "side by side at 16:9: 8:9 (pc_wide keeps 4:3 there)");
    Check(Near(ViewAspect(16.0f / 9.0f, 4, false), 16.0f / 9.0f), "2x2: the window's aspect");
    Check(Near(ViewAspect(4.0f / 3.0f, 2, false), 8.0f / 3.0f), "headless 4:3: 8:3 views");
    Check(ConfiguredView(2, false) == (Rect{0, 0, 1, 0.5f}), "the configured view: the layout's first");
}

// a session's players, their contexts (player 1 context 0, player 2 context 1)
struct Play {
    Players p;
    std::array<int, kMaxPlayers> ctx{{0, -1, -1, -1}};
    Play() {
        p.Begin(1, 1, "Player 2");
        p.Initialised(1);
        p.Joined(1);
        ctx[1] = 1;
    }
};

// a model of d3d8.cpp's Swap with views (SDK patch 0047): a view's Swap placed; the last presents; the frame's end
// presents a frame left open (FinishSplitFrame)
struct Presenter {
    int presents = 0, placed = 0, finished = 0, paces = 0, cleared = 0;
    bool open = false;
    void Swap(const View& v) {
        if (!v.draws) return;  // a hidden view (w = h = 0) is placed nowhere and presents nothing
        placed++;
        if (v.first) {
            open = true;
            cleared++;
        }
        if (v.last) {
            presents++;
            paces++;
            open = false;
        }
    }
    void FrameEnd() {
        if (open) {
            finished++;
            presents++;
            open = false;
        }
    }
};

// one frame: every context's turn in order; `swaps[c]` whether that turn's SCR_UpdateScreen swapped
static std::vector<View> Frame(Play& s, Presenter& pr, std::array<bool, kMaxPlayers> swaps = {{true, true, true, true}}) {
    std::vector<View> out;
    for (int c = 0; c < kMaxPlayers; c++) {
        bool used = false;
        for (int k = 0; k < kMaxPlayers; k++) used = used || (s.ctx[size_t(k)] == c && s.p.At(k).state != State::Empty);
        if (!used) continue;
        View v = ViewOf(s.p, s.ctx, c, false);
        out.push_back(v);
        if (v.draws && swaps[size_t(c)]) pr.Swap(v);
    }
    pr.FrameEnd();
    return out;
}

static void TwoPlayers() {
    Play s;
    Presenter pr;
    auto v = Frame(s, pr);
    Check(v.size() == 2 && v[0].draws && v[1].draws && v[0].views == 2, "two active players: two views");
    Check(v[0].first && !v[0].last && !v[1].first && v[1].last, "player 1's view first, player 2's last");
    Check(v[0].rect == (Rect{0, 0, 1, 0.5f}) && v[1].rect == (Rect{0, 0.5f, 1, 0.5f}), "player 1 top, player 2 bottom");
    Check(pr.presents == 1 && pr.placed == 2 && pr.paces == 1 && pr.finished == 0,
          "one present a frame, both views placed, paced once");
    for (int i = 0; i < 99; i++) Frame(s, pr);
    Check(pr.presents == 100 && pr.placed == 200 && pr.cleared == 100, "100 frames: 100 presents, 200 views, one clear each");

    // player 1 departs: player 2 alone, the whole window
    Presenter p2;
    s.p.Depart0();
    v = Frame(s, p2);
    Check(!v[0].draws && v[1].draws && v[1].views == 1 && v[1].first && v[1].last && v[1].rect == Rect{},
          "player 1 departed: its turn draws nothing, player 2's view is the whole window");
    Check(p2.presents == 1 && p2.placed == 1, "still one present a frame (not a blank screen)");
    // its rejoin: connecting, it draws its loading view again in the top half
    s.p.Rejoin0();
    v = Frame(s, p2);
    Check(v[0].draws && v[0].views == 2 && v[0].rect == (Rect{0, 0, 1, 0.5f}) && v[1].last,
          "player 1 rejoining: two views again");
    s.p.Joined(0);
    v = Frame(s, p2);
    Check(v[0].draws && v[1].draws && p2.presents == 3, "player 1 in the match again: two views, one present");

    // player 2 leaves (removed: its slot empty): player 1 alone, the whole window
    s.p.Leave(1);
    s.p.Released(1);
    s.ctx[1] = -1;
    Presenter p3;
    v = Frame(s, p3);
    Check(v.size() == 1 && v[0].draws && v[0].views == 1 && v[0].rect == Rect{} && p3.presents == 1,
          "player 2 gone: player 1 the whole window");
    // player 2 joins again: initialising draws nothing, then connecting and active draw
    s.p.Begin(1, 1, "Player 2");
    s.ctx[1] = 1;
    v = Frame(s, p3);
    Check(v[0].draws && v[0].views == 1 && !v[1].draws, "player 2 initialising: player 1 alone still");
    s.p.Initialised(1);
    v = Frame(s, p3);
    Check(v[1].draws && v[1].views == 2 && v[1].last, "player 2 connecting: its view in the bottom half");
}

static void MissingPresents() {
    Play s;
    Presenter pr;
    // player 2's turn presents nothing (its SCR_UpdateScreen returned early): the frame left open is presented
    Frame(s, pr, {{true, false, true, true}});
    Check(pr.presents == 1 && pr.finished == 1, "a missing last view: the frame presented at its end, once");
    // player 1's turn presents nothing: player 2's last view still presents
    Presenter p2;
    Frame(s, p2, {{false, true, true, true}});
    Check(p2.presents == 1 && p2.finished == 0, "a missing first view: the last one presents");
    // nobody swaps: nothing presented (as the title's frame that draws nothing)
    Presenter p3;
    Frame(s, p3, {{false, false, true, true}});
    Check(p3.presents == 0, "no view swapped: nothing presented, nothing finished");
}

static void FourPlayers() {
    Players p;
    std::array<int, kMaxPlayers> ctx{{0, 1, 2, 3}};
    for (int k = 1; k < 4; k++) {
        p.Begin(k, k, "Player");
        p.Initialised(k);
        p.Joined(k);
    }
    int lasts = 0, firsts = 0;
    for (int c = 0; c < 4; c++) {
        View v = ViewOf(p, ctx, c, false);
        lasts += v.last;
        firsts += v.first;
        Check(v.draws && v.views == 4 && v.rect == Layout(4, false)[size_t(c)], "four: each its quarter");
    }
    Check(lasts == 1 && firsts == 1, "four: one first, one last view");
    // player 3 removed, renumbering nothing: three views in the first three quarters, in context order
    p.Leave(2);
    p.Released(2);
    ctx[2] = -1;
    View v3 = ViewOf(p, ctx, 3, false);
    Check(v3.draws && v3.views == 3 && v3.index == 2 && v3.last && v3.rect == Layout(3, false)[2],
          "three left: player 4 takes the third quarter, last");
}

static void SharedAndPanels() {
    Check(DrawsShared(1, 1, 0), "one view: the shared front end drawn as the title's");
    Check(DrawsShared(2, 0, 0) && !DrawsShared(2, 1, 0), "two views: in the driver's turn only");
    Check(DrawsShared(2, 1, 1) && !DrawsShared(2, 0, 1), "player 1 departed: player 2 drives and draws it");
    Check(DrawsShared(2, -1, 0), "outside the turns: as the title's");
    Check(PaintsPanel(true, true, 0x100, 0x100), "its own viewport painted");
    Check(!PaintsPanel(true, true, 0x200, 0x100), "another player's viewport not painted in this turn");
    Check(PaintsPanel(true, false, 0x200, 0x100), "any other panel painted");
    Check(PaintsPanel(false, true, 0x200, 0x100), "outside the turns: everything as the title's");
}

static void Pads() {
    Check(PadPort(0, 3) == 3, "player 1 keeps the title's active pad");
    Check(PadPort(1, 0) == 1 && PadPort(2, 0) == 2 && PadPort(3, 0) == 3, "player k+1 reads port k");
    Check(AddedPlayerReadsPad(true, true) && !AddedPlayerReadsPad(false, true) && !AddedPlayerReadsPad(true, false),
          "routed and in its turn only");
}

// a view's placement (W x H window): inside its viewport, the aspect kept, never over another view
static void Placement() {
    // 1920x1080, two top and bottom, 32:9 frames (3.556): each exactly fills its half
    float a = 32.0f / 9.0f;
    split::ViewBlit top = split::PlaceInViewport(1920, 1080, 0, 0, 1, 0.5f, a, 1920, 540);
    split::ViewBlit bot = split::PlaceInViewport(1920, 1080, 0, 0.5f, 1, 0.5f, a, 1920, 540);
    Check(!top.empty && top.dx0 == 0 && top.dy0 == 0 && top.dx1 == 1920 && top.dy1 == 540, "the top view fills the top half");
    Check(!bot.empty && bot.dy0 == 540 && bot.dy1 == 1080 && bot.sx0 == 0 && bot.sx1 == 1920, "the bottom one the bottom half");
    Check(top.dy1 <= bot.dy0, "no overlap");
    // player 2 alone, its 32:9 frame in the whole 16:9 window: fitted, letterboxed, not stretched
    split::ViewBlit alone = split::PlaceInViewport(1920, 1080, 0, 0, 1, 1, a, 1920, 540);
    // (FrameRect: a frame wider than a viewport at least 4:3 fills its height, its sides cut, the 4:3 middle with the
    // HUD whole): the whole window, not stretched
    Check(!alone.empty && alone.dx0 == 0 && alone.dx1 == 1920 && alone.dy0 == 0 && alone.dy1 == 1080,
          "alone: the whole window at full height");
    Check(alone.sy0 == 0 && alone.sy1 == 540 && alone.sx0 == 480 && alone.sx1 == 1440,
          "alone: the frame's middle 16:9 shown (its sides cut), the 4:3 middle whole");
    // a frame wider than its viewport (4:1 cap at 21:9 halves): the height filled, the sides cut, clipped to it
    split::ViewBlit wide = split::PlaceInViewport(2560, 1080, 0, 0.5f, 1, 0.5f, 4.0f, 2160, 540);
    Check(!wide.empty && wide.dx0 >= 0 && wide.dx1 <= 2560 && wide.dy0 >= 540 && wide.dy1 <= 1080,
          "a wider frame stays inside its viewport");
    float shown_aspect = float(wide.dx1 - wide.dx0) / float(wide.dy1 - wide.dy0);
    float src_aspect = float(wide.sx1 - wide.sx0) / float(wide.sy1 - wide.sy0) * (4.0f / (2160.0f / 540.0f));
    Check(std::fabs(shown_aspect - src_aspect) < 0.02f, "its shown part keeps the frame's aspect (cut, not squeezed)");
    // side by side at 16:9: 4:3 frames pillarboxed... the 8:9 viewport is narrower than 4:3: fitted, black above and below
    split::ViewBlit left = split::PlaceInViewport(1920, 1080, 0, 0, 0.5f, 1, 4.0f / 3.0f, 640, 480);
    split::ViewBlit right = split::PlaceInViewport(1920, 1080, 0.5f, 0, 0.5f, 1, 4.0f / 3.0f, 640, 480);
    Check(!left.empty && left.dx1 <= 960 && right.dx0 >= 960 && left.dy0 > 0, "side by side: each in its half, letterboxed");
    // degenerate: a hidden view, a zero window
    Check(split::PlaceInViewport(1920, 1080, 0, 0, 0, 0, a, 1920, 540).empty, "a hidden view (0 x 0) places nothing");
    Check(split::PlaceInViewport(0, 0, 0, 0, 1, 1, a, 1920, 540).empty, "no window: nothing");
    // odd sizes: the two halves share their edge exactly
    split::ViewBlit t2 = split::PlaceInViewport(1365, 767, 0, 0, 1, 0.5f, 1365.0f / 383.5f, 1365, 384);
    split::ViewBlit b2 = split::PlaceInViewport(1365, 767, 0, 0.5f, 1, 0.5f, 1365.0f / 383.5f, 1365, 384);
    Check(t2.dy1 <= b2.dy0 && b2.dy1 <= 767, "odd window: halves never overlap");
}

// docs 17: the split views' field of view
static void Fov() {
    const double w169 = 16.0 / 9.0, half = 32.0 / 9.0;
    Check(std::fabs(HorPlusDeg(90, 4.0 / 3.0) - 90) < 1e-6, "Hor+ at 4:3: the title's own");
    Check(std::fabs(HorPlusDeg(90, w169) - 106.26) < 0.01, "Hor+ at 16:9: 106.3");
    Check(std::fabs(HorPlusDeg(90, half) - 138.88) < 0.05, "Hor+ at 32:9: 138.9 (run 17's)");
    // two top and bottom at 16:9: the window's angle across each strip
    double split = SplitFrameFov(90, half, ShownAspect(half, half), w169, false);
    Check(std::fabs(split - 106.26) < 0.01, "a 32:9 half: 106.3 across it (the window's), not 139");
    Check(VerticalDeg(split, half) < VerticalDeg(90, 4.0 / 3.0), "its vertical angle narrower (a strip of the window's view)");
    Check(std::fabs(SplitFrameFov(90, half, half, w169, true) - 138.88) < 0.05, "\"wide\": Hor+ at the view's, as before");
    // alone: the 32:9 frame cut to the window's 16:9: the frame wider, what shows exactly the window's Hor+
    double alone = SplitFrameFov(90, half, ShownAspect(half, w169), w169, false);
    Check(std::fabs(alone - 138.88) < 0.05, "alone: the frame 138.9, its shown middle 16:9");
    const double rad = 3.14159265358979323846 / 180.0;
    double shown_angle = 2 * std::atan(std::tan(alone * rad / 2) * w169 / half) / rad;
    Check(std::fabs(shown_angle - 106.26) < 0.05, "alone: 106.3 across what shows, as one player in this window");
    // side by side at 16:9: 8:9 viewports, the 4:3 frame fitted: the title's own 90
    double side = SplitFrameFov(90, 4.0 / 3.0, ShownAspect(4.0 / 3.0, 8.0 / 9.0), w169, false);
    Check(std::fabs(side - 90) < 0.01, "side by side: a 4:3 frame letterboxed, 90 as the title's");
    // not split-screen: a 16:9 frame in a 16:9 window, unchanged
    Check(std::fabs(SplitFrameFov(90, w169, ShownAspect(w169, w169), w169, false) - HorPlusDeg(90, w169)) < 1e-9,
          "one player, the window's frame: Hor+ unchanged");
    // other windows: 21:9 (64:27) halves are 128:27 (4.74), the 4:1 frame cut to the viewport? no: narrower, fitted
    double w219 = 64.0 / 27.0, f219 = 4.0;
    double s219 = SplitFrameFov(90, f219, ShownAspect(f219, w219 * 2), w219, false);
    Check(std::fabs(s219 - HorPlusDeg(90, w219)) < 0.01, "21:9 halves: the window's 21:9 angle (about 121)");
    double s43 = SplitFrameFov(90, 8.0 / 3.0, ShownAspect(8.0 / 3.0, 8.0 / 3.0), 4.0 / 3.0, false);
    Check(std::fabs(s43 - 90) < 0.01, "4:3 window halves: 90 across each");
    // a zoom (the scope's 40, the AUG's 55): the same policy, the player's own FOV
    double z = SplitFrameFov(40, half, half, w169, false);
    Check(std::fabs(z - HorPlusDeg(40, w169)) < 0.01 && z < split, "a zoom narrows by the same rule");
    // monotonic in fov, never above Hor+ at the view's aspect
    double last = 0;
    bool mono = true, below = true;
    for (double f = 10; f <= 170; f += 5) {
        double v = SplitFrameFov(f, half, half, w169, false);
        mono = mono && v > last;
        below = below && v <= HorPlusDeg(f, half) + 1e-9;
        last = v;
    }
    Check(mono && below, "monotonic in the FOV, never wider than Hor+");
}

// docs 17: the automatic team choice through the game's own menus
static void AutoJoinRules() {
    JoinChoice off = ChoiceOf(nullptr, nullptr, 1);
    Check(!off.On(), "unset: nothing automatic");
    JoinChoice c2 = ChoiceOf("0,5", nullptr, 1), c1 = ChoiceOf("0,5", nullptr, 0);
    Check(c2.On() && c2.team == 5 && c2.cls == 5 && !c1.On(), "\"0,5\": player 2 auto-assigned, player 1 its own menus");
    JoinChoice ct = ChoiceOf("1,2", "3,4", 1);
    Check(ct.team == 2 && ct.cls == 4, "per player team and class");
    Check(!ChoiceOf("4,7", nullptr, 0).On() && !ChoiceOf("4,7", nullptr, 1).On(), "slots not in the team menu: off");
    Check(ChoiceOf("1", "9", 0).cls == 5 && ChoiceOf("2", nullptr, 3).team == 0, "bad class: random; missing: off");
    Check(!AutoJoinsFor(0, 0) && AutoJoinsFor(1, 0) && AutoJoinsFor(0, 1), "only a player not driving the front end");

    JoinChoice c = ChoiceOf("2,1", "5,5", 1);  // player 2: Terrorist, random class
    JoinState st;
    ServerPlayer sp;
    Check(NextJoin(c, sp, st) == JoinStep::NoServerPlayer, "no server player yet: nothing");
    sp.known = true;
    sp.menu = 0;
    Check(NextJoin(c, sp, st) == JoinStep::Waiting, "not in the team menu yet: wait");
    sp.menu = kMenuChooseTeam;
    JoinStep s = NextJoin(c, sp, st);
    Check(s == JoinStep::Team && JoinLine(s, c) == "menuselect 1\n", "in the team menu: its own \"menuselect 1\"");
    Check(NextJoin(c, sp, st) == JoinStep::Waiting, "sent: waits for the server");
    sp.team = 1;
    sp.menu = kMenuChooseClass;
    s = NextJoin(c, sp, st);
    Check(s == JoinStep::Class && JoinLine(s, c) == "menuselect 5\n", "a team, the class menu: \"menuselect 5\"");
    sp.menu = 0;
    sp.health = 100;
    Check(NextJoin(c, sp, st) == JoinStep::Done && NextJoin(c, sp, st) == JoinStep::Done, "on its team: done, nothing more");
    // the server not taking it (a full team): resent after the retry, then given up
    JoinState st2;
    ServerPlayer full;
    full.known = true;
    full.menu = kMenuChooseTeam;
    int lines = 0;
    JoinStep last = JoinStep::Off;
    for (int i = 0; i < 400; i++) {
        last = NextJoin(c, full, st2, 60, 3);
        lines += last == JoinStep::Team;
        if (last == JoinStep::GaveUp) break;
    }
    Check(lines == 3 && last == JoinStep::GaveUp, "refused: three tries, then given up (no flood)");
    // a rejoin: a new connection starts over
    st.Reset();
    ServerPlayer again;
    again.known = true;
    again.menu = kMenuChooseTeam;
    Check(NextJoin(c, again, st) == JoinStep::Team, "after a reset (a rejoin): the team again");
    // a spectating choice: done once the server shows spectator
    JoinChoice spec = ChoiceOf("6,6", nullptr, 1);
    JoinState st3;
    ServerPlayer s3;
    s3.known = true;
    s3.menu = kMenuChooseTeam;
    Check(JoinLine(NextJoin(spec, s3, st3), spec) == "menuselect 6\n", "spectate: menuselect 6");
    s3.team = 3;
    s3.menu = 0;
    Check(NextJoin(spec, s3, st3) == JoinStep::Done, "spectator: done");
}

int main() {
    Fov();
    AutoJoinRules();
    Layouts();
    TwoPlayers();
    MissingPresents();
    FourPlayers();
    SharedAndPanels();
    Pads();
    Placement();
    std::printf("native_split_view_test: %d checks, %d failed\n", checks, failed);
    std::printf("note: simulated frames and placement arithmetic only; no GPU, no game (docs 16)\n");
    return failed ? 1 : 0;
}
