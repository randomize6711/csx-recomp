// HUD layout (N10, docs/n10_hud.md): the HUD's elements moved and resized, per profile, for a full screen and for
// split-screen. The client's HUD draws through the engine's functions for it (cl_enginefuncs, the table at 0x371610:
// SPR_Draw 0x22F70, SPR_DrawHoles 0x22FF0, SPR_DrawAdditive 0x23070, FillRGBA 0x476C0, DrawCharacter 0xB0F40,
// DrawConsoleString 0x47060), each called from the element's own Draw: the caller (the return address at the
// function's entry) says which element draws (kElements: the client functions' address ranges). At each entry the
// element's place is applied to the coordinates on the stack (x' = corner + (x - corner) * scale + offset, the corner
// the screen's nearest to the element so it keeps to its edge), and for sprites its scale to the quad's size (the
// sprite draw 0x47470 after its clipping, 0x47508: width at [esp+0x20], height at [esp+0x24]).
// CSX_LOG_HUDDRAW=1 logs each new caller once (what draws where), for mapping elements.
// Layouts: pc_settings.ini "hud.KEY" (a full screen) and "hud_split.KEY" (a split-screen player's view), "DX DY SCALE"
// (HUD units, 0.5..2), profile-scoped (profiles.cpp). The editor (PC Options > HUD and Menus > Edit HUD Layout, or
// "pc_hud_edit" in a match): Tab / Shift+Tab (the controller's bumpers) choose an element, outlined; the arrows (the
// D-pad) move it, Shift for larger steps; + and - (the triggers) resize it; R resets it, Delete resets them all; Enter
// (A) saves, Escape (B) puts back the saved layout. Elements stay inside the screen (kMargin).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;

bool LogDraws() {
    static const bool on = std::getenv("CSX_LOG_HUDDRAW") && *std::getenv("CSX_LOG_HUDDRAW") == '1';
    return on;
}

void Diagnose(const char* fn, uint32_t caller, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!LogDraws()) return;
    static std::mutex lock;
    static std::set<uint32_t> seen;
    std::lock_guard<std::mutex> l(lock);
    if (seen.size() > 400 || !seen.insert(caller).second) return;
    LOGI("hud draw: %s from %08X at %d,%d (%d x %d)", fn, caller, x, y, w, h);
}

int32_t I(uint32_t at) { return int32_t(R32(at)); }

struct Element {
    const char* key;
    const char* label;
    uint32_t entry;   // its Draw function (vtable entry), hooked: the shared number helpers draw for it after
    uint32_t lo, hi;  // the client code that draws it directly (callers' return addresses in [lo, hi))
};
struct Range {
    uint32_t lo, hi;
};
// mapped from CSX_LOG_HUDDRAW runs (docs/n10_hud.md, src/game/pc_hud_elements.inc)
#define CSX_HUD_ELEMENTS
#include "game/pc_hud_elements.inc"
const std::vector<Element>& Elements() {
    static const std::vector<Element> e(std::begin(kElements), std::end(kElements));
    return e;
}

thread_local int g_current = -1;  // the element whose Draw ran last (its entry hook), for the shared helpers

struct Place {
    float dx = 0, dy = 0, scale = 1;
    bool operator==(const Place& o) const { return dx == o.dx && dy == o.dy && scale == o.scale; }
};
constexpr float kMargin = 2, kMinScale = 0.5f, kMaxScale = 2.0f;
constexpr uint32_t kScreenW = 0x6CE2BC, kScreenH = 0x6CE2C0;  // the HUD's screen (SPR_Draw's bounds)
constexpr uint32_t kFillRGBA = 0x476C0, kCenterPrint = 0x1C5B0;

std::mutex g_lock;
std::vector<Place> g_places;      // in use (the editor's while it edits)
std::vector<Place> g_saved;       // the settings'
float g_global = 1;               // HUD Scale (hud_scale / hud_scale_split, percent): every element's on top of its own
unsigned g_seen = ~0u;
struct Box {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    void Add(float x, float y, float w, float h) {
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + w), y1 = std::max(y1, y + h);
    }
    bool Valid() const { return x1 > x0 && y1 > y0; }
};
std::vector<Box> g_box, g_last_box;  // this frame's drawn bounds per element, and the last whole frame's
bool g_editing = false;
int g_selected = 0;
std::vector<int> g_input;
thread_local float g_quad_scale = 1;  // the sprite being drawn's scale, for the quad (consumed there)
thread_local int g_quad_elem = -1;

