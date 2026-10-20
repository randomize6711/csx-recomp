// Mouse navigation in the game's own menus (FINDINGS.md "Mouse in the menus"). The window hands the pointer here in the
// menus' 640x480 coordinates (widescreen and resolution already taken out) and the clicks and wheel notches; the game's
// own mechanisms do the rest:
// - a menu table (the open screen 0x5523D4, its table at +0x380): the rows are where the table draw (0xCD030) puts
//   them: x from +0x2C to +0x34, y from +0x30 in steps of int(font height 0x37A81C x text scale +0x12C) + row spacing
//   +0x128, from the first row shown (+0x120); moving the pointer onto an enabled row (row +4 bit 0 clear) selects it
//   through the table's own move (0xD8360: the selection +0x9C and the scrolling follow), as the d-pad would;
// - a click is the pad's A (activates the row: open, accept, the row's command), on a list row's arrows (the ends
//   of the table's highlight image, +0x130: image, x, y, width, height) the d-pad's left or right, elsewhere on a
//   list row it only selects it and a double click is A (accepting the screen); the right button
//   is B (back, cancel); the wheel is the d-pad's up and down; a click on the help bar's buttons (the screen's
//   0,411,640x69 BackgroundImage, its name listing them right to left: Cancel_Accept_Reset) is that button;
// - the buy menus (MenuType "buymenu"): a pie the left stick picks from; the stick points from the pie's centre to
//   the pointer while it is on the ring, a click is A with it, the ammo tabs above are the triggers.
// The presses go through the pad as the game reads it (GameInfo::pad), so sounds, transitions and commands are the
// game's. Pointer movement only acts while it moves: keyboard and pad navigation keep their selection meanwhile.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/csx.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {

using kernel::R32;

constexpr uint32_t kCurrentScreen = 0x5523D4;
constexpr uint32_t kFontHeight = 0x37A81C;  // the UI font's line height in pixels (int)
constexpr uint32_t kTableMove = 0xD8360;    // move the selection by [esp+4] rows, table in esi; cdecl
constexpr uint32_t kMenuSound = 0xC5A00;    // play a menu sound (name, 0); cdecl
// the buy menus' pie, centre and ring in menu pixels (measured on the CT and T pies' art, all of them alike)
constexpr float kPieX = 227, kPieY = 251, kPieInner = 42, kPieOuter = 160;

// pad buttons as input::Pad keeps them
enum : uint16_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8 };
enum { kA = 0, kB = 1, kX = 2, kY = 3, kLT = 6, kRT = 7 };

struct ScreenUi {
    std::string type;                 // MenuType
    std::vector<std::string> legend;  // the help bar's buttons, rightmost first
};
std::map<std::string, ScreenUi> g_screens;

struct Press {
    uint16_t buttons = 0;
    int analog = -1;
};

std::mutex g_lock;
float g_x = -1, g_y = -1;  // the pointer, menu pixels; -1 while outside the window
bool g_moved = false;
std::deque<int> g_clicks;  // 1 left, 2 right; wheel notches as 100 + n
// on the game thread
std::deque<Press> g_presses;
int g_press_frames = 0;  // > 0: the front press is held; < 0: released, frames until the next
std::atomic<uint32_t> g_pad_buttons{0}, g_pad_analog{0xFF};  // what the pad filter adds this frame
std::atomic<int32_t> g_stick{0};                            // lx << 16 | ly, the pie's stick
std::atomic<bool> g_stick_on{false};

std::string GuestString(uint32_t va) {
    std::string s;
    for (int i = 0; va && i < 128; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}
float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}

