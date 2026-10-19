# TODO (priority order)

## Now
1. Install missing packages: `sudo pacman -S cmake jdk21-openjdk ghidra mingw-w64-gcc directx-shader-compiler`
2. Restructure into the framework's game template: `sdk/` submodule (pin 99078b1), `config/`, `src/`, `assets/` (symlinks
   to the read-only game dir), `CMakeLists.txt`.
3. Import default.xbe into Ghidra (XBE loader extension or raw load at the section VAs); apply the experimental
   symbol candidates as labels.
4. Verify the 130 candidate symbols, starting with the 37 `.XTLID` matches and the 34 constant matches; document the
   17 LTCG convention differences in `[abi]`.
5. Locate the missing host-replaced functions (CRT math, D3D render-state setters, DSOUND buffer lock/3D setters) or
   prove they are inlined.
6. Re-derive `layout.toml` offsets for 5659 (CDevice, DSOUND stream, effects image, XPP) from CS code.

## Next
7. Run `xbe_recomp` into `generated/`, inventory `x86_trap("unimplemented: ...")` sites.
8. Build the headless harness; boot to first trap; iterate.
9. Check which of the 145 imported kernel functions are stubs on the paths CS uses.

## Later
- Vulkan `gpu::Backend`, SDL3 audio, Linux app shell (window, settings, ImGui SDL3/Vulkan backends).
- XMV movie and XACT playback validation; `dspimage.bin` effects.
- Xbox Live (XONLINE/UIX) paths: make offline behaviour clean (no hangs on sign-in screens).
- PC enhancements (Phase 7).
