// input::Poll for the Linux window: the first SDL gamepad, and the keyboard and mouse. While a menu screen of the game
// is open the keyboard and mouse act as the console controller on port 0 (the title's menus read only the pad):
//   WASD left stick, arrows d-pad, Space A, Left Ctrl B, R X, Q Y, 1 white, 2 black, left mouse right trigger,
//   right mouse left trigger, Enter / Escape START, Tab BACK, Left Shift left stick click, middle mouse right stick
//   click; on the server browser's screens Escape is B, F5 X, Page Up / Page Down the triggers. While playing they
//   are the engine's own keys and run their bindings (Options > PC Controls), Escape
//   staying START (the game menu). F1 releases or captures the mouse.
// The developer console (src/game/pc_console.cpp): the key bound to Toggle Developer Console (PC Controls; by default
// the key left of 1, whatever the layout prints on it) opens and closes it, playing or in a menu. While it is open the
// keyboard is its alone: typed text (SDL text input, so the layout's characters), Backspace, Enter, Up / Down
// (history), Tab (completion), Page Up / Down and the wheel (scrolling), Escape closing it; held game keys are let go,
// and the mouse is released, captured again when it closes.
// PC Controls' press-a-key rebinding (src/game/pc_menu.cpp): Enter on a binding row (or a click on its key name)
// makes it wait; then the next key, mouse button or wheel notch is its key and nothing else gets it (no menu move, no
// game key), Escape cancels; F1 and F11 stay the window's. The name field of the profile's virtual keyboard
// (src/game/pc_mouse.cpp), once clicked: typed text (the layout's characters), Backspace, the arrows, Enter (Done),
// Escape (typing ends) go to the game's edit box.
// Keys by layout: letters are the letter the layout prints; the digit row and the punctuation keys are taken by their
// position (US names: Swedish Å is [, Ö ;, Ä ', the key left of Z <), so every layout can bind them; the menus show
// them as the layout prints them (CSX_KEY_LABELS, set here once the window is up).
// Mouse look: while the mouse is captured (relative mode), each count turns the view by 0.022 degrees x the
// sensitivity (CSX_MOUSE_SENS, else Options > PC Options, else 3: GoldSrc's m_yaw / m_pitch and
// CS's default) through the title's look routine (src/game/mouse_look.cpp); CSX_MOUSE_INVERT=1 inverts the pitch.
// CSX_MOUSE=stick feeds the right stick instead, as before (the title's stick response, acceleration and aim assist
// apply then).
// Raw Mouse Input (PC Options, pc_settings.ini "mouse_raw", on unless "0"): On, SDL's relative motion is the device's
// own counts (Wayland's zwp_relative_pointer_v1 unaccelerated deltas, X11's XInput2 raw values), nothing applied to
// them by the desktop; Off, the desktop's pointer motion instead (the compositor's or X server's acceleration and
// speed, Wayland's pointer scale): SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, which SDL reads at every motion, so a change
// applies at once. Either way the game adds no curve, smoothing or frame time to it.
#include <SDL3/SDL.h>

#include "linux/split_player.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "app/app.h"
#include "core/input.h"
#include "common/csx.h"
#include "common/pc_bindings.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "harness.h"
#include "linux/window.h"

