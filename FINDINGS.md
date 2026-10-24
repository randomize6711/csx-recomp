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

- **Distance attenuation was dropped (fixed, SDK patch 0024).** XACT runs the DSOUND library's 3D calculator as guest
  code and sends its result with IDirectSoundBuffer_Set3DVoiceData (0x2FCB91) to the **source** buffer (flags
  0x400000) it routes into the MIXIN buffer: in a de_airstrip match, all 710 blocks carrying a distance level (flag
  0x01, l3DDistanceVolume at +4) went to routed sources, and only that term was ever non-zero on them (cone, front /
  back and I3DL2 terms 0). Patch 0020 mixed a routed source as its volume x headroom x the destination's levels and
  never applied the source's own block, so every 3D sound played at full level whatever its distance (direction was
  right: it comes from the destination). The voice update applies a voice's distance and cone term D = clamp(+4 + +8)
  to every bin (0x24AF4E, analysis/dsound_xact_5659.md), a source's output into a submix included; patch 0024 applies
  it to the routed source.
- **The levels are the game's**: the calculator's inputs at the call (src/game/audio3d_probe.cpp, CSX_AUDIO_LOG3D=1)
  are the listener (0x3038A0, position at +8; distance, rolloff and Doppler factors 1) and the cue's source parameters
  (cue+0x44: position at +8, minimum / maximum distance at +0x38 / +0x3C). The level follows DirectSound's inverse
  distance law with a rolloff of 0.75 (its source not traced; the listener factors read 1) for the min 400 / max 10000 class (weapons and most world sounds): gain =
  400 / (400 + 0.75 (d - 400)), e.g. 699 units -386 mB, 902 -576, 951.6 -617 (computed values match to 1 mB), about
  -12.6 dB at 2140 units and -18 dB at 4000. Other classes seen: min 500 / max 100000, 50 / 2000, 120 / 600, 50 / 200,
  30 / 300, 10 / 350 (the shorter ones fall faster near their maximum). Inside the minimum distance the level is 0.
- **Checked on de_airstrip, de_dust and cs_italy** (tests/audio3d_check.sh, tools/check_audio3d.py, with XBE_LOG_3D=1
  logging each routed 3D voice's mixer gain): per class the level falls strictly with distance (rank correlation
  -1.000), the mixer applies XACT's latest level in 95-99 % of gain changes (the rest are a probe / mixer timing
  offset), far voices (-800 mB or lower) play 16-18.5 dB below voices at 0 mB; left and right still differ by
  azimuth. Music, menu and other 2D sounds do not go through a submix and are unchanged (the window capture check
  still passes).

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

## Windows build (2026-10-19)

Toolchain: Clang 23 targeting x86_64-w64-mingw32 (GNU ABI, UCRT) with LLD, the distribution's MinGW-w64 14 headers and
CRT and GCC 16's libstdc++ / winpthreads, cross-compiling from Linux (cmake/toolchains/x86_64-w64-mingw32-clang.cmake,
cmake/windows/CMakeLists.txt, tools/build_windows.sh). Third-party binaries from their official releases, pinned by
digest (tools/fetch_windows_deps.sh): SDL3 3.4.18 MinGW package, DXC v1.10.2605.37 (both the Linux build's versions).

Audit (compiler and ABI):
- **The SDK runtime is Windows first.** Upstream builds it with clang++ and LLD for the MSVC target, with a Win32 /
  D3D12 / XAudio2 / ImGui app layer (runtime/app, gpu/d3d12.cpp, audio/xaudio2.cpp, core/input.cpp); the kernel's
  memory, threads, disc and relaunch code has _WIN32 branches (VirtualAlloc / MapViewOfFileEx, CreateThread,
  overlapped ReadFile, CreateProcessW). The Linux harness reaches the same sources through small Win32 / Winsock
  shims (sdk/harness/compat). The Windows build compiles the Linux window build's source list (runtime without its
  app layer, the harness's app side and reference renderer, src/*: SDL3 window/input/audio, Vulkan backend) with the
  real Windows headers, so the runtime's _WIN32 branches are upstream's.
- **No MSVC-only construct in that source set**: no SEH (__try), no ATL in the runtime, no #pragma comment the build
  relies on (system_info.cpp's is in the app layer, not built). The recompiled code is plain C++ with GCC attributes.
- **Overridable functions** are __attribute__((weak)) definitions; on COFF they become weak externals, and LLD in
  MinGW mode resolves them to the host's strong definitions: 254 functions, the same set as the Linux window build
  (253) plus the XDK select (0x2630C8, below). Checked from the linked symbol table (tests/windows_check.sh).
- **C++ exceptions** across objects (the dispatcher throws X86Resume through recompiled frames) use SEH-based
  unwinding on x86-64 MinGW; a throw through a weak-overridden call chain was checked in a cross-compiled test
  under Wine. x87 state is kept in double, so long double's size (64 bit MSVC, 80 bit GNU) does not matter.
- **long is 32 bit on Windows** (LLP64, both ABIs); the runtime is written for it; nothing in src/ casts pointers to
  long.
- MinGW-w64 x86-64 is not a reason to switch to clang-cl: nothing needed MSVC's ABI. DXC's interfaces are COM
  (HRESULT returns, a fixed vtable ABI), so the MSVC-built dxcompiler.dll is called from GNU-ABI code safely.

Changes needed (all in our code, none in the SDK):
- src/vulkan/vk_shader.cpp: on Windows MinGW-w64's dxcapi.h (GUIDs without MS extensions; DXC's own header needs
  __declspec(uuid)) and a minimal CComPtr (ATL is not in MinGW-w64); a missing <cstring>.
- src/vulkan/vk_backend.cpp: a std::string(first, last) where MinGW's strchr returns char* (length form now).
- src/linux/window_main.cpp: setenv through _putenv_s on Windows (which also sets the process environment a relaunch
  inherits); the Windows relaunch's "--relaunched" argument skipped; the relaunch handover (below).
- The Vulkan backend counts presented frames in harness::g_frames like the software window (for the handover).
- Built without NDEBUG, as on Linux: a CMake Release build defines it, which drops the runtime's info log (only
  errors are written, logx kVerbose) and asserts.

Packaging: the C/C++ runtimes are linked statically; CounterStrike.exe imports UCRT (api-ms-win-crt-*, part of Windows
10+), KERNEL32, USER32, WS2_32, SDL3.dll and dxcompiler.dll (both packaged, with their licenses) and vulkan-1.dll (the
GPU driver's loader; its import library is made from cmake/windows/vulkan-1.def, the core 1.0-1.3 and KHR surface /
swapchain functions the backend calls directly). build/windows/dist: the exe, the two DLLs, run_game.bat, README.txt,
licenses/.

Platform differences:
- **Relaunch** (front end to match): Linux execs in place; Windows starts a new process (kernel::Relaunch,
  CreateProcessW with the arguments plus "--relaunched") and the old one keeps its window until the new one has
  presented two frames and signals a named event (the SDK's app/handover.cpp does this for its own app layer;
  window_main.cpp does it here). Log files continue cs_001.log -> cs_002.log as on Linux.
- **XDK select (0x2630C8)**: the runtime's host select (xnet.cpp, HLE_STDCALL(select, 5)) binds on Windows; on Linux
  the harness's compat/winsock2.h "#define select ws_select" renames it, so the recompiled XDK select runs there. Only
  used by networking (system link); not exercised offline.
- **Guest memory**: the first reservation hint (0x100000000) is taken under Wine, the runtime uses 0x200000000 (its
  own fallback list). The guest sees the same addresses either way.
- **Paths**: the game folder and hard disk are host paths through std::filesystem; the kernel's case-insensitive
  lookup (files.cpp MatchCase) tries the exact path first, which Windows' case-insensitive file systems satisfy.
- **Audio**: SDL's WASAPI output; a 6 channel device gets the mixer's stereo through SDL's conversion (stereo to the
  front pair). The mixer is the same code.
- **No network namespace** on Windows: the Linux test runs keep the title offline with unshare; on a Windows PC the
  title's sockets are real (Xbox Live sign-in fails offline, system link would broadcast on the LAN).
- wchar_t is 16 bit on Windows, 32 bit on Linux; DXC's arguments are wide strings for each platform's own DXC build.

Verified under Wine 11.20 (Linux, RADV through winevulkan; not Windows):
- Boot, intro movies through the overlay (vk_intro), the front end, single player to a de_airstrip match through the
  Windows relaunch and handover (vk_match), and the direct match: team menu, buy menu, 753 units walked, mouse look
  exactly -6.6 / -3.3 degrees (vk_match_direct, 32 of 32 steps). Window shots match the Linux window's.
- Audio through SDL WASAPI with the real device: the capture passes tools/check_audio.py (not silent, sounds placed to
  a side, 281 samples at full scale in one second at a gunshot, like the 293 in the Linux run); no late device callback logged.
- No error-level log line, fatal error screen, guest trap or Vulkan failure in these runs.

Open:
- **Not run on Windows itself** (no Windows machine or VM here). Wine shows the imports resolve and the Windows code
  paths run under Wine's implementation of them; driver, WASAPI, focus / relative mouse and file-system behaviour on
  Windows remain to be checked there.
- Console subsystem (a console window shows the log); a GUI subsystem build would need SDL_main or a WinMain.
- The upstream D3D12 path is not built; the Windows build uses the same Vulkan backend as Linux.

## Shadows and HUD colours (2026-10-19)

Confirmed:
- **Character shadows were drawn white** (played-in report: white circles under players, in both renderers). The
  shadow is a quad per character (prim 8, 4 vertices) of CS's GL layer: an inline array (NV097_INLINE_ARRAY) of
  positions and texture coordinates only (slots 0 and 9) with a 32x32 DXT3 blob texture, stage 0 MODULATE(texture,
  diffuse) for colour and alpha, blend SRCALPHA / INVSRCALPHA. Its colour is set before the array as the slot's
  current value (NV097_SET_VERTEX_DATA, slot 3 = diffuse): (0, 0, 0, 1), black, in all 375 shadow quads of a test
  run. The runtime kept current values but fed a primitive only the slots it sent per vertex, so the draw took the
  default white diffuse: the reference renderer's probe under a bot showed source 0.581 (the texture's grey) at alpha
  0.40, lifting the ground from 0.097 to 0.289; after the fix source 0 at alpha 0.38, darkening it to 0.060.
- **The hardware model**: each attribute slot keeps its last SET_VERTEX_DATA value and a draw reads it for every slot
  its arrays do not supply (xemu's pgraph, reference only: attributes with no array are uniforms of their inline
  value; SET_VERTEX_DATA4UB is x = byte 0 .. w = byte 3, as the runtime decodes it). SDK patch 0025: the push buffer
  walker remembers which slots have a value, inline-array vertices start from those values, and immediate / inline
  primitives feed them.
- **The same values colour CS's GL-layer HUD and texts**, which had been white: HUD numbers now show GoldSrc's fade
  (colour stepping from 200/255 down to 100/255, GoldSrc's MIN_ALPHA, after a value changes, so resting numbers are
  dim), the crosshair and the buy-zone / C4 icons are green, the freeze-time clock red, "You have spotted a friend"
  green and the friend's name and health red, alpha fades through the 4UB colour's top byte. Evidence: per draw logs
  of the slot 3 value and method (0x1A3C float, 0x194C bytes) with the HUD atlas (256x256 DXT3) draws.
- Checked: shadows dark under the bots in both renderers and in successive window shots as they run; resting health
  digits' mean luminance 82-84 (was 119.5) in regress_hud_colour.txt.

Open:
- Draws from vertex buffers (not the push buffer walker) that leave a slot out still get the backend's defaults, not
  the slot's current value; no such draw was identified in CS.
- No console capture to compare the HUD's exact brightness with.

## Lamp glows and bright areas (2026-10-19)

Played-in report: lamp lights much larger than intended, obscuring the scene.

Measured:
- **Glow sprites are the game's own geometry**: additive (ONE / ONE) quads of CS's GL layer, inline arrays of
  positions and texture coordinates, with world-space vertices from the engine and fixed-function transforms with the
  game's matrices; depth tested (LEQUAL), so walls hide them in both renderers (A/B window shots on de_dust). The
  quad sizes are the engine's: e.g. a de_dust glow (64x64 sprite) 552 units across at 2940 units away (GoldSrc's glow
  mode keeps a constant screen size), a nearer lamp sprite 39 units across. No large additive quads (60 units or more)
  are drawn around the de_airstrip and cs_italy spawns.
