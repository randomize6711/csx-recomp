// Linux window front end: boots the image as the headless harness does (main_test.cpp), then runs an SDL3 window on
// the main thread that shows the latest presented frame at 4:3 and feeds SDL input to input::Poll.
//   <exe> <game folder> [--instance N]     run from the folder that gets logs and hdd (the game folder may come from
//                                         GAME_DIR, as after a title relaunch)
// FPS (default 30, the console's rate for CS) paces presented frames; F11 or Alt+Enter toggles fullscreen; a click
// captures the mouse, F1 or leaving the window releases it.
#include <SDL3/SDL.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "harness.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "linux/window.h"

namespace {

SDL_Window* g_window = nullptr;
bool g_vulkan = false;
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
// compositor (Hyprland) leaves the window tiled.
bool WantFullscreen() { return pc::Get("display_mode", "windowed") == "fullscreen"; }
void ApplyDisplaySettings(SDL_Window* win) {
    bool full = WantFullscreen();
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
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
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
    SDL_Window* win = SDL_CreateWindow("Counter-Strike", win_w, win_h,
                                       SDL_WINDOW_RESIZABLE | (g_vulkan ? SDL_WINDOW_VULKAN : SDL_WindowFlags(0)));
    g_window = win;
    // the Vulkan backend presents by itself; the software path shows frames through an SDL renderer
    SDL_Renderer* ren = win && !g_vulkan ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!win || (!g_vulkan && !ren)) {
        LOGE("window: cannot create the window: %s", SDL_GetError());
        logx::Flush();
        return 1;
    }
    if (ren) SDL_SetRenderVSync(ren, 1);
    unsigned settings_seen = pc::Version();
    bool display_applied = false;
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
    // a file named keys in the run folder ("KEYNAME SECONDS", SDL key names) presses that key through SDL's own event
    // queue, so tests drive the window's input path as a keyboard would
    struct Held {
        SDL_Keycode key;
        std::chrono::steady_clock::time_point until;
        uint8_t button = 0;  // a mouse button instead of a key
    };
    std::vector<Held> held;
    auto push_key = [&](SDL_Keycode k, bool down) {
        SDL_Event ev{};
        ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        ev.key.key = k;
        ev.key.down = down;
        ev.key.windowID = SDL_GetWindowID(win);
        ev.key.which = kInjectedKeyboard;
        SDL_PushEvent(&ev);
    };
    const std::string keys_file = (app::ExeDir() / "keys").string();
    // "mouse" request: "DX DY" counts of relative motion, as a captured mouse gives them (tests of the mouse look),
    // "button N SECONDS" a mouse button (SDL numbering: 1 left, 2 middle, 3 right) held, "wheel N" wheel notches
    auto push_button = [&](uint8_t b, bool down) {
        SDL_Event ev{};
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
    std::deque<std::string> typed;  // SDL keeps the pointer until the event is read
    const bool test_input_only = std::getenv("CSX_TEST_INPUT_ONLY") && *std::getenv("CSX_TEST_INPUT_ONLY") == '1';
    if (test_input_only) LOGI("window: test input only, host keyboard, mouse and gamepads ignored");
    for (bool quit = false; !quit;) {
        auto tnow = std::chrono::steady_clock::now();
        if (FILE* kf = std::fopen(keys_file.c_str(), "r")) {
            char name[64] = {};
            double secs = 0.1;
            if (std::fscanf(kf, "%63s %lf", name, &secs) >= 1) {
                // SDL key names with spaces ("Left Ctrl") are written with underscores in the request file
                for (char* c = name; *c; c++)
                    if (*c == '_') *c = ' ';
                SDL_Keycode k = SDL_GetKeyFromName(name);
                if (k != SDLK_UNKNOWN) {
                    push_key(k, true);
                    held.push_back({k, tnow + std::chrono::milliseconds(long(secs * 1000))});
                } else {
                    LOGW("window: unknown key %s", name);
                }
            }
            std::fclose(kf);
            std::remove(keys_file.c_str());
        }
        if (FILE* tf = std::fopen(text_file.c_str(), "r")) {
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
        if (FILE* mf = std::fopen(mouse_file.c_str(), "r")) {
            float dx = 0, dy = 0;
            char word[16] = {};
            double secs = 0.1;
            int n = 0;
            long at = std::ftell(mf);
            if (std::fscanf(mf, "button %d %lf", &n, &secs) >= 1 && n > 0) {
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
                else push_key(held[i].key, false);
                held.erase(held.begin() + long(i));
            } else {
                i++;
            }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
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
                if (e.key.key == SDLK_F1) SDL_SetWindowRelativeMouseMode(win, !SDL_GetWindowRelativeMouseMode(win));
            }
            // the mouse is captured by a click in the window and let go with F1 or when the window loses focus
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !SDL_GetWindowRelativeMouseMode(win) &&
                e.button.which != window::kInjectedDevice && !window::ConsoleShown()) {
                SDL_SetWindowRelativeMouseMode(win, true);
                continue;
            }
            if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) SDL_SetWindowRelativeMouseMode(win, false);
            if (e.type == SDL_EVENT_MOUSE_MOTION && !SDL_GetWindowRelativeMouseMode(win) &&
                e.motion.which != window::kInjectedDevice)
                continue;
            window::HandleEvent(&e);
        }
        window::EndFrameInput();
        window::UpdateConsole();
        FinishHandover();
        // the display mode once the first frame is up, then whenever PC Options change (the game thread saves them)
        if ((!display_applied && harness::g_frames > 0) || (display_applied && pc::Version() != settings_seen)) {
            display_applied = true;
            settings_seen = pc::Version();
            ApplyDisplaySettings(win);
        }
        if (g_vulkan) {
            // the backend presents; shot requests go to it, the loop only handles events
            const std::string shot_file = (app::ExeDir() / "shot").string();
            if (FILE* sf = std::fopen(shot_file.c_str(), "r")) {
                char name[64] = {};
                if (std::fscanf(sf, "%63s", name) == 1) window::RequestShot(name);
                std::fclose(sf);
                std::remove(shot_file.c_str());
            }
            SDL_Delay(5);
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
