// tests/pc_netgraph_layout_test.py builds and runs this: src/game/pc_netgraph_layout.h on its own. The net graph as
// one parent: the engine's own layout (as SCR_DrawNetGraph draws it: backdrop, columns, text lines, from its origin)
// through the parent transform, measured, placed, and its final bounds in a window's pixels
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
// SCR_DrawNetGraph at net_graphwidth 300, graphheight 64: the backdrop (0x6E657: x - 5, y - 69, 310 x 98), the
// columns, and its text at the 0.75 font (the widths a play-test logged: 42, 78, 129, 136; the fps at x + 100), lines
// 15 apart; a Detail 2 line as wide as the sv line, about 230
static std::vector<Piece> Engine(bool detail2) {
    std::vector<Piece> v = {{"backdrop", -5, -69, 305, 29, false},
                            {"columns", 0, -64, 300, 0, false},
                            {"fps", 100, -64, 142, -55, true},
                            {"in", 0, -64, 78, -55, true},
                            {"out", 0, -49, 129, -40, true},
                            {"loss", 0, -34, 136, -25, true},
                            {"rate", 0, 0, 42, 9, true}};
    if (detail2) v.push_back({"sv", 0, 14, 230, 23, true});
    return v;
}

struct Case {
    const char* name;
    int w, h;
};

// the window's pixels of a 2D-layout point: the frame is 480 lines and round(480 x aspect) units wide, its middle
// 640 columns the layout's 0..640
struct Pixels {
    float side, px, py;
    float X(float x) const { return (x + side) * px; }
    float Y(float y) const { return y * py; }
};

static void Run(const Case& c, float scale, bool detail2) {
    const float aspect = float(c.w) / float(c.h);
    const float frame = std::round(480.0f * std::max(aspect, 4.0f / 3.0f));
    const float side = (frame - 640.0f) / 2.0f;
    const Pixels pix{side, float(c.w) / frame, float(c.h) / 480.0f};
    const pcnetgraph::Screen screen = pcnetgraph::ScreenOf(side, 0, 0, side > 0 ? frame : 640, 480);
    const std::vector<Piece> pieces = Engine(detail2);
    // the first frame: an origin from the estimate; the pieces measured through the transform there
    pcnetgraph::Origin o = pcnetgraph::Place(pcnetgraph::Estimate(300, 64, detail2 ? 30 : 24, scale), screen);
    pcnetgraph::Box measured;
    for (int frame_no = 0; frame_no < 2; frame_no++) {
        pcnetgraph::Transform tf{float(o.x), float(o.y), scale};
        pcnetgraph::Box box;
        for (const Piece& p : pieces) {
            // a text's width comes from the renderer at the font's scale: the transform's
            const float x0 = tf.X(float(o.x) + p.x0), y0 = tf.Y(float(o.y) + p.y0);
            const float x1 = p.text ? pcnetgraph::TextEnd(x0, (p.x1 - p.x0) * scale) : tf.X(float(o.x) + p.x1);
            const float y1 = tf.Y(float(o.y) + p.y1);
            box.Add(x0 - tf.ox, y0 - tf.oy, x1 - tf.ox, y1 - tf.oy);
        }
        measured.Add(box);
        o = pcnetgraph::Place(measured, screen);  // the next frame's place
    }
    // the final frame: every piece inside the parent box, the parent inside the screen with its margins
    const pcnetgraph::Transform tf{float(o.x), float(o.y), scale};
    char what[200];
    for (const Piece& p : pieces) {
        const float x0 = tf.X(float(o.x) + p.x0), x1 = p.text ? x0 + (p.x1 - p.x0) * scale : tf.X(float(o.x) + p.x1);
        const float y0 = tf.Y(float(o.y) + p.y0), y1 = tf.Y(float(o.y) + p.y1);
        std::snprintf(what, sizeof what, "%s: %s inside the net graph", c.name, p.what);
        Check(x0 - tf.ox >= measured.x0 - 0.01f && x1 - tf.ox <= measured.x1 + 0.01f &&
                  y0 - tf.oy >= measured.y0 - 0.01f && y1 - tf.oy <= measured.y1 + 0.01f, what);
        std::snprintf(what, sizeof what, "%s: %s inside the screen", c.name, p.what);
        Check(x0 >= screen.left && x1 <= screen.right && y0 >= screen.top && y1 <= screen.bottom, what);
    }
    const float right = pix.X(float(o.x) + measured.x1), bottom = pix.Y(float(o.y) + measured.y1);
    const float left = pix.X(float(o.x) + measured.x0), top = pix.Y(float(o.y) + measured.y0);
    const float mr = float(c.w) - right, mb = float(c.h) - bottom;
    const float lo = pcnetgraph::kMargin * pix.py - 0.5f, hi = (pcnetgraph::kMargin + 1) * std::max(pix.px, pix.py) + 0.5f;
    std::snprintf(what, sizeof what, "%s at %.0f%%%s: right margin %.1f px, bottom %.1f px (wanted %.1f..%.1f)", c.name,
                  scale * 100, detail2 ? ", Detail 2" : "", mr, mb, lo, hi);
    std::printf("%s; the net graph %.0f,%.0f to %.0f,%.0f\n", what, left, top, right, bottom);
    Check(mr >= lo && mr <= hi && mb >= lo && mb <= hi, what);
    Check(left > float(c.w) / 2 && top > float(c.h) / 2, "in the bottom right quarter");
    // one scale for all of it: the graph's width and the widest text's both scaled by it
    Check(std::fabs((tf.X(float(o.x) + 300) - tf.X(float(o.x))) - 300 * scale) < 0.01f, "the graph scaled");
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
    // the user's numbers: the whole net graph ends near 1633, 911 at 1649 x 927
    {
        const float frame = std::round(480.0f * 1649.0f / 927.0f), side = (frame - 640.0f) / 2.0f;
        const pcnetgraph::Screen s = pcnetgraph::ScreenOf(side, 0, 0, frame, 480);
        pcnetgraph::Box b;
        for (const Piece& p : Engine(true)) b.Add(p.x0 * 0.5f, p.y0 * 0.5f, p.x1 * 0.5f, p.y1 * 0.5f);
        const pcnetgraph::Origin o = pcnetgraph::Place(b, s);
        const float right = (float(o.x) + b.x1 + side) * 1649.0f / frame, bottom = (float(o.y) + b.y1) * 927.0f / 480.0f;
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
    const pcnetgraph::Origin ow = pcnetgraph::Place(wide, pcnetgraph::ScreenOf(0, 0, 0, 640, 480));
    Check(float(ow.x) + wide.x0 >= pcnetgraph::kMargin - 1, "a box too wide keeps to the left edge");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
