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

// a test's tap of a held command (pckey "tap +CMD SECONDS"; FINDINGS.md "Buy menu taps"): +CMD at the start of a
// Cbuf_Execute, -CMD at the first one SECONDS later (steady clock, the title's hold timers read QueryPerformanceCounter),
// so the hold is what the game sees and no request polling is in it. The press waits (at most a second) for the
// frame after the HUD's settings check (CHud::Think 0x1AE680, gHUD 0x5623D0: every 0.5 s of client time from
// m_flTime 0x562438 to the next check at 0x563F9C) ClientCmds "m_pitch 0.022\n" and the like: the server's
// SrtBuyMenu reply to the buy menu's tap Cbuf_AddTexts "xbui buymenu" without a line end, so a check in the same frame
// makes it "xbui buymenum_pitch 0.022" and the menu does not open (the title's own race, as on the console); a hold
// under 0.5 s started right after a check ends before the next
struct Tap {
    std::string cmd;  // +CMD
    double hold = 0;
    int stage = 0;    // 1 waiting for the check, 2 held
    float next = 0;   // the next check's time last frame
    std::chrono::steady_clock::time_point asked, down;
} g_tap;
constexpr uint32_t kHudTime = 0x562438, kHudNextCheck = 0x563F9C;

float GuestFloat(uint32_t va) {
    uint32_t v = R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void TapFrame(X86Context& ctx) {
    if (!g_tap.stage) return;
    auto now = std::chrono::steady_clock::now();
    if (g_tap.stage == 1) {
        float next = GuestFloat(kHudNextCheck);
        bool checked = next != g_tap.next && next > GuestFloat(kHudTime);
        g_tap.next = next;
        bool late = now - g_tap.asked > std::chrono::seconds(1);
        if (!checked && !late) return;
        if (late) LOGW("pc keys: tap %s without the HUD's settings check (none for a second)", g_tap.cmd.c_str());
        AddText(ctx, g_tap.cmd + "\n");
        g_tap.down = now;
        g_tap.stage = 2;
        return;
    }
    double held = std::chrono::duration<double>(now - g_tap.down).count();
    if (held < g_tap.hold) return;
    AddText(ctx, "-" + g_tap.cmd.substr(1) + "\n");
    g_tap.stage = 0;
    LOGI("pc keys: tap %s held %.3f s", g_tap.cmd.c_str(), held);
}

// "pckey" request file in the run folder (tests): lines "cmd TEXT" (console text), "key KEYNUM DOWN", and for the
// developer console "con TEXT" (typed text) and "conkey NAME" (toggle close backspace enter up down tab pgup pgdn),
// for the menus' mouse "pointer X Y" (menu pixels), "mclick BUTTON", "mwheel N", for PC Controls' key capture
// "bindstart" (Enter on a row), "bindkey KEYNUM" (the key the window would send), "bindcancel" (Escape), for the
// name field "name TEXT" (typed text) and "namekey NAME" (backspace left right enter escape), "set NAME VALUE" a PC
// setting changed and saved as PC Options' Accept does (tests of settings applied while playing), "tap +CMD SECONDS"
// (TapFrame)
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
        else if (l.rfind("tap +", 0) == 0) {
            size_t sp = l.find(' ', 4);
            if (sp != std::string::npos && !g_tap.stage) {
                g_tap.cmd = l.substr(4, sp - 4);
                g_tap.hold = std::atof(l.c_str() + sp + 1);
                g_tap.stage = 1;
                g_tap.next = GuestFloat(kHudNextCheck);
                g_tap.asked = std::chrono::steady_clock::now();
            }
        } else if (l.rfind("key ", 0) == 0) {
            int key = 0, down = 0;
            if (std::sscanf(l.c_str() + 4, "%d %d", &key, &down) == 2) csx::QueueEngineKey(key, down != 0);
        } else if (l.rfind("pointer ", 0) == 0) {
            float x = 0, y = 0;
            if (std::sscanf(l.c_str() + 8, "%f %f", &x, &y) == 2) csx::MenuPointer(x, y);
        } else if (l.rfind("mclick ", 0) == 0) {
            csx::MenuClick(std::atoi(l.c_str() + 7));
        } else if (l.rfind("mwheel ", 0) == 0) {
            csx::MenuWheel(std::atoi(l.c_str() + 7));
        } else if (l == "bindstart") {
            csx::BindStart();
        } else if (l.rfind("bindkey ", 0) == 0) {
            csx::BindInput(std::atoi(l.c_str() + 8));
        } else if (l == "bindcancel") {
            csx::BindInput(-1);
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
        } else if (l.rfind("set ", 0) == 0) {
            size_t sp = l.find(' ', 4);
            if (sp != std::string::npos) {
                pc::Set(l.substr(4, sp - 4), l.substr(sp + 1));
                pc::Save();
            }
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

// [[hook]] at 0x1AC313 (config/game.toml): the client's SelectSlot (0x1AC1A0; slot1..slot5, and invnext / invprev,
// which pick the next weapon's slot through it) storing its choice as the pending selection (gpActiveSel 0x548730,
// esi the weapon, bl hud_fastswitch). With fast switch it selects at once only a slot holding one item; with two or
// three grenades it leaves the choice pending for a confirmation this build no longer has (the HUD's draw 0x1ACC70
// only times it out after a second; the pad selects with cycle_grenade / cycle_weapon instead), so 4 and the wheel
// never reached a grenade. With fast switch the pending choice is selected at once as the one-item path does
// (ServerCmd with its name, g_weaponselect 0x552C78 and its timer 0x552C7C); pressing the key again steps on from it.
void PcWeaponFastSelect(X86Context& ctx, uint8_t*) {
    constexpr uint32_t kServerCmd = 0x543134, kWeaponSelect = 0x552C78, kSelectTimer = 0x552C7C;
    uint32_t weapon = ctx.esi;
    if (!(ctx.ebx & 0xFF) || weapon <= 1 || R32(kWeaponSelect) == R32(weapon + 0xA0)) return;
    kernel::CallGuest(ctx, R32(kServerCmd), {weapon});
    kernel::W32(kWeaponSelect, R32(weapon + 0xA0));
    kernel::W32(kSelectTimer, 0x3F800000);  // 1.0
}

// [[hook]] at 0x1AC1A0, SelectSlot's entry (eax the slot): a slot key pressed while nothing is pending and the weapon
// in hand (gHUD.m_Ammo 0x5624D4 + 0x1C) is in that slot steps on from it, as invnext does by making it the pending
// choice first (0x1AC996): 4 again goes from the HE grenade to the flashbang, then the smoke grenade
void PcWeaponSlotFromHeld(X86Context& ctx, uint8_t*) {
    constexpr uint32_t kActiveSel = 0x548730, kHeld = 0x5624D4 + 0x1C;
    uint32_t held = R32(kHeld);
    if (R32(kActiveSel) <= 1 && held > 1 && R32(held + 0x90) == ctx.eax) kernel::W32(kActiveSel, held);
}

bool PcScreenPassThrough(const std::string& name);  // pc_mouse.cpp
void PcConsoleFrame(X86Context& ctx);  // pc_console.cpp
void PcMouseFrame(X86Context& ctx);    // pc_mouse.cpp
void PcMenuFrame(X86Context& ctx);     // pc_menu.cpp
void PcFrameRateFrame(X86Context& ctx);  // pc_frame.cpp
void PcAutoExposureFrame(X86Context& ctx);  // dynamic_gamma.cpp
void PcBloomFrame(X86Context& ctx);         // pc_bloom.cpp

// [[hook]] at 0x3BB50 (config/game.toml): Cbuf_Execute's entry, on the game thread every frame
void PcKeysFrame(X86Context& ctx, uint8_t*) {
    PollRequests();
    PcConsoleFrame(ctx);
    PcMouseFrame(ctx);
    PcMenuFrame(ctx);
    PcFrameRateFrame(ctx);
    PcAutoExposureFrame(ctx);
    PcBloomFrame(ctx);
    // a menu takes the keyboard and mouse as the pad; a screen that passes the controls through (Spectator, the
    // Scoreboard: ControlPassThrough, the right stick turns the spectator's camera) leaves them the game's, as playing
    uint32_t screen = R32(kCurrentScreen);
    std::string name = screen ? GuestString(R32(screen)) : std::string();
    bool menu = screen && !PcScreenPassThrough(name);
    if (menu != g_menu) {
        g_menu = menu;
        LOGI("pc keys: %s%s", menu ? "menu " : "playing", menu ? name.c_str() : "");
        // the client's cvar exists only once a match runs, and a map load may set it back: set on every return to play
        if (!menu) AddText(ctx, "hud_fastswitch 1\n");
    }
    static uint32_t through = 0;  // the pass-through screen last logged
    if (uint32_t now = menu ? 0 : screen; now != through) {
        through = now;
        if (now) LOGI("pc keys: %s passes the controls through", name.c_str());
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
    TapFrame(ctx);
    for (auto [key, down] : keys) kernel::CallGuest(ctx, kKeyEvent, {uint32_t(key), down ? 1u : 0u});
}
