// Dynamic lights on the world (the flashlight, muzzle flashes, explosions): the engine's GoldSrc path is intact. The
// client puts a light where the player's view hits (the flashlight: CL_AllocDlight key 1 at the trace's end, 0x24B10;
// radius from the distance and the beam's angle, brightness falling with the distance), R_RecursiveWorldNode and the
// texture chains rebuild the lightmaps a light touches into the 128x128 RGBA blocks at [0x55199C] (R_BuildLightMap
// 0x4FD80, R_RenderDynamicLightmaps 0x513C0; lightmap_modified at 0x6E7800), and R_BlendLightmaps (0x51000) uploads
// each changed block with glTexSubImage2D. The Xbox's GL layer left glTexSubImage2D empty (0x72480: ret 0x24): its
// lightmaps are DXT1 textures (glTexImage2D 0x728A0 compresses them with XGCompressRect 0x2D198E), so no dynamic light
// ever reached the screen, and the flashlight lit nothing (the title's mp_flashlight is 0).
//
// Here glTexSubImage2D does the upload: the whole level 0 of the texture bound (glBindTexture 0x70050: the unit's
// texture number at 0x45280C, the GL texture objects at 0x452DFC, 0x2C bytes each, the D3D texture at +0x28) is
// compressed again by the game's own XGCompressRect into the texture's memory, as glTexImage2D made it, and marked
// changed so the renderer uploads it (gpu::MarkDirty). A raw (linear RGBA) texture is copied. Lightmaps only change
// where a light is, and only while it is.
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
}  // namespace

// [[hook]] at 0x72480 (config/game.toml), the GL layer's glTexSubImage2D (stdcall: target, level, x, y, width,
// height, format, type, pixels): the texture bound updated
void PcGlTexSubImage(X86Context& ctx, uint8_t*) {
    uint32_t a[9];
    for (uint32_t i = 0; i < 9; i++) a[i] = R32(ctx.esp + 4 + 4 * i);
    const uint32_t target = a[0], level = a[1], x = a[2], y = a[3], w = a[4], h = a[5], format = a[6], type = a[7],
                   pixels = a[8];
    uint32_t texnum = R32(kBoundTexture + 4 * R32(kTextureUnit));
    uint32_t res = texnum < 0x10000 ? R32(kGlTextures + texnum * kGlTextureSize + 0x28) : 0;
    gpu::TextureDesc d;
    if (target != kGlTexture2D || level != 0 || !pixels || !res || !gpu::Describe(res, d)) return;
    // the whole level in RGBA bytes: what R_BlendLightmaps uploads (a part of a DXT1 texture would need its blocks')
    if (x != 0 || y != 0 || w != d.width || h != d.height || format != kGlRgba || type != kGlUnsignedByte) {
        LOG_FIRST(4, "pc dlights: glTexSubImage2D of %ux%u at %u,%u (texture %ux%u, format %X) not handled", w, h, x, y,
                  d.width, d.height, format);
        return;
    }
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
        LOG_FIRST(4, "pc dlights: glTexSubImage2D into texture format %X not handled", d.format);
        return;
    }
    gpu::MarkDirty(res);
    LOG_FIRST(4, "pc dlights: texture %u (%ux%u, format %X) updated: a dynamic light on a lightmap", texnum, w, h,
              d.format);
}
