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

## 2026-10-19 — Vulkan milestone 2: menus

- Front end screens through the Vulkan backend, 45-48 dB PSNR against the reference renderer (docs/vulkan_plan.md).

## 2026-10-19 — Models in game; Vulkan milestone 3 (partly)

- SDK patch 0017: vertex program memory (LoadVertexShaderProgram / LoadVertexShader / SelectVertexShader); studio
  models render in both renderers (they were garbage). Regression 112/112, player control numbers unchanged.
- Vulkan: render targets sampled as textures (the glow chain), shader caches (CPU recording 6.7 -> 0.68 ms per
  frame), GPU timestamps; measured in game on the RX 9070 XT: GPU 0.23 ms, recording 0.68 ms, present 0.08 ms per
  frame (medians); the match runs at 72 frames/s, the title's own cap (immediate present gives the same).
- Straight into a match: launch fixtures (tests/launch) and drive.py --launch; 45 s to a moving player.
- tools/apply_sdk_patches.sh: files a patch creates are removed before re-applying (the reset left them behind and
  patch 0016 then failed to apply).
- Diagnostics: write watch (CSX_WATCH), Vulkan target shots, per-draw step shots, magenta undrawn areas, frame log.
- Open: a band of the world is not submitted at the de_airstrip spawn (both renderers; FINDINGS.md).

## 2026-10-19 — The world renders (both renderers)

- Cause of the missing world found and fixed (FINDINGS.md "In-game rendering"): recorded push buffers (SDK patch 0019:
  BeginPushBuffer / EndPushBuffer / RunPushBuffer bound with proofs), inline arrays and pushed commands drawn in
  order (patch 0018, with the program reload after select). The de_airstrip spawn shows the jungle, tunnel, path and
  hills; the first person weapon and player models are back.
- Software vs Vulkan at the same moment (vk_compare_direct vs match_direct, timer 4:55): walls/tunnel 39.7 dB,
  sky/hills 31.1 dB; differences: the reference renderer's black near triangles, bots and rain, lower ground tint.
