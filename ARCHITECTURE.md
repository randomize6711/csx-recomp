# Architecture

## Goal

A standalone native PC executable built by statically recompiling Counter-Strike: Xbox Edition's `default.xbe`
(x86 → C++), linked with a host implementation of the Xbox kernel and XDK APIs. The original XBE remains the
authoritative source of game behaviour; it is supplied by the user at build/run time and never committed.

## Direction: PC first (2026-10-23)

The port is developed as a native, modern PC game. **Compatibility with the original Xbox is not a requirement.**
Xbox hardware limits, networking protocols (system link, Live), memory limits, rendering constraints and engine
capacity limits are not kept only for compatibility with the console.

Decisions are weighed in this order:
1. Modern PC hardware and operating systems.
2. Performance, stability and memory safety.
3. A maintainable, extensible engine architecture.
4. Future PC online multiplayer.
5. Expanded gameplay and modding.
6. Modern rendering and graphical improvements.
7. The desirable Counter-Strike gameplay behaviour (not obsolete technical restrictions).

Working systems are not rewritten merely because they come from the Xbox. They are modernized when that gives a
concrete benefit or removes a relevant limit. The XBE stays the source of the game's behaviour (Goal). The other
rules (assets unmodified, SDK changes as patches, simulation timing untouched, each milestone's own requirements)
still apply unless a milestone changes them. Applied first in G8.2B (65 player slots, docs/player_slots_plan.md).

## Layers

```
 ┌──────────────────────────────────────────────────────────────┐
 │ generated/   recompiled game + statically linked XDK code    │  xbe_recomp output, never hand-edited
 │              (GoldSrc-Xbox engine, XACT, XMV, WMA, UIX ...)  │
 ├──────────────────────────────────────────────────────────────┤
 │ src/game/    CS-specific hooks (X86_FUNC overrides)          │  hand-written, minimal, documented
 │ src/pc/      PC enhancements (resolution, FOV, mouse, ...)   │  isolated from game logic
 ├──────────────────────────────────────────────────────────────┤
 │ sdk/runtime  kernel, D3D8 / DSOUND / XAPI / XNet HLE         │  XboxXBERecomp (upstream, unmodified
 │              gpu::Backend, audio, input, app shell            │  where possible)
 ├──────────────────────────────────────────────────────────────┤
 │ platform     Linux and Windows: Vulkan backend + SDL3        │  new, ours (Windows cross-built
 │              audio/window/input (src/linux, src/vulkan)      │  with Clang + MinGW-w64)
 └──────────────────────────────────────────────────────────────┘
```

## Decisions

1. **Framework: XboxXBERecomp.** It already covers function discovery, jump tables, x87/SSE/MMX, C++ EH, kernel,
   D3D8, DSOUND, XNet, system link, LTCG conventions, and has a Linux headless harness. Building our own would be
   far more work.
2. **XDK 5659 support is added as data, not code:** `xdk/5659/layout.toml` + `signatures.toml`, plus `[abi]` entries
   for LTCG register conventions in our `config/game.toml`. Every value is proven from CS code at a cited address,
   following upstream's convention.
3. **Upstream stays pristine.** The SDK is consumed as `sdk/` (submodule). Our additions (5659 data, Linux backends)
   live in our tree first. Generic fixes are kept as small patches suitable for upstream PRs.
4. **Linux strategy:** implement `gpu::Backend` with Vulkan (HLSL from `gpu/shaders.cpp` → SPIR-V via DXC), audio via
   SDL3, window/input via SDL3 (already used for pads). Bring-up and verification use the headless harness and its
   CPU reference renderer before any GPU work.
5. **Library code is recompiled, not reimplemented,** except where the runtime already provides HLE (D3D8, DSOUND,
   XAPI, XNet). XACT, XMV, WMA decoding and UIX run as recompiled code.
6. **Simulation timing is untouched.** High refresh rate work must decouple presentation from the game's own tick,
   never change the game's timestep. To be designed once the main loop is identified.

7. **SDK changes are patch files** (`patches/sdk/*.patch`, applied by `tools/apply_sdk_patches.sh`) so the submodule
   stays at an upstream commit. Current: `0001-abi-none-location` (`"none"` [abi] location for arguments LTCG compiled
   away). Workarounds that need no SDK edit are preferred (`-include unistd.h` for the harness build).
8. **Hardware registers the title reads directly** are modelled in `src/common` and started from `GameInfo::loaded`
   (first: APU XGSCNT, `apu_clock.cpp`).
9. **Unproven LTCG functions are left un-replaced** (symbol 0 + "pending" comment) rather than given guessed maps.
10. **One front end on both platforms.** Windows builds the Linux window build's sources (SDL3 + Vulkan + SDL audio)
   with Clang targeting MinGW-w64 from Linux (cmake/windows, tools/build_windows.sh); the generated code comes from
   the Linux recompiler run. The upstream D3D12 / XAudio2 app layer is not built (FINDINGS.md "Windows build").

## Workspace

```
csx-recomp/
  *.md                 project docs
  tools/               our scripts (xbe_info.py)
  analysis/            reports derived from the XBE (no game bytes)
  third_party/         upstream clones (gitignored): XboxXBERecomp, zydis, tomlplusplus
  build/               build outputs (gitignored)
  sdk/                 XboxXBERecomp submodule (pinned), patched in the working tree only
  patches/sdk/         our SDK patches
  config/game.toml     recompiler config (symbols, data, abi); includes xdk/5659/layout.toml
  xdk/5659/            5659 layout data we derive (later: signatures)
  src/common, src/game host code: game info, hardware models, hooks
  assets/default.xbe   read-only copy of the XBE (build input)
  out/build/linux-tools  tools built via CMake
  cmake/               Windows cross-build (toolchain file, CMakeLists, vulkan-1.def, launcher)
  build/windows        Windows build and its package (dist/); build/deps the downloaded SDL3 / DXC releases
  build/gamedata       reflink copy of the game folder for runs; build/gen, build/harness, build/run
```

`assets/` (game files) and `generated/` (recompiled code) are gitignored: no game code or data in the repository.
