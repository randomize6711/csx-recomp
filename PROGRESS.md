# Progress

## 2026-10-19 — Initial analysis

Done:
- Inspected the game directory (read-only), identified `default.xbe` and asset formats.
- Wrote `tools/xbe_info.py` (read-only XBE parser); reports for all four XBEs in `analysis/`.
- Cloned XboxXBERecomp @ 99078b1, Zydis v4.1.1, toml++ v3.4.0 to `third_party/`.
- Built `xbe_recomp` and `xbe_symbols` natively on Linux (clang 23) without CMake — `build/tools/`.
- Diffed CS kernel imports against the runtime: all 145 registered.
- Cross-build signature experiment: 130/238 functions, 6/12 data found (unverified candidates).

Environment:
- Present: clang/clang++ 23.1.3, lld, gcc 16.2, ninja 1.13.2, git, python 3.14, SDL3 3.4.18, Vulkan loader 1.4.363,
  glslc, wine 11.20, llvm tools.
- Missing: **cmake**, **Java + Ghidra**, **mingw-w64** (Windows cross builds), DXC (HLSL→SPIR-V for Vulkan), xxd.
  No passwordless sudo, so these need to be installed by the user.

Blockers:
- XDK 5659 has no signature/layout data in the framework (main blocker for Phase 3).
- Ghidra not installed (needed to verify symbols).
- The framework's game executable is Windows/D3D12-only; Linux needs new backends (planned, not blocking yet).

## 2026-10-19 — Toolchain complete, first headless boot

Done:
- User installed cmake, JDK 21, Ghidra 12.1.2, mingw-w64, DXC.
- Workspace restructured into the framework's game layout: `sdk/` submodule pinned at 99078b1, `config/`, `src/`,
  `xdk/5659/`, `assets/default.xbe` (read-only copy). `CSX_TOOLS_ONLY` + `linux-tools` preset build the tools via CMake.
- Ghidra project `build/ghidra/proj` from `xbe2pe.py` (flat PE with candidate symbols), auto-analysis 165 s.
- Recompilation survey: codegen 2 s, all 65 generated files compile, no reachable unimplemented instruction.
- Headless harness links and boots (`tools/iterate.sh`, ~40 s rebuild from scratch): through CRT, file system,
  XACT/DSOUND init to `Direct3D_CreateDevice`.
- Fixed/added: DSKERDAT dropped from code sections; APU sample counter (`src/common/apu_clock.cpp`);
  5659 layout for the effects image and XPP; `Direct3D_CreateDevice` [abi]; SDK patch for compiled-away arguments.
- Safety: the harness runs in a network namespace (`unshare -rn`) on a reflink copy of the game data.

Current blocker: 5659 D3D [data]/[layout] values and 15 LTCG [abi] maps (parallel analysis running), then the
~100 host-replaced functions the 5849 signatures missed.

## 2026-10-19 — Past Direct3D device creation and the UIX front end

- Merged proven 5659 D3D data/layout, 15 LTCG [abi] maps, 26 more D3D symbols (spot-checked independently).
- Runtime additions: D3DDevice_Reset (SDK patch 0002); offline XOnlineGetUsers (src/game/xonline.cpp).
- Diagnostics: [[hook]] on the fatal error screen and its 29 entry sites (src/game/diag_hooks.cpp).
- The title now creates the device, applies its default states, resets the device, initialises UIX and loads the UIX
  sound banks. Current blocker: XACT calls DirectSound buffer methods the runtime does not implement.

## 2026-10-19 — Title screen reached (headless, Linux)

- SDK patches 0003-0007: DSOUND 3D voice data/pitch/voice properties, D3D overlay, GPU time pointer, end-of-file read
  semantics, thread exit status. Config: XACT DSOUND methods, overlay, missed vtable method, stream vtable layout.
- Boot now runs: CRT -> file system -> XACT/DSOUND -> D3D device + Reset -> UIX -> cache unpack to Z: -> MGS/Valve/ESRB
  movies -> title screen ("Please press START to begin") -> attract movie after idle. No crash in 120 s runs.
- First frame captured with the reference renderer.
- Not yet: input, overlay compositing (movies invisible), any windowed/GPU presentation on Linux.

## 2026-10-19 — Main menu and an offline match (headless, Linux)

- Black REF=N captures explained (harness only applied clears on drawn frames; CS clears depth once) and fixed.
- Rendering fixes, each with evidence in FINDINGS.md: multisample 640x960 back buffer (0009), pretransformed super
  sample scale (0010), reference renderer near-w clip (0011), pretransformed z in depth units (0012).
- tools/drive.py scenario driver; tests/run_regression.sh with regress_boot_to_spawn (37 steps, ~90 s), shown to fail
  when patch 0007 is removed.
- Reached: notice, profile creation, main menu, Training, Single Player, Map Selection, title relaunch into
  de_airstrip, team selection, spawn with HUD/radar/buy menu; round timer runs and bots fight.
- Not yet: verified player control, overlay compositing, comparison against xemu, any windowed presentation.

## 2026-10-19 — Player control verified (headless)

- Memory dump facility and state search; player position, yaw and the Glock's shot counter located with evidence.
- regress_player_control: idle, run, turn, run along the new heading, fire three shots, all checked in game state.
- Harness: optional FPS pacing (0014), right stick / black / white / stick clicks in pad scripts (0013).
- Scenario driver: single presses with waits (re-press loops raced the 2 s draw measurements), 3-frame presses.

## 2026-10-19 — Linux window

- SDL3 window front end (src/linux, tools/build_window.sh), harness patch 0015 (HOST_OPT).
- Shows menus at 30 frames/s and gameplay at about 3 frames/s (reference renderer); keyboard, mouse (stick
  emulation), gamepads; fullscreen toggle; screenshots of the window output on request for tests.
- Not yet: audio output (null mixer), overlay (movies), usable in-game frame rate, raw mouse aim.

## 2026-10-19 — Vulkan milestone 1: title screen

- Plan (docs/vulkan_plan.md), shared draw helpers (SDK patch 0016), DXC SPIR-V verified, Vulkan backend renders the
  original title screen on the RX 9070 XT (RADV); 40.5 dB PSNR against the reference renderer.
- Regression 112/112 with the refactor; Windows-only sources syntax-checked (tools/check_windows.sh).
