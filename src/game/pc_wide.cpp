// Widescreen (Options > PC Options > Aspect Ratio, FINDINGS.md "Widescreen"). The game renders into a 640x480 view
// (a 640x960 back buffer, 1x2 super sampled); for a wider aspect the back buffer and the engine's window rectangle
// are made wider at the same 480 lines, so the 3D view covers the whole frame at the display's aspect:
// - the present parameters' width (0x553620) before Direct3D_CreateDevice (0xAF250);
// - the window rectangle (0x6CE210..0x6CE21C, centre 0x6CE220) the engine takes from the game interface (0xAF192);
//   GL_BeginRendering derives the GL size from it every frame (glwidth 0x6CE3C8, glheight 0x6CE274), and from that
//   SCR_CalcRefdef the 3D view rectangle (r_refdef.vrect 0x6E7A20..0x6E7A2C);
// - the FOV turned Hor+ where the projection and the culling planes are built: scr_fov_value is horizontal degrees
//   for a 4:3 view, so for a wider view it becomes the horizontal angle that keeps the 4:3 vertical angle
//   (R_SetupGL 0x4E112 and R_SetFrustum 0x4DAB9, both on the 3D view rectangle); the game's own value (the player's
//   FOV, a zoom) stays as it is. At 4:3 nothing is changed.
// The 2D layers keep their 640x480 layout, unstretched in the middle 640 columns (a 4:3 area as tall as the frame):
// - the client's screen width (the present parameters' width again after the device is made);
// - the 2D projection (GL_Set2D: the engine's 2D and the VGUI paint with the HUD, menus and scoreboard);
// - the paths in screen pixels, which no projection moves: the GL layer's screen rectangle (the sniper scope, the
//   menus' backdrops, the loading screen), the text renderer and the video overlay (movies).
// Full-screen backdrops reach the frame's edges from their own edge columns (the scope's surround stays black), and
// the screen fade (the flashbang's white-out), drawn over the GL size inside the VGUI paint, covers the whole frame.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;
using kernel::W32;

constexpr uint32_t kPresentParams = 0x553620;  // D3DPRESENT_PARAMETERS: BackBufferWidth, BackBufferHeight
constexpr uint32_t kWindowRect = 0x6CE210;     // left, top, right, bottom
constexpr uint32_t kWindowCentreX = 0x6CE220;
constexpr uint32_t kGlWidth = 0x6CE3C8, kGlHeight = 0x6CE274;
constexpr uint32_t kQglOrtho = 0x6A86C0, kQglViewport = 0x6A8D3C;  // the GL layer's glOrtho, glViewport
constexpr uint32_t kGlX = 0x6CE528, kGlY = 0x6CE27C;
constexpr uint32_t kD3dDevicePtr = 0x316FE8, kDevGlViewport = 0xEE0;  // D3D device, its viewport
constexpr uint32_t kGlScreenRect = 0x6F080;  // the GL layer's screen rectangle
constexpr uint32_t kViewWidth = 0x6E7A28, kViewHeight = 0x6E7A2C;  // r_refdef.vrect width, height

bool fade_widened = false;  // PcWideFade changed the GL width

// the frame's width at 480 lines, chosen once (the window rectangle's setup runs before the device's)
uint32_t FrameWidth() {
    static uint32_t width = 0;
    if (width) return width;
    std::string name = pc::Get("aspect", "auto");
    float aspect = 4.0f / 3.0f;
    if (name == "16:9") aspect = 16.0f / 9.0f;
    else if (name == "16:10") aspect = 16.0f / 10.0f;
    else if (name == "21:9") aspect = 64.0f / 27.0f;  // 2560x1080 / 3440x1440 panels sold as 21:9
    else if (name == "auto")  // the display's (the window sets it), 4:3 headless
        if (const char* d = std::getenv("CSX_DISPLAY_ASPECT")) aspect = float(std::atof(d));
    aspect = aspect > 0.0f ? std::clamp(aspect, 4.0f / 3.0f, 2.5f) : 4.0f / 3.0f;  // 32:9 gets 2.5:1, letterboxed
    width = uint32_t(std::lround(480.0f * aspect / 2.0f)) * 2;
    LOGI("pc wide: aspect %s (%.4f), frame %ux480", name.c_str(), aspect, width);
    return width;
}

// horizontal degrees for a 4:3 view, as wide as w:h keeping the 4:3 view's vertical angle
float HorPlus(float fov, int w, int h) {
    if (w <= 0 || h <= 0 || w * 3 == h * 4 || !(fov > 0.0f && fov < 180.0f)) return fov;
    const double rad = 3.14159265358979323846 / 180.0;
    return float(2.0 * std::atan(std::tan(fov * rad / 2.0) * (double(w) / h) * 0.75) / rad);
}

