# DSOUND methods XACT uses that the runtime does not implement (CS, XDK 5659)

Source: CS default.xbe, flat image `build/ghidra/default_flat.exe` (disassembly `build/ghidra/flat.dis`). Hardware
register names are taken from xemu `hw/xbox/mcpx/apu/apu_regs.h` (NV1BA0_PIO_*), used only to name what the code
writes; no code was taken from it. All addresses are CS 5659 virtual addresses.

Confidence: **proven** means read directly from the cited instructions. **probable** means the XDK name fits the
.XTLID alphabetical slot and the body, but the name itself is not proven. **uncertain** is marked as such.

## 0. Object model (proven)

- An `IDirectSoundBuffer*` (interface) is `CDirectSoundBuffer + 0x1C`. Every buffer wrapper does
  `ecx = iface ? iface - 0x1C : 0` (e.g. 0x2462AC..0x2462BD) and then calls a stdcall `CDirectSoundBuffer` method
  `(this, arg)` with `ret 8`.
- `CDirectSoundBuffer+0x0C` is the voice client (the `ecx` of the 0x24xxxx voice methods) and `+0x10` is the voice
  **settings**. `voice+0x70` is the same settings object.
- The `IDirectSoundStream` thunks at 0x246460..0x2464AA `jmp` to the `CDirectSoundStream` methods with no pointer
  adjustment. Those methods call the same voice-level functions as the buffer ones.

**Settings fields** (read in 0x24AF4E, 0x24B1A2, 0x24BC9C, 0x244FA3..0x245034):

| Offset | Field | Proof |
|---|---|---|
| +0x08 | creation flags | settings init 0x247240 stores them at 0x24724F |
| +0x0B bit 0 | flag `0x01000000`, the "use 3D voice data" gate (§2) | |
| +0x18 | base pitch | SetPitch 0x244FAE |
| +0x1C | volume minus headroom | SetVolume 0x244FC7/0x244FCA |
| +0x20 | headroom | SetHeadroom 0x245025; default 600 mB unless flags & 0x382010 (0x247259..0x247260), matching the runtime's `DefaultHeadroom` |
| +0x24 | mix bin count | |
| +0x28[] | mix bin ids (bytes) | |
| +0x30[bin] | per-bin volumes | 0x24B087..0x24B090 |
| +0xB4 | DS3D source parameter block, 0xA4 bytes | allocated only if flags & 0x10 (0x247267..0x24728E) |
| +0xB8 | 3D voice data block, 0x38 bytes, tag 'DSda' | allocated if flags & 0x400010 (0x2472DA..0x2472F1); allocation also sets the 0x01000000 gate (0x247305) |

**XACT's creation flags.** XACT ORs `0x400000` into both the stream and the buffer descriptor flags for its 3D
cues (0x2FBBF4..0x2FBBFC). It does **not** set DSBCAPS_CTRL3D (0x10). The library therefore does none of its own 3D
work for these voices; XACT supplies the computed 3D voice data. The runtime's `NewBuffer` only looks at
`flags & 0x10` (`is3d`), so it does not recognise these voices as 3D.

## 1. Set3DVoiceData: wrapper 0x246428 -> 0x245E3B -> 0x245075 (id-less; the name is my label)

- **Signature:** `(IDirectSoundBuffer* buf, const DS3DVOICEDATA* d)`, stdcall, 2 arguments, `ret 8` (0x246441,
  0x245E86).
- **Stream twin:** thunk 0x2464A5 -> `CDirectSoundStream` method 0x246165 (`ret 8`) -> 0x245075.
- **Call sites:** XACT 0x2FCB91 (buffer) and 0x2FCB9C (stream).

**What 0x245075 does** (proven):
- It sets `blk = [settings+0xB8]`. If `blk` is null, or the low byte of `d->flags` is 0, it only refreshes the HRTF
  when flag 0x40 is set, by calling 0x24B697(0, d) with the passed data (0x245190..0x24519B).
- Otherwise it stores `d->flags` at `blk+0` and copies each field group whose flag is set (0x24509B..0x245183). It
  then calls the voice update 0x24C1E1, which acts on each flag group and finally clears `blk->flags` (0x24C245).
- The copied fields **persist** in the block. Fields without their flag keep their previous values.

**Block layout and how the voice update uses each field:**

