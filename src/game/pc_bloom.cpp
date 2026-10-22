// PC Options > Bloom (pc_settings.ini "bloom", On by default; FINDINGS.md "Bloom") is the title's own cl_hdrbloom:
// the per-frame post pass (0xC2160) draws the bloom (0xC06D0, 0xC1FA0, 0xC0D00) only while the cvar is not 0, and its
// value is the blend colour the blurred image is added back with (the intensity). Each map sets its own intensity:
// the client's map start runs "exec <map>-light.cfg" (0xC3E90, the command at 0xC4089), which sets cl_hdrbloom (0.5
// on de_aztec; 0.6 is the cvar's default). Off sets the cvar to 0 when chosen and again after every map's light
// config; On gives back the intensity the cvar last held (the map's, or one set in the console). A console change
// stands until the setting changes or the next map starts.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kRecord = 0x37A588;    // cl_hdrbloom's cvar_t (FCVAR_ARCHIVE): string at +4, value at +0xC
constexpr uint32_t kDefault = 0x33AC90;   // its static default string "0.6", replaced when the client registers it
constexpr uint32_t kHdrBloom = kRecord + 0xC;

bool g_want = true;        // the setting
bool g_pending = false;    // the setting not in the cvar yet
float g_intensity = 0.0f;  // the intensity On gives back: the map's, or the last the cvar held
auto g_sent = std::chrono::steady_clock::time_point{};

float Value() {
    float v;
    std::memcpy(&v, kernel::HostPtr(kHdrBloom), 4);
    return v;
}

}  // namespace

// on the game thread every frame (pc_keys.cpp's PcKeysFrame): a changed setting set into cl_hdrbloom through the
// console's own command, sent again until the cvar holds it, then left alone. At the start only Off acts (On is the
// title's own state), once the client has registered the cvar (its value is 0 before, whatever the setting); each
// map's light config sets it again afterwards, which PcBloomMapConfig takes back.
void PcBloomFrame(X86Context&) {
    static unsigned seen = ~0u;
    float v = Value();
    if (v != 0) g_intensity = v;
    if (pc::Version() != seen) {
        bool first = seen == ~0u;
        seen = pc::Version();
        bool on = pc::Get("bloom", "1") != "0";
        if (first || on != g_want) {
            LOGI("pc: Bloom %s", on ? "on" : "off");
            g_pending = !first || !on;
        }
        g_want = on;
    }
    if (!g_pending || kernel::R32(kRecord + 4) == kDefault) return;
    if ((v != 0) == g_want || (g_want && g_intensity == 0)) {
        g_pending = false;
        LOGI("pc: Bloom %s, cl_hdrbloom %g", g_want ? "on" : "off", v);
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - g_sent < std::chrono::seconds(1)) return;
    g_sent = now;
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "cl_hdrbloom %g", g_want ? g_intensity : 0.0f);
    csx::QueueEngineCommand(cmd);
}

// pc_keys.cpp's PcMapLightConfig, after the map's light config (0x1A5B80 executes the buffer it added): its
// cl_hdrbloom is the intensity On gives back (0 included: a map without the bloom), unless Off's own 0 is left
void PcBloomMapConfig(X86Context&) {
    float v = Value();
    if (g_want || v != 0) g_intensity = v;
    LOGI("pc: the map's light config set cl_hdrbloom %g%s", v, g_want ? "" : ", Bloom off sets 0");
    if (!g_want && v != 0) {
        g_pending = true;
        g_sent = {};
    }
}
