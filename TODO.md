# TODO (priority order)

## Now
0. Vulkan backend (decided 2026-10-19): milestone 2 menus, milestone 3 a live match at a playable rate
   (docs/vulkan_plan.md). Install vulkan-validation-layers for CSX_VK_VALIDATION=1 runs.
1. Verify player control in the match: close the buy menu, move/turn/fire, check the view changes (REF=1 snapshots).
2. Compare in-game rendering with xemu (dark sky/wall area on de_airstrip, lightmaps, fog, rain, colours).
3. Overlay compositing so movies are visible (reference renderer first, then backends); front buffer resolve of the
   640x960 back buffer.
4. Prove the 8 register-convention D3D functions and the 12 unproven ones (analysis/missing_symbols_5659.md).
5. Relative paths without a drive ("valve/gfx/vgui/mouse.tga", "launcher/voice_ban.dt"): console semantics.
6. Unhandled DSOUND export B0080 at 0x246488 (still runs original code); MultiSampleMode adjustment (logged only).
7. Remove the temporary fatal-site hooks; keep LogFatalScreen.
8. Re-test patches 0009-0012 against the upstream titles' expectations before offering them upstream.

## Next
- Linux presentation: SDL3 window + Vulkan gpu::Backend (or a stopgap that blits the reference renderer output).
- Windows build of the full game exe (mingw-w64 cross or clang-cl) with the upstream D3D12 backend; the D3D12 side of
  patches 0010/0012 is compiled only there and untested.
- `xbe_symbols --sign` to produce xdk/5659/signatures.toml once the config is complete.

## Later
- XMV/XACT playback validation against xemu; 3D audio pan check.
- Xbox Live (XONLINE/UIX) paths: clean offline behaviour ("Sign In failed" on the main menu).
- PC enhancements (Phase 7).