float AsFloat(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
uint32_t Bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
}  // namespace

// [[hook]] at 0xAF223: the present parameters before Direct3D_CreateDevice
void PcWideDevice(X86Context&, uint8_t*) {
    if (R32(kPresentParams) == 640 && R32(kPresentParams + 4) == 480) W32(kPresentParams, FrameWidth());
}

// [[hook]] at 0xAF255: after Direct3D_CreateDevice. The client reads the present parameters' width (0x553620) as its
// screen width (about a hundred places: HUD layout, the "%d_pain.spr" resolution sprites), so it goes back to 640:
// the HUD keeps its 640x480 layout, which the 2D projection below centres
void PcWideDeviceMade(X86Context&, uint8_t*) {
    if (R32(kPresentParams) == FrameWidth() && FrameWidth() != 640) W32(kPresentParams, 640);
}

// [[hook]] at 0xAF1DF: the end of the window rectangle's setup
void PcWideRect(X86Context&, uint8_t*) {
    uint32_t left = R32(kWindowRect), w = FrameWidth();
    if (w == 640 || R32(kWindowRect + 8) - left != 640) return;
    W32(kWindowRect + 8, left + w);
    W32(kWindowCentreX, left + w / 2);
}

// [[hook]] at 0x4E118 (R_SetupGL, edx = scr_fov_value for CalcFovY on the view rectangle)
void PcWideFovView(X86Context& ctx, uint8_t*) {
    int w = int(R32(kViewWidth)), h = int(R32(kViewHeight));
    if (FrameWidth() == 640) return;
    float fov = AsFloat(ctx.edx), wide = HorPlus(fov, w, h);
    ctx.edx = Bits(wide);
    static float shown = 0;
    if (fov != shown) {
        shown = fov;
        LOGI("pc wide: view fov %g -> %.1f (%dx%d)", fov, wide, w, h);
    }
}

// [[hook]] at 0x4DAD6 (R_SetFrustum, before CalcFovY: ecx and [esp+0x10] = scr_fov_value for the side planes, [esp]
// and [esp+4] = the GL width and height as floats for CalcFovY's vertical angle). R_SetFrustum runs inside the VGUI
// paint (0xB4560), where the GL size is the 640x480 root panel's, so on a wider frame both the angle and the size
// come from the 3D view rectangle instead, as R_SetupGL's projection does: the culling planes match the projection
void PcWideFovFrustum(X86Context& ctx, uint8_t*) {
    int w = int(R32(kViewWidth)), h = int(R32(kViewHeight));
    if (FrameWidth() == 640 || w <= 0 || h <= 0) return;
    float fov = AsFloat(ctx.ecx), wide = HorPlus(fov, w, h);
    ctx.ecx = Bits(wide);
    W32(ctx.esp + 0x10, Bits(wide));
    W32(ctx.esp, Bits(float(w)));
    W32(ctx.esp + 4, Bits(float(h)));
    static float shown = 0;
    if (fov != shown) {
        shown = fov;
        LOGI("pc wide: culling fov %g -> %.1f (%dx%d)", fov, wide, w, h);
    }
}

// [[hook]] at 0x58FA7, on_true 0x58FEB (GL_Set2D 0x58F70: glViewport(glx, gly, glwidth, glheight), then the
// projection made glOrtho(0, glwidth, glheight, 0, -99999, 99999)). The 2D layers are laid out in 640x480: the
// engine's own 2D (GL size = the frame) and the VGUI paint (0xB4560: the HUD, menus, scoreboard), which sets the GL
// size to its 640x480 root panel around its drawing. On a wider frame both get the whole frame as the viewport and a
// projection spanning the extra columns on both sides, so those 640 columns sit unstretched in the middle and what
// is drawn outside them (fills past the edges) still shows.
bool PcWide2D(X86Context& ctx, uint8_t*) {
    const int frame = int(FrameWidth());
    int x = int(R32(kGlX)), y = int(R32(kGlY)), w = int(R32(kGlWidth)), h = int(R32(kGlHeight));
    if (frame == 640 || h != 480 || x != 0 || (w != frame && w != 640)) return false;
    if (w == 640) kernel::CallGuest(ctx, R32(kQglViewport), {0, uint32_t(y), uint32_t(frame), uint32_t(h)});
    double side = (frame - 640) / 2.0, v[6] = {-side, 640.0 + side, double(h), 0.0, -99999.0, 99999.0};
    uint32_t args[12];
    std::memcpy(args, v, sizeof v);
    kernel::CallGuest(ctx, R32(kQglOrtho), args, 12);
    return true;
}

// whether 2D drawn now lands on the wide frame (the viewport the GL layer last set: x, y, w, h), not a texture
bool DrawingToFrame() {
    uint32_t vp = R32(kD3dDevicePtr) + kDevGlViewport;
    return FrameWidth() != 640 && R32(vp) == 0 && R32(vp + 8) == FrameWidth();
}

