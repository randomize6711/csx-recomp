# XDK 5659 Direct3D [data] and [layout] for Counter-Strike default.xbe

Derived from CS code with llvm-objdump on `build/ghidra/default_flat.exe` (flat image at real VAs). Every value cites
the CS instruction(s) that prove it. "dev" = the CDevice at 0x314B40; "mp" = the miniport embedded at dev+0x1C28
(addressed as `lea 0x1c28(dev)` at 0x306752/0x306776/0x30599A ...).

**Result: 37/37 layout keys proven, 9/9 data symbols proven (incl. optional D3D_FvfShader). 0 unproven.**
**Surprise: every layout value is identical to 5849.** Only the [data] addresses differ (different image).

## CS functions identified on the way (used as proof sites)

| Address | Function (by behaviour) | Notes |
|---|---|---|
| 0x306460 | D3DDevice_SetTexture | eax = Stage, 1 stack arg = pTexture (ret 4) |
| 0x306610 | D3DDevice_SetIndices | ebx = pIndexData, 1 stack arg = BaseVertexIndex (ret 4) |
| 0x3060E0 | D3DDevice_SetTransform | eax = State, edx = pMatrix (ret 0) |
| 0x306240 | D3DDevice_SetViewport | 1 stack arg pViewport |
| 0x304650 | GetViewportOffsetAndScale (inline/LTCG form) | reads viewport, supersample, screen offset, z scale |
| 0x306CF0 | D3DDevice_SetScreenSpaceOffset | 2 stack floats; adds 0.53125 (constant at 0x3135CC) |
| 0x305A30 | D3DDevice_SetRenderTarget | 2 stack args |
| 0x305D40 | D3DDevice_GetBackBuffer2 | 1 stack arg; was 0 (missing) in config |
| 0x306670 | D3DDevice_GetDisplayFieldStatus (shape) | |
| 0x3066B0 | D3DDevice_BlockUntilVerticalBlank | KeWaitForSingleObject import 0x321208 |
| 0x309660 | D3DDevice_Swap | |
| 0x309D10 | D3D_SetFence | |
| 0x304F70 | D3DDevice_SetPixelShader | eax = Handle |
| 0x3070C0 | D3DDevice_SetStreamSource | eax = StreamNumber, ebx = pStreamData, 1 stack arg = Stride (ret 4) |
| 0x3073D0 | D3DDevice_SelectVertexShader | eax = Handle (other register not examined) |
| 0x304310 | D3DDevice_SetTextureState_TexCoordIndex | esi = Stage, 1 stack arg = Value (ret 4) |
| 0x3042E0 | D3DDevice_SetRenderState_Simple | fastcall ecx = method, edx = value; was 0 in config |
| 0x306D40 | render/texture state query (class 0x89 / 0x8A / 0x8B) | name not established |
| 0x308870, 0x308B11.. | state block apply loops (inlined SetRenderStateNotInline) | |
| 0x30C640.. | frame buffer init | |
| 0x30C300.. | device teardown | |
| 0x30E270 | vertical blank handler | |
| 0x30E1A0 | flip/gamma update | |
| 0x30EC70.. | display mode set | |