| Flag | Field(s) | Meaning | Proof |
|---|---|---|---|
| 0x01 | +0x04 | `l3DDistanceVolume`, mB | GetVoiceProperties returns it at props+0x48 (0x24BDCE..0x24BDD1); Calculate3D gets it from 0x244A61 -> `*0x262D04` (distance, min/max, rolloff) |
| 0x02 | +0x08 | `l3DConeVolume`, mB | props+0x4C (0x24BDDD); computed by 0x244A98 -> `*0x262D08` from the cone angles (source+0x20/+0x24) and cone outside volume (+0x34) |
| 0x04 | +0x0C, +0x10 | two mB terms (front and back split, from distance and azimuth/elevation, 0x244AB8 -> `*0x262D0C`); +0x0C goes to bins 2,6,7 and +0x10 to bins 8,9 (below) | the names "front/rear volume" are **uncertain** |
| 0x08 | +0x14, +0x18 | two mB terms from azimuth/elevation (0x244AEC -> `*0x262D10`, only when source+0x58 bit 1 is set); +0x14 goes to bins 6,7 and +0x18 to bin 2 | names **uncertain** |
| 0x10 | +0x1C, +0x20 | `lI3DL2DirectVolume` and `lI3DL2RoomVolume`, mB | props+0x54/+0x58 (0x24BDFB/0x24BE0A); I3DL2 calculator 0x24DAB0 (0x245B05) |
| 0x20 | +0x24 | `l3DDopplerPitch`, pitch units | props+0x50 (0x24BDEC); added to the pitch (below); from 0x244B0E (listener/source velocities) |
| 0x40 | +0x28, +0x2C | azimuth and elevation, float **degrees** (below) | used by the HRTF update 0x24B697 (0x24B70B..0x24B71C, 0x24B7B9) |
| 0x80 | +0x30, +0x34 | I3DL2 filter coefficients in hardware form | 0x24BAC7 packs them into `TAR_FCA`/`TAR_FCB` (0xFE820374/0xFE820378) at 0x24BB73..0x24BB97 and 0x24BBD9..0x24BBE5 |

**How the voice update turns the block into hardware writes** (all gated by `blk != 0 && settings` flag 0x01000000):

- **Volume** (flags 0x01–0x10) -> 0x24C09B -> 0x24AF4E -> `TAR_VOLA/B/C` (0xFE820360/364/368). Terms in mB, each
  clamped to [-10000, 0]:
  - `D = clamp(+4 + +8)`
  - bin 2: `clamp(+1C + +18 + +0C + D)`
  - bins 6, 7: `clamp(+1C + +14 + +0C + D)`
  - bins 8, 9: `clamp(+1C + +10 + D)`
  - bin 10 (I3DL2): `clamp(+20 + D)`
  - bins 0, 1, 3, 4, 5 and above 10: `D`

  The per-slot attenuation is `-(settings+0x30[bin] + settings+0x1C) - term`, converted ×64/100 and capped at 0xFFF
  (0x24B087..0x24B0EC). In level terms: mB = per-bin volume + volume + term.
- **Pitch** (flag 0x20) -> 0x24C133 -> 0x24B1A2:
  - `pitch = settings+0x18 + (3D on ? blk+0x24 : 0)`, plus the parent voice's pitch when the voice outputs into a
    submix buffer (`settings+0xB0`).
  - Clamped to [-32767, 0x1FFF], then `<<16` into `TAR_PITCH` (0xFE82037C).
  - Voices created with flags & 0x182000 get no pitch (0x24B1A9).
- **HRTF** (flag 0x40) -> 0x24B697: (azimuth, elevation) -> `*0x262D18` -> HRIR coefficient tables written to
  `SET_HRIR` (0xFE820400..0xFE820438) plus the ITD in `SET_HRIR_X` (0xFE82043C). The ITD sign follows
  `azimuth >= 0.0` (0x24B7B9..0x24B7D3).
- **Filter** (flag 0x80) -> 0x24BAC7 (above).

**Units, proven:**
- **Pitch:** `XAudioCalculatePitch` 0x244C0C (.XTLID 0xB00A3) computes `pitch = round(4096 * log2(freq / 48000))`
  (48000 -> 0 special case at 0x244C1B; 0x24A592 multiplies log2(freq × 1/48000) by 4096.0). The voice therefore
  plays at `48000 * 2^(pitch/4096)` Hz.
