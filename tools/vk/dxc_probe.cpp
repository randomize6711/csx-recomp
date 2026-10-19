// Writes representative shaders from the runtime's HLSL generator (sdk/runtime/gpu/shaders.cpp) so they can be
// compiled to SPIR-V with DXC: tools/vk/dxc_probe.sh
#include <cstdio>
#include <string>

#include "gpu/shaders.h"
#include "gpu/vertex.h"

using namespace gpu;

static void Write(const char* name, const std::string& s) {
    std::string path = std::string("build/vk_probe/") + name + ".hlsl";
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        std::fwrite(s.data(), 1, s.size(), f);
        std::fclose(f);
    }
}

int main() {
    // CS's UI text program (handle 0xB0064B51): oPos.xy = v0.xy, oT0.xy = v0.zw, oD0 = v3
    std::vector<uint32_t> text = {0x00000000, 0x00200015, 0x0836106C, 0x2070C800, 0x00000000, 0x002000BF,
                                  0x0836106C, 0x2070C848, 0x00000000, 0x0020061B, 0x0836106C, 0x2070F819};
    Write("vs_program_text", VertexProgramHlsl(text, (1 << 0) | (1 << 3)));
    Write("vs_fixed_rhw", FixedVertexHlsl((1 << 0) | (1 << 3) | (1 << 9), true));
    Write("vs_fixed_xyz", FixedVertexHlsl((1 << 0) | (1 << 3) | (1 << 9), false));
    State s{};
    s.tss[0][kTssColorOp] = 4;  // modulate texture and diffuse
    s.tss[0][kTssColorArg1] = 2;
    s.tss[0][kTssColorArg2] = 0;
    s.tss[0][kTssAlphaOp] = 2;
    s.tss[0][kTssAlphaArg1] = 2;
    for (int i = 1; i < 4; i++) s.tss[i][kTssColorOp] = s.tss[i][kTssAlphaOp] = 1;
    TexInfo tex[4] = {};
    tex[0].kind = kTex2D;
    Write("ps_fixed_tex", PixelHlsl(MakePixelKey(s, tex)));
    PixelKey pk{};
    pk.programmable = 1;
    pk.count = 1;
    pk.tex[0].kind = kTex2D;
    Write("ps_combiner", PixelHlsl(pk));
    Write("gs_point", PointHlsl());
    return 0;
}