- **Their brightness and colour came through the lost current colour** (fixed by SDK patch 0025, "Shadows and HUD
  colours"): the engine's distance / visibility fade and the lamp's colour are set as the diffuse slot's current value,
  e.g. 0.02 for the far de_dust glow, (0.58, 0.51, 0.25) for a warm lamp, 0.39 for de_dust's additive light polygons.
  Before the patch every one of them was drawn at full white, so a glow's soft edges were as bright as its core: a
  bigger, white, opaque-looking light. This is the likely cause of the report; it is not confirmed visually because no
  lamp could be brought into an unobstructed view from the positions the tests reach (spawn views, a short walk).
- **Bright areas wash out in both renderers alike** (de_dust sky looking up: the reference renderer's frame and the
  Vulkan window's agree), from the game's own gamma ramp (x1.67 in the midtones from team join, 128 -> 213, anything
  above about 60 % saturates) and its additive glow pass. Not a Vulkan problem; whether the console looks the same
  needs a console capture.

Open:
- The gamma ramp's strength and the glow pass against the console. (A lamp at the end of de_airstrip's Terrorist
  tunnel was later seen drawn large and white before patch 0025 and not after it: "Decals".)

## Decals (2026-10-19)

Played-in report: de_airstrip's A and B direction signs near the Terrorist spawn flicker, disappear and reappear with
the view and position.

Confirmed:
- **The signs are decals** (infodecal entities, read from the map's entity text in guest memory): {siteA}, {siteB},
  {arrow2a/c/d}, {target2}; the ones seen from the Terrorist tunnel are at the far end, 1630 units south of the spawn
  ({arrow2d} -1742 654 62, {siteB} -1693 658 72, {siteA} -1553 656 65, {arrow2c} -1498 662 85).
- **They are submitted every frame**: polygons (prim 10) of CS's GL layer with the alpha-tested decal texture, alpha
  test and blend on, depth LEQUAL, no depth writes, and polygon offset: NV097_SET_POLY_OFFSET_FILL_ENABLE (0x338) 1,
  slope factor 0, bias 0xC59C4000 = -5000.0 depth units (draw lists of the reference renderer).
- **Neither renderer applied the offset**, so a decal lay at the depth of its wall and the LEQUAL test let wall or
  decal win per pixel with the smallest change of position or angle (z-fighting): in the reference renderer the "B"
  and its arrow mostly drawn at one spot and gone 0.7 units further; in the Vulkan window "A" and "B" missing, partial
  or whole across eight shots 0.33 degrees apart. The upstream D3D12 backend applies it (d3d12.cpp); the Vulkan
  backend had the pipeline fields but never set them, the reference renderer had none.
- **Fix**: the reference renderer adds bias + slope x the triangle's larger depth gradient (depth units) before the
  depth test (SDK patch 0026). The Vulkan backend keys the offset per pipeline; its targets are D32F (RADV has no
  D24S8), whose constant bias would scale with each primitive's depth exponent, so it uses VK_EXT_depth_bias_control
  with the float representation (exact, in normalised depth: bias / zscale), falling back to 24 bit equivalent units
  without the extension (logged). After the fix the signs are solid in both renderers at every tested angle.
- Checked: regress_decals.txt (the "B" sign's ink share 0.57-0.59 at three angles; 0.00-0.46, varying, without the
  offset). The same Vulkan A/B also showed a lamp glow at the tunnel's end drawn large and white before patch 0025
  and absent after it (FINDINGS.md "Lamp glows and bright areas").

Open:
- Point and line offsets (0x330 / 0x334) are not applied; CS's decals use the fill offset.

## Original menus and PC Options (2026-10-19)

How the game builds its menus (CS 5659, evidence from the code and a dump of the loaded text):
- **Screens are text, loaded whole at startup**: FrontEndUI.txt for the front end, InGameUI.txt for the in-game menus
  (with _CS / _DE / _CS_DE variants chosen at 0xD7873; LocBase.txt for localised strings), from the game's packed
  files, through the engine's file system (0xCD9A0: open +0x2C, size +0x3C taken into ebp at 0xCDA2C, buffer of size
  + 1, read +0x54, close +0x30, then parsed by 0xD71B0's callees). FrontEndUI.txt is 216 KB, 97 screens.
- **The format**: a screen is a name and a block with MenuType, EnterSound / HighlightSound (menu/buy_submenu.wav,
  menu/buy_hover.wav), CachedImage / BackgroundImage (textures by name: Back_2ndary.1Pane, Cancel_Accept,
  Arrows_Selected...), BackgroundText (colour, rectangle, scale, text), handlers OnEntry / OnButtonDownA, B, X, Y,
  Back, Start / CommandSimulate1, 2, the four pie directions Generic_4_1..4 (CommandHighlight "tableMoveUp",
  "tableIncreaseValue"...), and a Table: a Layout (column widths and justification, location, size, row spacing,
  text scale, colours, highlight images) and Rows. A row holds cells: CellFixed (a label), CellList (choices
  ListOption / ListDefault "label" "value") and nested Tables (the help text shown at the bottom while the row is
  highlighted), and a CommandSelect. Example: SuperLauncher_Options' four rows open SuperLauncher_ControllerOptions,
  run "!pushSoundtracksToUI; xbui open SuperLauncher_Sound", open SuperLauncher_OnlineOptions, run "!PlayCredits".
- **Commands**: a command beginning with "!" is a console command (0xCA6F0 formats it and hands it to Cbuf_AddText
  0x3B970); others are UI commands ("open X", "back"). The screens' logic lives in console commands the game
  registers with Cmd_AddCommand (0x3C760), e.g. stuffUItoPlayerData 0xDAD40 (saves a screen's choices to the profile),
  pushSoundtracksToUI. There is no binding of a list to a variable in the text: values move through such commands.
- **The UI objects** (read by stuffUItoPlayerData, written by 0xD7920): screens in a list from 0x5523D0 (name pointer
  +0, next +0x378, table +0x380); a table's rows +0x60..+0x64 (32 bytes each); a row's cells +0xC..+0x10 (0x78 bytes
  each); a list cell's current choice index at +0, its choices +0xC..+0x10 (0x1C bytes each, std::string).
- **Settings storage**: the Xbox profile (U:\<id>\data.bin, T:\CurrentProfile.dat) via the game's own commands; PC
  settings go to a separate pc_settings.ini in the run folder so the profile format is untouched.

The integration (src/game/pc_menu.cpp, src/common/pc_settings.cpp; no game file changed, no UI code of ours):
- **Text added at load**: the hook after the size (0xCDA2E) gives a FrontEndUI file 64 KB more buffer, the hook after
  the read (0xCDB1A) inserts a "PC Options" row into SuperLauncher_Options (before View Credits, with its own help
  text) and appends a screen, SuperLauncher_PCOptions, written like the game's Control Options screen: same
  backgrounds, sounds, arrows, layout, help lines, A to accept and B to cancel. The game parses, draws and navigates it
  with its own code (fonts, highlight, sounds, controller).
- **Its commands**: OnEntry "!pc_options_load" (the lists take their values from pc_settings.ini), OnButtonDownA
  "!pc_options_save; xbui back". A hook in Cmd_ExecuteString (0x3CA25, after tokenising; the engine has no such
  command) serves pc_ commands and leaves through the function's empty-command exit (0x3CB27). (Cmd_ForwardToServer
  is not reached for unknown commands while not connected.) The rows' choices come from one table in pc_menu.cpp,
  which also generates their text.
- **First setting: Mouse Sensitivity** (0.5 to 10, default 3 = GoldSrc's), read by the window's mouse look
  (src/linux/window_input.cpp; CSX_MOUSE_SENS still wins when set). Checked: the row and screen in the reference
  renderer and the Vulkan window; changes saved on A, not on B; the saved value shown on the next launch; 100 mouse
  counts turn 13.2 degrees at sensitivity 6 (6.6 at 3); regress_pc_options.txt.

- **Further settings, each checked where it acts**:
  - Invert Mouse (Off / On): the mouse look's pitch; -50 counts give +3.3 degrees instead of -3.3 (CSX_MOUSE_INVERT
    still wins when set).
  - Display Mode (Windowed / Fullscreen, SDL's borderless desktop fullscreen; F11 switches and saves it): on Hyprland
    the window goes fullscreen at 2400x1350. Asked at window creation or before the first frame it is ignored (the
    tiling compositor keeps the window tiled), so it is applied once the first frame is up.
  - Master Volume (0-100, default 100): scales the mixer's output to the device (after the game's own Sound Options);
    a capture of the intro movies at 50 has 0.501 of the level at 100.
- **Dropped: Window Size.** SDL was asked for 960x720 and the window stayed at the compositor's tile (1181x1300) on
  this desktop (Hyprland, tiling); not verifiable here, so not offered.
- **Vulkan swapchain follows the window**: a Wayland surface takes the swapchain's size and reports neither out of
  date nor suboptimal on a resize, so the swapchain kept its first size (fullscreen showed the old 1190x652 image,
  tiling resizes were ignored). Present now compares the window's pixel size and recreates it (2400x1350 in
  fullscreen, 1181x643 after a tile change).

- **The pause menu too**: InGameUI.txt (165 KB, loaded through the same loader at a match's start) gets a row in its
  Options screen (after Online Options, centred between blank cells as its rows are) and InGame_PCOptions, written
  as InGame_ControllerOptions: the csxbackground overlay (Modal 50), gold title, buy_enter / buy_submenu sounds,
  Arrows_Selected, no help lines. pc_options_load / pc_options_save take the screen's name. Changes apply at once in
  the match: 100 mouse counts turned 7.7 degrees after setting 3.5 there (6.6 before). CSX_UI_DUMP=1 writes the menu
  files as loaded to uidump/ in the run folder (diagnostic).

Open:
- The mouse in menus (the original UI has no
  pointer: arrows, Space / Left Ctrl or a pad navigate); key bindings, field of view, frame rate limit and audio
  device are not offered yet (the frame pacing is read once by the SDK harness; FOV changes the game's look and
  needs care with the view model).

## PC key bindings (2026-10-19)

How the original game takes input (CS 5659):
- **GoldSrc's key system is intact.** The engine's key table (0x378250, 107 names) has the PC keys (letters and
  digits as their characters, SPACE 32, SHIFT 134, F1-F12 135-146, MOUSE1-5 241-245, MWHEELUP 240 / MWHEELDOWN 239,
  the keypad 160-175) and the pad's buttons as keys: GREEN 203 (A), RED 204 (B), BLUE 205 (X), YELLOW 206 (Y), BLACK
  211, WHITE 212, LTRIGGER 213, RTRIGGER 214, DPAD 215-218, START 219, BACK 220, LBUTTON / RBUTTON 221 / 222 (stick
  clicks). Pad presses reach Key_Event (0x66510: keydown[] 0x6CAFE0, keybindings[] 0x6CABE0, the client's key hook
  [0x7D6D28] first) through CEngine::TrapKey_Event (vtable 0x339E24 slot 9, 0xAEEE0; with a trap mode a binding UI
  could capture a key, unused). The controller layout is a set of bindings: in a match GREEN +jump, RED +attack2, BLUE
  +use, YELLOW cycle_weapon, BLACK cycle_grenade, WHITE drop, LTRIGGER +duck, RTRIGGER +attack, the d-pad the radio
  (followme, holdpos, report, needhelp), START "xbui mainmenu", BACK +showscores, LBUTTON +buymenu, RBUTTON +fastlook.
  bind / unbind are the engine's (0x65F40 / 0x65ED0, Key_StringToKeynum 0x65C70).
- **GoldSrc's commands work**: with keys sent to Key_Event, +forward walked 425 units in 1.5 s, +jump rose 41-44
  units, +attack fired, +reload reloaded, +buymenu opened the buy menu; invnext / invprev / lastinv / slot1-5 select
  weapons once hud_fastswitch is 1 (without it the slot keys wait for a confirming fire, as on a PC). The Scoreboard
  screen (InGameUI.txt) closes on OnButtonUpBack, the pad's BACK release, not on -showscores.
- **The open menu screen** is 0x5523D4 (0 while playing; TeamSelect_RootSinglePlayer, T_BuyMenuRoot, Scoreboard,
  InGame_RootSinglePlayer... while one is up).

The implementation (src/game/pc_keys.cpp, src/common/pc_bindings.cpp, src/linux/window_input.cpp, Options > PC
Controls):
- **PC keys are engine keys while playing**: the window queues key, mouse button and wheel events (engine numbers);
  a hook at Cbuf_Execute's entry (0x3BB50, every frame on the game thread) hands them to Key_Event with CallGuest, so
  their commands run in that pass. Releases always reach the engine for keys it saw pressed (no stuck keys when a
  menu opens). Escape stays the pad's START (the game menu); F1 / F11 stay the window's.
- **In the game's menus the keyboard stays the pad** (arrows d-pad, Space A, Left Ctrl B, R X, WASD left stick for
  the pie menus...) whatever the bindings, so menus are never lost to a binding; keys still held when play resumes
  leave the pad.
- **Bindings are the engine's**: the PC actions' keys are bound with bind (Cbuf_AddText 0x3B970, text on the guest
  stack) and checked against keybindings[] twice a second, so a rebinding by the game is undone; only keys this set
  are unbound. hud_fastswitch 1 is set whenever play resumes (the client's cvar). The pad's bindings are untouched.
  Scoreboard is the one action bound as a pad button (BACK held while its key is), as its screen needs BACK's release.
- **27 actions with PC defaults**: WASD, Space jump, Ctrl crouch, Shift walk, Mouse 1 / 2 fire, R reload, E use, the
  wheel next / previous weapon, Q last weapon, 1-5 weapon slots, G drop, B buy menu, Tab scoreboard, N night vision,
  Z X C V radio. Kept in pc_settings.ini as bind.<action> = <engine key name>.
- **PC Controls screens** (front end and pause menu) come from the same declaration as PC Options (pc_menu.cpp's
  registration: a screen is its id, title, rows of settings, reset and uniqueness flags; the text, the Options rows,
  load, save and defaults are made from it, and only FindTable / ListCell know the UI objects' layout). Each action
  row is a list of the bindable keys (None, letters, digits, Space... mouse buttons, wheel, arrows, keypad,
  punctuation; Escape, F1 and F11 left out); X restores the defaults (the game's Cancel_Accept_Reset legend), A saves,
  B cancels. A key chosen twice stays with the first action in the list and the later one becomes None, logged ("Tab
  is already Jump's, Scoreboard is left unbound").
- Checked in the Vulkan window through SDL (keys, clicks, wheel): jump, two shots, reload, knife / C4 / last weapon by
  3, wheel, Q, buy menu by B, scoreboard held on Tab and closed on release, Escape to the game menu and back; Jump
  rebound to Tab in the pause menu: Tab jumps, Space does not, Scoreboard unbound; X then A: Space jumps again. Mouse
  look and the pad unchanged. regress_pc_bindings.txt does the same headless (engine key events, the pad's A, the
  saved Tab binding as after a restart, the pause menu's X and A).

Open:
- (Done 2026-10-20: press-a-key rebinding, section "Press-a-key rebinding"; the list stays for the pad.)
- Gamepad buttons are not rebindable (the game's own Controller Setup chooses their layout).
- Whether the game stores the PC keys' bindings in the Xbox profile was not seen (they are set again from
  pc_settings.ini anyway).

## FOV and widescreen (2026-10-19)

How the Xbox version builds its view (CS 5659, from the code and draw lists):
- **FOV**: GoldSrc's. scr_fov_value (0x377668, horizontal degrees for a 4:3 view, default 90) is written every frame
  from the client's HUD_UpdateClientData result (0x1CD12: the server's per-player FOV, i.e. 90, or a weapon's zoom:
  the AWP gives 40 and 10). R_SetupGL (0x4E030) passes it to CalcFovY (0x4DFD0: the vertical FOV for the view
  rectangle's width and height, 0x6E7A28 / 0x6E7A2C, 640x480; values under 1 or over 179 become 90) and the result to
  MYgluPerspective (0x4DF80, near 1, far 4096, through the GL layer's glFrustum). R_SetFrustum (0x4DAB0) builds the
  culling planes from the same scr_fov_value and CalcFovY on the video size (0x6CE274 / 0x6CE3C8). The 3D projection
  is x 1, y 1.333 (90 x 73.74 degrees); the 2D layers use an orthographic 640x480 one (2/640, -2/480).
- **Console variables**: default_fov and fov exist (GoldSrc's) but do not change the view: the server's per-player
  FOV (SetFOV / m_iFOV) is what reaches scr_fov_value. zoom_sensitivity_ratio scales the stick's zoomed turning.
- **The viewmodel** is drawn with the same projection as the world (only the perspective and the 2D orthographic
  projections appear in a frame's draws), as in PC GoldSrc; there is no viewmodel FOV.
- **Zoom**: the server sets the player's FOV (40 / 10 for the AWP); it reaches scr_fov_value like the default.
- **Widescreen in the original**: r_anamorphic (cvar 0x3773E8, default 0) makes R_SetupGL use a 16:9 aspect
  (1.7778) with CalcFovY's vertical FOV from the 4:3 rectangle, i.e. Hor+ into the same 640x480 buffer, for a 16:9 TV
  to stretch. It is read nowhere else: R_SetFrustum stays 4:3 (objects at the wide edges would be culled) and the HUD
  and menus are not compensated (they would be stretched). The console's widescreen flag (XC_VIDEO_FLAGS, reported
  by the runtime) is not used by CS to set it.
- **Renderers**: the projection is the game's (a guest matrix through SetTransform and vertex shader constants), so
  the Vulkan backend and the reference renderer show the same view; at FOV 110 they match to 25.9 dB PSNR, as at 90.

Field of view (src/game/pc_view.cpp, Options > PC Options):
- A hook after the per-frame update (0x1CD17) replaces scr_fov_value's 90 with the chosen FOV, so the projection and
  the culling planes move together; a zoomed weapon's FOV is left as the game sets it. Choices 90 (Original, the
  default), 95 ... 120, saved as fov in pc_settings.ini, applied the next frame (also from the pause menu).
- Checked: the projection's x scale 0.7002 at 110 (1 / tan 55), the AWP's zoom 2.747 (40) and 11.43 (10) unchanged,
  back to 0.7002 after; no geometry missing at the edges; the same in Vulkan; regress_pc_fov.txt (seeded 110, an AWP
  bought through the buy menu zooms 40 / 10 and back, the pause menu sets 115).
- The first-person weapon looks smaller at a wide FOV (same projection as the world, as on a PC); a separate
  viewmodel FOV would need its draws found and given their own projection, cosmetic, not done.

Widescreen experiments (not kept):
- A wider back buffer alone (present parameters 854x480 before Direct3D_CreateDevice, 0x553620): the runtime makes
  an 854x960 buffer, the engine keeps its 640x480 window rectangle (0x6CE210..0x6CE21C, from the game interface
  [0x379FAC] +0x24 at 0xAF192, not from the present parameters) and draws into the left 640 columns.
- With the rectangle widened too (right edge 854 at 0xAF1DF): the 3D view fills 854x480 (r_refdef's rectangle
  854x480), Vert- (CalcFovY keeps the horizontal 90, the vertical shrinks to 58.7), the video size used by
  R_SetFrustum stays 640x480, and the HUD is mixed: the crosshair and most elements stay in 640-wide coordinates
  (the crosshair at x 320, left of the centre), the right-aligned ammo runs past the right edge. Menus, the scope
  overlay, fades and the glow pass's 320x240 / 160x120 targets are all laid out for 640x480.
- So the 3D renderer accepts a wide view (with a Hor+ FOV and a matching frustum), but each 2D layer needs its own
  treatment; see PROGRESS.md for the options.

## Widescreen (2026-10-20)

Native widescreen (src/game/pc_wide.cpp, Options > PC Options > Aspect Ratio: Automatic, 4:3, 16:10, 16:9, 21:9;
saved as aspect in pc_settings.ini, taken at start; Automatic is the display's aspect in the window, 4:3 headless).
The frame stays 480 lines high and gets wider (768, 854, 1138 at 16:10, 16:9, 21:9; back buffer x960 as before),
and both window renderers present it at that aspect, letterboxed. At 4:3 every hook leaves the game as it was.

The 3D view:
- Back buffer: the present parameters' width (0x553620) at 0xAF223, before Direct3D_CreateDevice. After it (0xAF255)
  the width goes back to 640: the client reads 0x553620 as its screen width in about a hundred places (HUD layout,
  the "%d_pain.spr" resolution sprites: at 854 it asked for sprites/854_pain.spr, which does not exist).
- View: the window rectangle's right edge (0x6CE218, centre 0x6CE220) at the end of its setup (0xAF1DF).
  GL_BeginRendering (0x56A10, every frame) makes the GL size (glwidth 0x6CE3C8, glheight 0x6CE274) and vid.width /
  vid.conwidth (0x6CE2BC / 0x6CE2D8) from it, SCR_CalcRefdef (0x55850) the view rectangle (r_refdef.vrect 0x6E7A20..
  0x6E7A2C). vid.width has to stay the frame's (kept at 640 in an experiment, the 3D view was 640 wide again).
- Hor+: scr_fov_value is horizontal degrees for a 4:3 view; at R_SetupGL (0x4E118) and R_SetFrustum (0x4DAD6) it
  becomes 2 atan(tan(fov / 2) x (w / h) x 3 / 4) for the view rectangle, which keeps the 4:3 vertical angle (73.7 at
  90). 16:9: 90 -> 106.3, the AWP's 40 -> 51.8 and 10 -> 13.3 (the same magnification as at 4:3); 21:9: 90 -> 121.3,
  FOV 110 -> 137.0. scr_fov_value itself stays the game's (the client and the zoom logic see the original values).
- Culling: R_SetFrustum runs inside the VGUI paint (0xB4560), where the GL size is the 640x480 root panel's, so its
  CalcFovY used 640x480 whatever the view: the side planes stayed 90 (logged from the planes: 90.00 x 73.74 at 21:9).
  At 21:9 the sky and a fence at the right edge of de_airstrip were missing (black in the reference renderer, stale
  in Vulkan). The hook gives R_SetFrustum the view rectangle's size and the Hor+ angle: 121.29 x 73.74, nothing
  missing at the edges. (At 4:3 the panel and the view are both 640x480, so the original is unaffected.)
- The first-person weapon uses the world's projection (no viewmodel FOV), so it keeps its 4:3 size on screen.

The 2D layers, all laid out in 640x480, kept unstretched in the middle 640 columns:
- GL_Set2D (0x58F70; hook at 0x58FA7, its glOrtho skipped): glViewport on the GL size, then glOrtho(0, glwidth,
  glheight, 0). Two callers matter: the engine's 2D (GL size = the frame) and the VGUI paint (0xB4560: HUD, menus,
  scoreboard, team and buy menus), which sets the GL size to its root panel (640x480) around its drawing. Both get
  the whole frame as viewport and glOrtho(-side, 640 + side, 480, 0) with side = (width - 640) / 2.
- Paths in screen pixels that no projection moves, shifted by side when the GL layer's viewport (device + 0xEE0) is
  the frame: the GL layer's screen rectangle 0x6F080 (XYZRHW quads: the sniper scope's ring, sprites/ghostring,
  drawn 0,0-640,480 by the client at 0x1ACBF5 below FOV 45, and its centre sprite; the menus' backdrops via 0xCC24D;
  the loading screen via 0xC2B53); the text renderer 0xB9B20 (thiscall: x, y floats, colour, string, flags, width;
  glyphs through its own vertex program: menus, HUD text, chat, centre messages); the video overlay's destination
  (D3DDevice_UpdateOverlay calls 0xC71FA and 0x2902BA: the intro movies).
- Full-screen screen rectangles (0,0-640,480) also cover the side columns, each from its texture's edge column: the
  scope's surround is black to the edges, the menus' and the spectator's backdrops continue (their bands are
  horizontal), so nothing of the 3D view shows beside a menu.
- Already right without changes: the glow pass (its targets follow the back buffer: 427x240, 427x120, 213x120 at
  16:9), the crosshair (HUD, centred with it). (This said R_PolyBlend's fades were right too: they were not drawn at
  all then; drawn, the screen fade covered the left 640 columns, fixed by PcWideFade, "Flashbang". And the glow's
  composite was not over the whole frame: it kept a scissor, fixed in G5, "Bloom".)
- Not seen: an element the original places just outside 640x480 to hide it would now show in the side columns (the
  viewport is the whole frame); none found in the menus, HUD, buy menu, scoreboard, spectator, scope or movies.

Checks: Vulkan against the reference renderer at the same moment, 16:9 26.9 dB PSNR and 21:9 27.7 dB (4:3 at FOV
110: 25.9 dB); 4:3 runs unchanged (the whole suite, back buffer 640x960); 16:10 frame 768x480, 90 -> 100.4.

## Native resolution (2026-10-20)

How the frame is made (investigated before changing anything):
- The title renders into a 640x960 back buffer (480 display lines, 1x2 super sampling; PC widescreen widens it to
  854 / 768 / 1138) and its own 640x480 coordinate system for every 2D layer (see "Widescreen"). Render targets: the
  back buffer and its depth, a second colour buffer the same size, the glow chain's four smaller targets (427x240,
  427x120, 213x120, 53x60 at 16:9: they follow the back buffer), all drawn by the GPU; the guest reads a target back
  only for its front buffer before a relaunch (PersistDisplay).
- Viewports, scissors and clear rectangles arrive in guest pixels; the projection (3D) and the 2D orthographic
  projection and XYZRHW positions are turned into clip space by the generated shaders (the guest viewport is in their
  constants), so a target's size only enters through the Vulkan viewport, the scissor and the clears.
- So the internal resolution is a host matter: draw each target at a scale while the game keeps its sizes. Enlarging
  the guest's back buffer instead (as widescreen did for width) would have changed the layout the client reads (its
  screen width, "%d_pain.spr"), the glow targets' memory and the 64 MB guest budget.

The Vulkan renderer (src/vulkan/vk_backend.cpp):
- Every render target is an image of guest size x scale (scale = internal resolution / back buffer guest size, per
  axis, never below 1); the Vulkan viewport is the whole image (clip space is size independent), scissors and clear
  rectangles are scaled from guest pixels. Textures the title draws from targets (the glow chain) are those images,
  sampled with the title's filters; the glow's 427x240 target is drawn at 1280x360 at 1440p, so the post effect keeps
  its proportions. A guest read of a scaled target gets it blitted down to the guest size first.
- The scale comes from PC Options > Resolution (pc_settings.ini resolution): Native (the window's pixel size, the
  display's in fullscreen), a size from the display's modes (SDL_GetFullscreenDisplayModes on the window's display,
  largest first, each size once), a WxH typed into the file (listed as custom), or Original (the title's own size).
  The size is fitted to the frame's aspect (4:3 or the widescreen aspect): a 4:3 frame at 2560x1440 is 1920x1440,
  pillarboxed at the present, a 21:9 frame at 3440x1440 is 3414x1440; within 0.5% the requested size is taken whole
  (854x480 is 16:9 to 0.06%).
- Applied at the next frame boundary (the targets are freed and made at the new size as the next frame draws them),
  so a change from the pause menu takes effect at once, no restart. The display mode itself is never changed (the
  window or fullscreen desktop stays as it is), so no setting can leave the display unusable.
- Fallbacks: an unparsable value is Native; a size past the device's limit (maxImageDimension2D, at most 8192) is
  reduced keeping the aspect; a scaled target that cannot be allocated is made at the guest size and the renderer
  stays at Original for the session (logged: "target ... failed, back to the guest size"). Native in a window
  smaller than the title's frame keeps the title's size (never below 1x).
- The software renderer draws on the CPU into the guest's own surfaces, at the title's size; Resolution does not
  apply to it (the frame is scaled at the present as before).

Checks (Vulkan window): 16:9 at 3840x2160 (back buffer 854x960 drawn x4.496 x2.25, frame 3840x2160), 4:3 at 2560x1440
(1920x1440), 21:9 at 3440x1440 (3414x1440), 16:10 at 1920x1200, 16:9 at 1920x1080; HUD, team / buy / pause menus,
spectator, the AWP's scope at 40 and 10 (crosshair at the centre, scope surround black to the edges), the intro
movies (overlay), decals and lighting: the same walk to de_airstrip's B sign at Original and at 2560x1440, scaled
down, match to 40.8 dB (only edges differ). The Hor+ FOV and culling are unchanged (they are in guest units).
Change from the pause menu: 2560x1440 to 2400x1350 at the next frame. Performance at 4K: 30 frames per second held.

## Mouse in the menus (2026-10-20)

What the menus are (CS 5659, the UI scripts FrontEndUI.txt / InGameUI.txt as loaded, CSX_UI_DUMP=1):
- No mouse support survives in the Xbox menu system. The executable keeps VGUI's cursor class, vgui_emulatemouse and
  the path valve/gfx/vgui/mouse.tga, but they belong to the VGUI panels (HUD), the texture is not on the disc, and
  nothing in the menu code reads a pointer.
- A screen (the open one at 0x5523D4) has an interactive table at +0x380 and a passive one at +0x384. A table keeps
  its layout from the script: the rectangle +0x1C..+0x28 and the inner one +0x2C..+0x38 (TableLocation /
  TableSize), row spacing +0x128, text scale +0x12C, the highlight image +0x130 (image, x, y, width, height; +0x144
  the unselected one), column widths +0x70; rows +0x60..+0x64 (0x20 each: +4 bit 0 disabled, +0xC..+0x10 cells of
  0x78, a list cell's choices at +0xC..+0x10 of 0x1C), the selection +0x9C, the first row shown +0x120 (scrolling),
  wrap +0x124. The table draw (0xCD030) puts row i at x +0x2C, y +0x30 + (i - first) x (int(font height 0x37A81C x
  text scale) + spacing); the screen draw is 0xCD230, a row's 0xCCD20, a cell's 0xCCA80.
- The selection moves through 0xD8360 (cdecl step, the table in esi: the new row clamped or wrapped, disabled rows
  stepped over, +0x120 scrolled to keep it shown); the input state machine (0xF7xxx-0xFAxxx) calls it for the d-pad
  and runs the screens' commands for the buttons (OnButtonDownA, RowSelect...).
- Other kinds: the buy menus (MenuType "buymenu") are pies of SelectBoxes chosen by the left stick's angle
  (PieceOffsetDegrees), the main menus ("superlauncher") are tables with stick pieces running tableMoveUp/Down, the
  profile name is a VirtualKeyboard. There are no sliders: volumes and the like are list cells with arrows. The help
  bar is a texture across the bottom (BackgroundImage at 0,411 640x69) named after its buttons right to left
  (Cancel_Accept_Reset: B rightmost, then A, then X); the in-game menus have none.

The mouse (src/game/pc_mouse.cpp on the game thread, src/linux/window_input.cpp in the window):
- The window maps the pointer into the menus' 640x480 (its letterbox at the frame's aspect, the menus' 640 columns
  in the middle of a wide frame), so it is right at any aspect and internal resolution; the pointer is SDL's own
  (the system arrow, as GoldSrc on a PC showed in its menus: the game has no cursor art).
- Tables: moving onto an enabled row selects it through 0xD8360; a click is the pad's A (the game's activation); on
  a setting's row a click on the arrows (the ends of the highlight image) is left / right, elsewhere it selects the
  row, and a double click is A (accepting the screen: the in-game screens have no help bar to click); the wheel is
  the d-pad's up / down (scrolling long lists, PC Controls); the right button is B. The help bar's buttons are
  clicked in slots measured on the art (488-585, 385-482, 292-380 across, 412-446 down).
- Buy pies: on the ring (42 to 160 pixels from the centre at 227,251, measured on the CT and T art) the left stick
  points at the pointer, so the game highlights that piece with its own art; a click is A, the right button B; the
  ammo tabs above are the triggers.
- The presses go through the pad as polled (GameInfo::pad, both the window and the headless harness): held three
  frames, two apart, so the game's sounds, transitions and commands run as for a controller. The pointer acts only
  when it moves, so the keyboard and a pad keep their selection; a pad or keyboard stick takes the pie back.
- Capture: playing, the mouse is captured (relative mode, the view); a menu or the console frees it with the pointer
  shown; leaving them captures it again (unless F1 let it go while playing; a click in the window takes it back). A
  click that closes a menu is not a shot (the button went to the menu); menu pointer motion never turns the view.
- Hover sound: the screen keeps its sounds' names at +0x8 EnterSound, +0xC SelectSound, +0x10 HighlightSound (parsed
  by 0xD4740), played by 0xC5A00 (cdecl: name, 0). What plays the HighlightSound originally: the stick pieces that
  move a table (Generic_4_x with CommandHighlight tableMoveUp / Down, 0xD8480 / 0xD8490 through the command table at
  0x37A990) play it (0xCAC90) before their command: the main menus ("superlauncher": every front-end screen with a
  selectable table, the pause menu's), team selection; the keyboard's d-pad on the main menu goes the same way (seen
  in the log of every menu sound: CSX_LOG_MENU_SOUNDS=1, a hook at 0xC5A00's entry); the virtual keyboard's d-pad
  plays it (0xD42F0), the buy pies from their stick (0xD0398). The generic screens (26 in the front end, 21 in game)
  have no HighlightSound and only passive tables; Spectator has one but no pieces. A hover that moves the selection
  plays the screen's HighlightSound only where the screen's own moves do (stick pieces moving its table, the virtual
  keyboard); the buy pies keep theirs from the stick the pointer drives.
- Audible, not only called: in the Vulkan window with the mixer captured (CSX_AUDIO_WAV, null device), the hover
  sound's waveform learnt from the game's own d-pad plays (aligned, averaged, high-passed against the menu music) is
  found at every mouse play: matched 7 to 28 times the level of quiet moments between plays (0.2 to 2.5 times), as
  strong as the d-pad's own plays (13 to 35 times, each measured against the others' average); the music's beat (0.7
  s) shows at control moments that fall on it (tools/check_menu_audio.py, drive.py `expect menuaudio`). Also heard
  in a play-test of the window build (2026-10-20).
- The virtual keyboard (SuperLauncher_CreateProfile's VirtualKeyboard, screen +0x38C): its grid's rectangle +0..+0xC
  (floats; 70,151 to 570,351: 5 rows of 14 columns, 35.7 x 40), the cell of row r, column c at +0x3C + (r x 14 + c) x
  0x14 (+0 its width in columns, +8 its UTF-16 label, only in a key's first column; +0xC its action: 0xD16C0 types
  the label's character, 0xD1370 Done, 0xD13A0 Shift (one shot), 0xD13D0 Caps Lock, 0xD14D0 Symbols, 0xD15A0
  Accents, 0xD1670 Space, 0xD1720 Backspace, 0xD1740 / 0xD1770 the cursor; +0x10 disabled), the selection +0xA38
  (the cell), +0xA44 / +0xA4C its row, +0xA40 the column moved from, +0xA48 the key's first column (as its d-pad
  moves, 0xD1040..., write them); the edit box +0xA5C (its text a std::wstring at +0x34: length +0x48, cursor +0x70;
  0xD04A0, stdcall with the edit box in esi, inserts a UTF-16 character and refuses past 15). The A press (0xD40F0's
  handler) calls the selected key's action and plays HighlightSound, or ErrorSound when refused.
- The name with the mouse and the PC keyboard: the pointer selects the key under it (the selection fields as the
  d-pad writes them, with the HighlightSound), a click presses it (the pad's A: the game's own key press); a click on
  the name field (the screen's EditBox rectangle, 110,113 460x26) starts typing: SDL text input (the layout's
  characters, Å and ö among them; the font has Latin-1) into 0xD04A0 with the key's sounds, Backspace and the arrows
  through 0xD0510 / 0xD1740 / 0xD1770, Enter is START (the screen's Done: menuCreatePlayerProfile), Escape or a click
  elsewhere ends typing; the keyboard is the pad's again then. The game's limit (15 characters) refuses the rest.
- Not done: the scoreboard's and spectator's screens have nothing to click (right button closes like B).

Window input details: a wheel event counts its whole notches (SDL's integer_y; a fast wheel or a fine one may give
several); the window logs its size ("window: size WxH", logical pixels: what pointer coordinates are in).

Checks: regress_pc_menu_mouse.txt (team selection, the pie: the Magnum bought, PC Options' arrow and double click,
Return to Game), regress_pc_menu_mouse_fe.txt (main menu with its hover sound, PC Options' arrows, Reset and Accept
on the help bar, PC Controls with the wheel, back), regress_pc_profile_name.txt (the virtual keyboard: a key by
pointer and click, the name field's typing, Backspace, the cursor, Escape, the 15 characters, Done); in the Vulkan
window (tools/run_window_tests.py, each in a headless gamescope): vk_menu_mouse.txt (21:9 and 2560x1080: the same in
game through the window's mapping, capture changes, Return to Game clicked without a shot fired, the keyboard still
navigating), vk_menu_mouse_fe.txt (the front end with SDL mouse and text events: the virtual keyboard and the name,
hover sounds found in the audio capture, PC Options and PC Controls, the right button, the console over the menu
taking the clicks), vk_menu_pointer_43 / _169 / _219 / _43_full.txt (raw window pixels to menu points computed
independently of the window's mapping: 4:3 and 16:9 windows, 21:9 letterboxed and 4:3 pillarboxed full screen,
points in the bars and beside the menus' columns doing nothing).

## Press-a-key rebinding (2026-10-20)

How the original menu holds a binding row (CS 5659, PC Controls as pc_menu.cpp declares it): a CellList cell (0x78
bytes: +0 the shown choice's index, +0xC..+0x10 the choices) whose choices are the game's std::wstring (MSVC, 0x1C
bytes: +4 up to 7 UTF-16 characters in place or, from 8 on, a pointer to them; +0x14 the length, +0x18 the capacity;
the parser widens the UI text's bytes, so the menus' text is Latin-1). The table draws the shown choice's string each
frame; nothing in the menu code reads keys other than the pad's.

The capture (src/game/pc_menu.cpp on the game thread, src/linux/window_input.cpp in the window):
- Starting: Enter on a PC Controls row (the window asks the game thread: a binding screen open with a row selected,
  csx::BindRowSelected), or a click on a row's key name between its arrows (src/game/pc_mouse.cpp). The arrows still
  step through the list; a click on the label selects, a double click is A (the pause menu has no help bar).
- Showing it: the shown choice's 0x1C bytes are kept and it points at "Press a key" (UTF-16 in a block of the game's
  heap from its own malloc 0x22D86A, made once), so the game's table draws it in its own font, highlight and arrows;
  the bytes are put back exactly when the wait ends (the game never frees or grows a string it did not make). The
  help lines say how: "Click the key or press Enter, then press a key or mouse button (Escape cancels). The arrows
  also choose one."
- Waiting: the window's next key-down (no repeats), mouse button press or wheel notch goes to the row and nothing else
  (no pad key, no menu move, no game key, no console toggle: the console's own key can be bound this way); Escape
  cancels; F1 / F11 stay the window's (F11 is handled before; F1 and other keys without an engine key are logged
  "cannot be bound" and the wait goes on). A double click's second press (SDL clicks 2) is not a key, so the click
  that started the wait cannot bind Mouse 1 (a later click does); Enter's own release and repeats are ignored. The
  pad does nothing in the menu meanwhile, its B cancels, and buttons held through the wait stay hidden from the menu
  until let go (B would otherwise leave the screen right after cancelling). Pointer moves and clicks leave the menu
  as it is. Leaving the screen or opening the console ends the wait.
- The key: its engine number is looked up in the row's list (pc_bindings.h's keys) and becomes the shown choice, as
  if stepped to with the arrows ("Jump set to K (saved with Accept)"). Accept saves as before (pc_settings.ini
  bind.<action>, applied to keybindings[] at once by pc_keys.cpp's sync); Cancel leaves without saving; Reset shows
  the defaults. A key chosen twice stays with the first row in the list and the later row becomes None at the save
  ("Wheel Up is already Crouch's, Previous Weapon is left unbound"), as for the lists.

Keyboard layouts: letters are bound as the layout prints them (keycode: AZERTY's A is a); the digit row and the
punctuation keys by their position (scancode), named as on a US keyboard for the engine: the key left of 1 is `,
Swedish Å is [, Ö ;, Ä ', the key left of Z (ISO keyboards) is < (an engine key number of its own, 60: bind takes a
single character as its number). The window passes the system layout's names for these positions
(CSX_KEY_LABELS, UTF-8 "C=NAME" lines from SDL_GetKeyFromScancode / SDL_GetKeyName); the menus show them in Latin-1
(Å, Ö, Ä, §: the game's font has them), a name outside Latin-1 falls back to the US one. New bindable keys: ', \, <,
Keypad . and Keypad /.

The lists' stepping (the pad's left / right, the arrows): tableIncreaseValue (0xDCAD0) and tableDecreaseValue
(0xDCB60) pass over a choice whose enable byte (cell +0x29 + index) is 0. The cell holds 0x27 of them (+0x29..+0x4F),
enough for the original lists; PC Controls' lists of over 100 keys read the cell's other fields and the next cell
past that, which skipped every fourth key from there (index 2 mod 4: -, then with the new keys [, \, Keypad /...).
Hooks at the two checks (0xDCB43, 0xDCBCB) take a choice from 0x27 on as enabled; the shorter lists are unchanged.

Engine notes: MOUSE4 / MOUSE5 (244 / 245) are in the engine's key names and fire their bindings. In a match started
from the front end the shot counter's Glock needs the Terrorist team (as regress_player_control.txt picks).

Checks: regress_pc_bind_capture.txt (headless: keyboard, mouse button and wheel keys, the pad's A ignored and its B
cancelling while waiting, Escape cancelling, an unknown key refused, the list still stepped by the pad, the
duplicate rule, the console's key, Accept; the title's relaunch into a match: the saved keys work and the old ones do
not; the pause menu's Reset and Accept restore Space), vk_bind_capture.txt (the Vulkan window with SDL events: Enter
not taken itself, a side button, a double click's second press not taken, the wheel, Escape, F1, a Swedish key shown
as Å, the console's new key opening it; after the relaunch the keys walk and jump; a rebinding in the pause menu
works at once on returning to the game).

## Play-test bugs: frame rate, PC Settings minimizing, menu pointer, loading screens (2026-10-20)

Frame rate (reported: capped at about 30):
- The cap was ours: the window paces presented frames to FPS=30 (the console presented every second vertical blank,
  presentation interval 0x80000002); the engine's own cap is fps_max (cvar 0x377D08, value 0x377D14, default 72),
  read by Host_FilterTime (0x5CD6B), which also holds 30 while loading and 100 when the cvar at 0x377DCC is set.
  `fps_max 1000` from the console did not change the value the engine reads (not traced); the value written
  directly does.
- Measured (headless, direct de_airstrip, Terrorist; drive.py `track`: the position every 5 ms): the game's clock
  keeps real time at every rate (spawn to Round_Start 11.76-11.77 s at 30, 60 and 72; round end to the next start
  17.2 s at 30 and 144); the Glock's shots for a run of presses are the same at 30, 60 and 144; bots buy, fight
  and plant at 144 over three rounds. The movement is not: CL_Move (0x2AE..) stores cmd->msec = (int)(
  host_frametime x 1000) (frame time double 0x6CE0E0, stored at 0x2AEDB) and the player moves by those milliseconds,
  dropping the fraction every frame:

  | Frames per second | Steady walk (units/s) | Why |
  |---|---|---|
  | 30 (the console's) | 247.5 | 33 of 33.3 ms |
  | 60 | 240 | 16 of 16.7 ms |
  | 72 | 234 | 13 of 13.9 ms |
  | 144 (computed) | 216 | 6 of 6.9 ms |

- Fix (src/game/pc_frame.cpp, config/game.toml, SDK patch 0027): PC Options > Frame Rate Limit (30 (Original), 60,
  72, 100, 120, 144, 165, 240) sets the window's pacing (app::SetFrameRate) and fps_max; above 30 a hook at the msec
  store carries the fraction to the next command, so the milliseconds add up to the time that passed. With it: 250
  units/s and a 45-unit jump at 60, 100, 144 and 240 (220 presented headless, the CPU's limit); at 30 the title's own
  rounding is kept (247.5, 45). Frame-keyed test presses ("press LS 60") last less real time at a higher rate.
- Missed because every test ran at FPS=30 and none measured speed against real time.

PC Settings minimizing (reported: entering or leaving PC Settings minimized the game):
- In the play-test the compositor made the window full screen (2400x1350 at 6 s, not through F11: no setting was
  written). Every save of PC Options (pc::Version changes) re-applied Display Mode: the setting said windowed, SDL's
  flag said full screen, so the window was made windowed and fell back to its tile (1181x643, then 581x643 beside
  another window), which looks minimized.
- Fix (src/linux/window_main.cpp): Display Mode is applied at start only when it asks for full screen, and later only
  when the setting itself changes (PC Options' row, F11); other saves leave the window alone. Each application is
  logged ("window: Display Mode ... applied").
- gamescope does not keep SDL's full screen flag, so vk_display_mode.txt checks the decision (no application on an
  unrelated save, on entering and leaving the screen; one when Display Mode changes), not the compositor's answer: a
  play-test on the desktop (Hyprland) is still needed.

Menu pointer (reported: sometimes off):
- Tables with a Header (team selection's blank header, the confirmations' question: InGame_ConfirmChangeMap,
  InGame_ConfirmRestartMatch, InGame_ConfirmQuit) draw it first, one row high, and their rows below it; the hit test
  took the first row at the table's top. The pointer was one row off on those screens: team selection pointed at
  Counter-Terrorist chose Terrorist; a confirmation's Yes, pointed at, answered No (the click landed on the question
  row, which the hit test called No). Tests had checked the hit test's own answer in the log.
- Fix (src/game/pc_mouse.cpp): the screen's Table Header is read from the menu text; its rows start one row lower
  ("pc mouse: screen ..., rows from X Y, ..., below a header").
- The window's mapping itself was right: raw window pixels at 4:3, 16:9, 21:9 letterboxed, 4:3 pillarboxed, and in
  the pause menu right after play land where expected, and the game's highlight is drawn there (pixel checks on the
  screenshots: the highlight bar dark throughout, the row beside it not). Each menu click is logged with the window
  point, size and frame aspect ("input: click at ...") for further reports.

Loading screens flickering (reported: severe flicker):
- Bursts of consecutive presented frames (the Vulkan window's "burst" shot request) during a match's loading showed
  the Counter-Terrorist and the Terrorist briefing in turn every frame. The title draws the loading screen about once
  per buffer (22 draws in its first two seconds) and presents again and again (Swap flags 0); its device is created
  with BackBufferCount 1, DISCARD and multisample type 0x2012 (the 640x960 super-sampled back buffer). The runtime's
  D3DDevice_Swap rotated every buffer's memory, so the back buffer the title drew into alternated between two
  memories holding two different pictures. On the console a multisampled back buffer is resolved into the display
  buffers at Swap and stays the render target, keeping what was drawn.
- Fix (SDK patch 0028, runtime/xdk/d3d8.cpp): a multisampled back buffer does not rotate; PersistDisplay and
  GetBackBuffer(-1) read it (it holds what was presented last). Both renderers take the frames from the same buffers.
- vk_loading.txt: no picture comes back every other frame (drive.py `expect steady`) while a match starts from the
  front end, after Change Map is confirmed, at Restart Match and at Quit back to the main menu.
- Missed because every capture was a single frame.

## Flashbang (2026-10-21)

The effect, from the title's code (the server and the client are both in default.xbe):
- Server: RadiusFlash 0x155170, from the flashbang's detonation. Every entity within 1500 units (FindEntityInSphere)
  that is a player, alive and taking damage; a grenade in water reaches only players in water, otherwise a player
  with the head under water (waterlevel 3) is left out. Line of sight: a trace from the grenade (raised one unit) to
  the player's body target must reach the player. Adjusted = damage - the distance to the trace's end x damage / 1500,
  at least 0. Facing (the view's forward against grenade - eye, dot >= 0): fade = adjusted x 1.5, hold = adjusted x
  0.667, alpha 255; facing away: x 0.75, x 0.5, alpha 200 (not PC CS 1.6's x 3 / 1.5 and x 1.75 / 3.5). Sent white
  with flags 0x10 through UTIL_ScreenFade (build 0x14B160, write 0x14B1E0, the ScreenFade message id at 0x55297C);
  bots are blinded through 0x17A140 instead.
- Client: the ScreenFade handler 0xB6500 keeps the fade at 0x74A244 (speed), 0x74A248 (fade end), 0x74A250 (hold end),
  0x74A254 (colour, alpha), 0x74A258 (flags), against the client time 0x74A290 (a double): FFADE_IN holds the alpha
  until the hold end, then falls linearly to 0 at the fade end; 0xB6630 computes it each frame (it clears flag 0x10
  the first time). Frame-rate independent by construction.
- The view code 0x4EFC7 (when 0x6E7AAC, only client draw, is 0) calls 0x4D860 (GoldSrc's R_PolyBlend; 0xB6630 is
  V_FadeAlpha) every frame, which draws it through the title's GL layer (0x6FF60 glBegin, 0x70ED0 glEnd, 0x71D40
  glVertex2f): alpha test off, blend on, depth test off, glViewport(glx, gly, glwidth, glheight) and glOrtho(0,
  glwidth, glheight, 0, -99999, 99999) from the GL size (0x6CE528, 0x6CE27C, 0x6CE3C8, 0x6CE274), SRCALPHA /
  INVSRCALPHA (FFADE_MODULATE: ZERO / SRCCOLOR), untextured, glBegin(GL_QUADS) with the corners 0, 0 to glwidth,
  glheight, then the state put back. One technique: a full-screen quad after
  the world and before the HUD (the HUD stays readable over the white); no gamma ramp, no texture. The glow post pass
  and the dynamic gamma's measurement come after it, so the measurement sees the white, as on the console.

Why it was missing in both renderers (two causes in the shared runtime; the title's path ran, its quad reached the
renderers with the right colour, blend and vertices):
1. ZEnable set inline. glDisable(GL_DEPTH_TEST) (0x708E4) is the library's D3DDevice_SetRenderState_ZEnable copied
   (its 0x30C / 0x1D78 push and the D3D_RenderState entry 0x317234), not a call, so the runtime kept depth testing
   on. Four more copies: 0xB9A53 (in the full-screen pass 0xB9880), 0xBE476 (the dynamic gamma's 0xBE1D0), 0xC0FF5
   and 0xC1313 (the glow passes of the post pass 0xC2160): those passes were depth tested too. SDK patch 0032:
   SyncDeferred reads ZEnable from the table before every draw (every writer, the library's own and the copies,
   keeps it).
2. The near/far cull range. The GL layer turns glDepthRange(n, f) into MinZ = -(n+f)/(f-n), MaxZ = (2-n-f)/(f-n):
   the 3D view's (0, 1) is MinZ -1, MaxZ 1, which the fade inherits (glViewport keeps the device's MinZ / MaxZ at
   device+0xEE0, mirrored by the runtime), so glOrtho's z 0 lands at depth -1. The library's lazy state (0x304884)
   sets NV097_SET_CLIP_MIN / MAX to MinZ and MaxZ times the Z scale for the fixed function (0 and the Z scale for
   vertex programs and the W buffer), and D3DRS_DEPTHCLIPCONTROL (1 here, D3DDCC_CULLPRIMITIVE) culls outside it:
   the console keeps the quad, both renderers culled it at 0 to 1. SDK patch 0033: FixedDepthCullRange gives the
   console's range; the reference renderer clips and culls with it (the depth clamped to the buffer's for the test,
   compared to within a millionth of the limit: the quad's vertices are on it exactly), the generated fixed function
   vertex shaders move z into it (DrawConstants::zcull) and the Vulkan viewport's depth range takes it back
   (VK_EXT_depth_range_unrestricted; without the extension the range stays 0 to 1).
Found by: the push buffer the quad writes (its combiner setup, begin / end, four 2-float positions) and the draw as the
runtime saw it (z enable 1); with that fixed, the reference renderer's pixel probe (no fragment from the quad) and
its shaded vertices (depth -16777215, -zscale).

Results (Vulkan, de_dust2, a flashbang at the feet while looking at it):
- White during the hold (99.9 % of the playfield at 250 or above; the HUD over it), then a fade to the scene; the
  fade lasted 7.233 s against the 7.225 s it was sent with, real time within 1 ms of client time. The measurement
  saw the white (mean 0.974, the ramp's high point to 0.605) and the exposure came back (middle 0.193 -> 0.201, high
  0.375 both); picture and HUD within 0.6 of their luminance before; Auto Exposure Off (identity ramp, no
  measurement) and On with the brightness cvar at 1 alike; the pause menu right after a flash as before.
- Frame rates (vk_flashbang_rates.txt; hold and length differ by where the grenade lands, the server's falloff):

  | rate (VSync) | presented | hold s | length s | lasted s (client / real) | exposure middle before -> after |
  |---|---|---|---|---|---|
  | 30 (on) | 30.0 | 2.223 | 7.226 | 7.239 / 7.239 | 0.193 -> 0.198 |
  | 60 (on) | 60.1 | 2.331 | 7.576 | 7.567 / 7.567 | 0.198 -> 0.202 |
  | 120 (off) | 119.5 | 2.279 | 7.406 | 7.393 / 7.393 | 0.202 -> 0.203 |
  | 144 (off) | 143.9 | 2.274 | 7.390 | 7.373 / 7.374 | 0.203 -> 0.202 |
  | 240 (off) | 236.2 | 2.273 | 7.388 | 7.369 / 7.369 | 0.202 -> 0.205 |

  Every fade ends within one frame of its end (the alpha truncates to 0 in its last 20 ms at the higher rates).
- Reference renderer (regress_flashbang.txt): white during the hold, the scene after. Headless the exposure after a
  flash looks dark for a while: the measurement samples the last snapped frame, which was the white one (a harness
  artifact; the window measures every frame).
- Regression: the headless suite (22 scenarios) and the window suite (20 tests) pass. vk_auto_exposure.txt, written
  while the white was missing, failed once: its `exposure recovered` read the first white measurement, 12 s old,
  because drive.py's `exposure` and `expect quiet` marked the log's end without reading it first (a sleep does not
  read it); both read it now, and the test passes (middle 0.177 before the flash, 0.173 after).

Full-screen coverage (play-test, 16:9 at 3840x2160 fullscreen: the white did not cover the whole screen):
- Cause: the PC widescreen frame, not the renderers. R_PolyBlend runs inside the VGUI paint (0xB4560), which sets the
  GL size to its 640x480 root panel around its drawing (as found for R_SetFrustum, "Widescreen"); the fade takes its
  viewport, projection and quad from the GL size, so on an 854x480 frame it drew 640 columns from the left edge, not
  centred and not stretched: at 1706x960 the white ended at column 1277 (640 / 854 of the width), the right quarter
  uncovered; at 21:9 the right 44 %. At 4:3 the panel is the whole screen, so the original was never affected. The
  viewport, scissor, render target, swapchain, internal resolution, fullscreen and windowed scaling were as for
  every other draw (the shot is the game's frame; the present's letterbox is outside it).
- Fix (src/game/pc_wide.cpp PcWideFade, hooks at 0x4D877 and the function's return 0x4DAA7): while R_PolyBlend draws a
  fade on a wider frame inside the paint (GL size 640x480 at 0, 0), the GL width is the frame's, and 640 again when it
  returns: its own glViewport, glOrtho and quad then span the frame, the same colour, alpha, blend, timing and order.
  Nothing else reads the GL size in between; nothing changes at 4:3 or with no fade. The other users of the GL size
  were checked: R_SetFrustum (already given the view's size), GL_Set2D (PcWide2D), R_SetupGL (a bounds compare),
  0x47940 (a centred picture) and SCR_CalcRefdef: no other full-screen effect takes it (the glow and the dynamic
  gamma's measurement use their own targets, which follow the back buffer). HUD and menus are untouched.
- Checked (drive.py `expect coverage`: nine patches touching the frame's edges, centre, edge middles, corners, their
  medians; the fade's opacity (shot - base) / (255 - base) against the same view once the fade is over, Auto Exposure
  off so the frames compare directly): Vulkan 4:3 windowed 1152x960, 16:10 1536x960, 16:9 fullscreen at 3840x2160 and
  21:9 2275x960, reference renderer 4:3, 16:9 (854x960) and 21:9 (1138x960): opacity 1.00 in every patch in the hold,
  0.5 to 0.9 in the fade, every patch covered (a missing strip reads about 0).
  Hold 2.223 s and overruns -0.02 to +0.01 s as before. Headless suite 24 of 24 and the window suite 24 of 24 (the
  four coverage tests added; vk_flashbang_rates failed once on its 120 fps buy, the title's buy menu not opening, and
  passed on its own).
- Found on the way (not changed, it is the bloom): the bloom's last pass, an additive full-screen triangle drawn after
  the fade (draw 1183 at 4:3), keeps the scissor its earlier passes set for the small targets (SetScissors at 0xC15F7
  / 0xC1E8E, inclusive): only SetScissors programs the surface clip (method 0x200) and only SetViewport resets it
  (0x306428); SetRenderTarget does neither (wrong: its tail jump ends in SetViewport, so it clears them; fixed in
  G5, "Bloom"), and the title sets no viewport before that pass. Both renderers keep it,
  the reference renderer probed (bounds 0, 0 to 160, 60 of the 640x960 back buffer; a quarter of the width at 16:9 and
  21:9), Vulkan from the same state (vk_backend.cpp's scissor). Its input is near black in normal play; under the fade
  it adds the white's glow to that corner, which is brighter during the fade (0.85 against 0.5 opacity headless). Whether
  the console clips it the same way needs a console capture; left for the bloom work.

Remaining uncertainty: no console capture of the flash; the Vulkan depth buffer is D32F, so fixed function fragments
between MinZ and 0 are tested and written below 0 instead of clamped as the console's integer buffer would (the fade
draws with the depth test off; nothing else seen lands there); whether CLIP_MIN is inclusive on the NV2A (the quad's
vertices are exactly on it and the title relies on it being drawn); D3DDCC values other than 1 and reversed ranges
(MaxZ below MinZ) are left to 0 to 1 (not used by this title).

## Dynamic gamma (auto exposure, 2026-10-21)

The title's algorithm (0xBDFC0 and below, read in full):
- 0xBE1D0 makes a 40x60 LIN_L8 surface at this+0x410 (the back buffer's size over 16, format 0x13, contiguous
  memory) and a CPU buffer at this+0x414, and draws the back buffer into the surface through the luminance pixel
  shader (0.30 / 0.59 / 0.11, blue to alpha). A state at 0x5BD990 draws one frame in three: 0 draw, 1 wait for
  D3DResource_IsBusy, 2 rest.
- Every frame (any state) 0xBE760 locks the surface inline, copies its bytes and builds a 256-bin histogram: the dark
  percentile against cl_dynamicgamma_darkthreshold (this+0x404), the light one against _lightthreshold (+0x408), the
  mean (+0x40C), all 0..1. The title is built to read a result a frame or more old.
- 0xBE960 moves the ramp's low (+0x424), high (+0x428) and middle (+0x42C) points towards dark, light and mean by
  _darkadjustdt / _lightadjustdt / _contrastadjustdt per second times the frame time, then clamps: low in
  [0, _lightclamp], high in [_darkclamp, 1], middle between them. 0xBEB70 writes the ramp (low -> 0, high -> 255, the
  middle with _bias) into the device's ramp slot, flagged for the next Swap; when the flag at this+0x430 is not 1 or
  the game is in a menu (0x551854, 0x5523F0) 0xBE15A writes the identity ramp instead (the pause menu shows it).
- The clamps and bias are not the cvar defaults: they differ per map (de_dust2 high floor 0.375; de_inferno 0.200,
  low ceiling 0.150, bias 0.750), so on these maps the high point rests on its floor and the low and middle points
  follow the scene. Where the per-map values come from was not found (no code reads the names; the map packs are not
  searchable as text).
- cl_dynamicgamma 0 skips 0xBDFC0: nothing moves the ramp, which stays as last set; the title's own non-adaptive ramp
  is the identity (0xBF030, set through 0xBF0E0 on its ways out of a match), the back buffer shown as drawn: about
  half bright (de_dust2 at the spawn 44 mean luminance against 73 adapted).

Why the reference renderer sank to black (all three in the renderer, none in the title or its timing):
- Its surface planes had two cases, 32 bit and "16 bit RGB565": an 8 bit target got a 565 value written over two
  bytes at each pixel. The bytes the title read were the 565 values' low bytes (blue plus the low green bits shifted
  up): for a grey picture noise over 0..255, so the dark percentile came out high, the low point rose to its ceiling
  and the high point stayed up: everything below 15-40 % of the back buffer black.
- The headless backend rasterises only snapshot frames (and REF=N ones); clears run on every frame, so on all others
  the title measured a cleared (black) target.
- Snapshot frames take about a second: the title's frame-time steps are large then (the "gamma fade after a long
  frame" of "Rendering accuracy").
- SDK patch 0030: an 8 bit plane stores the blue channel (the console's B8 surface) and loads it as grey; draws into
  8 bit linear targets run on every headless frame (cheap: 2400 pixels), sampling what the last drawn frame left in
  guest memory. After the first drawn frame the headless measurement agrees with Vulkan's (de_dust2 spawn: mean 0.216
  against 0.214, ramp 0.092 / 0.375 / 0.210 against 0.080 / 0.375 / 0.219).

The Vulkan read-back:
- The title's lock is inlined (Lock2DSurface, D3D_BlockOnResource are 0 in config/game.toml), so a [[hook]] at
  0xBE777 (where 0xBE760 loads the surface; src/game/dynamic_gamma.cpp) calls d3d::ReadLate (SDK patch 0031), which
  goes to the backend's SyncLate as a NOOVERWRITE Lock2DSurface does. Only that surface is read; no other target is
  copied.
- SyncLate = DeliverLate + RecordLate (src/vulkan/vk_backend.cpp). RecordLate copies the target, when it was drawn
  or cleared since the last copy (Target::drawn), after the frame's draws so far: shrunk to its guest size (it is drawn
  at 72x60 at the internal resolution) and copied into one of two small host-visible buffers per target, with a
  barrier for the host. DeliverLate writes the newest copy whose frame's fence has signalled into guest memory (LIN_L8:
  the blue byte of each pixel; 32 bit targets: rows as they are), never waiting; until one has finished the title
  reads memory as it is, as on the console.
- Measured: a copy per measurement draw (every third frame) and each delivered once (4183 copies, 4182 delivered over
  a run); no synchronous read-back. Frame times with and without it (vk_auto_exposure_rates.txt, readback switched
  off with CSX_DYNGAMMA_READBACK=0): limit 30: 30.00 / 30.00 frames/s; 120 (VSync): 120.0 / 119.9; 240 (VSync off):
  237.7 / 238.9 (run to run noise); GPU 0.54-0.58 ms and recording 2.6-2.7 ms per frame alike.

Results (Vulkan):
- The measurement reads the frame (de_dust2 spawn: dark 0.086, light 0.294, mean 0.214) and the ramp settles on it
  (low 0.080, middle 0.219, high on the map's floor 0.375) and stays there over time (no change in 10 s; the same view
  comes back to the same points through a 2.5 minute run: middle 0.219, 0.218, 0.226 at its three rates). Looking at the floor: mean 0.178, middle follows to 0.171; the sky: 0.218 / 0.221; back
  level: within 0.02 of before. The pause menu shows the identity ramp and the adapted one returns after it.
- Frame-rate independent: at 30, 120 and 237 frames per second the points settle within 0.013 of each other and track
  the measurement at each rate.
- Before / after (vk_audit_effects.txt shots, 4:3): de_dust2's spawn views from mean luminance 145-171 (flat,
  washed out: the ramp frozen at its brightest) to 102-135, contrast (deviation) 39-60 -> 47-80; de_inferno (night)
  from 89-99 to 61-69, contrast up 2-5.
- PC Options > Auto Exposure (On by default, pc_settings.ini "auto_exposure"): the title's own cl_dynamicgamma, set
  through the console's command when the setting changes (and once at the start, sent again until the cvar holds it);
  a console change stands until the setting changes again. Going Off sets the title's identity ramp (0xBF030) once.
  Note: cl_dynamicgamma 0 on the match's command line (releasemap) did not stop it; changelevel did not reload a map
  in these tests, so whether a map load sets cl_dynamicgamma back is not known (the setting re-sends only on change).
  G6: it does: each map's light config sets cl_dynamicgamma 1 as the map starts, after the command line; Off is now
  taken back there (FINDINGS.md "Auto Exposure at the start").

Found on the way: the flashbang's white-out is not drawn in either renderer. A flashbang that blinds the player sets
the client's screen fade (ScreenFade at 0xB6500: white, alpha 255, held then faded over about 4 s); the fade routine
0x4D860 runs (alpha 255 at 0x4D86D, from the view code 0x4EFC7) and draws a full-screen quad through the GL layer
(depth test off, SRCALPHA / INVSRCALPHA, glColor4ubv), yet the frame shows nothing: a 45-frame burst through the fade
and a draw list of a frame inside it (no draw with the depth test off) in Vulkan, a reference snapshot at alpha 255.
The cause is in the shared push-buffer path (not the gamma ramp, which the flash does not use). Fixed: "Flashbang"
above (an inlined ZEnable and the fixed function's near/far cull range; it is held about 2 s, then fades over 5).

Remaining uncertainty: the console's exact response (how fast and how far it adapts on hardware) is unverified
without a capture; the per-map clamps' source; whether a LIN_L8 render target writes blue on the NV2A (the shader
writes the same value to every channel here, so it does not matter for this title).

## Graphics fidelity audit (2026-10-21)

Question: which of the console's graphical effects are missing or wrong in the Vulkan renderer. No console capture
or emulator footage is available here (reviews of the time only say the lighting and shadows were reworked for the
Xbox), so "original" is established from the title's own code and data: its render cvars and the passes it
submits, read at the API level, with the reference renderer (CPU, harness) as a second, independent reading of the
same submissions. Where both renderers agree and the code shows no other path the effect is classed as correct;
where only hardware could tell, uncertain.

Method:
- CSX_VK_AUDIT=1 (src/vulkan/vk_backend.cpp Audit): every simple / deferred render state value, texture stage state,
  primitive and texture kind / format / size logged the first time a draw uses it; the passes into small targets
  (glow chain, measurement) with their pixel shader words; each frame's additive draw count when it changes;
  texture re-uploads; gamma ramp changes; the small targets saved every 600 frames.
- 8 maps (de_aztec, cs_office, de_inferno, de_vostok, de_chateau, de_dust2, de_tides, cs_havana) plus de_airstrip
  through the front end and directly, in both renderers with the same console-driven steps: four views at the
  spawn, a pistol shot, an HE grenade, a flashbang and a smoke grenade bought from the buy pie and thrown
  (tests/scenarios/vk_audit_effects.txt and its headless twin audit_effects.txt; empty matches,
  tests/launch/*_nobots). Console variables changed at run time with `pckey cmd` (cl_hdrbloom, cl_hdrblur,
  cl_dynamicgamma). noclip does not work in this build (sv_cheats 1 on the command line too): the camera stays
  where the spawn and the spectator's free look reach.
- The title's render cvars and their defaults, from the cvar records in the XBE (name, string, flags, value):
  cl_hdrbloom 0.6, cl_hdrblur 1, cl_dynamicgamma 1 (with _bias 0.8, _darkclamp 0.3, _lightclamp 0.4,
  _darkadjustdt 0.15, _lightadjustdt 0.07, _contrastadjustdt 0.04, _darkthreshold 0.03, _lightthreshold 0.06),
  r_shadows 1, r_dynamic 1, r_detailtextures 0, gl_overbright 1, gl_envmapsize 256, r_glowshellfreq 2.2. The
  per-frame post pass (0xC2160) runs the bloom (0xC06D0, 0xC1FA0, 0xC0D00) when cl_hdrbloom is not 0 and the
  dynamic gamma (0xBDFC0) when cl_dynamicgamma is not 0.

What CS submits (all maps): triangles, strips, fans, quads, quad strips and polygons (no points or lines);
programmable vertex shaders and pixel shaders (texture modes project2d, bumpenvmap, dependent AR / GB); fixed
function lighting and hardware fog never enabled (D3DRS_LIGHTING 0, FOGENABLE 0 on every map); blends SRCALPHA /
INVSRCALPHA and ONE / ONE with ADD; alpha test; polygon offset; anisotropic filtering x4 and GAUSSIANCUBIC;
textures DXT1/3/5, R8G8B8A8, X8R8G8B8, V8U8, render targets LIN_A8R8G8B8 and LIN_L8; one cube texture (de_aztec).
No point sprites, CopyRects, volume or depth textures, or draws reading their own target: none of the backend's
unsupported paths is reached except the cube texture, and no draw is dropped.

Effects:

| Effect | Class | Evidence |
|---|---|---|
| Dynamic gamma (auto exposure, cl_dynamicgamma) | 2: incorrect in Vulkan (and in the reference renderer) | below |
| de_aztec water | 2: an input missing in Vulkan (fixed in G4, "Water") | below |
| HDR bloom and blur (cl_hdrbloom, cl_hdrblur) | 1, a filter detail 2 | below |
| Muzzle flash | 4: drawn, not seen in captures | below |
| Dynamic lights (r_dynamic) | 4 | below |
| Lamp glows, light sprites | 1 | "Lamp glows and bright areas" (current colour fix) |
| Character shadows (r_shadows, blob quads) | 1 | "Shadows and HUD colours"; no shadow maps exist (no depth textures used) |
| Fog | 1 | hardware fog never enabled; de_airstrip's fog is sprite geometry, 25 dB against the reference |
| Smoke, explosions, grenade effects | 1 | HE fireball, smoke cloud and the flashbang's burst in both renderers alike on de_dust2 |
| Flashbang white-out (ScreenFade) | 3: missing in both renderers (found in G2); fixed in G3 | "Flashbang" above |
| Bullet impacts and decals | 1 | "Decals" (polygon offset); impact marks in both |
| Transparency and blending | 1 | every blend factor and op in use is mapped; HUD, sprites, glass alike in both |
| Texture filtering, mipmaps | 1 (GAUSSIANCUBIC 2) | full mip chains, anisotropy x4 honoured, LOD bias 0 |
| Rain, surface bump water (V8U8, dependent reads) | 1 | all maps' 64x64 V8U8 pass alike in both |
| DXT1 noise (D3DRS_DXT1NOISEENABLE 1) | 4 | a hardware dither of DXT1 decoding, not emulated by either renderer |
| Multisampling (2x vertical back buffer) | by design | replaced by the internal resolution (native-resolution work), not an effect |

**Dynamic gamma.** Every frame the title draws the back buffer through a luminance pixel shader (c0 = 0x004D961C,
the 0.30 / 0.59 / 0.11 weights, AB dot product, blue to alpha) into a 40x60 LIN_L8 target, waits for it
(D3DResource_IsBusy at 0xBE048), locks it (0xBE7AA) and builds a 256-bin histogram (0xBE760): dark and light
percentiles against cl_dynamicgamma_darkthreshold / _lightthreshold and the mean, from which 0xBE960 / 0xBEB70 move
the gamma ramp (written into the device's ramp slot and flagged pending, 0xBE15A, picked up at the next Swap). The
lock is inlined (Lock2DSurface and D3D_BlockOnResource are 0 in config/game.toml), so no HLE call tells a renderer
the CPU reads the target:
- Vulkan: the target lives only on the GPU (drawn as BGRA8) and is never written back (Sync writes back linear 32
  bit targets only, and is not called for it: CSX_VK_AUDIT logged no lock). The title reads untouched guest memory;
  the ramp is set once, at team join, to its brightest (16 -> 79, 32 -> 159, 64 -> 255 on de_inferno) and never
  changes again: 4 ramp changes in a whole match (the last one the join, not a flashbang as first read). The picture is flat and washed out,
  the HUD too (it is drawn before the ramp). Changing cl_dynamicgamma 0 / 1 in the console changes nothing.
- Experiment (not kept): the 40x60 target written back to guest memory at every present (its blue channel per byte).
  The ramp then follows the scene (16 -> 38, 32 -> 128 on de_inferno, further steps as the view changes) and the
  night map turns darker and more contrasted (mean luminance 99 -> 67).
- The reference renderer draws into guest memory, so the title reads real values there, but its ramp sinks towards
  black within a minute on all 8 maps (de_inferno 102 -> 3 mean, de_dust2 155 -> 52; it was also the "gamma fade
  after a long frame" of "Rendering accuracy"), with speckle noise at lamps. Cause not found yet: the reference's
  slow frames (large frame times in the adjustment), its L8 target contents, or both.
- Classed incorrect in both; the highest visual impact of the audit (the brightness and contrast of every frame).

**de_aztec water.** (Corrected by "Water" below: the title's D3D turns stage 0's mode into the cube mode for a cube
texture, and the reference renderer samples it as a cube, not its first face.) Its water pixel shader (texture modes
0x1821: stage 0 project2d, stage 1 project2d, stage 2 bump env map) samples stage 0 as a 2D texture while a 32x32
X8R8G8B8 cube texture is bound there, takes
dot(expand(v0), expand(t0)) into r1 (combiner 0, RGB output 0x20D0) and makes the water's alpha from r1 (alpha
combiner 1, 0x0DD1D22D), its colour from the bump-mapped stage 2. Vulkan binds no cube texture (a placeholder takes
its place), so the alpha term is wrong; the reference renderer samples the cube's first face. Only de_aztec of the
9 maps binds it (de_aztec also has a 128x128 V8U8 bump map of 3 levels). How the console samples a cube texture
through a 2D texture mode is unverified (the reference's first-face reading is an assumption); the water is not in
view from the spawns, so its picture is still to be compared.

**HDR bloom.** The chain runs in Vulkan as on the reference: the back buffer into 320x240 through 2 x t0 - 1 (the
bright pass: only back buffer values above 0.5 survive; the title keeps the back buffer around half brightness and
the ramp brings it up), two blur passes with weights 1/3 and 1/6 at offsets 1 and 3 texels (320x120, 160x120;
cl_hdrblur scales the offsets), added back with ONE / ONE and the blend colour cl_hdrbloom. At the spawns the bright
pass is near black (means under 3 of 255 on de_dust2 with the sky in view), so the bloom adds little:
cl_hdrbloom 0, 0.6 and 3 differ in under 0.1 % of pixels. That is the title's design given what it draws, and both
renderers agree. One detail: the first downsample samples the back buffer with GAUSSIANCUBIC (D3DTEXF 5), which the
Vulkan sampler draws as linear (the console's wider kernel is not emulated). (G5, "Bloom": now the convolution's 3x3
Gaussian in both renderers; the composite's leaked scissor fixed.)

**Muzzle flash.** On each shot the frame gains additive quads: a 32x32 DXT1 sprite for 2 frames and an 8x8 one for
about 10 (MODULATE by the current colour, depth tested, no depth writes). Frame bursts taken during them show no
bright flash at the pistol; whether the sprite is dim by design (its colour is the engine's) or wrongly coloured
needs a frame-exact comparison with the reference renderer, which the snapshot timing does not give.

**Dynamic lights.** Firing and explosions change no lightmap (no texture is uploaded again after its first upload,
apart from a V8U8 map) and add no world pass. The world is the push buffer recorded at load ("In-game rendering"),
which is never recorded again. Whether the console lights the world with r_dynamic's lights at all is unknown
without a capture; both renderers agree.

Other observations:
- The spectator screen (after dying, Free Look) draws the menu's backdrop art over the view and one status line in
  garbled characters (where "Enemy: name" was); not a graphics effect (the UI's text), noted in TODO.md.

Recommended order: (1) dynamic gamma in Vulkan (a write-back of the LIN_L8 measurement for the title's inlined lock,
then checking the adaptation on several maps; the reference's collapse alongside); (2) de_aztec's cube texture
(bind cube textures; done in G4, "Water"); (3) GAUSSIANCUBIC; (4) the muzzle flash comparison and dynamic lights, only with
evidence of a difference.

Toggles: the title's own cvars switch the optional effects cleanly and are the right layer for them:
cl_dynamicgamma 0 (the static ramp from the brightness settings) and cl_hdrbloom 0 (the bloom passes skipped).
Shadows (r_shadows) are basic legibility and the water's terms are part of its shader: neither should be switched
off. A master "Xbox Graphical Effects" setting can set both cvars; separate "Bloom" and "Auto Exposure" settings are
justified only once the dynamic gamma works in Vulkan (today switching it does nothing visible).

## Water (de_aztec, 2026-10-22)

The river on de_aztec is the only water of the 9 maps drawn with this shader (and the only cube texture CS binds).
Its draws, read from the reference renderer at a fixed camera over it and from the title's code:
- Surfaces: 270 quads at z -528, x -624 to 1920, y 576 to 912 (the river and the stretch into the tunnels), each
  drawn twice in a frame with the same state (both draws blend; the second sits on the first). Drawn with the world,
  before entities, sprites and particles (splash and mist particles cross it later in the frame).
- Vertex program (B0064BF1, v0 position, v9 texture coordinates): t0 = c5 - v0, the eye less the vertex (c5 is the
  eye, its w 1, so t0.w = 0); d0 = c6 * c4 + c6 = (0.5, 0.5, 1, 0.5), the water's normal (0, 0, 1) as a colour; t1,
  t2 and t3 = v9 scaled by c7 to c9 (the bump map's and the water texture's tiling).
- Stages: 0 the cube texture (32x32 X8R8G8B8, one level, at 82EE2000; clamp, linear, no mip), 1 the 128x128 V8U8
  bump map (3 levels), 2 the water texture, offset by stage 1 through the bump env matrix; 3 unused. Texture modes
  0x1821 as set: stage 0 PROJECT2D, 1 PROJECT2D, 2 BUMPENVMAP.
- The mode stage 0 really gets: the title's D3D sets the shader stage program (NV097 method 0x1E70) lazily before a
  draw (the inlined code at 0x30F0B1 to 0x30F2CD, one block per stage): a stage with no texture gets mode 0 (except
  PASSTHRU, CLIPPLANE, DOT_ZW, DOTPRODUCT), modes 1 to 3 become 3 (CUBEMAP) when the bound texture's format has the
  cube bit (4), 2 (PROJECT3D) for a volume texture and 1 (PROJECT2D) otherwise, and DOT_STR_3D / DOT_STR_CUBE
  likewise. So the console samples stage 0 as a cube along t0.xyz (q unused, which matters: t0.w is 0). The SDK's
  pixel key does the same (sdk/runtime/gpu/shaders.cpp MakePixelKey, "as D3D's lazy shader stage setup does"); the
  audit's "samples stage 0 as a 2D texture" read the mode as set, not as sent.
- The cube's six faces lie one after another in guest memory (+X -X +Y -Y +Z -Z, each a whole mip chain, 128-byte
  aligned: FaceSize), swizzled like 2D textures; their texels are the D3D convention's (+X face: s = -z, t = -y, and
  so on): face 0's centre holds +X, its top left (u 0, v 0) +X +Y +Z. It is not a unit normalisation cube: the texel
  along V is V |V_major| / |V|^2 ((s, t, 1) / (1 + s^2 + t^2) on each face; a face's centre (0, 0, 1), its corners
  (+-1, +-1, 1) / 3), checked against the texels at both and at traced pixels.
- Pixel shader: r1 = dot(expand(v0), expand(t0)) = r, the cube texel's z (the normal is up); alpha = r * c2.a + (1 -
  r) * c3.a with c2.a 0.102 (26/255) and c3.a 0.302 (77/255): 0.10 looking straight down, 0.30 at grazing angles (a
  Fresnel term); colour = the bump-offset water texture (stage 2). Blended SRCALPHA / INVSRCALPHA, depth tested, no
  depth writes.
- Vulkan bound no cube texture ("cube and volume textures are not drawn yet"): stage 0 had no texture, its mode
  became 0, t0 read (0, 0, 0, 1), r = -1 and the alpha was 0.302 at every angle: a grey film over what lies under it
  when looking down at it. The reference renderer was right: its key has stage 0 in the cube mode and it samples the
  cube along t0.xyz with the D3D face convention (its "first face" path is only for a cube read in the 2D mode,
  which the key never leaves); the audit's statement was wrong.

Camera for tests: noclip does not work, and no spawn sees the river. tools/drive.py `setview X Y Z YAW PITCH` holds
the player at an origin with view angles (src/game/state_probe.cpp): the server's origin of client 0's edict
(svs.clients at 0x636CE4, client_t::edict +0x4B9C, the entvars origin at +0x88, velocity +0xA0) written every 20 ms
with no velocity, and the client's view yaw and pitch (0x552C10, 0x552C14) set once; "setview off" lets go. Nothing
in the game's movement or collision changes. (The client copy 0x3AE3E0 that `state` reads is overwritten from the
server every frame; poking it does nothing.) Views used, map de_aztec, no bots, Auto Exposure off, bloom on:
- graze: 100 744 -490, yaw 0, pitch 12 (looking along the river, 55 units above the water);
- steep: 800 800 -330, yaw 0, pitch 70;
- back: 1700 744 -470, yaw 180, pitch 25.

Fix (src/vulkan/vk_backend.cpp EnsureTexture, MakeImage): a cube texture is made a cube-compatible image of six
layers with a cube view, each face's levels decoded from guest memory with DecodeLevel(d, level, face) (the decode
the reference renderer uses) into layer face; the shader generator already declared TextureCube and sampled along
tc.xyz for it. Volume and depth textures are still not drawn (none in CS). The sampler is the stage's (clamp,
linear).

Checked:
- Reference renderer, exact (tests/scenarios/regress_water*.txt, drive.py `expect probealpha`): the water's alpha at
  the pixel 200 columns right of the centre and 160 rows below it (of 960; the same ray at every aspect) is graze
  0.245, steep 0.161, back 0.231 against 0.244, 0.157, 0.230 from the formula above with the cube's texels (a unit
  normalisation cube would give 0.227, 0.132, 0.194, no cube 0.302), at 4:3, 16:9 and 21:9 alike. The three rays hit
  the -X, +Z and +X faces: face order, orientation and decoding as above.
- Vulkan against the reference (drive.py `expect chroma`: mean chroma of a box of water, how much of the colour of
  what lies under it shows through): graze 17.8, steep 40.9 to 41.5, back 13.7 at 4:3, 16:9 and 21:9
  (tests/scenarios/vk_water_43/169/219.txt; 1152x960, 1706x960 and 2275x960 frames), the reference 17.3 to 17.4,
  38.4 to 39.4, 13.6; Vulkan before the fix 15.4, 29.8, 10.8 (outside the bounds).
- Nothing else changed: the same views before and after the fix with the bloom off (cl_hdrbloom 0) differ only where
  water is drawn (the river, the shallow water over its banks, the stretch into the tunnel); walls, sky, HUD and the
  weapon are identical. With the bloom on, its blur carries the water's change a little way round it.
- The two renderers' overall brightness still differs a little in these views (walls 2 to 4 levels, one bank patch
  about 24, the water's mean colour -5 to +8 either way) as it did before the fix and away from the water; the
  water's chroma, which carries its alpha, matches. Not the cube's doing; not chased here.
- Regressions: headless 27 scenarios, 26 on the first run, regress_flashbang failing at its buy (weapon 17) and
  passing on rerun; window 27/27. That failure was the title's buy-message race and the tests' hold: see "Buy menu taps".
- Performance (window build, de_aztec, the three views held 10 s each, VSync off, the limit at 60, 120 and 240, the
  renderer's own GPU timestamps and CPU spans; build/window_before is the build before the fix): before and after
  agree within the run-to-run noise at every rate and view, GPU 0.88 to 1.21 ms a frame, CPU recording (the render
  thread inside Clear / Draw) 5.0 to 6.8 ms, present 0.24 to 0.28 ms; 60 and 120 held (frame interval 16.67 and 8.35
  to 8.97 ms; the back view runs at 112 to 113 at the 120 limit before and after), 240 not reached (113 to 143
  frames/s before and after: the game thread is CPU-bound, TODO). Pacing: the 99th percentile interval no worse
  after (at 120: 9.5 to 11.3 ms after, 10.9 to 12.5 before). The cube is one 24 KiB image
  (vkGetImageMemoryRequirements on RADV for the same create info: 32x32 B8G8R8A8, 6 layers), made and uploaded once
  per run (one creation line); no wait, fence, submit or readback was added (the upload goes through the frame's
  upload ring as 2D textures do).
- Vulkan validation (CSX_VK_VALIDATION=1, the Khronos layer with synchronisation validation, the message limit off,
  the same views at 60): the same five message kinds before and after the fix, none about the cube texture or its
  barriers. They are older than G4: the swapchain present mode info chained with swapchainCount 0 (each present),
  VK_EXT_swapchain_maintenance1 enabled without VK_EXT_surface_maintenance1 on the instance, shaders declaring
  DemoteToHelperInvocation without the feature enabled, one image view of a transfer-only image, and a blit after a
  clear without a barrier between them (each present: SYNC-HAZARD-WRITE-AFTER-WRITE). TODO.
- Uncertainty: Vulkan filters a cube across face edges (seamless), the reference renderer within a face (clamped);
  the console's behaviour at face edges is not known (GeForce 3 generation hardware is not documented as seamless).
  For this cube the neighbouring texels across an edge encode nearly the same vector, so the difference is within a
  texel's quantisation. Both renderers take the texel as an 8 bit unsigned value expanded with 2x - 1, as the
  register combiners do; the console's colour interpolation of d0 (0.5 as 127 or 128) moves r by under 0.004. No
  console capture of the water.

## Bloom (2026-10-22)

- The pipeline, traced in a de_dust2 frame (Vulkan, every draw's state) and in the title. The per-frame post pass
  0xC2160 runs the bloom while cl_hdrbloom (value 0x37A594) is not 0: 0xC06D0 saves the state, 0xC1FA0 draws the
  passes, 0xC0D00(1) the composite, 0xC0870 restores; then the dynamic gamma (0xBDFC0). At 4:3 (back buffer 640x960,
  vertical supersampling):
  1. bright pass into a 320x240 target (scissor 0,0 to 160,120 of it): the back buffer on stage 0 with min and mag
     GAUSSIANCUBIC (D3DTEXF 5), one quad; the pixel shader's one stage is 2 t0 - 1 on colour and alpha (RGB input
     0x48: t0 EXPAND_NORMAL times 1, output r0; the final combiner passes r0). What survives is the back buffer above
     half; the title draws dark and the gamma ramp brightens afterwards, so on these maps it is mostly the sky
     (de_aztec's white sky, de_dust2's blue sky little, de_inferno's night none).
  2. blur into 320x120 (scissor 320x120) and 3. into 160x120 (scissor 160x60): four bilinear taps each from the
     previous target, weights 1/3 and 1/6 at 1 and 3 texels (cl_hdrblur scales the offsets), as G1 found.
  4. composite: the 160x120 target added to the back buffer with ONE / ONE and the blend colour cl_hdrbloom.
  The targets follow the back buffer's width (427x240 at 16:9, 569x240 at 21:9). In every run with the audit log
  (CSX_VK_AUDIT, the front end and matches on several maps) filter 5 is set only on stage 0 and only for pass 1:
  GAUSSIANCUBIC has no other user seen.
- Each map sets its own intensity: the client's map start runs `exec "<map>-light.cfg"` (0xC3E90; 0x1A5B80 formats
  the command at 0xC4089, adds it and executes the buffer), which sets cl_hdrbloom: 0.5 on de_aztec (0.6 is the cvar's
  default, FCVAR_ARCHIVE). Found with a write watch on the cvar (CSX_WATCH).
- **Scissor.** The composite drew with the scissor its passes left (0,0 to 160,60 of the 640x960 back buffer: a
  quarter of the width by a sixteenth of the height, top left, at every aspect), so the glow was added in that corner
  only; it shows under a flashbang's fade (the white glows: the corner was 10 to 32 levels brighter than its edge in
  the fade, de_aztec and de_dust2). The cause is the SDK's SetRenderTarget, not the title: the XDK's
  D3DDevice_SetRenderTarget (0x305A30) ends in 0x304B10 with the viewport {0, 0, MAXINT, MAXINT, 0, 1} (0x3135E0),
  which tail-jumps (0x304E26) to D3DDevice_SetViewport (0x306240), and that calls SetScissors(0, 0, 0) (0x306428):
  on the console every SetRenderTarget clears the scissors. The HLE reset only the viewport. SDK patch 0034 clears
  them there too (both renderers share the state). G3's note ("SetRenderTarget does neither") was wrong: it read
  SetRenderTarget's body without following the tail jump. The other passes are unchanged: each sets its scissor
  after its SetRenderTarget (the same rectangles before and after). In the traced frame (1321 draws) only the three
  passes and, before the fix, the composite drew with a scissor; the world, the HUD and the dynamic gamma's
  measurement draw with none either way.
- **GAUSSIANCUBIC.** The title's D3D (its lazy stage setup, 0x30F2D0..0x30F3B6, method 0x41B14) programs the NV2A's
  convolution filter for it: MIN 7 (CONVOLUTION_2D_LOD0) and MAG 4 when either filter is above 3, with the kernel
  GAUSSIAN_3 when either is 5 (QUINCUNX otherwise). The hardware kernel is not documented; xemu (hw/xbox/nv2a, psh.c)
  models CONVOLUTION_2D_LOD0 with GAUSSIAN_3 as a 3x3 kernel [1 2 1; 2 4 2; 1 2 1] / 16 at one texel offsets, each
  tap filtered linearly, and its authors note it was not checked against hardware (xqemu issue 238). Both renderers
  now do that (SDK patch 0035: shaders.cpp gauss3, MakePixelKey's kTexGauss for a 2D texture with min or mag 5; the
  reference renderer's Sample the same nine bilinear taps), at the texture's guest texel size (one texel of the 640x960
  back buffer, 1.8 texels of Vulkan's 1152x960 at 4:3). Before, both took filter 5 as bilinear. The kernel sums to 1:
  the bright pass's mean is unchanged (1.07 at de_aztec's sky); its high frequencies drop 7 % (texels differ by up to
  36 of 255 at the sky's edge), after the two blurs by at most 6 and 1. On screen it is a slightly softer, steadier
  glow edge; the change of brightness is under a level.
- **The visible difference.** On de_aztec's sky the glow now brightens the frame's upper right quarter by 3 to 4
  levels (2.8 to 3.7 at 4:3, 16:9 and 21:9; before the fix 0); the corner seam under a flashbang's fade is gone
  (de_dust2 +26 / +32 at its edges, now +2 / +3; de_aztec +10 / +11, now -1 / 0; with Bloom Off +4 and +2 / +3, the
  scene's own edges). de_dust2 and de_inferno change little: their bright pass holds little. With Bloom Off the two
  builds draw the same.
- **PC Options > Bloom** (On / Off, default On; pc_settings.ini "bloom"; src/game/pc_bloom.cpp) is cl_hdrbloom
  itself: Off sets it to 0 (the title then skips the passes) when chosen and again after every map's light config
  (a hook at 0xC408E); On gives back the intensity the cvar last held: the map's, or one set in the console. At the
  start On does nothing (the title's own value stands). A console change stands until the setting changes or the next
  map starts; nothing else is stored. The row is the tenth of twelve (the pause menu's list now scrolls to Master
  Volume; the front end's already scrolled).
- Found on the way, not changed (G4, TODO.md): Auto Exposure Off chosen before the start is only in force from the
  first change of a setting (34 adjustments in 38 s headless on de_aztec with auto_exposure=0), until any setting
  changes (the identity ramp is set again then). The tests started with Auto Exposure Off compare frames under that
  running ramp; the Bloom tests take their frames after a change. The cause given here (registration sets 1) was
  incomplete: G6 found the map's light config setting 1 (FINDINGS.md "Auto Exposure at the start"), now fixed.

- Performance (de_aztec, gamescope 1280x960, 4:3, VSync off: the white sky ahead, the sky to the side, the river;
  Bloom On at the map's 0.5; before = the build before G5, after = with both patches and the option; GPU time and CPU
  recording per frame in ms):
  60 fps limit: GPU 1.024 / 0.815 / 0.935 before, 1.061 / 0.819 / 0.952 after; recording 4.27 / 3.40 / 3.55 before,
  4.46 / 3.42 / 3.44 after; 60.0 fps both. 120: GPU 1.037 / 0.808 / 0.927 and 1.046 / 0.810 / 0.943; 119.8 to 119.9
  fps both. 240: GPU 1.032 / 0.808 / 0.922 and 1.045 / 0.805 / 0.937; 149.5 / 172.4 / 189.1 fps before, 150.2 /
  171.4 / 183.4 after (the CPU bound frame rate of these views; the river's 3 % is recording noise, 3.33 and 3.49 ms,
  the GPU's difference is 0.015 ms). The bloom's passes (now over the whole frame on the composite, nine bilinear taps
  on the bright pass) cost about 0.01 to 0.04 ms of GPU time. Peak resident memory 167 MB either way. Validation
  (the 60 fps run with the layers, no message limit): the same five older kinds as G4 recorded (FINDINGS.md
  "Water"), in proportion to the frames presented (3029 and 3101 per present message, 17 shader module, 19
  output-not-consumed, one image view, one device extension, before and after); nothing new from the bloom.

Remaining uncertainty: the convolution kernel is xemu's model, not a hardware measurement (no console capture);
whether the NV2A's MAG 4 differs from its MIN path; the bright pass and blurs otherwise as G1 compared them.

## Auto Exposure at the start (2026-10-22, G6)

- The bug (G4, TODO.md): Auto Exposure Off saved in pc_settings.ini did not hold in a match until a setting was
  changed. Two causes, the second the one that mattered:
  1. Before the client registers its cvars, cl_dynamicgamma's value is 0 whatever the setting (the static record at
     0x37A2F0 holds its default string "1" at 0x32A090 and value 0; Cvar_RegisterVariable, 0x422A0, called for it at
     0x2BDEF, puts an allocated copy of the string at +4 and its atof at +0xC). PcAutoExposureFrame compared the value
     and took Off as done before registration.
  2. Each map's light config ("exec <map>-light.cfg", run as the client's map start at 0xC4089, FINDINGS.md "Bloom")
     sets cl_dynamicgamma 1: a probe on both sides of it read 0 before and 1 after on de_aztec, de_dust2 and
     de_inferno, with Off already applied and the cvar registered (the same probe: de_dust2's and de_inferno's
     configs set cl_hdrbloom 0.5 too, as de_aztec's does). Every map start is a title relaunch (the front end, Change
     Map, Quit: FINDINGS.md "Title relaunch"; Restart Match is not and does not run the config again), so the
     config runs once per instance, after the frame code's Off. That is why cl_dynamicgamma 0 on the match's command
     line did not stop it in G4 either (the command line runs before the config).
  With the cvar back at 1 the title's post pass (0xC2160) ran the dynamic gamma (0xBDFC0) every frame: headless on
  de_aztec it adjusted once a second from the first frame of the match and the ramp's high point sank from 0.85 to
  its 0.6 clamp, so the held view's mean was 122 against 61 with the identity ramp. A change of any setting re-sent
  Off (the cvar then held 1) and set the identity ramp, which is why it held from the first change on.
- The fix (src/game/dynamic_gamma.cpp, pc_keys.cpp, config/game.toml): the hook after the light config (0xC408E,
  now PcMapLightConfig, which Bloom already used) also calls PcAutoExposureMapConfig: with Off and the cvar not 0 it
  sets cl_dynamicgamma to 0 through the title's Cvar_DirectSet (0x41F90, what the console's command calls; the
  string is the engine's "0", 0x329F70) and sets the identity ramp (0xBF030), in the hook, before the map's first
  frame. On leaves the map's 1 (the title's own state). The frame code compares nothing before the cvar is registered
  (its string pointer still the static default). Bloom's frame code waits for registration the same way (it already
  took the light config back; before, it logged "Bloom off" as done before the cvar existed). The adaptation itself
  (0xBDFC0, 0xBE760, 0xBE960) and its timing are unchanged.
- Checked: regress_ae_startup (headless de_aztec, Auto Exposure and Bloom Off saved: the map's 1 and 0.5 taken back,
  no adjustment logged, the held view's frame equal to the one after Off is chosen again, -0.0; the four combinations
  changed while playing: On adjusts again, Off stops it and sets the identity ramp, Bloom follows its own setting,
  each saved); on the G5 build the same scenario fails (adjustments from the start; frame +61.2). vk_ae_maps (the
  Vulkan window, Off saved: front end, de_airstrip, Change Map to de_aztec; in each instance Off is in the cvar before
  the bloom's first pass, no adjustment; at a held view the frame equals the one after On and Off again, -0.1).
  vk_bloom_43, _169 and _219 (4:3, 16:9 at 3840x2160, 21:9 at 144 frames per second) start with Off and expect the
  map's 1 taken back and no adjustment.
- Tests measured under the running ramp: the water tests (regress_water*, vk_water_*) start with Auto Exposure Off
  and bound the mean chroma of a box of water. On G5 the same run logged 46 adjustments, so the bounds (graze 16.3 to
  19.5, steep 34 to 43, back 12.3 to 15.5) were the brightened ramp's; with Off holding, the frames are darker and the
  graze box measured 10.4. The water itself is unchanged: the cube's alpha probes read the same (0.245, 0.161, 0.231).
  Re-measured at the identity ramp, both renderers agree as before: graze 10.4 (reference) / 10.5 (Vulkan), steep
  21.8 to 24.6 / 23.3 to 24.0 over the runs, back 8.1 / 8.1, at 4:3, 16:9 and 21:9. The bounds are now 9.7 to 11.6,
  20.5 to 26.1 and 7.3 to 9.2 (the same relative width as before).
- Found on the way, fixed (tools/drive.py): the game's log is written through stdio (info lines unflushed, warnings
  flushed), so the driver's read could end mid-line; it kept the two halves as two lines and a wait for that line
  never matched (vk_loading waited its 900 s for a shot line that was in the log, once in seven full window suites). A cut line
  now waits for its end.

## Vulkan validation (2026-10-22, G6)

The Khronos layer (CSX_VK_VALIDATION=1, synchronisation validation, errors, warnings and performance warnings, no
message limit) in the test's headless gamescope. Each kind reproduced with a minimal scenario (the front end's first
screens, 12 s: ESRB and Valve logo videos, the title) and in a match (de_aztec, three views held, 60 frames per second).

Before (G5, 452713d) and after (G6), message counts:

| Kind | Origin | Front end, no WSI layer (G5 / G6) | de_aztec match, no WSI layer (G5 / G6) | Status |
|---|---|---|---|---|
| SwapchainPresentModeInfo arraylength, -07760 | gamescope WSI layer | 0 / 0 | 0 / 0 (one each per present with the layer, both builds) | external, documented |
| vkCreateDevice-01387 | gamescope WSI layer | 0 / 0 | 0 / 0 (once with the layer, both builds) | external, documented |
| SYNC-HAZARD-WRITE-AFTER-WRITE | renderer (Present, ComposeOverlay) | 769 / 0 | 3074 / 0 | fixed (647962b) |
| vkDestroyImage-image-01000 | renderer (FreeImage between frames) | 1 / 0 | 0 / 0 | fixed (412b0ad) |
| ShaderModuleCreateInfo-pCode-08740 | renderer (device features) | 0 / 0 | 17 / 0 | fixed (463ab3d) |
| ImageViewCreateInfo-image-04441 | renderer (GuestSized's copy) | 0 / 0 | 1 / 0 | fixed (9aef86b) |
| WARNING-Shader-OutputNotConsumed | renderer (title's shaders, performance warning) | 0 / 0 | 19 / 19 | not changed, documented |

None comes from the recompiled code or the compatibility layer: every message names a Vulkan call of the renderer
(src/vulkan/vk_backend.cpp) or, for the first two, the gamescope layer's own calls.

1. **VUID-VkSwapchainPresentModeInfoKHR-swapchainCount-arraylength and -07760** (each present: a
   VkSwapchainPresentModeInfoKHR chained to vkQueuePresentKHR with swapchainCount 0) and
   **VUID-vkCreateDevice-ppEnabledExtensionNames-01387** (once: VK_EXT_swapchain_maintenance1 enabled without
   VK_EXT_surface_maintenance1 on the instance). Not the renderer's: it enables at most four device extensions
   (swapchain, push descriptor, depth bias control, depth range unrestricted) and chains nothing to a present; the
   extension is the fifth in the list the layer saw. They come from gamescope's WSI layer (VK_LAYER_FROG_gamescope_wsi,
   implicit, enabled by gamescope's ENABLE_GAMESCOPE_WSI=1), which sits above the validation layer: with
   DISABLE_GAMESCOPE_WSI=1 the same runs have none of the three. Not fixed (outside the project; harmless to the
   picture). On the desktop (Hyprland, no gamescope) the layer is not loaded. Severity: none for the game. A future
   test run could set DISABLE_GAMESCOPE_WSI=1 for its validation passes.
2. **SYNC-HAZARD-WRITE-AFTER-WRITE** (one per present, one per video frame). Present clears the swapchain image black
   (the letterbox) and blits the frame over it; ComposeOverlay copies the frame into its target and blits the video
   over it; neither had a barrier between the two transfer writes to the same image, so the clear or the copy could
   land after the blit (a black or stale frame on hardware that overlaps transfers). Fixed: TransferWriteBarrier
   (transfer write to transfer write, the image staying in TRANSFER_DST_OPTIMAL) between them. The picture is
   unchanged where the order already held (every test's frames).
3. **VUID-vkDestroyImage-image-01000** (once, the front end: the ESRB video changing to the Valve logo). A resource
   lifetime bug: FreeImage queued an image on the current frame slot's garbage, which the slot destroys when it next
   starts. Freed between frames (ComposeOverlay before a frame's first command when the video changes size,
   UpdateResolution after Present's submit), that start was the very next one, while the frame just submitted on the
   other slot could still read the image on the GPU. Fixed: an image freed while no frame records waits and goes with
   the next frame started, destroyed after that frame's fence (which covers every earlier submission); one freed while
   recording goes with its frame as before. (A first attempt, starting the frame inside FreeImage, began the wrong
   slot when called from UpdateResolution between the submit and frame_++; the validation run showed it at once.)
4. **VUID-VkShaderModuleCreateInfo-pCode-08740** (once per pixel shader with a discard: 17 in the match). DXC with
   -fspv-target-env=vulkan1.3 compiles discard (texkill, the alpha test, clip planes) to OpDemoteToHelperInvocation,
   whose capability needs the shaderDemoteToHelperInvocation feature. Every Vulkan 1.3 device has it; the device
   now enables it. The shaders and their output are unchanged (the driver already ran them).
5. **VUID-VkImageViewCreateInfo-image-04441** (once). GuestSized's downscaled copy of a drawn target (transfer
   source and destination only, read through copies) was given an image view, which needs a sampled or attachment
   usage. Fixed: MakeImage makes no view for an image without such a usage; nothing used that view.
6. **WARNING-Shader-OutputNotConsumed** (performance warnings, 19 in the match: 15 pipelines whose vertex shader writes
   the fog factor, TEXCOORD4 at location 8, that the pixel shader does not read; 4 whose vertex input has attributes
   2 and 3 of the title's vertex declaration that the vertex shader does not read). Not invalid (the layer says so);
   the write is discarded and the driver drops it when it links the pipeline. Not changed: the vertex shaders and
   input state follow the title's own declarations and programs, and trimming them per pipeline would add keys for no
   change in the picture or measurable cost.

- The sweep (G6, and G5 for comparison; the validation layer as above with gamescope's WSI layer at its default):
  vk_bloom_169 (16:9, full screen at an internal 3840x2160), vk_bloom_219 (21:9 at 2560x1080, 144 limit),
  vk_display_mode (windowed, borderless and full screen, the resolution changed), vk_vsync_play (VSync on and off at
  60, 120 and 240 limits), vk_flashbang (the white fade), vk_ae_maps (the front end's videos, two relaunches and a
  Change Map), and the 240 perf scenario (4:3, three views). G6 in every one: the WSI layer's two messages per present
  and one per device (three in vk_ae_maps, one per instance), and OutputNotConsumed (3 to 39, one per pipeline using
  such a shader; 39 over three instances); nothing else. G5 in the same runs: also SYNC-HAZARD-WRITE-AFTER-WRITE once
  per present (6446 in vk_flashbang, 27647 in vk_ae_maps), pCode-08740 (4 to 18), image-04441 (1 per instance) and
  vkDestroyImage-image-01000 (once in vk_display_mode, at a resolution change, and once in vk_ae_maps, at the video
  change). Under the layer the game runs at 18 to 30 frames per second, so the tests' frame rate checks fail
  (vk_bloom_219, vk_vsync_play, both builds; they pass without it), and the G5 runs fail the new Auto Exposure
  expectations (the bug); the counts are what is compared.

- Performance, G5 against G6 (de_aztec, gamescope 1280x960, 4:3, VSync off, Bloom On at the map's 0.5: the white sky
  ahead, the sky to the side, the river, 10 s each; both builds log the 95th percentile, which G6 adds to the frame
  timing line; GPU time and CPU recording per frame in ms, sky / side / river). Auto Exposure Off saved: G5 ran 146
  adjustments anyway, G6 none; On: 146 to 147 both.
  - 60 limit, Off: GPU 1.046 / 0.807 / 0.954 (G5) and 1.040 / 0.797 / 0.932 (G6); recording 4.56 / 3.37 / 3.62 and
    4.23 / 3.33 / 3.43; 60.0 fps; 95th percentile interval 17.6 to 17.9 ms both; 99th (the worst second's) up to 27.0
    (G5) and 19.1 (G6). On: GPU 1.049 / 0.821 / 0.952 and 1.039 / 0.811 / 0.952; 95th 17.3 to 17.7 and 18.0 to 18.8.
  - 120, Off: GPU 1.038 / 0.822 / 0.945 and 1.011 / 0.796 / 0.926; 119.7 to 120.0 fps; 95th 9.3 to 9.4 and 9.3 to
    10.1; 99th 10.0 to 10.1 and 10.3 to 11.5. On: GPU 1.046 / 0.808 / 0.945 and 1.043 / 0.814 / 0.945; 118.8 to 119.9
    fps both; 95th 9.3 to 11.5 and 9.2 to 12.0.
  - 240 (CPU bound in these views), Off: 149.6 / 170.0 / 181.5 fps and 153.2 / 173.5 / 186.6; GPU 1.034 / 0.806 /
    0.936 and 1.018 / 0.783 / 0.912; 95th 6.2 to 7.8 and 6.2 to 9.1. On: 153.1 / 174.7 / 185.1 and 149.8 / 169.2 /
    184.6; 95th 6.1 to 8.6 and 6.1 to 7.8.
  The differences go both ways between runs and with the build (the 240 runs' frame rates swap order between Off and
  On): noise. The identity ramp saves the dynamic gamma's measurement draw (GPU about 0.01 to 0.02 ms less with Off on
  G6). Present 0.25 to 0.29 ms throughout. Peak resident memory 167 to 170 MB, G6 0.6 to 2.0 MB lower in every pair.

- Tests: headless 30 of 30, window 32 of 32 (after the water bounds and the driver fix; before them the water tests
  failed at the identity ramp and vk_loading hung once).

Remaining: the gamescope WSI layer's messages (1; the desktop does not load the layer, so they were not seen
there), the performance warnings (6). Checked on one GPU and driver only (AMD Radeon RX 9070 XT, RADV, Mesa 26.2.5):
the write-after-write and destroy-in-use hazards never showed on screen here, and another driver could have shown
them (a black or stale frame, a fault). Nothing in this section depends on the console or on xemu: the Auto
Exposure cause is the title's own (its light configs, read in the title), the fixes are Vulkan usage.

## Weapon over the HUD (2026-10-23, G6.1)

- Seen in play (16:9 full screen at 3840x2160, FOV 120, de_airstrip, the K&M SMG): parts of the money and ammunition
  numbers hidden behind the first-person weapon, others drawn over it. Not every number, not every weapon: a depth
  test, not an order. Reproduced in the Vulkan window on de_aztec at a held view (tools/drive.py setview) at 4:3, 16:9
  and 21:9 with FOV 120 and the K&M SMG; not at FOV 90 (the default), not in the reference renderer at any setting.
- The title's order and state (CSX_VK_STEPSHOTS' per-draw log of the frame): the 3D view (the world, the player
  models, then the weapon: fixed function fans and strips, depth tested and written, the viewport's MinZ -1, MaxZ 1,
  the GL layer's glDepthRange(0, 1)), the post pass (bloom), then the HUD: textured quads, blended, depth test on
  (ZEnable 1, LEQUAL), depth writes off, an orthographic projection with z 0 and MinZ 0, MaxZ 1. So the HUD is drawn
  last and is depth tested against the view: it shows wherever the view's depth is at or above 0. The weapon uses the
  world's projection and depth range (FINDINGS.md "FOV and widescreen": there is no viewmodel FOV or depth hack).
- The depth. With the view's projection (its z row -1.00025, -2.00025) and MinZ -1, MaxZ 1, screen z is
  1.0005 - 4.0005 / d times the Z scale for an eye distance d: -1 at 2 units, 0 at 4 (the probe's vertices below fit
  it: w 3.189, z -0.254 of the Z scale). The console's cull range for the fixed function is MinZ to MaxZ (-1 to 1,
  FINDINGS.md "Flashbang", SDK patch 0033), so what lies 2 to 4 units from the eye is kept, below 0. The console's
  depth buffer is a 24 bit integer (0 to the Z scale): it cannot hold a value below 0, which is stored (and compared)
  as 0. The reference renderer does that (it culls at MinZ to MaxZ, then
  clamps to 0 to the Z scale for the test and the store): at a lost digit of the reserve ammunition its probe saw the
  weapon's triangle at screen z +1.28M to -4.75M (Z scale 16.78M), stored 0, and the HUD's quad at 0 pass (LEQUAL).
  Vulkan's depth buffer is D32F and its viewport took the cull range as it is (VK_EXT_depth_range_unrestricted): the
  weapon's fragments kept their negative depth and the HUD's 0 failed against them. At FOV 90 the weapon's parts that
  close to the eye are outside the frame; at FOV 120 the frame reaches them (the arm, the magazine and the receiver at
  the bottom right, where the money and ammunition are).
- Not the HUD's layout, the weapon's projection or placement, the draw order or an aspect ratio handling: the same
  frame in the reference renderer draws every number over the weapon, at every aspect.
- Since G3 (5a80764, the flashbang's white-out: the cull range at MinZ to MaxZ through the unrestricted depth range):
  before it Vulkan clipped at 0 to 1, so those weapon parts were not drawn at all and the HUD showed. Present in G5
  and G6 (the G5 build loses the same digit, 12,12,14 against the reference's 46,62,67).
- The fix (src/vulkan/vk_backend.cpp): every pipeline clamps fragment depth to 0 to 1 (rasterization's
  depthClampEnable with a user defined clamp range [0, 1], VK_EXT_depth_clamp_control) and keeps clipping at the
  viewport's depth range, the cull range (VK_EXT_depth_clip_enable, which a clamp would otherwise turn off): culled at
  MinZ to MaxZ, then stored and compared within the buffer's range, as on the console. Depth bias comes before the
  clamp, as in the reference. A range of 0 to 1 is unchanged (its fragments are already within it).
  VK_EXT_depth_clamp_zero_one was tried first and does not do it: with a float buffer and the unrestricted range its
  clamp leaves values outside 0 to 1 as they are. Without the two extensions the log says so and depth is not clamped
  (the HUD can then be lost behind a weapon at a wide FOV, as before).
- Checked (the Vulkan window against the reference renderer, the same steps and held view, de_aztec, FOV 120 at 4:3,
  16:9, 21:9 and FOV 90 at 16:9): the Glock, the knife, the K&M SMG at rest and after a burst, its reload, a switch part
  way, the CV-47. HUD pixels the reference has and Vulkan lost in the bottom band (rows 800 to 900 of 960, more than 40
  levels darker): the K&M SMG at rest 219 to 220 before, 3 to 4 after; after a burst 756 to 768 before, 3 to 4 after;
  the Glock, the knife and the CV-47 0 to 10 either way; at FOV 90 none either way. The reload and the switch differ
  by their animation frame (the two renderers' shots are not the same instant), with the HUD over the weapon in both.
  The weapon itself, its animations, proportions and placement are unchanged (the same model, projection and
  position; only depth values below 0 change, to 0).

- Performance (de_aztec's three held views, the window at 60, 120 and 240 frames per second, G6 against G6.1): GPU
  time per frame 0.82 to 1.07 ms in both, recording 3.3 to 4.4 ms in both, the frame rate and its 95th percentile the
  same within noise (240: 150.8 to 186.6 against 151.5 to 181.5), memory within 1 MB. Validation (the layers through
  vk_viewmodel_hud_169): no new message; the gamescope WSI layer's and OutputNotConsumed as after G6 (FINDINGS.md
  "Vulkan validation"). Tests: headless 30 of 30, window 35 of 35 (vk_viewmodel_hud_43, 169 and 219 new; they fail
  on G6, the log line and the share of the ammunition's blue).

- The translucent shapes in the same screenshot: the blue circle at the top left is the radar (the title's, translucent
  with the view cone; the same in both renderers). The large translucent grey rectangles across the scene, with
  straight edges, are de_airstrip's mist sprites over the water: camera facing quads (one depth per quad), grey 0.29
  at alpha 0.12 to 0.16, SRC_ALPHA / INV_SRC_ALPHA (the reference's probe at a mist pixel: draws 696 and 737), their
  straight edges where the quad meets the ground and the water under the depth test, as GoldSrc sprites do. The same
  in the reference renderer; intended (the title's content), not changed.
- Seen on the way, not investigated: thin vertical rain streaks over de_airstrip in Vulkan that the reference
  renderer's frames do not show.

Remaining uncertainty: that the NV2A stores a depth below 0 as 0 (and does not wrap it) is the integer buffer's
behaviour as the reference models it, not measured on a console; the HUD's order and depth test are the title's own.

## Xbox Graphical Effects (2026-10-23, G7)

The question: which of the title's graphical effects a master "Xbox Graphical Effects" On / Off in PC Options can
switch off without taking away part of the original picture's correctness. Answered from the earlier audits ("Graphics
fidelity audit", "Bloom", "Dynamic gamma", "Auto Exposure at the start", "Water", "Flashbang", "Weapon over the HUD"):
the title's render cvars and the passes it submits, read in both renderers; nothing new was assumed optional because
PC Counter-Strike 1.6 lacks it.

- **Optional, switched by the master** (each is a post pass the title itself skips while its cvar is 0, and each is
  already a PC Options setting):

  | Effect | Cvar | What Off leaves |
  |---|---|---|
  | Bloom: bright pass (2 t0 - 1 through GAUSSIANCUBIC) into 320x240, two blurs, the ONE / ONE composite with the map's intensity (0.5 on de_aztec and de_airstrip) | cl_hdrbloom (cl_hdrblur scales the blur's offsets, only while the bloom runs) | the frame without the glow (the title's 0) |
  | Auto Exposure: the 40x60 luminance measurement, its histogram and the gamma ramp that follows it | cl_dynamicgamma | the title's identity ramp (0xBF030), one fixed brightness |

- **Always on** (not effects that can be left out, or not the title's to switch):

  | Kept | Why |
  |---|---|
  | GAUSSIANCUBIC as the 3x3 Gaussian (SDK patch 0035) | the filter the title asks for; its only user is the bloom's first pass, so with the bloom off nothing samples with it |
  | The composite's scissor cleared at SetRenderTarget (patch 0034) | the XDK's behaviour (a fix, not an effect) |
  | The flashbang's white-out (screen fade, G3; over the whole frame at every aspect) | gameplay: what blinds the player |
  | de_aztec's water (the cube texture, the bump map's dependent reads, G4) | terms of the water's own pixel shader |
  | Shadows (r_shadows' blob quads), lamp glows and light sprites, smoke, explosions, decals, de_airstrip's fog and mist sprites, the muzzle flash | the title's content; shadows are legibility ("Graphics fidelity audit") |
  | Texture filtering (anisotropy x4, mipmaps), blending, alpha test, polygon offset | render state the title sets on each draw |
  | Vulkan's depth clamp (G6.1), the depth range, resolution and aspect handling | rendering correctness and PC display settings, not Xbox effects |
  | The brightness setting's own ramp | the title's options (the identity ramp follows them) |

  Not emulated either way: DXT1 noise (a hardware dither). No new effect was added.

- **Settings.** pc_settings.ini holds the saved preferences: "xbox_effects" (new, On by default), "bloom",
  "auto_exposure" (On by default). What takes effect is master AND setting (src/common/pc_settings.cpp
  pc::XboxEffect): pc_bloom.cpp and dynamic_gamma.cpp read it where they read their own setting before, so the
  master is one more input to the code that already sets each cvar, takes the map's value back and gives it back.
  The master never writes "bloom" or "auto_exposure"; Off then On with Bloom saved Off leaves the bloom off. The row
  is the ninth of thirteen in PC Options, above Auto Exposure and Bloom (both menus' lists scroll).
- **Precedence**, from the earliest to the latest word in a frame:
  1. The cvars' defaults (cl_hdrbloom 0.6, cl_dynamicgamma 1) as the client registers them.
  2. The map's light config ("exec <map>-light.cfg" at the client's map start) sets the map's cl_hdrbloom and
     cl_dynamicgamma 1.
  3. Right after it (the hook at 0xC408E) an effect that is off (master Off or its own setting Off) is set back to 0
     before the map's first frame (Auto Exposure with the identity ramp); an effect that is on keeps the map's value,
     which is also what On gives back later (map-specific intensities are kept, not stored by the master).
  4. A setting changed while playing (the master, Bloom or Auto Exposure) sets the cvar through the console's own
     command: Off 0 (and the identity ramp), On the intensity the cvar last held (the map's, or a console value) and
     cl_dynamicgamma 1.
  5. A console command (cl_hdrbloom X, cl_dynamicgamma X) is the title's own and stands as typed, also while the
     master is Off: cl_hdrbloom until the Bloom setting's effect changes (the master or Bloom switched so that the
     bloom goes on or off) or the next map; cl_dynamicgamma until any PC setting is changed (Auto Exposure re-asserts
     itself on every change, as before G7) or the next map. A console intensity is what On gives back while it is
     the last the cvar held; the next map's light config replaces it with the map's.
  Every map start is a relaunch (a new process reading pc_settings.ini), so the master, as the other settings, holds
  across map changes and restarts with nothing carried over in memory: no temporary override accumulates.
- **Tests.** regress_xbox_effects (headless; started with the master saved Off): all eight combinations of master,
  Bloom and Auto Exposure changed while playing at one held view; the cvars follow master AND setting (drive.py
  `expect effects`, CSX_LOG_EFFECTS=1 logs the two cvars when either changes), the saved values stay as set, the
  adjustment runs only with both On, and every frame with the master Off is the frame with the master On and both
  effects Off (mean luminance within 0.0 of 60.8); the bloom brightens the sky with the master On (+7.6 levels in
  the upper right quarter). vk_xbox_effects_43 / _219 (4:3, 21:9, FOV 120, the master saved Off): both effects off
  from the first frame, the reserve ammunition over the K&M SMG within vk_viewmodel_hud's bounds (0.149 and 0.044,
  the reference renderer's values) with the master Off, On and Off again (Off's frame unchanged, the identity ramp),
  the flashbang's white-out full and even over the whole frame. vk_xbox_effects_169 (16:9 from the front end): the
  relaunch into de_airstrip with both off, the pause menu's row turning the master On (saved; the map's 0.5 and the
  adjustment back), a console cl_hdrbloom 0.8 kept through Off and On, Change Map to de_aztec with the master Off
  (taken back before its first frame), On giving de_aztec's 0.5 rather than the console's 0.8, a console
  cl_dynamicgamma 1 standing while Off until the next setting change. The menu tests' clicks follow the new row
  (Auto Exposure now row 10, Bloom 11, Display Mode 12).

- Performance (de_aztec's three held views, the window at 60, 120 and 240 frames per second; G6.1, G7 with the master
  On, G7 with it Off): with the master On the GPU time per frame is G6.1's (1.009 to 1.031 / 0.786 to 0.795 / 0.914
  to 0.927 ms against 1.018 to 1.027 / 0.790 to 0.801 / 0.912 to 0.929), the frame rate at 60 and 120 the limit;
  at 240 (CPU bound) 153.1 / 174.4 / 186.4 against 152.8 / 181.9 / 196.5, recording 4.50 / 3.44 / 3.63 ms against
  4.25 / 3.17 / 3.29: run to run noise of these views (G6.1's own run in G6.1 measured 150.8 / 172.6 / 186.6); the
  change adds no per-frame work beyond two reads of settings on a change. With the master Off the GPU time drops
  0.03 to 0.05 ms (the bloom's passes and the measurement skipped), 155.2 / 176.1 / 192.2 at 240. Memory 167 to 169
  MB in all. Validation (the layers through vk_viewmodel_hud_169 on G6.1 and G7, and vk_xbox_effects_169 on G7): the
  same message kinds as G6.1 (gamescope's WSI layer, OutputNotConsumed; the device extension message once per
  process, three in vk_xbox_effects_169's relaunches), in proportion to the frames presented; nothing new.
  Tests: headless 31 of 31, window 38 of 38.

Remaining uncertainty: the optional set is the title's own two post passes; whether the console's dashboard or any
other build had a switch for them is unknown (none in this XBE). The bloom's intensity On gives back is the last the
cvar held: a map whose light config set no cl_hdrbloom would keep the previous one (de_aztec, de_airstrip and the
other maps run in the tests set it).

## Player collision (2026-10-23, G8.1)

The question: can players' bodies be kept from blocking each other (Custom Game > Player Collision Off) without
touching combat, the world or the bots' navigation. Answered from the XBE's code (the engine and the game DLL are one
image) and checked in matches.

- **Where bodies meet.** One movement function moves every player: pm_shared's PM_Move (0x1A5B20: pmove = ppmove
  (0x552C34), PM_PlayerMove (0x1A5490), the ground flag, the friction). The server reaches it through a function pointer
  (0x37BE38) for the player and for each bot (bots are clients; their moves run the same
  code), the client through its prediction table (HUD_PlayerMove 0x18B700, set at 0x1C74A). It moves against
  pmove's physents (count at +0x240, the array at +0x244, 0xD4 bytes each: name, player +0x20, origin, model,
  studiomodel, mins, maxs, info +0x50, angles, solid +0x60...), which the engine fills before each move: the world
  (entry 0), the solid entities near the move, and the other players (player != 0). The engine's trace over them
  (_PM_PlayerTrace 0x756D0, called through pmove's function pointers) and its point and stuck tests read only that
  list. Player against player, against a bot and bot against bot are therefore one mechanism: the other bodies in
  the mover's physents. Doors, platforms and other moving brushes are physents too (player 0); hostages are
  (player 0).
- **What else meets a player.** In GoldSrc shots, the knife, grenades in flight and their blast, and every
  non-player entity's movement go through the engine's SV_Move on the edicts' own solidity and the studio hitboxes,
  not through physents (checked here by the result: shots and an HE grenade hurt a bot with collision Off as On).
  Hit detection, damage and hitboxes do not depend on the movement list.
- **Prediction.** The client predicts its own player with the same PM_Move against physents it fills itself (the
  other players from the last update); on the single process here the client and the server read
  the same setting, so the prediction and the server agree.
- **The setting.** The single player match's Custom Options become the match's command line: 0xE2270 joins the
  table's selected values ("+mp_timelimit 60 +mp_maxrounds 7 ...") when Match Options is Custom (the presets use
  fixed strings) and the front end relaunches the title with it (Z:\releasemap.txt). A tenth row, Player Collision
  (On / Off, default On; "+pc_player_collision 1" / "0"), goes after the nine (the screen's own code, 0xE1DE0,
  reaches them by position, so the row must come last), in the screen's format (src/game/pc_menu.cpp
  ExtendCustomOptions). In the match the command (served before the engine's lookup, PcConsoleCommand) sets the
  value during the map load, before any player spawns; once a player has moved it is fixed: the command is refused
  ("set at the match's start"), so there is no live toggle and no change within or between rounds. No setting on the
  command line (the presets, Training, a match started otherwise) is the title's own behaviour, logged as such.
- **Off** (src/game/pc_collision.cpp, a hook at PM_Move's entry): the other players' physents are taken out of the
  list (the world and every other entry kept, in order, so the indices the move reports for the ground and the
  touched entities stay consistent within the move); the server's move of each player and bot and the client's
  prediction alike. Bodies pass through each other and cannot stand on each other. Unchanged: the world, brushes and
  doors, hostages (they block as before), every trace, damage and hitbox, spawn selection (the game's own checks for
  an occupied spawn point), the bots' navigation (their own logic; they may still slow for a teammate ahead, which is
  their behaviour, not a collision).
- **Persistence.** The game keeps the Custom Options' choices from one front end to the next (a later match's
  command line still has Friendly Fire On and the row's Off after a match, Quit and a new front end); the row's
  choice is kept the same way. Reset (X) gives its default, On, as for the other rows. The in-match Change Map
  relaunches with the same options (Off held on de_aztec after de_airstrip).
- **Multiplayer.** The row is in the single player Custom Options only. System link and Xbox Live hosts have their
  own Custom Options screens (SuperLauncher_LocalServerCustomOptions, LiveServerCustomOptions); the row is not added
  there: a remote client is a separate process that never sees the host's command line, so its prediction would
  still collide while the host's server lets players pass (a correction each time they meet), and system link
  between two instances has not been run here at all. The server-side change is authoritative (a client cannot set
  it: the command is the host's own and refused once the match runs), but network play with it is untested and not
  offered.

- **Tests.** regress_player_collision_on (tests/launch/de_aztec_2bots, no setting) and _off
  (de_aztec_2bots_nocollide, "+pc_player_collision 0"): two Counter-Terrorist bots held with bot_stop 1 (their
  movement physics still run), bodies placed with the probe (drive.py place, players). The player walking at a bot
  from 256 units: On stops 32.0 from it, Off walks 556.8 (as on open floor) and ends 300.8 past it. The player
  dropped onto a bot's head: On stands at -179.9 (72 up), Off falls to the floor (-252.0); bot 2 dropped onto bot
  1: the same (the log shows client 2's move leaving the other players out). A Glock shot at bot 1 (100 to 90 On,
  100 to 44 Off) and an HE grenade bought through the buy menu (90 to 50, 44 to 7) hurt it either way. A wall stops
  the player 160 units on either way. sv_restartround: everyone spawns at least 173 apart, the setting is applied
  once, a drop onto a bot's head after the restart behaves as before it; the command typed during the match is
  refused. vk_player_collision_43 / _219 (FOV 120): the tenth row by the mouse, the match it starts with collision
  off. _169: by the keys, kept through Change Map (de_aztec), Quit and the next front end (the next match Off again),
  Reset gives On. The HUD at FOV 120 as before (vk_viewmodel_hud_43/169/219 pass; the play shots).
  Headless 33 of 33 (regress_pc_bindings failed once in the full run at its PC Controls Reset step and passed alone
  three times: pad timing, unrelated: the change touches no in-game menu), window 41 of 41.
- Performance (de_aztec's three held views, the window at 60, 120 and 240, G7 against G8.1): GPU time per frame
  1.016 to 1.237 / 0.779 to 0.952 / 0.917 to 1.082 ms in both (the same views within 0.07 ms either way), CPU
  recording 3.11 to 4.18 ms in both, 60 and 120 at the limit, 240 (CPU bound) 160.0 / 179.6 / 195.4 against 163.8 /
  187.7 / 199.5, the 95th percentile within noise; peak memory 167 to 169 MB in both. The hook is one check per
  player move with collision On; Off adds a pass over at most the move's physents. Validation (the layers through
  vk_viewmodel_hud_169 on G7 and G8.1, and vk_player_collision_43 through the front end and a match on G8.1): the
  same message kinds as G7 (gamescope's WSI layer, OutputNotConsumed, the device extension message once per
  process), nothing new.

Remaining uncertainty: the bots' avoidance of teammates is their own logic and was not changed (with collision Off
they can still pause behind each other); hostages keep blocking (they are not players); network play is untested and
the option is not offered there.

## Player slots, 65 (2026-10-23, G8.2B checkpoint 2)

The engine, the client, the HUD and the game DLL hold 65 player slots (svs.maxclientslimit 65, the match's command
line -maxplayers 65), any mix of humans and bots. No 17-slot mode is kept. Made of 401 operand patches
(config/player_slots_patches.toml, generated by tools/slot_patches.py from config/player_slots.toml and
config/guest_tables.toml), 26 hooks (config/game.toml) and 8 host replacements of whole functions (src/game/player_slots*.cpp).

- **Tables moved to guest tables** (each ends at a guard page; 21 tables, 532 KB committed in 0x70000000+16 MB): the
  server's lag compensation, last ping and last loss tables; the client's frame ring player states
  (cl_frame_playerstates [16][65]: 13 frame multiplies that feed a state take its 0x3FBB stride, the two sites that
  share the frame pointer get the state from hooks 0x23FAE / 0x32F81, the frame and cl clears clear it), cl.players,
  predicted_players, the studio player cache; the HUD's g_PlayerExtraInfo, g_PlayerInfoList, the Spectator values, the
  player records and flags (all [N + 2]: the HUD's MAX_PLAYERS is N + 1, so its i < MAX_PLAYERS loops, which left
  out player 17 in the original, reach player 65).
- **Arrays inside static objects** moved by this-relative displacements: gHUD's per player field of view (+0xAC),
  gHUD.m_Spectator's overview positions (+0xF30), g_VoiceStatus's five bit vectors (now 3 dwords each), talk times,
  labels and speaker icon entities, g_StudioRenderer's per player state (+0x40B0: found by a crash, below).
  **Heap objects grown** with the arrays at their end: the ScorePanel (0x294 -> 0x538: rows [76], is-a-team [76],
  sorted flags [67]), CSoundEnt (64 -> 129 sounds: one per client is reserved, so 64 world sounds stay), the StatShot
  block (damage for players 1..65).
- **The server's client_t block** holds 65 client_t and, after them, the voice listen bits (3 dwords per client:
  client_t's own 64 bits could not hold player 65); Voice_Get/SetClientListening use them by patches, the relay's
  test (0xABDF8) and the two client_t clears by hooks.
- **Limits raised**: the maxplayers clamp, the 17-client loops (duplicate names, userinfo, voice relay, edict to
  userinfo), Bad maxclients, svc_updateuserinfo, customization, the event lookup, CL_LinkPlayers (17 x 0x74C, found in
  checkpoint 2), the prediction loops, the HUD message bounds (ScoreInfo, TeamInfo, ScoreAttrib, Spectator, the
  spectator's next / previous / find / menu), CVoiceStatus's loops.
- **Host code**: CVoiceGameMgr's ClientConnected, PlayerHasBlockedPlayer, ClientCommand and UpdateMasks (65-bit masks
  in host memory: the original indexes arrays of one-dword masks with a x4 scale no operand can widen), the client's
  HandleVoiceMaskMsg and UpdateServerState ("vban" with 3 dwords), StatShot's Send and its client handler.
- **Protocol 47** (docs/networking.md): svc_pings' slot 8 bits, VoiceMask and StatShot masks 3 dwords, every version
  check and writer raised (an original console or older build is refused at connect).
- **Spawns**: TeamFull and its three inlined copies compared a team's players with its map's spawn point count; the
  counts are set to 65 once counted (hook 0x14837E; a team with no points stays closed). The Xbox spawn choice
  (0x11EAA0) does not telefrag: with every point of a team taken it picks one of the occupied points (0x125BF0).
  Players on one point overlap (collision Off) or push apart (On); a death at spawn is accepted (Player Collision
  Off is the setting for crowded maps).
- **trigger_hurt**'s per tick mask (1 << (entindex - 1) in pev->impulse, aliasing players 33..64) is a 65-bit set per
  trigger in host memory (hooks 0x16B9B2, 0x16B9DD, 0x16B9F5).
- **Validation** of player indices from the wire where the original writes a table unchecked: svc_pings, Radar, the
  field of view message, DeathMsg, two talk messages, StatShot AddDamage, StudioDrawPlayer's entity number; the
  prediction's player physents kept within 600.
- **Found by running 64 bots** (the static audit had not): g_StudioRenderer (0x5693D0) +0x40B0 is a dword per
  player [17] indexed by the drawn player's entity number - 1 (StudioDrawPlayer 0x190D80). Players 18 and up wrote
  past it, through the renderer's own fields into its mesh batch vector (0x56D564): player 60 set the vector's end
  pointer below its start and the mesh drawing (0x194E80) crashed a moment after the 65th player joined (found from
  the core dump and CSX_WATCH on the vector). Moved to a guest table; a hook checks the index against it.
- **Scoreboard**: its list shows about 20 rows and does not scroll (checked in the window with 65 players); it now
  pages: 20 rows at a time, the next page every 5 s while it stays open, the first page each time it opens (hook
  0x1B98A0 skips the rows off the page). vk_bots64_scoreboard's four shots show all 65 players, both teams' headers.
- **Map changes**: a new map is a new match, which the front end starts by relaunching the title (each boot maps the
  guest tables anew). The console's changelevel and map do not change maps in this Xbox build (changelevel drops the
  bots and stays, map shows LostHost): the same with 17 players (de_dust2, 16 bots) as with 65.
- **Measurements, headless** (de_aztec, 64 bots and the player, regress_bots_aztec_64): 65 players, 557 of 1760
  edicts, teams 32 / 33, 60 of 64 bots walking 100 units in 20 s, kills and three round ends; guest memory 42.2 MB
  committed at most (37 MB with 16 bots), 72.1 MB reserved, 1734 MB free in one block: no memory limit is near.
  de_dust2 (8 / 8 spawn points) and collision Off pass the same checks.
- **Performance** (the Vulkan window 1920x1080, VSync Off, limit 240, de_aztec, the player held at one view for 55 s
  after the bots joined: vk_perf_bots16 / _bots64): 16 bots 116.5 frames/s, 95th percentile interval 11.4 ms, 99th
  13.0 ms; 64 bots 67.5 frames/s, 33.4 ms and 39.6 ms. GPU 0.81 / 0.95 ms a frame, CPU recording 4.87 / 5.93 ms, the
  server frame 0.67 / 5.54 ms mean (worst second 7.3 / 11.0 ms), of which the bots' AI, navigation, the game rules
  and the network 0.63 / 5.23 ms; the game uses one core fully in both (103 % of one core: the game thread is the
  limit, as with 16 bots in G8.2). Memory committed 42.2 / 42.0 MB. **The bottleneck at 64 bots is the bots' think
  time** (each bot scans every player with up to 5 traces: grows with the square of the count, 8x the server time
  for 4x the bots) and its unevenness (the 95th percentile frame is 2.3x the mean: bots think in bursts). Visibility
  is not timed apart from the AI (they share the bots' think functions). Not optimized in this checkpoint (no AI
  rewrite without approval); candidates: spreading the bots' vision scans over frames, cheaper visibility tests.
- **Vulkan validation** (CSX_VK_VALIDATION=1, the 64-bot scoreboard run): no new messages; only the three known from
  gamescope's WSI layer (TODO.md: swapchain_maintenance1 without surface_maintenance1, VkSwapchainPresentModeInfoKHR
  with swapchainCount 0).
- **Tests**: headless 42 of 42 (tests/run_regression.sh: 36 at once; the six that read the local player's position
  through tools/drive.py's state probe failed until the probe read predicted_players where it moved, then passed);
  their expectations changed where the feature changed: regress_bots_dust2_16 and _nocollide (all 16 bots join: 17
  players on 16 points, no spacing asserted On), regress_bots_aztec_24 (all 24 join: 25 players). Tool suites:
  recomp_patches, slot_audit_test (every relocated table patched, the patch file matching its configs),
  guest_tables_test. Window: the default suite 27 of 27 (tools/run_window_tests.py), vk_bot_count_169 (the front end
  to a match and back), vk_bots64_scoreboard, vk_perf_bots16 / _64, the validation run.
- **Not tested**: voice chat (no voice device on PC: the masks and messages are exercised every update, nothing is
  heard); StatShot's statistics are sent and parsed (the regression scenarios where the player fires), their tracker
  (the Xbox Live statistics) shows nothing on PC; a human second player (one process, the loopback client only).

## Player slot tooling (2026-10-23, G8.2B checkpoint 1)

Tools for raising the 17 player slots to 65 safely (docs/player_slots_plan.md); no player table is moved and no
player count changed yet.

- **Instruction operand patches** (SDK patch 0036, sdk/tools/recomp/patches.cpp): a `[[patch]]` in the recompiler
  config names one instruction (address, its bytes, optionally its text), one operand (`imm` or `disp`, `index` when
  there are two of a kind) and the old and new value. xbe_recomp checks the image's sha256 (`[xbe] sha256`, now in
  config/game.toml), the bytes, the text and the old value before analysis, and after it that analysis decoded an
  instruction there; it refuses branches, calls, ret and enter, new values wider than the operation, and two patches
  for one operand; every failure is reported and nothing is generated. Patches apply where instructions are decoded,
  so analysis and code generation see the same values; the generated code carries the full value (an imm8 compare can
  take 0x80) and marks each patched instruction; <out>/patches.txt lists them all with the instruction before and
  after. The XBE file is never changed. Without patches the generated code is byte for byte what it was. Also
  `--describe ADDRESS` (an instruction's bytes, text and operands: the fields a patch needs) and `--listing FILE`
  (every analysed instruction by function, for audits). A full recompile takes 2 s.
- **The player slot audit** (tools/slot_audit.py, config/player_slots.toml, analysis/player_slots_inventory.json):
  reads the listing, so it covers the code the program contains. 20 tables (the server's lag compensation, ping and
  loss tables, client_t's voice bits, the client's frame ring player states, cl.players, predicted players, the
  model cache, six HUD tables, CVoiceStatus, the ScorePanel, the game's voice masks, StatShot, CSoundEnt) with their
  direct, end-pointer, alias, stride and object-field references; 76 classified single sites (limits, sizes, protocol
  fields, engine limits, values of 17 that are not player counts); every other compare or load of 16, 17, 18, 32 and
  33 in the engine, front end, game and HUD as a candidate, classified by pattern where the code shows it (block
  copies, string compares, switch bounds, weapon ids, enumerations). Found beyond the investigation: the HUD's
  ScoreInfo, TeamInfo and ScoreAttrib handlers bound the player index at 17 (0x1B5890, 0x1B5A20, 0x1D5AAA), the
  front end's max players loops (0xE3DF9..0xE3E3E), CVoiceStatus's clears and loop of its 17 pointers (0x1B7E52,
  0x1B7FCE, 0x1B91AC). Counts: cl.players 54 direct and 6 end-pointer references in 27 functions, g_PlayerExtraInfo
  96 and 7 in 39, the game's voice masks 69, the frame ring 18 absolute player state and 60 frame field references
  and 86 stride multiplies in 33 functions.
- **The frame ring** (the highest risk): besides the absolute references, a register trace through each function
  that computes frame addresses (a register holding an imul by 0x1B60 or a frame address, followed through copies,
  adds and lea until overwritten) finds 236 accesses through frame-derived registers, 50 of them in the player states,
  in 17 functions (0x1D270, 0x23B70, 0x23B9D, 0x23D8D, 0x242F0, 0x24FE0, 0x254B0, 0x25BF0, 0x25C4D, 0x25EC0, 0x26000,
  0x30900, 0x31420, 0x3158D, 0x323B0, 0x32EB0, 0x92B90); a wider net lists 275 register-relative accesses past the
  first player state and 127 within it in the frame functions (many are cl_entity_t and entity_state_t fields), and
  10 pointer walks by 0xFB. The trace does not follow branches or stack spills: it narrows the review, it does not
  prove coverage.
- **Unresolved** (in the inventory): the server's write of the protocol version (46: checked at 0x30945, 0x2E822,
  0x98C78, written somewhere not yet found), the scoreboard widget's visible rows, the client's MAX_PACKET_ENTITIES
  check, 34 compares or loads of 17 not yet classified (listed by address), and the frame ring review lists above.
- **Guest tables** (src/common/guest_tables.cpp, config/guest_tables.toml, tools/guest_tables.py): a region of the
  guest's user space (0x70000000, 16 MB of address space) taken out of the game's allocator before the title starts;
  each table ends at a page boundary with an uncommitted guard page after it, so the first byte past its end faults,
  and the fault is reported with the table and element (Linux: SIGSEGV handler chained before the previous one;
  Windows: a vectored handler that reports and passes the fault on); an access before a table faults only below its
  first page. Element() refuses an out of range index for host code. A layout is validated (alignment, page ends,
  guard pages, no overlap, inside the region, 32-bit arithmetic) and a region in use is refused; a layout that
  cannot be mapped stops the program before the title runs. No production table yet, so nothing is mapped today.
- **Tests**: tests/recomp_patches.py (5 patches applied, 16 refusals alone and 3 together, determinism, unchanged
  output without patches), tests/slot_audit_test.py (27 cases: synthetic and the real program, the committed
  inventory matching a fresh audit), tests/guest_tables_test.py (38 cases: the generator and the runtime's guest
  memory, faults in child processes). Runtime: the headless harness and window build with the new start-up path;
  regress_boot_to_spawn, regress_player_control, regress_player_collision_on and regress_bots_aztec_16 pass (with
  CSX_LOG_MEMORY=1); the new files also compile for the Windows target (syntax check; the full Windows build was not
  run).

## Guest memory (2026-10-23, G8.2B checkpoint 1)

What the "64 MB" is, measured (CSX_LOG_MEMORY=1, headless) and read from the code (analysis/memory_architecture.md;
the proposal is docs/memory_modernization.md).

- The 64 MB is only the contiguous pool (textures, vertex and sound buffers, movie frames, the host's frame buffers
  and upload ring; 63 MB after the kernel's first MB). The game's heap (the XAPI NT heap: doubling segments, large
  blocks reserved alone) lives in the 2 GB user range, committed on demand; the engine's hunk, zone and cache are
  malloc'ed lists, and its -heapsize and -zone options are dead code (Sys_InitMemory 0xAD310 parses -heapsize and
  discards it). Nothing sizes itself from MmQueryStatistics' 64 MB.
- de_airstrip from the front end: 37.5 MB of user memory committed (38.6 MB reserved in 16 reservations), 25.6 MB of
  the contiguous pool. de_aztec with 16 bots: 37.2 MB peak committed (52.1 MB reserved in 15 reservations, all between
  0x10000 and 0x3640000; 1994 MB of the user range free, in one block: no fragmentation), 22.6 MB contiguous; with 2
  bots the same within 0.5 MB. System pool 0.4 MB.
- The engine's allocators never check for NULL: running out of heap would be a write to guest page 0 and a crash
  without a message (Sys_Error only for map images and texture creation).

## Bot count (2026-10-23, G8.2)

The question: how many bots a single player match can hold, against a target of 64, and what limits it. Answered
from the XBE (engine and game DLL in one image), then checked in matches.

- **The original.** The single player match's Match Options offer "Number of Bots" 1 to 11 (default 7; "+wait
  +bot_quota N" on the match's command line, joined by 0xE2270); the quick matches' presets use 7, 7, 9 and 11. With
  Bot Team on one team the screen's code (0xE1D10) caps the list at "8 bots" and disables "9 bots" and up (its loop
  runs to the fifteenth choice: the list was laid out for more).
- **Player slots: 17.** The title starts every match with "-maxplayers 17 +maxplayers 17" (its command line template
  at 0x33AA60). The engine's SV_Init (0xA1960) takes -maxplayers, clamps it to 1..17 and sets svs.maxclientslimit to
  17 (0xA1A10), allocating 17 client_t of 0x5010 bytes. The client is built for the same 17: svc_updateuserinfo
  refuses a slot of 17 or more (0x2CA2C, "svc_updateuserinfo > MAX_CLIENTS"), "Bad maxclients" checks the server's.
  56 compares against 17 in the code (27 in the engine: per-player loops such as one over 0x74C-byte records at
  0x25FE5; 20 in the game DLL; 9 elsewhere) are that constant. Every map ran with maxclients 17.
- **Bots.** The bot manager adds bots while bot_quota asks and slots remain ("Unable to create bot: Server is full
  (%d/%d clients)"), and while the bot's team has room ("Could not add bot to the game: Team is full": a team is full
  at its map's spawn points, the title's rule). bot_quota 24 gives 16 bots and nothing past the slots.
- **Spawn points** (info_player_start the Counter-Terrorists', info_player_deathmatch the Terrorists', counted with
  the probe's spawns request on all 20 maps): 8 and 8 on cs_havana, de_airstrip, de_dust, de_dust2, de_fastline,
  de_piranesi, de_tides, de_truth; cs_italy and cs_office 10 / 10; cs_militia 8 / 10; cs_miami 16 / 8; de_cbble 11 /
  8; de_prodigy 10 / 8; de_vostok 12 / 12; de_aztec 16 / 18; de_chateau 18 / 17; de_corruption, de_inferno and
  de_stadium 16 / 16. So 17 players fit every map whose two teams together have 17 spawn points; on the 8 / 8 maps the
  game takes 15 bots with the player and refuses the sixteenth (the team is full), which is the title's own guard
  against two players in one spawn.
- **Edicts.** sv.max_edicts is 1040; de_airstrip uses 640 without bots, de_aztec 353 with 16 bots: room to spare.
- **Network.** A match here is one process (the client on the loopback). The protocol carries player slots in bytes
  and the original clients (the same engine) accept slots below 17 only; 17 players is the title's own maximum, so a
  17-slot match is what an original console expects. Beyond 17 every client would have to change with the server.
  System link and Live were not run (not in this work).
- **Past 17 slots** the engine, the HUD and the game DLL need their per-player storage moved to larger tables (17 is
  compiled into all three; the recompilation adds no limit of its own), svc_pings needs a wider slot field, and the
  maps need more spawn points: investigated in docs/player_slots_plan.md (with analysis/player_slots_*.md), not
  implemented yet. This part of G8.2 is the baseline: 16 bots (with the player, the original 17 slots).
- **The setting** (src/game/pc_menu.cpp ExtendBotCount, PcBotCountLimit): the list becomes "0 bots" to "16 bots"
  (default still "7 bots"; "0 bots" first, "12 bots" to "16 bots" after "11 bots"), with a second help line ("16 bots
  and you fill the game's 17 player slots."). The screen's code reads the list by position, so its cap is redone for
  the longer list by a hook at 0xE1D2B (the table and the Bot Team choice in hand, it leaves at 0xE1D63): bots on one
  team are capped at "8 bots" and "9 bots" to "16 bots" are disabled, as the original; the arrows pass over disabled
  choices to the first one ("0 bots" now, "1 bot" before). Keyboard, mouse and pad drive the list as before. The game
  keeps the screen's choices from one front end to the next (after a match and Quit, 16 is still chosen). Separate
  Counter-Terrorist and Terrorist counts: the bot manager has one bot_quota and bot_join_team (any, ct, t), so separate
  counts would need a second quota in the bot code; not done (not needed by the architecture).

- **Spawns with every spawn point in use.** On de_dust2 with 16 bots (8 and 8, every point taken) the game's spawn
  choice (unchanged) can put a player on an occupied point at the match's first spawn: with collision Off, two
  players stood exactly in one spot in 2 of 8 first spawns, and bodies passing through each other came within 31
  units in most runs (Off allows that); with collision On no two within 31 units in 7 runs (presumably the movement's
  stuck check moving a player placed in another out at once: inferred, the placement itself was not caught On). In the freeze times after round ends none in any run. Collision Off leaves two
  players in one spot until they walk off, harmlessly (they pass through each other). After four round ends (the win
  limit) the match restarts and every configuration, the original's 8 bots on one team included, showed players on
  one spot for a moment: the title's own restart.
- **Tests.** regress_bots_aztec_7 (the original default: 8 players), _11 (the original's most: 12), _16 (17),
  regress_bots_dust2_16 (16: the sixteenth bot refused, 8 / 8), _dust2_16_nocollide (16, collision Off: the bodies
  left out of at least 12 movers' moves) and regress_bots_aztec_24 (24 asked: 17, the rest refused): the players
  counted, spawned apart at the start and in the freeze time after two round ends (not with collision Off), every
  bot walking 100 units or more in 20 s, kills and two round ends in the game's log (1 minute rounds), the teams within
  one (8 and 9, 8 and 8, 6 and 6, 4 and 4), edicts 324 to 360 of 1040 on de_aztec. vk_bot_count_43 / _169 / _219 (FOV
  120): the list to 16, de_aztec with 17 players, the scoreboard showing all 17 (CT Forces 8, Terrorist Forces 9, no
  row cut), the HUD at FOV 120; _169 also the one team cap at 8 and the choice kept after Quit.
- **Performance** (the window, 1920x1080, VSync off, limit 240, 60 s measured after joining; CSX_LOG_SERVERTIME for
  the server frame: every player's and bot's move, the bots' AI and navigation, the game rules and the loopback
  messages). de_aztec with the view held (setview, the same picture): 0 / 7 / 11 / 16 bots: 147.6 / 145.3 / 141.9 /
  137.0 frames/s, 95th percentile 7.46 / 7.69 / 8.07 / 8.83 ms, 99th 8.42 / 8.74 / 9.23 / 10.03 ms, GPU 1.109 to 1.133
  ms (the same), CPU recording 4.26 to 4.36 ms (the same), server frame 0.100 / 0.206 / 0.264 / 0.384 ms mean (worst
  0.22 / 3.72 / 5.12 / 5.94), of which moves 0.005 / 0.015 / 0.020 / 0.031 ms, peak memory 170.1 / 172.6 / 172.6 /
  172.7 MB. With the player's own view (de_aztec, de_dust2, cs_havana; the frame rate follows what the player looks
  at, 74 to 227 frames/s): server frame 0.08 to 0.13 ms without bots, 0.14 to 0.55 ms with 7 to 16 (worst 8.6 ms),
  memory +2 to +5 MB. The cost of 16 bots is about 0.3 ms of server time a frame on average: 7 % of the frame rate
  at 140 frames/s, nothing on the GPU. AI and navigation are not timed apart (they share the bots' think functions);
  the network is the loopback here.

Remaining uncertainty (this part): system link and Live with more bots were not run. Grenade use and objectives with many bots are
not asserted: in the tests' and benchmarks' minutes the logs show kills in every run with bots and one bomb plant (de_dust2,
16 bots), no grenade kill; whether bots throw grenades as often with 16 as with 7 was not measured. The 8 / 8 maps take
15 bots by the title's own team-full rule.

Suites (part one): headless 39 of 39, window 44 of 44; Vulkan validation of the HUD scenario and a 16 bot match: no
new messages (the swapchain present mode and shader output warnings seen since G6 only); the perf scenarios as G7
within noise at 60, 120 and 240 frames/s (one
low sky view at 120, 117.8 frames/s, was not there on a rerun: 119.5 against G7's 119.7), peak memory the same.

## Buy menu taps (2026-10-22)

- The title opens the buy menu on +buymenu's release (0x18FCB0, then 0xE62C0), and only for a tap: held 0.05 to 0.5
  s of real time (the press time from QueryPerformanceCounter at 0x552E70, the bounds the doubles at 0x365FD0 and
  0x365D10), with two of the pad's axes (0x384F8C and 0x384F90 for the active pad) under 0.4. A longer hold does
  nothing. An accepted tap queues "xbui canbuy" (0xCA6F0), which sends "impulse 53"; the server answers with the
  SrtBuyMenu message, whose handler (0x1ADC80) queues "xbui buymenu" with no newline (the string at 0x35F054), and
  "xbui buymenu" opens T_ or CT_BuyMenuRoot when the player is in a buy zone (0x552346) and no spectator screen is up.
- That missing newline is the title's own race: CHud::Think (0x1AE680) checks the client's settings every 0.5 s of
  client time (m_flTime 0x562438, the next check at 0x563F9C) and sends "m_pitch 0.022\n" each time, the float cvar
  never equalling the double it is compared with. When that check runs in the frame SrtBuyMenu arrives, the command
  buffer holds "xbui buymenum_pitch 0.022" and no menu opens (seen in the buffer itself: "xbui buymenum_pitch
  0.022"). On the console a pad tap meets it as rarely as a frame's chance; the tests met it about one buy in three.
- The tests' hold was a second, separate cause: `pckey cmd +buymenu`, `sleep 0.2`, `pckey cmd -buymenu` held it 0.2
  to 0.5 s of drive.py's polling, sometimes more (0.6 s fails 3 of 3, as the title means it to). A hold made exact
  (0.266 s) still failed 2 of 5, which is how the race was found.
- The fix is the harness's, not the game's: `pckey tap +CMD SECONDS` (drive.py) has pc_keys.cpp press the command on
  the frame just after the HUD's check (when 0x563F9C moves on), so the server's answer lands well before the next
  one, and release it SECONDS later by the host's steady clock; it logs the hold, and drive.py fails a hold outside
  SECONDS to SECONDS + 0.1. The title's timing and rules are unchanged (a 0.7 s tap is still refused).
- Checked: regress_flashbang 20 of 20 headless, regress_buy_tap 3 of 3 (taps 0.08 and 0.35 s open the menu and 0.7 s
  does not, at 30, 60, 120 and 240 frames per second), the other headless buy scenarios, and in the Vulkan window
  vk_flashbang_rates 3 of 3, vk_buy_tap 5 (every tap right; one run's 240 fps sample read 203, the other four 229 to
  240), vk_flashbang 2 of 2, the four coverage tests and vk_auto_exposure. Holds
  logged 0.200 to 0.233 s for the 0.2 s taps (one frame at 30 fps), 0.080 to 0.100, 0.350 to 0.367 and 0.700 to
  0.704 in regress_buy_tap; no buy failed.

## High pixel density (2026-10-21)

- On the desktop (Hyprland, 3840x2160 at scale 1.6) the window was made without SDL_WINDOW_HIGH_PIXEL_DENSITY, so its
  Wayland surface had its logical size: full screen 2400x1350, the swapchain 2400x1350, and Hyprland scaled the
  picture up 1.6x. The 3D was drawn at the Resolution setting (3840x2160), shrunk into the 2400x1350 swapchain and
  enlarged again by the compositor (soft). SDL's desktop mode was the logical 2400x1350 too, so Native meant that.
- Now the window asks for high pixel density: the surface has the display's pixels (full screen 3840x2160, a 581x643
  window 930x1029), the swapchain and Native follow them, and the desktop mode and the Resolution list are given in
  pixels (mode x pixel density). Window sizes, the pointer and the menus' mapping stay in logical units, as SDL's
  events give them: menu clicks land as before. The window logs both ("window: size 581x643 (930x1029 pixels,
  density 1.60, display scale 1.60)"). CSX_HIDPI=0 asks for the old logical surface.
- The software renderer's output takes the pixels the same way (SDL_GetRenderOutputSize).
- gamescope has no fractional scale, so the window tests (scale 1) are unchanged; desk_hidpi.txt is the desktop check
  (run on the scaled desktop: PC Options by clicks, the full-screen swapchain at 3840x2160).
- Unexplained once: in one full window-test run vk_vsync_play's game process ended at 109.6 s in a match (VSync off,
  240) with no core dump, no journal entry, no error and no exit line; two reruns and the next full run passed. Only a
  default SIGPIPE ends a process that silently; the window now reports SIGPIPE (and carries on), SIGTERM / SIGHUP /
  SIGINT and a plain exit in run.out, so a recurrence says how it ended. (A different crash, SIGSEGV in the
  runtime's socket table lookup under the network code, sub_ED5B0, is in a core from 2026-10-20 20:43: not seen
  since, not investigated yet.)

## Raw mouse input (2026-10-21)

Reported: at up to 240 frames per second (VSync off) aiming felt as if mouse acceleration were on.

The path, traced:
- SDL3 3.4.18 on the desktop runs the native Wayland driver (Hyprland offers wp_fifo_v1, so SDL prefers Wayland);
  relative mode, on while playing, goes through zwp_relative_pointer_v1 with zwp_pointer_constraints_v1 (a lock):
  SDL refuses relative mode without both, so there is no warp emulation (now logged: "input: relative mouse mode on
  through ..."; a refusal is logged with SDL's error). Under gamescope (the window tests) it is X11 with XInput2 raw
  motion.
- SDL's motion (SDL3 source read, 3.5.0's, the same logic as the installed 3.4.18 is assumed): Wayland sends the
  protocol's dx_unaccel / dy_unaccel, X11 the XInput2 raw values, unless SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE is set
  (then the compositor's accelerated deltas times the window's pointer scale, or the X server's valuators); the
  relative speed scale hint is unset. Switching relative mode drops motion still queued and puts the pointer back
  where it was. Hyprland's own acceleration is off anyway (input:accel_profile flat, sensitivity 0).
- The game: a captured motion becomes degrees (counts x 0.022 x the sensitivity, Invert flipping the pitch) added
  after the title's stick look in its look routine (0x19E160; hook at 0x19E4EB, "Mouse look"); the stick term
  there is the stick's alone (squared stick x factors x frame time), and only spawn and setangle copy the engine's
  angles back (0x30BB7, 0xCA172). No curve, smoothing, filtering or frame time touches the mouse: the same counts
  turned 9.9 / 165 / 132 degrees exactly at 240, and 75.9 at 144, 120, 60 and 30 frames per second.
- What was uneven: the Vulkan window loop read events after a fixed 5 ms sleep. At 190-240 frames per second (4-5 ms
  frames) a frame got one batch, none or two: with a steady 1000 Hz stream the turn per look pass varied up to twice
  its mean (spread, the deviation over the mean, 0.25-0.30; at 30 frames per second 0.08). Nothing was lost (the
  totals were exact), but the speed on screen wavered from frame to frame, which reads as acceleration. Now the loop
  waits for the next event (SDL_WaitEventTimeout, 5 ms at most) and reads the mouse as it comes: spread 0.08-0.11 at
  190-240 (the rest is the frame time's own variation), at most 0.46 degrees in a pass for 0.34 on average (was 0.66),
  0.02 at 30. The test request files are read every 5 ms, not at every wake.
- This is the one defect measured; whether it was all of what was felt needs the play-test. At 215 frames per second
  on the 120 Hz display (IMMEDIATE, the compositor showing the latest frame) the frames shown are also unevenly
  spaced in game time.

Raw Mouse Input (PC Options, after Invert Mouse; pc_settings.ini "mouse_raw", On without a line):
- On: the hint off, SDL's relative motion is the device's own counts (Wayland's unaccelerated deltas, X11's raw
  values): no desktop acceleration, no display scale or DPI in it (1.6x fractional scaling does not enter the counts).
  This is what the game already used; the setting makes it explicit and logged ("input: Raw Mouse Input on: ...").
- Off: the hint on, the desktop's pointer motion (the compositor's acceleration and speed, and on Wayland the
  window's pointer scale; the X server's on X11). With Hyprland's flat profile that is the same speed apart from the
  pointer scale, not verified with a physical mouse. The game adds nothing either way.
- Applied at once (SDL reads the hint at every motion), from the window thread when the setting changes.
- Menus, PC Options, PC Controls, the profile name, buy menus, the pause menu and the console keep the pointer:
  relative mode is off there and motion goes to the menu pointer or nowhere; the spectator's camera turns through
  the same look path (vk_spectator.txt).
- Software renderer: its window loop is paced by its SDL renderer's present (VSync), so it still reads the mouse once
  per display refresh; the Vulkan renderer (the one played) waits on events.

Checks: vk_mouse_raw.txt (Raw Mouse Input on by default and the native path logged; motions pushed from an SDL timer
thread at 500-2000 a second, arriving between the window's polls as a mouse's do: slow, medium and fast streams turning
exactly at 240, 144, 120, 60 and 30, the turn spread at 240 at most 0.18, sensitivity 1.5, Invert Mouse, full screen,
no turn from motion over the pause menu and the open console or after them, Off from the pause menu's PC Options
saved and applied, On again); regress_pc_options.txt (the row saved at its default); the PC Options tests moved one
row down. A physical mouse on the desktop is still needed for the Wayland backend's deltas and for Off.

## VSync and the 120 frames per second limit (2026-10-21)

Reported: with Frame Rate Limit 240 the game stayed at about 120.

Measured (the Vulkan window's "frame timing" line every second: frames the title presented, successful swapchain
presents, frame intervals, time blocked acquiring and presenting, the limiter's sleep (app::PacedNanoseconds, SDK
patch 0029), the present mode, the display's refresh from SDL; "pc frame" every second: movement commands and their
milliseconds; per-thread CPU from top):
- The display: HDMI-A-1 3840x2160 at 120.00 Hz (Hyprland 0.56.2, misc:vrr 1, general:allow_tearing false); SDL
  reports 120.00 Hz for the window's display. The RX 9070 XT (RADV) offers IMMEDIATE, MAILBOX, FIFO and FIFO_RELAXED
  on the Wayland surface; gamescope's WSI layer offers MAILBOX and FIFO.
- The swapchain always used FIFO (vsync). On the desktop at a 240 limit: 120.0 frames/s, 120.0 presented, intervals
  8.33 ms (99% under 8.41), 2.2-2.4 ms of every frame blocked in vkAcquireNextImageKHR, the limiter 0 ms. The play-test's
  own log (cs_miami, internal 3840x2160): 120.0 for 998 of 1005 seconds, about 3 ms a frame blocked in the present.
  The 120 was VSync at the display's refresh, not the game: nothing else waited.
- VSync off on the same desktop and match spawn (de_airstrip, eight bots): IMMEDIATE 178-179, MAILBOX 174-185
  frames/s, nothing blocked (acquire 0.03 ms, present 0.02, limiter 0), one guest thread at 98.5% CPU. With the limit at
  1000 the same: 170-190. So the game is CPU-bound on its one game thread, about 5.3-5.8 ms a frame here: the
  renderer's recording 2.6-2.7 ms (about 1,100 draws), the GPU 0.8-0.9 ms (it overlaps), the title's own code (client,
  listen server, bot AI) the rest. In other views and with idle bots up to 232-235; headless (no window) 240 at the
  start and a median of 191 through three rounds of fighting. The title's own limits (fps_max, Host_FilterTime, the
  emulated 60 Hz vertical blank, which CS does not wait on) do not hold it below the limit.
- 240 can be reached where a frame costs under 4.2 ms (menus, light views); during a match with bots on this machine
  it is about 170-235. Above 120 it needs VSync off.

VSync (src/vulkan/vk_backend.cpp, src/game/pc_menu.cpp, src/linux/window_main.cpp):
- PC Options > VSync (On / Off, pc_settings.ini "vsync" 1 / 0; no line is On, as before). The present modes the
  surface offers are queried once and logged ("vulkan: present modes offered"). On: FIFO. Off: IMMEDIATE when offered,
  else MAILBOX, else FIFO. Each swapchain logs its mode ("vulkan: swapchain WxH, N images, present mode M (VSync
  on|off)"). CSX_VK_PRESENT=immediate|mailbox|fifo|fifo_relaxed still forces a mode for measurements.
- IMMEDIATE never waits; a frame replaces the shown one at once and may tear where the compositor allows it. MAILBOX
  never waits and never tears: the newest frame is shown at the next refresh and older ones are dropped. On Hyprland
  without allow_tearing and an immediate window rule the compositor shows the latest frame at its own refresh, so
  IMMEDIATE should not tear there and should look like MAILBOX (frames past 120 rendered, 120 a second reaching the
  display): Hyprland's documented behaviour, not measured on the screen. The game's frame rate, input and movement
  run at the higher rate either way.
- A change takes effect at the next present: the backend sees the settings version change and makes the swapchain
  again from the old one (oldSwapchain), nothing else. The SDL window is not touched: size, full screen, focus and
  mouse capture stay, and it is not minimized (now logged: "window: minimized" / "restored").
- The Frame Rate Limit is independent: the limiter paces first, then FIFO caps at the refresh. VSync On + 240 is
  the display's rate; VSync Off + 144 is 144; Off or On + 60 is 60 (measured at 120 Hz).
- Software renderer: the title renders on its own threads and the window loop shows the latest frame through an
  SDL renderer; VSync sets that renderer's vsync (SDL_SetRenderVSync, applied on change), which paces only the
  window's presents, never the game, so its frame rate is the limit's alone. With VSync off the loop sleeps 1 ms when
  no new frame came.

No Unlimited choice: in a match the game is CPU-bound below 240 here, so an Unlimited setting gives no more than
240 there, while menus would run at the engine's own fps_max ceiling with nothing verified above 240 (GoldSrc
movement at under 4 ms a command, frame-keyed code). The list stays 30 (Original) to 240.

Gameplay at the new combinations (the msec carry of "Frame rate" above unchanged): movement 1000 ms a second at
every rate, two seconds of walking 500 units and a 45-unit jump at VSync On + 240 (120), Off + 240, Off + 144,
Off + 60 and On + 60 (vk_vsync_play.txt); headless at 240: the Glock fires 6 shots for 30 presses as at 30 and 144,
three bot rounds with kills and a time-out win (300 s), 17.2 s from each round's end to the next start as at 30 and
144, the movement milliseconds equal to the engine's frame time in every second.

Missed before because there was no VSync setting to test, every window test ran at FPS=30 (below any refresh) and
the renderer's log reported only the frame rate, not where a frame's time went.

Checks: vk_vsync.txt (front end at 60 Hz: modes listed, a saved vsync = 0 starting with MAILBOX past 60, On and Off
from PC Options saved to pc_settings.ini, the swapchain made again, 60 / 240, full screen kept, not minimized, no
Display Mode applied), vk_vsync_play.txt (in a match at 120 Hz: FIFO by default at 120, Off from the pause menu's PC
Options, the mouse captured again, the combinations above with movement, jumps and the window untouched).

## Play-test bugs: walking right, grenades, the spectator's camera (2026-10-20)

Walking right (reported: D did nothing):
- The play-test's pc_settings.ini had bind.moveright = e and other keys the player never chose (Next Weapon Down Arrow,
  Previous Weapon Left Arrow, Last Weapon Space): no capture was logged for them. In the menus the keyboard is the
  pad, WASD the left stick; PC Controls (as PC Options) has the stick pieces of the game's Control Options screen,
  whose right and left run tableIncreaseValue / tableDecreaseValue on the selected row. Holding D (or pushing the
  controller's stick) on a row stepped its key unseen: on Move Right d became e, Accept saved it, and D was bound to
  nothing in the match. Reproduced headless with the pad's stick on Move Right (d -> e).
- Fix (src/linux/window_input.cpp): on a PC binding screen the keyboard is not the stick (its letters are keys a
  player presses there to bind or try them); the controller's stick and d-pad keep the screen's original behaviour.
- Missed because no test pressed movement keys on PC Controls or checked that bindings stay as they were.
- Movement itself was right: W, S, A, D and W+D each walk along the view (W+D at -45 degrees), nothing moves once
  the keys are let go, and after the console or the pause menu the keys walk again (a wall stands about 170 units
  right of the de_airstrip spawn: a test walking right twice stops there).
- The play-test's own pc_settings.ini keeps the changed keys until PC Controls' Reset (or the rows) puts them back.

Grenades (reported: they could not be selected):
- The client's weapon slots are GoldSrc's (CHudAmmo, gHUD.m_Ammo 0x5624D4): SelectSlot (0x1AC1A0; slot1..slot5, and
  invnext 0x1AC940 / invprev, which in this build pick the next weapon's slot through it) selects at once with
  hud_fastswitch only when the slot holds one item; with two or three grenades it leaves its choice pending
  (gpActiveSel 0x548730) for a confirmation. This build has none: there is no attack confirmation (gpLastSel is only
  written by cancelselect), and the ammo HUD's draw (0x1ACC70) clears the choice after a second (g_weaponselect
  0x552C78, its timer 0x552C7C). The controller selects with cycle_grenade / cycle_weapon instead. So with one HE
  grenade 4 worked, with an HE, a flashbang and a smoke grenade 4 showed the list and nothing came of it, and the
  wheel stopped at the knife; Mouse 1 fired the pistol.
- Fix (src/game/pc_keys.cpp, config/game.toml): at SelectSlot's store of the pending choice (0x1AC313) a fast-switch
  choice is selected at once as its one-item path does (ServerCmd with the weapon's name, g_weaponselect, the
  timer); at its entry (0x1AC1A0) a slot key steps on from the weapon in hand when it is in that slot, as invnext
  does (0x1AC996). 4 selects the HE grenade, again the flashbang, the smoke grenade, the HE grenade; the wheel down
  goes knife, HE, flashbang, smoke; the wheel up enters the slot at its first grenade (invprev goes by slots in this
  build); BLACK (cycle_grenade) is unchanged. Each grenade is thrown with Mouse 1 and the pistol comes back.
- The weapon in hand for tests: the client data of the engine's first frame record (0x74B3D8, static; all records
  alike once a weapon is held a moment): 17 Glock, 29 knife, 4 HE, 25 flashbang, 9 smoke, 6 C4, 19 MP5; state lines
  end with it (src/game/state_probe.cpp), drive.py `expect weapon`.
- Missed because the binding tests used one-item slots (knife, C4) and never bought more than one grenade.

The spectator's camera (reported: it could not be turned):
- After dying the Spectator screen opens (0x5523D4 set), and the Scoreboard while Tab is held; both have
  ControlPassThrough "1": the controls stay the game's (the controller's right stick turns the camera, its buttons
  change target and mode). The PC input treated every open screen as a menu: the mouse was freed as a pointer and the
  keys became the pad, so the mouse did not turn the camera, and holding Tab stopped walking and looking.
- Fix (src/game/pc_keys.cpp, src/game/pc_mouse.cpp): a screen with ControlPassThrough (read from the menu text) is
  play for the PC keys and mouse: the mouse stays captured and turns the view (the look hook works there), keys are
  the game's ("pc keys: Spectator passes the controls through").
- Missed because no window test died or held the scoreboard while moving the mouse.

Checks: vk_movement.txt (the window: PC Controls left as it was with WASD held, every direction, after the console
and the pause menu), regress_pc_grenades.txt (three grenades: 4, the wheel, BLACK, throws), vk_spectator.txt (the
window: walking with the scoreboard held, the spectator's camera turned by the mouse in two modes and after the pause
menu).

## Developer console (2026-10-20)

What the Xbox build keeps of GoldSrc's console (CS 5659):
- **Output**: Con_Printf (0x40790) adds one line per call, not per newline (echo prints each argument with a call
  of its own, so each word gets a line), to a ring of 200 lines of 78 characters (0x744060; newest 0x55191C, shown
  from 0x551918, wrapped 0x551914). condump and condebug (qconsole.log) exist.
- **Drawing**: Con_Draw (0x40B20) draws ten lines from (30, 30), 18 apart, in the game's own font (the text renderer
  0xBCBC0, colour 255 200 100), a ">>>" prompt at (30, 240) and the input line (0x743FE0, length byte 0x551920, up to
  99 characters) at (80, 240), while the flag 0x551910 is set; no backdrop. SCR_UpdateScreen calls it (0x55F87) before
  the game's menus (0x55F94), the VGUI paint (0xB4660) during play.
- **Commands**: toggleconsole (0x40630, flips the flag), consoleup / consoledown (0x40920 / 0x40970, a line), and
  consoleexecute (0x40A40: a newline appended, Cbuf_AddText, the line cleared). messagemode, messagemode2 and
  condebug point at an empty function (0x1141E0). Unused leftovers of a pad-driven console: 0x40A90 (button bits:
  1 executes, 0x10 closes) and 0x40AC0 (the stick scrolls), with no callers.
- **Gone**: GoldSrc's line editor (Key_Console), its history and completion. Nothing writes the input line;
  Key_Event (0x66510) knows key destinations game (0), chat (1: Key_Message 0x65B90) and menu, and its console-key
  path (0x66743, the toggle for keys flagged in 0x6CB7E0 while 0x785458 is set) is dead, the table is never filled.

Restored (src/game/pc_console.cpp, the window's input in src/linux/window_input.cpp), on the original console:
- Options > PC Controls > Toggle Developer Console (both menus; pc_settings.ini bind.console, default ` "left of 1").
  The key is bound to toggleconsole with the other actions; the window takes the key left of 1 by its position
  (SDL_SCANCODE_GRAVE), so it is the console key on any layout (` US, § Swedish, ^ German).
- While the console is open the window sends it the keyboard: SDL text input (the layout's characters, printable
  ASCII kept), Backspace, Enter, Up / Down (history, 64 commands), Tab (completion of the first word from the engine's
  command list 0x5518E4: next +0, name +4, and the cvar array 0x434A20 / count 0x551928; one match completes, several
  extend to the common prefix and are listed), Page Up / Down and the wheel (consoleup / consoledown, three lines),
  Escape (closes it; it does not open the game menu). Text goes into the game's own input line; Enter echoes "]line"
  through Con_Printf as GoldSrc does and runs it through consoleexecute. Game keys held when it opens are released,
  mouse buttons and motion are ignored, the mouse is let go and captured again on closing if it was captured.
- The pad is not taken: it keeps playing or navigating the menus while the console is open, as the console never
  took the pad (the pad-driven leftovers are not wired).
- Over a menu (the pause menu, the front end) SCR_UpdateScreen's menus cover the console; a hook after them
  (0x55F99) runs Con_Draw once more, so it is drawn above the menu (it has no backdrop: its lines can overlap the
  menu's text). In play it is drawn once, as before.
- At 16:9 / 21:9 it is centred with the rest of the 2D (it draws with the text renderer, see "Widescreen").
- Limitations: no cursor movement inside the line (typing and Backspace at its end), no backdrop, ten lines shown
  (Page Up / Down scroll through the 200 kept). (The PC Controls list stepping over "-" was the list's enable bytes
  running out: section "Press-a-key rebinding", fixed 2026-10-20.)

Checks: regress_pc_console.txt (4:3: default key, typing, echo, completion, history, a typed toggleconsole closing
it, the pad jumping meanwhile, the pause menu's PC Controls rebinding it and only the new key opening it),
regress_pc_console_wide.txt (21:9, F2 from pc_settings.ini, sv_gravity 600 set and read back, centred);
vk_console.txt (Vulkan window at 16:9: the ` key, W and mouse motion not reaching the game, text input, Enter,
Up, Backspace, Escape closing without the pause menu, mouse capture restored, the console over the pause menu).

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
