# CS:Xbox engine client and client HUD: what is sized by the player count

Scope: the engine client (cl_*, prediction, studio) plus the client DLL HUD/VGUI code, all in the one XBE.
Evidence comes from build/ghidra/flat.dis. Reference counts come from a scan of every immediate or displacement
>= 0x300000 in the listing. Instruction encodings that the disassembler reads
as huge immediates add a few false hits, so the counts are approximate.

Legend: **P** = proved from code. **I** = inferred from HL1 SDK/engine source.

## 0. Constants in this build
| item | value | evidence |
|---|---|---|
| MAX_CLIENTS (engine) | 17 | svc_updateuserinfo `cmp $0x11` at 0x2ca2c, "Bogus player index" at 0x2e02a, prediction loops at 0x25fe5 and 0x26276 (P) |
| MAX_PLAYERS (client) | 17. Arrays are [MAX_PLAYERS+1]=18 and indexed 1..17 | memset sizes at 0x1b926d (P) |
| CL_UPDATE_BACKUP | 16. It is a variable at 0x371cf0 (initial value 16, never written) used in `idivl`, but cl.frames[] is a static array of 16 | (P) |
| client_state_t `cl` | 0x749d40 .. 0x780dc8 (size 0x37088). CL_ClearState does `rep stosl` 0xdc22 dwords at 0x28e37 | (P) |
| cl.maxclients | 0x778a5c (48 refs) | (P) |
| cl.playernum | 0x771224 (41 refs) | (P) |
| cl.parsecount / parsecountmod | 0x74a060 / 0x74a064 | (P) |

## 1. Static per-player arrays

### 1a. Engine client
| item | base VA | stride x count | container | code refs | grow-in-place shifts... | notes |
|---|---|---|---|---|---|---|
| **cl.frames[16].playerstate[17]** (packed entity_state_t, 0xFB = 251 bytes) | frame0 = 0x74a2a0. playerstate[0] = frame+0x18 (0x74a2b8) | 0xFB x 17 = 0x10AB, inside frame_t (stride **0x1B60**) | frame_t, which sits inside `cl` | 35 `imul $0xfb`, 51 `imul $0x1b60`, about 82 constant refs into 0x74a2a0..0x7658a0 | the frame_t tail (time +0x10c8, clientdata +0x10d0 = 0x74b370, weapondata +0x12a0 = 0x74b540, +0x1b20 = 0x74bdc0 ...) and the frame_t stride. 16 frames grow, so every later part of `cl` moves: commands 0x7658a0 (stride 0x4c), predicted/local frames 0x765d60 (stride 0xb4c x 16), playernum 0x771224 ... cl.players ... end 0x780dc8 | P. Sites: 0x23fa4 `leal 0x18(%eax,%esi)`, 0x25f5e/0x25f6c, 0x8f10f `0x74a1bd(%edx,%ecx)` (that is ps[idx-1]). 33 players: +16x0xFB = 0xFB0 per frame, stride about 0x2B10. 65 players: +48x0xFB = 0x2F10, stride about 0x4A70. **Hardest item.** |
| **cl.players[17]** player_info_t | 0x7791f0 (userid +0, userinfo +4, name +0x104, ... +0x194, +0x234 0x779424, hashedcdkey +0x238) | 0x24C x 17 = 0x270C, ending 0x77b8fc | `cl` (offset 0x2f4b0). Followed by instanced_baseline-like entity_state[] at 0x77b8fc (stride 0xfb, <0x100) and the rest of `cl` | about 60 refs in about 22 functions. 32 `imul $0x24c`. End-pointer loops: 0x1db1d `cmp $0x77ba00`, 0x28e21 `cmp $0x77ba90` | about 73 refs after the array (0x77b8fc..0x780dc8) | P |
| **predicted_players[17]** {movetype,solid,usehull,active,origin,angles} | 0x3ae3d0 (.bss, standalone) | 0x28 x 17 = 0x2A8, ending 0x3ae678 (next var 0x3ae680) | standalone | 2 base sites: 0x25eec (`$0x3ae3dc`), 0x26054 (`$0x3ae3d8`). Loops `cmp $0x11` at 0x25fe5 and 0x26276 | nothing else, if moved out | P. Easy. |
| pmove physents for players | written via 0x552c34->+0x244, stride 0xd4, counter +0x240 | <=17 added per frame (loop 0x26060..0x26276) | pmove struct (MAX_PHYSENTS) | 1 function | n/a | P for the loop. MAX_PHYSENTS=600 is I. |
| **DM_PlayerState[17]** {name[260], modelname[260], model*} (engine r_studio player model cache) | 0x6a38a0 | 0x20C x 17 = 0x22CC | standalone .bss (0x6a5b80 after it is bone lighting, unrelated) | 7 `imul $0x20c`, about 14 refs (0x8f8bb..0x8fbfb) | nothing, if moved | P layout, I name. Easy. |
| cl_entities | pointer 0x551890, malloc(cl.max_edicts(0x749d40) x **0x74C**) at 0x2e31f | dynamic | n/a | 48 `imul $0x74c` | none | P. **Not static**: players use ents 1..maxclients. The 0x74C loop at 0x25fe5 walks cl_entities[1..17]. Fine as long as max_edicts >= maxclients+1. |

