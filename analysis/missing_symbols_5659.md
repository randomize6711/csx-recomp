# Missing host-replaced symbols — Counter-Strike default.xbe (XDK 5659)

Scope: the 108 `[symbols]` in `config/game.toml` that are `0` without a "pending [abi] proof" comment.
Method: `.XTLID` table (330 id/address pairs, dumped from the XBE), 5849 signature constants (`k`, `ret`, `regs`,
`xtlid`), call graphs and bodies from `build/ghidra/flat.dis` (llvm-objdump of the flat PE), function starts from the
recompiler's `x86_func_mapping.cpp`. All addresses below are CS addresses; every claim cites an instruction.

| Class | Count |
|---|---|
| FOUND | 36 |
| INLINED (LTCG folded into callers) | 9 |
| NOT LINKED | 51 |
| UNPROVEN | 12 |

## Key facts used throughout

- **XTLID ids are the same in 5659 and 5849** (37 exports matched by id in the signature experiment) and sort
  alphabetically by export name within a library: e.g. IDirectSoundBuffer_GetStatus 0xB0054 < Play 0xB0059 < Release
  0xB005D < SetBufferData 0xB0060 < … < StopEx 0xB007E. The 5849 signatures carry the ids of the missing DSOUND
  exports (SetFrequency 0xB006A, Lock 0xB0056, Unlock 0xB007F, …); none of them is in CS's `.XTLID`.
- XTLID is **not exhaustive**: two buffer wrappers XACT calls (0x246428, 0x246444) have no id. So "NOT LINKED" for
  DSOUND also rests on enumerating every wrapper in the DSOUND export block (0x246207–0x2464B8, 0x246A19–0x246A76,
  0x24709A–0x2470DB, 0x2475B7–0x247A74) and every call from other sections into DSOUND (.text, XMV, XACTENG, XPP):
  no unnamed wrapper of the right arity exists.
- **D3D render state array is 0x316FF8** (`D3D_RenderState`): game code indexes it directly, `0x124BC
  mov edx,[0x316FF8+edx*4]`, `0x1267E mov [0x316FF8+edi*4],esi`. With the runtime's own enum
  (`sdk/runtime/gpu/state.h`: kRsPsTextureModes 136 … kRsDoNotCullUncompressed 165) slot = 0x316FF8 + 4*state, i.e.
  complex states 0x317218..0x31728C. Each complex setter stores exactly one slot (listed below), which names it.
  Anchors already in the config agree: PSTextureModes 0x3042B0→0x317218, FogColor 0x303980→0x317220, ZEnable
  0x303B00→0x317234, StencilEnable 0x303BA0→0x317238, StencilFail 0x303C30→0x31723C, CullMode 0x3039E0→0x317244,
  LineWidth 0x303D20→0x31726C, Dxt1NoiseEnable 0x303D90→0x317274.

## FOUND (36)

Convention column: "stdcall N" = same as the runtime's declaration, no `[abi]` needed. "LTCG" = registers; the
observed map is recorded as evidence for the [abi] worker, **not** finalized here.

### Render-state setters (22) — all `stdcall 1` (value read at `[esp+4]` at entry, `ret 4`), matching `HLE_STDCALL(D3DDevice_SetRenderState_##name, 1)`

