// input::Poll for the Linux window: the first SDL gamepad, and the keyboard and mouse. While a menu screen of the game
// is open the keyboard and mouse act as the console controller on port 0 (the title's menus read only the pad):
//   WASD left stick, arrows d-pad, Space A, Left Ctrl B, R X, Q Y, 1 white, 2 black, left mouse right trigger,
//   right mouse left trigger, Enter / Escape START, Tab BACK, Left Shift left stick click, middle mouse right stick
//   click. While playing they are the engine's own keys and run their bindings (Options > PC Controls), Escape
//   staying START (the game menu). F1 releases or captures the mouse.
// The developer console (src/game/pc_console.cpp): the key bound to Toggle Developer Console (PC Controls; by default
// the key left of 1, whatever the layout prints on it) opens and closes it, playing or in a menu. While it is open the
// keyboard is its alone: typed text (SDL text input, so the layout's characters), Backspace, Enter, Up / Down
// (history), Tab (completion), Page Up / Down and the wheel (scrolling), Escape closing it; held game keys are let go,
// and the mouse is released, captured again when it closes.
// Mouse look: while the mouse is captured (relative mode, SDL's unaccelerated motion), each count turns the view by
// 0.022 degrees x the sensitivity (CSX_MOUSE_SENS, else Options > PC Options, else 3: GoldSrc's m_yaw / m_pitch and
// CS's default) through the title's look routine (src/game/mouse_look.cpp); CSX_MOUSE_INVERT=1 inverts the pitch.
// CSX_MOUSE=stick feeds the right stick instead, as before (the title's stick response, acceleration and aim assist
// apply then).
#include <SDL3/SDL.h>

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

// SDL key -> engine key number (keys.c: letters, digits and punctuation as their characters, the rest from 127 up).
// The key left of 1 is ` by its position (SDL_SCANCODE_GRAVE), as PC games take the console key: it prints ` on a US
// layout, § on a Swedish one, ^ on a German one
int EngineKey(SDL_Keycode k, SDL_Scancode sc = SDL_SCANCODE_UNKNOWN) {
    if (sc == SDL_SCANCODE_GRAVE) return '`';
    if ((k >= SDLK_A && k <= SDLK_Z) || (k >= SDLK_0 && k <= SDLK_9)) return int(k);
    switch (k) {
        case SDLK_SPACE: return 32;
        case SDLK_TAB: return 9;
        case SDLK_RETURN: return 13;
        case SDLK_BACKSPACE: return 127;
        case SDLK_GRAVE: case SDLK_MINUS: case SDLK_EQUALS: case SDLK_LEFTBRACKET: case SDLK_RIGHTBRACKET:
        case SDLK_SEMICOLON: case SDLK_COMMA: case SDLK_PERIOD: case SDLK_SLASH: case SDLK_BACKSLASH:
        case SDLK_APOSTROPHE: return int(k);
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
bool g_mouse_was_captured = false;
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
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (g_console) break;
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
            if (g_console && e.wheel.y != 0) {
                csx::QueueConsoleKey(e.wheel.y > 0 ? csx::ConsoleKey::PageUp : csx::ConsoleKey::PageDown);
            } else if (!csx::MenuOpen() && e.wheel.y != 0) {
                int key = e.wheel.y > 0 ? 240 : 239;
                EngineKeyEvent(key, true);
                EngineKeyEvent(key, false);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (g_console) break;  // the view stays put while the console is open
            if (Mouse().stick) {
                g_mouse_dx += e.motion.xrel;
                g_mouse_dy += e.motion.yrel;
            } else if (e.motion.which == window::kInjectedDevice ||
                       SDL_GetWindowRelativeMouseMode(static_cast<SDL_Window*>(window::SdlWindow()))) {
                // captured: straight to the view angles, scaled only (no acceleration)
                float k = Mouse().Sens() * kDegreesPerCount;
                csx::AddLook(e.motion.xrel * k, (Mouse().Invert() ? 1.0f : -1.0f) * e.motion.yrel * k);
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
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

// the console opened or closed (once per window frame, the window thread): text input on while it is open, the keys
// held for the game let go, the mouse released and captured again afterwards
void UpdateConsole() {
    bool open = csx::ConsoleOpen();
    std::lock_guard<std::mutex> lock(g_lock);
    if (open == g_console) return;
    g_console = open;
    g_swallow_text = false;
    SDL_Window* win = static_cast<SDL_Window*>(SdlWindow());
    if (open) {
        for (int k : std::set<int>(g_engine_held)) EngineKeyEvent(k, false);
        g_pad_held.clear();
        g_keys.clear();
        g_mouse_buttons = 0;
        g_mouse_was_captured = SDL_GetWindowRelativeMouseMode(win);
        SDL_SetWindowRelativeMouseMode(win, false);
        SDL_StartTextInput(win);
    } else {
        SDL_StopTextInput(win);
        g_keys.clear();
        if (g_mouse_was_captured) SDL_SetWindowRelativeMouseMode(win, true);
    }
    if (open)
        LOGI("input: console open, mouse released (%s before)", g_mouse_was_captured ? "captured" : "free");
    else
        LOGI("input: console closed, mouse %s", g_mouse_was_captured ? "captured again" : "left free");
}

bool ConsoleShown() { return g_console; }

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
    // keyboard and mouse
    float lx = (Down(SDLK_D) ? 32767.0f : 0) - (Down(SDLK_A) ? 32767.0f : 0);
    float ly = (Down(SDLK_W) ? 32767.0f : 0) - (Down(SDLK_S) ? 32767.0f : 0);
    float rx = g_mouse_dx * kMouseScale, ry = -g_mouse_dy * kMouseScale;
    g_mouse_dx = g_mouse_dy = 0;
    for (auto& [key, buttons] : g_pad_held) p.buttons |= uint16_t(buttons);
    if (Down(SDLK_UP)) p.buttons |= 1;
    if (Down(SDLK_DOWN)) p.buttons |= 2;
    if (Down(SDLK_LEFT)) p.buttons |= 4;
    if (Down(SDLK_RIGHT)) p.buttons |= 8;
    if (Down(SDLK_RETURN) || Down(SDLK_ESCAPE)) p.buttons |= 16;
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