namespace {

std::mutex g_lock;
std::set<SDL_Keycode> g_keys;
uint32_t g_mouse_buttons = 0;
float g_mouse_dx = 0, g_mouse_dy = 0;  // since the last poll
SDL_Gamepad* g_pad = nullptr;
constexpr float kMouseScale = 1200.0f;  // CSX_MOUSE=stick: stick units per pixel of motion in one poll
constexpr float kDegreesPerCount = 0.022f;  // GoldSrc's m_yaw and m_pitch

struct MouseConfig {
    bool stick = false;
    bool sens_env = false;  // CSX_MOUSE_SENS given: it wins over the PC Options setting
    float env_sens = 3.0f;
    bool invert_env = false;  // CSX_MOUSE_INVERT given: it wins over the PC Options setting
    bool invert = false;
    MouseConfig() {
        if (const char* m = std::getenv("CSX_MOUSE")) stick = std::strcmp(m, "stick") == 0;
        if (const char* v = std::getenv("CSX_MOUSE_SENS")) {
            sens_env = true;
            env_sens = std::max(0.0f, float(std::atof(v)));
        }
        if (const char* v = std::getenv("CSX_MOUSE_INVERT")) {
            invert_env = true;
            invert = *v == '1';
        }
        LOGI("input: mouse %s, sensitivity %.2f (%.4f degrees per count)%s%s", stick ? "on the right stick" : "look",
             Sens(), Sens() * kDegreesPerCount, sens_env ? " from CSX_MOUSE_SENS" : "",
             Invert() ? ", pitch inverted" : "");
    }
    // GoldSrc's sensitivity: the environment's, else Options > PC Options (pc_settings.ini), else the game's 3
    float Sens() const {
        if (sens_env) return env_sens;
        static unsigned seen = ~0u;
        static float sens = 3.0f;
        if (pc::Version() != seen) {
            seen = pc::Version();
            sens = std::max(0.0f, pc::GetFloat("mouse_sensitivity", 3.0f));
        }
        return sens;
    }
    bool Invert() const { return invert_env ? invert : pc::Get("mouse_invert", "0") == "1"; }
};
const MouseConfig& Mouse() {
    static const MouseConfig c;
    return c;
}

bool Down(SDL_Keycode k) { return g_keys.count(k) != 0; }

// Playing (no menu screen open), keys and mouse buttons are the engine's own keys and run their bindings (Options >
// PC Controls, src/game/pc_keys.cpp); Escape stays the pad's START (the game menu). In the game's menus they stay the
// pad (arrows, Space, Left Ctrl...) whatever the bindings, so the menus are always reachable.
std::set<int> g_engine_held;  // engine keys down: their release goes to the engine whatever is open then
bool g_was_menu = true;

// the keys taken by their position: the digit row and the punctuation keys, as engine keys (their US characters; the
// ISO key left of Z, which a US keyboard lacks, is <)
const struct {
    SDL_Scancode sc;
    char key;
} kPositionKeys[] = {
    {SDL_SCANCODE_GRAVE, '`'}, {SDL_SCANCODE_1, '1'}, {SDL_SCANCODE_2, '2'}, {SDL_SCANCODE_3, '3'},
    {SDL_SCANCODE_4, '4'}, {SDL_SCANCODE_5, '5'}, {SDL_SCANCODE_6, '6'}, {SDL_SCANCODE_7, '7'},
    {SDL_SCANCODE_8, '8'}, {SDL_SCANCODE_9, '9'}, {SDL_SCANCODE_0, '0'}, {SDL_SCANCODE_MINUS, '-'},
    {SDL_SCANCODE_EQUALS, '='}, {SDL_SCANCODE_LEFTBRACKET, '['}, {SDL_SCANCODE_RIGHTBRACKET, ']'},
    {SDL_SCANCODE_BACKSLASH, '\\'}, {SDL_SCANCODE_NONUSHASH, '\\'}, {SDL_SCANCODE_SEMICOLON, ';'},
    {SDL_SCANCODE_APOSTROPHE, '\''}, {SDL_SCANCODE_COMMA, ','}, {SDL_SCANCODE_PERIOD, '.'},
    {SDL_SCANCODE_SLASH, '/'}, {SDL_SCANCODE_NONUSBACKSLASH, '<'},
};

// SDL key -> engine key number (keys.c: letters, digits and punctuation as their characters, the rest from 127 up):
// letters as the layout prints them, the digit row and punctuation by position (the key left of 1 is ` as PC games
// take the console key: it prints ` on a US layout, § on a Swedish one, ^ on a German one). Keys without a scancode
// (tests) go by their key code.
int EngineKey(SDL_Keycode k, SDL_Scancode sc = SDL_SCANCODE_UNKNOWN) {
    if (k >= SDLK_A && k <= SDLK_Z) return int(k);
    for (const auto& p : kPositionKeys)
        if (p.sc == sc) return p.key;
    if (k >= SDLK_0 && k <= SDLK_9) return int(k);
    switch (k) {
        case SDLK_SPACE: return 32;
        case SDLK_TAB: return 9;
        case SDLK_RETURN: return 13;
        case SDLK_BACKSPACE: return 127;
        case SDLK_GRAVE: case SDLK_MINUS: case SDLK_EQUALS: case SDLK_LEFTBRACKET: case SDLK_RIGHTBRACKET:
        case SDLK_SEMICOLON: case SDLK_COMMA: case SDLK_PERIOD: case SDLK_SLASH: case SDLK_BACKSLASH:
        case SDLK_APOSTROPHE: case SDLK_LESS: return int(k);
        case SDLK_UP: return 128;
        case SDLK_DOWN: return 129;
        case SDLK_LEFT: return 130;
        case SDLK_RIGHT: return 131;
        case SDLK_LALT: case SDLK_RALT: return 132;
        case SDLK_LCTRL: case SDLK_RCTRL: return 133;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return 134;
        case SDLK_F2: case SDLK_F3: case SDLK_F4: case SDLK_F5: case SDLK_F6: case SDLK_F7: case SDLK_F8:
        case SDLK_F9: case SDLK_F10: case SDLK_F12: return 135 + int(k - SDLK_F1);
        case SDLK_INSERT: return 147;
        case SDLK_DELETE: return 148;
        case SDLK_PAGEDOWN: return 149;
        case SDLK_PAGEUP: return 150;
        case SDLK_HOME: return 151;
        case SDLK_END: return 152;
        case SDLK_KP_7: return 160;
        case SDLK_KP_8: return 161;
        case SDLK_KP_9: return 162;
        case SDLK_KP_4: return 163;
        case SDLK_KP_5: return 164;
        case SDLK_KP_6: return 165;
        case SDLK_KP_1: return 166;
        case SDLK_KP_2: return 167;
        case SDLK_KP_3: return 168;
        case SDLK_KP_ENTER: return 169;
        case SDLK_KP_0: return 170;
        case SDLK_KP_PERIOD: return 171;
        case SDLK_KP_DIVIDE: return 172;
        case SDLK_KP_MINUS: return 173;
        case SDLK_KP_PLUS: return 174;
        case SDLK_CAPSLOCK: return 175;
        default: return -1;  // Escape (the game menu), F1 / F11 (the window) and the rest
    }
}

// SDL mouse button -> MOUSE1..MOUSE5
int EngineMouseKey(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return 241;
        case SDL_BUTTON_RIGHT: return 242;
        case SDL_BUTTON_MIDDLE: return 243;
        case SDL_BUTTON_X1: return 244;
        case SDL_BUTTON_X2: return 245;
        default: return -1;
    }
}

