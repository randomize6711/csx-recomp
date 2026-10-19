# Architecture

## Goal

A standalone native PC executable built by statically recompiling Counter-Strike: Xbox Edition's `default.xbe`
(x86 → C++), linked with a host implementation of the Xbox kernel and XDK APIs. The original XBE remains the
authoritative source of game behaviour; it is supplied by the user at build/run time and never committed.

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
 │ platform     Windows: D3D12 + XAudio2 (upstream)             │
 │              Linux:   Vulkan backend + SDL3 audio/window     │  new, ours
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
  build/gamedata       reflink copy of the game folder for runs; build/gen, build/harness, build/run
```

`assets/` (game files) and `generated/` (recompiled code) are gitignored: no game code or data in the repository.
