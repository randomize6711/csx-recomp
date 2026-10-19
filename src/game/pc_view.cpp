// Field of view (Options > PC Options, FINDINGS.md "FOV and widescreen"). The engine's view FOV is scr_fov_value
// (0x377668, horizontal degrees at 4:3), written every frame from the client's HUD_UpdateClientData result
// (0x1CD12: the server's per-player FOV, 90, or a weapon's zoom: 55, 40, 15, 10) and read by R_SetupGL (0x4E112:
// CalcFovY 0x4DFD0 then MYgluPerspective 0x4DF80) and R_SetFrustum (0x4DAB9: the culling planes), so a value changed
// here moves the projection and the culling together. Only the unzoomed 90 is replaced: zoomed weapons keep the FOV
// they set, so their magnification is the original's. The first-person weapon is drawn with the same projection
// (GoldSrc has no separate viewmodel FOV), so it looks smaller at a wide FOV, as on a PC.
#include <cstring>

#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
constexpr uint32_t kScrFov = 0x377668;
constexpr float kOriginalFov = 90.0f;  // CS's DEFAULT_FOV, the unzoomed view

float UserFov() {
    static unsigned seen = ~0u;
    static float fov = kOriginalFov;
    if (pc::Version() != seen) {
        seen = pc::Version();
        fov = pc::GetFloat("fov", kOriginalFov);
        if (!(fov >= 60.0f && fov <= 130.0f)) fov = kOriginalFov;
    }
    return fov;
}
}  // namespace

// [[hook]] at 0x1CD17 (config/game.toml): after the per-frame scr_fov_value update, on the game thread
void PcFovFrame(X86Context&, uint8_t*) {
    float v, user = UserFov();
    std::memcpy(&v, kernel::HostPtr(kScrFov), 4);
    if (v == kOriginalFov && user != kOriginalFov) std::memcpy(kernel::HostPtr(kScrFov), &user, 4);
    static float shown = 0;
    float now = v == kOriginalFov ? user : v;
    if (now != shown) {
        shown = now;
        LOGI("pc view: fov %g%s", now, v == kOriginalFov ? "" : " (the game's zoom)");
    }
}
