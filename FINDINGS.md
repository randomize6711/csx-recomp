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
- Rebinding is by the list (left / right on a row) with keyboard or pad; pressing the wanted key on a row is not
  offered (the original lists have no capture; TrapKey_Event's trap mode could be used).
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
