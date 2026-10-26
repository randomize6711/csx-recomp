// Linux window front end: boots the image as the headless harness does (main_test.cpp), then runs an SDL3 window on
// the main thread that shows the latest presented frame at 4:3 and feeds SDL input to input::Poll.
//   <exe> <game folder> [--instance N]     run from the folder that gets logs and hdd (the game folder may come from
//                                         GAME_DIR, as after a title relaunch)
// FPS (default 30, the console's rate for CS) paces presented frames; F11 or Alt+Enter toggles fullscreen; a click
// captures the mouse, F1 or leaving the window releases it.
#include <SDL3/SDL.h>
#include <csignal>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <functional>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "harness.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "linux/split_player.h"
#include "linux/window.h"

int CompositorMain(const std::string& game);  // compositor.cpp: split-screen's window

namespace {

SDL_Window* g_window = nullptr;
bool g_vulkan = false;
std::atomic<float> g_refresh{0};

// the refresh rate of the window's display, logged when it changes (start, the window moved, the mode changed)
void UpdateRefresh(SDL_Window* win) {
    const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    float hz = dm ? dm->refresh_rate : 0.0f;
    if (hz == g_refresh) return;
    g_refresh = hz;
    LOGI("window: display refresh %.2f Hz%s", hz, dm ? "" : " (unknown)");
}
std::mutex g_shot_lock;
std::string g_shot;

// setenv where Windows has _putenv_s (which also sets the process environment a relaunch's CreateProcess inherits)
void SetEnv(const char* name, const char* value, bool overwrite = true) {
    if (!overwrite && std::getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

#ifndef _WIN32
// How the process ends, for an exit that left nothing else (a window test once ended with no core, no error and no
// log line): SIGPIPE (a write to a closed socket or pipe, the X connection or the network's) is reported and survived
// (the write fails instead), SIGTERM / SIGHUP / SIGINT reported before their default action, a plain exit() logged.
// Written with write(2) to stderr (run.out), safe in a handler.
void SignalNote(int sig) {
    const char* text = sig == SIGPIPE ? "window: SIGPIPE (write to a closed socket or pipe), carried on\n"
                       : sig == SIGTERM ? "window: SIGTERM, ending\n"
                       : sig == SIGHUP  ? "window: SIGHUP, ending\n"
                                        : "window: SIGINT, ending\n";
    ssize_t n = write(2, text, std::strlen(text));
    (void)n;
    if (sig != SIGPIPE) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }
}
void WatchExit() {
    for (int sig : {SIGPIPE, SIGTERM, SIGHUP, SIGINT}) std::signal(sig, SignalNote);
    std::atexit([] {
        LOGI("window: process exit");
        logx::Flush();
    });
}
#else
void WatchExit() {}
#endif

#ifdef _WIN32
// A title relaunch on Windows starts a new process (kernel::Relaunch, CreateProcess with " --relaunched") and keeps the
// old one, window up, until the new one has shown its first frames and signals this event (the SDK's app layer does
// it in app/handover.cpp, which this front end does not use)
HANDLE g_handover = nullptr;
void OpenHandover() {
    g_handover = OpenEventW(EVENT_MODIFY_STATE, FALSE, kernel::HandoverEvent(GetCurrentProcessId()).c_str());
    if (g_handover) LOGI("window: relaunched, the previous instance waits for the first frames");
}
void FinishHandover() {
    if (!g_handover || harness::g_frames < 2) return;
    SetEvent(g_handover);
    CloseHandle(g_handover);
    g_handover = nullptr;
}
#else
void OpenHandover() {}
void FinishHandover() {}
#endif

// Options > PC Options: Display Mode, windowed or fullscreen (SDL's borderless desktop fullscreen). Applied once the
// window shows its first frame: asked earlier (at creation or before the window is mapped) a tiling Wayland
// compositor (Hyprland) leaves the window tiled. Only a change of the setting applies it later.
bool WantFullscreen() { return pc::Get("display_mode", "windowed") == "fullscreen"; }
void ApplyDisplaySettings(SDL_Window* win) {
    bool full = WantFullscreen();
    LOGI("window: Display Mode %s applied", full ? "fullscreen" : "windowed");
    if (full == ((SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0)) return;
    SDL_SetWindowFullscreen(win, full);
    SDL_SyncWindow(win);  // the window manager's answer, for the log
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    LOGI("window: %s asked, now %s %dx%d", full ? "fullscreen" : "windowed",
         (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) ? "fullscreen" : "windowed", w, h);
}

}  // namespace

namespace window {
void* SdlWindow() { return g_window; }
bool UseVulkan() { return g_vulkan; }
float DisplayRefresh() { return g_refresh; }
std::string TakeShotRequest() {
    std::lock_guard<std::mutex> lock(g_shot_lock);
    return std::exchange(g_shot, {});
}
void RequestShot(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_shot_lock);
    g_shot = name;
}
}  // namespace window

namespace {

// keyboard id the keys file's events carry; with CSX_TEST_INPUT_ONLY=1 every other input event is dropped, so a test
// run is not steered by whatever the desktop types into the window when it takes focus
constexpr SDL_KeyboardID kInjectedKeyboard = window::kInjectedDevice;

bool HostInputEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: return e.key.which != kInjectedKeyboard;
        case SDL_EVENT_MOUSE_MOTION: return e.motion.which != window::kInjectedDevice;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: return e.button.which != window::kInjectedDevice;
        case SDL_EVENT_MOUSE_WHEEL: return e.wheel.which != window::kInjectedDevice;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: return true;
        default: return false;
    }
}