### 1b. Client DLL HUD / VGUI
| item | base VA | stride x count | container | refs | notes |
|---|---|---|---|---|---|
| **g_PlayerExtraInfo[18]** (frags +0, deaths +2, origin +0x10, radarflash +0x1c.., +0x28, teamnumber +0x2a, teamname[16] +0x2c, dead +0x3c, has_c4/vip +8/+0xc, talking +0x48) | 0x57e978 | 0x4C x 18 = 0x558 (memset 0x156 dwords at 0x1b926d) | standalone .bss, followed by 0x57eef0 (other HUD data) | about 97 refs / 43 functions. 61 `imul $0x4c` image-wide (some unrelated). End-pointer loops: 0x1d7ab9 (0x57eec0), 0x1d84a9 (0x57eec4), 0x1a73f5 (0x57eec8), 0x1b85cd (0x57eeec) | P. Writers: ScoreInfo 0x1b57a0, TeamInfo 0x1b59d0, ScoreAttrib 0x1d5a50, **Radar 0x1d5980 (no index bound check at all)**, voice 0x18b8f0, 0xef1bc |
| **g_PlayerInfoList[18]** hud_player_info_t (name* +0, ping, pl, thisplayer +0x1a.., model, colors) | 0x5487b0 | 0x14 x 18 = 0x168, ending 0x548918 | standalone | 26 refs (`0x5487b0(,%r*5,4)` style) + end-pointers 0x548904/0x548908/0x54890a/0x548918 | P. Filled by GetPlayerInfo loop 0x1d5a10 (i=1..17) |
| g_IsSpectator[] (int) | 0x548750 | 4 x (18..24 slots of room, up to 0x5487b0) | standalone | 2 (Spectator msg 0x1b5ae2/0x1b5afb, bound 17 at 0x1b5acc) | P addr, I name |
| per-player 16-byte record (XUID/Live id?) + byte flag[] | 0x548918 (x16, 17 entries, ending at 0x548a28) and flag bytes 0x548d40[idx] | 0x10 x 17 | standalone | 4 + 1 (0x1b92a0, 0x1a8c35) | P layout, I meaning (Xbox Live) |
| g_TeamInfo[18] | 0x548a28 | 0x2C x 18 (memset 0xc6 dwords) | standalone | 27 | indexed by team, not player. Must hold a team-member count >17 (short fields) |
| **ScorePanel members** m_iSortedRows[27] +0x1a0, m_iIsATeam[27] +0x20c, m_bHasBeenSorted[17] +0x278, then +0x28c/+0x290 | heap object (gViewPort 0x552b7c -> +0xa5c) | NUM_ROWS = 27 = 17 + 2x5 teams | ScorePanel class | about 15 sites (0x1b9534..0x1ba4ca) | P offsets, I meaning. Growing NUM_ROWS shifts the fields after +0x1a0 inside the object (and its `new` size) |
| **CVoiceStatus g_VoiceStatus** (static at 0x56d6e8): ptr[17] at +0x7c68 (0x575350), float talk-time[] at +0x9544 (0x576c2c..), bitvecs +0x24/+0x28/+0x2c/+0x30 | 0x56d6e8 | 4 x 17 and 4 x 17 | embedded in the CVoiceStatus object, which runs up to about 0x576c78 (hostage array follows) | 3 for +0x7c68, 1 for 0x576c28, plus loops 0x1b839a (`cmp $0x11`), 0x1b86b3 | P. Growing it shifts +0x7cb0, 0x7cc8, 0x7ccc (ban mgr), 0x94d4 labels, 0x9540. Talk-time is written by 0xef1a7..0xef1bc (bound 17) |
| radar/spectator "seen" flag byte[32] (idx 1..32) | 0x562564..0x562583 (base 0x562563+idx) | 1 x 32 | standalone | 9 | P. **Already sized 32** (bound `cmp $0x20` at 0x1d37da) |
| Death notices rgDeathNoticeList | 0x548f50 | 0x64 x 4 (MAX_DEATHNOTICES=4, 0x1d6d48) | standalone | about 12 | not per-player. Killer/victim bytes index g_PlayerInfoList **unchecked** (0x1d6d7c, 0x1d6e05) |
| Studio (client) per-player arrays | none found | | | | P (no static arrays bounded by 17 in 0x1cc000..0x1d6000. The client uses IEngineStudio GetPlayerState plus g_PlayerExtraInfo). Engine-side gait data lives in player_info_t (I) |

