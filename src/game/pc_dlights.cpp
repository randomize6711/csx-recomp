// Dynamic lights on the world (the flashlight, muzzle flashes, explosions): the engine's GoldSrc path is intact. The
// client puts a light where the player's view hits (the flashlight: CL_AllocDlight key 1 at the trace's end, 0x24B10;
// radius from the distance and the beam's angle, brightness falling with the distance), R_RecursiveWorldNode and the
// texture chains rebuild the lightmaps a light touches into the 128x128 RGBA blocks at [0x55199C] (R_BuildLightMap
// 0x4FD80, R_RenderDynamicLightmaps 0x513C0; lightmap_modified at 0x6E7800), and R_BlendLightmaps (0x51000) uploads
// each changed block with glTexSubImage2D. The Xbox's GL layer left glTexSubImage2D empty (0x72480: ret 0x24): its
// lightmaps are DXT1 textures (glTexImage2D 0x728A0 compresses them with XGCompressRect 0x2D198E), so no dynamic light
// ever reached the screen, and the flashlight lit nothing (the title's mp_flashlight is 0).
//
// Here glTexSubImage2D does the upload (and a sweep at R_DrawWorld's end takes any block still marked changed): the
// whole level 0 of the texture bound (glBindTexture 0x70050: the unit's texture number at 0x45280C, the GL texture
// objects at 0x452DFC, 0x2C bytes each, the D3D texture at +0x28) is compressed again by the game's own XGCompressRect into the texture's memory, as glTexImage2D made it, and marked
// changed so the renderer uploads it (gpu::MarkDirty). A raw (linear RGBA) texture is copied. Lightmaps only change
// where a light is, and only while it is.
#include <chrono>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"

namespace {
using kernel::R32;
constexpr uint32_t kBoundTexture = 0x45280C, kTextureUnit = 0x45281C, kGlTextures = 0x452DFC, kGlTextureSize = 0x2C;
constexpr uint32_t kXGCompressRect = 0x2D198E;
constexpr uint32_t kGlTexture2D = 0x0DE1, kGlRgba = 0x1908, kGlUnsignedByte = 0x1401;
constexpr uint32_t kFormatDxt1 = 0x0C, kFormatLinearA8B8G8R8 = 0x3F;
// the engine's lightmaps (GL_BuildLightmaps 0x529xx): the RGBA blocks, bytes a texel, the first lightmap's texture
// number, the changed flags; 128 blocks of 128x128 at most (lightmap_polys at 0x6D5960 has 0x80 entries)
constexpr uint32_t kLightmapBuffer = 0x55199C, kLightmapBytes = 0x6E6380, kLightmapTextures = 0x6E7A04,
                   kLightmapModified = 0x6E7800, kMaxLightmaps = 0x80, kBlock = 128;
constexpr uint32_t kFlashlightLight = 0x7791EC;

// a texture's level 0 from RGBA rows (the whole level: XGCompressRect takes power-of-two sizes only), marked changed
bool Upload(X86Context& ctx, uint32_t res, const gpu::TextureDesc& d, uint32_t pixels) {
    const uint32_t w = d.width, h = d.height;
    if (d.format == kFormatDxt1) {
        X86Context saved = ctx;
        ctx.esp -= 0x100;
        ctx.eax = 2;  // the flags glTexImage2D passes in eax
        float alpha_ref = 0.01f;
        uint32_t ref;
        std::memcpy(&ref, &alpha_ref, 4);
        kernel::CallGuest(ctx, kXGCompressRect, {d.data, kFormatDxt1, w * 2, w, h, pixels, kFormatLinearA8B8G8R8, w * 4, ref});
        ctx = saved;
    } else if (d.format == kFormatLinearA8B8G8R8 && d.linear) {
        uint32_t pitch = d.pitch ? d.pitch : w * 4;
        for (uint32_t row = 0; row < h; row++)
            std::memcpy(kernel::HostPtr(d.data + row * pitch), kernel::HostPtr(pixels + row * w * 4), w * 4);
    } else {
        LOG_FIRST(4, "pc dlights: a lightmap in texture format %X not handled", d.format);
        return false;
    }
    gpu::MarkDirty(res);
    return true;
}

// uploads counted, logged once a few seconds (what a play-test's log shows of the lights)
void Count(const char* how) {
    static uint32_t n = 0;
    static auto last = std::chrono::steady_clock::now();
    n++;
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    LOG_FIRST(64, "pc dlights: %u lightmap uploads in %.0f s (last by %s)", n,
              std::chrono::duration<double>(now - last).count(), how);
    n = 0;
    last = now;
}
}  // namespace

