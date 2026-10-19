#pragma once
// HLSL from the runtime's generator (sdk/runtime/gpu/shaders.cpp) to SPIR-V with DXC, in process (libdxcompiler).
#include <cstdint>
#include <string>
#include <vector>

namespace vk_shader {

enum class Stage { Vertex, Pixel, Geometry };

// SPIR-V words, empty on failure (logged with the source). Bindings in set 0: b0 0, b1 1, t0-t3 2-5, s0-s3 6-9.
// Cached by source, stage and entry.
const std::vector<uint32_t>& Compile(const std::string& hlsl, Stage stage, const char* entry = "main");

}  // namespace vk_shader
