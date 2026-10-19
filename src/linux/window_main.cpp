// Linux window front end: boots the image as the headless harness does (main_test.cpp), then runs an SDL3 window on
// the main thread that shows the latest presented frame at 4:3 and feeds SDL input to input::Poll.
//   <exe> <game folder> [--instance N]     run from the folder that gets logs and hdd (the game folder may come from
//                                         GAME_DIR, as after a title relaunch)
// FPS (default 30, the console's rate for CS) paces presented frames; F11 or Alt+Enter toggles fullscreen; a click
// captures the mouse, F1 or leaving the window releases it.
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "linux/window.h"

namespace {

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
        if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) setenv("INSTANCE", argv[++i], 1);
        else game = argv[i];
    }
    if (game.empty() && std::getenv("GAME_DIR")) game = std::getenv("GAME_DIR");
    setenv("GAME_DIR", game.c_str(), 1);
    setenv("FPS", "30", 0);
    if (game.empty()) {
        std::fprintf(stderr, "usage: %s <game folder> [--instance N]\n", argv[0]);
        return 2;
    }
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        LOGE("window: SDL_Init failed: %s", SDL_GetError());
        logx::Flush();
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("Counter-Strike", 1280, 960, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!ren) {
        LOGE("window: cannot create the window: %s", SDL_GetError());
        logx::Flush();
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    LOGI("window: SDL %d.%d.%d, renderer %s", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
         SDL_GetRendererName(ren));

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
        SDL_PushEvent(&ev);
    };
    const std::string keys_file = (app::ExeDir() / "keys").string();
    for (bool quit = false; !quit;) {
        auto tnow = std::chrono::steady_clock::now();
        if (FILE* kf = std::fopen(keys_file.c_str(), "r")) {
            char name[64] = {};
            double secs = 0.1;
            if (std::fscanf(kf, "%63s %lf", name, &secs) >= 1) {
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
            if (e.type == SDL_EVENT_MOUSE_MOTION && !SDL_GetWindowRelativeMouseMode(win)) continue;
            window::HandleEvent(&e);
        }
        window::EndFrameInput();
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