// the table's rows on screen: index of the row under (x, y), -1 for none; its rectangle in *rx0 etc.
struct RowHit {
    int row = -1;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
RowHit HitRow(uint32_t table, float x, float y) {
    RowHit h;
    uint32_t rows = R32(table + 0x60), end = R32(table + 0x64);
    int count = rows ? int((end - rows) / 0x20) : 0;
    if (count <= 0) return h;
    int font = int(R32(kFontHeight));
    int height = int(float(font) * F(table + 0x12C)) + int(R32(table + 0x128));
    if (height <= 0) return h;
    float left = F(table + 0x2C), top = F(table + 0x30), right = F(table + 0x34), bottom = F(table + 0x38);
    int first = int(R32(table + 0x120));
    int shown = int(bottom - top) / height;
    int last = std::min(first + shown, count - 1);
    // the highlight image may reach past the table (the main menu's bar starts 40 pixels left of it)
    if (uint32_t img = R32(table + 0x130); img) {
        left = std::min(left, F(table + 0x2C) + F(table + 0x134));
        right = std::max(right, F(table + 0x2C) + F(table + 0x134) + F(table + 0x13C));
    }
    if (x < left || x >= right || y < top) return h;
    int i = first + int((y - top) / float(height));
    if (i > last || i >= count) return h;
    h.row = i;
    h.x0 = F(table + 0x2C);
    h.y0 = top + float((i - first) * height);
    h.x1 = h.x0 + (right - left);
    h.y1 = h.y0 + float(height);
    return h;
}
bool RowEnabled(uint32_t table, int i) { return !(R32(R32(table + 0x60) + uint32_t(i) * 0x20 + 4) & 1); }
// a row whose cells include a list of choices (a setting with arrows)
bool RowIsList(uint32_t table, int i) {
    uint32_t row = R32(table + 0x60) + uint32_t(i) * 0x20;
    for (uint32_t c = R32(row + 0xC), e = R32(row + 0x10); c && c + 0x78 <= e; c += 0x78)
        if ((R32(c + 0x10) - R32(c + 0xC)) / 0x1C > 1) return true;
    return false;
}

void Queue(Press p) { g_presses.push_back(p); }

// a selection move through the table's own code, with the screen's HighlightSound (+0x10) when it moved: the main
// menus ("superlauncher") have one and play it from their stick pieces (0xCAC90); the generic tables have none and
// the d-pad moves them silently
void Select(X86Context& ctx, uint32_t screen, uint32_t table, int row) {
    int sel = int(R32(table + 0x9C));
    if (row == sel || !RowEnabled(table, row)) return;
    uint32_t esi = ctx.esi;
    ctx.esi = table;
    kernel::CallGuest(ctx, kTableMove, {uint32_t(row - sel)});
    ctx.esi = esi;
    uint32_t sound = R32(screen + 0x10);
    if (int(R32(table + 0x9C)) == sel || !sound) return;
    kernel::CallGuest(ctx, kMenuSound, {sound, 0});
    LOGI("pc mouse: highlight %s", static_cast<const char*>(kernel::HostPtr(sound)));
}

void Click(X86Context& ctx, uint32_t screen, const ScreenUi* ui, float x, float y, bool twice) {
    // the help bar
    if (ui && !ui->legend.empty() && y >= 412 && y < 446) {
        static const float slot[3][2] = {{488, 585}, {385, 482}, {292, 380}};
        for (size_t k = 0; k < ui->legend.size() && k < 3; k++) {
            if (x < slot[k][0] || x >= slot[k][1]) continue;
            const std::string& b = ui->legend[k];
            int button = b == "Accept" || b == "Select" || b == "Join" ? kA
                       : b == "Cancel" || b == "Back"                  ? kB
                       : b == "Reset" || b == "REMOVE"                 ? kX
                                                                       : -1;
            if (button >= 0) Queue({0, button});
            return;
        }
    }
    if (ui && ui->type == "buymenu") {
        float dx = x - kPieX, dy = kPieY - y, r = std::sqrt(dx * dx + dy * dy);
        if (r >= kPieInner && r < kPieOuter) Queue({0, kA});
        else if (y < kPieY - 100 && r >= kPieOuter - 10 && r < kPieOuter + 70) Queue({0, x < kPieX ? kLT : kRT});
        return;
    }
    uint32_t table = R32(screen + 0x380);
    if (!table) return;
    RowHit h = HitRow(table, x, y);
    if (h.row < 0 || !RowEnabled(table, h.row)) return;
    Select(ctx, screen, table, h.row);
    if (RowIsList(table, h.row) && R32(table + 0x130)) {
        // the arrows at the ends of the highlight image
        float a0 = F(table + 0x2C) + F(table + 0x134), a1 = a0 + F(table + 0x13C);
        float arrow = std::min(28.0f, (a1 - a0) / 4);
        if (x >= a0 && x < a0 + arrow) Queue({kLeft, -1});
        else if (x >= a1 - arrow && x < a1) Queue({kRight, -1});
        // elsewhere on a setting's row: selected; a double click is A (it accepts the whole screen: the in-game
        // screens have no help bar to click)
        else if (twice) Queue({0, kA});
        return;
    }
    Queue({0, kA});
}

}  // namespace

// the screens' menu types and help bars, from the menu text as it loads (pc_menu.cpp's PcUiLoadText)
void PcMouseLearnUi(const std::string& text) {
    std::istringstream in(text);
    std::string line, screen;
    int depth = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c = line.find("//");
        std::string code = c == std::string::npos ? line : line.substr(0, c);
        if (depth == 0) {
            size_t a = code.find_first_not_of(" \t");
            if (a != std::string::npos && code[a] != '{' && code[a] != '}')
                screen = code.substr(a, code.find_last_not_of(" \t") - a + 1);
        }
        if (depth == 1 && !screen.empty()) {
            std::istringstream w(code);
            std::string key, value;
            w >> key;
            size_t q0 = code.find('"'), q1 = q0 == std::string::npos ? q0 : code.find('"', q0 + 1);
            if (q1 != std::string::npos) value = code.substr(q0 + 1, q1 - q0 - 1);
            if (key == "MenuType") g_screens[screen].type = value;
            if (key == "BackgroundImage") {
                std::istringstream v(value);
                std::string name;
                std::vector<float> n;
                v >> name;
                for (float f; v >> f;) n.push_back(f);
                if (n.size() >= 8 && n[n.size() - 4] == 0 && n[n.size() - 3] == 411 && n[n.size() - 2] == 640) {
                    std::vector<std::string> parts;
                    for (size_t at = 0; at <= name.size();) {
                        size_t e = name.find('_', at);
                        if (e == std::string::npos) e = name.size();
                        parts.push_back(name.substr(at, e - at));
                        at = e + 1;
                    }
                    g_screens[screen].legend = parts;
                }
            }
        }
        for (char ch : code) {
            if (ch == '{') depth++;
            if (ch == '}') depth = std::max(0, depth - 1);
        }
    }
}