| Symbol | Address | Proof (store slot = state; 5849 constant) |
|---|---|---|
| D3DDevice_SetRenderState_VertexBlend | 0x304260 | 0x3042A3 stores 0x31721C (137); k 0x200 |
| D3DDevice_SetRenderState_FillMode | 0x304190 | 0x3041D4 stores 0x317224 (139); method 0x8038C |
| D3DDevice_SetRenderState_BackFillMode | 0x3041E0 | 0x3041EA stores 0x317228 (140), then tail-jumps to FillMode 0x3041F3 |
| D3DDevice_SetRenderState_TwoSidedLighting | 0x304200 | 0x304244 stores 0x31722C (141); method 0x417C4, dirty 0x9000 (5849 k 0x9000); tail-jumps to FillMode |
| D3DDevice_SetRenderState_NormalizeNormals | 0x303AB0 | 0x303AE9 stores 0x317230 (142); k 0x200, method 0x403A4 |
| D3DDevice_SetRenderState_FrontFace | 0x303A60 | 0x303A95 stores 0x317240 (146); method 0x403A0 (5849 k) |
| D3DDevice_SetRenderState_TextureFactor | 0x303CB0 | 0x303CFA stores 0x317248 (148); k 0x784 |
| D3DDevice_SetRenderState_LogicOp | 0x304130 | 0x304163 stores 0x317250 (150); method 0x417BC |
| D3DDevice_SetRenderState_EdgeAntiAlias | 0x303900 | 0x303932 stores 0x317254 (151); method 0x80320 |
| D3DDevice_SetRenderState_MultiSampleAntiAlias | 0x3040B0 | 0x3040BC stores 0x317258 (152); shares helper 0x3049E0 (method 0x41D7C, 5849 k) |
| D3DDevice_SetRenderState_MultiSampleMask | 0x3040F0 | 0x3040FB stores 0x31725C (153); helper 0x3049E0 |
| D3DDevice_SetRenderState_MultiSampleMode | 0x304050 | 0x304059 stores 0x317260 (154); k 0x1A04/0x1A14 |
| D3DDevice_SetRenderState_MultiSampleRenderTargetMode | 0x304080 | 0x304089 stores 0x317264 (155); k 0x1A04/0x1A14 |
| D3DDevice_SetRenderState_ShadowFunc | 0x303940 | 0x303975 stores 0x317268 (156); method 0x41E6C |
| D3DDevice_SetRenderState_SampleAlpha | 0x304110 | 0x30411B stores 0x317270 (158); helper 0x3049E0 |
| D3DDevice_SetRenderState_YuvEnable | 0x303E20 | 0x303E2B stores 0x317278 (160) |
| D3DDevice_SetRenderState_OcclusionCullEnable | 0x303E60 | 0x303E6B stores 0x31727C (161); k 0x1E00, 0x41D84 |
| D3DDevice_SetRenderState_StencilCullEnable | 0x303ED0 | 0x303EDB stores 0x317280 (162); k 0x1E00, 0x41D84 |
| D3DDevice_SetRenderState_RopZCmpAlwaysRead | 0x303F40 | 0x303F44 stores 0x317284 (163) |
| D3DDevice_SetRenderState_RopZRead | 0x303F60 | 0x303F64 stores 0x317288 (164) |
| D3DDevice_SetRenderState_DoNotCullUncompressed | 0x303F80 | 0x303F84 stores 0x31728C (165) |
| D3DDevice_SetRenderState_Simple | 0x3042E0 | appends ecx (method) and edx (value) to the push buffer 0x3042F5/0x3042F8, ret 0 — fastcall, matches `HLE_FASTCALL(…, 0)`; callers pass `ecx = [0x3283A8+state*4]` (0x12670, 0x3088AB) |

(ZBias, 149, is 0x303FA0 → stores 0x31724C; it is not in the missing list.)

### Texture-state setters (4) — LTCG

| Symbol | Address | Observed convention | Proof |
|---|---|---|---|
| D3DDevice_SetTextureState_BumpEnv | 0x304420 | Stage=eax, Type=stack1, Value=stack2; ret 8 | 0x30447D stores `[0x314930+(stage*32+type)*4]`; method `0x41AD0+(stage*16+type)*4` (0x304463); k 0x784. Runtime declares stdcall 3 |
| D3DDevice_SetTextureState_BorderColor | 0x304490 | Stage=eax, Value=ebx; ret 0 | method `0x41B24+stage*0x40` (0x3044B7, 5849 k 0x41B24); 0x3044CB stores 0x3149A4+stage*128 |
| D3DDevice_SetTextureState_ColorKeyColor | 0x3044E0 | Stage=eax, Value=ebx; ret 0 | method `0x40AE0+stage*4` (0x304502, 5849 k 0x40AE0); stores 0x3149A8+stage*128 |
| D3DDevice_SetTextureStageStateNotInline | 0x304520 | Stage=eax, Type=edx, Value=ecx; ret 0 | `cmp edx,0xC` dispatch, 0x304540 stores `[0x314930+(stage*32+type)*4]`, dirty constant 0x480F (5849 k) at 0x304562 |

