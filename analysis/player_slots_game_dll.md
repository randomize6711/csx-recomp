# CS:Xbox game DLL: what has to change for 33 or 65 player slots

Read-only static analysis of assets/default.xbe through build/ghidra/flat.dis. Nothing was built or run.

## Where the game DLL is (from string xrefs)

| Range | Contents | Evidence |
|---|---|---|
| 0x012000-~0x0bf000 | engine | Host_Error 0x5bdc6, maxplayers cmd 0x6d4xx, "Server is full" 0x994a9 |
| ~0x0bf000-0x102000 | XBUI front end and in-game UI (Live, menus) | SuperLauncher_*, playeroptions_SwitchTeam 0xfe793 |
| 0x102000-0x102c00, 0x103400-0x116000 | engine-side voice, XACT, vgui, FS | "Voice - chan", XACT, FS: strings |
| **0x102c80-0x103370** | **game DLL CVoiceGameMgr** | "CVoiceGameMgr::ClientCommand" 0x102fe0 |
| **0x116000-~0x18b300** | **game DLL** (LinkUserMessages 0x116aa0, player.cpp ~0x117000, gamerules 0x13b000-0x150000, triggers/soundent ~0x150000-0x16f000, bot manager 0x160000-0x163400, CCSBot 0x168000-0x18b300) | message names, #Hint_, bot_* cvars, "I fell off!" 0x18b086 |
| 0x18b300-0x1e0000 | client DLL / HUD (cam_command 0x18ba78, ScoreInfo hook 0x1a6802) | |

Key globals (proved): gpGlobals = *(0x527ce8), maxClients = gpGlobals+0x90. g_pGameRules = *(0x552adc). TheBots = *(0x552a8c). Engine functions are called through 0x527axx/0x527bxx: MESSAGE_BEGIN *0x527b58, END 5c, WRITE_BYTE 60, CHAR 64, SHORT 68, LONG 6c, COORD 74, STRING 78; ENTINDEX *0x527bbc, INDEXENT *0x527bc0, PvAllocEntPrivateData *0x527b9c, REG_USER_MSG *0x527bcc. operator new = 0x22d86a.

None of the game DLL's `cmp $0x11` sites is a player bound except the voice manager (0x102db0/0x102e84/0x102eaf). The others are weapon IDs (USP=16 / GLOCK=17: 0x137616, 0x1384de, 0x138e06, 0x139590, 0x13a162, 0x171b56, 0x178ca3, 0x178d90, 0x18de1c) or switch tables (0x14c358, 0x14c5e0, 0x14ca8e). The `$0x20` sites are ammo slots (m_rgAmmo +0x620, 0x121af9/0x121c6a/0x121ceb), weapon_data (0x14301e, 32 x 0x44), character tests, sound pitch arguments, and the bot's 64-entry list check (0x179a6e). None of them is a player bound.

---

## PROVED from code

### 1. Arrays and loops with a constant player bound

