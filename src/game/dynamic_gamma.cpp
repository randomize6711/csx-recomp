// The dynamic gamma's measurement read back from the GPU (FINDINGS.md "Dynamic gamma").
//
// Every frame the title's dynamic gamma (cl_dynamicgamma, 0xBDFC0) works on a 40x60 LIN_L8 surface it made at
// this+0x410 (0xBE1D0: the back buffer's size over 16, format 0x13): one frame in three it draws the back buffer
// through a luminance pixel shader into it (state 0x5BD990: 0 draw, 1 wait for D3DResource_IsBusy, 2 rest), and every
// frame 0xBE760 locks it, copies its bytes into this+0x414 and builds the histogram the gamma ramp follows (0xBE960
// moves the ramp's low, high and middle points towards the histogram's dark and light percentiles and its mean,
// 0xBEB70 writes the ramp). The lock is inlined, so the runtime never sees the CPU read a surface the GPU drew; a
// renderer that draws on the GPU leaves the title reading memory nothing wrote (the ramp then froze at its brightest).
//
// PC Options > Auto Exposure (pc_settings.ini "auto_exposure", On by default) is the title's own cl_dynamicgamma: Off
// skips the measurement, which would leave whatever ramp it set last; the title's own identity ramp (0xBF030, what it
// sets on its other ways out of a match, through 0xBF0E0) is set once as it goes off, so Off is one fixed brightness
// whenever it is chosen (PcAutoExposureFrame). Each map's light config sets cl_dynamicgamma 1 as the map starts; Off
// takes it back there (PcAutoExposureMapConfig). PC Options > Xbox Graphical Effects Off turns it off as Auto
// Exposure Off does, its own setting kept (pc::XboxEffect); On gives back what Auto Exposure holds.
//
// The hook at 0xBE777, where 0xBE760 loads the surface (ebp = this), asks for the surface as a NOOVERWRITE lock does
// (d3d::ReadLate, SDK patch 0031): the newest finished copy goes to guest memory and a new copy is started after the
// latest measurement draw. The title reads a result a frame or two old, as on the console, and nothing waits for the
// GPU.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "common/csx.h"
#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

namespace {
float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}
}  // namespace

// [[hook]] at 0xBE777 (config/game.toml): the game thread, once a frame while cl_dynamicgamma is on
// CSX_DYNGAMMA_READBACK=0 leaves the read out (the renderer's behaviour before it: for comparisons)
void DynamicGammaRead(X86Context& ctx, uint8_t*) {
    static const bool off = std::getenv("CSX_DYNGAMMA_READBACK") && *std::getenv("CSX_DYNGAMMA_READBACK") == '0';
    if (!off) d3d::ReadLate(kernel::R32(ctx.ebp + 0x410));
}

// [[hook]] at 0xBE960 (config/game.toml), the adjustment's entry (ecx = this, the frame time in seconds at [esp+4]):
// CSX_LOG_DYNGAMMA=1 logs once a second what this frame measured (the histogram's dark and light percentiles and its
// mean, 0..1, at this+0x404 / 0x408 / 0x40C) and the ramp's points that follow them (low, high, middle at +0x424 /
// 0x428 / 0x42C) before this frame's step
void DynamicGammaLog(X86Context& ctx, uint8_t*) {
    static const bool log = std::getenv("CSX_LOG_DYNGAMMA") && *std::getenv("CSX_LOG_DYNGAMMA") == '1';
    if (!log) return;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) return;
    last = now;
    uint32_t self = ctx.ecx;
    // the measurement surface's bytes as the title reads them (40x60, rows 64 bytes apart): their mean, 0..255
    uint32_t surface = kernel::R32(self + 0x410), sum = 0, n = 0;
    if (surface) {
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(mem::VirtualFromPhysical(kernel::R32(surface + 4))));
        for (uint32_t y = 0; y < 60; y++)
            for (uint32_t x = 0; x < 40; x++, n++) sum += p[y * 64 + x];
    }
    // the clamps 0xBE960 applies, cl_dynamicgamma_darkclamp (the high point's floor) and _lightclamp (the low point's
    // ceiling), and _bias, as they stand (cvar values at record + 0xC)
    LOGI("dynamic gamma: measured dark %.3f light %.3f mean %.3f, ramp low %.3f high %.3f middle %.3f, step %.4f s, "
         "clamps %.3f %.3f, bias %.3f, surface mean %.1f, cl_dynamicgamma %g",
         F(self + 0x404), F(self + 0x408), F(self + 0x40C), F(self + 0x424), F(self + 0x428), F(self + 0x42C),
         F(ctx.esp + 4), F(0x37A324), F(0x37A338), F(0x37A310), n ? double(sum) / n : -1.0, F(0x37A2FC));
}