### Other D3D (10)

| Symbol | Address | Convention | Proof |
|---|---|---|---|
| D3DDevice_SetScreenSpaceOffset | 0x306CF0 | stdcall 2 (floats), ret 8 | adds [0x3135CC] = 0.53125 (bytes 0000083f) to each, stores device+0xEF8 / +0xEFC (0x306D01, 0x306D11) — the 5849 layout comment's exact behaviour |
| D3DDevice_SetFlickerFilter | 0x306CB0 | LTCG: Value=esi, ret 0 | calls kernel ordinal 2 AvSendTVEncoderOption (import 0x321250) with (miniport [dev+0x1C28], option 0xB, esi, 0) at 0x306CD3; caches value 0x317EA4. Only caller of option 0xB. BLACK maps it `["esi"]`. Runtime HLE is a no-op |
| D3DDevice_LoadVertexShader | 0x307250 | LTCG: Handle=eax, Address=stack; ret 4 | method 0x41E9C with Address at 0x3072AE/0x3072B4, copies program from handle+0x114 (5849 k 0x114, 0x41E9C). Same map as BLACK `["eax","stack"]` |
| D3DDevice_SetVertexShaderConstant1 | 0x307C40 | fastcall ecx=Register, edx=pData, ret 0 (= 5849) | methods 0x41EA4 + 0x100B80 (0x307C56/0x307C60), 4 dwords; length 0x60 matches 5849 `len 0x5F` of Constant1 (1Fast is 0x49) |
| D3DDevice_SetVertexShaderConstant4 | 0x307CA0 | fastcall ecx, edx, ret 0 (= 5849) | 0x41EA4 + 0x400B80, `add eax,0x4C` (5849 k 0x4C), MMX copy of 16 dwords |
| D3DDevice_End | 0x308820 | no args, ret 0 | method 0x417FC value 0 (0x30883D), `and 0xFFFFE7FF` device flags (0x308854), SetFence(1) — both 5849 constants |
| D3DDevice_MakeSpace | 0x30A2B0 | no args, ret 0 | `push [0x3181A4]>>1; call D3D_MakeRequestedSpace 0x30A170` — same shape as 5849 (len 0xF) |
| D3D_BlockOnResource | 0x30A3E0 | LTCG: pResource=eax, ret 0 (5849: stack, ret 4) | type tests 0x70000/0x50000 (0x30A3EF/0x30A3F5), blocks with D3D_BlockOnTime 0x30A000 on device time [dev+0x2C]; Lock2DSurface calls it with eax=container (0x30D295) |
| D3D_InitDefaultState | 0x30B3F0 | no args, ret 0 (= 5849) | SetTransform(i, identity 0x3137D0) for i<10 (0x30B400 loop), simple states via 0x3042E0, texture stage states via 0x304520/0x304310, reads AutoDepth dev+0x1A20 at 0x30B49F (the 5849 anchor). Only caller 0x30C152 |
| Lock2DSurface | 0x30D280 | LTCG: pContainer=stack1, FaceType=stack2, Level=stack3, pLockedRect=esi, pRect=eax, Flags=stack4; ret 0x10 | Flags tested 0x20/0x40 (0x30D286, 0x30D2C3), `or 0xF0000000` (5849 k), writes Pitch/pBits to [esi]/[esi+4] (0x30D2FB). Identical to BLACK `["stack","stack","stack","esi","eax","stack"]` |

### Ready-to-paste `[symbols]` lines

stdcall/fastcall ones (runtime declaration already matches, no `[abi]`):

