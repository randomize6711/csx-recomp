# CS:Xbox engine server and net protocol: what changes for 33 / 65 slots

Static read-only analysis of assets/default.xbe (build/ghidra/flat.dis). All VAs are guest VAs.
Status labels: **P** = I read it in the code here. **I** = inferred from HL1/GoldSrc source structure. Function names are mine, picked from strings and behaviour.

## 0. Key globals and helper functions (P)
| what | VA |
|---|---|
| svs.clients ptr / svs.maxclients / svs.maxclientslimit | 0x636CE4 / 0x636CE8 / 0x636CEC |
| sv.num_edicts / sv.max_edicts / sv.edicts / sv.baselines | 0x62B418 / 0x62B41C / 0x62B420 / 0x62B424 |
| SV_UPDATE_BACKUP / mask (8 for SP, 16 for MP) | 0x379970 / 0x379974, set at a1a1a-a1a27 |
| host_client | 0x6CDFEC |
| malloc / free (CRT) | 0x22D86A / 0x22C609 |
| server-lifetime block list (X_AddBlockToListTail 0xC86F0, list 0x552324) | stands in for the Hunk |
| SZ_GetSpace / MSG_WriteBits(val=eax,bits=ecx) / MSG_ReadBits(bits=eax) | 0x3ECE0 / 0x3E220 / 0x3E560 |
| Sys_Error (fatal, 334 callers) / Con_Printf | 0xAC090 / 0x40790 |

## 1. client_t storage and static per-client arrays

| item | evidence | now | 33 / 65 | difficulty |
|---|---|---|---|---|
| Clamp on -maxplayers | SV_SetMaxclients a1960: `cmp $0x11` at a19f3/a19f8 | 1..17 | change the clamp | trivial |
| svs.maxclientslimit | `movl $0x11,0x636cec` a1a10 | 17 | 33/65 | trivial |
| svs.clients block | `push $0x55120` a1a0b → malloc; header `$0x55110` a1a40; `rep stos` of 0x15444 dwords a1a4f (= 17*0x5010) | **fixed size**, sized for 17 at compile time (it is not `limit*sizeof`) | allocate limit*0x5010+0x10: 0xA5220 for 33, 0x145420 for 65 | easy (3 constants) |
| client_t layout | stride 0x5010; init loop a1a80 (stride 0x5010, bounded by 0x636cec) | — | unchanged | — |
| Per-client frames | 951e0: malloc(UPDATE_BACKUP*0x828) per client, ptr at client+0x3598; loop bounded by maxclientslimit | scales on its own | — | none |
| Hard-coded 17-client loops (iterate svs.clients up to 17, not to maxclients) | **99126** (SV_CheckForDuplicateNames: "(%d)%-0.*s", loop 99060-991bd, also `cmp $0x11` at 99103); **9e84f / 9e8b1** (SV_ExtractFromUserinfo name loop, client+0x4d10 = name); **abe8e** `cmp $0x55110,%ebp` (SV_ParseVoiceData broadcast loop) | 17 | change to maxclients, or above 17 these slots are ignored (names not deduped, no voice relayed) | easy |
| Edict→client lookup with hard-coded 17 | 791c5 (`cmp $0x11,%esi; jg` → client userinfo at client+0x4bc8, i.e. GetInfoKeyBuffer) | 17 | players 18+ get "" as userinfo, which breaks the game DLL's infokey reads | easy |
| **STATIC lag-comp array** `truepositions[17]` | base 0x4E46C0, stride **0x58**, 17 entries (0x5D8 bytes, to 0x4E4C98); cleared by `rep stos 0x176` at aaadc and ab16e, `movl $0x5d8` at ab13d; read at aa9db (SV_GetTrueOrigin), aaa5b (SV_GetTrueMinMax); indexed 1-based via 0x4E4668 at ab07f and ab266 | 17 entries, but the loops in SV_SetupMove aaac0-ab399 (unrolled x4: +0x14040/+0x160 at aae3d) run to **svs.maxclients** | above 17 it **overflows** into 0x4E4C98+ (0x4E4C98 is the Sys_FloatTime state used at ac2b2) and corrupts memory | medium: relocate it to a bigger BSS block or malloc, then patch about 10 base or size immediates |
| **STATIC lastping[17] (u16)** | 0x4E0448 (`movw %ax,0x4e0448(,%esi,2)` 9da86, read 9daa2); next var 0x4E0470 | ≤20 slots | overflow above 20 | easy relocation |
| **STATIC lastloss[17] (u8)** | 0x4E449C (9da9c/9dab0); next var 0x4E44B0 | ≤20 | overflow above 20 | easy |
|  (both above live in SV_GetNetInfo 9da40, called by SV_EmitPings 9dbe0, which loops to maxclients) | | | | |
| Voice receive bitvec in client_t (m_VoiceStreams) | client+0x5008 dword[idx>>5] (Voice_GetClientListening ac3b0, Voice_SetClientListening ac410; tested at abdf8); bounded only by maxclients | 1 dword plus 4 bytes of 8-byte struct padding (0x500C-0x5010): 64 bits usable | OK up to 64. **Slot 65 writes 0x5010, which is the next client's `active` field** | medium (grow client_t to 0x5018, which changes the stride in about 180 `0x5010` immediates, or move the bitvec out) |
| ED_Alloc start | 7b3b0: `movl 0x636ce8,%edi; incl %edi` | maxclients+1 | scales | none |
| Player edicts reserved | SV_SpawnServer a0321: num_edicts = maxclients+1; a03c0 loop sets client+0x4B9C = &edicts[i+1] | scales | none | none |
| Player-vs-entity delta choice | 9d8d2 / 9d322 / 9d38f: entnum 1..svs.maxclients → player delta 0x551F08, else 0x551F0C | scales | none | none |
| SV_WriteEntitiesToClient player test | 9ddb8 `cmpl 0x636ce8,%ebp` | scales | none | none |
| "Server is full" | 99470 loops svs.maxclients | scales | none | none |
| maxplayers cmd | 6d4bf clamps to 0x636CEC | scales once the limit is raised | none | none |
| EV_SV_Playback client index | 78e09 checks against 0x636CE8 | scales | none | none |

