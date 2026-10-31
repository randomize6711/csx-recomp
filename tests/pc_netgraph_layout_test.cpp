// tests/pc_netgraph_layout_test.py builds and runs this: src/game/pc_netgraph_layout.h on its own. The net graph as
// one parent: the engine's own layout (as SCR_DrawNetGraph draws it: backdrop, columns, text lines, from its origin)
// through the parent transform and its two draw paths (fills, the text renderer), measured, placed, and its final
// bounds in a window's pixels
#include <cmath>
#include <cstdio>
#include <vector>

#include "game/pc_netgraph_layout.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

// a piece the engine draws, in its own layout relative to the origin (the 2D layout's units)
struct Piece {
    const char* what;
    float x0, y0, x1, y1;
    bool text;
};
// SCR_DrawNetGraph at net_graphwidth 300, graphheight 64, as a play-test logged it (scale 0.5, origin 587 458; here
// at scale 1): the backdrop (0x6E657: x - 5, y - 69, 310 x 98), the columns, the two rate labels ("30/s", "60/s") at
// x + 250 (the graph's width less 50) on the origin's line and 64 above it, 42 wide; the in, out and loss lines at
// x, 64, 48 and 34 above it (78, 128, 124 wide); a Detail 2 line as wide as the sv line, about 230
static std::vector<Piece> Engine(bool detail2) {
    std::vector<Piece> v = {{"backdrop", -5, -69, 305, 29, false},
                            {"columns", 0, -64, 300, 0, false},
                            {"rate label 60/s", 250, -64, 292, -55, true},
                            {"in", 0, -64, 78, -55, true},
                            {"out", 0, -48, 128, -39, true},
                            {"loss", 0, -34, 124, -25, true},
                            {"rate label 30/s", 250, 0, 292, 9, true}};
    if (detail2) v.push_back({"sv", 0, 14, 230, 23, true});
    return v;
}

struct Case {
    const char* name;
    int w, h;
};

// one frame of the net graph as the game draws it: every piece through the parent transform, a fill landing at its
// frame x, a string at the x it is given plus the text hook's side column (pc_wide.cpp PcWideText). Returns the
// pieces' final boxes in the frame's units; compensate: whether strings are given TextDrawX (the fix)
struct Drawn {
    const Piece* piece;
    pcnetgraph::Box frame;
};
static std::vector<Drawn> DrawFrame(const std::vector<Piece>& pieces, const pcnetgraph::Transform& tf, float side,
                                    bool compensate) {
    std::vector<Drawn> out;
    for (const Piece& p : pieces) {
        pcnetgraph::Box b;
        if (p.text) {
            const float want = tf.X(tf.ox + p.x0);  // the frame x it belongs at
            const float given = compensate ? pcnetgraph::TextDrawX(want, side) : want;
            const float x = pcnetgraph::TextFrameX(given, side);
            b.Add(x, tf.Y(tf.oy + p.y0), x + (p.x1 - p.x0) * tf.scale, tf.Y(tf.oy + p.y1));
        } else {
            b.Add(tf.X(tf.ox + p.x0), tf.Y(tf.oy + p.y0), tf.X(tf.ox + p.x1), tf.Y(tf.oy + p.y1));
        }
        out.push_back({&p, b});
    }
    return out;
}

static void Run(const Case& c, float scale, bool detail2) {
    const float aspect = float(c.w) / float(c.h);
    const float frame = std::round(480.0f * std::max(aspect, 4.0f / 3.0f));
    const float side = (frame - 640.0f) / 2.0f;
    const float px = float(c.w) / frame, py = float(c.h) / 480.0f;  // pixels a frame unit, across and down
    // the view rectangle SCR_DrawNetGraph gets: the whole frame (logged 0 0 854 480 at 16:9)
    const pcnetgraph::Screen screen = pcnetgraph::ScreenOf(0, 0, frame, 480);
    const std::vector<Piece> pieces = Engine(detail2);
    // the first frame from the estimate, then the measured box (relative to the origin) places the next
    pcnetgraph::Origin o = pcnetgraph::Place(pcnetgraph::Estimate(300, 64, detail2 ? 30 : 24, scale), screen);
    pcnetgraph::Box measured;
    for (int n = 0; n < 2; n++) {
        const pcnetgraph::Transform tf{float(o.x), float(o.y), scale};
        for (const Drawn& d : DrawFrame(pieces, tf, side, true))
            measured.Add(d.frame.x0 - tf.ox, d.frame.y0 - tf.oy, d.frame.x1 - tf.ox, d.frame.y1 - tf.oy);
        o = pcnetgraph::Place(measured, screen);
    }
    const pcnetgraph::Transform tf{float(o.x), float(o.y), scale};
    const std::vector<Drawn> drawn = DrawFrame(pieces, tf, side, true);
    char what[200];
    pcnetgraph::Box parent;
    parent.Add(tf.ox + measured.x0, tf.oy + measured.y0, tf.ox + measured.x1, tf.oy + measured.y1);
    for (const Drawn& d : drawn) {
        std::snprintf(what, sizeof what, "%s: %s inside the net graph", c.name, d.piece->what);
        Check(d.frame.x0 >= parent.x0 - 0.01f && d.frame.x1 <= parent.x1 + 0.01f && d.frame.y0 >= parent.y0 - 0.01f &&
                  d.frame.y1 <= parent.y1 + 0.01f, what);
        std::snprintf(what, sizeof what, "%s: %s inside the screen (no text cut)", c.name, d.piece->what);
        Check(d.frame.x0 >= screen.left && d.frame.x1 <= screen.right && d.frame.y0 >= screen.top &&
                  d.frame.y1 <= screen.bottom, what);
        // the same transform for every piece: its offset from the origin is the engine's, scaled
        std::snprintf(what, sizeof what, "%s: %s at its place in the parent", c.name, d.piece->what);
        Check(std::fabs((d.frame.x0 - tf.ox) - d.piece->x0 * scale) < 0.01f &&
                  std::fabs((d.frame.y0 - tf.oy) - d.piece->y0 * scale) < 0.01f, what);
    }
    const float right = parent.x1 * px, bottom = parent.y1 * py, left = parent.x0 * px, top = parent.y0 * py;
    const float mr = float(c.w) - right, mb = float(c.h) - bottom;
    const float lo = pcnetgraph::kMargin * py - 0.5f, hi = (pcnetgraph::kMargin + 1) * std::max(px, py) + 0.5f;
    std::snprintf(what, sizeof what, "%s at %.0f%%%s: right margin %.1f px, bottom %.1f px (wanted %.1f..%.1f)", c.name,
                  scale * 100, detail2 ? ", Detail 2" : "", mr, mb, lo, hi);
    std::printf("%s; the net graph %.0f,%.0f to %.0f,%.0f\n", what, left, top, right, bottom);
    Check(mr >= lo && mr <= hi && mb >= lo && mb <= hi, what);
    Check(left > float(c.w) / 2 && top > float(c.h) / 2, "in the bottom right quarter");
}

