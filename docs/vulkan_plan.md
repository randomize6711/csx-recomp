# Vulkan backend plan

Goal: a hardware-accelerated `gpu::Backend` for the native Linux build (later usable on Windows), next to the CPU
reference renderer, which stays as the reference and regression backend. SDL3 keeps the window and input.

## What exists (sdk/runtime/gpu, upstream XboxXBERecomp)

| Component | File | Platform | Reuse |
|---|---|---|---|
| Backend interface (Init, Clear, Draw, Copy, Present, Sync, SyncLate, Flush, SetGamma, fences, reports) | backend.h | neutral | implement it |
| Console state model (render/texture states, streams, shaders, constants, viewport, targets, super sample) | state.h | neutral | as is |
| Vertex assembly: streams to host buffers, format decode, index rebuild for fans/quads/loops | vertex.cpp | neutral (uses DXGI_FORMAT codes) | as is; map DXGI formats to VkFormat |
| Texture description and decoding to BGRA8 / BC1-3 / R32F / R16 | texture.cpp | neutral | as is; BC1-3 are native on RADV |
| HLSL generation: vertex programs, fixed function, register combiners, point sprites, screen quads | shaders.cpp | neutral | as is, compiled with DXC to SPIR-V (verified, below) |
| Render thread wrapper (calls recorded and replayed on a thread) | render_thread.cpp | neutral | later, unchanged |
| D3D12 renderer | d3d12.cpp, ext.h | Windows | design reference only |

The D3D12 renderer's own logic that Vulkan has to re-implement: the render target cache keyed by guest surface address
(with signatures of guest memory to see CPU writes, readbacks for CPU reads, feedback copies for targets sampled while
bound), the texture cache (address, format, palette, dirty epoch), the pipeline cache (PsoKey), per-frame upload rings
for constants and vertices, descriptor management, frames in flight and fences, presentation (scaled blit, gamma),
CopyRects, visibility queries, and the depth copies some titles need.

## Shader path (verified 2026-10-19, tools/vk/dxc_probe.sh)

