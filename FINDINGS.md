# Findings

Verified facts only, each with how it was verified. Unverified items live under **Assumptions / to verify**.
Game directory (read-only): `~/Documents/Decomp/xboxcs/Counter-Strike`

## Game files (verified 2026-10-19 by listing and header dumps)

- 82 files, 1.6 GB. Four XBEs: `default.xbe` (the game), `update.xbe`, `downloader.xbe`, `dashupdate.xbe`.
- `.SXWad` archives: one per map (`cs_*`, `de_*`, 29 maps), `FrontEnd.SXWad`, five `CacheTree-<lang>.SXWad`.
  Header starts with u32 `1`, u32 entry count (FrontEnd: 0xD7, de_dust2: 0x478); entries carry plain Xbox paths
  such as `D:\cstrike\resource\LocBase.txt`, `liblist.gam`, `Media\PieMenuUI\CT_BuyMenuEquipment.xpr`.
  The game parses these with its own code, so no host-side support is needed.
- `Media/`: `Wave.xwb` (magic `WBND`, XACT wave bank), `Sound.xsb` (`SDBK`, XACT sound bank), per-map
  `sound/DVD<map>.xwb/.xsb`, `sound/dspimage.bin` (DSP effects image), `sound/soundtrack/*.wma` (2 files),
  `CounterStrike.uix` / `InGame.uix` (magic `XSK0`…`UIX`, Xbox Live UI skins), `dlcustom.xpr` (`XPR0`),
  `arialuni.ttf`, `Videos/*.xmv` (5 movies: AttractMovie, Credits, ESRB, MGS_Logo, Valve_Logo; XMV format, `xobX` tag).
- SHA-1 `default.xbe` = `3848adbd0905f102d523eb91f9655b8091c110bb`.

## default.xbe (verified with `tools/xbe_info.py`, full report in `analysis/default.xbe.txt`)

- Title "Counter-Strike", ID `0x4D530036` (MS-054), version 0x2, retail keys, build timestamp 2003-10-22.
- Debug path `c:\Counter-Strike XBox\GOLDSRC-XBOX\xbox_cs\Final\-xbox_cs-f.exe` (GoldSrc-derived Xbox codebase).
- Base 0x10000, image size 0x7F7FE0, entry **0x00202C3C**, kernel thunk table 0x003210C0.
- 20 sections: `.text` (0x22E20C), `D3DX`, `DSOUND`, `XNET`, `XONLINE`, `XMV`, `WMADEC`, `XGRPH`, `XACTENG`, `D3D`, `XPP`,
  `.rdata`, `.data` (vsize 0x46A680, raw 0x38B48 — large zero-fill BSS), `DOLBY`, `XON_RD`, `.data1`, `DSKERDAT`,
  `$$XTIMAGE`, `$$XSIMAGE`, `.XTLID`.
- **XDK build 5659** for all 13 libraries: XAPILIB, D3DX8, DSOUND, XBOXKRNL, XONLINES, UIX, XVOICE, XMV, LIBCPMT,
  LIBCMT, **D3D8LTCG** (link-time code generated Direct3D), XGRAPHCL, **XACTENLT** (LTCG XACT).
- TLS directory present: no raw template, zero-fill 0x90, index variable at 0x3A5500.
- 145 kernel imports (list in `analysis/default.xbe.txt`).
- The three helper XBEs are separate Microsoft Live/dashboard tools (debug paths under `xboxfre-aug03\private\...`),
  also XDK 5659. They are not needed for gameplay.

## XboxXBERecomp (commit 99078b1, cloned to `third_party/XboxXBERecomp`, MIT)

- Ships XDK data for **build 5849 only** (`xdk/5849/layout.toml`, `signatures.toml`). `xbe_symbols` on default.xbe
  stops with "no signatures for these library builds; available: 5849".
- All 145 CS kernel imports are registered by the runtime (`RegisterExport`/`RegisterData` ordinals diffed against the
  import list). Not yet checked: which of these are stubs.
- `xbe_recomp` and `xbe_symbols` build natively on Linux with clang 23 + Zydis 4.1.1 + toml++ 3.4.0 (no CMake needed;
  `build/tools/`).
- The README's `xbe_symbols --library` option no longer exists; the current option is `--name`.
- Undecodable or unimplemented instructions become `x86_trap(...)` calls in generated code (runtime failure, not a
  build failure) — `tools/recomp/codegen_emit.cpp:78`, `codegen.cpp:149`, `codegen_int.cpp:473`.
- The game executable target is Windows-only: `cmake/XbeRecomp.cmake` links d3d12, dxgi, xaudio2, ImGui win32/dx12.
- Linux support today = `harness/`: kernel + XDK runtime + generated code built with clang, null audio, null GPU
  backend, and a CPU reference renderer that can dump frames to TGA. Uses `harness/compat/` Win32 shims.
- GPU abstraction: `runtime/gpu/backend.h` `gpu::Backend` (~10 virtuals: Init, Clear, Draw, Copy, Present, Sync,
  Flush, SetGamma, fences, reports). Shaders are generated as HLSL (`runtime/gpu/shaders.h`).
- No specific support for XMV, XACT or WMA; these run as recompiled library code over the DSOUND/D3D HLE.
  Bink support is irrelevant (CS has no Bink).
- `XOnlineStartup` returns 0; XNet/sockets are implemented for system link (`runtime/xdk/xnet.cpp`).

## Cross-build signature experiment (5849 signatures on a 5659 XBE)

