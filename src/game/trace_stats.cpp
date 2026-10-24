// CSX_LOG_TRACES=1 (FINDINGS.md "High population performance", a measurement): the engine's TraceLine (pfnTraceLine
// 0x76DE0, the game DLL's line traces: start, end, no-monsters, the entity to skip, the result) counted once a second
// by its caller (the return address), and how many repeat an earlier call's arguments exactly with no entity linked
// in between (SV_LinkEdict 0xB79C0 is where an entity's position or size takes effect for traces): those would get
// the same result again.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_set>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
bool On() {
    static const bool on = std::getenv("CSX_LOG_TRACES") && *std::getenv("CSX_LOG_TRACES") == '1';
    return on;
}

struct Key {
    uint32_t v[8];
    bool operator==(const Key& o) const { return !std::memcmp(v, o.v, sizeof v); }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        size_t h = 1469598103934665603ull;
        for (uint32_t x : k.v) h = (h ^ x) * 1099511628211ull;
        return h;
    }
};

std::unordered_set<Key, KeyHash> g_seen;  // since the last link
std::map<uint32_t, uint64_t> g_callers;
uint64_t g_calls = 0, g_repeats = 0, g_links = 0;
auto g_second = std::chrono::steady_clock::now();
}  // namespace

// [[hook]] at 0x76DE0 (pfnTraceLine's entry: [esp] the return address, +4 start, +8 end, +0xC no-monsters, +0x10 the
// entity to skip)
void TraceLineCount(X86Context& ctx, uint8_t*) {
    if (!On()) return;
    using kernel::R32;
    uint32_t a = R32(ctx.esp + 4), b = R32(ctx.esp + 8);
    Key k{{R32(a), R32(a + 4), R32(a + 8), R32(b), R32(b + 4), R32(b + 8), R32(ctx.esp + 0xC), R32(ctx.esp + 0x10)}};
    g_calls++;
    if (!g_seen.insert(k).second) g_repeats++;
    g_callers[R32(ctx.esp)]++;
    auto now = std::chrono::steady_clock::now();
    if (now - g_second < std::chrono::seconds(1)) return;
    g_second = now;
    std::string top;
    for (int i = 0; i < 8 && !g_callers.empty(); i++) {
        auto best = g_callers.begin();
        for (auto it = g_callers.begin(); it != g_callers.end(); ++it)
            if (it->second > best->second) best = it;
        char buf[48];
        snprintf(buf, sizeof buf, " %06X:%llu", best->first, (unsigned long long)best->second);
        top += buf;
        g_callers.erase(best);
    }
    LOGI("traces: %llu a second, %llu repeat a call since the last link (%llu links); by caller:%s",
         (unsigned long long)g_calls, (unsigned long long)g_repeats, (unsigned long long)g_links, top.c_str());
    g_calls = g_repeats = g_links = 0;
    g_callers.clear();
}

// [[hook]] at 0xB79C0 (SV_LinkEdict's entry): entities moved, earlier results may differ
void TraceLinkCount(X86Context&, uint8_t*) {
    if (!On()) return;
    g_links++;
    g_seen.clear();
}
