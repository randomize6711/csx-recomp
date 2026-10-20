// PC keyboard and mouse as the engine's own keys (FINDINGS.md "PC key bindings"). The Xbox build keeps GoldSrc's key
// system: the pad's buttons reach Key_Event (0x66510) as keys GREEN..RBUTTON (203-222) through CEngine::TrapKey_Event
// (0xAEEE0) and run keybindings[] (0x6CABE0) like a PC's keys; the PC key names (SPACE, MOUSE1, MWHEELUP...) are in the
// same table. Host key events are queued here and handed to Key_Event on the game thread at the start of every
// Cbuf_Execute (0x3BB50), so the commands they bind run in that same pass. The PC keys' bindings (pc_bindings.h,
// pc_settings.ini) are kept in keybindings[] with the engine's own bind command, checked twice a second so a
// rebinding by the game (unbindall, a controller layout) is undone; hud_fastswitch 1 (set whenever play resumes)
// makes the weapon slot keys select at once, as on a PC (the pad's cycle_weapon does not use slots). Whether a menu
// screen is open (the UI's current screen 0x5523D4) is published for the window's input: menus keep their pad keys.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "app/app.h"
#include "common/csx.h"
#include "common/pc_bindings.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {

using kernel::R32;

constexpr uint32_t kKeyEvent = 0x66510;    // Key_Event(int key, qboolean down), cdecl
constexpr uint32_t kCbufAddText = 0x3B970;  // Cbuf_AddText, text in edi
constexpr uint32_t kKeyBindings = 0x6CABE0;  // char* keybindings[256]

constexpr uint32_t kCurrentScreen = 0x5523D4;  // the UI's open screen, 0 while playing

std::atomic<bool> g_menu{false};
std::mutex g_lock;
std::deque<std::pair<int, bool>> g_queue;
std::deque<std::string> g_commands;

std::string GuestString(uint32_t va) {
    std::string s;
    for (int i = 0; va && i < 256; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}

// Cbuf_AddText with the text on the guest stack below the caller's frame
void AddText(X86Context& ctx, const std::string& text) {
    uint32_t esp = ctx.esp, edi = ctx.edi;
    uint32_t buf = (esp - 0x200 - uint32_t(text.size())) & ~15u;
    std::memcpy(kernel::HostPtr(buf), text.c_str(), text.size() + 1);
    ctx.esp = buf - 16;
    ctx.edi = buf;
    kernel::CallGuest(ctx, kCbufAddText, {});
    ctx.esp = esp;
    ctx.edi = edi;
}

}  // namespace

namespace csx {

void QueueEngineKey(int key, bool down) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_queue.emplace_back(key, down);
}

void QueueEngineCommand(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_commands.push_back(text);
}

bool MenuOpen() { return g_menu; }

std::string EngineBinding(int key) {
    uint32_t p = R32(kKeyBindings + uint32_t(key) * 4);
    return p ? GuestString(p) : std::string();
}

}  // namespace csx

