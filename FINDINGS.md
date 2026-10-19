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
  DSOUND. Directly called functions with traps contain only `int3`. `sub_00239001` (1,713 callers) is MSVC
  `__security_check_cookie` (cmp ecx, [0x3707A4]; jne -> report at 0x238FD1); its int3 at 0x23900F is padding after an
  unconditional jmp, unreachable. `0x239032` is `__security_init_cookie` (default cookie 0xBB40E64E).
  All 152 int3 sites checked: they follow int3/ret/jmp (padding), sit in phantom data regions, or follow calls that
  never return — 0x23048F (32 sites, builds an exception record from template 0x327294, consistent with
  _CxxThrowException), 0x22DF01 (5, calls 0x22DE31(1,0), exit path), kernel PsTerminateSystemThread (258),
  KeBugCheck (95), HalReturnToFirmware (49). None is reachable in normal execution.
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

## Bring-up past Direct3D and UIX (2026-10-19)

- 5659 D3D [data] and all 37 [layout] keys proven (analysis/d3d_layout_5659.md); every layout value equals 5849, only
  data addresses differ. 15 LTCG [abi] maps proven (analysis/abi_5659.md). 26 more host-replaced D3D functions found
  (analysis/missing_symbols_5659.md); 51 symbols are not linked in CS (Bink, 7 CRT math, many DSOUND 3D setters), 9
  inlined (incl. SetGammaRamp at 0xBE15A), 12 unproven.
- **D3DDevice_Reset** = 0x305980 (ebx = parameters), not implemented by the runtime: CS calls it from its video setup
  0x108470 (640x480, multisample 0x11, depth 0x2C, interval 1). Added to the runtime (patches/sdk/0002).
- **0x108470 is the title's fatal error screen** (Reset, clear 0xFF00FF00, one quad, Swap, Sleep(1000) forever),
  reached from 29 sites (18 calls, 11 tail jumps).
- **UIX needs XOnline**: the UIX front end (0x208817 -> 0x2084D6 -> 0x207E0B) calls .XTLID 0x30005 at 0x275876
  (XOnlineGetUsers by its body: clears a 0x700 byte user array and a count, reads stored accounts), which returns
  XONLINE_E_NOT_INITIALIZED (0x80150005, global 0x3A5698 null) because the runtime's XOnlineStartup builds no state.
  The failure is fatal (site 0xF33FC). Fixed offline in src/game/xonline.cpp: no stored accounts, S_OK.
- **XACT calls DSOUND buffer methods the runtime does not replace**: after loading uixdefault.xsb/.xwb, XACT
  0x2FBD3D calls id-less wrapper 0x246444 (CDirectSoundBuffer method 0x245E89 -> 0x2451A6, sets bit 0 of
  [[buffer+0x10]+0xB]), which faults on the runtime's buffer objects. Also unhandled: SetEG 0x2462E4, SetPitch
  0x2462AC (probable), SetLFO 0x2462C8 (probable), GetVoiceProperties 0x24640C (probable), CreateSoundStream 0x247843,
  id-less 0x246428 (method 0x245E3B -> 0x245075). Next blocker.
- XACT's 3D path: XACT computes voice settings with DSOUND's 3D calculation (0x2464AF -> 0x24588A, args 0x3038A0,
  source+0x44, 0x303658, +0xC0, out +0xE8) and applies the result with 0x246428, whose block (dword 0 = flags,
  fields +0x4..+0x34) is copied into [[buffer+0x10]+0xB8] before the voice update 0x24C1E1. So the block holds
  computed voice parameters, not positions; the runtime's position-based 3D model does not see them. Field meanings
  under analysis (analysis/dsound_xact_5659.md).

## To the title screen (2026-10-19)

- XACT 3D path implemented from analysis/dsound_xact_5659.md (SDK patch 0003): 3D voice data (id-less 0x246428 /
  0x246444 and stream twins), SetPitch (1/4096 octave re 48 kHz), SetLFO/SetEG (logged, not played),
  GetVoiceProperties, CreateSoundStream, stream SetOutputBuffer. DirectSoundUseFullHRTF 0x244BED must run its original
  body: 0x2485C4 fills the calculator table 0x262CF4..0x262D18 XACT calls through (lock 0x262654 is statically
  initialised, lock count -1).
- Missed function 0x10DCC0 (virtual method via [vtable+0x224]) added to [functions].
- CDirectSoundStream vtables at +0/+4 (constructor 0x246D95, destructor 0x245535).
- D3DDevice_UpdateOverlay 0x30A760 (ret 0x14) / GetOverlayUpdateStatus 0x30A940: overlay surface at device+0x1C1C,
  update vertical blank count at +0x2410. Movies (XMV) present only through the overlay (SDK patch 0004 records it;
  compositing not implemented yet).