std::string Prefix() { return csx::SplitPlayer() ? "hud_split." : "hud."; }

Place Parse(const std::string& v) {
    Place p;
    if (std::sscanf(v.c_str(), "%f %f %f", &p.dx, &p.dy, &p.scale) != 3) return Place();
    p.scale = std::clamp(p.scale, kMinScale, kMaxScale);
    p.dx = std::clamp(p.dx, -640.f, 640.f), p.dy = std::clamp(p.dy, -480.f, 480.f);
    return p;
}

// the settings read again when they change (the editor's own changes are its)
void Load() {
    if (pc::Version() == g_seen) return;
    g_seen = pc::Version();
    const auto& el = Elements();
    g_saved.assign(el.size(), Place());
    for (size_t i = 0; i < el.size(); i++) g_saved[i] = Parse(pc::Get(Prefix() + el[i].key, "0 0 1"));
    g_global = std::clamp(std::atof(pc::Get(csx::SplitPlayer() ? "hud_scale_split" : "hud_scale", "100").c_str()) / 100.0,
                          0.5, 2.0);
    if (!g_editing) g_places = g_saved;
    g_box.assign(el.size(), Box());
    g_last_box.assign(el.size(), Box());
}

int ElementOf(uint32_t caller) {
    const auto& el = Elements();
    for (size_t i = 0; i < el.size(); i++)
        if (caller >= el[i].lo && caller < el[i].hi) return int(i);
    for (const Range& r : kShared)
        if (caller >= r.lo && caller < r.hi) return g_current;
    return -1;
}

// the element's place applied to a point: scaled from the screen corner nearest the element (its last bounds' centre)
void Apply(int e, int32_t& x, int32_t& y) {
    Place p = g_places[size_t(e)];
    p.scale *= g_global;
    if (p == Place()) return;
    float w = float(I(kScreenW)), h = float(I(kScreenH));
    const Box& b = g_last_box[size_t(e)].Valid() ? g_last_box[size_t(e)] : Box{float(x), float(y), float(x), float(y)};
    float cx = (b.x0 + b.x1) / 2 < w / 2 ? 0 : w, cy = (b.y0 + b.y1) / 2 < h / 2 ? 0 : h;
    x = int32_t(std::lround(cx + (float(x) - cx) * p.scale + p.dx));
    y = int32_t(std::lround(cy + (float(y) - cy) * p.scale + p.dy));
}

// a draw's coordinates (and size, when it has them) at its entry; the caller decides the element
int Transform(uint32_t caller, uint32_t xa, uint32_t ya, uint32_t wa, uint32_t ha) {
    std::lock_guard<std::mutex> l(g_lock);
    Load();
    int e = ElementOf(caller);
    if (e < 0) return -1;
    int32_t x = I(xa), y = I(ya);
    Apply(e, x, y);
    kernel::W32(xa, uint32_t(x));
    kernel::W32(ya, uint32_t(y));
    float s = g_places[size_t(e)].scale * g_global;
    if (wa) {
        int32_t w = int32_t(std::lround(float(I(wa)) * s)), h = int32_t(std::lround(float(I(ha)) * s));
        kernel::W32(wa, uint32_t(w));
        kernel::W32(ha, uint32_t(h));
        g_box[size_t(e)].Add(float(x), float(y), float(w), float(h));
    } else {
        g_box[size_t(e)].Add(float(x), float(y), 8 * s, 12 * s);  // a character's cell, about
    }
    return e;
}
}  // namespace

// [[hook]]s at the elements' Draw entries (config/game.toml PcHudEnter0..): the element drawing now
template <int E>
void Enter() {
    g_current = E;
}
void PcHudEnter0(X86Context&, uint8_t*) { Enter<0>(); }
void PcHudEnter1(X86Context&, uint8_t*) { Enter<1>(); }
void PcHudEnter2(X86Context&, uint8_t*) { Enter<2>(); }
void PcHudEnter3(X86Context&, uint8_t*) { Enter<3>(); }
void PcHudEnter4(X86Context&, uint8_t*) { Enter<4>(); }
void PcHudEnter5(X86Context&, uint8_t*) { Enter<5>(); }
void PcHudEnter6(X86Context&, uint8_t*) { Enter<6>(); }

// [[hook]]s at the entries (config/game.toml): the caller at [esp], the arguments after it
namespace {
void Sprite(X86Context& ctx, const char* fn) {
    Diagnose(fn, R32(ctx.esp), I(ctx.esp + 8), I(ctx.esp + 12), 0, 0);
    int e = Transform(R32(ctx.esp), ctx.esp + 8, ctx.esp + 12, 0, 0);
    g_quad_elem = e;
    g_quad_scale = e < 0 ? 1.f : g_places[size_t(e)].scale * g_global;
}
}  // namespace

