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