Not found (P, by searching): `bt*` instructions in engine code (<0xE0000): none. The `1<<n` sites in the engine are all PVS/leaf bitmasks (96280 SV_ValidClientMulticast, 9dac0 leaf check with MAX_ENT_LEAFS 0x30) or unrelated, apart from the voice bitvec above. I found no 32-bit client mask in multicast, sound or events. The full scan of 0x636CE4/0x6CDFEC functions for indexed BSS arrays turned up only the 3 static arrays above, plus the ip/userid filters (0x6391C0, 0x639C00) and the precache tables, which are not per-player.

## 2. Bitmasks over players
| mask | evidence | width | limit |
|---|---|---|---|
| client_t voice stream bits | ac3b0 / ac410 / abdf8 (`shl %cl` with idx&31, dword idx>>5 at +0x5008) | 32 per dword; 2 dwords fit before 0x5010 | 64 OK, 65 overflows into the next client_t |
| Game-DLL CVoiceGameMgr VoiceMask (0x34331C) | 102db0 / 102e30 (`cmp $0x11`), game DLL | — | outside my area (game side), flagged for that agent |
No other per-player masks in the engine server (P, within my scan).

## 3. Wire protocol (svc table at 0x371CF8: {id, name, handler}, ids 0..0x37, read from .data)
| message | server writer | encoding of the player/entity index | 33 | 65 | notes |
|---|---|---|---|---|---|
| svc_serverinfo (11) | SV_SendServerinfo 97060: maxclients byte at 971ed-97205, playernum byte (bl) at 97212 | 8-bit | ok | ok | **The client rejects >17**: CL_ParseServerInfo 2e8e1 `cmp $0x11; jg` → "Bad maxclients (%u) from server" (0x32C554) |
| svc_updateuserinfo (13) | byte slot (I: HL1) | 8-bit | ok | ok | client: 2ca2c slot `<0x11` else "svc_updateuserinfo > MAX_CLIENTS" |
| **svc_pings (17)** | SV_EmitPings 9dbe0: 1 bit, **slot 5 bits** (9dc42), ping 12, loss 7; loop to maxclients | **5-bit** | **breaks (slot 32)** | **breaks** | client 319a0 reads 5 bits and indexes cl.players stride 0x24C (0x779318) with no bounds check. Needs 6/7 bits on both ends, which is a protocol change |
| svc_packetentities 0x28 / svc_deltapacketentities 0x29 | SV_EmitPacketEntities 9d3f0: count u16 (9d48e), delta seq byte (9d463); entity header 9d1b0 and 9d923-9d968: delta ≤63 → 6 bits, else **11-bit absolute entnum**; end marker 16 zero bits (9d98a) | 11-bit (max 2048 edicts) | ok | ok (1760 edicts) | above ~84 slots max_edicts >2048 would break this. Player-vs-entity delta keyed on maxclients (P) |
| MAX_PACKET_ENTITIES | 9dded `cmp $0x100,%edi` (stack buf 0xFB2C = 256*0xFB); client "newindex == MAX_PACKET_ENTITIES" 0x32AED8 | 256 | ok | tight: 64 visible players + weapons/C4/hostages/grenades share 256; excess entities are silently dropped (not fatal) | |
| svc_sound (6) | SV_StartSound 95e10: flags 9, vol 8, atten 8, channel 3, **entity 11**, sound idx 8/16, pitch 8 (95fa6-9602f) | 11-bit | ok | ok | |
| svc_event | EV playback 789d0; SV_EmitEvents 9cc20 count capped 0x1F (9cce2) = 5 bits per update; per-client queue 64 (0x40 at 78d5a), event_info stride 0x58 | count ≤31/update | ok | more drops under load | not per-player indexed |
| svc_voicedata (0x35) | abe52: byte sender index, u16 len | 8-bit | ok | ok | the relay loop is hard-coded 17 (abe8e) |
| svc_clientdata / svc_spawnbaseline / svc_time | not player-count indexed (I: HL1 delta-based) | — | ok | ok | |
| SVC_PlayerInfo (connectionless, 'D'=0x44) | 9afb0: 0x800-byte buffer with FSB_ALLOWOVERFLOW (9afe3) | — | ~1.4 KB ok | ~2.7 KB **overflows the 2 KB buffer** (reply truncated or dropped, not a crash) | server browser / system link |
| SVC_Info / InfoString | 9aab7 / 9a4b5: 1400-byte buffers, maxplayers as byte | 8-bit | ok | ok | |