- **Azimuth and elevation:** produced by `*0x262CF8` = 0x247ED9 as degrees. It uses a piecewise approximation
  (ratio × 45, 90 − ratio × 45, 180 − x, sign flips; constants 45.0 at 0x365ECC, 90.0 at 0x365F1C, 180.0 at
  0x365EF8). Inputs are the relative position dotted with the listener's front (+0x20), top (+0x2C) and right (+0x44)
  axes. In head-relative mode the defaults are (0,0,1), (0,1,0) and (1,0,0) (0x2622F4, 0x262300, 0x24FC84).
  - Azimuth is negated when the right-axis component is negative (0x24800F..0x248020). So **positive azimuth = source
    to the listener's right**, assuming right = top × front, as the default axes imply. The x87 flag reading behind
    that conclusion was done by hand: confidence moderate. It is worth a runtime check (log the azimuth for one known
    source).
  - The runtime's own 3D code uses the same convention (`right = top × front`, `+side` -> louder right).

**What matters for a stereo software mixer:**
- Fields +4, +8, +0C, +10, +14, +18, +1C (levels) and +24 (doppler pitch) are audible.
- Azimuth (+28) is the only left/right cue. On the console, stereo directionality comes from the HRTF on bins 6–9;
  the bins themselves are fed equally. The runtime folds 6/8 to the left and 7/9 to the right equally, so without a
  pan from azimuth these voices would be centred.
- +20 (room send to bin 10) only matters if the runtime mixes I3DL2 reverb. It currently skips effect sends when
  speaker bins are present (dsound.cpp `Levels`).
- +30/+34 are filter coefficients. The runtime ignores filters today (`BUFFER(SetFilter, 2) { return 0; }`), so
  storing them and ignoring them is consistent.

**Who computes the block (proven).** XACT runs the library's own 3D calculator as guest code, then hands the result
to Set3DVoiceData:
1. `0x2470DB` (id-less thunk -> 0x246DBA) with XACT's listener (0x3038A0) and source (cue+0x44): relative position,
   distance (+0x68), azimuth/elevation (+0x74/+0x78), via `*0x262CF4`/`*0x262CF8`.
2. `0x2464AF` (id-less thunk -> 0x24588A, "Calculate3D", `ret 0x14`) with (listener 0x3038A0, source, I3DL2 listener
   0x303658, I3DL2 source cue+0xC0, out = cue+0xE8). XACT call sites: 0x2FCB07 and 0x2FCB69.
3. Set3DVoiceData(buffer or stream, cue+0xE8).

These run as recompiled code and touch only guest structs and the DSOUND critical sections (0x244969/0x24498E,
0x244B35 -> kernel imports). **There is one dependency, and it is a blocker.** The calculator calls through a
function table at 0x262CF4..0x262D18 that only `0x2485C4` fills (and sets `[0x262D1C] = 3`). 0x2485C4 is called only
from `DirectSoundUseFullHRTF` 0x244BED (.XTLID 0xB002F), which the game calls at 0x1066E8. The runtime replaces
`DirectSoundUseFullHRTF` with a no-op, so the table stays 0 and XACT's first 3D calculation calls through null.
**Fix:** let 0x244BED run its original body (it is the lock 0x244B35, the table stores, then the unlock), e.g. by
calling `GUEST_ORIG` from the HLE or by setting the symbol to 0 in the config.

## 2. Use3DVoiceData: wrapper 0x246444 -> 0x245E89 -> 0x2451A6 (id-less; the name is my label)

- **Signature:** `(IDirectSoundBuffer* buf, BOOL use)`, stdcall, 2 arguments, `ret 8` (0x24645D, 0x245ED2, 0x2451C0).
- **Effect:** 0x2451A6 sets (use != 0) or clears bit 0 of byte `[settings+0xB]`, i.e. settings flags
  `0x01000000`, and returns 0. It does not update the voice itself.
- **Who reads the bit:** the volume conversion 0x24AF6E, the pitch 0x24B1C4 (`testl $0x1000000`), the filter
  0x24BB73 and GetVoiceProperties 0x24BDC2. When it is clear, the 3D block's volume terms, doppler pitch and filter
  coefficients are ignored and GetVoiceProperties reports 0 for the 3D fields. Default: set when the block is
  allocated (flags & 0x400010, 0x247305).
- **Stream twin:** thunk 0x2464AA -> 0x2461B7 (`ret 8`) -> 0x2451A6.
- **Call sites:**
  - XACT 0x2FBDD9: `use = (cue+0x14 & 2) || (other+0x14 & 2)`, right after SetOutputBuffer 0x246A19.
  - XACT 0x2FBD99 (stream).
  - Game `.text` 0x1039B1 (buffer; `use = [esi+0x14] & 2`), 0x103980 and 0x10398D (stream).