```toml
D3DDevice_SetRenderState_VertexBlend                 = 0x304260  # stores RS[137] 0x31721C at 0x3042A3
D3DDevice_SetRenderState_FillMode                    = 0x304190  # stores RS[139] 0x317224 at 0x3041D4, method 0x8038C
D3DDevice_SetRenderState_BackFillMode                = 0x3041E0  # stores RS[140] 0x317228 at 0x3041EA, tail-jumps to FillMode
D3DDevice_SetRenderState_TwoSidedLighting            = 0x304200  # stores RS[141] 0x31722C at 0x304244, method 0x417C4
D3DDevice_SetRenderState_NormalizeNormals            = 0x303AB0  # stores RS[142] 0x317230 at 0x303AE9
D3DDevice_SetRenderState_FrontFace                   = 0x303A60  # stores RS[146] 0x317240 at 0x303A95, method 0x403A0
D3DDevice_SetRenderState_TextureFactor               = 0x303CB0  # stores RS[148] 0x317248 at 0x303CFA
D3DDevice_SetRenderState_LogicOp                     = 0x304130  # stores RS[150] 0x317250 at 0x304163, method 0x417BC
D3DDevice_SetRenderState_EdgeAntiAlias               = 0x303900  # stores RS[151] 0x317254 at 0x303932, method 0x80320
D3DDevice_SetRenderState_MultiSampleAntiAlias        = 0x3040B0  # stores RS[152] 0x317258 at 0x3040BC
D3DDevice_SetRenderState_MultiSampleMask             = 0x3040F0  # stores RS[153] 0x31725C at 0x3040FB
D3DDevice_SetRenderState_MultiSampleMode             = 0x304050  # stores RS[154] 0x317260 at 0x304059
D3DDevice_SetRenderState_MultiSampleRenderTargetMode = 0x304080  # stores RS[155] 0x317264 at 0x304089
D3DDevice_SetRenderState_ShadowFunc                  = 0x303940  # stores RS[156] 0x317268 at 0x303975, method 0x41E6C
D3DDevice_SetRenderState_SampleAlpha                 = 0x304110  # stores RS[158] 0x317270 at 0x30411B
D3DDevice_SetRenderState_YuvEnable                   = 0x303E20  # stores RS[160] 0x317278 at 0x303E2B
D3DDevice_SetRenderState_OcclusionCullEnable         = 0x303E60  # stores RS[161] 0x31727C at 0x303E6B
D3DDevice_SetRenderState_StencilCullEnable           = 0x303ED0  # stores RS[162] 0x317280 at 0x303EDB
D3DDevice_SetRenderState_RopZCmpAlwaysRead           = 0x303F40  # stores RS[163] 0x317284 at 0x303F44
D3DDevice_SetRenderState_RopZRead                    = 0x303F60  # stores RS[164] 0x317288 at 0x303F64
D3DDevice_SetRenderState_DoNotCullUncompressed       = 0x303F80  # stores RS[165] 0x31728C at 0x303F84
D3DDevice_SetRenderState_Simple                      = 0x3042E0  # fastcall ecx=method edx=value, pushes both at 0x3042F5
D3DDevice_SetScreenSpaceOffset                       = 0x306CF0  # x,y + 0.53125 [0x3135CC] -> dev+0xEF8/0xEFC, ret 8
D3DDevice_SetVertexShaderConstant1                   = 0x307C40  # fastcall ecx,edx; 0x41EA4 + 0x100B80 at 0x307C56
D3DDevice_SetVertexShaderConstant4                   = 0x307CA0  # fastcall ecx,edx; 0x41EA4 + 0x400B80 at 0x307CD8
D3DDevice_End                                        = 0x308820  # method 0x417FC value 0 at 0x30883D, flags & 0xFFFFE7FF
D3DDevice_MakeSpace                                  = 0x30A2B0  # push [0x3181A4]>>1; call D3D_MakeRequestedSpace 0x30A170
D3D_InitDefaultState                                 = 0x30B3F0  # default transforms/states, reads AutoDepth dev+0x1A20 at 0x30B49F
```

LTCG ones — address proven, convention observed; add only together with a reviewed `[abi]` map:

