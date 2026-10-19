# TODO (priority order)

## Now
1. Merge analysis/d3d_layout_5659.md into xdk/5659/layout.toml + config [data]; iterate past Direct3D_CreateDevice.
2. Merge analysis/abi_5659.md (15 LTCG maps) into config [abi]; un-park the symbols.
3. Merge analysis/missing_symbols_5659.md (FOUND entries) into config [symbols].
4. Iterate the headless boot to the first frame; capture with REF=N (reference renderer).
5. Inspect `sub_00239001` (1,713 callers, contains int3) — make sure no int3 path is reachable in normal play.

## Next
6. Once the config is complete: `xbe_symbols --sign config/game.toml --out xdk/5659/signatures.toml` (5659 signatures
   for other titles / upstream).
7. Check which of the 145 kernel imports are stubs on paths CS uses.
8. Offer the two SDK fixes upstream (`<unistd.h>` in memory.cpp; `none` ABI location) and an APU register note.

## Later
- Vulkan `gpu::Backend`, SDL3 audio, Linux app shell (window, settings, ImGui SDL3/Vulkan backends).
- Windows build (mingw-w64 cross or native clang-cl), D3D12 path as upstream.
- XMV movie and XACT playback validation.
- Xbox Live (XONLINE/UIX) paths: clean offline behaviour.
- PC enhancements (Phase 7).