// "mouse" request "stream DX DY N HZ" (tests): N relative motions of DX DY counts pushed from an SDL timer thread at
// HZ a second, so they reach the event queue as a mouse's do, whenever the window loop is between polls
struct MouseStream {
    std::atomic<int> left{0};
    float dx = 0, dy = 0;
    SDL_WindowID window = 0;
    int total = 0;
};
MouseStream g_stream;
Uint64 StreamTick(void*, SDL_TimerID, Uint64 interval) {
    SDL_Event ev{};
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.windowID = g_stream.window;
    ev.motion.which = window::kInjectedDevice;
    ev.motion.xrel = g_stream.dx;
    ev.motion.yrel = g_stream.dy;
    SDL_PushEvent(&ev);
    if (--g_stream.left > 0) return interval;
    LOGI("window: mouse stream done, %d motions", g_stream.total);
    return 0;
}

// the frame scaled to the window at the title's display aspect (4:3, or PC widescreen's), whatever the surface's own
// size (640x960 for CS)
SDL_FRect Letterbox(int ww, int wh, float aspect) {
    float w = float(ww), h = float(wh), tw = w, th = w / aspect;
    if (th > h) {
        th = h;
        tw = h * aspect;
    }
    return {(w - tw) * 0.5f, (h - th) * 0.5f, tw, th};
}

}  // namespace

