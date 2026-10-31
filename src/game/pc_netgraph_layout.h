// The net graph's place (src/game/pc_hud.cpp; tested on its own by tests/pc_netgraph_layout_test.py): the box of
// everything SCR_DrawNetGraph drew (its backdrop, columns and text, measured as it draws), relative to the origin it
// was given, set kMargin inside the screen's bottom right corner. No game memory: 2D units (480 lines a frame).
#pragma once
#include <algorithm>
#include <cmath>

namespace pcnetgraph {

// 7 units of the 480-line 2D layout: 14 pixels at 927 lines, 16 at 1080, 30 at 2160 (it scales with the screen)
inline constexpr float kMargin = 7.0f;

struct Box {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    bool Valid() const { return x1 >= x0 && y1 >= y0; }
    void Add(float ax0, float ay0, float ax1, float ay1) {
        x0 = std::min(x0, ax0), y0 = std::min(y0, ay0), x1 = std::max(x1, ax1), y1 = std::max(y1, ay1);
    }
    void Add(const Box& b) {
        if (b.Valid()) Add(b.x0, b.y0, b.x1, b.y1);
    }
};

// the screen in the 2D layout: a wider frame keeps 640 columns in the middle with a side column either side
struct Screen {
    float left, top, right, bottom;
};
inline Screen ScreenOf(float side, float view_x, float view_y, float view_w, float view_h) {
    if (side > 0) return {-side, view_y, 640.0f + side, view_y + view_h};
    return {view_x, view_y, view_x + view_w, view_y + view_h};
}

// the origin that puts the box (relative to its origin) kMargin inside the screen's right and bottom edges, kept
// inside the left and top ones when it is larger than the screen
struct Origin {
    int x, y;
};
inline Origin Place(const Box& rel, const Screen& s, float margin = kMargin) {
    float x = s.right - margin - rel.x1, y = s.bottom - margin - rel.y1;
    x = std::max(x, s.left + margin - rel.x0);
    y = std::max(y, s.top + margin - rel.y0);
    return {int(std::floor(x)), int(std::floor(y))};
}

// before the first measurement: the engine's own layout (its backdrop from x - 5, y - graphheight - 5 to
// x + graph + 5, y + 29; the text from x, about 6 units a character)
inline Box Estimate(int graph_width, float graph_height, int text_chars) {
    Box b;
    b.Add(-5.0f, -graph_height - 5.0f, std::max(float(graph_width) + 5.0f, 6.0f * float(text_chars)), 29.0f);
    return b;
}

// a string's right end from what the text renderer answered: the x after it (the engine API's own answer), or its
// width (an answer smaller than the x it started at)
inline float TextEnd(float x, float answer) { return answer >= x ? answer : x + answer; }

}  // namespace pcnetgraph