```toml
# D3DDevice_SetTextureState_BumpEnv        = 0x304420  # observed: Stage=eax, Type=stack, Value=stack; ret 8
# D3DDevice_SetTextureState_BorderColor    = 0x304490  # observed: Stage=eax, Value=ebx; ret 0
# D3DDevice_SetTextureState_ColorKeyColor  = 0x3044E0  # observed: Stage=eax, Value=ebx; ret 0
# D3DDevice_SetTextureStageStateNotInline  = 0x304520  # observed: Stage=eax, Type=edx, Value=ecx; ret 0
# D3DDevice_SetFlickerFilter               = 0x306CB0  # observed: Value=esi; ret 0 (runtime HLE is a no-op)
# D3DDevice_LoadVertexShader               = 0x307250  # observed: Handle=eax, Address=stack; ret 4
# D3D_BlockOnResource                      = 0x30A3E0  # observed: pResource=eax; ret 0
# Lock2DSurface                            = 0x30D280  # observed: stack, stack, stack, esi, eax, stack; ret 0x10
```

## INLINED (9)

| Symbol | Example inlined body |
|---|---|
| D3DDevice_Begin | game 0x6FFEF–0x70011: MakeSpace check, `mov [push],0x417FC; mov [push+4],edi` (primitive type) |
| D3DDevice_FlushVertexCache | game 0xAF495: `mov [push],0x41710` (5849 k 0x41710); also 0x194CD5, 0x19604F, 0x197232 |
| D3DDevice_SetGammaRamp | game 0xBE15A–0xBE1A7: compares/copies 0xC0 dwords to dev+0x1E04+idx*0x300, sets dev+0x240C and dev+0x2404+idx*4 (all 5849 k: 0xC0, 0x240C, 0x2478). **No function to replace**: the runtime must read the ramp from device memory (CDevice_GammaRamps 0x1E04 / GammaPending 0x2404, unchanged from 5849) |
| D3DDevice_GetBackBuffer2 | game 0xBDF73 `mov ecx,[dev+0x1A14]` followed by the D3DResource_AddRef body (0xFFFF/0x70000/0x50000 tests) |
| D3DDevice_GetRenderTarget2 | game 0xBFC6B `mov edi,[dev+0x1A04]` + inline AddRef |
| D3DDevice_GetDepthStencilSurface2 | game 0xBE213 `mov ecx,[dev+0x1A08]` + inline AddRef |
| D3D_InternalAddRef | `add dword [reg],0x80000` inside D3D functions: SetRenderTarget 0x305A6F/0x305A75, SetTexture 0x3064FF, SetIndices 0x306621, SetStreamSource 0x3070D0 |
| D3DDevice_SetRenderStateNotInline | game 0x12662–0x126AC: state<0x5C → SetRenderState_Simple([0x3283A8+state*4], v) + store; state<0x88 → `or [0x316FF0],[0x328188+state*4]` + store (5849 k 0x5C, 0x88); also 0x155BB, 0x6ECD3, 0xC08CB |
| D3DDevice_SetSoftDisplayFilter (probable) | no standalone function; AvSendTVEncoderOption option 0xE with constant 0 inside 0x30C0E6 (0x30C1E8–0x30C1EF). Runtime HLE is a no-op |

## NOT LINKED (51)