int main(int argc, char** argv) {
    std::string game;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) SetEnv("INSTANCE", argv[++i]);
        else if (!std::strcmp(argv[i], "--renderer") && i + 1 < argc) SetEnv("CSX_RENDERER", argv[++i]);
        else if (!std::strcmp(argv[i], "--relaunched")) continue;  // Windows relaunch marker (kernel::Relaunch)
        else game = argv[i];
    }
    // the renderer survives a title relaunch (exec without arguments) through the environment
    g_vulkan = std::getenv("CSX_RENDERER") && !std::strcmp(std::getenv("CSX_RENDERER"), "vulkan");
    if (game.empty() && std::getenv("GAME_DIR")) game = std::getenv("GAME_DIR");
    SetEnv("GAME_DIR", game.c_str());
    SetEnv("FPS", "30", false);
    if (game.empty()) {
        std::fprintf(stderr, "usage: %s <game folder> [--instance N]\n", argv[0]);
        return 2;
    }
    // split-screen (docs/splitscreen.md): the compositor (the window, the players' processes), or one of its players
    if (std::getenv("CSX_SPLIT_COMPOSITOR") && *std::getenv("CSX_SPLIT_COMPOSITOR") == '1') return CompositorMain(game);
    const bool split_player = splitp::Active();
    if (split_player) {
        g_vulkan = true;  // its frames go to the compositor through exported Vulkan images
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");  // no window of its own: input comes from the compositor
        // its hidden window never has the focus: its controller (the compositor's virtual one) counts anyway
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    WatchExit();
    OpenHandover();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        LOGE("window: SDL_Init failed: %s", SDL_GetError());
        logx::Flush();
        return 1;
    }
    // the display's aspect, for PC widescreen's "auto" (src/game/pc_wide.cpp), and a first window of the frame's shape
    float aspect = 4.0f / 3.0f;
    int win_w = 1280, win_h = 960;
    if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()); dm && dm->w > 0 && dm->h > 0) {
        char text[32];
        std::snprintf(text, sizeof text, "%.5f", double(dm->w) / dm->h);
        SetEnv("CSX_DISPLAY_ASPECT", text, false);
        std::string a = pc::Get("aspect", "auto");
        aspect = a == "16:9" ? 16.0f / 9.0f : a == "16:10" ? 1.6f : a == "21:9" ? 64.0f / 27.0f
               : a == "auto" ? float(dm->w) / dm->h : 4.0f / 3.0f;
        aspect = std::clamp(aspect, 4.0f / 3.0f, 2.5f);
        win_w = std::min(int(960 * aspect), dm->w * 9 / 10);
        win_h = int(win_w / aspect);
    }
    // high pixel density: on a scaled desktop (Hyprland at 1.6) the surface has the display's own pixels (3840x2160,
    // not the 2400x1350 of its logical size, which the compositor scaled up); window sizes and pointer positions stay
    // logical, the swapchain and the software renderer's output take the pixels. CSX_HIDPI=0 asks for the logical size
    const bool hidpi = !(std::getenv("CSX_HIDPI") && *std::getenv("CSX_HIDPI") == '0');
    SDL_Window* win = SDL_CreateWindow("Counter-Strike", win_w, win_h,
                                       split_player ? SDL_WINDOW_HIDDEN
                                       : SDL_WINDOW_RESIZABLE | (hidpi ? SDL_WINDOW_HIGH_PIXEL_DENSITY : SDL_WindowFlags(0)) |
                                           (g_vulkan ? SDL_WINDOW_VULKAN : SDL_WindowFlags(0)));
    g_window = win;
    if (split_player) splitp::Start(win);
    else csx::SetSplitStarter(splitp::StartFromWindow);  // the Split Screen screen restarts the window as the compositor
    if (win) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        LOGI("window: size %dx%d (%dx%d pixels, density %.2f, display scale %.2f%s)", win_w, win_h, pw, ph,
             SDL_GetWindowPixelDensity(win), SDL_GetWindowDisplayScale(win), hidpi ? "" : ", CSX_HIDPI=0");
    }
    // the display the window is on: its desktop mode and the modes it lists, for PC Options > Resolution
    // (src/game/pc_menu.cpp); the Vulkan renderer draws at the chosen size, the display mode itself is not changed
    if (SDL_DisplayID display = win ? SDL_GetDisplayForWindow(win) : 0) {
        // in the display's pixels (a scaled desktop's mode is given in logical units with its pixel density)
        auto pixels = [](const SDL_DisplayMode* m) {
            float d = m->pixel_density > 0 ? m->pixel_density : 1.0f;
            return std::make_pair(int(std::lround(m->w * d)), int(std::lround(m->h * d)));
        };
        if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(display)) {
            auto [w, h] = pixels(dm);
            SetEnv("CSX_DESKTOP_MODE", (std::to_string(w) + "x" + std::to_string(h)).c_str(), false);
        }
        int count = 0;
        std::string list;
        if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count)) {
            std::vector<std::pair<int, int>> sizes;  // largest first, each size once whatever its refresh rates
            for (int i = 0; i < count; i++)
                if (auto p = pixels(modes[i]); p.first >= 640 && p.second >= 480) sizes.push_back(p);
            std::sort(sizes.begin(), sizes.end(), std::greater<>());
            sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
            for (size_t i = 0; i < sizes.size() && i < 16; i++)
                list += (list.empty() ? "" : ",") + std::to_string(sizes[i].first) + "x" + std::to_string(sizes[i].second);
            SDL_free(modes);
        }
        SetEnv("CSX_DISPLAY_MODES", list.c_str(), false);
        LOGI("window: display %s, desktop %s, modes %s", SDL_GetDisplayName(display),
             std::getenv("CSX_DESKTOP_MODE") ? std::getenv("CSX_DESKTOP_MODE") : "?", list.c_str());
    }
    // the punctuation keys' names as the keyboard layout prints them, for PC Controls (kept when given: a relaunch, a
    // test's layout)
    if (!std::getenv("CSX_KEY_LABELS")) SetEnv("CSX_KEY_LABELS", window::KeyLabels().c_str());
    // the Vulkan backend presents by itself; the software path shows frames through an SDL renderer
    SDL_Renderer* ren = win && !g_vulkan ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!win || (!g_vulkan && !ren)) {
        LOGE("window: cannot create the window: %s", SDL_GetError());
        logx::Flush();
        return 1;
    }
    // VSync (PC Options) for the software renderer: the window's own presents of the latest frame wait for the
    // display's blank or not; the title renders on its threads either way, so its frame rate is the limit's alone
    bool vsync = pc::Get("vsync", "1") != "0";
    if (ren) SDL_SetRenderVSync(ren, vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
    if (ren) LOGI("window: VSync %s", vsync ? "on" : "off");
    UpdateRefresh(win);
    unsigned settings_seen = pc::Version();
    bool display_applied = false;
    std::string mode_applied;  // the Display Mode last applied
    LOGI("window: SDL %d.%d.%d, %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
         g_vulkan ? "Vulkan renderer" : SDL_GetRendererName(ren));

    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (app::kGame.window) app::kGame.window(win);
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    std::ifstream f(game + "/" + IMAGE_XBE, std::ios::binary);
    std::vector<uint8_t> xbe((std::istreambuf_iterator<char>(f)), {});
    if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game, (app::ExeDir() / hdd).string())) return 1;

    SDL_Texture* tex = nullptr;
    uint32_t tw = 0, th = 0;
    uint64_t shown = 0;
    std::vector<uint8_t> px;
    auto last_flush = std::chrono::steady_clock::now();
    // WINDOW_SHOT_EVERY=N: the window's own output (letterbox and scaling included) saved every N seconds to
    // shots/window_<n>.bmp, to check what the window shows
    const long shot_every = std::getenv("WINDOW_SHOT_EVERY") ? std::atol(std::getenv("WINDOW_SHOT_EVERY")) : 0;
    auto next_shot = last_flush + std::chrono::seconds(shot_every);
    int shots = 0;
    // a file named keys in the run folder ("KEYNAME SECONDS", SDL key names, "KEYNAME#SCANCODENAME" for a key at a
    // layout's position) presses that key through SDL's own event queue, so tests drive the window's input path as a
    // keyboard would
    struct Held {
        SDL_Keycode key;
        std::chrono::steady_clock::time_point until;
        uint8_t button = 0;  // a mouse button instead of a key
        SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    };
    std::vector<Held> held;
    auto push_key = [&](SDL_Keycode k, SDL_Scancode sc, bool down) {
        SDL_Event ev{};
        ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        ev.key.key = k;
        ev.key.scancode = sc;
        ev.key.down = down;
        ev.key.windowID = SDL_GetWindowID(win);
        ev.key.which = kInjectedKeyboard;
        SDL_PushEvent(&ev);
    };
    const std::string keys_file = (app::ExeDir() / "keys").string();
    // "mouse" request: "DX DY" counts of relative motion, as a captured mouse gives them (tests of the mouse look),
    // "button N SECONDS" a mouse button (SDL numbering: 1 left, 2 middle, 3 right) held, "wheel N" wheel notches
    float pointer_x = 0, pointer_y = 0;  // where the injected pointer is, window pixels
    auto push_button = [&](uint8_t b, bool down) {
        SDL_Event ev{};
        ev.button.x = pointer_x;
        ev.button.y = pointer_y;
        ev.button.clicks = b == 11 ? 2 : 1;  // 11: a left double click
        if (b == 11) b = SDL_BUTTON_LEFT;
        ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        ev.button.button = b;
        ev.button.down = down;
        ev.button.windowID = SDL_GetWindowID(win);
        ev.button.which = window::kInjectedDevice;
        SDL_PushEvent(&ev);
    };
    const std::string mouse_file = (app::ExeDir() / "mouse").string();
    // "text" request: its contents typed as SDL text input (what a keyboard layout gives while text input is on)
    const std::string text_file = (app::ExeDir() / "text").string();
    const std::string winctl_file = (app::ExeDir() / "winctl").string();
    std::deque<std::string> typed;  // SDL keeps the pointer until the event is read
    const bool test_input_only = std::getenv("CSX_TEST_INPUT_ONLY") && *std::getenv("CSX_TEST_INPUT_ONLY") == '1';
    if (test_input_only) LOGI("window: test input only, host keyboard, mouse and gamepads ignored");
    for (bool quit = false; !quit;) {
        auto tnow = std::chrono::steady_clock::now();
        // the request files every 5 ms (the loop wakes for each mouse event)
        static auto next_files = tnow;
        // (a split-screen player leaves them to the compositor, whose folder player 1 shares)
        const bool files = tnow >= next_files && !split_player;
        if (files) next_files = tnow + std::chrono::milliseconds(5);
        auto request = [&](const std::string& path) { return files ? std::fopen(path.c_str(), "r") : nullptr; };
        if (FILE* kf = request(keys_file)) {
            char name[64] = {};
            double secs = 0.1;
            if (std::fscanf(kf, "%63s %lf", name, &secs) >= 1) {
                // SDL key names with spaces ("Left Ctrl") are written with underscores in the request file
                for (char* c = name; *c; c++)
                    if (*c == '_') *c = ' ';
                // "NAME#SCANCODE": the key a layout puts at that position (Swedish Å#[), else the system layout's
                char* at = std::strchr(name, '#');
                if (at) *at++ = 0;
                SDL_Keycode k = SDL_GetKeyFromName(name);
                SDL_Scancode sc = at ? SDL_GetScancodeFromName(at) : SDL_GetScancodeFromKey(k, nullptr);
                if (k != SDLK_UNKNOWN) {
                    push_key(k, sc, true);
                    held.push_back({k, tnow + std::chrono::milliseconds(long(secs * 1000)), 0, sc});
                } else {
                    LOGW("window: unknown key %s", name);
                }
            }
            std::fclose(kf);
            std::remove(keys_file.c_str());
        }
        // "winctl" request (tests): "fullscreen 1|0" changes the window as a compositor's own key would (the settings
        // untouched)
        if (FILE* wf = request(winctl_file)) {
            int on = 0;
            if (std::fscanf(wf, "fullscreen %d", &on) == 1) {
                SDL_SetWindowFullscreen(win, on != 0);
                LOGI("window: made %s from outside (test)", on ? "full screen" : "windowed");
            }
            std::fclose(wf);
            std::remove(winctl_file.c_str());
        }
        if (FILE* tf = request(text_file)) {
            char text[256] = {};
            if (std::fgets(text, sizeof text, tf)) {
                typed.emplace_back(text);
                while (!typed.back().empty() && (typed.back().back() == '\n' || typed.back().back() == '\r'))
                    typed.back().pop_back();
                if (typed.size() > 16) typed.pop_front();
                SDL_Event ev{};
                ev.type = SDL_EVENT_TEXT_INPUT;
                ev.text.windowID = SDL_GetWindowID(win);
                ev.text.text = typed.back().c_str();
                SDL_PushEvent(&ev);
            }
            std::fclose(tf);
            std::remove(text_file.c_str());
        }
        if (FILE* mf = request(mouse_file)) {
            float dx = 0, dy = 0;
            char word[16] = {};
            double secs = 0.1;
            int n = 0;
            long at = std::ftell(mf);
            double hz = 0;
            if (std::fscanf(mf, "stream %f %f %d %lf", &dx, &dy, &n, &hz) == 4 && n > 0 && hz > 0) {
                if (g_stream.left == 0) {
                    g_stream.dx = dx;
                    g_stream.dy = dy;
                    g_stream.total = n;
                    g_stream.window = SDL_GetWindowID(win);
                    g_stream.left = n;
                    LOGI("window: mouse stream of %d motions of %g %g at %g a second", n, dx, dy, hz);
                    SDL_AddTimerNS(Uint64(1e9 / hz), StreamTick, nullptr);
                }
            } else if (std::fseek(mf, at, SEEK_SET) == 0 && std::fscanf(mf, "button %d %lf", &n, &secs) >= 1 &&
                       n > 0) {
                push_button(uint8_t(n), true);
                held.push_back({SDLK_UNKNOWN, tnow + std::chrono::milliseconds(long(secs * 1000)), uint8_t(n)});
            } else if (std::fseek(mf, at, SEEK_SET) == 0 && std::fscanf(mf, "%15s %d", word, &n) == 2 &&
                       !std::strcmp(word, "wheel")) {
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_WHEEL;
                ev.wheel.windowID = SDL_GetWindowID(win);
                ev.wheel.which = window::kInjectedDevice;
                ev.wheel.y = float(n);
                ev.wheel.integer_y = n;
                SDL_PushEvent(&ev);
            } else if (std::fseek(mf, at, SEEK_SET) == 0 && std::fscanf(mf, "window %f %f", &dx, &dy) == 2) {
                // "window X Y": the pointer moved to that point of the window (logical pixels), as a real mouse
                pointer_x = dx;
                pointer_y = dy;
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.windowID = SDL_GetWindowID(win);
                ev.motion.which = window::kInjectedDevice;
                ev.motion.x = dx;
                ev.motion.y = dy;
                SDL_PushEvent(&ev);
            } else if (std::fseek(mf, at, SEEK_SET) == 0 && std::fscanf(mf, "menu %f %f", &dx, &dy) == 2) {
                // "menu X Y": the pointer moved to that point of the menus' 640x480, through the window's own mapping
                int ww = 0, wh = 0;
                SDL_GetWindowSize(win, &ww, &wh);
                const float aspect = window::Latest().aspect, frame_w = 480.0f * aspect;
                float tw = float(ww), th = tw / aspect;
                if (th > float(wh)) {
                    th = float(wh);
                    tw = th * aspect;
                }
                pointer_x = (float(ww) - tw) / 2 + (dx + (frame_w - 640.0f) / 2) / frame_w * tw;
                pointer_y = (float(wh) - th) / 2 + dy / 480.0f * th;
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.windowID = SDL_GetWindowID(win);
                ev.motion.which = window::kInjectedDevice;
                ev.motion.x = pointer_x;
                ev.motion.y = pointer_y;
                SDL_PushEvent(&ev);
            } else if (std::fseek(mf, at, SEEK_SET) == 0 && std::fscanf(mf, "%f %f", &dx, &dy) == 2) {
                SDL_Event ev{};
                ev.type = SDL_EVENT_MOUSE_MOTION;
                ev.motion.windowID = SDL_GetWindowID(win);
                ev.motion.which = window::kInjectedDevice;
                ev.motion.xrel = dx;
                ev.motion.yrel = dy;
                SDL_PushEvent(&ev);
            }
            std::fclose(mf);
            std::remove(mouse_file.c_str());
        }
        for (size_t i = 0; i < held.size();) {
            if (tnow >= held[i].until) {
                if (held[i].button) push_button(held[i].button, false);
                else push_key(held[i].key, held[i].sc, false);
                held.erase(held.begin() + long(i));
            } else {
                i++;
            }
        }
        splitp::Frame();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (split_player) {  // the compositor's window keeps full screen, focus and capture; input only here
                window::HandleEvent(&e);
                continue;
            }
            if (test_input_only && HostInputEvent(e)) continue;
            if (test_input_only && e.type == SDL_EVENT_TEXT_INPUT &&
                std::none_of(typed.begin(), typed.end(),
                             [&](const std::string& t) { return t.c_str() == e.text.text; }))
                continue;  // the host keyboard's text, not a scenario's
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                bool alt_enter = e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT);
                if (e.key.key == SDLK_F11 || alt_enter) {
                    // kept as the PC Options Display Mode
                    bool full = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
                    pc::Set("display_mode", full ? "fullscreen" : "windowed");
                    pc::Save();
                    continue;
                }
                if (e.key.key == SDLK_F1 && !window::MouseFree()) window::SetPlayCapture(!window::PlayCapture());
            }
            // playing, the mouse is captured (window::UpdateConsole) and let go with F1 or when the window loses focus; a
            // click in the window while it is let go captures it again (the click itself is not a shot)
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !SDL_GetWindowRelativeMouseMode(win) &&
                e.button.which != window::kInjectedDevice && !window::MouseFree()) {
                window::SetPlayCapture(true);
                continue;
            }
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) SDL_SetWindowRelativeMouseMode(win, false);
            if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
                UpdateRefresh(win);
            if (e.type == SDL_EVENT_WINDOW_MINIMIZED) LOGI("window: minimized");
            if (e.type == SDL_EVENT_WINDOW_RESTORED) LOGI("window: restored");
            if (e.type == SDL_EVENT_WINDOW_RESIZED) {  // logical size (the pointer's units), then the pixels
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(win, &pw, &ph);
                LOGI("window: size %dx%d (%dx%d pixels)", int(e.window.data1), int(e.window.data2), pw, ph);
            }
            // the free pointer's motion only matters to the menus
            if (e.type == SDL_EVENT_MOUSE_MOTION && !SDL_GetWindowRelativeMouseMode(win) &&
                e.motion.which != window::kInjectedDevice && !window::MouseFree())
                continue;
            window::HandleEvent(&e);
        }
        window::EndFrameInput();
        window::UpdateConsole();
        FinishHandover();
        // the display mode once the first frame is up (full screen only: a window is what there is), then when the
        // Display Mode setting itself changes (PC Options, F11). Not on other saves: a window the compositor made full
        // screen (Hyprland's fullscreen key) went back to its tile whenever PC Options were accepted
        // split-screen player 1: the window is the compositor's, which takes Display Mode and VSync from it
        if (split_player && splitp::Player() == 1 && harness::g_frames > 0 && pc::Version() != settings_seen) {
            settings_seen = pc::Version();
            std::string want = "display " + pc::Get("display_mode", "windowed") + " vsync " + pc::Get("vsync", "1");
            if (want != mode_applied) {
                mode_applied = want;
                csx::SplitSend(csx::kSplitSetup, want);
            }
        }
        if (!split_player && ((!display_applied && harness::g_frames > 0) || (display_applied && pc::Version() != settings_seen))) {
            settings_seen = pc::Version();
            std::string mode = pc::Get("display_mode", "windowed");
            if (display_applied ? mode != mode_applied : mode == "fullscreen") ApplyDisplaySettings(win);
            display_applied = true;
            mode_applied = mode;
            if (bool v = pc::Get("vsync", "1") != "0"; ren && v != vsync) {
                vsync = v;
                SDL_SetRenderVSync(ren, vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
                LOGI("window: VSync %s", vsync ? "on" : "off");
            }
        }
        if (g_vulkan) {
            // the backend presents; shot requests go to it, the loop only handles events
            const std::string shot_file = (app::ExeDir() / "shot").string();
            if (FILE* sf = request(shot_file)) {
                char name[64] = {};
                if (std::fscanf(sf, "%63s", name) == 1) window::RequestShot(name);
                std::fclose(sf);
                std::remove(shot_file.c_str());
            }
            // until the next event, 5 ms at most: a mouse's motion is read as it comes, not in 5 ms batches (at frame
            // rates past 200 a fixed sleep left some frames without the motion that came in and gave the next two
            // batches: the view turned unevenly)
            SDL_WaitEventTimeout(nullptr, 5);
            auto now = std::chrono::steady_clock::now();
            if (now - last_flush > std::chrono::seconds(1)) {
                logx::Flush();
                last_flush = now;
            }
            continue;
        }
        uint32_t w = 0, h = 0;
        {
            auto& fr = window::Latest();
            std::lock_guard<std::mutex> lock(fr.lock);
            if (fr.serial != shown) {
                shown = fr.serial;
                px = fr.pixels;
                w = fr.width;
                h = fr.height;
            }
        }
        if (!vsync && !w) {
            SDL_Delay(1);  // no new frame and no blank to wait for: the latest one is still shown
            continue;
        }
        if (w && h) {
            if (!tex || w != tw || h != th) {
                if (tex) SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, int(w), int(h));
                SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
                tw = w;
                th = h;
            }
            SDL_UpdateTexture(tex, nullptr, px.data(), int(w * 4));
        }
        int ww = 0, wh = 0;
        SDL_GetRenderOutputSize(ren, &ww, &wh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex) {
            SDL_FRect dst = Letterbox(ww, wh, window::Latest().aspect);
            SDL_RenderTexture(ren, tex, nullptr, &dst);
        }
        // a file named shot holding a name saves the window's output as shots/<name>.bmp, as WINDOW_SHOT_EVERY does
        std::string shot_name;
        if (shot_every > 0 && std::chrono::steady_clock::now() >= next_shot) {
            next_shot += std::chrono::seconds(shot_every);
            shot_name = "window_" + std::to_string(shots++);
        }
        const std::string shot_file = (app::ExeDir() / "shot").string();
        if (FILE* sf = std::fopen(shot_file.c_str(), "r")) {
            char name[64] = {};
            if (std::fscanf(sf, "%63s", name) == 1) shot_name = name;
            std::fclose(sf);
            std::remove(shot_file.c_str());
        }
        if (!shot_name.empty()) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(ren, nullptr)) {
                std::filesystem::create_directories(app::ExeDir() / "shots");
                std::string path = (app::ExeDir() / "shots" / (shot_name + ".bmp")).string();
                SDL_SaveBMP(shot, path.c_str());
                SDL_DestroySurface(shot);
                LOGI("window: shot %s", path.c_str());
            }
        }
        SDL_RenderPresent(ren);  // vsync paces this loop; the title runs on its own threads
        auto now = std::chrono::steady_clock::now();
        if (now - last_flush > std::chrono::seconds(1)) {
            logx::Flush();
            last_flush = now;
        }
    }
    LOGI("window: closed");
    kernel::Shutdown(0);
}