// actions bound as a pad button (pc::Action::pad): engine key number -> buttons, and the ones held now
std::map<int, unsigned> g_pad_actions;
std::map<int, unsigned> g_pad_held;
unsigned PadAction(int key) {
    static unsigned seen = ~0u;
    if (pc::Version() != seen) {
        seen = pc::Version();
        g_pad_actions.clear();
        for (const pc::Action& a : pc::Actions())
            if (const pc::KeyInfo* k = pc::FindKey(pc::BoundKey(a)); a.pad && k && k->number >= 0)
                g_pad_actions[k->number] = a.pad;
    }
    auto it = g_pad_actions.find(key);
    return it == g_pad_actions.end() ? 0 : it->second;
}

void EngineKeyEvent(int key, bool down) {
    if (key < 0) return;
    if (down && !g_engine_held.insert(key).second) return;  // auto repeat
    if (!down && !g_engine_held.erase(key)) return;
    csx::QueueEngineKey(key, down);
}

int16_t Stick(float v) { return int16_t(std::clamp(v, -32767.0f, 32767.0f)); }

// tests: the run folder's "cmd" file ("BUTTONS [FRAMES]", buttons joined by +, as the headless harness's pad script
// takes them: UP DOWN LEFT RIGHT START BACK A B X Y BLACK WHITE LT RT LTHUMB RTHUMB, LS LSD LSL LSR, RS RSD RSL RSR)
// pressed on the pad for that many presented frames, so scenarios drive the pad in the window as headless
struct ScriptedPress {
    uint64_t until = 0;
    std::string buttons;
};
ScriptedPress g_scripted;
void ApplyScripted(const std::string& s, input::Pad& p, float& lx, float& ly, float& rx, float& ry) {
    for (size_t at = 0; at <= s.size();) {
        size_t e = s.find('+', at);
        if (e == std::string::npos) e = s.size();
        std::string b = s.substr(at, e - at);
        at = e + 1;
        static const std::map<std::string, uint16_t> digital = {{"UP", 1},     {"DOWN", 2},    {"LEFT", 4},
                                                                 {"RIGHT", 8}, {"START", 16},  {"BACK", 32},
                                                                 {"LTHUMB", 64}, {"RTHUMB", 128}};
        static const std::map<std::string, int> analog = {{"A", 0}, {"B", 1}, {"X", 2},     {"Y", 3},
                                                           {"BLACK", 4}, {"WHITE", 5}, {"LT", 6}, {"RT", 7}};
        if (auto d = digital.find(b); d != digital.end()) p.buttons |= d->second;
        else if (auto a = analog.find(b); a != analog.end()) p.analog[a->second] = 255;
        else if (b == "LS") ly = 32767;
        else if (b == "LSD") ly = -32767;
        else if (b == "LSL") lx = -32767;
        else if (b == "LSR") lx = 32767;
        else if (b == "RS") ry = 32767;
        else if (b == "RSD") ry = -32767;
        else if (b == "RSL") rx = -32767;
        else if (b == "RSR") rx = 32767;
    }
}