| # | Item | VA evidence | Size now | For 33 / 65 | Difficulty |
|---|---|---|---|---|---|
| A | **CVoiceGameMgr** VOICE_MAX_PLAYERS = **17**, VOICE_MAX_PLAYERS_DW = **1** | Init clamps m_nMaxPlayers (this+0x14) to 17 (0x102db0-0x102dc2). DW loops `cmp $1` at 0x103283 and 0x1032a2. Globals: g_PlayerModEnable 0x561fcc (1 dword), g_SentBanMasks 0x561fd0[17], g_SentGameRulesMasks 0x562018[17], g_BanMasks 0x562060[17], g_bWantModEnable 0x5620a4 (1 dword); each array is 0x44 bytes, padded to 0x48 | 17 players, 32-bit masks | **ClientConnected 0x102e30 (called from 0x1487aa) has no bounds check.** It takes idx = ENTINDEX-1, sets bit idx in g_bWantModEnable[idx>>5], and clears 17 bits in g_SentGameRulesMasks[idx] and g_SentBanMasks[idx]. When idx >= 17 it writes out of bounds into the next array, and when idx >= 32 it writes past 0x5620a8 into unknown globals. The rest is bounds-checked against m_nMaxPlayers (0x102ec0, 0x102f40, Update 0x1030e0), so players 18+ simply get no voice. Minimal fix: return early from 0x102e30 when idx >= m_nMaxPlayers. Full fix: the arrays become [65], DW becomes 3, and VoiceMask grows | Easy (clamp), Medium (full: message and client change too) |
| B | **StatShot tracker** (Xbox-only, singleton *0x552b08, `new 0x54` at 0x14401b, vtable 0x351f10) | Reset [2] 0x153800 clears +0x10..+0x50 (17 dwords, player 1..17). AddDamage [3] 0x153890 does `dmg[ENTINDEX(victim)] += ±n` at **+0xc+idx*4 with no check** (0x1538d4). It is called from CBasePlayer damage paths at 0x118249 and 0x118a48 on every hit. Send [4] 0x1538e0 loops 17 (`movl $0x11` at 0x153911), builds an enemy mask and a team mask, and sends StatShot(7) = SHORT, SHORT, BYTE, BYTE, BYTE | 17 dwords; masks sent as **16-bit** shorts, so bit 16 (player 17) is already truncated | **Heap overflow on the first hit on player index >= 18** (+0x54 is past the 0x54-byte block). Must fix: either bound idx <= 17 (stats only), or allocate 0x10+65*4, loop maxClients, and send wider masks (message size and client parser change) | Easy (bound) / Medium (extend) |
| C | **CSoundEnt** MAX_WORLD_SOUNDS = 64 | Object `PvAlloc 0x8a4` at 0x150b18 (vtable 0x351700). Pool at +0x94, stride 0x20, 64 entries (`cmp $0x40` at 0x1509b3). Initialize reserves maxClients client sounds (loop at 0x1509c8). Errors: "Free Sound List is full!" 0x150a5d, "Could not AllocSound() for Client Reserve! (DLL)" 0x150a6d. SoundPointerForIndex is range-checked (0x150aaf/0x150ac6) | 64 slots | 33 players leave 31 world sounds. 64 leave 0. With 65 the client reserve fails: ALERT only, and the index check prevents corruption. Raise the pool to 128: the object grows to 0x94+128*0x20 and the `$0x40` bounds change | Easy-Medium |
| — | CBasePlayer | `PvAlloc 0x8b4` at 0x125aff, vtable 0x3460f8. CBot members start at +0x8b4 (CBot ctor 0x1682f0) | no per-player arrays found | none | — |
| — | CHalfLifeMultiplay | `new 0x2c0` at 0x14e1c2, ctor 0x143cb0 (CHalfLifeRules is 0x2d8 at 0x14e191) | none (map votes at +0xcc are indexed by vote, 0x148c5b) | none | — |
| — | CCSBot | `PvAlloc 0x27ec` at 0x163336, ctor 0x1725d0. Indexed members: enemy queue +0x26a8 [20] x 8-byte EHANDLE (wrap 0x14, 0x175e76); per-zone +0x22d4; approach points +0x2344 (stride 12); +0x241c [64] x 8; +0x2788 [5]. Visible counts +0x26a0 (enemies) and +0x26a4 (friends) are ints | **no player-sized arrays** (this is the pre-CZ Xbox bot: no m_playerTravelDistance, no BotProfile) | none | — |
| — | CCSBotManager | `new 0x2d0` at 0x12a84d, ctor 0x161670. Zones at +0x20, stride 0x74, count +0x1f0, 16 areas each (0x162b4e). Radio timestamps +0x204 [24][2] (0x163230) | none | none | — |

**Loops on gpGlobals->maxClients, which scale on their own** (77 direct sites): 0x103149 0x103201 0x11995b 0x11d55c 0x11d5c4 0x11fd63 0x11fdbe 0x12280d 0x1232dd 0x14288f 0x143d0b 0x1465a9 0x146601 0x147cdb 0x147d96 0x14895b 0x148a01 0x148ae4 0x148baf 0x14a075 0x14a0b1 0x14aec1 0x14aefe 0x14b01d 0x14b142 0x14b295 0x14b2d6 0x14b3b5 0x14b42e 0x1509c8 0x1579b9 0x15b926 0x15bbd1 0x160625-0x160d3c (bot_util UTIL_*Count, 16 sites) 0x1619cb 0x161a1d (StartFrame: BotThink for each FL_FAKECLIENT) 0x161bc3-0x161d75 0x162232 0x16262d-0x16267f 0x163276 0x168ab6-0x168cd6 0x16b4d0 0x16d0ea 0x16d19d 0x175c4b 0x175d9a 0x179e93 0x179feb 0x17f607 0x18a339. A scan of the ±50-instruction window around each found no player-indexed member or global array (the only hits were 0x148c5b map votes, 0x14a00f the map list, 0x17f6d7 nav-area per-direction lists, and 0x18a3a8 the sin table 0x542600). A scan after every ENTINDEX call found only items A and B.