The runtime's generated HLSL compiles to valid Vulkan 1.3 SPIR-V with DXC 1.10 (`dxc -spirv -HV 2018
-fspv-target-env=vulkan1.3 -fvk-b-shift 0 0 -fvk-t-shift 2 0 -fvk-s-shift 6 0`, Shader Model 6.0 profiles; spirv-val
passes): the fixed function vertex shader (pretransformed and transformed), CS's UI text vertex program, a fixed
function pixel shader with a texture, a register combiner pixel shader and the point sprite geometry shader.

- `-HV 2018`: the generator writes FXC-era HLSL (vector `||` and `?:`); HLSL 2021 rejects it.
- Bindings, all in set 0: b0 (192 vertex constants) = 0, b1 (DrawConstants) = 1, t0-t3 = 2-5, s0-s3 = 6-9.
- Vertex inputs get locations in declaration order (ascending v registers), so location i = the i-th present register.
- Stage interface: both stages use the generator's VOut struct, so locations match.
- At run time the backend will call libdxcompiler (installed with directx-shader-compiler) and cache SPIR-V by source
  hash (memory first, a disk cache later).

## Vulkan design

- Vulkan 1.3 core: dynamic rendering (no render pass objects), synchronization2; VK_KHR_push_descriptor for the per-draw
  set (supported by RADV). SDL3 creates the surface (SDL_Vulkan_CreateSurface).
- Clip space: D3D's y up is kept in the shaders; the viewport is flipped (negative height), so winding stays as on D3D.
- Targets: one VkImage per guest surface (address + kind), colour B8G8R8A8_UNORM, depth D32_SFLOAT_S8_UINT (RADV has
  no D24S8). Created on first use, sized as the guest surface (640x960 for CS's back buffer).
- Textures: decoded with texture.cpp, uploaded through a staging ring, cached by address/format/palette with the
  dirty epoch the runtime keeps; BC1-3 uploaded compressed.
- Pipelines: keyed by vertex layout, shader hashes, blend, depth/stencil, raster state, topology, target formats.
- Frames in flight: 2, each with its command buffer, upload ring, fence; resources freed only after their frame's fence.
- Presentation: the back buffer target blitted to the swapchain image at 4:3 (letterboxed), FIFO present mode.

## Risks

1. **Depth precision**: CS relies on 24-bit integer depth (pretransformed z 0.5 units must compare <= text at 0,
   FINDINGS.md). D32F would keep 0.5/16777215 > 0 and bring back the menu artifact. Plan: quantise z to 24-bit steps in
   the vertex shader epilogue (string-level addition to the generated HLSL in the backend, as QuadHlsl does), verified
   against the reference renderer.
2. **Guest memory coherence**: the title may read or write render targets and textures with the CPU (save thumbnails,
   screenshots, dynamic textures). Needs the D3D12 renderer's signature/readback scheme; first milestones ignore it.
3. **Pixel centre conventions**: the generator already models the console's half-pixel and integer centres for D3D12;
   Vulkan's rasterisation rules match D3D's, so the same HLSL should give the same coverage. To be checked by image
   comparison against the reference renderer.
4. **Format coverage**: palettised P8, swizzled and linear formats go through DecodeLevel to BGRA8; YUY2 overlay later.
5. **Shader compile stalls**: DXC at draw time; mitigated by caching, later a disk cache.
6. **Windows parity**: the Vulkan backend is developed and tested on Linux (RX 9070 XT, RADV); Windows keeps D3D12.

## Milestones

1. **Title screen**: instance/device/swapchain through SDL3; Clear, one pretransformed textured quad (DXT1, blending,
   depth test), present at 4:3. Compared with the reference renderer's capture of the same frame.
2. **Menus**: vertex programs (text), multiple textures and formats, scissors, depth quantisation, many draws per frame;
   front end screens compared with reference captures.
3. **Live match at a playable rate**: transformed geometry, lightmaps/multitexture combiners, fog, alpha test, culling,
   indexed draws, frames in flight, pipeline and texture caches; frame time measured on de_airstrip.

Each milestone: a scenario run with `--renderer vulkan` beside the software renderer, captures compared, numbers
recorded in PROGRESS.md, committed.

## Code location

`src/vulkan/` in this repository (built into the Linux window front end, kept out of the Windows build), so the SDK
stays a set of small patches. It includes only runtime headers (state, vertex, texture, shaders, backend).

## Status

### Milestone 1: title screen (done 2026-10-19)

- src/vulkan/vk_backend.cpp, vk_shader.cpp; the window front end takes `--renderer vulkan` (or CSX_RENDERER=vulkan,
  kept across title relaunches). Built by tools/build_window.sh with third_party/Vulkan-Headers (v1.4.363, pinned;
  or the vulkan-headers package), -lvulkan, -ldxcompiler.
- RX 9070 XT, RADV (Mesa 26.2.5), API 1.4.354: instance/device/swapchain through SDL3, 640x960 colour and D32S8 depth
  targets, DXC SPIR-V at draw time, push descriptors, dynamic rendering, flipped viewport, 4:3 letterboxed blit.
- tests/scenarios/vk_title.txt: the presented back buffer saved after boot. Against the reference renderer's capture
  of the title (tools/compare_images.py): PSNR 40.5 dB, mean difference under 1 level, 0.9% of pixels over 8 levels,
  all on sharp edges in a column pattern (texture sampling phase, CPU float vs GPU sub-texel precision; which one the
  console matches is open, to settle against xemu).
- Depth quantisation (risk 1) was needed at once: without it the D32F depth brought back the menu artifact (shown in a
  run that had reached the menus). The Vulkan backend floors z to whole depth units in both vertex epilogues.
- Not yet: point sprites, cube/volume/depth textures, CopyRects, guest readback (Sync), gamma, visibility queries,
  overlay, border colours, the render thread.
- Test isolation: the window, when it took focus on the desktop, received host keystrokes that navigated the menus and
  made the title relaunch. CSX_TEST_INPUT_ONLY=1 (set by tools/drive.py for window runs) drops host keyboard, mouse and
  gamepad events; scenario keys carry a marker keyboard id.

### Milestone 2: menus (done 2026-10-19)

- tests/scenarios/vk_menus.txt drives the Vulkan build with SDL key events through title, notice, profile keyboard,
  main menu, Single Player and Map Selection, saving each presented frame; navigation is exact now that host input
  is ignored in test runs.
- Against the reference renderer's captures of the same screens (regress_boot_to_spawn snapshots), PSNR / pixels over
  8 levels: title 40.5 dB / 0.90%, keyboard 47.2 / 0.42%, main menu 45.0 / 0.11%, Single Player 46.6 / 0.11%,
  Map Selection 47.9 / 0.11%. No unimplemented Vulkan path was reached in the front end.