void PcHudSprDraw(X86Context& ctx, uint8_t*) { Sprite(ctx, "SPR_Draw"); }
void PcHudSprHoles(X86Context& ctx, uint8_t*) { Sprite(ctx, "SPR_DrawHoles"); }
void PcHudSprAdditive(X86Context& ctx, uint8_t*) { Sprite(ctx, "SPR_DrawAdditive"); }
void PcHudFill(X86Context& ctx, uint8_t*) {
    Diagnose("FillRGBA", R32(ctx.esp), I(ctx.esp + 4), I(ctx.esp + 8), I(ctx.esp + 12), I(ctx.esp + 16));
    Transform(R32(ctx.esp), ctx.esp + 4, ctx.esp + 8, ctx.esp + 12, ctx.esp + 16);
}
void PcHudChar(X86Context& ctx, uint8_t*) {
    Diagnose("DrawCharacter", R32(ctx.esp), I(ctx.esp + 4), I(ctx.esp + 8), 0, 0);
    Transform(R32(ctx.esp), ctx.esp + 4, ctx.esp + 8, 0, 0);
}
void PcHudString(X86Context& ctx, uint8_t*) {
    Diagnose("DrawConsoleString", R32(ctx.esp), I(ctx.esp + 4), I(ctx.esp + 8), 0, 0);
    Transform(R32(ctx.esp), ctx.esp + 4, ctx.esp + 8, 0, 0);
}

// [[hook]] at 0x47508 in the sprite draw (0x47470), after its clipping: the quad's width and height at [esp+0x20],
// [esp+0x24] scaled with the element's (its texture coordinates unchanged: the sprite stretched); the drawn bounds
void PcHudQuad(X86Context& ctx, uint8_t*) {
    float s = g_quad_scale;
    int e = g_quad_elem;
    g_quad_scale = 1, g_quad_elem = -1;
    if (e < 0) return;
    int32_t w = I(ctx.esp + 0x20), h = I(ctx.esp + 0x24);
    if (s != 1) {
        w = int32_t(std::lround(float(w) * s)), h = int32_t(std::lround(float(h) * s));
        kernel::W32(ctx.esp + 0x20, uint32_t(w));
        kernel::W32(ctx.esp + 0x24, uint32_t(h));
    }
    std::lock_guard<std::mutex> l(g_lock);
    if (size_t(e) < g_box.size()) g_box[size_t(e)].Add(float(I(ctx.esp + 0x2C)), float(I(ctx.esp + 0x30)), float(w), float(h));
}

// ---- the editor -------------------------------------------------------------------------------------------------
namespace {
void Say(X86Context& ctx, const std::string& text) {
    uint32_t esp = ctx.esp;
    uint32_t str = (esp - 0x400 - uint32_t(text.size())) & ~15u;
    std::memcpy(kernel::HostPtr(str), text.c_str(), text.size() + 1);
    ctx.esp = str - 16;
    kernel::CallGuest(ctx, kCenterPrint, {str});
    ctx.esp = esp;
}

void Fill(X86Context& ctx, float x, float y, float w, float h, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    uint32_t esp = ctx.esp;
    ctx.esp -= 0x200;
    kernel::CallGuest(ctx, kFillRGBA, {uint32_t(int32_t(x)), uint32_t(int32_t(y)), uint32_t(int32_t(std::max(1.f, w))),
                                       uint32_t(int32_t(std::max(1.f, h))), r, g, b, a});
    ctx.esp = esp;
}

void Save() {
    const auto& el = Elements();
    for (size_t i = 0; i < el.size(); i++) {
        const Place& p = g_places[i];
        if (p == Place()) pc::Erase(Prefix() + el[i].key);
        else {
            char v[64];
            std::snprintf(v, sizeof v, "%g %g %g", p.dx, p.dy, p.scale);
            pc::Set(Prefix() + el[i].key, v);
        }
    }
    pc::Save();
    g_saved = g_places;
    g_seen = pc::Version();
}

// keeps the element's last bounds inside the screen
void Clamp(int e) {
    const Box& b = g_last_box[size_t(e)];
    if (!b.Valid()) return;
    float w = float(I(kScreenW)), h = float(I(kScreenH));
    Place& p = g_places[size_t(e)];
    if (b.x0 < kMargin) p.dx += kMargin - b.x0;
    if (b.y0 < kMargin) p.dy += kMargin - b.y0;
    if (b.x1 > w - kMargin) p.dx -= b.x1 - (w - kMargin);
    if (b.y1 > h - kMargin) p.dy -= b.y1 - (h - kMargin);
}
}  // namespace

