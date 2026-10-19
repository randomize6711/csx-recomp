// input::Poll for the Linux window: the first SDL gamepad and the keyboard and mouse, both as the console controller on
// port 0. The title reads only the controller (XInputGetState), so keyboard and mouse are folded into one:
//   WASD left stick, mouse look (below), arrows d-pad, Space A, Left Ctrl B, R X, Q Y,
//   1 white, 2 black, left mouse right trigger, right mouse left trigger, Enter / Escape START, Tab BACK,
//   Left Shift left stick click, middle mouse right stick click; F1 releases or captures the mouse.
// Mouse look: while the mouse is captured (relative mode, SDL's unaccelerated motion), each count turns the view by
// 0.022 degrees x the sensitivity (CSX_MOUSE_SENS, else Options > PC Options, else 3: GoldSrc's m_yaw / m_pitch and
// CS's default) through the title's look routine (src/game/mouse_look.cpp); CSX_MOUSE_INVERT=1 inverts the pitch.
// CSX_MOUSE=stick feeds the right stick instead, as before (the title's stick response, acceleration and aim assist
// apply then).
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <set>

#include "app/app.h"
#include "core/input.h"
#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
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

int16_t Stick(float v) { return int16_t(std::clamp(v, -32767.0f, 32767.0f)); }

}  // namespace

namespace window {

void HandleEvent(const void* p) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    std::lock_guard<std::mutex> lock(g_lock);
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN: g_keys.insert(e.key.key); break;
        case SDL_EVENT_KEY_UP: g_keys.erase(e.key.key); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: g_mouse_buttons |= 1u << e.button.button; break;
        case SDL_EVENT_MOUSE_BUTTON_UP: g_mouse_buttons &= ~(1u << e.button.button); break;
        case SDL_EVENT_MOUSE_MOTION:
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
            break;
        default: break;
    }
}

void EndFrameInput() {}

}  // namespace window

namespace input {

void Init() {}

Pad Poll(int port) {
    Pad p;
    p.connected = port == 0;
    if (port != 0) return p;
    std::lock_guard<std::mutex> lock(g_lock);
    // keyboard and mouse
    float lx = (Down(SDLK_D) ? 32767.0f : 0) - (Down(SDLK_A) ? 32767.0f : 0);
    float ly = (Down(SDLK_W) ? 32767.0f : 0) - (Down(SDLK_S) ? 32767.0f : 0);
    float rx = g_mouse_dx * kMouseScale, ry = -g_mouse_dy * kMouseScale;
    g_mouse_dx = g_mouse_dy = 0;
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
