// DXT1 and DXT4/5 decoding and encoding for the dynamic lights' lightmap blocks (src/game/pc_dlights.cpp; tested on
// its own by tests/pc_dxt_test.py)
#pragma once
#include <cstdint>
#include <cstring>

namespace pcdxt {

// a DXT1 level (or DXT4/5's: each block's 8 bytes of alpha first, then its colour block, always four colours) into
// RGBA bytes (R, G, B, A: the engine's GL_RGBA order), w x h texels, 4x4 blocks row by row
inline void DecodeDxt(const uint8_t* src, uint8_t* out, uint32_t w, uint32_t h, bool dxt45) {
    auto colour = [](uint16_t c, uint8_t* rgb) {
        rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
        rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        rgb[2] = uint8_t((c & 31) * 255 / 31);
    };
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++) {
            uint8_t alpha[8] = {255, 255, 255, 255, 255, 255, 255, 255};
            uint64_t alpha_bits = 0;
            if (dxt45) {
                const int a0 = src[0], a1 = src[1];
                alpha[0] = uint8_t(a0);
                alpha[1] = uint8_t(a1);
                for (int k = 2; k < 8; k++)
                    alpha[k] = a0 > a1 ? uint8_t(((8 - k) * a0 + (k - 1) * a1 + 3) / 7)
                               : k < 6 ? uint8_t(((6 - k) * a0 + (k - 1) * a1 + 2) / 5)
                                       : uint8_t(k == 6 ? 0 : 255);
                for (int k = 0; k < 6; k++) alpha_bits |= uint64_t(src[2 + k]) << (8 * k);
                src += 8;
            }
            const uint16_t c0 = uint16_t(src[0] | src[1] << 8), c1 = uint16_t(src[2] | src[3] << 8);
            const bool four = dxt45 || c0 > c1;
            uint8_t p[4][4];
            colour(c0, p[0]);
            colour(c1, p[1]);
            p[0][3] = p[1][3] = p[2][3] = 255;
            p[3][3] = four ? 255 : 0;
            for (int k = 0; k < 3; k++) {
                p[2][k] = uint8_t(four ? (2 * p[0][k] + p[1][k]) / 3 : (p[0][k] + p[1][k]) / 2);
                p[3][k] = uint8_t(four ? (p[0][k] + 2 * p[1][k]) / 3 : 0);
            }
            const uint32_t bits = uint32_t(src[4]) | uint32_t(src[5]) << 8 | uint32_t(src[6]) << 16 | uint32_t(src[7]) << 24;
            src += 8;
            for (uint32_t t = 0; t < 16; t++) {
                uint8_t* o = out + ((by * 4 + t / 4) * w + bx * 4 + t % 4) * 4;
                std::memcpy(o, p[(bits >> (2 * t)) & 3], 4);
                if (dxt45) o[3] = alpha[(alpha_bits >> (3 * t)) & 7];
            }
        }
}

// RGBA bytes (w x h texels, multiples of 4) into a DXT1 level, or DXT4/5's (each block's alpha first): a quick
// range-fit encoder for the dynamic lights' lightmaps, whose blocks are smooth. The colour endpoints are the block's
// channel minimum and maximum, inset by a sixteenth of the range, as RGB565 with c0 > c1 (four colours, never DXT1's
// transparent index); each texel takes the nearest of the four colours as DecodeDxt makes them. Alpha (DXT4/5): its
// minimum and maximum, the nearest of the eight steps. On the host: the title's XGCompressRect, run as guest code,
// took about 20 ms a 128x128 lightmap (the 2026-11-16 playtest: the flashlight and explosions at 3 frames a second)
inline void EncodeDxt(const uint8_t* rgba, uint8_t* dst, uint32_t w, uint32_t h, bool dxt45) {
    auto to565 = [](int r, int g, int b) { return uint16_t((r * 31 + 127) / 255 << 11 | (g * 63 + 127) / 255 << 5 | (b * 31 + 127) / 255); };
    auto colour = [](uint16_t c, int* rgb) {
        rgb[0] = ((c >> 11) & 31) * 255 / 31;
        rgb[1] = ((c >> 5) & 63) * 255 / 63;
        rgb[2] = (c & 31) * 255 / 31;
    };
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++) {
            const uint8_t* px[16];
            for (uint32_t t = 0; t < 16; t++) px[t] = rgba + ((by * 4 + t / 4) * w + bx * 4 + t % 4) * 4;
            if (dxt45) {
                int a0 = 0, a1 = 255;
                for (const uint8_t* p : px) a0 = p[3] > a0 ? p[3] : a0, a1 = p[3] < a1 ? p[3] : a1;
                int steps[8] = {a0, a1};
                for (int k = 2; k < 8; k++) steps[k] = ((8 - k) * a0 + (k - 1) * a1 + 3) / 7;
                uint64_t bits = 0;
                if (a0 > a1)
                    for (uint32_t t = 0; t < 16; t++) {
                        int best = 0, err = 1 << 30;
                        for (int k = 0; k < 8; k++) {
                            int e = px[t][3] - steps[k];
                            e = e < 0 ? -e : e;
                            if (e < err) err = e, best = k;
                        }
                        bits |= uint64_t(best) << (3 * t);
                    }
                dst[0] = uint8_t(a0);
                dst[1] = uint8_t(a1);
                for (int k = 0; k < 6; k++) dst[2 + k] = uint8_t(bits >> (8 * k));
                dst += 8;
            }
            int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (const uint8_t* p : px)
                for (int k = 0; k < 3; k++) lo[k] = p[k] < lo[k] ? p[k] : lo[k], hi[k] = p[k] > hi[k] ? p[k] : hi[k];
            for (int k = 0; k < 3; k++) {
                const int inset = (hi[k] - lo[k]) / 16;
                hi[k] -= inset;
                lo[k] += inset;
            }
            uint16_t c0 = to565(hi[0], hi[1], hi[2]), c1 = to565(lo[0], lo[1], lo[2]);
            if (c0 < c1) {
                const uint16_t s = c0;
                c0 = c1;
                c1 = s;
            }
            uint32_t bits = 0;
            if (c0 != c1) {
                int p[4][3];
                colour(c0, p[0]);
                colour(c1, p[1]);
                for (int k = 0; k < 3; k++) {
                    p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
                    p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
                }
                for (uint32_t t = 0; t < 16; t++) {
                    int best = 0, err = 1 << 30;
                    for (int i = 0; i < 4; i++) {
                        int e = 0;
                        for (int k = 0; k < 3; k++) e += (px[t][k] - p[i][k]) * (px[t][k] - p[i][k]);
                        if (e < err) err = e, best = i;
                    }
                    bits |= uint32_t(best) << (2 * t);
                }
            }
            dst[0] = uint8_t(c0);
            dst[1] = uint8_t(c0 >> 8);
            dst[2] = uint8_t(c1);
            dst[3] = uint8_t(c1 >> 8);
            for (int k = 0; k < 4; k++) dst[4 + k] = uint8_t(bits >> (8 * k));
            dst += 8;
        }
}

}  // namespace pcdxt