| Symbols | Evidence |
|---|---|
| BinkDoFrame, BinkGetSummary, BinkNextFrame, BinkWait | no BINK section (20 sections listed in FINDINGS.md); movies are XMV |
| crt_sin, crt_cos, crt_tan, crt_atan, crt_sqrt, crt_log, crt_log10 | the CRT bodies call the shared argument-check helpers 0x2354D5/0x2354EC; their only callers are crt_acos 0x22EAE5, crt_pow 0x22ECB0, crt_asin 0x22F461. Game code uses inline x87 instead: 104 fsin, 60 fcos, 281 fsqrt, 22 fpatan, 5 fptan, 3 fsincos in .text. The fldln2/fldlg2 fragments 0x2356C6/0x2356F5 are not referenced by any call, jump or pointer |
| IDirectSoundBuffer_GetCurrentPosition (0xB0053), _Lock (0xB0056), _SetAllParameters (0xB005F), _SetFrequency (0xB006A), _SetI3DL2Source (0xB006C), _SetMaxDistance (0xB006F), _SetMinDistance (0xB0070), _SetPosition (0xB0078), _SetRolloffCurve (0xB0079), _SetVelocity (0xB007B), _Unlock (0xB007F) | ids (from 5849 signatures) absent from `.XTLID`; the buffer wrapper block 0x246290–0x246444 holds 15 wrappers, all accounted for (see DSOUND table) — the two id-less ones take a struct pointer (0x245075) and a BOOL (0x2451A6), not these arguments |
| CDirectSoundStream_SetFrequency, _SetI3DL2Source, _SetMaxDistance, _SetMinDistance, _SetPosition, _SetVelocity | all CDirectSoundStream setters in 0x245ED5–0x2461B7 pop 8 (one argument); the 3D ones pop 12/20 in 5849. SetFrequency's slot is taken by SetPitch (below) |
| IDirectSound_CommitEffectData (0xB0038), _EnableHeadphones (0xB003D), _GetEffectData (0xB003F), _GetOutputLevels (0xB0040), _GetSpeakerConfig (0xB0041), _SetDistanceFactor (0xB0048), _SetDopplerFactor (0xB0049), _SetOrientation (0xB004D), _SetPosition (0xB004E), _SetRolloffFactor (0xB004F), _SetVelocity (0xB0050) | ids absent; the IDirectSound wrappers present are exactly 0xB0037/3A/3B/3C/45/4A/4B/4C/51; no unnamed CDirectSound method pops 0x14/0x20 (listener SetPosition/SetOrientation shapes) |
| DirectSoundCreateBuffer (0xB0003) | id absent; only 0x247A2D (DirectSoundCreate) and 0x247A74 (DirectSoundCreateStream) call the shared creation helper 0x2479C0 |
| DirectSoundUseLightHRTF (0xB0031) | id absent; UseFullHRTF 0xB002F is linked and called from .text |
| XAudioDownloadEffectsImage (0xB00A6) | id absent; the XAudio exports present are 0xB00A3–0xB00A5; CS loads dspimage.bin through IDirectSound_DownloadEffectsImage 0x246207 |
| getsockopt (0x40007), WSARecvFrom (0x40020) | ids absent; the XNET socket thunks 0x263056–0x263143 are contiguous and all named |
| D3DResource_BlockUntilNotBusy (0x8006A) | id absent (CS has 0x80069, 0x8006E, 0x8006F, 0x80071, 0x80072); its 5849 body is a jmp to D3D_BlockOnResource, which is found (0x30A3E0) and also inlined (game 0xAF4A5–0xAF4C0) |
| D3DDevice_Release | the only 0x928-dword clear is Direct3D_CreateDevice's failure path (0x30A58C); nothing in the D3D section touches the device refcount at +0x938 |
| D3DDevice_SetMaterial, D3DDevice_SetLight, D3DDevice_LightEnable | no store or `lea` of dev+0xF00/0xF44 (materials) or the light fields anywhere; the only references are reads in the internal lighting push 0x30FB30/0x30FC20 (0x30FB4D, 0x30FC0C) and 0x30FF6F/0x310006 (offsets unchanged from 5849) |
| D3DDevice_SetPalette | no store to dev+0xF98 (palettes) anywhere |
| D3DDevice_GetVertexShaderFunction | its error constant 0x88760867 occurs nowhere in the image |

## UNPROVEN (12)

D3D_KickOffAndWaitForIdle, D3DDevice_BlockOnFence, D3DDevice_InsertFence, D3DDevice_IsFencePending — no standalone
function with those shapes; KickOff 0x30A2C0, BlockOnTime 0x30A000 and SetFence 0x309D10 are called directly from game
code (e.g. KickOff at 0x197487, SetFence(1) at 0x72055, BlockOnTime at 0x194D05), most likely inlined but the bodies are
too small to tell apart. D3DDevice_BeginPush, D3DDevice_EndPush (no 5849 signature; game code writes the push pointer
through the device register, not distinguishable from other inlined pushes). D3DDevice_CreatePixelShader,
D3DDevice_DeletePixelShader, D3DDevice_DeleteVertexShader (allocator XMemAlloc 0x202E9A has 180 callers; no unique
pattern). D3DDevice_SetTile, D3DDevice_SwitchTexture (no constants). D3DDevice_SetVertexShaderConstant1Fast (only one
1-constant function exists, assigned to Constant1 by length).

