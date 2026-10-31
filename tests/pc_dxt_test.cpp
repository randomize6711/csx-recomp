// tests/pc_dxt_test.py builds and runs this: src/game/pc_dxt.h on its own
#include <cstdio>

#include "game/pc_dxt.h"

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    uint8_t out[4 * 4 * 4];
    // DXT1: c0 white (0xFFFF) > c1 black: index 0 white, 1 black, 2 two thirds, 3 a third
    const uint8_t dxt1[8] = {0xFF, 0xFF, 0x00, 0x00, 0b11100100, 0, 0, 0};  // texels 0..3: 0, 1, 2, 3
    pcdxt::DecodeDxt(dxt1, out, 4, 4, false);
    Check(out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 255, "DXT1 index 0: c0");
    Check(out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 255, "DXT1 index 1: c1");
    Check(out[8] == 170 && out[12] == 85, "DXT1 indices 2 and 3: the thirds");
    Check(out[16] == 255, "DXT1 texel 4 (index 0)");
    // DXT1 three-colour mode (c0 <= c1): index 3 transparent black
    const uint8_t dxt1t[8] = {0x00, 0x00, 0xFF, 0xFF, 0b11000000, 0, 0, 0};
    pcdxt::DecodeDxt(dxt1t, out, 4, 4, false);
    Check(out[12] == 0 && out[15] == 0 && out[0] == 0 && out[3] == 255, "DXT1 three colours: index 3 transparent");
    // DXT5: alpha 255..0 (a0 > a1: eight values), colour always four-colour even with c0 < c1
    uint8_t dxt5[16] = {255, 0, 0b10001000, 0, 0, 0, 0, 0, 0x00, 0x00, 0x1F, 0x00, 0b00000001, 0, 0, 0};
    // alpha indices: texel 0 -> 0 (255), texel 1 -> 1 (0), texel 2 -> 2 (219); colour texel 0 index 1 (blue)
    pcdxt::DecodeDxt(dxt5, out, 4, 4, true);
    Check(out[3] == 255 && out[7] == 0 && out[11] == 219, "DXT5 alpha: a0, a1 and the first step");
    Check(out[0] == 0 && out[1] == 0 && out[2] == 255, "DXT5 colour: c1 (blue) at texel 0");
    Check(out[4] == 0 && out[6] == 0, "DXT5 colour: c0 (black) at texel 1");
    // an 8x8 level: the second block goes to texels 4..7 of the first rows
    uint8_t two[32] = {};
    two[8] = 0xFF;
    two[9] = 0xFF;  // block 1's c0 white, all indices 0
    uint8_t big[8 * 8 * 4] = {};
    pcdxt::DecodeDxt(two, big, 8, 4, false);
    Check(big[0] == 0 && big[4 * 4] == 255 && big[(8 + 4) * 4] == 255, "blocks laid out left to right");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
