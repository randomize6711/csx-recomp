// The game's server log lines in the host log, for tests: CS's game code reports rounds, kills and objectives with
// UTIL_LogPrintf (0x14BC80, cdecl (format, ...)), which writes them only when the title's log is on. The hook at its
// entry formats the line itself and logs it as "game: <line>", whatever the title's setting (FINDINGS.md "Rounds").
// The formats the game uses take %s, %d / %i, %c, %f and %%; anything else is copied as it is.
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
std::string GuestString(uint32_t va, size_t max = 512) {
    std::string s;
    for (size_t i = 0; va && i < max; i++) {
        char c = static_cast<const char*>(kernel::HostPtr(va))[i];
        if (!c) break;
        s += c;
    }
    return s;
}
}  // namespace

// [[hook]] at 0x14BC80 (config/game.toml): the stack holds the return address, the format, then the arguments
void LogGameLine(X86Context& ctx, uint8_t*) {
    std::string fmt = GuestString(kernel::R32(ctx.esp + 4), 1024), out;
    uint32_t arg = ctx.esp + 8;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out += fmt[i];
            continue;
        }
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0123456789.l", fmt[j])) j++;
        char conv = j < fmt.size() ? fmt[j] : 0;
        char buf[64];
        switch (conv) {
            case 's': out += GuestString(kernel::R32(arg)); arg += 4; break;
            case 'd':
            case 'i': std::snprintf(buf, sizeof buf, "%d", int32_t(kernel::R32(arg))); out += buf; arg += 4; break;
            case 'u': std::snprintf(buf, sizeof buf, "%u", kernel::R32(arg)); out += buf; arg += 4; break;
            case 'c': out += char(kernel::R32(arg)); arg += 4; break;
            case 'f': {
                uint64_t bits = kernel::R32(arg) | uint64_t(kernel::R32(arg + 4)) << 32;
                double d;
                std::memcpy(&d, &bits, 8);
                std::snprintf(buf, sizeof buf, "%.2f", d);
                out += buf;
                arg += 8;
                break;
            }
            case '%': out += '%'; break;
            default: out.append(fmt, i, j - i + 1); break;
        }
        i = j;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    LOGI("game: %s", out.c_str());
}