// the developer console: the engine key that toggles it, whether it is shown (as the window last saw), and the text
// event a toggle key's own press may still bring
bool g_console = false;
bool g_swallow_text = false;
bool g_capture_play = true;  // the mouse captured while playing (F1 lets it go, a click in the window takes it back)

// the window's point (logical pixels) in the menus' 640x480: the frame letterboxed at its aspect, the menus' 640
// columns in the middle of a wider frame (src/game/pc_wide.cpp)
bool MenuPoint(float wx, float wy, float& mx, float& my) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(static_cast<SDL_Window*>(window::SdlWindow()), &ww, &wh);
    if (ww <= 0 || wh <= 0) return false;
    const float aspect = window::Latest().aspect;
    float tw = float(ww), th = tw / aspect;
    if (th > float(wh)) {
        th = float(wh);
        tw = th * aspect;
    }
    float fx = (wx - (float(ww) - tw) / 2) / tw, fy = (wy - (float(wh) - th) / 2) / th;
    if (fx < 0 || fx >= 1 || fy < 0 || fy >= 1) return false;
    const float frame_w = 480.0f * aspect;
    mx = fx * frame_w - (frame_w - 640.0f) / 2;
    my = fy * 480.0f;
    return true;
}
// the menus take the mouse: a menu screen open and no console over it
bool MenuMouse() { return csx::MenuOpen() && !g_console; }
int ConsoleKeyNumber() {
    static unsigned seen = ~0u;
    static int key = -1;
    if (pc::Version() != seen) {
        seen = pc::Version();
        key = -1;
        for (const pc::Action& a : pc::Actions())
            if (std::string(a.id) == "console")
                if (const pc::KeyInfo* k = pc::FindKey(pc::BoundKey(a)); k) key = k->number;
    }
    return key;
}

// a key while the console is open: its editing keys; the rest only type (through text input)
void ConsoleEditKey(SDL_Keycode k) {
    using K = csx::ConsoleKey;
    switch (k) {
        case SDLK_ESCAPE: csx::QueueConsoleKey(K::Close); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: csx::QueueConsoleKey(K::Enter); break;
        case SDLK_BACKSPACE: csx::QueueConsoleKey(K::Backspace); break;
        case SDLK_UP: csx::QueueConsoleKey(K::HistoryBack); break;
        case SDLK_DOWN: csx::QueueConsoleKey(K::HistoryForward); break;
        case SDLK_TAB: csx::QueueConsoleKey(K::Complete); break;
        case SDLK_PAGEUP: csx::QueueConsoleKey(K::PageUp); break;
        case SDLK_PAGEDOWN: csx::QueueConsoleKey(K::PageDown); break;
        default: break;
    }
}

}  // namespace

