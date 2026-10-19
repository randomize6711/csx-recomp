# TODO (priority order)

## Now
1. DSOUND: implement the buffer/stream methods XACT uses that the runtime lacks (id-less 0x246444 / 0x246428,
   SetEG, SetPitch, SetLFO, GetVoiceProperties, CreateSoundStream) — identify each from its CDirectSound body first.
2. Iterate the headless boot to the first frame; capture with REF=N.
3. Prove the 8 register-convention D3D functions in analysis/missing_symbols_5659.md and the 12 unproven ones.
4. Remove the temporary fatal-site hooks once boot is stable (keep LogFatalScreen).

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