namespace {
// the PC bindings into keybindings[]: a key whose binding differs is bound again, a key this left behind (rebound to
// another or none) is unbound if it still holds what this set
void SyncBindings(X86Context& ctx) {
    static auto next = std::chrono::steady_clock::now();
    static unsigned seen = ~0u;
    static std::map<int, std::string> ours;  // key number -> command bound by this
    auto now = std::chrono::steady_clock::now();
    if (now < next && pc::Version() == seen) return;
    next = now + std::chrono::milliseconds(500);
    seen = pc::Version();
    std::map<int, std::pair<std::string, std::string>> want;  // key number -> name, command
    for (const pc::Action& a : pc::Actions())
        if (const pc::KeyInfo* k = pc::FindKey(pc::BoundKey(a)); k && k->number >= 0 && !a.pad)
            want[k->number] = {k->name, a.command};
    std::string text;
    for (auto& [num, nc] : want)
        if (csx::EngineBinding(num) != nc.second) text += "bind \"" + nc.first + "\" \"" + nc.second + "\"\n";
    for (auto& [num, cmd] : ours)
        if (!want.count(num) && csx::EngineBinding(num) == cmd)
            for (const pc::KeyInfo& k : pc::BindableKeys())
                if (k.number == num) text += std::string("unbind \"") + k.name + "\"\n";
    ours.clear();
    for (auto& [num, nc] : want) ours[num] = nc.second;
    if (!text.empty()) {
        AddText(ctx, text);
        LOGI("pc keys: bindings set (%zu keys)", want.size());
    }
}

// "pckey" request file in the run folder (tests): lines "cmd TEXT" (console text), "key KEYNUM DOWN", and for the
// developer console "con TEXT" (typed text) and "conkey NAME" (toggle close backspace enter up down tab pgup pgdn),
// for the menus' mouse "pointer X Y" (menu pixels), "mclick BUTTON", "mwheel N", for the name field "name TEXT"
// (typed text) and "namekey NAME" (backspace left right enter escape)
void PollRequests() {
    static auto next = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now < next) return;
    next = now + std::chrono::milliseconds(50);
    std::string path = (app::ExeDir() / "pckey").string();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("cmd ", 0) == 0) csx::QueueEngineCommand(l.substr(4));
        else if (l.rfind("key ", 0) == 0) {
            int key = 0, down = 0;
            if (std::sscanf(l.c_str() + 4, "%d %d", &key, &down) == 2) csx::QueueEngineKey(key, down != 0);
        } else if (l.rfind("pointer ", 0) == 0) {
            float x = 0, y = 0;
            if (std::sscanf(l.c_str() + 8, "%f %f", &x, &y) == 2) csx::MenuPointer(x, y);
        } else if (l.rfind("mclick ", 0) == 0) {
            csx::MenuClick(std::atoi(l.c_str() + 7));
        } else if (l.rfind("mwheel ", 0) == 0) {
            csx::MenuWheel(std::atoi(l.c_str() + 7));
        } else if (l.rfind("name ", 0) == 0) {
            csx::QueueTextEntry(l.substr(5));
        } else if (l.rfind("namekey ", 0) == 0) {
            using T = csx::TextKey;
            static const std::map<std::string, T> names = {{"backspace", T::Backspace}, {"left", T::Left},
                                                            {"right", T::Right},         {"enter", T::Enter},
                                                            {"escape", T::Escape}};
            if (auto it = names.find(l.substr(8)); it != names.end()) csx::QueueTextEntryKey(it->second);
        } else if (l.rfind("con ", 0) == 0) {
            csx::QueueConsoleText(l.substr(4));
        } else if (l.rfind("conkey ", 0) == 0) {
            using K = csx::ConsoleKey;
            static const std::map<std::string, K> names = {
                {"toggle", K::Toggle}, {"close", K::Close}, {"backspace", K::Backspace}, {"enter", K::Enter},
                {"up", K::HistoryBack}, {"down", K::HistoryForward}, {"tab", K::Complete}, {"pgup", K::PageUp},
                {"pgdn", K::PageDown}};
            if (auto it = names.find(l.substr(7)); it != names.end()) csx::QueueConsoleKey(it->second);
        }
    }
    std::fclose(f);
    std::remove(path.c_str());
}
}  // namespace

void PcConsoleFrame(X86Context& ctx);  // pc_console.cpp
void PcMouseFrame(X86Context& ctx);    // pc_mouse.cpp

// [[hook]] at 0x3BB50 (config/game.toml): Cbuf_Execute's entry, on the game thread every frame
void PcKeysFrame(X86Context& ctx, uint8_t*) {
    PollRequests();
    PcConsoleFrame(ctx);
    PcMouseFrame(ctx);
    uint32_t screen = R32(kCurrentScreen);
    if ((screen != 0) != g_menu) {
        g_menu = screen != 0;
        LOGI("pc keys: %s%s", screen ? "menu " : "playing", screen ? GuestString(R32(screen)).c_str() : "");
        // the client's cvar exists only once a match runs, and a map load may set it back: set on every return to play
        if (!screen) AddText(ctx, "hud_fastswitch 1\n");
    }
    SyncBindings(ctx);
    std::deque<std::pair<int, bool>> keys;
    std::deque<std::string> commands;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        keys.swap(g_queue);
        commands.swap(g_commands);
    }
    for (const std::string& c : commands) AddText(ctx, c + "\n");
    for (auto [key, down] : keys) kernel::CallGuest(ctx, kKeyEvent, {uint32_t(key), down ? 1u : 0u});
}