Run: `xbe_symbols default.xbe build/xdk_experiment --name game` with `xdk_experiment/5659` = copy of `5849`.
Output `analysis/game_5849sigs_experiment.toml`, log `analysis/xbe_symbols_experiment.log`.

- Found **130 of 238** functions and **6 of 12** data symbols; 37 via the `.XTLID` export table (exact), 34 D3D
  functions via constant-matching (scores 0.60–1.00, need manual confirmation).
- 17 matched D3D functions read registers / pop different stack sizes than the 5849 reference (LTCG conventions),
  e.g. `D3DDevice_SetTexture` at 0x306460 reads eax, pops 4 (reference: stack only, pops 8).
- Missing include the CRT math helpers (`crt_sin/cos/...`), most `D3DDevice_SetRenderState_*`, Begin/End,
  light/material, palette, gamma, `DirectSoundCreateBuffer`, `IDirectSoundBuffer_Lock/Unlock` and 3D-position setters.
- **None of these addresses are verified yet.** They are candidates for Ghidra confirmation.

## Recompilation survey (2026-10-19, xbe_recomp @ 99078b1)

- `xbe_recomp` finishes in ~2 s: 15,360 functions (1,036 by direct calls, 4,011 pointer seeds, 3,758 gap fill),
  873,593 instructions, 376 jump tables, 93 C++ catch blocks; 2.84 MB of 3.21 MB code-section bytes decoded.
- All 65 generated files compile with clang 23 (-O1) with no errors.
- **DSKERDAT is not x86 code**: 113 "functions" decoded there and none is called from outside the section
  (48,645 call sites checked). Removed from `code_sections`.
- Trap inventory: every function containing an unimplemented instruction (aaa, arpl, bound, iretd, insb, les...)
  has **no direct caller** except `int3` sites; they are data decoded by pointer seeding / gap fill in WMADEC, XMV,
  DSOUND. Directly called functions with traps contain only `int3` (e.g. `sub_00239001`, 1,713 callers — to inspect).
  1,427 "divide error" traps are the generator's divide-by-zero checks, not problems.

## Headless boot on Linux (harness, network namespace, reflink copy of the game data)

- Boots: CRT init, kernel imports resolved (0 missing), the title mounts D:/T:/U:/Z:, opens `D:\FrontEnd.sxwad`, probes
  `d:\shared\`, `d:\cstrike\`, `d:\valve\` (GoldSrc mod folders, absent on the disc) and opens `d:\default.xbe`.
- **XACT reads the APU sample counter directly**: `mov eax, [0xFE80200C]` at 0x3026FD (sub_003026E9, called from
  game code 0x1066A0 through XACT 0x2F91BF). NV_PAPU_XGSCNT = global sample counter (xemu `apu_regs.h`,
  `mcpx_apu_read`: frames × 32 samples at 48 kHz). The runtime models no APU registers. Fixed in
  `src/common/apu_clock.cpp` (commits the page, host thread keeps the counter running). The XPP section also reads it
  (sub_0031D572).
- Other hardware MMIO in code: APU 0xFE80xxxx/0xFE82xxxx and NV2A 0xFD00xxxx inside DSOUND/D3D internals, NVNet
  0xFEF00000 inside XNET. Reached only if those internals run instead of the runtime's HLE.
- After the fix: effects image `dspimage.bin` loads (6 effects), XACT initialises, the title calls
  `Direct3D_CreateDevice`, which stops on missing 5659 D3D data/layout (expected next step).

## XDK 5659 values proven from CS code (in `xdk/5659/layout.toml` / `config/game.toml`)

| Value | Proof |
|---|---|
| EffectsImage_Skip0/Skip1/Desc = 0x804/0x80C/0x818 | 0x2486DA, 0x2486E0, 0x2486E9 (same as 5849) |
| XPP_Current/Changed/Previous = 0/4/8 | XGetDevices 0x3193A7 (0x3193B2..BA), XGetDeviceChanges 0x3193C9 (0x3193D2) |
| XPP_GamepadType = 0x31823C | .XTLID id 0x1007C |
| D3D_Device = 0x314B40, D3D_pDevice = 0x316FE8 | Direct3D_CreateDevice 0x30A551 stores 0x314B40 into 0x316FE8 |
| CDevice_Flags = 0x8, CDevice_Size = 0x24A0 | 0x30A545/0x30A565 (device+8 gets BehaviorFlags & 0x10); 0x30A58C clears 0x928 dwords |
| Direct3D_CreateDevice ABI = none, none, none, eax, stack, ecx | 0x30A542, 0x30A5AD, ret 4 + caller 0x1084E1 |

## Assumptions / to verify

- Third-party licenses (from prior knowledge, confirm before reusing any code): Half-Life SDK — Valve's restrictive
  SDK license; Xash3D FWGS — GPLv3; xemu and Cxbx-Reloaded — GPLv2. All are **reference-only** for this MIT-based
  project; no code to be copied.
- Missing D3D functions may be inlined into callers by LTCG rather than absent; must be checked per function.
- 5849 CDevice/DSOUND struct offsets in `layout.toml` are expected to differ in 5659 and must be re-derived.
- `.data1`/`DOLBY` sections: purpose not yet confirmed. DSKERDAT is presumably the DSP (DSP56300) program image
  DirectSound downloads; verified only that it is not x86 called from code.
- XGSCNT granularity of 32 samples follows xemu's model, not a hardware measurement.