// [[hook]] at 0x4D877 (R_PolyBlend 0x4D860 with a fade to draw, FINDINGS.md "Flashbang") and 0x4DAA7 (its return).
// It draws the screen fade (the flashbang's white-out) over the GL size: glViewport(glx, gly, glwidth, glheight),
// glOrtho(0, glwidth, glheight, 0) and a quad from 0, 0 to glwidth, glheight. It runs inside the VGUI paint (0xB4560),
// where the GL size is the 640x480 root panel's, so on a wider frame it covered the left 640 columns only. While it
// draws, the GL width is the frame's (the whole screen, as 640 is at 4:3), and 640 again when it returns.
void PcWideFade(X86Context&, uint8_t*) {
    static bool shown = false;
    if (FrameWidth() == 640 || R32(kGlWidth) != 640 || R32(kGlHeight) != 480 || R32(kGlX) != 0) return;
    fade_widened = true;
    W32(kGlWidth, FrameWidth());
    if (!shown) {
        shown = true;
        LOGI("pc wide: screen fade over the %ux480 frame (drawn inside the 640x480 VGUI paint)", FrameWidth());
    }
}
void PcWideFadeDone(X86Context&, uint8_t*) {
    if (!fade_widened) return;
    fade_widened = false;
    W32(kGlWidth, 640);
}

// [[hook]] at 0x6F080: the GL layer's screen rectangle (x0, y0, x1, y1, s0, t0, s1, t1 floats in display pixels,
// drawn as XYZRHW vertices in the current colour and texture, so neither the projection nor the viewport moves it).
// Its callers lay it out in 640x480: the sniper scope's ring over the whole screen and its centre dot (client
// 0x1ACBF5 / 0x1ACC61), the loading screen (0xC2B53) and the menu's rectangles (0xCC24D). On a wider frame drawn to
// the back buffer it moves to the centred 640 columns; one covering the whole 640x480 also covers the side columns,
// each with its texture's edge column (the scope's black surround, the menus' and spectator's backdrops).
void PcWideRect2D(X86Context& ctx, uint8_t*) {
    static bool inside = false;
    const uint32_t frame = FrameWidth();
    if (inside || !DrawingToFrame()) return;
    float a[8];
    std::memcpy(a, kernel::HostPtr(ctx.esp + 4), sizeof a);
    const float side = (frame - 640) / 2.0f;
    if (a[0] == 0 && a[1] == 0 && a[2] == 640 && a[3] == 480) {
        inside = true;
        // each side column from the texture's edge column (just inside it, clear of a wrapping neighbour)
        const float du = (a[6] - a[4]) * 0.002f;
        for (int right = 0; right < 2; right++) {
            float x0 = right ? side + 640.0f : 0.0f, u = right ? a[6] - du : a[4] + du;
            float bar[8] = {x0, 0, x0 + side, 480, u, a[5], u, a[7]};
            uint32_t args[8];
            std::memcpy(args, bar, sizeof bar);
            kernel::CallGuest(ctx, kGlScreenRect, args, 8);
        }
        inside = false;
    }
    a[0] += side;
    a[2] += side;
    std::memcpy(kernel::HostPtr(ctx.esp + 4), a, 8 * 4);
}

// [[hook]] at 0xB9B20: the game's text renderer (thiscall: x, y floats in display pixels, colour, string, flags, width;
// glyphs drawn with its own vertex program in screen pixels): menus, HUD text, chat, centre messages
void PcWideText(X86Context& ctx, uint8_t*) {
    if (!DrawingToFrame()) return;
    float x;
    std::memcpy(&x, kernel::HostPtr(ctx.esp + 4), 4);
    x += (FrameWidth() - 640) / 2.0f;
    std::memcpy(kernel::HostPtr(ctx.esp + 4), &x, 4);
}

// [[hook]] at 0xC71FA and 0x2902BA (the two D3DDevice_UpdateOverlay calls: the movie player's and XMV's; [esp+8] =
// the destination RECT in display pixels, laid out for 640x480): the video overlay moves to the centred 640 columns.
// A rectangle already reaching past 640 is left alone, so one kept between calls is moved once.
void PcWideOverlay(X86Context& ctx, uint8_t*) {
    uint32_t rect = R32(ctx.esp + 8);
    if (FrameWidth() == 640 || !rect) return;
    int32_t left = int32_t(R32(rect)), right = int32_t(R32(rect + 8));
    if (left < 0 || right > 640 || right <= left) return;
    int32_t side = int32_t(FrameWidth() - 640) / 2;
    W32(rect, uint32_t(left + side));
    W32(rect + 8, uint32_t(right + side));
}