namespace {
constexpr uint32_t kRecord = 0x37A2F0;   // cl_dynamicgamma's cvar_t: string at +4, value (float) at +0xC
constexpr uint32_t kDefault = 0x32A090;  // its static default string "1": Cvar_RegisterVariable (0x422A0) replaces it
                                         // with an allocated copy and sets the value from it
constexpr uint32_t kDynamicGamma = kRecord + 0xC;
constexpr uint32_t kIdentityRamp = 0xBF030;   // the title's identity gamma ramp, no arguments
constexpr uint32_t kCvarDirectSet = 0x41F90;  // Cvar_DirectSet(cvar_t*, const char*), as the console's command sets
constexpr uint32_t kZero = 0x329F70;          // the engine's "0" string

bool g_want = true;     // the setting
bool g_pending = false;  // the setting not in the cvar yet
}  // namespace

// on the game thread every frame (pc_keys.cpp's PcKeysFrame): PC Options' Auto Exposure set into cl_dynamicgamma when it
// changes (and at the start), through the console's own command; the value is sent again until the cvar holds it, then
// left alone, so a console change stands until the setting is changed again or the next map starts. Nothing is
// compared before the client registers the cvar (0x2BDEF): until then its value is 0 whatever the setting
void PcAutoExposureFrame(X86Context& ctx) {
    static unsigned seen = ~0u;
    static auto sent = std::chrono::steady_clock::time_point{};
    if (pc::Version() != seen) {
        seen = pc::Version();
        bool on = pc::XboxEffect("auto_exposure");
        if (on != g_want || !g_pending)
            LOGI("pc: Auto Exposure %s (cl_dynamicgamma %d)%s", on ? "on" : "off", on ? 1 : 0,
                 pc::XboxEffectsMaster() ? "" : ", Xbox Graphical Effects off");
        g_want = on;
        g_pending = true;
    }
    if (!g_pending || kernel::R32(kRecord + 4) == kDefault) return;
    if ((F(kDynamicGamma) != 0) == g_want) {
        g_pending = false;
        if (!g_want) {
            kernel::CallGuest(ctx, kIdentityRamp, {});
            LOGI("pc: Auto Exposure off, the title's identity ramp set");
        }
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - sent < std::chrono::seconds(1)) return;
    sent = now;
    csx::QueueEngineCommand(g_want ? "cl_dynamicgamma 1" : "cl_dynamicgamma 0");
}

// pc_keys.cpp's PcMapLightConfig, the hook after each map's light config (0xC408E): the config sets cl_dynamicgamma 1
// on every map (de_aztec, de_dust2, de_inferno), so Off chosen earlier was undone as each map started (FINDINGS.md
// "Auto Exposure at the start"). Off sets it back to 0 there, through the title's Cvar_DirectSet as the console's
// command does, and the identity ramp, before the map's first frame; On leaves the map's value (the title's own state)
void PcAutoExposureMapConfig(X86Context& ctx) {
    float v = F(kDynamicGamma);
    LOGI("pc: the map's light config set cl_dynamicgamma %g%s", v, g_want || v == 0 ? "" : ", Auto Exposure off sets 0");
    if (g_want || v == 0) return;
    kernel::CallGuest(ctx, kCvarDirectSet, {kRecord, kZero});
    kernel::CallGuest(ctx, kIdentityRamp, {});
    g_pending = false;
    LOGI("pc: Auto Exposure off, cl_dynamicgamma %g, the title's identity ramp set", F(kDynamicGamma));
}