int main() {
    // the user's 1649 x 927 window (16:9 on a scaled desktop): the graph should end about 1633, 911
    Run({"1649x927", 1649, 927}, 0.5f, false);
    Run({"1649x927", 1649, 927}, 0.5f, true);
    Run({"1920x1080 16:9", 1920, 1080}, 0.5f, true);
    Run({"1680x1050 16:10", 1680, 1050}, 0.5f, true);
    Run({"2560x1440", 2560, 1440}, 0.5f, true);
    Run({"3840x2160 4K", 3840, 2160}, 0.5f, true);
    Run({"2560x1080 21:9", 2560, 1080}, 0.5f, true);
    Run({"1600x1200 4:3", 1600, 1200}, 0.5f, true);
    Run({"1920x1080 Original size", 1920, 1080}, 1.0f, true);
    Run({"1920x1080 Medium", 1920, 1080}, 0.75f, true);
    // split screen: each player's viewport (its own process's frame)
    Run({"split 1920x540", 1920, 540}, 0.5f, true);
    Run({"split quarter 960x540", 960, 540}, 0.5f, true);
    // the user's 1649 x 927 screenshot: the graph's right end and its rate labels 107 units apart (the side column)
    // when strings are given the frame x as is; with TextDrawX they share the parent's coordinates
    {
        const float frame = std::round(480.0f * 1649.0f / 927.0f), side = (frame - 640.0f) / 2.0f;
        const pcnetgraph::Transform tf{587, 458, 0.5f};
        const std::vector<Piece> pieces = Engine(true);
        const std::vector<Drawn> before = DrawFrame(pieces, tf, side, false), after = DrawFrame(pieces, tf, side, true);
        const float columns = before[1].frame.x1, label_before = before[6].frame.x0, label_after = after[6].frame.x0;
        std::printf("1649x927: the columns end at %.0f (%.0f px); the 30/s label from %.0f (%.0f px) before the fix, %.0f"
                    " after\n", columns, columns * 1649.0f / frame, label_before, label_before * 1649.0f / frame, label_after);
        // the screenshot: the graph ending about 1425 px, its labels about 1580 (here 1425 and 1582)
        Check(std::fabs(columns * 1649.0f / frame - 1425) < 6 && std::fabs(label_before * 1649.0f / frame - 1580) < 6,
              "the model reproduces the screenshot");
        Check(std::fabs(label_before - (tf.X(tf.ox + 250)) - side) < 0.01f, "the screenshot's split: the side column");
        Check(std::fabs(label_after - tf.X(tf.ox + 250)) < 0.01f && label_after + 21 <= columns,
              "the 30/s label inside the graph (the same transform)");
        pcnetgraph::Box b;
        for (const Piece& p : pieces) b.Add(p.x0 * 0.5f, p.y0 * 0.5f, p.x1 * 0.5f, p.y1 * 0.5f);
        const pcnetgraph::Origin o = pcnetgraph::Place(b, pcnetgraph::ScreenOf(0, 0, frame, 480));
        const float right = (float(o.x) + b.x1) * 1649.0f / frame, bottom = (float(o.y) + b.y1) * 927.0f / 480.0f;
        std::printf("1649x927: ends at %.0f, %.0f\n", right, bottom);
        Check(std::fabs(right - 1633) <= 3 && std::fabs(bottom - 911) <= 3, "1649x927: ends at about 1633, 911");
    }
    // the transform: about the origin, one scale
    const pcnetgraph::Transform t{500, 400, 0.5f};
    Check(t.X(500) == 500 && t.X(700) == 600 && t.Y(300) == 350, "the parent transform");
    // nothing cut short: the renderer's width for the net graph's strings is past any screen
    Check(pcnetgraph::kTextWidth > 4000, "the text's width cuts nothing");
    Check(pcnetgraph::TextEnd(500, 690) == 690 && pcnetgraph::TextEnd(500, 190) == 690, "a string's end");
    // a box too wide for the screen keeps to its left edge
    pcnetgraph::Box wide;
    wide.Add(-5, -69, 900, 29);
    const pcnetgraph::Origin ow = pcnetgraph::Place(wide, pcnetgraph::ScreenOf(0, 0, 640, 480));
    Check(float(ow.x) + wide.x0 >= pcnetgraph::kMargin - 1, "a box too wide keeps to the left edge");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