- device+0x30 = pointer to the GPU's time (D3D_BlockOnTime 0x30A007, inlined IsFencePending 0xAD052); the runtime
  points it at the device time (+0x2C) so inlined fence checks pass (SDK patch 0005).
- **End-of-file reads must fail before being queued**: the cache reader (0x1FC230) double-buffers 128 KB overlapped
  reads and expects the read past the end of CacheTree-English.SXWad to fail synchronously, leaving OVERLAPPED.Internal
  at the STATUS_PENDING XAPI ReadFile set (0x1FEDB2); XAPI GetOverlappedResult (0x2019E9) then returns TRUE. The runtime
  wrote STATUS_END_OF_FILE into the status block, which made the cache step fatal ("Cache", 0x5522E8). SDK patch 0006.
- **ETHREAD.ExitStatus (+0x120)** is read by XAPI GetExitCodeThread (0x200C04, after the signal state at +4); the
  runtime did not write it, so the main thread waited forever for the cache worker. SDK patch 0007.
- The cache worker unpacks D:\CacheTree-<lang>.SXWad to Z: (55 MB for English) on first boot.
- Result: MGS, Valve and ESRB movies play (through the overlay), then the title screen is drawn ("Please press START
  to begin", one 1024x512 DXT quad, 640x480 orthographic), and after ~50 s idle the attract movie starts, as on the
  console. First frame captured with the reference renderer (REF=1). Colours not yet compared against the console.

## Front end to an offline match (2026-10-19)

- **Black REF=200 captures (harness artifact, not a game bug)**: CS clears target and depth exactly twice, at start-up
  (Reset and the first frame, flags 0xF3, z 1.0), and never again. The harness only passed clears to the reference
  renderer on drawn frames, so with REF=N the depth buffer stayed 0 and every later quad (z 0.5, LESSEQUAL) failed.
  Evidence: on a captured frame the only draw targets the presented surface (rt 0x03B8C000) yet writes no pixel;
  with every clear applied the same frames show the title screen. Harness patch 0008.
- **Multisample back buffer**: CreateDevice gets MultiSampleType 0x2012. CS's frame buffer init stores it at
  device+0x196C (0 means 0x11) and derives the back buffer scale X = (t>>4)&15, Y = t&15 into +0x96C/+0x970
  (0x30C6B3..0x30C6FF): a 640x960 back buffer for the 640x480 display. While the back buffer is the target the super
  sample scale (+0x95C/+0x960, set by 0x304B10 from the back buffer scale; MultiSampleMode RS 154 halves X, mode 2 Y)
  multiplies the viewport (0x304650: X*ssX + screen space offset, W*ssX*0.5, H*ssY*-0.5). SDK patch 0009.
- **Text and other screen-space programs write target pixels**: the UI text vertex program (handle 0xB0064B51, 3
  instructions: oPos.xy = v0.xy, oT0.xy = v0.zw, oD0 = v3) is fed y in 960-line units (rows at 369.84, 449.84...,
  shadow offset +2,+4), so programmable output is not scaled by the super sample factor.
- **Pretransformed (XYZRHW) positions are in display pixels**: the full-screen background quads (FVF 0x144) span
  0,0..640,480, so the console scales them by the super sample factor. SDK patch 0010 (state, D3D12 shader, reference
  renderer).
- **Pretransformed z is in depth buffer units**: every frame CS draws its backgrounds pretransformed at z 0.5 with
  ZWRITE on and LESSEQUAL (0x203), then text at depth 0 (the text program leaves oPos.z at 0, as xemu's oPos
  initialisation (0,0,0,1) also gives), and never clears depth. It only renders if 0.5 compares <= 0, i.e. z is in
  24-bit depth units (0.5 quantises to 0), not 0..1 scaled by 2^24-1. Experiment: with unscaled z the menus render
  cleanly with depth testing on. SDK patch 0012. Not re-tested on the upstream titles.
- **Reference renderer near-w clip**: it clipped against w > 1e-6, while a program that writes only oPos.xy ends with
  w clamped to 2^-64 (MagClamp, as the D3D12 path does), so every glyph was discarded. Harness patch 0011.
- **Title relaunch**: choosing a map makes CS quick-reboot into itself (HalReturnToFirmware(2) after a missing
  d:\-xbox_cs-f.xbe probe) with launch data; the runtime writes hdd/launch.bin and execs the image's executable name
  from GameInfo::images ("CounterStrike"), so the harness binary must carry that name. The new instance loads
  InGame.uix, D:\de_airstrip.sxwad and media\sound\DVDde_airstrip.xwb.
- **Reached**: notice screen (A ACCEPT), Create a Profile keyboard (profile saved to U:\<id>\, T:\CurrentProfile.dat),
  main menu (Training, Single Player, Xbox Live, System Link, Options, Change Profile; "Sign In failed" shown), Training
  (Hostage-Rescue, Demolition), Single Player (Beginner..Custom), Map Selection, in-game team selection (Auto-Select,
  CT, T, $6000), spawned as a Terrorist with the bomb: HUD, radar, buy menu, 3D level rendered. 30 s later the round
  timer has advanced (4:47 -> 4:26) and the kill feed shows a bot kill ("Matt" -> "Troy"): simulation and bot AI run.
  Player movement is not verified yet (the buy menu may still have had focus).
- Input: the harness reads a press from build/run/cmd; written non-atomically it could be read empty and dropped,
  so tools/drive.py writes it to a temporary file and renames it.

## Player control (2026-10-19)

- Input path: the title polls XInputGetState (0x319670), which the runtime replaces; it calls input::Poll (pad script or
  build/run/cmd in the harness) and writes packet, buttons, 8 analog buttons and both sticks (XINPUT_STATE). Menu and
  gameplay input take the same path.
- State found by diffing memory dumps (src/common/memory_dump.cpp, tools/find_state.py) around controlled input:
  - 0x3AE3E0: float x, y, z of the local player; z 36.031 standing on de_airstrip. Constant while idle, moves while the
    left stick is held. Copies of the same triple: 0x3038A8 (beside the 3D audio listener at 0x3038A0), 0x4C7138 with
    z + 17.03 (consistent with a view origin).
  - 0x3AE3F0: yaw in degrees. Proof: turning right for 20 frames took it from 0 to 230.751 while the 3D audio
    listener's forward vector (0x3038C0) went from (1, 0, 0) to (-0.633, -0.774, 0) = (cos 230.75, sin 230.75).
  - 0x3A2988: int, 20 -> 19 -> 18 -> 17 over three single right-trigger presses with the Terrorist's Glock (20 round
    magazine). Reads 0 when the player is a Counter-Terrorist (USP): it is not a generic current-magazine value.
