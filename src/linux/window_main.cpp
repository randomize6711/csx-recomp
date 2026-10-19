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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
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
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: return true;
        default: return false;
    }
}

// the frame scaled to the window at the display's 4:3, whatever the surface's own size (640x960 for CS)
SDL_FRect Letterbox(int ww, int wh) {
    float w = float(ww), h = float(wh), tw = w, th = w * 3.0f / 4.0f;
    if (th > h) {
        th = h;
        tw = h * 4.0f / 3.0f;
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
    SDL_Window* win = SDL_CreateWindow("Counter-Strike", 1280, 960,
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
    // "mouse" request: "DX DY" counts of relative motion, as a captured mouse gives them (tests of the mouse look)
    const std::string mouse_file = (app::ExeDir() / "mouse").string();
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
        if (FILE* mf = std::fopen(mouse_file.c_str(), "r")) {
            float dx = 0, dy = 0;
            if (std::fscanf(mf, "%f %f", &dx, &dy) == 2) {
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
                push_key(held[i].key, false);
                held.erase(held.begin() + long(i));
            } else {
                i++;
            }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (test_input_only && HostInputEvent(e)) continue;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                bool alt_enter = e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT);
                if (e.key.key == SDLK_F11 || alt_enter) {
                    SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                    continue;
                }
                if (e.key.key == SDLK_F1) SDL_SetWindowRelativeMouseMode(win, !SDL_GetWindowRelativeMouseMode(win));
            }
            // the mouse is captured by a click in the window and let go with F1 or when the window loses focus
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !SDL_GetWindowRelativeMouseMode(win)) {
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
        FinishHandover();
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
            SDL_FRect dst = Letterbox(ww, wh);
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