## DSOUND exports CS links that the runtime does not handle (outside the missing list, important)

| XTLID | Address | Name (evidence) | Runtime |
|---|---|---|---|
| 0xB0067 | 0x2462E4 | IDirectSoundBuffer_SetEG — reaches voice method 0x244FEB, the same as IDirectSoundStream_SetEG (0xB008C, named by the 5849 xtlid) via 0x245FCB; id fits the alphabetical slot | no HLE |
| 0xB0076 | 0x2462AC | IDirectSoundBuffer_SetPitch (probable) — slot between SetOutputBuffer 0x75 and SetPlayRegion 0x77; shares voice method 0x244FA3 with stream export 0xB0099 (slot between SetOutputBuffer 0x98 and SetVolume 0x9E) | no HLE — **XACT drives pitch through this**, there is no SetFrequency |
| 0xB006D | 0x2462C8 | IDirectSoundBuffer_SetLFO (probable) — slot 0x6D between SetHeadroom 0x6B and SetLoopRegion 0x6E; shares voice 0x244FD8 with stream 0xB0092 | no HLE |
| 0xB0055 | 0x24640C | IDirectSoundBuffer_GetVoiceProperties (probable) — slot between GetStatus 0x54 and Play 0x59 (Lock is 0x56); voice method 0x24BC9C reads the hardware voice array 0x262DD0 and unpacks fields (a getter); shares it with stream 0xB0081 | no HLE |
| — | 0x246428 / 0x246444 | id-less buffer wrappers called by XACT: 3D parameter block copy (voice 0x245075, flag mask in dword 0) and a BOOL flag at voice+0xB bit 0 (0x2451A6). Names unproven | no HLE |
| 0xB0099/92/8D/90/96/82/80/81, 0xB009E | 0x246460–0x2464A0 | IDirectSoundStream_* thunks that jmp to CDirectSoundStream_* (the runtime's audit treats a jmp to a host function as covered) | covered where the CDirectSoundStream target is hooked; SetPitch/SetLFO/GetVoiceProperties targets (0x245ED5, 0x245F79, 0x246113) are not |
| 0xB003B | 0x247843 | IDirectSound_CreateSoundStream — slot between CreateSoundBuffer 0x3A and DownloadEffectsImage 0x3C; calls the same implementation 0x24766B as DirectSoundCreateStream 0x247A74 | no HLE; XACT calls it (0x247843 from XACTENG) |
| 0xB00A3 | 0x244C0C | XAudioCalculatePitch (probable) — slot before 0xB00A4/A5; converts a frequency: `cmp eax,0xBB80` (48000) returns 0, else float math via 0x24A592 | pure helper, safe as original code |
| 0xB00A4 / 0xB00A5 | 0x244C5B / 0x244C56 | XAudioCreateAdpcmFormat / XAudioCreatePcmFormat by alphabetical slot only (jmp 0x247C77 / 0x247C2D) | pure helpers |

These match the runtime's warning "exports running their original code: B00A3 … B003B".

## Side findings for the D3D layout worker (not acted on)

- `D3D_RenderState = 0x316FF8` (game indexing 0x124BC, 0x1267E; `0x317218 - 136*4`).
- `D3D_DirtyFlags = 0x316FF0` (`or [0x316FF0], …` at 0x304234, 0x30453B/0x304547, 0x3088D8).
- `D3D_SimpleRenderStateEncode = 0x3283A8` (indexed by state < 0x5C to get the method for SetRenderState_Simple at
  0x12670, 0x3088AB); deferred dirty-bit table 0x328188 (0x12697, 0x3088CF).
- Texture stage state array 0x314930 (`[0x314930 + (stage*32 + type)*4]`, 0x30447D, 0x304540), i.e. TextureState_Stage = 32.
- Device push pointer / limit are the first two dwords of D3D_Device (0x314B40/0x314B44 used by 0x3042E0, 0x307C40).
- SetTransform 0x3060E0 is called with eax = state, edx = pMatrix (0x30B400–0x30B407), useful for its pending [abi].
