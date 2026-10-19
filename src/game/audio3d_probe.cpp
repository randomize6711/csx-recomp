// CSX_AUDIO_LOG3D=1: each 3D sound's distance and the distance level XACT computed for it, for checking attenuation
// (FINDINGS.md "Audio"). XACT runs the DSOUND library's own 3D calculator (0x2464AF, guest code) on the cue's source
// parameters and its listener, then hands the result block to IDirectSoundBuffer_Set3DVoiceData at 0x2FCB91:
// eax = the buffer, ebx = the block (cue+0xE8: flags, then l3DDistanceVolume in mB at +4), esi = the cue, whose source
// parameters at +0x44 hold the position at +8 and the minimum / maximum distance at +0x38 / +0x3C, and edi = the
// listener (0x3038A0), position at +8 (the calculator's inputs, analysis/dsound_xact_5659.md). Only reads.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
float F(uint32_t va) {
    float f;
    std::memcpy(&f, kernel::HostPtr(va), 4);
    return f;
}
}  // namespace

// [[hook]] at 0x2FCB91 (config/game.toml), before the Set3DVoiceData call
void Audio3DProbe(X86Context& ctx, uint8_t*) {
    static const bool on = std::getenv("CSX_AUDIO_LOG3D") && *std::getenv("CSX_AUDIO_LOG3D") == '1';
    if (!on || !(kernel::R32(ctx.ebx) & 1)) return;  // only blocks that carry a distance level
    uint32_t src = ctx.esi + 0x44, lis = ctx.edi;
    float d = 0;
    for (int i = 0; i < 3; i++) {
        float x = F(src + 8 + i * 4) - F(lis + 8 + i * 4);
        d += x * x;
    }
    LOGI("audio3d: voice %08X distance %.1f min %.0f max %.0f level %d mB", ctx.eax, std::sqrt(d), F(src + 0x38),
         F(src + 0x3C), int32_t(kernel::R32(ctx.ebx + 4)));
}