The register conventions above are observations made while reading these functions, not full [abi] proofs (out of
this task's scope); they may help the [abi] work.

## [data]

| Symbol | Value | Proof |
|---|---|---|
| D3D_Device | 0x314B40 | .XTLID 0x80001 → 0x314B40; SetRenderState_Simple 0x3042E0 insn 0 reads it (same anchor as the 5849 signature) |
| D3D_pDevice | 0x316FE8 | .XTLID 0x80003 → 0x316FE8; Direct3D_CreateDevice 0x30A551 stores 0x314B40 there |
| D3D_DirtyFlags | 0x316FF0 | SetTexture 0x3064ED/0x3064F3 ors 0x4800 into it; SelectVertexShader 0x3073EA/0x3073F1 ors 0x70; SetTransform 0x30610B/0x306110 |
| D3D_RenderState | 0x316FF8 | SetPixelShader 0x305075 `mov edi, 0x316FF8` + `rep movsd` of the shader definition (5849: imm in SetPixelShader); state query 0x306D64 reads [0x316FF8 + rs*4]; state apply 0x3088B9 stores [0x316FF8 + rs*4] |
| D3D_DeferredTextureState | 0x314930 | state query 0x306D93..0x306D98 reads [0x314930 + (stage*32 + type)*4] (stage < 4, type < 0x20); SetTextureState_TexCoordIndex 0x30431F/0x304322 stores at 0x3149A0 + stage*0x80 = base + kTssTexCoordIndex(28)*4 |
| D3D_Streams | 0x317FC8 | SetStreamSource 0x307117 stores Stride at 0x317FC8 + n*12, 0x30711D the buffer at 0x317FD0 + n*12 (Stride is the struct's first field, 5849 Stream_Stride = 0) |
| D3D_IndexData | 0x317E98 | SetIndices 0x306627/0x30662A stores pIndexData->Data ([ebx+4]) there, 0x306631 stores 0 for a null buffer |
| D3D_SimpleRenderStateEncode | 0x3283A8 | state apply 0x3088AB: rs < 0x5C loads ecx = [0x3283A8 + rs*4] and calls SetRenderState_Simple 0x3042E0 (also 0x308B70); the 92 entries are NV2A methods 0x40260..0x41E78 |
| D3D_FvfShader (optional) | 0x317EB0 | device init 0x30C111 stores 0x317EB0 into dev+0x794, then calls SetVertexShader 0x307AD0 with 4 (D3DFVF_XYZRHW) |

## [layout]

| Key | Value | Same as 5849 | Proof |
|---|---|---|---|
| CDevice_Push | 0x0 | yes | SetTexture 0x3064AF reads [dev], 0x3064DC stores the advanced pointer; SetRenderState_Simple 0x3042E0/0x3042F0 |
| CDevice_PushThreshold | 0x4 | yes | SetTexture 0x3064B1 compares the push pointer with [dev+4]; SetRenderState_Simple 0x3042E8 |
| CDevice_BaseVertex | 0x1C | yes | SetIndices 0x30665E stores BaseVertexIndex |
| CDevice_IndexBuffer | 0x7A0 | yes | SetIndices 0x30663B reads the old one, 0x306661 stores the new one |
| CDevice_ZScale | 0x948 | yes | 0x30471C/0x30472C scale the viewport Z range by it |
| CDevice_SuperSampleX | 0x95C | yes | 0x304670 scales viewport X by it; SetViewport 0x306341 |
| CDevice_SuperSampleY | 0x960 | yes | 0x304692 scales viewport Y by it; SetViewport 0x306395 |
| CDevice_Transforms | 0xC60 | yes | SetTransform 0x3060EE..0x3060FC copies 16 dwords to +0xC60 + State*64 |
| CDevice_Textures | 0xF88 | yes | SetTexture 0x306473 reads, 0x3064A6 stores [stage] |
| CDevice_Palettes | 0xF98 | yes | state query 0x306DC7 reads [stage] (stage < 4); teardown 0x30C307..0x30C353 releases 4 entries there (and 4 at +0xF88) |
| CDevice_RenderTarget | 0x1A04 | yes | SetRenderTarget 0x305AA7 stores the new target |
| CDevice_Depth | 0x1A08 | yes | SetRenderTarget 0x305ACF stores the new depth surface |
| CDevice_BackBuffer | 0x1A14 | yes | GetBackBuffer2 0x305D5D returns [+0x1A14] for index 0 |
| CDevice_FrontBuffer | 0x1A18 | yes | GetBackBuffer2 0x305D44..0x305D5D returns [+0x1A18] for index -1 |
| CDevice_SwapCount | 0x2478 | yes | Swap 0x3096BE/0x309700 increments it, 0x309741 returns it |
| CDevice_Size | 0x24A0 | yes | Direct3D_CreateDevice 0x30A58C clears 0x928 dwords of the device on failure |
| CDevice_Flags | 0x8 | yes | Direct3D_CreateDevice 0x30A542..0x30A565 ors BehaviorFlags & 0x10 into it; SetPixelShader 0x305067 tests that bit |
| CDevice_Time | 0x2C | yes | D3D_SetFence 0x309DAF..0x309DBF adds 2; SetTexture 0x306486 copies it into the resource being released |
| CDevice_PixelShader | 0x784 | yes | SetPixelShader 0x304F84 stores the handle |
| CDevice_VertexShader | 0x794 | yes | device init 0x30C111 stores the FVF shader object 0x317EB0; SelectVertexShader 0x3073F7 the current one |
| CDevice_BackBufferScale | 0x96C | yes | frame buffer init 0x30C6E6 stores the X scale as a float, 0x30C6FF the Y scale at +4 |
| CDevice_Viewport | 0xEE0 | yes | SetViewport 0x3063E1..0x30640B stores X, Y, Width, Height, MinZ, MaxZ |
| CDevice_ScreenSpaceOffset | 0xEF8 | yes | SetScreenSpaceOffset 0x306D01 stores x + 0.53125, 0x306D11 y + 0.53125 at +4 |
| CDevice_AutoDepth | 0x1A20 | yes | frame buffer init 0x30CA99 stores the depth surface (+0x1A6C); default state 0x30B49F reads it to set ZEnable via 0x303B00 |
| CDevice_VBlankCallback | 0x1DB8 | yes | vblank handler 0x30E33E loads [mp+0x190] (= dev+0x1DB8), 0x30E365 calls it |
| CDevice_VBlankEvent | 0x1DBC | yes | BlockUntilVerticalBlank 0x3066BB clears its SignalState (+0x1DC0), 0x3066C7 waits on it; vblank handler 0x30E331 signals mp+0x194 |
| CDevice_DisplayFlags | 0x1DDC | yes | mode set 0x30EC87 stores the mode entry at mp+0x1B4; vblank handler 0x30E2DD and 0x306682 test its field bits |
| CDevice_VBlankCount | 0x1DE8 | yes | vblank handler 0x30E296..0x30E2A3 increments mp+0x1C0; 0x306675 reads it |
| CDevice_Field | 0x1DF8 | yes | vblank handler 0x30E304/0x30E313 stores mp+0x1D0; 0x30668F reads it |
| CDevice_GammaRamps | 0x1E04 | yes | flip 0x30E204..0x30E213 applies mp+0x1DC + index*0x300; SetGammaRamp-area 0x306F0E addresses +0x1E04 |
| CDevice_GammaPending | 0x2404 | yes | flip 0x30E1FA tests [mp+0x7DC + index*4] == 1, 0x30E218 clears it; mode set 0x30EC78 sets it |
| DirtyFlags_Texture | 0x4800 | yes | SetTexture 0x3064ED ors it into D3D_DirtyFlags |
| TextureState_Stage | 32 | yes | SetTextureState_TexCoordIndex 0x30431F shifts the stage by 7 (0x80 bytes); state query 0x306D93 stage*32 + type |
| Stream_Size | 12 | yes | SetStreamSource 0x3070D6/0x3070D9 indexes by n*3*4 |
| Stream_Stride | 0x0 | yes | SetStreamSource 0x307117 stores Stride at D3D_Streams + n*12 |
| Stream_Buffer | 0x8 | yes | SetStreamSource 0x30711D stores the vertex buffer at D3D_Streams + 8 + n*12 |
| PixelShader_Definition | 0x8 | yes | SetPixelShader 0x304FF2/0x305019/0x30502E read the definition pointer at [handle+8] (CreatePixelShader not located) |

## Ready to paste: xdk/5659/layout.toml

```toml
# D3D8 CDevice, the static device D3D_pDevice (0x316FE8) points at (0x314B40). All values match 5849.
CDevice_Push          = 0x0     # D3DDevice_SetTexture 0x3064AF reads it, 0x3064DC stores the advanced pointer
CDevice_PushThreshold = 0x4     # D3DDevice_SetTexture 0x3064B1 compares the push pointer with it
CDevice_BaseVertex    = 0x1C    # D3DDevice_SetIndices 0x30665E stores BaseVertexIndex
CDevice_IndexBuffer   = 0x7A0   # D3DDevice_SetIndices 0x30663B reads the old one, 0x306661 stores the new one
CDevice_ZScale        = 0x948   # viewport offset/scale 0x30471C scales the Z range by it
CDevice_SuperSampleX  = 0x95C   # viewport offset/scale 0x304670 scales viewport X by it
CDevice_SuperSampleY  = 0x960   # viewport offset/scale 0x304692 scales viewport Y by it
CDevice_Transforms    = 0xC60   # D3DDevice_SetTransform 0x3060EE copies 16 dwords to +0xC60 + type * 64
CDevice_Textures      = 0xF88   # D3DDevice_SetTexture 0x306473 reads, 0x3064A6 stores [stage]
CDevice_Palettes      = 0xF98   # state query 0x306DC7 reads [stage], the device teardown 0x30C307 releases them
CDevice_RenderTarget  = 0x1A04  # D3DDevice_SetRenderTarget 0x305AA7 stores it
CDevice_Depth         = 0x1A08  # D3DDevice_SetRenderTarget 0x305ACF stores it
CDevice_BackBuffer    = 0x1A14  # D3DDevice_GetBackBuffer2 0x305D5D returns it for index 0
CDevice_FrontBuffer   = 0x1A18  # D3DDevice_GetBackBuffer2 0x305D5D returns it for index -1
CDevice_SwapCount     = 0x2478  # D3DDevice_Swap 0x3096BE increments it, 0x309741 returns it
CDevice_Size          = 0x24A0  # Direct3D_CreateDevice 0x30A58C clears 0x928 dwords of the device on failure
CDevice_Flags             = 0x8    # Direct3D_CreateDevice 0x30A542 ors BehaviorFlags & 0x10 into it (0x30A565), D3DDevice_SetPixelShader 0x305067 tests that bit
CDevice_Time              = 0x2C   # D3D_SetFence 0x309DB2 adds 2 (0x309DBF stores), D3DDevice_SetTexture 0x306486 reads it
CDevice_PixelShader       = 0x784  # D3DDevice_SetPixelShader 0x304F84 stores the handle
CDevice_VertexShader      = 0x794  # the device init 0x30C111 stores the FVF shader object 0x317EB0, D3DDevice_SelectVertexShader 0x3073F7 the current one
CDevice_BackBufferScale   = 0x96C  # the frame buffer init 0x30C6E6 stores the X scale as a float, 0x30C6FF the Y scale at +4
CDevice_Viewport          = 0xEE0  # D3DDevice_SetViewport 0x3063E1 to 0x30640B stores X, Y, Width, Height, MinZ, MaxZ
CDevice_ScreenSpaceOffset = 0xEF8  # D3DDevice_SetScreenSpaceOffset 0x306D01 stores x + 0.53125, 0x306D11 y + 0.53125 at +4
CDevice_AutoDepth         = 0x1A20 # the frame buffer init 0x30CA99 stores the depth buffer, the default state 0x30B49F reads it
CDevice_VBlankCallback    = 0x1DB8 # the vertical blank handler 0x30E33E loads [miniport + 0x190] and calls it (0x30E365), the miniport is at +0x1C28
CDevice_VBlankEvent       = 0x1DBC # D3DDevice_BlockUntilVerticalBlank 0x3066C7 waits on it, 0x3066BB clears its SignalState
CDevice_DisplayFlags      = 0x1DDC # the mode set 0x30EC87 stores the mode entry at miniport + 0x1B4, the vertical blank handler 0x30E2DD tests it
CDevice_VBlankCount       = 0x1DE8 # the vertical blank handler 0x30E2A3 increments miniport + 0x1C0; 0x306675 reads it
CDevice_Field             = 0x1DF8 # the vertical blank handler 0x30E304 stores the field at miniport + 0x1D0; 0x30668F reads it
CDevice_GammaRamps        = 0x1E04 # the flip 0x30E20A applies miniport + 0x1DC + index * 0x300
CDevice_GammaPending      = 0x2404 # the flip 0x30E1FA tests [miniport + 0x7DC + index * 4], 0x30E218 clears it
# D3D8 globals
DirtyFlags_Texture    = 0x4800  # D3DDevice_SetTexture 0x3064ED ors it into D3D_DirtyFlags
TextureState_Stage    = 32      # D3DDevice_SetTextureState_TexCoordIndex 0x30431F: stage * 0x80 bytes; state query 0x306D93 stage * 32 + type
Stream_Size           = 12      # D3DDevice_SetStreamSource 0x3070D6 indexes the stream table by n * 12
Stream_Stride         = 0x0     # D3DDevice_SetStreamSource 0x307117 stores Stride
Stream_Buffer         = 0x8     # D3DDevice_SetStreamSource 0x30711D stores the vertex buffer
PixelShader_Definition = 0x8  # D3DDevice_SetPixelShader 0x304FF2 reads the definition pointer
```

## Ready to paste: config/game.toml [data]

```toml
D3D_DeferredTextureState    = 0x314930  # state query 0x306D98 reads [0x314930 + (stage * 32 + type) * 4]
D3D_Device                  = 0x314B40  # .XTLID 0x80001
D3D_DirtyFlags              = 0x316FF0  # D3DDevice_SetTexture 0x3064ED ors 0x4800 into it
D3D_FvfShader               = 0x317EB0  # the device init 0x30C111 stores it into CDevice_VertexShader
D3D_IndexData               = 0x317E98  # D3DDevice_SetIndices 0x30662A stores pIndexData->Data
D3D_pDevice                 = 0x316FE8  # .XTLID 0x80003
D3D_RenderState             = 0x316FF8  # D3DDevice_SetPixelShader 0x305075 copies the definition to it
D3D_SimpleRenderStateEncode = 0x3283A8  # state apply 0x3088AB passes [0x3283A8 + rs * 4] to SetRenderState_Simple
D3D_Streams                 = 0x317FC8  # D3DDevice_SetStreamSource 0x307117 stores Stride at it + n * 12
```

## Extra [symbols] found (were 0 in config)

```toml
D3DDevice_GetBackBuffer2       = 0x305D40  # index -1 -> +0x1A18, 0 -> +0x1A14, adds a reference
D3DDevice_SetRenderState_Simple = 0x3042E0  # fastcall: ecx = method, edx = value, appends to the push buffer
D3DDevice_SetScreenSpaceOffset = 0x306CF0  # stores x/y + 0.53125 at +0xEF8/+0xEFC (2 stack floats; check the runtime's declared convention)
```

Also: 0x313330 is a table of complex render-state setter pointers indexed by render state (state apply 0x3088E7,
`call [0x313330 + rs*4]`; e.g. [0x31356C] = 0x303B00 = SetRenderState_ZEnable). It could locate several of the
missing `D3DDevice_SetRenderState_*` symbols (not done here).
