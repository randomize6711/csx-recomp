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

## Linux window (2026-10-19)

- src/linux + tools/build_window.sh: the harness host with an SDL3 window (system SDL 3.4.18, OpenGL renderer under
  Hyprland/Wayland); every frame drawn by the reference renderer, the presented 640x960 surface decoded with
  gpu::DescribeSurface/DecodeLevel and shown 4:3 (letterboxed). Input: SDL keyboard/mouse/gamepad into input::Poll.
- Verified with SDL key events pushed through the window's queue (tests/scenarios/window_to_menu.txt): title, notice,
  main menu, map selection, a match loading screen (Terrorist Briefing, Aztec), and live gameplay frames in cs_training;
  the window survives the title's quick-reboot relaunch (exec reopens it); SIGINT closes it cleanly.
- Speed: the single-threaded reference renderer draws 3.1 frames per second in-game at -O3 (about 1.5 at the
  harness's default -O1) on a Ryzen 7 5700X3D; menus run at the paced 30. Not playable in-game yet.
- Keyboard menu navigation by fixed delays is as timing sensitive as the pad scenarios (presses carried over to
  Training in two runs).

## In-game rendering (2026-10-19)

Confirmed:
- **Studio models use program memory vertex shaders.** The model renderer (0x1953xx..0x195720) loads raw microcode
  with an unbound D3D function at 0x3072E0, D3DDevice_LoadVertexShaderProgram by shape (proof in config/game.toml),
  and selects it through SelectVertexShaderDirect (0x307370, guest code) -> SelectVertexShader(0x318088 | 1, 0). The
  runtime's SelectVertexShader was a stub, so every model draw used the stale FVF 0x242 and whatever stream 0 last
  held: garbage positions, screen-sized quadrants in both renderers. Evidence: a write watch (src/common/
  write_watch.cpp) on the "vertex buffer" page showed only the model loader's copy, the contiguous allocator log showed
  the only block there was the model loader's 16 KB, and the draws came from return address 0x195720.
  SDK patch 0017 adds program memory (136 slots), LoadVertexShaderProgram, LoadVertexShader and SelectVertexShader
  (inputs from the object's D3DVERTEXATTRIBUTEFORMAT at +0x14: stream, offset, format per register; the library's
  slot map at 0x37B9F8 is the identity in the XBE). Trees, grass, the view model and player models now render.
- **Model vertices are SHORT positions and packed normals** decoded by the vertex programs (logged after the fix).
- **The glow pass** renders the back buffer into 320x240, then 320x120 and 160x120 (bright pass, near black at the
  spawn in both renderers), 40x60 (average), and adds 160x120 back with ONE/ONE. Textures carry virtual addresses
  (0x836DA000), surfaces physical ones (0x036DA000).
- **The DrawVerticesUP epilogue** (0x6F4E2) clears stream 0 in guest memory as D3D does after a UP draw; the runtime's
  DrawUp restores the previous stream in its own state instead (no visible effect found; noted).
- **No visibility tests**: CS never pushes the report methods (0x17C8/0x17CC/0x17D0).
- **The front end hands a match over through Z:\releasemap.txt** (the command line) plus the launch page
  (tests/launch/README.md).

- **The missing world (fixed 2026-10-19): CS draws through three paths the runtime did not see.** Measured with a
  per-site count of draws per frame and per-draw step shots (CSX_VK_STEPSHOTS):
  1. **Recorded push buffers.** The BSP world renderer (selected by the cvar jcab_render_bsp at 0x51ED0) records the
     whole world at load between D3DDevice_BeginPushBuffer (0x312C90) and EndPushBuffer (0x312D10), 576 indexed
     draws from 0x1986A6 into the buffer made by CreatePushBuffer (0x312C10, kept at 0x552C04), and every frame sets
     the camera into vertex constants 3..6 and calls RunPushBuffer (0x312900). None was bound: the runtime drew the
     576 draws once at load (with the state of the moment) and the replays jumped into an empty buffer. SDK patch 0019
     records host draws between Begin and End and replays them at RunPushBuffer (proofs in config/game.toml).
     Replaying needs the recorded draw's render states, streams, indices, textures and shaders as they were, and of
     the constants only what the recording wrote (so the camera set before RunPushBuffer stays): comparing with the
     state at BeginPushBuffer alone missed values the second recording set again (stream 0 stayed the tree model's,
     constants 8..13 stayed the frame's), and replaying over the frame's render states drew every surface black.
  2. **Inline arrays.** The GoldSrc model renderer (triangle commands, 0x91990) and the GL layer push Begin, an
     NV097_INLINE_ARRAY (0x1818) of packed vertices and End themselves; the push buffer walker ignored 0x1818, so all
     of it vanished (first person weapon, player models, about 470 primitives a frame). The vertex formats D3D's
     stream setup pushes (0x1760) read 0 / 0x1000, because they come from the library's vertex shader object, which
     the host SetVertexShader does not keep: the layout is taken from the host's shader / FVF state (patch 0018).
  3. **Pushed commands drawn with later state.** The walker ran only at the next HLE draw, so inline draws took the
     render target, textures and shader of whatever HLE call came after them (127 strips landed in a glow target).
     Patch 0018 walks pending commands at the start of every D3D entry point (HLE_BEFORE in xdk/hle.h).
  Also: a vertex program selected before its microcode is loaded (the world renderer selects at 0x1963FD, loads at
  0x19691A) ran the previous program; a load now rebuilds the selected shader (patch 0018).
- **The frame cap is the game's**: fps_max (cvar 0x377D08, default "72.0", FCVAR_ARCHIVE) limits the host frame rate
  as GoldSrc's Host_FilterTime does (read at 0x5CDAF); the runtime adds no cap. Kept: the movement physics depends on
  the frame time; raising it is a PC enhancement.

Open:
- RunPushBuffer fixups (pFixup) are not applied (CS passes none); the world's re-recording path (0x1979DA) is
  supported by clearing the recording at BeginPushBuffer, not otherwise exercised.

## Audio (2026-10-19)

Confirmed:
- **The mixer was producing silence in the match** when the game's sounds came only from XACT's 3D path: XACT plays
  every 3D sound as a pair, a MIXIN buffer (flags 0x00602000, DSBCAPS_MIXIN 0x2000) that receives the 3D voice data
  (azimuth, distance and cone levels, Doppler) and a source buffer (flags 0x00400000) holding the wave, which XACT
  plays and routes with SetOutputBuffer(source, mixin). The runtime ignored SetOutputBuffer, so sources played
  through their own extra sends (I3DL2 bin 10 and LFE only) and the azimuth on the mixin was never heard: the output
  was centred (left = right in every second). Evidence: per-voice logs of creation, 3D data and Play in one match.
  SDK patch 0020 routes a source into its MIXIN destination: the source's volume times the destination's levels (its
  bins 6/8/7/9/2, the 3D voice data terms, the azimuth pan) and the destination's Doppler pitch (the library adds the
  parent's pitch for a voice that outputs into a submix, analysis/dsound_xact_5659.md). A destination released first
  (XACT releases the mixin ~30 ms before its source) leaves the source silent.
- **Direction is right**: for sounds away from the player, the side of the source in game coordinates (relative
  position against GoldSrc's right vector from the player's yaw) and the sign of XACT's azimuth agree in every sample
  (e.g. 144.8 units right, 45.5 ahead: 72.6 degrees, XACT 75.9; 86 left, 57.5 ahead: -56.2, XACT -59.9), and the
  listener's front is the player's forward. The mixer pans positive azimuth to the right. Measured in a capture: the
  balance follows the sounds (-0.88 left, +0.23 right).
- **Direct starts lacked the HD sound banks**: the front end copies HDWeapons, HDCounterStrike, HDStreaming and more
  to Z: on boot; a match started with an empty hdd has only the DVD map bank. tools/make_zcache.sh saves a filled Z:
  as build/zcache (game data, not in git), which drive.py --launch seeds.
- **No mix bin headroom**: the game sets headroom 0 on all 32 mix bins, so the console sums without headroom and
  saturates; the mixer's final clamp does the same. Clipping appears only with several loud sounds at once (one
  second of a 50 s match, 293 samples).
- **Output**: the mixer's 48 kHz stereo float goes to SDL3 (src/linux/sdl_audio.cpp); PipeWire takes it as it is
  (48000 Hz, 2 channels, F32), 960 frame (20 ms) device period, no late callbacks over a match, and a capture's length
  matches the run's (no drift).

Approximations / open:
- The console positions 3D voices with its HRTF filter (DSP, per-voice coefficients from azimuth and elevation); the
  mixer pans linearly by the sine of the azimuth instead. Elevation and the front/back distinction are not rendered.
- Envelopes (SetEG) and filters (SetFilter, the I3DL2 filter coefficients) are not applied; the I3DL2 reverb is not
  mixed. Voice starts and stops are not ramped (the console's envelopes would).

## Mouse look (2026-10-19)

Confirmed:
- **The title's own view angles are the yaw at 0x552C10 and the pitch at 0x552C14** (degrees; yaw positive to the
  left, pitch positive looking down, as in GoldSrc). Found by dumping memory before and after controlled stick turns
  and looks (296 floats followed the yaw), then bisecting with writes (src/common/memory_dump.cpp "poke"): writing all
  candidates turned the view, and of them only 0x552C10 turns it and stays; every other copy (the render view at
  0x5623D0+0x84, the audio listener, the player state at 0x3AE3EC, client copies near 0x7D66xx) is rebuilt from it.
- **The stick look routine 0x19E160** turns the stick into angle changes through a response curve, a turn acceleration
  ramp (timer at 0x5CBBA8) and aim assist state, adds them at 0x19E4C3 (yaw) and 0x19E4DD (pitch) and clamps the pitch
  from 0x19E4EB against cvars ([0x543714]+0xC and [0x5436D0]+0xC; -89 measured). It returns early where the title
  takes no look input.
- Native mouse look adds the mouse's degrees in a hook at 0x19E4EB (src/game/mouse_look.cpp): after the stick, before
  the clamp. Degrees = counts x 0.022 x sensitivity (GoldSrc's m_yaw / m_pitch; default sensitivity 3). Measured:
  -30 / +75 degrees of yaw and -20 / +35 of pitch exactly, the clamp at -89, 100 steps of 0.1 degree making 10.001
  (no acceleration), 100 counts in the window turning 6.600 degrees; the stick turns as before; the player's
  server-side yaw follows (the movement command carries the new view).
- The mouse bypasses the stick's response curve, turn acceleration and the aim assist slowdown in that routine (the
  stick keeps them); it is applied once per pass, not scaled by frame time, so frame rate does not change it.

## Rounds (2026-10-19)

Confirmed:
- **The game's server log as a test oracle**: CS's game code reports joins, round starts and ends, kills and
  objectives through UTIL_LogPrintf (0x14BC80, cdecl (format, ...); skipped unless the title's log is on, [0x7D6D4C]);
  a hook at its entry formats the line on the host and logs it as "game: ..." (src/game/game_log.cpp), e.g.
  'World triggered "Round_End"', '"Neil<4><BOT><CT>" killed "empty<1><...><TERRORIST>" with "m249"',
  'Team "CT" triggered "Target_Saved" (CT "2") (T "1")'. The local player is "empty<1>".
- **Complete offline rounds on three maps** (tests/scenarios/rounds_direct.txt, headless, FPS=30, direct starts):

  | Map | Rounds seen | Player | Objectives | Round ends |
  |---|---|---|---|---|
  | de_airstrip | 4 + start of the 5th | moved, fired, killed in round 1, respawned | bomb planted and detonated | CT elimination, Target_Bombed, Target_Saved (time) |
  | de_dust | 4 + start of the 5th | moved 79 units, 3 shots (20 -> 17), respawned each round | bomb carried and dropped | Terrorists_Win, Target_Saved twice |
  | cs_italy | 4 + start of the 5th | killed in three rounds, respawned | hostages touched and all four rescued | CTs_Win, Terrorists_Win twice |

  Bots buy, move, fight (kills with xm1014, sg552, m249, m3, ump45, sg550, mp5navy), plant and rescue; the round timer
  runs out to Target_Saved; scores advance; no fatal error screen or guest trap over 6 to 13 minutes per map.
- The rescue lines on cs_italy come right after the CT win in the log (the game's order).

- **Stability**: tests/scenarios/vk_soak_direct.txt ran the Vulkan window on de_airstrip for 39.5 minutes of real
  time at the game's 72 fps cap, nine complete rounds with the player moving and turning by mouse: no fatal error
  screen, guest trap or Vulkan failure, no dropped draw, upload peak under 9 MB of 64 MB.

Open:
- Bomb defusal and the player planting were not seen in these runs (bots decide); a player-driven objective test
  needs scripted movement on each map.

## Video (2026-10-19)

Confirmed:
- **The boot intro plays through the video overlay**: MGS_Logo.xmv, Valve_Logo.xmv and ESRB.xmv open about 10 s apart;
  the XMV player decodes into two YUY2 surfaces (format byte 0x24, 320x240, pitch 640) it alternates and shows with
  UpdateOverlay (source 0,0-320,240, destination 0,0-640,480 of the 640x480 mode, no colour key, overlay enabled).
  Nothing composited the overlay, so the intro played invisible (its sound did play).
- SDK patch 0021: Backend::SetOverlay with the overlay decoded at each present (YUY2 with ITU-R BT.601 studio range,
  as the console's overlay converts; 32 bit RGB copied), forwarded through the render thread with a copy of the
  pixels; a shared CompositeOverlay (bilinear, colour key honoured) used by the reference renderer's captures and the
  software window; the Vulkan backend blits the overlay into a copy of the presented target (the title's buffer stays
  as drawn). Shown in captures and the window: the MGS studio logo animation, Valve's logo, the ESRB notice; the
  movies' soundtracks play in stereo; the 4:3 source fills the 4:3 display.

Open:
- The Vulkan path does not apply a colour key (logged; CS's movies use none). AttractMovie.xmv and Credits.xmv were
  not exercised (attract mode after idling in the menus, credits from the options).

## Rendering accuracy (2026-10-19)

Compared: the Vulkan window against the reference renderer at the same moments of a direct de_airstrip start
(tests/scenarios/vk_compare_direct.txt and its headless twin compare_direct.txt, FPS=30, tools/compare_images.py).

Confirmed:
- **Gamma is set and was not applied.** The title's inlined SetGammaRamp writes the ramp into slot swapcount&1 of the
  device (0x314B40, ramps at +0x1E04, second slot +0x300) and flags it pending (+0x2404, index +0x240C); the runtime
  picks it up at the next swap (its parity rule matches). CS sets an identity ramp at boot, then at team join a
  brighter one (64 -> 107, 128 -> 213, about x1.67 in the midtones, from the brightness/gamma cvars 0x37A198 = 0.0 and
  0x37A184 = "2.5") that stays for the match. Now applied: the Vulkan backend runs a full screen LUT pass on the
  presented target (the title's buffer stays as drawn), the software window and the reference captures look up the
  same ramp (SDK patch 0022).
- **Fog and the lower ground tint**: the difference reported earlier (Vulkan's ground fog lighter) was the gamma ramp
  plus a capture artifact, not the fog or glow. The reference renderer's snap frames take about a second each, and
  the title answers that long frame by starting a gamma fade (213 -> 165 at the next frame, then down by one a frame);
  the second snap frame was saved with the faded ramp. Snaps now use the ramp in effect when they were asked for.
  After both fixes the late in-match frame (45 s in) matches to 25.1 dB PSNR, region means within 1-5 levels (sky
  127.5 / 127.6, hills 66.8 / 65.2, fog band 86.8 / 81.8, ground 90.6 / 89.9); what differs is the animated smoke
  puffs, rain and bots, which move between runs. The faint horizontal bands in the ground fog appear in both.
- **Incorrect ground triangles** (reference renderer, black near triangles at the spawn): a triangle clipped at
  w = 1e-30 projected to huge coordinates and its edge functions overflowed. SDK patch 0023 also clips at z >= 0 in
  clip space; the triangles now render as on the GPU.
- **CPU readback**: the front end reads its 640x960 front buffer before the relaunch (PersistDisplay, 2457600
  bytes). The Vulkan backend's Sync now copies a render target back into guest memory (logged "target 03934000
  (640x960) written back").
- **Point sprites and CopyRects are not used by CS** in the paths run: the Vulkan dropped-draw counters (points) and
  the first-use logs of point sprites and CopyRects stayed at zero over the in-match runs and the 39.5 minute soak.
  Rain is drawn as quads.
- **Transparency and depth**: the fog sprites, glow add, HUD and weapon blend and occlude alike in both renderers at
  the compared moments (no region differs beyond the moving particles).

Open:
- What starts the gamma fade on a long frame is not identified (it does not happen at a steady frame rate; a real
  hitch on a PC would show it). de_dust and cs_italy were not compared image by image; point sprites and CopyRects
  remain unimplemented in Vulkan (logged on first use).

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