## 3. The XTLID-named methods

The wrappers are proven and the voice-level calls are proven; identities fit the slot order and the bodies.

**IDirectSoundBuffer_SetPitch** 0x2462AC (.XTLID 0xB0076, probable name)
- `(buf, LONG pitch)`, `ret 8`. 0x245C19 -> 0x244FA3: `settings+0x18 = pitch` (0x244FAE), then the pitch update
  0x24C133.
- Units: §1, `48000 * 2^(pitch/4096)` Hz. Shares voice method 0x244FA3 with stream thunk 0x246465
  (.XTLID 0xB0099) -> 0x245ED5 (`ret 8`).
- **XACT drives pitch with these, not SetFrequency** (no `IDirectSoundBuffer_SetFrequency` is linked). Call sites:
  buffer 0x2FBFF6, stream 0x2FC001.

**IDirectSoundBuffer_SetLFO** 0x2462C8 (.XTLID 0xB006D, probable)
- `(buf, const DSLFODESC*)`, `ret 8`. 0x245C67 -> 0x244FD8 -> 0x24B806.
- 0x24B806 reads desc +0 (LFO select), +4, +8, +0xC, +0x10, +0x14 (0x24B81D..0x24B868), matching DSLFODESC
  {dwLFO, dwDelay, dwDelta, lPitchModulation, lFilterCutOffRange, lAmplitudeModulation}. It writes the voice LFO
  registers (CFG_MISC 0xFE820318, LFO_ENV 0xFE82036C and 0xFE820350).
- Stream twin: 0x24646A (.XTLID 0xB0092) -> 0x245F79 (`ret 8`).
- Call sites: XACT 0x2FC355 (buffer) and 0x2FC360 (stream), reached only from XACT's sound-bank event interpreter
  at 0x302E89.

**IDirectSoundBuffer_SetEG** 0x2462E4 (.XTLID 0xB0067)
- `(buf, const DSENVELOPEDESC*)`, `ret 8`. 0x245CB5 -> 0x244FEB -> 0x24B935.
- 0x24B935 reads desc +0..+0x24 (0x24B94E..0x24BA1C), matching DSENVELOPEDESC {dwEG, dwMode, dwDelay, dwAttack,
  dwHold, dwDecay, dwRelease, dwSustain, lPitchScale, lFilterCutOff}.
- Stream twin: the runtime already maps `IDirectSoundStream_SetEG` = 0x24646F (thunk -> 0x245FCB) to a no-op.
- Call sites: XACT 0x2FC30F (buffer) and 0x2FC31A (stream), only from the event interpreter at 0x302E19.
- **Uncertain:** whether CS's sound banks contain EG/LFO events. Log the first calls to find out.

**IDirectSoundBuffer_GetVoiceProperties** 0x24640C (.XTLID 0xB0055, probable)
- `(buf, DSVOICEPROPS* p)`, `ret 8`. 0x245DED -> 0x245062 -> 0x24BC9C.
- Returns 0x88780032 when the voice is not on hardware (bit 0 of voice+0x12 clear, 0x24BCA6..0x24BCAC); otherwise it
  fills the struct (`ret 4`, result 0):

| Offset | Field |
|---|---|
| +0x00 | mix bin count (settings+0x24) |
| +0x04 + i×8 | {dwMixBin, lVolume} for the voice's bins (volume = −(current hardware attenuation × 100/64)); unused slots are {−1, −10000} (0x24BD92..0x24BDA7), 8 pairs |
| +0x44 | lPitch, the current hardware pitch (hardware voice +0x7C >> 16) |
| +0x48 | l3DDistanceVolume |
| +0x4C | l3DConeVolume |
| +0x50 | l3DDopplerPitch |
| +0x54 | lI3DL2DirectVolume |
| +0x58 | lI3DL2RoomVolume |

  The 3D fields are 0 unless the block exists and the 0x01000000 gate is set. Struct size 0x5C.
- Stream twin: 0x2464A0 (.XTLID 0xB0081) -> 0x246113 (`ret 8`).
- Call sites: XACT 0x2FBDB1 (buffer, before SetOutputBuffer), 0x2FBD75 (stream, before stream SetOutputBuffer),
  0x2FC212 (buffer) and 0x2FC21D (stream).