namespace csx {

void MenuPointer(float x, float y) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (x != g_x || y != g_y) g_moved = true;
    g_x = x;
    g_y = y;
}

void MenuClick(int button) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_clicks.push_back(button);
}

void MenuWheel(int notches) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (int i = 0; i < std::abs(notches) && i < 8; i++) g_clicks.push_back(notches > 0 ? 101 : 99);
}

// GameInfo::pad: the presses and the pie's stick added to the polled pad; a pad or keyboard stick or d-pad in use
// takes the pie's stick back
void MenuPad(int port, input::Pad& p) {
    if (port != 0) return;
    p.buttons |= uint16_t(g_pad_buttons.load());
    if (uint32_t a = g_pad_analog.load(); a < 8) p.analog[a] = 255;
    if (g_stick_on.load()) {
        if (p.lx || p.ly || (p.buttons & 15 & ~g_pad_buttons.load())) {
            g_stick_on = false;
        } else {
            int32_t s = g_stick.load();
            p.lx = int16_t(s >> 16);
            p.ly = int16_t(s & 0xFFFF);
        }
    }
}

}  // namespace csx

namespace {
bool ui_known(const std::string& name) { return g_screens.count(name) != 0; }
}  // namespace

// on the game thread every frame (pc_keys.cpp's PcKeysFrame)
void PcMouseFrame(X86Context& ctx) {
    // the press being made: held three frames, then two released before the next
    if (g_press_frames > 0 && --g_press_frames == 0) {
        g_presses.pop_front();
        g_pad_buttons = 0;
        g_pad_analog = 0xFF;
        g_press_frames = -2;
    } else if (g_press_frames < 0) {
        g_press_frames++;
    }
    if (g_press_frames == 0 && !g_presses.empty()) {
        g_pad_buttons = g_presses.front().buttons;
        g_pad_analog = g_presses.front().analog < 0 ? 0xFFu : uint32_t(g_presses.front().analog);
        g_press_frames = 3;
    }

    float x, y;
    bool moved;
    std::deque<int> clicks;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        x = g_x;
        y = g_y;
        moved = std::exchange(g_moved, false);
        clicks.swap(g_clicks);
    }
    uint32_t screen = R32(kCurrentScreen);
    static std::string shown;  // the screen last logged, cleared in play
    if (!screen) {
        shown.clear();
        g_stick_on = false;
        if (!g_presses.empty() && g_press_frames <= 0) g_presses.clear();
        return;
    }
    std::string name = GuestString(R32(screen));
    if (name != shown) {
        shown = name;
        char rows[96] = "";
        if (uint32_t t = R32(screen + 0x380)) {
            uint32_t r0 = R32(t + 0x60), r1 = R32(t + 0x64);
            int h = int(float(int(R32(kFontHeight))) * F(t + 0x12C)) + int(R32(t + 0x128));
            std::snprintf(rows, sizeof rows, ", %d rows from %.0f %.0f, %d high, %.0f wide",
                          r0 ? int((r1 - r0) / 0x20) : 0, F(t + 0x2C), F(t + 0x30), h, F(t + 0x34) - F(t + 0x2C));
        }
        LOGI("pc mouse: screen %s%s%s", name.c_str(), ui_known(name) ? "" : " (no menu data)", rows);
    }
    auto it = g_screens.find(name);
    const ScreenUi* ui = it == g_screens.end() ? nullptr : &it->second;
    uint32_t table = R32(screen + 0x380);
    if (moved && x >= 0) {
        if (ui && ui->type == "buymenu") {
            float dx = x - kPieX, dy = kPieY - y, r = std::sqrt(dx * dx + dy * dy);
            if (r >= kPieInner && r < kPieOuter) {
                g_stick = int32_t(int16_t(dx / r * 32767)) << 16 | uint16_t(int16_t(dy / r * 32767));
                g_stick_on = true;
            } else {
                g_stick_on = false;
            }
        } else if (table) {
            RowHit h = HitRow(table, x, y);
            if (h.row >= 0) Select(ctx, screen, table, h.row);
        }
    }
    for (int c : clicks) {
        if ((c == 1 || c == 11) && x >= 0) Click(ctx, screen, ui, x, y, c == 11);
        else if (c == 2) Queue({0, kB});
        else if (c == 101 || c == 99) Queue({c == 101 ? kUp : kDown, -1});
    }
    static uint32_t logged = 0;
    if (!clicks.empty() || (moved && screen != logged)) {
        logged = screen;
        LOGI("pc mouse: %s at %.0f %.0f, selection %d%s", name.c_str(), x, y, table ? int(R32(table + 0x9C)) : -1,
             g_stick_on.load() ? ", pie stick" : "");
    }
}