### 2. Bitmasks over players

| Mask | VA | Width | 33/65 effect |
|---|---|---|---|
| Voice masks (A) | 0x102e3c-0x102eb4, 0x10318e-0x103329 | 32-bit, 1 dword | see A |
| StatShot enemy/team masks (B) | 0x15392a-0x153952, sent 0x153988/0x153994 | built in 32 bits, **sent as 16-bit** | see B |
| trigger_hurt per-tick mask in pev->impulse (+0x184) | 0x16b9b4-0x16ba06: `1 << (entindex-1)` | 32-bit; x86 `shl` takes the count mod 32 | players 33-64 alias players 1-32, so some damage ticks are skipped in the same frame. No corruption. Fix: per-player timestamp or a 2-dword scheme | Low |
| Bot "enemy" masks | none; the bot keeps counts and EHANDLE queues | — | — |

### 3. User messages carrying a player index (LinkUserMessages 0x116aa0-0x116f45)

Sizes are from REG_USER_MSG; write sequences come from the send sites.

| Msg (gmsg var) | Size | Writes (send site) | Index field |
|---|---|---|---|
| DeathMsg (0x5529bc) | var | B B B B Str (0x149844) | BYTE killer, BYTE victim |
| ScoreAttrib (0x5529c0) | 2 | B B (0x11efef) | BYTE |
| ScoreInfo (0x5529c4) | 9 | B S S S S (9 sites, e.g. 0x11bcf8) | BYTE |
| TeamInfo (0x5529c8) | var | B Str (5 sites) | BYTE |
| StatusValue (0x552a14) | var | B S (0x123baa) | value is SHORT (fine) |
| Radar (0x552a30) | 7 | B X X X (0x1227ae) | BYTE |
| Spectator (0x552a34) | 2 | B B | BYTE |
| ClCorpse (0x552a70) | var | Str L L L X X X L B B B | BYTE |
| HostagePos (0x552a64) | 8 | B B X X X | hostage id BYTE |
| HLTV (0x552a74) | 2 | B B | BYTE |
| BotRadio / SendAudio | var | B Str ... | BYTE |
| StatDcon (0x552a50) | 1 | B = ENTINDEX (0x1539f0) | BYTE |
| **StatShot (0x552a4c)** | 7 | **S S** B B B | **16-bit masks: breaks above 16** |
| **VoiceMask** (game DLL 0x102dcd) | 8 | L L | **32-bit masks: breaks above 32** |

There are no SayText, Location, BombDrop-with-index, or BotVoice messages in this build. Every index field is a BYTE (fine up to 255). Only StatShot and VoiceMask need a format change.

### 4. Team limits, bot quota, bot names

- **Team full = spawn points.** TeamFull 0x146e00: T full when m_iNumTerrorist (+0x4c) >= m_iSpawnPointCount_T (+0x5c), and CT full when m_iNumCT (+0x50) >= m_iSpawnPointCount_CT (+0x60). The counts are set at 0x1482e4-0x14837c from the info_player_deathmatch (T) and info_player_start (CT) entities, with no cap. The same test is inlined in SelectDefaultTeam 0x13b3cc-0x13b411, in AddBot 0x162c0f-0x162c2f ("Team is full" 0x162d49), and in MaintainBotQuota 0x162693-0x1626b5, which stops adding when both teams are full. The player menu shows #Terrorists_Full / #CTs_Full / #All_Teams_Full (0x13dccb-0x13dd8f). **So the real cap is the map's T+CT spawn count.** It was not measured: the SXWad packs are compressed. src/game/state_probe.cpp already logs "spawns NAME ct C t T". 64 bots need at least 32 spawns per side, or a patched TeamFull plus spawn overlap handling.
- **Team stacking** 0x146e30: (team+1) > other + mp_limitteams (+0x9c), giving "Team is stacked" 0x162cfe. This is relative, so it scales.
- **bot_quota** (cvar value 0x37bf9c) in MaintainBotQuota 0x162580. With bot_quota_match (0x37bfb0) set, the quota is humans x factor. With bot_join_after_player (0x37c0a0) and no humans, it is 0. It is clamped to maxClients − humans (−1 more with bot_auto_vacate 0x37c0b4) at 0x162627-0x162688. CreateBot 0x163270 refuses when UTIL_ClientsInGame (0x160620) >= maxClients. There is no constant cap and no cvar bounds. The 16-bot cap is in the front end (src/game/pc_menu.cpp kMaxBots, 0xE1D10, max 8 on one team).
- **Bot names** (0x162390): there is no BotProfile.db. The table at 0x37fdc8 is [26 letters][12 ptrs] with 158 names. The letter range depends on bot_difficulty (jump table 0x1624c8): diff0 rows 12-25 (71 names), diff1 rows 10-20 (59), diff2 rows 0-12 (93), diff3 rows 0-2 (**28**). The bot_add prefix '!' / '-' / '?' selects rows 0-7 (62), 8-16 (49), or 17-25 (47). The code tries up to 30 random names for an unused one (0x162280; loop `cmp $0x1e` at 0x1624b2) and then **accepts a duplicate**. Nothing fails. 64 expert bots would mostly be duplicates (cosmetic; the engine's handling of duplicate names is the engine's matter). An optional fix is to append a number.
- The bot id counter 0x3a4ca4 (CBot ctor 0x1682f0, stored at +0x8d4) only rotates zones; it is unbounded and harmless.