namespace csx {
bool HudEditing() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_editing;
}
void HudEditInput(int key) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_editing) g_input.push_back(key);
}
}  // namespace csx

// "pc_hud_edit" (pc_menu.cpp): the editor on (in a match) or off (as Escape)
void PcHudEditCommand() {
    std::lock_guard<std::mutex> l(g_lock);
    Load();
    if (Elements().empty()) {
        LOGW("pc hud: no HUD element is mapped: the editor has nothing to move");
        return;
    }
    g_editing = !g_editing;
    if (!g_editing) g_places = g_saved;
    else g_input.push_back(0);  // shows the first element's name
    LOGI("pc hud: the HUD editor %s (%s layout)", g_editing ? "opened" : "closed", csx::SplitPlayer() ? "split-screen" : "full screen");
}

// [[hook]] at 0x6E4B0 (config/game.toml), SCR_DrawNetGraph's entry: drawn in 2D after the HUD each frame. The frame's
// bounds become the last; while editing, the keys are applied and the chosen element outlined
void PcHudFrame2D(X86Context& ctx, uint8_t*) {
    std::vector<int> keys;
    {
        std::lock_guard<std::mutex> l(g_lock);
        Load();
        for (size_t i = 0; i < g_box.size(); i++)
            if (g_box[i].Valid()) g_last_box[i] = g_box[i], g_box[i] = Box();
        g_current = -1;  // the HUD is drawn: the next frame's elements set it again
        if (!g_editing) return;
        keys.swap(g_input);
    }
    const auto& el = Elements();
    std::string say;
    for (int k : keys) {
        std::lock_guard<std::mutex> l(g_lock);
        int n = int(el.size());
        Place& p = g_places[size_t(g_selected)];
        bool show = true;
        switch (k) {
            case 'n': g_selected = (g_selected + 1) % n; break;
            case 'p': g_selected = (g_selected + n - 1) % n; break;
            case 'l': p.dx -= 2; break;
            case 'r': p.dx += 2; break;
            case 'u': p.dy -= 2; break;
            case 'd': p.dy += 2; break;
            case 'L': p.dx -= 10; break;
            case 'R': p.dx += 10; break;
            case 'U': p.dy -= 10; break;
            case 'D': p.dy += 10; break;
            case '+': p.scale = std::min(kMaxScale, p.scale + 0.05f); break;
            case '-': p.scale = std::max(kMinScale, p.scale - 0.05f); break;
            case 'x': p = Place(); break;
            case 'X': g_places.assign(el.size(), Place()); break;
            case 's':
                Save();
                g_editing = false;
                LOGI("pc hud: the layout saved (%s)", csx::SplitPlayer() ? "split-screen" : "full screen");
                say = "HUD layout saved";
                show = false;
                break;
            case 'c':
                g_places = g_saved;
                g_editing = false;
                LOGI("pc hud: the editor closed, the saved layout kept");
                say = "HUD layout unchanged";
                show = false;
                break;
            default: break;
        }
        if (g_editing && k != 'n' && k != 'p' && k) Clamp(g_selected);
        if (show && g_editing) {
            const Place& q = g_places[size_t(g_selected)];
            char t[160];
            std::snprintf(t, sizeof t, "HUD: %s  (%+.0f, %+.0f) x%.2f\nTab next  Arrows move  +/- size  R reset  Enter save  Esc cancel",
                          el[size_t(g_selected)].label, q.dx, q.dy, q.scale);
            say = t;
        }
    }
    // the guest's drawing outside the lock (FillRGBA comes back through PcHudFill)
    if (!say.empty()) Say(ctx, say);
    Box b;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_editing) return;
        b = g_last_box[size_t(g_selected)];
    }
    if (!b.Valid()) return;
    float x = b.x0 - 2, y = b.y0 - 2, w = b.x1 - b.x0 + 4, h = b.y1 - b.y0 + 4;
    Fill(ctx, x, y, w, 1, 255, 220, 40, 255);
    Fill(ctx, x, y + h - 1, w, 1, 255, 220, 40, 255);
    Fill(ctx, x, y, 1, h, 255, 220, 40, 255);
    Fill(ctx, x + w - 1, y, 1, h, 255, 220, 40, 255);
}
