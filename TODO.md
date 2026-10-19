# TODO (priority order)

## Now
1. Drive input in the harness (pad script START) and reach the main menu; find the next gaps.
2. Overlay compositing so movies are visible (reference renderer first, then D3D12/Vulkan backends).
3. Understand why REF=200 captures are black while REF=1 shows the frame.
4. Prove the 8 register-convention D3D functions and the 12 unproven ones (analysis/missing_symbols_5659.md).
5. Relative paths without a drive ("valve/gfx/vgui/mouse.tga", "launcher/voice_ban.dt"): check what the console does
   with them (current directory semantics) and whether the runtime should map them.
6. Unhandled DSOUND export B0080 at 0x246488 (still runs original code).
7. Remove the temporary fatal-site hooks once boot is stable.

## Next
- Linux presentation: SDL3 window + Vulkan gpu::Backend (or a stopgap that blits the reference renderer output).
- Windows build of the full game exe (mingw-w64 cross or clang-cl) with the upstream D3D12 backend.
- Offer the SDK patches upstream (0001-0007), plus the <unistd.h> fix.
- `xbe_symbols --sign` to produce xdk/5659/signatures.toml once the config is complete.

## Later
- XMV/XACT playback validation against xemu; 3D audio pan check.
- Xbox Live (XONLINE/UIX) paths: clean offline behaviour beyond XOnlineGetUsers.
- PC enhancements (Phase 7).