- Vulkan in game: ~1150 draws a frame, median 72 frames/s (the game's fps_max cap), minimum 64 over a run; GPU
  0.75 ms, RunPushBuffer replay 1.6 ms a frame including its 576 draws.
- Window key requests take SDL names with underscores (Left_Ctrl): scenarios had sent an unknown "LCtrl", so the buy
  menu never closed and W did not walk; vk_match_direct now checks the walk in game state (716 units).
- Regression 112/112, player control numbers unchanged; menus unchanged (40.5-47.9 dB).

## 2026-10-19 — Audio output (Linux)

- SDL3 output for the window build (src/linux/sdl_audio.cpp), null output kept for headless and test runs
  (CSX_AUDIO=null by default in drive.py window runs); capture to WAV (CSX_AUDIO_WAV) for checks.
- The in-match mixer output was centred and partly missing: XACT's 3D sounds play through MIXIN buffers the runtime
  did not route. SDK patch 0020: SetOutputBuffer routing with the destination's levels, pan and Doppler. Direction
  verified against game geometry (FINDINGS.md "Audio").
- build/zcache (tools/make_zcache.sh) seeds Z: for direct starts so the HD sound banks are there.
- tests/audio_check.sh + tools/check_audio.py: not silent, placed to a side, under 0.5% full scale, finite.
- Regression 112/112 (the turn measured 129.2 degrees in one run, 129.3 before: frame timing).

## 2026-10-19 — Native mouse look (Linux window)

- The title's view angles found (0x552C10 yaw, 0x552C14 pitch) and a hook in its look routine adds the mouse's
  degrees before its pitch clamp (src/game/mouse_look.cpp, config [[hook]] at 0x19E4EB with the proof).
- Window: captured relative motion x 0.022 x CSX_MOUSE_SENS (default 3), CSX_MOUSE_INVERT, CSX_MOUSE=stick for the
  old stick emulation; click captures, F1 / focus loss releases (unchanged).
- Tests: regress_mouse_look (headless, direct match: exact yaw/pitch changes, clamp, no acceleration, stick intact)
  in run_regression.sh, which now passes a scenario's --launch fixture; vk_match_direct checks the window's mouse.
- Tools: state lines carry the view yaw/pitch; drive.py steps look, mouse, poke, pokefile and expect yaw/pitch.
- Regression 257/257 (the 112 earlier checks unchanged).

## 2026-10-19 — Complete offline rounds on three maps

- Game server log in the host log (src/game/game_log.cpp, [[hook]] at 0x14BC80): rounds, kills, objectives, scores.
- Map fixtures de_dust_beginner and cs_italy_beginner; tests/scenarios/rounds_direct.txt; drive.py count step.
- de_airstrip, de_dust, cs_italy: four complete rounds each with movement, shots, deaths and respawns, bot combat,
  bomb plant/detonation, hostage rescue, time expiry (FINDINGS.md "Rounds").

## 2026-10-19 — Video playback (overlay)

- SDK patch 0021: the XMV movies' overlay composited (reference captures, software window, Vulkan); the boot intro
  is visible with its sound. Tests: intro_overlay.txt (headless), vk_intro.txt (window). Regression 257/257.

## 2026-10-19 — Rendering accuracy

- Gamma: the title's ramp (x1.67 at team join) applied in the Vulkan backend (LUT pass at present), the software
  window and the reference captures (SDK patch 0022); snaps keep the ramp in effect when asked for, as the slow
  reference frames start the title's gamma fade.
- Vulkan Sync: render targets read back into guest memory (the front end's PersistDisplay).
- Reference renderer near-plane clipping fixed (SDK patch 0023): no black ground triangles at the spawn.
- Vulkan against reference, late in-match frame: 20.4 -> 25.1 dB PSNR, region means within 1-5 levels; the fog
  difference was gamma and the capture artifact. Point sprites and CopyRects never reached by CS.
- Soak: Vulkan window, de_airstrip, 39.5 minutes, nine rounds, no errors (FINDINGS.md "Rounds").
- Tests: compare_direct.txt (headless twin of vk_compare_direct.txt, adds in_match_late).

## 2026-10-19 — Windows build (cross-compiled)

- Audit: the SDK runtime is Windows first; the GNU ABI (Clang + MinGW-w64) has no blocker: weak overrides, C++
  exceptions, threads checked; no MSVC-only construct in the built sources (FINDINGS.md "Windows build").
- cmake/toolchains/x86_64-w64-mingw32-clang.cmake, cmake/windows/CMakeLists.txt (the Linux window build's sources),
  tools/fetch_windows_deps.sh (SDL3 3.4.18, DXC 1.10.2605.37, digests checked), tools/build_windows.sh ->
  build/windows/dist (CounterStrike.exe, SDL3.dll, dxcompiler.dll, run_game.bat, README.txt, licenses).
- Source changes: DXC header / CComPtr on Windows, setenv, a strchr type, the Windows relaunch handover.
- Under Wine 11: intro, front end, relaunch into the match, direct match with movement and exact mouse look, audio
  through WASAPI. Not run on Windows itself.
- Tests: tests/windows_check.sh (build, PE and import checks, override parity with Linux, Wine scenarios);
  drive.py --wine. Linux: window build vk_match_direct 32/32.

## 2026-10-19 — 3D sound distance attenuation (Linux)

- Played-in report: distant gunshots as loud as near ones. XACT puts the distance level its 3D calculator computes
  (l3DDistanceVolume) on the source buffer it routes into the MIXIN buffer; the routed mix (patch 0020) ignored the
  source's own 3D block. SDK patch 0024 applies the source's distance / cone term, as the voice update does for every
  bin (FINDINGS.md "Audio").
- Evidence and tests: src/game/audio3d_probe.cpp ([[hook]] 0x2FCB91, CSX_AUDIO_LOG3D=1) logs each 3D update's distance
  and level; XBE_LOG_3D=1 logs the mixer gain; tools/check_audio3d.py, tests/audio3d_check.sh: on de_airstrip,
  de_dust and cs_italy the level falls with distance in every sound class, reaches the mixer, far voices play 16-18.5
  dB below near ones. regress_audio3d.txt in run_regression.sh (which now passes --env from scenario headers).
- Regression 272/272 (257 plus the new scenario's 15 checks).

## 2026-10-19 — Character shadows and HUD colours

- Played-in report: white circles under players. CS's GL layer sets the shadow's colour (black) as the diffuse slot's
  current value before an inline array of positions and texture coordinates; the runtime fed inline / immediate
  primitives only the slots they sent per vertex. SDK patch 0025 feeds the slots' current values (as the hardware
  does). Shadows are dark; the HUD numbers, crosshair, icons and texts take the game's colours and fades instead of
  white (FINDINGS.md "Shadows and HUD colours").
- Tests: regress_hud_colour.txt (resting HUD digits' luminance); drive.py step expect region. Regression 286/286.

## 2026-10-19 — Lamp glows (investigation)

- Glow and lamp sprites are the engine's additive quads; their fade and colour came through the current colour that
  patch 0025 restored (they were full white). Sizes are the game's; depth testing works. Not confirmed visually at a
  lamp. Bright-area wash-out (sky) is identical in both renderers: the game's gamma ramp and glow pass
  (FINDINGS.md "Lamp glows and bright areas").

## 2026-10-19 — Decals (de_airstrip A / B signs)

- The flickering signs are infodecal decals drawn with polygon offset (bias -5000 depth units) that neither renderer
  applied: z-fighting with their walls. SDK patch 0026 (reference renderer) and the Vulkan backend (float depth bias,
  VK_EXT_depth_bias_control) apply it (FINDINGS.md "Decals").
- Tests: regress_decals.txt; drive.py step expect darkfrac. Regression 321/321; Vulkan vk_match_direct 32/32.

## 2026-10-19 — PC Options in the original menus (proof of concept)

- The front end's menus are text (FrontEndUI.txt) the game parses and draws with its own UI code; screen commands are
  console commands (FINDINGS.md "Original menus and PC Options").
- src/game/pc_menu.cpp adds, at load, a "PC Options" row to Options and a SuperLauncher_PCOptions screen in the
  game's own format; its commands (pc_options_load / pc_options_save) are served by a hook in Cmd_ExecuteString;
  src/common/pc_settings.cpp keeps pc_settings.ini. First setting: Mouse Sensitivity, used by the native mouse look.
- Tests: regress_pc_options.txt; drive.py --pc.

## 2026-10-19 — PC Options: invert, display mode, master volume

- Invert Mouse, Display Mode (windowed / fullscreen, applied after the first frame for Wayland tiling compositors;
  F11 saves it) and Master Volume join Mouse Sensitivity in Options > PC Options; Window Size was not verifiable on a
  tiling compositor and is not offered.
- The Vulkan swapchain now follows the window's size on Wayland (fullscreen, resizes).
- Regression 384/384 (regress_pc_options also switches Invert Mouse on and checks every row's save).

## 2026-10-19 — PC Options in the pause menu

- InGameUI.txt gets the same row and an InGame_PCOptions screen in the pause menu's own style; settings change during
  a match (mouse sensitivity checked in the Vulkan window). Test: regress_pc_options_ingame.txt. Regression 426/426.

## 2026-10-19 — PC key and mouse bindings

- The engine's GoldSrc key system serves PC keys: keys, mouse buttons and the wheel are engine keys while playing
  (src/game/pc_keys.cpp hands them to Key_Event), bound with the engine's bind from Options > PC Controls (27 actions,
  PC defaults, restore defaults, duplicates resolved); menus keep the keyboard as the pad (FINDINGS.md "PC key
  bindings").
- pc_menu.cpp is now a registration of PC screens (PC Options, PC Controls) from which both menus' text is made.
- Tests: regress_pc_bindings.txt; drive.py steps pckey, click, wheel, expect rose.

## 2026-10-19 — Field of view

- The view's FOV path traced (scr_fov_value, CalcFovY, MYgluPerspective, R_SetFrustum; r_anamorphic found, see
  FINDINGS.md "FOV and widescreen"). Options > PC Options > Field of View (90 original ... 120) replaces the unzoomed
  FOV; zoom unchanged; projection and culling together; both renderers. Test: regress_pc_fov.txt.

