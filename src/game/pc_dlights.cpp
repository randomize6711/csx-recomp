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
#include <utility>

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
constexpr uint32_t kWorldModel = 0x778A68, kJcabRenderBsp = 0x3771EC, kFrameCount = 0x6E7FA0, kSurfaceSize = 0x5C,
                   kBuildLightMap = 0x4FD80, kRDynamic = 0x37735C + 0xC;  // r_dynamic's value

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
    static bool first = true;
    static auto last = std::chrono::steady_clock::now();
    n++;
    auto now = std::chrono::steady_clock::now();
    if (!std::exchange(first, false) && now - last < std::chrono::seconds(5)) return;
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

// The lightmaps' RGBA blocks: the engine's buffer at [0x55199C] is freed once the lightmaps are compressed into
// their DXT1 textures (null in a match: the play-test's log), so each block a light reaches gets its own 128x128 RGBA
// block in guest memory (the game's malloc), filled once from its texture (DXT1 decoded), so the block's other
// surfaces keep their light; a new world (a map) starts again. The engine's buffer is used while it is there.
constexpr uint32_t kMalloc = 0x22D86A, kFree = 0x22C609;  // cdecl
uint32_t g_blocks[kMaxLightmaps] = {};
uint32_t g_blocks_world = 0;

// a DXT1 level into RGBA bytes (R, G, B, A: the engine's GL_RGBA order), w x h texels, 4x4 blocks row by row
void DecodeDxt1(const uint8_t* src, uint8_t* out, uint32_t w, uint32_t h) {
    auto colour = [](uint16_t c, uint8_t* rgb) {
        rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
        rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        rgb[2] = uint8_t((c & 31) * 255 / 31);
    };
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++, src += 8) {
            const uint16_t c0 = uint16_t(src[0] | src[1] << 8), c1 = uint16_t(src[2] | src[3] << 8);
            uint8_t p[4][4];
            colour(c0, p[0]);
            colour(c1, p[1]);
            p[0][3] = p[1][3] = p[2][3] = 255;
            p[3][3] = c0 > c1 ? 255 : 0;
            for (int k = 0; k < 3; k++) {
                p[2][k] = uint8_t(c0 > c1 ? (2 * p[0][k] + p[1][k]) / 3 : (p[0][k] + p[1][k]) / 2);
                p[3][k] = uint8_t(c0 > c1 ? (p[0][k] + 2 * p[1][k]) / 3 : 0);
            }
            const uint32_t bits = uint32_t(src[4]) | uint32_t(src[5]) << 8 | uint32_t(src[6]) << 16 | uint32_t(src[7]) << 24;
            for (uint32_t t = 0; t < 16; t++) {
                uint8_t* o = out + ((by * 4 + t / 4) * w + bx * 4 + t % 4) * 4;
                std::memcpy(o, p[(bits >> (2 * t)) & 3], 4);
            }
        }
}

// block lm's RGBA rows (0 when its texture is not a 128x128 DXT1 or raw RGBA one)
uint32_t BlockBuffer(X86Context& ctx, uint32_t lm) {
    if (lm >= kMaxLightmaps) return 0;
    if (uint32_t base = R32(kLightmapBuffer)) return base + lm * kBlock * kBlock * 4;
    const uint32_t world = R32(kWorldModel);
    if (world != g_blocks_world) {  // another map: its lightmaps are new
        for (uint32_t& b : g_blocks)
            if (b) {
                X86Context saved = ctx;
                ctx.esp -= 0x100;
                kernel::CallGuest(ctx, kFree, {b});
                ctx = saved;
                b = 0;
            }
        g_blocks_world = world;
    }
    if (g_blocks[lm]) return g_blocks[lm];
    const uint32_t first = R32(kLightmapTextures);
    uint32_t res = first ? R32(kGlTextures + (first + lm) * kGlTextureSize + 0x28) : 0;
    gpu::TextureDesc d;
    if (!res || !gpu::Describe(res, d) || d.width != kBlock || d.height != kBlock) return 0;
    X86Context saved = ctx;
    ctx.esp -= 0x100;
    uint32_t block = kernel::CallGuest(ctx, kMalloc, {kBlock * kBlock * 4});
    ctx = saved;
    if (!block) return 0;
    uint8_t* out = static_cast<uint8_t*>(kernel::HostPtr(block));
    if (d.format == kFormatDxt1) {
        DecodeDxt1(static_cast<const uint8_t*>(kernel::HostPtr(d.data)), out, kBlock, kBlock);
    } else if (d.format == kFormatLinearA8B8G8R8 && d.linear) {
        uint32_t pitch = d.pitch ? d.pitch : kBlock * 4;
        for (uint32_t row = 0; row < kBlock; row++)
            std::memcpy(out + row * kBlock * 4, kernel::HostPtr(d.data + row * pitch), kBlock * 4);
    } else {
        LOG_FIRST(4, "pc dlights: lightmap %u in texture format %X: not rebuilt", lm, d.format);
        X86Context again = ctx;
        ctx.esp -= 0x100;
        kernel::CallGuest(ctx, kFree, {block});
        ctx = again;
        return 0;
    }
    g_blocks[lm] = block;
    LOG_FIRST(8, "pc dlights: lightmap %u taken from its texture into a block of its own", lm);
    return block;
}