// [[hook]] at 0x72480 (config/game.toml), the GL layer's glTexSubImage2D (stdcall: target, level, x, y, width,
// height, format, type, pixels): the texture bound updated. The engine uploads a whole lightmap (R_BlendLightmaps) or
// the band of rows a light changed (the multitexture path, 0x50420: x 0, the whole width, pixels at the band's first
// row of the 128x128 block): the whole level is compressed again from the block's start
void PcGlTexSubImage(X86Context& ctx, uint8_t*) {
    uint32_t a[9];
    for (uint32_t i = 0; i < 9; i++) a[i] = R32(ctx.esp + 4 + 4 * i);
    const uint32_t target = a[0], level = a[1], x = a[2], y = a[3], w = a[4], h = a[5], format = a[6], type = a[7],
                   pixels = a[8];
    uint32_t texnum = R32(kBoundTexture + 4 * R32(kTextureUnit));
    uint32_t res = texnum < 0x10000 ? R32(kGlTextures + texnum * kGlTextureSize + 0x28) : 0;
    gpu::TextureDesc d;
    if (target != kGlTexture2D || level != 0 || !pixels || !res || !gpu::Describe(res, d)) return;
    if (x != 0 || w != d.width || y + h > d.height || format != kGlRgba || type != kGlUnsignedByte) {
        LOG_FIRST(4, "pc dlights: glTexSubImage2D of %ux%u at %u,%u (texture %ux%u, format %X) not handled", w, h, x, y,
                  d.width, d.height, format);
        return;
    }
    if (Upload(ctx, res, d, pixels - y * w * 4)) Count("glTexSubImage2D");
}

// [[hook]] at 0x5236D, the end of R_DrawWorld (after R_BlendLightmaps, or where it was skipped): a lightmap still
// marked changed (lightmap_modified, 0x6E7800) by a dynamic light is uploaded here, whichever drawing path left it
// (the Xbox BSP renderer draws the world from its recording, not through the chains that upload)
void PcDlightsSweep(X86Context& ctx, uint8_t*) {
    // the flashlight's light (dlight key 1, kept at 0x7791EC: origin, radius +0xC), logged for the play-tests
    if (uint32_t fl = R32(kFlashlightLight)) {
        float v[4];
        std::memcpy(v, kernel::HostPtr(fl), sizeof v);
        if (v[3] > 0) LOG_FIRST(4, "pc dlights: the flashlight's light at %.0f %.0f %.0f, radius %.0f", v[0], v[1], v[2], v[3]);
    }
    if (R32(kLightmapBytes) != 4) return;
    const uint32_t base = R32(kLightmapBuffer), first = R32(kLightmapTextures);
    if (!base || !first) return;
    for (uint32_t i = 0; i < kMaxLightmaps; i++) {
        if (!R32(kLightmapModified + 4 * i)) continue;
        kernel::W32(kLightmapModified + 4 * i, 0);
        uint32_t texnum = first + i;
        uint32_t res = R32(kGlTextures + texnum * kGlTextureSize + 0x28);
        gpu::TextureDesc d;
        if (!res || !gpu::Describe(res, d) || d.width != kBlock || d.height != kBlock) continue;
        if (Upload(ctx, res, d, base + i * kBlock * kBlock * 4)) Count("the frame's sweep");
    }
}