**IDirectSound_CreateSoundStream** 0x247843 (.XTLID 0xB003B)
- `(IDirectSound* ds, const DSSTREAMDESC* desc, IDirectSoundStream** out, IUnknown* unk)`, `ret 0x10`.
- It adjusts `ds - 8` and calls 0x24766B, the same creation the exported `DirectSoundCreateStream` 0x247A74 ends in
  (after creating the device with 0x2479C0).
- Call site: XACT 0x2FBC1B, with the 0x400000 flag ORed in for 3D cues (§0).

## 4. Other unhandled exports seen on the same XACT path (outside the asked list, noted)

- **Stream SetOutputBuffer** thunk 0x246A71 (.XTLID 0xB0098) -> 0x246975 (`ret 8`), called by XACT 0x2FBD7E and
  game 0x10396F (with 0). The runtime handles the buffer version (`BUFFER(SetOutputBuffer, 2) { return 0; }`) but not
  this stream one. It needs the same no-op.
- **XAudio helpers** 0xB00A3 0x244C0C (CalculatePitch), 0xB00A4 0x244C5B and 0xB00A5 0x244C56: XACT calls 0x244C56
  as `(channels, 48000, 16, &wfx)` at 0x2FBBE6, so 0xB00A5 is consistent with CreatePcmFormat and 0xB00A4 with
  CreateAdpcmFormat (alphabetical order), but the names are probable only. They are pure helpers (math, format
  structs, critical section), so the runtime's "exports running their original code" warning for them is benign.

## 5. Implementation sketch (against sdk/runtime/xdk/dsound.cpp)