// the Xbox BSP renderer (jcab_render_bsp) draws the world from its recording: no world surface goes through the
// texture chains, so none had its lightmap rebuilt for a dynamic light. R_PushDlights (0x4BA10, every view) still
// marks the surfaces each light reaches (R_MarkLights 0x4B870: dlightframe +0x30, dlightbits +0x34, the surface's
// fields read from +0x14 there); a surface marked
// this frame (r_framecount 0x6E7FA0), or lit last time it was built (+0x50: its light gone, built once more without
// it), is rebuilt here as R_RenderDynamicLightmaps does: R_BuildLightMap (0x4FD80; the surface and the row stride
// pushed, the destination in eax) into its place in its block (lightmaptexturenum +0x38, light_s +0x1C,
// light_t +0x20), its block marked changed
void RebuildWorld(X86Context& ctx) {
    float jcab, dynamic;
    std::memcpy(&jcab, kernel::HostPtr(kJcabRenderBsp), 4);
    std::memcpy(&dynamic, kernel::HostPtr(kRDynamic), 4);
    uint32_t world = R32(kWorldModel);
    if (jcab == 0.0f || dynamic == 0.0f || !world) {  // the engine's own paths; r_dynamic 0: no dynamic light
        LOG_FIRST(2, "pc dlights: world not rebuilt here (jcab_render_bsp %g, r_dynamic %g, world %08X)", jcab, dynamic, world);
        return;
    }
    const uint32_t count = R32(world + 0xB0), surfaces = R32(world + 0xB4), frame = R32(kFrameCount);
    if (!surfaces || count > 0x40000) {
        LOG_FIRST(2, "pc dlights: the world's %u surfaces at %08X not rebuilt", count, surfaces);
        return;
    }
    uint32_t built = 0, marked_now = 0, marked_next = 0, marked_last = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t surf = surfaces + i * kSurfaceSize;
        if (R32(surf + 0x08) & 0x10) continue;  // no lightmap (SURF_DRAWTILED: water, sky)
        // marked this frame, or for the next one: R_PushDlights runs twice a frame here (0xB6045 before the view's
        // r_framecount step, 0xB6113 after it), the second marking with r_framecount + 1
        const uint32_t mark = R32(surf + 0x30);
        marked_now += mark == frame;
        marked_next += mark == frame + 1;
        marked_last += mark + 1 == frame;
        if (mark != frame && mark != frame + 1 && !R32(surf + 0x50)) continue;
        uint32_t lm = R32(surf + 0x38);
        uint32_t block = BlockBuffer(ctx, lm);
        if (!block) continue;
        uint32_t dest = block + (R32(surf + 0x20) * kBlock + R32(surf + 0x1C)) * 4;
        X86Context saved = ctx;
        ctx.esp -= 0x100;
        ctx.eax = dest;
        kernel::CallGuest(ctx, kBuildLightMap, {surf, kBlock * 4});
        ctx = saved;
        kernel::W32(kLightmapModified + 4 * lm, 1);
        built++;
    }
    if (built) LOG_FIRST(8, "pc dlights: %u world surfaces lit again by dynamic lights", built);
    // while the flashlight is on: what the surfaces' marks were (for the play-test's log)
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    uint32_t fl = R32(kFlashlightLight);
    float radius = 0;
    if (fl) std::memcpy(&radius, kernel::HostPtr(fl + 0xC), 4);
    if (radius > 0 && std::chrono::steady_clock::now() - last > std::chrono::seconds(2)) {
        last = std::chrono::steady_clock::now();
        LOG_FIRST(16, "pc dlights: frame %u, %u surfaces: marked this frame %u, the next %u, the last %u; %u rebuilt",
                  frame, count, marked_now, marked_next, marked_last, built);
    }
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
    if (R32(kLightmapBytes) != 4) {
        LOG_FIRST(2, "pc dlights: lightmaps of %u bytes a texel, not rebuilt", R32(kLightmapBytes));
        return;
    }
    const uint32_t first = R32(kLightmapTextures);
    if (!first) {
        LOG_FIRST(2, "pc dlights: no lightmap textures");
        return;
    }
    RebuildWorld(ctx);
    for (uint32_t i = 0; i < kMaxLightmaps; i++) {
        if (!R32(kLightmapModified + 4 * i)) continue;
        kernel::W32(kLightmapModified + 4 * i, 0);
        uint32_t texnum = first + i;
        uint32_t res = R32(kGlTextures + texnum * kGlTextureSize + 0x28);
        gpu::TextureDesc d;
        if (!res || !gpu::Describe(res, d) || d.width != kBlock || d.height != kBlock) continue;
        uint32_t pixels = BlockBuffer(ctx, i);
        if (pixels && Upload(ctx, res, d, pixels)) Count("the frame's sweep");
    }
}
