// Viewmodel field of view (N5E, PC Options > Advanced Graphics > Weapon Field of View, pc_settings.ini viewmodel_fov):
// the first-person weapon drawn with a projection of its own. The title draws it with the world's (FINDINGS.md "FOV
// and widescreen": R_SetupGL, 0x4E1F0, sets the projection through the GL layer's MYgluPerspective, 0x4DF80, and the
// weapon, R_DrawViewModel 0x4D300, called from R_RenderView at 0x4EFC2, uses it as it is), so a wider Field of View
// draws the weapon smaller. Here:
//   - R_SetupGL's call (0x4E264) gives the world's perspective (the vertical angle, the aspect, near and far: four
//     doubles on the stack), kept;
//   - before R_DrawViewModel the projection matrix is pushed and loaded with the same perspective at the weapon's
//     vertical angle (its horizontal degrees at 4:3, as Field of View's: 2 atan(tan(h / 2) x 3 / 4)); after it the
//     matrix is popped (the GL layer's glMatrixMode 0x6A889C, glPushMatrix 0x6A88C0, glLoadIdentity 0x6A84CC,
//     glPopMatrix 0x6A8CA8: guest function pointers, stdcall);
//   - "Same as Field of View" (the default) leaves the title's projection alone; a scoped weapon (the game's zoom:
//     scr_fov under 59) is not drawn by the title, nothing changes then either.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "common/pc_settings.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;

constexpr uint32_t kPerspective = 0x4DF80;  // MYgluPerspective(double fovy, aspect, near, far), cdecl
constexpr uint32_t kMatrixMode = 0x6A889C, kPushMatrix = 0x6A88C0, kLoadIdentity = 0x6A84CC, kPopMatrix = 0x6A8CA8;
constexpr uint32_t kProjection = 0x1701, kModelView = 0x1700;
constexpr uint32_t kScrFov = 0x377668;

double g_world[4] = {0, 0, 0, 0};  // the world's fovy, aspect, near, far (R_SetupGL's, this frame)
bool g_pushed = false;

float Wanted() {  // the weapon's horizontal degrees at 4:3, 0: the world's
    static unsigned seen = ~0u;
    static float fov = 0;
    if (pc::Version() != seen) {
        seen = pc::Version();
        fov = pc::GetFloat("viewmodel_fov", 0.0f);
        if (!(fov >= 50.0f && fov <= 130.0f)) fov = 0;
    }
    return fov;
}

void Gl(X86Context& ctx, uint32_t pointer, std::initializer_list<uint32_t> args) {
    if (uint32_t fn = R32(pointer)) kernel::CallGuest(ctx, fn, args);
}

void Split(double d, uint32_t& lo, uint32_t& hi) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    lo = uint32_t(b);
    hi = uint32_t(b >> 32);
}
}  // namespace

// [[hook]] at 0x4E264 (config/game.toml): R_SetupGL about to call MYgluPerspective, its four doubles at esp
void PcViewmodelWorld(X86Context& ctx, uint8_t*) { std::memcpy(g_world, kernel::HostPtr(ctx.esp), sizeof g_world); }

// [[hook]] at 0x4EFC2: R_RenderView about to draw the first-person weapon
void PcViewmodelBefore(X86Context& ctx, uint8_t*) {
    g_pushed = false;
    float fov = Wanted(), scr;
    std::memcpy(&scr, kernel::HostPtr(kScrFov), 4);
    if (fov <= 0 || !(scr >= 59.0f) || g_world[1] <= 0) return;
    double fovy = 2.0 * std::atan(std::tan(double(fov) * M_PI / 360.0) * 0.75) * 180.0 / M_PI;
    uint32_t a[8];
    Split(fovy, a[0], a[1]);
    Split(g_world[1], a[2], a[3]);
    Split(g_world[2], a[4], a[5]);
    Split(g_world[3], a[6], a[7]);
    Gl(ctx, kMatrixMode, {kProjection});
    Gl(ctx, kPushMatrix, {});
    Gl(ctx, kLoadIdentity, {});
    kernel::CallGuest(ctx, kPerspective, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
    Gl(ctx, kMatrixMode, {kModelView});
    g_pushed = true;
    static float logged = -1;
    if (fov != logged) {
        logged = fov;
        LOGI("pc view: the weapon drawn at %g degrees (vertical %.2f; the world's %.2f)", fov, fovy, g_world[0]);
    }
}

// [[hook]] at 0x4EFC7: the weapon drawn: the world's projection back
void PcViewmodelAfter(X86Context& ctx, uint8_t*) {
    if (!g_pushed) return;
    g_pushed = false;
    Gl(ctx, kMatrixMode, {kProjection});
    Gl(ctx, kPopMatrix, {});
    Gl(ctx, kMatrixMode, {kModelView});
}
