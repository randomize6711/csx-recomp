// DXT1 and DXT4/5 decoding for the flashlight's lightmap blocks (src/game/pc_dlights.cpp; tested on its own by
// tests/pc_dxt_test.py)
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

}  // namespace pcdxt