### 5. O(n²) per-frame structures (identified, not measured)

| Site | What | Cost |
|---|---|---|
| 0x175c30, called from 0x175dd0, called from bot update 0x173e1f | each bot scans all maxClients players. It calls IsVisible 0x174830 (up to 5 point traces through 0x174780) and counts visible enemies/friends (+0x26a0/+0x26a4) | bots x players x ≤5 traces |
| 0x179d60 (loop 0x179e93), 0x1851f4, 0x186e9b, 0x174155, 0x174a80 | other IsVisible users over players | the same order |
| 0x17f5f0 (loop 0x17f607) | per bot, per player, nav-area work (0x17d980, 0x17f550) | bots x players |
| 0x168ab6 / 0x168b84 / 0x168c8e (CBot helpers), 0x16d0ea, 0x18a1a4 (loop 0x18a339 calling 0x171b10) | per-call loops over all players | O(n) per call, O(n²) overall |
| CVoiceGameMgr::Update 0x1030e0 | m_nMaxPlayers² calls to CanPlayerHearPlayer | capped at 17 unless raised |
| (engine, for reference) PM_Move physents | every player and bot moves against all other players (FINDINGS "Player collision") | n² in the engine |

---

## INFERRED from HL SDK / CS 1.6 / CZ source (not checked instruction by instruction)

- Spawn selection (EntSelectSpawnPoint ~0x11eb00-0x11ef48) follows CS 1.6. When every spot is occupied it reuses spots and may telefrag (TakeDamage 200 at the spot). The team-full rule normally keeps players within the spawn count, so lifting TeamFull without more spawns would cause stacked or telefragged spawns.
- Everything client-side that receives these messages is HUD code (outside this range) and would need matching changes for StatShot and VoiceMask. That includes g_PlayerExtraInfo, CVoiceStatus (HandleVoiceMaskMsg 0x1b8d50), and the scoreboard arrays.
- The front end (0xbf000-0x102000) has `cmp $0x10` loops at 0x1018a3 and 0x10e900-0x10e947 and a voice-related `cmp $0x11` (0x101766 is a switch, not a player bound). These were not analysed; that area belongs to the engine/UI review.

## Minimum game-DLL patch set for 33/65

1. Clamp 0x102e30 (voice ClientConnected) to idx < m_nMaxPlayers. **Required: memory corruption.**
2. Bound 0x153890 (StatShot AddDamage) to idx 1..17, or grow the 0x54 block plus the loop and message. **Required: heap corruption.**
3. Optional: raise CSoundEnt to 128 sounds (0x8a4 alloc, `$0x40` bounds). Optional: fix the trigger_hurt 32-bit alias.
4. Gameplay: maps need ≥ N/2 spawns per side, or patch TeamFull (0x146e00 and its 3 inlined copies) and handle spawn overlap.
5. Raise the front-end bot list (pc_menu kMaxBots) and the engine slot count (other reviews). Expect bot think cost to grow about 4x (33) or 16x (65) from the O(n²) vision scan.