- Measured at 30 presented frames per second (harness FPS=30): 90 frames of full forward stick move the player 765.1
  units (about 255 units/s) along the yaw within 0.0 degrees; 20 frames of full right stick turn 129.3 degrees; the
  same numbers come out in separate runs. Three trigger presses fire three shots. Round freeze time after spawning
  blocks movement for several seconds (a first test that moved too early measured 260 units).
- At spawn the buy menu opens by itself during freeze time; B was pressed before moving. Movement worked after it.
- The team menu's Auto-Select varies the team between runs; tests pick Terrorist explicitly.
- Harness pacing: unpaced the harness presented 100-400 frames per second, so presses held a fixed number of frames
  lasted 20-80 ms and could carry into the next screen (an A on "Done" also selected Training). With FPS=30 (CS creates
  its device with presentation interval 0x80000002, read as every second vertical blank) a 3-frame press is 100 ms.

## Assumptions / to verify

- Third-party licenses (from prior knowledge, confirm before reusing any code): Half-Life SDK — Valve's restrictive
  SDK license; Xash3D FWGS — GPLv3; xemu and Cxbx-Reloaded — GPLv2. All are **reference-only** for this MIT-based
  project; no code to be copied.
- Missing D3D functions may be inlined into callers by LTCG rather than absent; must be checked per function.
- 5849 CDevice/DSOUND struct offsets in `layout.toml` are expected to differ in 5659 and must be re-derived.
- `.data1`/`DOLBY` sections: purpose not yet confirmed. DSKERDAT is presumably the DSP (DSP56300) program image
  DirectSound downloads; verified only that it is not x86 called from code.
- XGSCNT granularity of 32 samples follows xemu's model, not a hardware measurement.
- Presentation interval 0x80000002 read as "two vertical blanks, or immediate when late" (30 frames per second); the
  runtime does not model the interval, the harness paces only with FPS=N.
- Azimuth sign (positive = right) for the 3D pan: moderate confidence, needs a listening check.
- Synchronous-error I/O semantics (status block and event untouched) are inferred from CS's own XAPI and reader code,
  consistent with NT; not checked against the console kernel binary.
- "Sign In failed" on the main menu is the offline Live sign-in; not compared with a console without network.
- MultiSampleMode / MultiSampleAntiAlias adjustments of the super sample scale are logged, not modelled (CS has not
  set MultiSampleMode on the back buffer in the runs so far).
- Overlay (movies) still not composited; the console resolves the 640x960 back buffer into a 640x480 front buffer,
  the runtime presents the 640x960 surface directly (presentation scales it).