System link with an original console: an unmodified Xbox client refuses any server with maxclients >17 (2e8e1). The 5-bit svc_pings slot is another wire change. So a 33/65 server works only with patched clients. The recomp keeps full compatibility only with ≤17 slots (or ≤32 with only the maxclients checks patched, since the 5-bit pings work up to 32). Delta descriptions (delta.lst) are inside the SXWad packs and I did not inspect them. In HL1 the player delta has no slot-indexed field (I).

## 4. Per-frame limits that grow with players
| limit | VA | value | risk at 65 |
|---|---|---|---|
| Packet entities per client frame | 9dded | 256 | tight, silent drops |
| Per-server-frame unique entity states (shared pool dedup table 0x6381A0, count 0x551F30, reset per frame in SV_SendClientMessages 9e409) | 9562a `cmp $0x400` → "Too many entities fort one frame!!" via Sys_Error 0xAC090 | 1024 distinct entity numbers across all clients per frame | **fatal** if the union of all clients' visible entities passes 1024. Unlikely in CS (maps have a few hundred ents), but more players means more dropped weapons |
| Shared entity_state pool | 95300-95660: pages 0x10000 B (256 states x 0x100) in 0x379200[], capacity 0x3791F8 grows by 0x100 per page, warning at 0x10000 states (9557f) | 65536 states (16 MB max) | refcounted; ≈ distinct states × backups, fine |
| physents / moveents (SV_AddLinksToPM a8d90) | a901e `cmp $0x258`, a8fea `cmp $0x40` | 600 / 64 | ok (players are physents) |
| sv.datagram / sv.reliable_datagram | a0303-a030d | 4000 / 4000 | more multicast warnings (non-fatal) |
| sv.multicast / sv.spectator / sv.signon | a0312-a039f | 1024 / 1024 / 32768 | ok |
| client datagram | client+0x25C0, maxsize 4000 (99cc5) | 4000 | ok |
| netchan.message | 69717 | 3990, fragmented | ok |
| SV_New_f msg buffer (full userinfo of all clients, fragmented) | 98200 stack 0x101D8 | 64 KB | ok for 65 |
| SVC_PlayerInfo | 9afe3 | 2048 | overflow at ~50+ named players |

## 5. Edict budget and memory
- **max_edicts = 0x311 + 15*maxclients** (P, a0221-a0229; SP forces sv_clienttrace=1 at a0205). 17 → 1040, 33 → 1280, 65 → 1760. The 11-bit wire entnum is the ceiling (2048), reached at about 84 slots.
- Edicts malloc(max_edicts*0x318+0x10) a026f; baselines malloc(max_edicts*0xFB+0x10) a02b9; moved_edict/moved_from arrays max_edicts*4 / *12 (95140). All of them scale.
- No Hunk: "-heapsize" (ad326) is vestigial (0x3000000 default, then 0x6CE0D4 is reset to 0x400). Engine data is CRT malloc tracked on X_ block lists (0x55231C/24/2C/34). How full the heap gets cannot be read off statically.
- Per client: client_t 0x5010 (20,496 B), frames 16*0x828 = 0x8280 (33,408 B), plus 15 extra edicts (15*(0x318+0xFB) = 15,645 B), about 69.5 KB in all. Extra over 17: +16 clients ≈ 1.1 MB, +48 clients ≈ 3.3 MB (the original Xbox has 64 MB; on PC this is negligible unless the guest heap is capped).
- Correct svs.clients sizes: 33*0x5010+0x10 = 0xA5220; 65*0x5010+0x10 = 0x145420.

## 6. Change list (engine server + protocol), easiest first
1. a19f3/a19f8 clamp, a1a10 limit, a1a0b/a1a40/a1a4f alloc size. (Proven to be the only svs.clients sizing.)
2. Hard-coded 17 loops: 99103/99126, 9e84f/9e8b1, abe8e (0x55110), 791c5 → use 0x636CE8.
3. Relocate static arrays truepositions (0x4E46C0, 0x58*N), lastping (0x4E0448), lastloss (0x4E449C).
4. Voice bitvec: fine up to 64; 65 needs client_t to grow or a separate array.
5. Protocol: svc_pings slot width (9dc42 writer, 319a0 reader) for >32. Client accepts maxclients>17 (2e8e1) plus the other client 17-caps (2ca2c, 2e02a, 38da6, 25fe5/26276 loops, cl.players stride 0x24C at 0x779xxx; cl.frames playerstate at 0x74A1BD stride 0xFB per frame 0x1B60, 8f0bf). Those are client-side: I located them but did not analyse them.
6. Optional: SVC_PlayerInfo 2 KB buffer (9afe3); the 1024 per-frame unique-entity Sys_Error (9562a) as a safety margin.
