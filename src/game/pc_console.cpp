// The developer console (FINDINGS.md "Developer console"). The Xbox build keeps a console of its own, drawn over the
// view with the game's font: Con_Printf (0x40790) adds a line per call to a 200 line ring (0x744060, 78 characters a
// line; newest 0x55191C, shown from 0x551918), Con_Draw (0x40B20, called by SCR_UpdateScreen and the VGUI paint) shows
// ten lines from (30, 30), a ">>>" prompt and the input line (0x743FE0, length byte 0x551920, up to 99 characters) at
// y 240 while the flag 0x551910 is set; toggleconsole (0x40630) flips it, consoleup / consoledown (0x40920 / 0x40970)
// scroll a line, consoleexecute (0x40A40) adds the input line to the command buffer and clears it. GoldSrc's line
// editor (Key_Console), its history and completion were left out: nothing writes the input line. They are done here
// on that same line: typed text, Backspace, Enter (through consoleexecute, the command echoed as GoldSrc does), Up /
// Down through the commands entered, Tab completing a command or variable name from the engine's own lists, Page Up /
// Down scrolling, Escape closing. The window hands keys here while the console is open; Options > PC Controls >
// Toggle Developer Console binds the key that opens and closes it.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <strings.h>
#include <string>
#include <vector>

#include "common/csx.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {

using kernel::R32;
using kernel::W32;

constexpr uint32_t kConsoleShown = 0x551910;
constexpr uint32_t kInputLine = 0x743FE0, kInputLength = 0x551920;
constexpr int kInputMax = 98;  // consoleexecute appends the newline below 99
constexpr uint32_t kConsoleUp = 0x40920, kConsoleDown = 0x40970, kConsoleExecute = 0x40A40;
constexpr uint32_t kConPrintf = 0x40790;  // cdecl (format, ...)
constexpr uint32_t kConDraw = 0x40B20;
constexpr uint32_t kCurrentScreen = 0x5523D4;  // the UI's open screen, 0 while playing
constexpr uint32_t kCmdFunctions = 0x5518E4;  // cmd_function_t list: next +0, name +4
constexpr uint32_t kCvars = 0x434A20, kCvarCount = 0x551928;  // cvar_t* array (name at +0), count

std::atomic<bool> g_open{false};
std::mutex g_lock;
std::deque<std::pair<csx::ConsoleKey, std::string>> g_queue;
std::vector<std::string> g_history;
size_t g_history_at = 0;  // == size: the line being typed