## 2. Bitmasks over players
| item | width | evidence | 33/65 |
|---|---|---|---|
| VoiceMask msg (client 0x1b8c80) | reads exactly 2 x 32-bit (audible mask -> +0x28, server-banned mask -> +0x30). VOICE_MAX_PLAYERS_DW = 1 | P | needs 2 or 3 dwords per mask, on both server and client |
| CVoiceStatus::UpdateServerState (0x1b8850) | loop i<32 (`cmp $0x20` at 0x1b89e9), `1<<i` into one 32-bit int, sends "vban %x" (1 hex dword) | P | needs multi-dword "vban" plus the server parser |
| CPlayerBitVec indexing | `sar $5` / `and $0x1f` (0x1b89c0, 0x1b8b66, 0x1b86d9): already written as CBitVec, only 1 dword of storage | P | storage grows to 2 or 3 dwords and shifts the CVoiceStatus fields after it |
| server CVoiceGameMgr::Init clamps maxClients to 17 | 0x102db0 `cmp $0x11` / `mov $0x11` | P (server side, for reference) | |
| svc_pings slot field | **5 bits** (0x31a00: readbits 5 = slot, 12 = ping, 7 = loss). **No bound check vs 17**: slots 17..31 write past cl.players | P | 33 needs 6 bits, 65 needs 7 bits (protocol change on both sides) |

## 3. Scoreboard / HUD layout
- The scoreboard is the VGUI ScorePanel (gViewPort 0x552b7c +0xa5c). The Xbox build fills rows into a list control: each row is pushed into a std::vector of 0x20-byte items at panel->+0xbc->+0x5c via 0xde610/0xddc10 in the row loop 0x1b9820, which runs 0..m_iRows (+0x19c). So the draw path has **no fixed row array**. Whether rows scroll or clip depends on the list widget (0xdd520 family). That widget is not analysed (the row height/visible count is probably in the UI layout data, not in code).
- Fixed limits: m_iSortedRows / m_iIsATeam = 27 entries (17 players + 2x5 teams), m_bHasBeenSorted = 17. With 33 or 65 players, sorting writes past them, so NUM_ROWS must become 43 or 75 (P offsets, I formula).
- Team loops use m_iNumTeams (+0x190), and the team header loop ends at 0x548d26 (team array).
- Spectator: next/prev player wraps at 17 (0x1a9089: >17 becomes 1, <1 becomes 17). The find-by-name loop (0x1a9260) and the spectator menu loop (0x1b3f2a) are `i=1; i<17`, so they **skip slot 17** (an existing off-by-one).
- UI front end (SuperLauncher server options, 0xe3de0/0xe3e5f) has a 17-entry enable table at obj+0xa1 for the max-players option (P).
- Unknown 32-entry tables (0x1ab8d0, stride 0x14c) and 0x1b5ff5 (`<0x20`) look non-player (I).

## 4. maxclients check from svc_serverinfo
- **P:** svc_serverinfo at 0x2e8d3..0x2e8e4: `movzbl` into cl.maxclients (0x778a5c), then `cmp $1; jl` and `cmp $0x11; jg` jump to 0x2eed8, which prints "Bad maxclients (%u) from server\n" (0x32c554) and bails out. The field is 1 byte, so it can carry up to 255.
- Other hard 17 checks on the engine client: svc_updateuserinfo 0x2ca2c ("> MAX_CLIENTS" 0x32c9b0, followed by Host_Error-like 0x5bc40). Customization 0x2e02a ("Bogus player index" 0x32c700). Event/player lookup 0x38da6 (`ent-1 < 17` -> cl.players[].+0x234). The prediction loops 0x25fe5 and 0x26276. End-pointer loops 0x1db1d and 0x28e21.
- 0x8f0bf, 0x1d13de, 0x1d0553 compare renderfx (0x22a) against kRenderFxDeadPlayer=17 (**unrelated**). They then bound renderamt (player#) by cl.maxclients or GetMaxClients.
- 0xe88c4 and 0xe8a30 are voice-codec switch tables (unrelated).

## 5. What it takes / difficulty
| item | 33 | 65 | difficulty |
|---|---|---|---|
| frame_t.playerstate | stride 0x1B60 -> about 0x2B10. Every frame-relative offset after +0x18 changes, cl grows by 0xFB00 | about 0x4A70, cl grows by 0x2F100 | **Hard.** About 51 + 35 + 82 + 27 sites, plus all ~440 refs to later `cl` fields if done in place. Better: move playerstate out to a parallel array `ps[16][65]` and rewrite only the about 35 `imul $0xfb` + about 10 base sites |
| cl.players | relocate to new BSS (60 refs / 22 functions) or grow in place (shifts about 73 refs) | same | Medium |
| predicted_players, DM_PlayerState | relocate (about 3 and about 14 sites) | same | Easy |
| g_PlayerExtraInfo / g_PlayerInfoList / g_IsSpectator / 0x548918 | relocate the 4 standalone arrays (about 97 + 26 + 2 + 5 refs) and change the memset sizes and end pointers | same | Medium (many sites, mechanical) |
| ScorePanel / CVoiceStatus embedded arrays | object layouts grow, so later member offsets shift (about 15 and about 30 sites) | same | Medium |
| voice masks, vban, svc_pings 5-bit slot | 2 dwords, 6 bits | 3 dwords, 7 bits | Medium (protocol, both sides) |
| hard `cmp $0x11` bounds (about 15 client sites + about 8 engine sites) | change the immediates | same | Easy |
| Bad maxclients check | 0x2e8e1 imm | same | Trivial |
| scoreboard visible rows | list widget, not proven | | Unknown |
