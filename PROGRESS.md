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

## 2026-10-20 — Widescreen

- Options > PC Options > Aspect Ratio (Automatic = the display's, 4:3, 16:10, 16:9, 21:9; at the next start): a wider
  frame at 480 lines (back buffer, engine window rectangle), Hor+ FOV for the projection and the culling planes (the
  planes were built on the VGUI panel's 640x480: edge geometry was culled at 21:9), zoom magnification as at 4:3.
- The original UI unstretched in the middle 640 columns: the 2D projection (engine 2D, VGUI paint), the screen-pixel
  paths (screen rectangles: scope, backdrops, loading screen; text renderer; video overlay), the client's screen width
  kept at 640; full-screen backdrops continue to the edges, the scope's surround is black. Both window renderers
  present at the frame's aspect. FINDINGS.md "Widescreen".
- Tests: regress_pc_widescreen.txt (16:9), regress_pc_widescreen_ultrawide.txt (21:9, FOV 110).

## 2026-10-20 — Developer console

- The Xbox build's own console found intact for output (200 line ring, Con_Draw with the game's font, toggleconsole,
  consoleup / consoledown, consoleexecute) but without its line editor, history and completion; restored on it:
  typing, Enter, history, Tab completion, scrolling, Escape, drawn over menus too (FINDINGS.md "Developer console").
- PC Controls > Toggle Developer Console (default the key left of 1 on any layout); keyboard and mouse kept from the
  game while it is open, mouse capture restored, the pad unchanged.
- Tests: regress_pc_console.txt, regress_pc_console_wide.txt (21:9); vk_console.txt (Vulkan window); drive.py step
  `type TEXT`, pckey lines `con TEXT` / `conkey NAME`.

## 2026-10-20 — Native resolution

- PC Options > Resolution (Native, the display's modes from SDL3, a custom WxH, Original): the Vulkan renderer draws
  every render target at the chosen size per axis, the game keeping its 640x480 layout; applied at the next frame,
  fallbacks for invalid or failing sizes (FINDINGS.md "Native resolution"). PC Options gained X Restore Defaults.
- The window takes the harness's `cmd` pad presses (tests drive the pad in the Vulkan window as headless).
- Tests: vk_resolution.txt (2560x1440 targets, a change from the pause menu).

## 2026-10-20 — Mouse in the menus

- The original menus driven by the mouse: hover selects through the table's own move, clicks and the wheel become
  the game's pad presses, arrows on settings, double click accepting, the help bar's buttons, the buy pies by angle,
  right button back; the mouse captured in play and free in menus and the console (FINDINGS.md "Mouse in the menus").
- Hover plays the main menus' HighlightSound through the game's menu sound player, as their stick pieces do.
- Tests: regress_pc_menu_mouse.txt, regress_pc_menu_mouse_fe.txt, vk_menu_mouse.txt; drive.py `point X Y`, pckey
  `pointer X Y`, `mclick N`, `mwheel N`.

## 2026-10-20 — Mouse validation, the profile name, press-a-key rebinding

- Mouse in the menus checked in the Vulkan window with real SDL events in a headless gamescope
  (tools/run_window_tests.py): cursor positions at 4:3 and 16:9 windowed, 21:9 letterboxed and 4:3 pillarboxed full
  screen from raw window pixels; hover sounds found in the audio capture as strongly as the game's own d-pad plays
  (tools/check_menu_audio.py, a menu-sound log hook at 0xC5A00). Hover plays the HighlightSound only where the
  screen's own moves do.
- The profile name: the virtual keyboard by pointer and click, a click on the name field to type with the PC
  keyboard into the game's edit box (its insert, limits and sounds; Å/Ä/Ö), Backspace, the cursor, Enter as Done.
- PC Controls: Enter or a click on a key name waits ("Press a key" in the game's own row), the next key, mouse button
  or wheel notch is the binding; Escape / the pad's B cancel; keys by position for every layout with the layout's
  names (Swedish Å Ö Ä §), new keys ' \ < and keypad . /; the pad's list stepping, Accept / Cancel / Reset and the
  duplicate rule as before (FINDINGS.md "Press-a-key rebinding").
- The lists' stepping no longer skips keys in PC Controls (the original list's enable bytes cover 0x27 choices; hooks
  take the rest as enabled): the old "steps over -" was every fourth key from there.
- Tests: regress_pc_profile_name.txt, regress_pc_bind_capture.txt (with the relaunch into a match),
  vk_menu_mouse_fe.txt, vk_menu_pointer_*.txt, vk_bind_capture.txt; drive.py `pointat`, `expect count`,
  `expect menuaudio`, keys as NAME#SCANCODE.

## 2026-10-20 — Play-test bugs: walking right, grenades, the spectator's camera

- Walking right: WASD were the pad's stick in the menus and PC Controls' stick pieces stepped the selected row's key
  (D turned Move Right into E); on PC Controls the keyboard is no longer the stick. vk_movement.txt walks every
  direction in the window, after the console and the pause menu.
- Grenades: the client's slot selection left a slot with several grenades pending for a confirmation this build lacks;
  with fast switch the choice is now selected at once and 4 steps through the grenades (regress_pc_grenades.txt; the
  weapon in hand is in the state lines).
- Spectator and scoreboard: screens with ControlPassThrough are play for the PC input, so the mouse turns the
  spectator's camera and walking goes on while Tab holds the scoreboard (vk_spectator.txt).

## 2026-10-20 — Play-test bugs: frame rate, PC Settings minimizing, menu pointer, loading screens

- Frame rate: PC Options > Frame Rate Limit (30 Original to 240) sets the window's pacing and fps_max; the movement
  command's dropped fraction of a millisecond is carried above 30, so speed (250 units/s) and jumps (45) are the same
  at every rate (regress_pc_frame_rate.txt at 144; SDK patch 0027).
- PC Settings no longer re-apply Display Mode on every save (a window made full screen by the compositor stayed full
  screen; vk_display_mode.txt).
- Menu pointer: tables with a header (team selection, the confirmations) are hit one row lower, as drawn
  (vk_menu_pointer_pause.txt, pixel checks; the confirmations' Yes works by mouse).
- Loading screens: a multisampled back buffer is no longer rotated at Swap (SDK patch 0028); vk_loading.txt checks
  four loading transitions with bursts of frames.

## 2026-10-21 — VSync, the 120 frames per second limit

- The 120 was VSync: the swapchain always presented with FIFO and the display runs at 120 Hz; every frame waited
  2-3 ms for it (now measured by the window's "frame timing" line: frames, presents, intervals, time blocked, the
  limiter's sleep (SDK patch 0029), present mode, refresh). Without VSync a match runs at 170-235 frames per second
  on this machine: CPU-bound on the one game thread (renderer recording, client, server, bots), not the GPU.
- PC Options > VSync (On by default, pc_settings.ini "vsync"): FIFO, or IMMEDIATE (MAILBOX when not offered) for Off;
  a change makes the swapchain again at the next present, the window untouched. The software renderer sets its SDL
  renderer's vsync (only the window's presents).
- Frame Rate Limit and VSync combine as expected; movement, jumps, firing, bot rounds and round timing checked at
  the new combinations and at 240 (vk_vsync.txt, vk_vsync_play.txt). No Unlimited choice (nothing above 240 can be
  verified in a match here).

## 2026-10-21 — Raw mouse input

- Traced the mouse path: SDL3 on Hyprland runs native Wayland, relative mode uses the relative pointer protocol and
  SDL takes its unaccelerated deltas; the game adds counts x 0.022 x sensitivity to the view with no curve or frame
  time (exact turns at 30-240 frames per second). The one defect: the Vulkan window loop read events every 5 ms, so
  at 190-240 frames per second the turn per frame wavered up to twice its mean; the loop now waits on events (spread
  0.25 -> 0.10).
- PC Options > Raw Mouse Input (On by default, pc_settings.ini "mouse_raw"): On the device's counts, Off the
  desktop's pointer motion (SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE), applied at once; the input path is logged.
- vk_mouse_raw.txt: motion streams from an SDL timer thread at several rates and frame rates, sensitivity, invert,
  full screen, menus and console, the setting from PC Options.

## 2026-10-21 — High pixel density

- The window asks SDL for high pixel density: on the 1.6x-scaled desktop the swapchain is the display's 3840x2160
  in full screen (it was the logical 2400x1350, enlarged by Hyprland), Native and the Resolution list are in pixels,
  the pointer and menus stay in logical units (desk_hidpi.txt on the desktop; window tests unchanged).

## 2026-10-21 — Graphics fidelity audit (G1)

- CSX_VK_AUDIT in the Vulkan backend logs what the title draws with (states, textures, passes into small targets,
  additive draws, gamma ramp changes) and saves the glow and measurement targets; vk_audit_effects.txt and its
  headless twin take the same shots in empty matches (new no-bot launch fixtures for de_dust2, de_aztec, de_inferno).
- 9 maps in both renderers: fog, shadows, glows, decals, smoke, explosions, flashbang, blending, filtering and the
  bloom chain agree; the bloom adds little at the spawns by the title's design (its bright pass keeps back buffer
  values above 0.5).
- Found: the dynamic gamma never adapts in Vulkan (its 40x60 LIN_L8 measurement is read by the CPU through an
  inlined lock and never written back: the ramp freezes at its brightest at team join, the picture flat), and sinks
  to black in the reference renderer; de_aztec's water reads a cube texture Vulkan does not bind; the bloom's
  GAUSSIANCUBIC downsample is drawn linear. Muzzle flash and dynamic lights uncertain. Order and toggles in
  FINDINGS.md; G2 starts with the dynamic gamma.

## 2026-10-21 — Dynamic gamma (G2)

- The reference renderer's ramp sank to black because it stored the title's 8 bit luminance target as RGB565 (noise
  to the histogram) and, headless, measured a cleared target on frames it does not draw: fixed (SDK patch 0030); it
  now agrees with Vulkan.
- Vulkan reads the measurement back late, as a NOOVERWRITE lock: a hook where the title reads it (0xBE777) calls
  d3d::ReadLate (SDK patch 0031), the backend copies the target after it is drawn into a small host buffer and writes
  the newest finished copy to guest memory, never waiting. The ramp follows the scene, settles and stays, at 30, 120
  and 240 frames per second, at no measurable cost; de_dust2 and de_inferno are no longer flat and washed out.
- PC Options > Auto Exposure (On by default) is the title's cl_dynamicgamma; Off shows its identity ramp.
- Tests: vk_auto_exposure.txt, vk_auto_exposure_rates.txt, regress_dynamic_gamma.txt; drive.py exposure steps and
  `expect quiet`.
- Found: the flashbang's white screen fade is not drawn in either renderer (next).

## 2026-10-21 — Flashbang white-out (G3)

- Traced the effect through the title: the server's RadiusFlash (distance falloff over 1500 units, line of sight,
  facing: the Xbox's own multipliers) sends a white ScreenFade; the client holds and fades it by its clock and draws
  it every frame as a full-screen quad through its GL layer (depth test off, glOrtho at GL depth -1).
- Missing in both renderers for two runtime reasons: the GL layer sets ZEnable through an inlined copy of the
  library's setter (the runtime kept depth testing on; SDK patch 0032 reads it from the library's table), and the
  console culls the fixed function at the viewport's MinZ to MaxZ (-1 to 1 here) where both renderers culled at 0 to
  1 (SDK patch 0033; Vulkan through VK_EXT_depth_range_unrestricted).
- The white-out holds and fades for the time it was sent with at 30, 60, 120, 144 and 240 frames per second, VSync on
  and off; the exposure comes back after it, Auto Exposure On or Off.
- CSX_LOG_FADE=1 logs the fade (src/game/screen_fade.cpp). Tests: vk_flashbang.txt, vk_flashbang_rates.txt,
  regress_flashbang.txt; drive.py fade steps and `expect regiondiff`.
- Headless suite (22) and window suite (20) pass. drive.py's `exposure` and `expect quiet` now read the log before
  marking its end (vk_auto_exposure.txt had read a 12 s old measurement after a sleep).

## 2026-10-21 — Flashbang over the whole screen (G3)

- Play-test at 16:9 (3840x2160 fullscreen): the white-out left the right quarter of the screen uncovered. R_PolyBlend
  draws the fade over the GL size inside the VGUI paint, whose 640x480 root panel it is; on a wider frame that is the
  left 640 columns. PcWideFade (src/game/pc_wide.cpp) gives it the frame's width while it draws; 4:3 unchanged.
- Checked in Vulkan at 4:3, 16:10, 16:9 (fullscreen, 4K internal) and 21:9 and in the reference renderer at 4:3, 16:9
  and 21:9: drive.py `expect coverage` (nine patches touching the frame's edges, full in the hold, even in the fade).
- Headless suite 24 of 24 (regress_flashbang_wide, _ultrawide added), window suite 24 of 24 (vk_flashbang_cover_43,
  _1610, _169, _219 added; vk_flashbang_rates passed on a rerun after a buy that did not open).
- Found: the bloom's last pass keeps a scissor at the top left (TODO.md, for the bloom work).

## 2026-10-22 — de_aztec's water (G4)

- The water's alpha is a Fresnel term from a 32x32 cube texture on stage 0, sampled along eye - P. The title's D3D
  sends stage 0 in the cube mode for a cube texture (its lazy stage setup at 0x30F0B1), as the SDK's pixel key already
  does; the reference renderer sampled it right (the audit's "first face" was a misreading). Vulkan bound no cube
  texture: the water was 0.302 opaque at every angle. It now binds cube textures as Vulkan cubes (six layers, the
  shared decode). The cube is not a unit normalisation cube (texel V |V_major| / |V|^2), which the tests take in.
- tools/drive.py `setview` (a fixed camera over the river, test only), `expect probealpha` and `expect chroma`; tests
  regress_water(_wide, _ultrawide) and vk_water_43/169/219.
- Headless 27 (regress_flashbang's buy failed once, passed on rerun), window 27/27. That buy failure is the title's
  buy-message race with the HUD's settings check and the tests' hold (FINDINGS.md "Buy menu taps"; fixed in the tests).
- Performance before and after the fix (60, 120, 240 limits): GPU, CPU recording, present and pacing the same within
  noise; the cube is 24 KiB, uploaded once; no new synchronisation or readback. Validation: no new messages (five
  older kinds recorded in FINDINGS.md "Water").

## 2026-10-22 — Bloom (G5)

- Test harness first (FINDINGS.md "Buy menu taps"): `pckey tap` presses +buymenu after the HUD's settings check and
  releases it on the steady clock; regress_flashbang 20 of 20, the buy tests and the window flashbang tests repeated.
- The bloom traced (FINDINGS.md "Bloom"): bright pass 2 t0 - 1 from the back buffer through GAUSSIANCUBIC, two blurs,
  composite ONE / ONE with cl_hdrbloom; each map's light config sets the intensity (0.5 on de_aztec).
- SDK patch 0035: GAUSSIANCUBIC as the 3x3 Gaussian (xemu's model of the NV2A's convolution filter) in both renderers.
- SDK patch 0034: SetRenderTarget clears the scissors as the XDK's does (through SetViewport); the composite covers the
  whole frame instead of the top-left sixteenth; the corner seam under a flashbang's fade is gone.
- PC Options > Bloom (On / Off, default On) through cl_hdrbloom, kept Off after each map's light config.
- Tests regress_bloom, vk_bloom_43/169/219 (`expect seam`). Headless all passed, window 31 of 31 (vk_display_mode's
  clicks follow the twelfth row). Performance the same within noise; validation no new messages.
- Found, not changed: Auto Exposure Off at launch only holds from the first setting change (TODO.md).

## 2026-10-22 — Vulkan validation, Auto Exposure at the start (G6)

- Auto Exposure Off saved now holds from the map's first frame (FINDINGS.md "Auto Exposure at the start"): each map's
  light config set cl_dynamicgamma 1 after Off was applied; the hook after the config takes it back with the identity
  ramp. On is the title's own behaviour; the adaptation is unchanged. Tests regress_ae_startup (fails on G5) and
  vk_ae_maps (front end, relaunch, Change Map); the Bloom window tests start with Off.
- Vulkan validation (FINDINGS.md "Vulkan validation"): four renderer fixes, a barrier between two transfer writes to
  one image (Present, ComposeOverlay), images freed between frames destroyed after the next frame's fence, no view for
  a transfer-only image, shaderDemoteToHelperInvocation enabled. Left: gamescope's WSI layer's messages (external) and
  the OutputNotConsumed performance warning.
- The water tests' chroma bounds re-measured at the identity ramp (they had been taken under the ramp that kept
  running); drive.py waits for a log line's end (vk_loading's one hang); the frame timing line logs the 95th
  percentile.
- Tests: headless 30 of 30, window 32 of 32. Performance against G5 the same within noise at 60, 120 and 240;
  memory 0.6 to 2 MB lower.

## 2026-10-23 — Weapon over the HUD (G6.1)

- At a wide FOV (120) the first-person weapon hid parts of the money, ammunition and timer in Vulkan (FINDINGS.md
  "Weapon over the HUD"): the cull range below 0 (since G3) kept the weapon's parts 2 to 4 units from the eye at a
  negative depth in the float buffer, and the HUD, depth tested at 0, failed against them. The console's integer
  buffer and the reference renderer store them at 0. Fix: depth clamped to 0 to 1 (VK_EXT_depth_clamp_control's user
  range, clipping kept by VK_EXT_depth_clip_enable). The weapon, its FOV, placement and the HUD's layout unchanged.
- The radar's blue circle and de_airstrip's grey mist rectangles: the title's own, the same in both renderers.
- Tests vk_viewmodel_hud_43, 169 and 219 (drive.py `expect bluefrac`; they fail on G6). Headless 30 of 30, window
  35 of 35; performance the same within noise at 60, 120 and 240; validation no new messages.

## 2026-10-23 — Xbox Graphical Effects (G7)

- PC Options > Xbox Graphical Effects (On / Off, default On; pc_settings.ini "xbox_effects"), above Auto Exposure and
  Bloom: the master over the title's two optional post passes, Bloom (cl_hdrbloom) and Auto Exposure
  (cl_dynamicgamma), found to be the only optional effects (FINDINGS.md "Xbox Graphical Effects": the rest is the
  title's content, its own shader terms, gameplay or rendering fixes). What takes effect is master AND setting
  (pc::XboxEffect); the saved Bloom and Auto Exposure are never rewritten; map intensities and console values behave
  as with the individual settings.
- CSX_LOG_EFFECTS=1 and drive.py `expect effects`; tests regress_xbox_effects (all eight combinations),
  vk_xbox_effects_43/169/219 (persistence through relaunches, Change Map, the menu row, console values, the HUD over
  the weapon and the flashbang's white-out with the master Off). The menu tests follow the new row.
- Headless 31 of 31, window 38 of 38; performance the same as G6.1 within noise (master Off 0.03 to 0.05 ms less GPU
  time); validation no new messages.

## 2026-10-23 — Player collision (G8.1)

- Custom Game > Player Collision (On / Off, default On): a tenth row in the single player match's Custom Options, its
  "+pc_player_collision 0|1" on the match's command line as the other rows' choices (FINDINGS.md "Player
  collision"). Bodies meet only in pm_shared's PM_Move (0x1A5B20), the one movement code of the server's players
  and bots and of the client's prediction, against pmove's physents; Off leaves the other players out of that list
  at PM_Move's entry (src/game/pc_collision.cpp). Shots, grenades, damage, hitboxes, the world, doors and hostages
  unchanged. Fixed for the match from its start (refused once a player has moved); kept by the game's own Custom
  Options memory and through Change Map. Not offered for system link or Live (untested; a remote client's
  prediction would not know it).
- Probe and drive.py: place, players, expect apart / bodymoved / bodyz / health / healthdrop / spawnapart; fixtures
  de_aztec_2bots and de_aztec_2bots_nocollide; tests regress_player_collision_on/_off and vk_player_collision_43/169/
  219.
- Headless 33 of 33 (regress_pc_bindings once flaky, passing alone), window 41 of 41; performance as G7 within noise;
  validation no new messages.

## 2026-10-23 — Bot count, part one: 16 bots in the 17 slots (G8.2)

- Custom Game > Number of Bots "0 bots" to "16 bots" (default still 7; the original offered 1 to 11), a help line
  naming the 17 player slots; the screen's one-team cap (8) redone for the longer list by a hook at 0xE1D2B
  (FINDINGS.md "Bot count"). The bot manager's own rules unchanged: full slots and full teams (a team is full at its
  map's spawn points, 8 / 8 maps take 15 bots with the player).
- CSX_LOG_SERVERTIME=1 (server frame and player move times); probe `spawns`, drive.py playercount / bodiesmoved /
  teams; tests regress_bots_* (7, 11, 16, 24 asked on de_aztec; 16 on de_dust2 with collision On and Off) and
  vk_bot_count_43/169/219 (the list, 17 players, the scoreboard at FOV 120, the choice kept).
- Headless 39 of 39, window 44 of 44; 16 bots cost about 0.3 ms of server time a frame; validation no new messages.
- Past 17 slots (the goal: 64 bots and the player): investigated, not implemented. 17 is the original game's
  MAX_CLIENTS in the engine, the HUD and the voice manager (the recompilation adds no limit); the per-player tables,
  svc_pings' 5-bit slot and the maps' spawn points have to change. Plan in docs/player_slots_plan.md, evidence in
  analysis/player_slots_*.md; waiting for approval.

## 2026-10-23 — G8.2B checkpoint 1: tooling for 65 slots, guest memory

Branch g8.2b (from g8.2). Nothing in the game's behaviour changes: no player table moved, no count raised.
- Recompiler `[[patch]]` (SDK patch 0036): build-time operand patches checked against the exact image, recorded in
  patches.txt; `--describe`, `--listing`. config/game.toml now names the image's sha256. tests/recomp_patches.py.
- Player slot audit: tools/slot_audit.py, config/player_slots.toml, analysis/player_slots_inventory.json (20 tables,
  76 sites, candidates, the frame ring's register trace, unresolved list). tests/slot_audit_test.py.
- Guest tables (config/guest_tables.toml, tools/guest_tables.py, src/common/guest_tables.cpp): fixed-address tables
  with guard pages and fault reports, mapped before the title starts (none yet). CSX_LOG_MEMORY=1 (SDK patch 0037).
  tests/guest_tables_test.py.
- Memory: analysis/memory_architecture.md, docs/memory_modernization.md; the 64 MB is only the contiguous pool, the
  heap (37 MB with 16 bots) grows in the 2 GB user range.
- Tests: the three tool suites pass; headless regress_boot_to_spawn, regress_player_control,
  regress_player_collision_on, regress_bots_aztec_16 pass with the new start-up path.
- Next: checkpoint 2 (docs/player_slots_plan.md section 7), after approval.

## 2026-10-23 — G8.2B checkpoint 2: 65 player slots

Branch g8.2b. The engine, client, HUD and game DLL hold 65 universal player slots; 64 bots and the player play a
match. Waiting for approval (checkpoint 3: the menu's Number of Bots to 64, the window tests, benchmarks at 16 / 32 /
64). Commits: cec1f95 (tools/slot_patches.py, guest table texts), 950c5ac (engine server and client, protocol 47),
0623f1a (HUD, game DLL, teams past spawn points, the studio renderer's array found by a crash).
- How it is made: config/player_slots.toml (relocated tables with their reviewed end pointers / aliases / other
  objects, [[change]] constants) + config/guest_tables.toml -> tools/slot_patches.py -> config/player_slots_patches.toml
  (401 operand patches, included by game.toml); hooks and host functions in src/game/player_slots.cpp (engine),
  player_slots_hud.cpp (HUD, client voice, StatShot handler), player_slots_game.cpp (voice manager, StatShot, trigger_hurt,
  team sizes). Regenerate after any config change: python3 -I tools/guest_tables.py && python3 -I tools/slot_patches.py
  (then xbe_recomp and the build); tests/slot_audit_test.py checks the file matches its configs and every relocated
  table is patched; analysis/player_slots_inventory.json regenerated by tools/slot_audit.py.
- Protocol 47, one PC format (docs/networking.md).
- Tests added: regress_bots_aztec_64, regress_bots_aztec_64_nocollide, regress_bots_dust2_64 (8/8 spawn points), vk_bots64_scoreboard, vk_perf_bots16 / vk_perf_bots64 (measurements).
- Spawns: the user's decision (2026-10-23): overlapping or dying at a shared spawn point is accepted (Player Collision
  is the control); no hull-tested placement.
- Results: window default suite 27 of 27 and vk_bot_count_169; headless 42 of 42 (36 in one run; the 6 that read the probe's position after state_probe.cpp read the moved
  predicted_players); 64 bots and the player on de_aztec (On and Off) and de_dust2 pass; the scoreboard pages; Vulkan
  validation: nothing new. Performance (window 1920x1080, VSync Off): 16 bots 116.5 frames/s, 64 bots 67.5 (server
  frame 0.67 -> 5.54 ms, the bots' AI); memory 42 MB committed at most.
- Open for checkpoint 3 or later: the menu's Number of Bots (to 64), the bots' think time at 64 (not optimized),
  TextMsg / death notice name reads unchecked against the wire, the Xbox communicator's 16 voice records (no PC voice),
  in-process map changes (the title never did them: each match is a relaunch).

## 2026-10-24 — G8.2B: Number of Bots to 64 in the menu

Branch g8.2b, at the user's request after playing 64 bots ("I just need to be able to set that higher number of bots
when in the game"): the menu part of checkpoint 3. The benchmarks at 16 / 32 / 64 are not part of it.
- Custom Game > Match Options > Number of Bots: 0 to 16, then 20 to 64 in fours; Bot Team on one team not capped at 8
  (src/game/pc_menu.cpp).
- mp_limitteams 0 allowed 20 more on one team (MAX_TEAM_DIFF): now N - 1 (config/player_slots.toml, 404 patches).
- SDK patch 0038: DISPATCH_LEVEL is a host lock and DPCs run at it (XACT's event queue raced between the DPC thread
  and the game thread: a crash with 64 bots on one team; FINDINGS "Number of Bots to 64").
- run_game.sh --bots N, --nocollide, any map.
- Tests: regress_bots_aztec_64_oneteam (new, fixture de_aztec_bots64_ct), vk_bot_count_43 / _169 / _219 (to 64).
  Results: the full headless regression passes (43 scenarios), and so do vk_bot_count_43, _169 and _219 (159 steps each).

## 2026-10-24 — G8.2B: high population performance, the scoreboard for 65

Branch g8.2b, approved by the user after testing 65 players: optimise high bot counts without changing gameplay, and
modernise the scoreboard (CS 1.6 / Condition Zero's information, the Xbox look) with real ping and BOT. Earlier work of
the day committed first (2b29f10 .. a1f7952).
- Tools: benchmarks vk_perf_{aztec,dust2}_{16,32,64} (fixtures *_nocollide), tools/perf_summary.py, CSX_LOG_INTERVALS,
  the sampling profiler CSX_PROFILE (src/game/profiler.cpp) and tools/profile_report.py, CSX_LOG_LOCKS (SDK patch
  0039), CSX_LOG_TRACES, run_window_tests.py --exe / --tag / $RUN.
- Found (FINDINGS "High population performance"): one game thread is the limit; bots' line-of-sight traces are the
  largest server cost; a fixed 450 ms busy-wait in the sound system froze every round restart; the generated code
  was built at -O1.
- Changed: generated code at -O2 (SDK patch 0040; -O3 no better); the sound system's stop wait ends after one pass of
  its thread (src/game/sound_wait.cpp). de_aztec 64 bots: 67 to 84 frames/s, server frame 6.1 to 3.8 ms, worst frame
  543 to ~150 ms; 0.1% lows two to three times higher everywhere. Renderer upload dedupe tried, no gain, reverted.
- Scoreboard (FINDINGS "Scoreboard"): Ping column (BOT from a server-set *bot key, measured pings, "-" before one),
  25 lines, scrolling (wheel, Page Up / Down, Home, End) with the section header kept and a "more below" line, rows
  padded to the column count (the original's spectators' header now shows), the 5 s paging removed.
- XACT race: regress_xact_dispatch_level (lockset check on XACT's event heap; fails with CSX_DISPATCH_LOCK=0);
  SDK patch 0039 also logs a wait at DISPATCH_LEVEL (none seen).
- Found, not fixed: bot_kick mid-round crashes the original game too (G7 build): StatShot AddDamage on released memory.
- Tests: scoreboard_rows_test.py, vk_scoreboard_169 / _43 / _219 with tools/check_scoreboard.py, regress_xact_dispatch_level.
  Results: headless regression 44 of 44; window default suite 27 of 27; vk_bot_count_169 / _43 / _219 (159 steps
  each); vk_scoreboard_169 / _43 / _219 (59 steps each, check_scoreboard all pass); Vulkan validation on
  vk_scoreboard_169 only the known gamescope WSI messages; scoreboard_rows_test 1222 checks, 0 failed.
  Commits: 7dfd293 (build -O2, lock stats), eef344a (benchmarks, profiler), 7b4b19f (XACT test), 0a9cfe3 (sound wait),
  ee4304e (scoreboard), then these docs.

## 2026-10-24 — G8.2B: the packet overflow at a round restart

Found while play-testing the scoreboard (64 bots, Player Collision Off, the player dead): KeBugCheck at 0x22DEDF, the
stack check of SV_SendClientDatagram. Cause (FINDINGS "Packet overflow"): a round restart's packet for a spectator is 6
to 9 KB with 65 players, the engine built it in 4000 bytes on its stack, and its bit writer wrote on past the buffer
after the overflow (and wrote whole dwords past the bits).
- Fixed (src/game/net_buffers.cpp/.h): a bounds-checked bit writer (bit for bit the engine's output), the inlined
  end stores checked, SZ_GetSpace's oversized and negative requests; capacity apart from the datagram: the loopback
  client's packet built in 60 KB, its netchan datagram 64 KB, the loopback queue on the host; a network client keeps
  4000 / 4037 bytes (TODO 1e4). From the network: NET_GetLong's split parts and Netchan_Process's fragments checked.
- Tests: net_buffers_test.py (18,314 checks), regress_packet_overflow (ten restarts while spectating; the build before
  fails it with the crash), soak_bots64_nocollide (19 rounds, not in the regression run).
  Results: headless regression 45 of 45; window default suite 27 of 27; vk_scoreboard_169 / _43 / _219 and
  vk_bot_count_169 pass; benchmarks within their earlier spread.

## 2026-10-24 — N0: PC multiplayer architecture investigation

Approved by the user after the scoreboard test (investigation only; N1 waits for approval of the architecture).
- Read: the engine's netcode (connecting, netchan, snapshots, delta, prediction, interpolation, lag compensation,
  voice, timeouts, map changes, queries, rcon), the dedicated server remnants and the Xbox Dedicated Server host
  option, system link discovery and the menus, split-screen. Evidence: analysis/netcode_engine.md,
  analysis/dedicated_server_browser.md, analysis/splitscreen.md.
- Tested: two headless processes in one private network namespace. No path connects them today: the Linux
  relaunch leaks the host's lobby sockets (system link host not found); with that worked around, discovery works
  and the join fails in the joiner's front end; the engine's connect is steered by the lobby.
- Found: the connect challenge is never checked; the info replies leak the session keys; no player identity; the
  last slot is never lag-compensated; changelevel disabled (map change = relaunch everywhere); the Xbox game has no
  split-screen; GoldSrc's dedicated path is in the image but off.
- Proposed (docs/pc_multiplayer.md): keep the engine's netcode, replace the transport under the netchan (MTU-sized
  datagrams, message fragmentation with limits, cookie handshake, connection ids), PC sessions instead of system
  link, a dedicated mode through ca_dedicated, the browser in the System Link Matches screen, split-screen as one
  client process per local player. Roadmap N1 transport, N2 dedicated server, N3 browser, N4 split-screen.
- Docs: docs/pc_multiplayer.md (new), FINDINGS "PC multiplayer investigation", ARCHITECTURE decision 16, ROADMAP,
  TODO 1e4, docs/networking.md. No code changed.

## 2026-10-25 — N1: PC transport and remote clients, stabilised

Approved by the user 2026-10-25 (Option B: optional per-install identity, port 27015, 30 snapshots a second).
Built (docs/pc_multiplayer.md section 15): the encrypted UDP transport under the engine's netchan (src/net), the
engine glue, direct connect without the lobby, map-change rejoin, the engine's security checks. Checkpointed first
(df2bb0b .. 39acee2), then stabilised:
- net_disconnect: the runner never emptied its run folders, so a section's --after matched an earlier run's log and
  client2 started with the host; that exposed a real bug: in the server's first second every challenge landed in the
  same table entry and all but the last client were refused (the case of every map change). Fixed: the issue order
  as the entry's time (612eb8e). Regression net_join_early, net_join_together.
- In-process reconnect (connect, retry) crashed the client: a second world load ran the texture records into
  com_argv. A connect after joining relaunches into +connect, as map changes do (612eb8e). net_reconnect.
- net_full_65's stop at 27 s: the previous session's 10 minute tool limit (no crash, no error). Passes (83 s); a
  variant with two remote humans and 62 bots (net_full_65_2remote) passes with scoreboards, kills, round ends.
- Runner: verdicts PASS / FAIL / TIMEOUT / CRASH / NOT STARTED per process, result.txt, INTERRUPTED on signals,
  CPU / memory / descriptor sampling; orphaned games ended (one had taken a later run's input).
- Security review (independent, 2026-10-25): 8 defects fixed with tests (44d5e37, e364175): a use after free on
  reconnect, closing connections kept alive, unauthenticated rejects, rate-table flood cost, identity.key 0644, the
  stufftext filter bypass (server-run commands), the client-command parser difference (kick / pause), sodium_init.
  Not for the open internet yet: no server authentication (section 15.4).
- SDK patch 0042: the Linux relaunch kept 8 descriptors (the old guest memory's memfd among them) a relaunch.
Results: network scenarios 12 of 12 (section 15.2), unit tests all pass, measurements in section 15.3.
Offline: headless regression 45 of 46 (regress_lagcomp_last_slot's bot out of view, fixed b29b171, passes), window
suite 30 of 30 with scoreboard, bot count and Player Collision. Branch g8.2b, not merged. N2 waits for approval.

## 2026-10-25 — N1 signed off

The full headless regression after the lag-compensation fix, twice (docs/pc_multiplayer.md 15.8): 44 of 46 under
load, 45 of 46 alone; the three different failures each passed every rerun. Two scenarios' assumptions removed
(regress_decals: the player placed after its walk; regress_player_collision_on: a crouched bot's head counts).
The 65 slots: the host's player, 2 remote humans, 62 bots (65 occupied, checked by the run). N2 approved.

## 2026-10-26 — N2: the dedicated server

Approved by the user 2026-10-25 after the N1 sign-off. Built (docs/pc_multiplayer.md section 16): csx-server, the
engine as GoldSrc's dedicated server (ca_dedicated, no client, no host player slot: all 65 for remote players and
bots), no window, GPU, audio device or input (it links libc, libstdc++ and libsodium only); a fixed 30 Hz tick that
sleeps; settings from a file and the command line, each checked; clean shutdown on quit, SIGINT, SIGTERM; map
changes relaunch it with its settings (SDK patch 0043). Boot fixes in FINDINGS "PC multiplayer N2" (the game code's
client part, the name cvar, LostHost, the launcher export, bot_join_after_player).
- Server identity (transport v3): every accept signed by the server's persistent Ed25519 key; clients trust on
  first use or pin it, refuse a changed key. Password and LAN-only options.
- Remote administration over the player's authenticated connection (admins.cfg keys and permissions), off by
  default, rate limited, logged; rcon refused from the network.
- Snapshots: 30 a second per client (the engine's "interval after now" scheduling gave 17-21 at a 30 Hz tick), on
  the listen server too.
- Tests: 13 dedicated scenarios (1, 2 and 8 clients, 65 slots as 2 humans + 63 bots and as 65 bots, lifecycle, map
  changes, trust, admin, malformed, loss, restarts). Found and fixed on the way: the relaunch's map read with its NUL.
Results: network 24 of 24 (+ the admin map change), unit tests pass, headless 46 of 46, window 30 of 30 (one rerun of
vk_player_collision_169). Measured: 65 players at 23% of a core and 101 MB on the dedicated server (48% and 125 MB
for the listen host). Bots are set at a match's start (the title's rule): admin bots N applies from the next map.
Branch g8.2b, not merged. N3 (server browser) waits for approval.

## 2026-10-26 — N3: the server browser

Approved by the user 2026-10-26 after N2. Built (docs/pc_multiplayer.md section 17), in the title's own menus:
System Link becomes Multiplayer (LAN Matches, Internet Matches, Favorites, Recent Servers, Join by Address, Host a
Match); the list is the console's OptiMatch Sessions screen with System Link's button bar, filled with the game's own
row functions; pad, keyboard and mouse; sorting, filters, details (players and bots apart, up to 65/65).
- Discovery (src/net): a signed, rate-limited server query on the game port that never answers more than it is
  asked; LAN broadcast over ports 27015-27020; an optional self-hostable master server (tools/master: csx-master,
  csx-query, csx-fakeservers) with cookie-proved addresses, signed registrations, expiry and limits; a listing is not
  trust (each server's own signed answer must match).
- Joining: the server's proven key checked against known servers and pinned keys before anything (the identity
  warning asks, Accept trusts explicitly), full / version / no answer as messages, the password asked, then the
  title's own relaunch into the server with the key pinned; a join that ends returns to the browser with why.
- Favorites and recent servers in PC settings, tied to identity keys (a favorite follows its key to a new address).
- Fixed on the way: the profile walks overran their 128-byte block with the PC screens' lists (the title ended at the
  main menu once the browser's filters were added: PC screens are skipped now); the menu keyboard logged typed text
  (never the password now); skipping LostHost without relaunching crashed the client.
- Tests: tests/net_browser_test.py (103 checks), 6 network scenarios (LAN, join, favorites, address, master, 300
  servers) between real processes, 3 window scenarios (4:3, 16:9, 21:9), the fuzzer at the query.
Results: network 31 of 31 (the 25 of N1 and N2 and the 6 new), unit tests pass (transport 109, browser 103, server
config 22, stuff filter 14, net buffers 18,314), headless 46 of 46, window 33 of 33 (the 30 of N2 and the 3 browser
scenarios, no rerun). Measured: 300 internet servers listed in 1.6 s at
30 menu frames a second, LAN answers in 0-2 ms from dedicated servers. Not yet secure for the public internet: the
engine's client parsers against a hostile server (section 17.5). Branch g8.2b, not merged. N4 (split-screen) waits
for approval.

## 2026-10-27 — N4: split-screen for up to four local players

Approved by the user 2026-10-26 after N3. Built (docs/splitscreen.md): architecture B of the two compared (one game
process per local player under a compositor that owns the one window, against a second local player inside one
engine, which the title's single client state, view and pad rule out without a redesign).
- The compositor (src/linux/compositor.cpp, the window build with CSX_SPLIT_COMPOSITOR=1): the SDL window, the Vulkan
  swapchain, every input device, the layouts (1 full; 2 top and bottom or side by side; 3 one wide over two; 4 2x2),
  process supervision; a SOCK_SEQPACKET socketpair to each player (src/linux/split_ipc.h). Players render off screen
  into three exported Vulkan images each (VK_KHR_external_memory_fd), blitted into their viewports; the existing
  Vulkan renderer is unchanged otherwise.
- Players: player 1 plays the front end and hosts or joins; players 2-4 follow with +connect, the server's key pinned
  and the password from the environment; each is an ordinary client (own identity, connection, slot, entity, HUD,
  scoreboard, audio stream). The server does not know a client is local. Offline and menu-hosted split matches leave
  room for four local players (61 bots at most: 65 slots).
- Input: player 1 on keyboard and mouse (raw mouse kept) or a controller too; controllers to players 2-4 in
  connection order as virtual SDL gamepads in their processes (no input reaches two players), hotplug, START to join
  a running match or retry after a refusal. The Split Screen screen in the main menu (count, layout, devices).
- Player 1's Display Mode and VSync drive the window; full screen, windowed, resize and 4:3 / 16:9 / 21:9 checked.
- Fixed on the way: a 65-slot offline match had no slot for a joining local player (the bot reserve); views
  letterboxed after a layout change (filled, cropped sides); the password in the compositor's log; gamescope dropping
  SDL's full screen flag.
- Tests: 4 network scenarios (net_split_ded_4, net_split_browse, net_split_listen, net_split_full), 8 window
  scenarios (vk_split_2, 2v, 2_219, 3, 4, 4_43, play, 4_audio) and 7 benchmarks (vk_perf_split_*,
  tools/split_perf.py).
Results on the final build: network 35 of 35 (31 of N1-N3 and the 4 split scenarios; net_split_full failed once on
its own test, which counted before player 1 picked a team: fixed and rerun alone), headless 46 of 46, window 40 of 40
(the 27 default, the 6 scoreboard / bot / collision / browser scenarios, the 7 split scenarios), vk_split_4_audio run
alone, unit tests unchanged and passing. Measured: four players with 61 bots each above 93 frames/s on average,
460% of one core, 719 MB; one player without split-screen unchanged (vk_perf_aztec_64 90.2 frames/s, against 84.0
before N4). Not done: a team change through a menu, player 1 on a controller only as its own scenario, a name row,
a crashed player's restart, latency end to end, listening to four streams (docs/splitscreen.md section 14). Linux
only, as asked. Branch g8.2b, not merged. N5 waits for approval.

## 2026-10-27 — N4.6: split-screen completion

Approved by the user 2026-10-27 after N4 (reported incomplete). Done (docs/splitscreen.md section 16), architecture
unchanged:
- Names: Split Screen, Y opens Player Names; each row opens the profile keyboard (on-screen with the controller,
  typed with a physical keyboard); 1-20 characters, letters, digits, space and a few marks (no `;` or quotes: the
  name goes into a console command); saved as split_name_1..4 in pc_settings.ini; player 1 keeps its profile name
  unless one is set. Each player joins under its own name, local or online.
- Controllers: 2, 3 and 4 controllers with no keyboard player and keyboard plus 3 controllers tested with virtual
  SDL gamepads only (no physical controller test). Fixed: triggers read half pulled on virtual and real pads (a
  crouch, one shot per hold); START on a stopped player's controller restarts it.
- Change Teams: the title's own team menu and confirmation (TeamSelect, the server's team rules: full, stacked,
  spectator only when dead with allow_spectators) added to the pause menu of network and split matches.
- Crash recovery: a player 2-4 process that ends is restarted after 3 s (3 times in 2 minutes at most, then START),
  its viewport says so, its controller stays its own, the server frees its slot at once when it rejoins (the same
  identity replaces the old connection), no processes or images left. Tested by killing players (SIGSEGV, SIGKILL)
  in a round, two at once, in a map change.
- Audio: four streams measured (captures through SDL's dummy device, the game's sounds logged): every kind heard,
  the mix clipped (+0.5 dBFS) and now does not (a limiter per stream, -2.6 dBFS). Not listened to: section 16.6 is the
  user's checklist.
- Player 1's pacing: the bots' AI bursts in the listen server (23-34 ms server frames with 61 bots), not the
  compositor; on csx-server player 1's 1% low is 77 frames/s against 28.5. The scripted-input poll (8% of player 1's
  game thread) fixed. Hosting offline matches in a separate server process is left for approval.
- Full regression on the final build: unit 8/8, headless 46/46, network 35/36, window 26/27 + 7/7, split 15/16; the
  three failures and their reruns in docs/splitscreen.md 16.8. Reruns alone: net_split_full (test fixed) PASS,
  vk_auto_exposure_rates PASS (+0.017, near its limit), vk_split_2_219 PASS three times (its failure, a SIGKILL during
  the intro, unexplained).
Commits 36d10dc, 6482d5e, 8ed2944, fca4389, 2b83464, 2542e20, c89b5a6 and the docs. Linux only. Branch g8.2b, not
merged. N5 waits for approval.

## 2026-10-28 — N4.7: final stabilization, a match in its own server process

Approved by the user 2026-10-27 after N4.6. Done (docs/splitscreen.md section 17):
- Hosting: a match from the menus (Single Player, Host a Match, split-screen) runs in csx-server, which the game
  starts in local_server/ and joins over the PC transport; offline on 127.0.0.1, Host a Match on the PC port with LAN
  discovery; 65 slots; the pause menu pauses the simulation (alone), Restart Match, Change Map and the server's
  console commands go to the server as the owner's commands; the server ends with the match and when the game dies;
  a failed start is a message in the menus. The listen server is kept: PC Options > Match Server > In Game.
- Security: a network client's `changemap` relaunched csx-server into an empty map; refused (net_ded_changemap).
- N4.6's intermittents: vk_auto_exposure_rates (the test's view went into the pitch clamp: fixed) and net_split_full
  (fixed in N4.6) passed in the complete suite; vk_split_2_219's SIGKILL came from outside, twice more in N4.7, cause
  not found (needs root to trace).
- Found and fixed by the new mode: bots starting the round before the owner joined, the offline pause menu's extra
  row, Custom Options' one-team row refused, a quit server's status left "ready"; the benchmarks counted the menus'
  frames and joined by Auto Assign (section 11's "alone" figure overstated).
- Measured (old listen against new local server, one run each, the machine shared): 64 bots single player 118.0 ->
  162.7 frames/s, 1% low 28.8 -> 106.4; four players with 61 bots, player 1 95.0 -> 138.1, 1% low 28.9 -> 85.6; 0-16
  bots within the spread; single-player not regressed; the server costs about 100 MB and 4-39% of a core; its 30 Hz
  tick drops to 24-28 with 61-64 bots (as the dedicated server's).
- Full regression on the final build: unit 8/8, headless 44/46, network 35/37, window 27/27 + 16/17, split 15/16
  (65 of 65 slots). The six failures (section 17.8): regress_bots_aztec_11's spawn check (unexplained, listen
  server), regress_player_collision_off (a crouched bot: test assumption), net_ded_lifecycle and net_disconnect (the
  runner's kill of a frozen client read as a crash since N4.7's drive.py change), vk_local_server_port (its Quit row,
  not updated with the pause menu fix), vk_split_teams (a momentary Spectator screen a 30 Hz server often skips). All
  six PASS when rerun alone after the fixes.
- Manual checks not performed (section 17.7: physical controllers, listening, hotplug, crash messages, LAN between
  two PCs).
Commits 5d89631, 78c454c, b98d769, d65b2ee, c0c60ee, b3820ed, cafdfc1, 07d9814, 24d705b, 1a0398c, 5455eb8, 004b6f9,
1a58994 and the docs. Linux only. Branch g8.2b, not merged. N5 waits for approval.

## 2026-10-29 — N4.8: the multiplayer frontend

Approved by the user 2026-10-28 after N4.7. Done (docs/multiplayer_frontend.md):
- Main menu > Multiplayer: the party screen (four panels, A or START joins, B leaves, controllers remembered by GUID),
  saved player profiles (create, rename, edit the title's controller options, delete with a confirmation; migrated from
  split_name_N), then Local Only (csx-server on loopback, a random password, not announced), Host LAN Match (LAN
  discovery, an optional password, no master server) or the Server Browser (a party-sized slot check); maps from the
  title's list and the map folder, Bomb Defusal / Hostage Rescue by map, Custom Rules (Y) and saved rulesets, a lobby
  with teams and ready, START by player 1; per-player pause menus (player 1's Return to Lobby, End Match, Quit Game;
  players 2-4 Leave Match).
- Found and fixed: a loopback server answering LAN discovery, OwnerSlot before clients (a LAN host's crash), a stale
  T:\releasemap.txt, a browser join losing its password, the tests following an earlier run's server log, and (by the
  full regression) the lobby's team in plain split matches.
- Full regression on 38691c6: unit 9/9, headless 46/46, network 39/39, window 27/27 + 17/17, split 15/20; the five
  failures were the lobby team bug (fixed, e586adc).
- Complete suite on the final build (2026-10-29/30, every test, the window suite all 101 scenarios): unit 9/9, headless
  46/46, network 38/39 then (net_split_browse's timing fixed in the test, 7f1b9ad) a second complete run 39/39, window
  100/101 (vk_soak_direct is a hand-run scenario the runner cannot start; run as documented: pass, 30.8 min). Automated
  regression complete; most of it ran beside another project's game runs (logged).
- Measured: the party screen with four members, 2 processes (compositor 46 MB, player 1 154 MB), 7% CPU; lobby start to
  both players in game 4.3 s.
- Manual checks pending (physical controllers, audio, the look next to the title's screens, a second PC on the LAN).
Branch g8.2b, not merged. N5 waits for approval.

## 2026-10-30 — Autonomous pass N5B..N11

Under the user's mandate of 2026-10-30, the pass was implemented without per-milestone stops. Full report:
docs/report_n5_n11.md. Status table and continuation notes: docs/autonomous_pass.md. Manual checklist:
docs/manual_checklist_n5_n11.md.

**Done**
- N5B..F: menus, input, prompts, audio, HUD options, single window.
- N6: DLC maps listed, the title update analysed, map import with Sienna CZ playable, Deathmatch, Gun Game, the mode
  registry.
- N7: death camera, gravity, flashlight, spray, flashbang colour, controller chords.
- N8: mod loader and skin-pack converter.
- N9: invite codes, server console admin, map rotation, server GUI.
- N10: HUD layout, editor and scale.
- N11: an experimental 96-slot build, tested with 95 bots and 96 players; protocol 48 refuses the stable client.

**Not done**
- Ragdolls; free-for-all Deathmatch; sound mods; HD artwork import; Wii motion and pointer; 128 slots.
- Blocked on the user's registrations: Steam and Discord.

**Tests**
- Targeted only: headless, window, network and unit, all passing on the final build (docs/autonomous_pass.md).
- No complete regression and no physical controllers.
- The final build boots from the intro to gameplay (vk_custom_gamemode).

Commits e97c3e2 (N5B..N10 combined), be4d28b (N11), and the docs. Branch g8.2b, not merged. Waiting for the user's
manual testing and approval; no optimization or complete regression until then.
