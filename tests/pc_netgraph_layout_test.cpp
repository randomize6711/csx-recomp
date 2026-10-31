// tests/pc_netgraph_layout_test.py builds and runs this: src/game/pc_netgraph_layout.h on its own. The net graph's
// final bounds in a window's pixels, from a measured box, at the resolutions and layouts the user plays
#include <cmath>
#include <cstdio>

#include "game/pc_netgraph_layout.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

// a viewport of W x H pixels: the 2D layout's frame is 480 lines and W/H x 480 units wide (pc_wide.cpp FrameWidth,
// 4:3 and wider), the 640 columns in its middle
struct Case {
    const char* name;
    int w, h;
};

// the box SCR_DrawNetGraph draws relative to its origin: at 50% (150 columns) its backdrop -5..155 x -69..29, its
// text from 0 to 190 (Detail 2's longest line, as measured), 12 high from 14 lines under the origin
static pcnetgraph::Box Drawn() {
    pcnetgraph::Box b;
    b.Add(-5, -69, 155, 29);   // backdrop
    b.Add(0, -64, 150, 0);     // columns
    b.Add(0, -64, 190, -52);   // the fps line
    b.Add(0, 14, 170, 26);     // the last line
    return b;
}

// the margin wanted: kMargin units (7 of 480 lines), one more at most for the origin's whole units
static void Run(const Case& c) {
    const float aspect = float(c.w) / float(c.h);
    const float frame = std::round(480.0f * std::max(aspect, 4.0f / 3.0f));
    const float side = (frame - 640.0f) / 2.0f;
    const pcnetgraph::Box rel = Drawn();
    // the engine's view rectangle at a wider frame: the frame's own width (logged in a play-test: 0 0 854 480)
    const pcnetgraph::Screen s = pcnetgraph::ScreenOf(side, 0, 0, side > 0 ? frame : 640, 480);
    const pcnetgraph::Origin o = pcnetgraph::Place(rel, s);
    const float py = float(c.h) / 480.0f, px = float(c.w) / frame;  // pixels a unit, down and across
    const float margin_lo = pcnetgraph::kMargin * py - 0.5f, margin_hi = (pcnetgraph::kMargin + 1) * std::max(px, py) + 0.5f;
    const float right = (float(o.x) + rel.x1 + side) * px, bottom = (float(o.y) + rel.y1) * py;
    const float left = (float(o.x) + rel.x0 + side) * px, top = (float(o.y) + rel.y0) * py;
    const float mr = float(c.w) - right, mb = float(c.h) - bottom;
    char what[160];
    std::snprintf(what, sizeof what, "%s: right margin %.1f px, bottom %.1f px (wanted %.1f..%.1f)", c.name, mr, mb,
                  margin_lo, margin_hi);
    std::printf("%s; box %.0f,%.0f to %.0f,%.0f\n", what, left, top, right, bottom);
    Check(mr >= margin_lo && mr <= margin_hi && mb >= margin_lo && mb <= margin_hi, what);
    Check(left > float(c.w) / 2 && top > float(c.h) / 2, "in the bottom right quarter, not the middle");
}

int main() {
    // the user's screenshot: 1649 x 927 (a 16:9 window on a scaled desktop); the graph ends about 1633, 911
    Run({"1649x927", 1649, 927});
    {
        const float aspect = 1649.0f / 927.0f, frame = std::round(480.0f * aspect), side = (frame - 640.0f) / 2.0f;
        const pcnetgraph::Box rel = Drawn();
        const pcnetgraph::Origin o = pcnetgraph::Place(rel, pcnetgraph::ScreenOf(side, 0, 0, frame, 480));
        const float right = (float(o.x) + rel.x1 + side) * (1649.0f / frame), bottom = (float(o.y) + rel.y1) * (927.0f / 480.0f);
        Check(std::fabs(right - 1633) <= 3 && std::fabs(bottom - 911) <= 3, "1649x927: ends at about 1633, 911");
    }
    // the margin scales with the screen (7 of 480 units): about 16 px at 1080 lines, 21 at 1440, 31 at 2160
    Run({"1920x1080", 1920, 1080});
    Run({"2560x1440", 2560, 1440});
    Run({"3840x2160", 3840, 2160});
    Run({"3440x1440 ultrawide", 3440, 1440});
    Run({"1600x1200 4:3", 1600, 1200});
    // split screen: each player's own viewport (its process's frame), top and bottom halves of 1920x1080, quarters
    Run({"split 1920x540", 1920, 540});
    Run({"split quarter 960x540", 960, 540});
    // a box wider than the screen stays inside its left edge
    {
        pcnetgraph::Box wide;
        wide.Add(-5, -69, 900, 29);
        const pcnetgraph::Origin o = pcnetgraph::Place(wide, pcnetgraph::ScreenOf(0, 0, 0, 640, 480));
        Check(float(o.x) + wide.x0 >= pcnetgraph::kMargin - 1, "a box too wide keeps to the left edge");
    }
    // the renderer's answer: the x after the text, or its width
    Check(pcnetgraph::TextEnd(500, 690) == 690 && pcnetgraph::TextEnd(500, 190) == 690, "a string's end");
    // before any measurement: the estimate covers the backdrop
    const pcnetgraph::Box e = pcnetgraph::Estimate(150, 64, 24);
    Check(e.x0 == -5 && e.y0 == -69 && e.y1 == 29 && e.x1 >= 155, "the estimate before a measurement");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
