// csx-server-tool: the Counter-Strike Xbox dedicated server tool (docs/server_tool.md).
//   csx-server-tool [--data DIR] [--minimized]
//   csx-server-tool --script FILE [--data DIR]          no window: runs a script against the real server (tests)
//   csx-server-tool --screenshot FILE.bmp [--page NAME] [--size WxH] [--frames N] [--data DIR]
// One window (SDL3 + SDL_Renderer + Dear ImGui), a tray icon where the system has one. The server is a child process
// (controller.h); closing the tool stops it (after asking).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "script.h"
#include "theme.h"
#include "ui.h"

namespace fs = std::filesystem;

namespace {

// a system font close to the era's (Tahoma / Verdana on Windows; a plain sans on Linux), else ImGui's own
ImFont* LoadFonts(float px, ImFont** mono) {
    ImGuiIO& io = ImGui::GetIO();
    const char* sans[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\tahoma.ttf", "C:\\Windows\\Fonts\\verdana.ttf", "C:\\Windows\\Fonts\\segoeui.ttf",
#else
        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
#endif
    };
    const char* monos[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\lucon.ttf", "C:\\Windows\\Fonts\\cour.ttf",
#else
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationMono-Regular.ttf", "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
#endif
    };
    std::error_code ec;
    ImFont* main = nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    for (const char* f : sans)
        if (fs::exists(f, ec) && (main = io.Fonts->AddFontFromFileTTF(f, px, &cfg))) break;
    if (!main) {
        ImFontConfig d;
        d.SizePixels = std::max(13.0f, px);
        main = io.Fonts->AddFontDefault(&d);
    }
    *mono = nullptr;
    for (const char* f : monos)
        if (fs::exists(f, ec) && (*mono = io.Fonts->AddFontFromFileTTF(f, px, &cfg))) break;
    if (!*mono) {
        ImFontConfig d;
        d.SizePixels = std::max(13.0f, px);
        *mono = io.Fonts->AddFontDefault(&d);  // ProggyClean: a monospaced pixel font
    }
    return main;
}

// the tray icon: a small olive square with a gold frame, drawn here (no image file)
SDL_Surface* MakeIcon() {
    const int n = 32;
    SDL_Surface* s = SDL_CreateSurface(n, n, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    auto* px = static_cast<uint32_t*>(s->pixels);
    const SDL_PixelFormatDetails* f = SDL_GetPixelFormatDetails(s->format);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            bool edge = x < 2 || y < 2 || x >= n - 2 || y >= n - 2;
            bool bar = (y >= 9 && y < 13 && x >= 7 && x < 25) || (y >= 19 && y < 23 && x >= 7 && x < 25) ||
                       (x >= 7 && x < 11 && y >= 9 && y < 23);
            Uint8 r = edge ? 196 : bar ? 222 : 74, g = edge ? 181 : bar ? 228 : 86, b = edge ? 80 : bar ? 214 : 66;
            px[y * (s->pitch / 4) + x] = SDL_MapRGBA(f, nullptr, r, g, b, 255);
        }
    return s;
}

struct Tray {
    SDL_Tray* tray = nullptr;
    SDL_TrayEntry *show = nullptr, *startstop = nullptr, *status = nullptr, *quit = nullptr;
};

std::string ExeDir() {
    const char* b = SDL_GetBasePath();
    return b ? std::string(b) : fs::current_path().string();
}

void Usage() {
    std::printf("csx-server-tool: the Counter-Strike Xbox dedicated server tool\n"
                "  csx-server-tool [--data DIR] [--minimized]\n"
                "  csx-server-tool --script FILE [--data DIR]   (no window: a test script against the real server)\n"
                "  csx-server-tool --screenshot FILE.bmp [--page NAME] [--tab N] [--size WxH] [--frames N] [--data DIR]\n"
                "                  [--start-server --wait SECONDS]   (the profile's server started, captured after SECONDS)\n"
                "Data (profiles, settings): csx-server-tool-data/ beside the program when it is writable, else the\n"
                "per-user folder; --data chooses another.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string data, script, shot, page_name;
    int shot_w = 1280, shot_h = 800, shot_frames = 6, tab = 0;
    bool minimized = false, start_server = false;
    double shot_wait = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--data") data = next();
        else if (a == "--script") script = next();
        else if (a == "--screenshot") shot = next();
        else if (a == "--page") page_name = next();
        else if (a == "--tab") tab = std::atoi(next().c_str());
        else if (a == "--frames") shot_frames = std::max(2, std::atoi(next().c_str()));
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &shot_w, &shot_h);
        else if (a == "--minimized") minimized = true;
        else if (a == "--start-server") start_server = true;
        else if (a == "--wait") shot_wait = std::atof(next().c_str());
        else if (a == "-h" || a == "--help") return Usage(), 0;
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), Usage(), 2;
    }

    if (!script.empty()) {
        if (!SDL_Init(0)) return std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()), 1;
        char* pref = SDL_GetPrefPath("csx-recomp", "csx-server-tool");
        tool::Paths paths = tool::ResolvePaths(ExeDir(), pref ? pref : ".", data);
        SDL_free(pref);
        int rc = tool::RunScript(script, paths, ExeDir());
        SDL_Quit();
        return rc;
    }

    if (!shot.empty()) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()), 1;
    char* pref = SDL_GetPrefPath("csx-recomp", "csx-server-tool");
    tool::App app;
    app.ctl.paths = tool::ResolvePaths(ExeDir(), pref ? pref : ".", data);
    SDL_free(pref);
    app.settings_path = (fs::path(app.ctl.paths.data) / "tool.ini").string();
    app.settings.Load(app.settings_path);

    int w = shot.empty() ? app.settings.GetInt("window_w", 1180) : shot_w;
    int h = shot.empty() ? app.settings.GetInt("window_h", 780) : shot_h;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (shot.empty() && app.settings.GetBool("window_maximized", false)) flags |= SDL_WINDOW_MAXIMIZED;
    SDL_Window* win = SDL_CreateWindow("Counter-Strike Xbox Dedicated Server", w, h, flags);
    if (!win) return std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()), 1;
    SDL_SetWindowMinimumSize(win, 900, 600);
    if (shot.empty() && app.settings.Get("window_x") != "")
        SDL_SetWindowPosition(win, app.settings.GetInt("window_x", SDL_WINDOWPOS_CENTERED), app.settings.GetInt("window_y", SDL_WINDOWPOS_CENTERED));
    if (SDL_Surface* icon = MakeIcon()) SDL_SetWindowIcon(win, icon), SDL_DestroySurface(icon);
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    if (!ren) return std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()), 1;
    SDL_SetRenderVSync(ren, 1);
    app.window = win;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the tool keeps its own settings (tool.ini)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float scale = shot.empty() ? SDL_GetWindowDisplayScale(win) : 1.0f;
    if (scale <= 0) scale = 1;
    ImFont* mono = nullptr;
    LoadFonts(std::round(13.0f * scale), &mono);
    app.mono = mono;
    theme::Apply(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    app.Init();
    if (!page_name.empty())
        for (int p = 0; p < int(tool::Page::Count); p++)
            if (SDL_strcasecmp(page_name.c_str(), tool::PageName(tool::Page(p))) == 0) app.SetPage(tool::Page(p));
    if (!shot.empty()) app.SetConfigTab(tab);
    if (start_server) app.ctl.Start();
    const Uint64 shot_start = SDL_GetTicks();

    Tray tray;
    if (shot.empty()) {
        SDL_Surface* icon = MakeIcon();
        tray.tray = SDL_CreateTray(icon, "Counter-Strike Xbox Dedicated Server");
        if (icon) SDL_DestroySurface(icon);
        if (tray.tray) {
            SDL_TrayMenu* m = SDL_CreateTrayMenu(tray.tray);
            tray.status = SDL_InsertTrayEntryAt(m, -1, "Offline", SDL_TRAYENTRY_BUTTON | SDL_TRAYENTRY_DISABLED);
            tray.show = SDL_InsertTrayEntryAt(m, -1, "Show Window", SDL_TRAYENTRY_BUTTON);
            tray.startstop = SDL_InsertTrayEntryAt(m, -1, "Start Server", SDL_TRAYENTRY_BUTTON);
            SDL_InsertTrayEntryAt(m, -1, nullptr, 0);
            tray.quit = SDL_InsertTrayEntryAt(m, -1, "Exit", SDL_TRAYENTRY_BUTTON);
            SDL_SetTrayEntryCallback(tray.show, [](void* w, SDL_TrayEntry*) {
                SDL_ShowWindow(static_cast<SDL_Window*>(w));
                SDL_RestoreWindow(static_cast<SDL_Window*>(w));
                SDL_RaiseWindow(static_cast<SDL_Window*>(w));
            }, win);
            SDL_SetTrayEntryCallback(tray.startstop, [](void* a, SDL_TrayEntry*) { static_cast<tool::App*>(a)->TrayStartStop(); }, &app);
            SDL_SetTrayEntryCallback(tray.quit, [](void* a, SDL_TrayEntry*) {
                auto* app = static_cast<tool::App*>(a);
                SDL_ShowWindow(app->window);
                if (app->RequestClose()) app->quit_requested = true;
            }, &app);
            app.tray_available = true;
        }
    }
    bool hidden = false;
    if (minimized || (shot.empty() && app.settings.GetBool("start_minimized", false))) {
        if (app.tray_available) hidden = true;
        else SDL_ShowWindow(win), SDL_MinimizeWindow(win);
    }
    if (!hidden) SDL_ShowWindow(win);

    int frame = 0;
    std::string tray_text;
    bool tray_running = false;
    while (!app.quit_requested) {
        // a hidden or minimized window still runs the controller (the server's output, crash handling), more slowly
        bool idle = hidden || (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED);
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, idle ? 250 : 33);
        while (got) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win))) {
                if (app.tray_available && app.settings.GetBool("minimize_to_tray", false)) app.hide_to_tray = true;
                else if (app.RequestClose()) app.quit_requested = true;
                else SDL_ShowWindow(win), hidden = false;
            }
            got = SDL_PollEvent(&e);
        }
        if (app.hide_to_tray) {
            app.hide_to_tray = false;
            SDL_HideWindow(win);
            hidden = true;
        }
        if (hidden && (SDL_GetWindowFlags(win) & SDL_WINDOW_HIDDEN) == 0) hidden = false;  // shown from the tray
        if (tray.tray) {
            std::string t = app.TrayStatus();
            if (t != tray_text) {
                tray_text = t;
                SDL_SetTrayEntryLabel(tray.status, t.c_str());
                SDL_SetTrayTooltip(tray.tray, t.c_str());
            }
            if (app.ctl.Running() != tray_running || frame == 0) {
                tray_running = app.ctl.Running();
                SDL_SetTrayEntryLabel(tray.startstop, tray_running ? "Stop Server" : "Start Server");
            }
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.Frame();
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 74, 86, 66, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (!shot.empty() && ++frame >= shot_frames && double(SDL_GetTicks() - shot_start) / 1000.0 >= shot_wait) {
            SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr);
            bool ok = s && SDL_SaveBMP(s, shot.c_str());
            if (s) SDL_DestroySurface(s);
            std::printf("%s %s\n", ok ? "screenshot" : "screenshot failed:", ok ? shot.c_str() : SDL_GetError());
            break;
        }
        SDL_RenderPresent(ren);
        if (shot.empty()) frame++;
    }

    if (shot.empty()) {
        SDL_WindowFlags f = SDL_GetWindowFlags(win);
        app.settings.SetBool("window_maximized", (f & SDL_WINDOW_MAXIMIZED) != 0);
        if (!(f & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) {
            int x, y, ww, hh;
            SDL_GetWindowPosition(win, &x, &y);
            SDL_GetWindowSize(win, &ww, &hh);
            app.settings.SetInt("window_x", x), app.settings.SetInt("window_y", y);
            app.settings.SetInt("window_w", ww), app.settings.SetInt("window_h", hh);
        }
        app.Shutdown();
    }
    // a running server is stopped (the exit dialog said so); the controller's process waits for it
    if (app.ctl.Running()) {
        app.ctl.Stop();
        for (int i = 0; i < 600 && app.ctl.Running(); i++) {
            app.ctl.Update();
            SDL_Delay(25);
        }
    }
    if (tray.tray) SDL_DestroyTray(tray.tray);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
