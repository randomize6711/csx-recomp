# TODO (priority order)

## Now
0. Graphics fidelity (FINDINGS.md "Graphics fidelity audit", "Dynamic gamma", "Flashbang", "Water"): de_aztec's
   water against a console capture (cube filtering across face edges: seamless in Vulkan, per face in the reference
   renderer, unknown on the console). Other screen-space
   passes that now draw without the depth test (the ZEnable fix: 0xB9880, the glow passes) and GL-layer draws now
   kept below depth 0 (the cull range fix): look for any that changed on a map. The Vulkan depth buffer is D32F:
   fixed function fragments below depth 0 are not clamped (nothing seen there yet). Where the per-map dynamic gamma clamps come from (each map's
   "<map>-light.cfg", FINDINGS.md "Bloom", is the likely source). The bloom's GAUSSIANCUBIC kernel (xemu's model)
   against a console capture; the muzzle flash (drawn, not seen) and dynamic lights, frame-exact against the
   reference.
0. Vulkan validation, left after G6 (FINDINGS.md "Vulkan validation"): gamescope's WSI layer chains a
   VkSwapchainPresentModeInfoKHR with swapchainCount 0 to every present and enables VK_EXT_swapchain_maintenance1
   without VK_EXT_surface_maintenance1 (not the renderer's; DISABLE_GAMESCOPE_WSI=1 runs have none); the performance
   warning OutputNotConsumed (vertex outputs and attributes the title's shaders leave unread). Validation was run on
   one GPU (RADV): a check on another vendor's driver would be worth having.
0. The spectator screen (Free Look after dying): one status line in garbled characters, the menu backdrop over the
   view.
0. Play-test on the desktop (Hyprland): Display Mode with the compositor's own fullscreen key, VSync Off at a 240
   limit (frame rate past 120, whether IMMEDIATE tears in full screen: it should not without allow_tearing), the
   menu pointer after the header fix, the loading screens.
0. Stability: an unexplained silent exit of the window build in vk_vsync_play (once; SIGPIPE and exits are now
   logged) and a SIGSEGV in the runtime's socket table (core of 2026-10-20 20:43, sub_ED5B0): investigate if seen
   again.
0. Mouse play-test with a physical mouse on the desktop: Raw Mouse Input On (aim feel at 240, VSync off) and Off
   (speed with the pointer scale at 1.6x); the software renderer still reads the mouse once per refresh.
0. Frame rate past ~190 in matches: the game thread is CPU-bound (renderer recording about 2.7 ms of 5.5, the rest
   the title's code with bots); profile it (no perf on this machine) before any optimisation. An Unlimited limit
   only once rates above 240 can be checked in a match.
0. Weapons: the wheel up enters the grenade slot at its first grenade (this build's invprev goes by slots).
0. Windows: run build/windows/dist on a real Windows 10/11 PC (startup, Vulkan driver, WASAPI, mouse capture,
   relaunch handover, a full round); then a GUI-subsystem exe (no console) if wanted. Then image comparisons on de_dust
   and cs_italy.
0. Developer console remainder: cursor movement inside the input line, a backdrop behind its lines.
0. Native resolution remainder: the software renderer at the internal resolution (CPU cost), fullscreen exclusive
   display modes (only the desktop's size is used; the rendering size is independent).
0. Widescreen stage 3 (after stages 1-2 settle): move suitable HUD elements (health/armour/timer left, money/ammo
   right, radar) towards the frame's edges, keeping their look; aspect change without a restart (device Reset).
0. PC Options next: audio output device; Window Size where
   the compositor honours it. PC Controls: a key taken from another row could clear that row at once (now the save
   keeps it with the first row in the list, as before).
0a. Linux play-test follow-up: compare the gamma ramp (x1.67) and glow pass with a console capture; current vertex
   data for vertex-buffer draws; point / line polygon offsets.
0b. Audio remainder: HRTF-like positioning (elevation, front/back) instead of the linear pan, envelopes (SetEG),
   filters and the I3DL2 reverb; listening test on headphones by a person.
1. Vulkan remainder: point sprites and CopyRects (unused by CS so far, logged on first use), feedback copies;
   occasional frame dips with ~1150 draws. Install vulkan-validation-layers for CSX_VK_VALIDATION=1 runs. The upload
   ring's overflow path re-uses offsets a draw already took (never reached: peak 9 MB of 64 MB).
1b. The gamma fade after a long frame is the dynamic gamma (FINDINGS.md "Graphics fidelity audit"): see G2 above.
1c. Player-driven objectives (defuse, plant) with scripted movement per map.
2. Compare in-game rendering with xemu (dark sky/wall area on de_airstrip, lightmaps, fog, rain, colours).
2a. Thin vertical rain streaks over de_airstrip in the Vulkan window that the reference renderer's frames do not show
   (seen during G6.1, not investigated). Without VK_EXT_depth_clip_enable and VK_EXT_depth_clamp_control the depth
   clamp is skipped (logged) and the HUD can be lost behind the weapon at a wide FOV (FINDINGS.md "Weapon over the
   HUD").
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