namespace window {

void HandleEvent(const void* p) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    std::lock_guard<std::mutex> lock(g_lock);
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (csx::BindCapturing()) {  // a binding waits: this key is its, Escape cancels, repeats do nothing
                int key = EngineKey(e.key.key, e.key.scancode);
                if (e.key.repeat) break;
                if (e.key.key == SDLK_ESCAPE) csx::BindInput(-1);
                else if (key >= 0) csx::BindInput(key);
                else LOGI("input: %s cannot be bound", SDL_GetKeyName(e.key.key));  // F1, Print Screen...
                break;
            }
            if (csx::TextEntryOpen() && !g_console) {  // the name field: its editing keys; characters come as text
                using T = csx::TextKey;
                switch (e.key.key) {
                    case SDLK_BACKSPACE: csx::QueueTextEntryKey(T::Backspace); break;
                    case SDLK_LEFT: csx::QueueTextEntryKey(T::Left); break;
                    case SDLK_RIGHT: csx::QueueTextEntryKey(T::Right); break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: if (!e.key.repeat) csx::QueueTextEntryKey(T::Enter); break;
                    case SDLK_ESCAPE: if (!e.key.repeat) csx::QueueTextEntryKey(T::Escape); break;
                    default: break;
                }
                break;
            }
            if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && !g_console && csx::BindRowSelected()) {
                if (!e.key.repeat) csx::BindStart();  // PC Controls: the selected row waits for a key
                break;
            }
            if (EngineKey(e.key.key, e.key.scancode) == ConsoleKeyNumber() && ConsoleKeyNumber() >= 0) {
                if (!e.key.repeat) {
                    csx::QueueConsoleKey(csx::ConsoleKey::Toggle);
                    g_swallow_text = g_console;  // closing: the key's character is not typed
                }
            } else if (g_console) {
                ConsoleEditKey(e.key.key);
            } else if (int key = EngineKey(e.key.key, e.key.scancode); !csx::MenuOpen() && key >= 0) {
                if (unsigned pad = PadAction(key)) g_pad_held[key] = pad;
                else EngineKeyEvent(key, true);
            } else {
                g_keys.insert(e.key.key);
            }
            break;
        case SDL_EVENT_KEY_UP:
            g_pad_held.erase(EngineKey(e.key.key, e.key.scancode));
            EngineKeyEvent(EngineKey(e.key.key, e.key.scancode), false);
            g_keys.erase(e.key.key);
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (g_console && !std::exchange(g_swallow_text, false)) csx::QueueConsoleText(e.text.text);
            else if (!g_console && csx::TextEntryOpen()) csx::QueueTextEntry(e.text.text);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (g_console) break;
            if (csx::BindCapturing()) {  // its key; a double click's second press is not one
                if (e.button.clicks < 2 && EngineMouseKey(e.button.button) >= 0)
                    csx::BindInput(EngineMouseKey(e.button.button));
                break;
            }
            if (MenuMouse()) {  // the menus: the pointer clicks (src/game/pc_mouse.cpp), never the pad's triggers
                float mx = -1, my = -1;
                bool in = MenuPoint(e.button.x, e.button.y, mx, my);
                if (in) csx::MenuPointer(mx, my);
                int ww = 0, wh = 0;  // where a click lands, for reports of the pointer seeming off
                SDL_GetWindowSize(static_cast<SDL_Window*>(window::SdlWindow()), &ww, &wh);
                LOGI("input: click at %.0f %.0f of %dx%d (frame %.4f) -> menu %.0f %.0f", e.button.x, e.button.y, ww, wh,
                     window::Latest().aspect, mx, my);
                if (e.button.button == SDL_BUTTON_LEFT) csx::MenuClick(e.button.clicks >= 2 ? 11 : 1);
                if (e.button.button == SDL_BUTTON_RIGHT) csx::MenuClick(2);
                break;
            }
            if (int key = EngineMouseKey(e.button.button); !csx::MenuOpen() && key >= 0) {
                if (unsigned pad = PadAction(key)) g_pad_held[key] = pad;
                else EngineKeyEvent(key, true);
            } else {
                g_mouse_buttons |= 1u << e.button.button;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            g_pad_held.erase(EngineMouseKey(e.button.button));
            EngineKeyEvent(EngineMouseKey(e.button.button), false);
            g_mouse_buttons &= ~(1u << e.button.button);
            break;
        case SDL_EVENT_MOUSE_WHEEL:  // MWHEELUP / MWHEELDOWN, a press and a release per notch, as the engine expects
            if (csx::BindCapturing() && e.wheel.y != 0) {
                csx::BindInput(e.wheel.y > 0 ? 240 : 239);
            } else if (g_console && e.wheel.y != 0) {
                csx::QueueConsoleKey(e.wheel.y > 0 ? csx::ConsoleKey::PageUp : csx::ConsoleKey::PageDown);
            } else if (MenuMouse() && e.wheel.y != 0) {  // whole notches (SDL adds up a fine wheel's steps)
                if (e.wheel.integer_y) csx::MenuWheel(e.wheel.integer_y);
            } else if (!csx::MenuOpen() && e.wheel.y != 0) {
                // a press and release per whole notch (SDL can join several in one event; a fine wheel's partial
                // step is still one)
                int key = e.wheel.y > 0 ? 240 : 239;
                for (int n = std::max(1, std::abs(e.wheel.integer_y)); n > 0; n--) {
                    EngineKeyEvent(key, true);
                    EngineKeyEvent(key, false);
                }
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (g_console) break;  // the view stays put while the console is open
            if (MenuMouse()) {     // the menus: the pointer, never the view
                float mx, my;
                if (MenuPoint(e.motion.x, e.motion.y, mx, my)) csx::MenuPointer(mx, my);
                else csx::MenuPointer(-1, -1);
                break;
            }
            if (Mouse().stick) {
                g_mouse_dx += e.motion.xrel;
                g_mouse_dy += e.motion.yrel;
            } else if (e.motion.which == window::kInjectedDevice || window::RelativeMouse()) {
                // captured: straight to the view angles, scaled only (no acceleration)
                float k = Mouse().Sens() * kDegreesPerCount;
                csx::AddLook(e.motion.xrel * k, (Mouse().Invert() ? 1.0f : -1.0f) * e.motion.yrel * k);
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            // a split-screen player takes only the controller the compositor gives it (a virtual one)
            if (splitp::Active() && !SDL_IsJoystickVirtual(e.gdevice.which)) break;
            if (!g_pad) {
                g_pad = SDL_OpenGamepad(e.gdevice.which);
                if (g_pad) LOGI("input: gamepad %s", SDL_GetGamepadName(g_pad));
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) {
                SDL_CloseGamepad(g_pad);
                g_pad = nullptr;
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            csx::MenuPointer(-1, -1);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            g_keys.clear();
            g_mouse_buttons = 0;
            for (int k : std::set<int>(g_engine_held)) EngineKeyEvent(k, false);
            g_pad_held.clear();
            break;
        default: break;
    }
}

void EndFrameInput() {}

// Raw Mouse Input, applied when the setting changes (the window thread)
void ApplyMouseSettings() {
    static unsigned seen = ~0u;
    if (pc::Version() == seen) return;
    seen = pc::Version();
    static int applied = -1;
    const int raw = pc::Get("mouse_raw", "1") != "0";
    if (raw == applied) return;
    applied = raw;
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, raw ? "0" : "1");
    const char* driver = SDL_GetCurrentVideoDriver();
    const bool wayland = driver && !std::strcmp(driver, "wayland");
    LOGI("input: Raw Mouse Input %s: %s (video driver %s)", raw ? "on" : "off",
         raw ? (wayland ? "the relative pointer's unaccelerated deltas" : "XInput2 raw values")
             : (wayland ? "the compositor's accelerated pointer deltas, scaled" : "the X server's accelerated motion"),
         driver ? driver : "?");
}

// once per window frame (the window thread): the console's text input on while it is open, the keys held for the
// game let go when it opens; the mouse captured while playing and free over the menus and the console (its pointer
// shown), captured again when play resumes
void UpdateConsole() {
    ApplyMouseSettings();
    bool open = csx::ConsoleOpen();
    std::lock_guard<std::mutex> lock(g_lock);
    SDL_Window* win = static_cast<SDL_Window*>(SdlWindow());
    if (open != g_console) {
        g_console = open;
        g_swallow_text = false;
        if (open) {
            for (int k : std::set<int>(g_engine_held)) EngineKeyEvent(k, false);
            g_pad_held.clear();
            g_mouse_buttons = 0;
        }
        g_keys.clear();
        LOGI("input: console %s", open ? "open" : "closed");
    }
    // text input (the layout's characters) while the console or the name field takes the keyboard
    static bool text = false;
    if (bool want_text = g_console || csx::TextEntryOpen(); want_text != text) {
        text = want_text;
        if (text) SDL_StartTextInput(win);
        else SDL_StopTextInput(win);
        g_keys.clear();
    }
    const bool free = g_console || csx::MenuOpen();
    const bool focused = splitp::Active() ? splitp::Focused() : (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool want = !free && g_capture_play && focused;
    if (splitp::Active()) {  // a split-screen player: the compositor captures the mouse for it when it wants it
        splitp::SendCapture(!free && g_capture_play);
        return;
    }
    static bool failed = false;  // relative mode refused (logged once; asked again only when wanted anew)
    if (want != SDL_GetWindowRelativeMouseMode(win) && !(want && failed)) {
        // SDL drops the motion still queued from before the switch: no turn from the menu's pointer, none left over
        if (!SDL_SetWindowRelativeMouseMode(win, want)) {
            failed = true;
            LOGW("input: relative mouse mode refused: %s", SDL_GetError());
        } else {
            LOGI("input: mouse %s", want ? "captured (playing)" : free ? "free (menu or console)" : "free");
            static bool logged = false;
            if (want && !logged) {
                logged = true;
                const char* driver = SDL_GetCurrentVideoDriver();
                LOGI("input: relative mouse mode on through %s", driver && !std::strcmp(driver, "wayland")
                         ? "Wayland's relative pointer and pointer lock (native relative motion)"
                         : driver && !std::strcmp(driver, "x11") ? "X11 XInput2 raw motion"
                                                                 : driver ? driver : "?");
            }
        }
    }
    if (!want) failed = false;
    static int mode = -1;  // what the log said last: 0 captured, 1 menu or console, 2 released by the player
    int now = !free ? (g_capture_play ? 0 : 2) : 1;
    if (now != mode) {
        mode = now;
        LOGI("input: mouse mode %s", now == 0 ? "play (captured)" : now == 1 ? "menu (pointer)" : "play (released, F1)");
    }
}

// F1 while playing: the mouse let go or captured again; a click in the window while playing captures it
void SetPlayCapture(bool on) { g_capture_play = on; }
bool RelativeMouse() {
    return splitp::Active() ? splitp::Captured()
                            : SDL_GetWindowRelativeMouseMode(static_cast<SDL_Window*>(SdlWindow()));
}
bool PlayCapture() { return g_capture_play; }
bool MouseFree() { return g_console || csx::MenuOpen(); }

bool ConsoleShown() { return g_console; }

// the positional keys as the system's layout prints them, for the menus' key names (pc_bindings.cpp, through
// CSX_KEY_LABELS): lines "C=NAME" (C the engine key's character, NAME in UTF-8), only for keys that differ from US
std::string KeyLabels() {
    std::string labels, shown;
    for (const auto& p : kPositionKeys) {
        if ((p.key >= '0' && p.key <= '9') || labels.find(std::string(1, p.key) + "=") != std::string::npos) continue;
        SDL_Keycode k = SDL_GetKeyFromScancode(p.sc, SDL_KMOD_NONE, false);
        std::string name = k == SDLK_UNKNOWN ? "" : SDL_GetKeyName(k);
        if (name.empty() || name == std::string(1, p.key) || name.find('\n') != std::string::npos) continue;
        labels += std::string(1, p.key) + "=" + name + "\n";
        shown += " " + std::string(1, p.key) + "=" + name;
    }
    LOGI("input: key names from the layout:%s", shown.empty() ? " as US" : shown.c_str());
    return labels;
}

}  // namespace window

namespace input {

void Init() {}

Pad Poll(int port) {
    Pad p;
    p.connected = port == 0;
    if (port != 0) return p;
    std::lock_guard<std::mutex> lock(g_lock);
    // play resumed: keys still down from the menu leave the pad. (Engine keys held when a menu opens stay engine
    // keys until released: +showscores opens the Scoreboard screen and lasts while its key is held.)
    bool menu = csx::MenuOpen();
    if (!menu && g_was_menu) {
        g_keys.clear();
        g_mouse_buttons = 0;
    }
    g_was_menu = menu;
    // keyboard and mouse; on PC Controls WASD are not the stick: its pieces step the selected row's key (D would make
    // Move Right's key E unseen), and those are keys a player presses there to try them
    const bool wasd = !csx::BindRowSelected();
    float lx = wasd ? (Down(SDLK_D) ? 32767.0f : 0) - (Down(SDLK_A) ? 32767.0f : 0) : 0;
    float ly = wasd ? (Down(SDLK_W) ? 32767.0f : 0) - (Down(SDLK_S) ? 32767.0f : 0) : 0;
    float rx = g_mouse_dx * kMouseScale, ry = -g_mouse_dy * kMouseScale;
    g_mouse_dx = g_mouse_dy = 0;
    for (auto& [key, buttons] : g_pad_held) p.buttons |= uint16_t(buttons);
    if (Down(SDLK_UP)) p.buttons |= 1;
    if (Down(SDLK_DOWN)) p.buttons |= 2;
    if (Down(SDLK_LEFT)) p.buttons |= 4;
    if (Down(SDLK_RIGHT)) p.buttons |= 8;
    // the server browser's screens (pc_browser.cpp): Escape goes back (B), F5 refreshes (X), Page Up / Down the tabs
    const bool browser = menu && csx::BrowserKeys();
    if (Down(SDLK_RETURN) || (Down(SDLK_ESCAPE) && !browser)) p.buttons |= 16;
    if (browser && Down(SDLK_ESCAPE)) p.analog[1] = 255;
    if (browser && Down(SDLK_F5)) p.analog[2] = 255;
    if (browser && Down(SDLK_PAGEUP)) p.analog[6] = 255;
    if (browser && Down(SDLK_PAGEDOWN)) p.analog[7] = 255;
    if (Down(SDLK_TAB)) p.buttons |= 32;
    if (Down(SDLK_LSHIFT)) p.buttons |= 64;
    if (g_mouse_buttons & (1u << SDL_BUTTON_MIDDLE)) p.buttons |= 128;
    if (Down(SDLK_SPACE)) p.analog[0] = 255;
    if (Down(SDLK_LCTRL)) p.analog[1] = 255;
    if (Down(SDLK_R)) p.analog[2] = 255;
    if (Down(SDLK_Q)) p.analog[3] = 255;
    if (Down(SDLK_2)) p.analog[4] = 255;  // black
    if (Down(SDLK_1)) p.analog[5] = 255;  // white
    if (g_mouse_buttons & (1u << SDL_BUTTON_RIGHT)) p.analog[6] = 255;
    if (g_mouse_buttons & (1u << SDL_BUTTON_LEFT)) p.analog[7] = 255;
    // the gamepad on top: SDL's y axes point down, the console's up
    if (g_pad) {
        auto axis = [](SDL_GamepadAxis a) { return float(SDL_GetGamepadAxis(g_pad, a)); };
        auto btn = [](SDL_GamepadButton b) { return SDL_GetGamepadButton(g_pad, b); };
        lx += axis(SDL_GAMEPAD_AXIS_LEFTX);
        ly -= axis(SDL_GAMEPAD_AXIS_LEFTY);
        rx += axis(SDL_GAMEPAD_AXIS_RIGHTX);
        ry -= axis(SDL_GAMEPAD_AXIS_RIGHTY);
        const struct {
            SDL_GamepadButton b;
            uint16_t bit;
        } digital[] = {{SDL_GAMEPAD_BUTTON_DPAD_UP, 1},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 2},
                       {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 4},    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 8},
                       {SDL_GAMEPAD_BUTTON_START, 16},       {SDL_GAMEPAD_BUTTON_BACK, 32},
                       {SDL_GAMEPAD_BUTTON_LEFT_STICK, 64},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 128}};
        for (const auto& d : digital)
            if (btn(d.b)) p.buttons |= d.bit;
        const SDL_GamepadButton analog[6] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                             SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
                                             SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,  // black
                                             SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};  // white
        for (int i = 0; i < 6; i++)
            if (btn(analog[i])) p.analog[i] = 255;

        p.analog[6] = std::max<uint8_t>(p.analog[6], uint8_t(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767));
        p.analog[7] = std::max<uint8_t>(p.analog[7], uint8_t(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767));
    }
    // a scripted press (tests)
    {
        const std::string cmd = (app::ExeDir() / "cmd").string();
        if (FILE* f = std::fopen(cmd.c_str(), "r")) {
            char b[64] = {};
            double frames = 3;
            if (std::fscanf(f, "%63s %lf", b, &frames) >= 1)
                g_scripted = {harness::g_frames.load() + uint64_t(std::max(frames, 1.0)), b};
            std::fclose(f);
            std::remove(cmd.c_str());
        }
        if (harness::g_frames.load() <= g_scripted.until) ApplyScripted(g_scripted.buttons, p, lx, ly, rx, ry);
    }
    p.lx = Stick(lx);
    p.ly = Stick(ly);
    p.rx = Stick(rx);
    p.ry = Stick(ry);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (port == 0 && g_pad) SDL_RumbleGamepad(g_pad, left, right, 200);
}

}  // namespace input
