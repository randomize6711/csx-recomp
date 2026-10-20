# TODO (priority order)

## Now
0. Windows: run build/windows/dist on a real Windows 10/11 PC (startup, Vulkan driver, WASAPI, mouse capture,
   relaunch handover, a full round); then a GUI-subsystem exe (no console) if wanted. Then image comparisons on de_dust
   and cs_italy.
0. Developer console remainder: cursor movement inside the input line, a backdrop behind its lines; the PC Controls
   list stepping over the "-" key.
0. Mouse in the menus remainder: the profile name's virtual keyboard.
0. Native resolution remainder: the software renderer at the internal resolution (CPU cost), fullscreen exclusive
   display modes (only the desktop's size is used; the rendering size is independent).
0. Widescreen stage 3 (after stages 1-2 settle): move suitable HUD elements (health/armour/timer left, money/ammo
   right, radar) towards the frame's edges, keeping their look; aspect change without a restart (device Reset).
0. PC Options next: press-a-key capture in PC Controls (TrapKey_Event's trap mode), frame rate limit (SDK harness
   pacing read at run time), audio output device; Window Size where the compositor honours it.
0a. Linux play-test follow-up: compare the gamma ramp (x1.67) and glow pass with a console capture; current vertex
   data for vertex-buffer draws; point / line polygon offsets.
0b. Audio remainder: HRTF-like positioning (elevation, front/back) instead of the linear pan, envelopes (SetEG),
   filters and the I3DL2 reverb; listening test on headphones by a person.
1. Vulkan remainder: point sprites and CopyRects (unused by CS so far, logged on first use), feedback copies;
   occasional frame dips with ~1150 draws. Install vulkan-validation-layers for CSX_VK_VALIDATION=1 runs. The upload
   ring's overflow path re-uses offsets a draw already took (never reached: peak 9 MB of 64 MB).
1b. Identify what starts the title's gamma fade after a long frame (FINDINGS.md "Rendering accuracy").
1c. Player-driven objectives (defuse, plant) with scripted movement per map.
2. Compare in-game rendering with xemu (dark sky/wall area on de_airstrip, lightmaps, fog, rain, colours).
4. Prove the 8 register-convention D3D functions and the 12 unproven ones (analysis/missing_symbols_5659.md).
5. Relative paths without a drive ("valve/gfx/vgui/mouse.tga", "launcher/voice_ban.dt"): console semantics.
6. Unhandled DSOUND export B0080 at 0x246488 (still runs original code); MultiSampleMode adjustment (logged only).
7. Remove the temporary fatal-site hooks; keep LogFatalScreen.
8. Re-test patches 0009-0012 against the upstream titles' expectations before offering them upstream.

## Next
- `xbe_symbols --sign` to produce xdk/5659/signatures.toml once the config is complete.

## Later
- XMV/XACT playback validation against xemu; 3D audio pan check.
- Xbox Live (XONLINE/UIX) paths: clean offline behaviour ("Sign In failed" on the main menu).
- PC enhancements (Phase 7).