**New Voice fields:**
- `bool vd3d` (has a 3D voice data block: creation flags & 0x400010)
- `bool use3d` (the 0x01000000 gate; default `vd3d`)
- `int32_t d3[14]` (the block's dwords +0x04..+0x34, persistent)
- `int32_t pitch` (base pitch, units of 1/4096 octave, relative to 48 kHz)
- `bool pitch_set` (false means use the format rate)

`NewBuffer` and `DirectSoundCreateStream`: `v.vd3d = v.use3d = (flags & 0x400010) != 0`. Keep `is3d = flags & 0x10`
for the existing position-based path, and do not apply that path to 0x400000-only voices (it already doesn't).

**Handlers** (config [symbols] names to add are in brackets):

| Handler | Symbol | Args, ret | Behaviour |
|---|---|---|---|
| `BUFFER(Set3DVoiceData, 2)` | [IDirectSoundBuffer_Set3DVoiceData = 0x246428] | 2, `ret 8` | `f = R32(a[1])`. If `!v.vd3d \|\| !(f & 0xFF)`: return 0 (the console would only refresh the HRTF). Else for each group bit copy: 0x01 -> d3[0] (+4); 0x02 -> d3[1]; 0x04 -> d3[2], d3[3]; 0x08 -> d3[4], d3[5]; 0x10 -> d3[6], d3[7]; 0x20 -> d3[8]; 0x40 -> d3[9], d3[10] (raw float bits); 0x80 -> d3[11], d3[12]. Return 0. |
| `STREAM(Set3DVoiceData, 2)` | [CDirectSoundStream_Set3DVoiceData = 0x246165] | 2 | same |
| `BUFFER(Use3DVoiceData, 2)` | [IDirectSoundBuffer_Use3DVoiceData = 0x246444] | 2, `ret 8` | `v.use3d = a[1] != 0;` return 0 |
| `STREAM(Use3DVoiceData, 2)` | [CDirectSoundStream_Use3DVoiceData = 0x2461B7] | 2 | same |
| `BUFFER(SetPitch, 2)` | [IDirectSoundBuffer_SetPitch = 0x2462AC] | 2, `ret 8` | `v.pitch = int32_t(a[1]); v.pitch_set = true;` return 0 |
| `STREAM(SetPitch, 2)` | [CDirectSoundStream_SetPitch = 0x245ED5] | 2 | same |
| `BUFFER(SetLFO, 2)`, `STREAM(SetLFO, 2)` | [IDirectSoundBuffer_SetLFO = 0x2462C8], [CDirectSoundStream_SetLFO = 0x245F79] | 2 | return 0 (no LFO in the mixer; log the first calls with the descriptor) |
| `BUFFER(SetEG, 2)` | [IDirectSoundBuffer_SetEG = 0x2462E4] | 2 | return 0, like the existing stream SetEG; log the first calls |
| `BUFFER(GetVoiceProperties, 2)` | [IDirectSoundBuffer_GetVoiceProperties = 0x24640C] | 2, `ret 8` | see below |
| `STREAM(GetVoiceProperties, 2)` | [CDirectSoundStream_GetVoiceProperties = 0x246113] | 2 | same |
| `HLE_STDCALL(IDirectSound_CreateSoundStream, 4)` | [IDirectSound_CreateSoundStream = 0x247843] | 4, `ret 0x10` | same as `DirectSoundCreateStream(a[1], a[2])` |
| `STREAM(SetOutputBuffer, 2)` | [CDirectSoundStream_SetOutputBuffer = 0x246975] | 2 | return 0, as the buffer version |

GetVoiceProperties fills 0x5C bytes at `a[1]`:
- +0: `v.nbins` (or 2);
- +4 + i×8: `{bin, bin_mb + volume}` for each used bin, and `{0xFFFFFFFF, -10000}` for unused slots up to 8;
- +0x44: effective pitch;
- +0x48/+0x4C/+0x50/+0x54/+0x58: `use3d && vd3d ? d3[0], d3[1], d3[8], d3[6], d3[7] : 0`.

It returns 0. The console returns 0x88780032 when the voice is not on hardware (not playing); match that only if a
caller turns out to depend on it.

**Mixing:**
- **Rate.** Replace the rate step with: `p = (v.pitch_set ? v.pitch : round(4096*log2(v.rate/48000.0))) +
  (v.use3d && v.vd3d ? d3[8] : 0)`, clamped to [-32767, 8191]; `freq = 48000 * 2^(p/4096)`.
  - The existing `SetFrequency(f)` should set `pitch = XAudioCalculatePitch(f)` (f == 0 means the format rate),
    which keeps a single model.
  - `SetFormat` resets `pitch_set` (the runtime already resets `freq`).
- **Levels.** In `Levels`, when `v.use3d && v.vd3d`, multiply each bin's gain by `Db(term(bin))`, using the bin
  formula in §1 (terms clamped to [-10000, 0]).
- **Pan.** Then apply a pan from azimuth (d3[9] as float degrees, positive = right): `pan = sin(az × π/180)`, with the
  same law as the existing 3D code (`L *= min(1, 1 − pan)`, `R *= min(1, 1 + pan)`). This is an approximation: the
  console uses the HRTF (0x24B697) on the 3D bins. A higher-fidelity option is to call the guest HRTF provider
  (`*0x262D18` via 0x24AC79 with azimuth and elevation) for the HRIR coefficient and ITD tables and convolve.
- Filter terms d3[11..12] and the room send d3[7] stay unmodelled, consistent with the runtime's current SetFilter
  and effect-send handling.

**Required alongside, or XACT's 3D math calls null:** let `DirectSoundUseFullHRTF` 0x244BED run its original body
(GUEST_ORIG from the HLE, or set the symbol to 0) so that 0x2485C4 fills the calculator table 0x262CF4..0x262D18.

**Runtime audit list:** add the new symbols to `AuditExports` `handled[]`.

## Call sites into these from XACTENG / .text (complete for the listed targets; from flat.dis)

| Target | Call sites |
|---|---|
| 0x246428 Set3DVoiceData (buffer) | 0x2FCB91 |
| 0x2464A5 Set3DVoiceData (stream) | 0x2FCB9C |
| 0x246444 Use3DVoiceData (buffer) | 0x1039B1, 0x2FBDD9 |
| 0x2464AA Use3DVoiceData (stream) | 0x103980, 0x10398D, 0x2FBD99 |
| 0x2464AF Calculate3D | 0x2FCB07, 0x2FCB69 |
| 0x2470DB listener/source relative | 0x2FCAEA, 0x2FCB4C |
| 0x2462AC SetPitch | 0x2FBFF6 |
| 0x246465 stream SetPitch | 0x2FC001 |
| 0x2462C8 SetLFO | 0x2FC355 |
| 0x24646A stream SetLFO | 0x2FC360 |
| 0x2462E4 SetEG | 0x2FC30F |
| 0x24646F stream SetEG (handled) | 0x2FC31A |
| 0x24640C GetVoiceProperties | 0x2FBDB1, 0x2FC212 |
| 0x2464A0 stream GetVoiceProperties | 0x2FBD75, 0x2FC21D |
| 0x247843 CreateSoundStream | 0x2FBC1B |
| 0x246A71 stream SetOutputBuffer | 0x10396F, 0x2FBD7E |
| 0x244BED DirectSoundUseFullHRTF | 0x1066E8 |