std::string GuestString(uint32_t va) {
    std::string s;
    for (int i = 0; va && i < 256; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}

std::string Line() {
    int n = std::clamp<int>(int8_t(*static_cast<uint8_t*>(kernel::HostPtr(kInputLength))), 0, kInputMax);
    return std::string(static_cast<const char*>(kernel::HostPtr(kInputLine)), size_t(n));
}

void SetLine(const std::string& text) {
    std::string t = text.substr(0, kInputMax);
    std::memcpy(kernel::HostPtr(kInputLine), t.c_str(), t.size() + 1);
    *static_cast<uint8_t*>(kernel::HostPtr(kInputLength)) = uint8_t(t.size());
}

// Con_Printf("%s\n", text) with the strings on the guest stack below the caller's frame
void Print(X86Context& ctx, const std::string& text) {
    uint32_t esp = ctx.esp;
    uint32_t str = (esp - 0x400 - uint32_t(text.size())) & ~15u, fmt = str - 16;
    std::memcpy(kernel::HostPtr(str), text.c_str(), text.size() + 1);
    std::memcpy(kernel::HostPtr(fmt), "%s\n", 4);
    ctx.esp = fmt - 16;
    kernel::CallGuest(ctx, kConPrintf, {fmt, str});
    ctx.esp = esp;
}

// the engine's command and variable names starting with a prefix (case blind, as Cmd_ExecuteString matches)
std::vector<std::string> Names(const std::string& prefix) {
    std::vector<std::string> out;
    auto take = [&](const std::string& n) {
        if (n.size() >= prefix.size() && strncasecmp(n.c_str(), prefix.c_str(), prefix.size()) == 0) out.push_back(n);
    };
    int guard = 0;
    for (uint32_t c = R32(kCmdFunctions); c && guard < 4096; c = R32(c), guard++) take(GuestString(R32(c + 4)));
    uint32_t count = std::min<uint32_t>(R32(kCvarCount), 4096);
    for (uint32_t i = 0; i < count; i++)
        if (uint32_t v = R32(kCvars + i * 4)) take(GuestString(R32(v)));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Complete(X86Context& ctx) {
    std::string line = Line();
    if (line.empty() || line.find(' ') != std::string::npos) return;
    std::vector<std::string> names = Names(line);
    if (names.empty()) return;
    if (names.size() == 1) {
        SetLine(names[0] + " ");
        return;
    }
    // the longest common prefix, then the candidates listed
    std::string common = names[0];
    for (const std::string& n : names) {
        size_t i = 0;
        while (i < common.size() && i < n.size() && std::tolower(common[i]) == std::tolower(n[i])) i++;
        common.resize(i);
    }
    if (common.size() > line.size()) SetLine(common);
    Print(ctx, "]" + line);
    for (size_t i = 0; i < names.size() && i < 24; i++) Print(ctx, "  " + names[i]);
    if (names.size() > 24) Print(ctx, "  ... " + std::to_string(names.size() - 24) + " more");
}

void Execute(X86Context& ctx) {
    std::string line = Line();
    Print(ctx, "]" + line);
    if (!line.empty() && (g_history.empty() || g_history.back() != line)) g_history.push_back(line);
    if (g_history.size() > 64) g_history.erase(g_history.begin());
    g_history_at = g_history.size();
    LOGI("pc console: ] %s", line.c_str());
    kernel::CallGuest(ctx, kConsoleExecute, {});
}

void Recall(int step) {
    if (g_history.empty()) return;
    long at = std::clamp<long>(long(g_history_at) + step, 0, long(g_history.size()));
    g_history_at = size_t(at);
    SetLine(g_history_at < g_history.size() ? g_history[g_history_at] : std::string());
}

void SetShown(bool shown) {
    W32(kConsoleShown, shown ? 1 : 0);
}

}  // namespace

namespace csx {

void QueueConsoleKey(ConsoleKey k) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_queue.emplace_back(k, std::string());
}

void QueueConsoleText(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_queue.emplace_back(ConsoleKey::Text, text);
}

bool ConsoleOpen() { return g_open; }

}  // namespace csx

// called by PcKeysFrame (Cbuf_Execute's entry, the game thread) before the engine keys, so a command entered here
// runs in this same pass
void PcConsoleFrame(X86Context& ctx) {
    std::deque<std::pair<csx::ConsoleKey, std::string>> queue;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        queue.swap(g_queue);
    }
    using K = csx::ConsoleKey;
    for (auto& [k, text] : queue) {
        bool shown = R32(kConsoleShown) != 0;
        if (k == K::Toggle) {
            SetShown(!shown);
            continue;
        }
        if (!shown) continue;
        switch (k) {
            case K::Close: SetShown(false); break;
            case K::Text: {
                std::string line = Line();
                for (char c : text)
                    if (c >= 32 && c < 127 && int(line.size()) < kInputMax) line += c;
                SetLine(line);
                break;
            }
            case K::Backspace: {
                std::string line = Line();
                if (!line.empty()) line.pop_back();
                SetLine(line);
                break;
            }
            case K::Enter: Execute(ctx); break;
            case K::HistoryBack: Recall(-1); break;
            case K::HistoryForward: Recall(1); break;
            case K::Complete: Complete(ctx); break;
            case K::PageUp:
                for (int i = 0; i < 3; i++) kernel::CallGuest(ctx, kConsoleUp, {});
                break;
            case K::PageDown:
                for (int i = 0; i < 3; i++) kernel::CallGuest(ctx, kConsoleDown, {});
                break;
            default: break;
        }
    }
    bool shown = R32(kConsoleShown) != 0;
    if (shown != g_open) {
        g_open = shown;
        if (shown) g_history_at = g_history.size();
        LOGI("pc console: %s", shown ? "open" : "closed");
    }
}

// [[hook]] at 0x55F99 (SCR_UpdateScreen, after the game's menus are drawn by 0xCD7D0): SCR_UpdateScreen draws the
// console (0x55F87) before the menus, which cover it with their backdrop; with a menu open the console's own Con_Draw
// (0x40B20) runs once more on top, so it can be read and typed into from the pause menu and the front end
void PcConsoleOverMenus(X86Context& ctx, uint8_t*) {
    if (R32(kConsoleShown) && R32(kCurrentScreen)) kernel::CallGuest(ctx, kConDraw, {});
}
