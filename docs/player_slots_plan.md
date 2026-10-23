# Player slots past 17: investigation and plan (2026-10-23)

Goal: 64 bots plus the human (65 slots) in a single player Custom Game, if it can be done safely. G8.2's first
part (16 bots in the original 17 slots, commits on branch g8.2) is the baseline this builds on.

Evidence: static disassembly of `build/ghidra/default_flat.exe`. Nothing in this document was changed in code. The
detailed tables, with the address behind every claim and P (proved from the code) / I (inferred from HL1 source)
marks, are in:

- `analysis/player_slots_engine_server.md`: engine server, protocol, per-frame limits, edicts, memory;
- `analysis/player_slots_engine_client.md`: engine client, prediction, client HUD, scoreboard, voice status;
- `analysis/player_slots_game_dll.md`: game rules, players, teams, bots, user messages.

## 1. Where 17 comes from

The original game sets the limit, not the recompilation. The framework has no player limit. Its guest address
space has room for relocated tables, and the host code reads `svs.maxclients` rather than assuming 17. It has two
gaps:

- **No way to change data.** The recompiler can hook instructions (`[[hook]]`) but cannot change an instruction's
  immediate or displacement. Raising the limit safely needs that ability (section 4, step 0).
- **Memory is the Xbox's 64 MB.** The game allocates through its CRT malloc inside that space, and how full that heap
  gets with 65 players can only be measured at run time.

The 17 itself is compiled into every part of the game, which all share the same engine and HUD source with
`MAX_CLIENTS` / `MAX_PLAYERS` / `VOICE_MAX_PLAYERS` = 17:

| Where | What | Evidence |
|---|---|---|
| Title | Every match starts with "-maxplayers 17 +maxplayers 17" | template 0x33AA60 |
| Engine server | Clamps maxplayers to 1..17, limit 17, client_t block a **constant** 17 x 0x5010 | 0xA19F3, 0xA1A10, push 0x55120 at 0xA1A0B |
| Engine client | Rejects a server with maxclients > 17 ("Bad maxclients") | 0x2E8E1 |
| Engine client | svc_updateuserinfo slot < 17 | 0x2CA2C |
| Engine client | Customization index < 17 | 0x2E02A |
| Engine client | Static arrays of 17 | see 2 |
| Client HUD | Arrays [18] indexed 1..17 | memset 0x1B926D |
| Game DLL | Voice manager clamped to 17 | 0x102DB0 |
| Game DLL | Shot statistics block of 18 ints | alloc 0x54 at 0x14401B |

The game's other limits that matter for 64 bots:

- **Teams are capped by spawn points.** A team is full at its map's spawn count (TeamFull 0x146E00, with copies at
  0x13B3CC, 0x162C0F and 0x162693). The most spawns any team has is 18 (de_chateau CT, de_aztec T). So no map can take
  more than 36 players, and the 8 / 8 maps take 16.
- **bot_quota has no constant cap.** It is clamped to maxClients minus the humans (0x162580).

## 2. What depends on the count

### Has to change for any count above 17

**Engine server**

- **Constants and the client_t block.** The clamp, the limit, and the block's size, header and clear count (0xA19F3,
  0xA19F8, 0xA1A10, 0xA1A0B, 0xA1A40, 0xA1A4F).
- **Four loops bounded by 17 instead of maxclients.**
  - Duplicate names (0x99103, 0x99126).
  - The userinfo name loop (0x9E84F, 0x9E8B1).
  - The voice relay (0xABE8E).
  - The edict-to-userinfo lookup at 0x791C5 (players 18 and up would get empty userinfo).
- **Three static arrays for 17 that loops up to maxclients index.** At 18 or more these overrun the arrays and corrupt
  memory.
  - Lag compensation positions at 0x4E46C0 (0x58 x 17; it is followed by timer state).
  - The last ping table at 0x4E0448.
  - The last loss table at 0x4E449C.

**Engine client**

- **The frame ring's player states.** cl.frames[16] each embed playerstate[17] (0xFB each, at frame+0x18, frame
  0x1B60). There are 51 frame-stride sites, 35 state-stride sites, and about 82 constant references. **This is the
  hardest item.**
- **cl.players[17].** 0x24C each at 0x7791F0, inside `cl`, with about 60 references and two end-pointer loops.
- **predicted_players[17].** At 0x3AE3D0, 2 sites, with loops at 0x25FE5 and 0x26276.
- **The player model cache[17].** At 0x6A38A0, about 14 references.

**Client HUD**

- g_PlayerExtraInfo[18] at 0x57E978 (about 97 references; the Radar handler writes into it without a check).
- g_PlayerInfoList[18] at 0x5487B0 (about 26 references).
- Small arrays at 0x548750 and 0x548918 / 0x548D40.
- The scoreboard's sort arrays [27] / [27] / [17], inside its panel object.
- The voice status object's pointer[17] and talk times (inside 0x56D6E8).
- The spectator's next / previous loop, which wraps at 17 (0x1A9089).

**Game DLL**

- Voice ClientConnected (0x102E30) writes at ENTINDEX-1 with no check. From player 18 it writes out of bounds.
- StatShot AddDamage (0x153890) does `dmg[ENTINDEX]` with no check. The first hit on player 18 overflows the heap
  block. Its Send loop is fixed at 17.

**Protocol**

- **svc_pings sends the slot in 5 bits** (writer 0x9DC42, reader 0x319A0 with no bounds check). That is already broken
  at 33, so it needs 7 bits for 65.
- **VoiceMask is two 32-bit longs**, and the client's ban sender loops to 32 and sends one dword.

### Only matters at 65

- **The voice listen bit vector** at client_t+0x5008 holds 64 bits; slot 65 would write the next client's `active`.
- **CSoundEnt has a 64-sound pool** and reserves one per client (0x1509B3, 0x1509C8). At 64 players, world sounds,
  which the bots hear through it, run out. That would change bot behaviour, so the pool has to grow.
- **trigger_hurt keeps a 32-bit player mask** in pev->impulse (0x16B9B4). Players 33 and up alias, which skips some
  damage ticks.
- **At most 256 packet entities per client per frame** (0x9DDED). 64 visible players plus their weapons get close,
  and the excess is silently dropped.
- **At most 1024 distinct entity states per server frame** (0x9562A, Sys_Error past it). This needs measuring at 65.
- **The server info reply buffer is 2048 bytes** (0x9AFE3). It gets truncated past about 50 players, but does not
  crash.

### Already scales

- svs.maxclients-bounded server loops, and per-client frames (allocated from the limit).
- num_edicts = maxclients+1 and ED_Alloc's start.
- max_edicts = 0x311 + 15 x maxclients (1040 / 1280 / 1760 for 17 / 33 / 65).
- cl_entities (from max_edicts).
- Entity numbers on the wire (11 bits).
- Every player index in a user message is a byte (DeathMsg, ScoreInfo, TeamInfo, Radar, ...).
- svc_serverinfo maxclients is a byte.
- CBasePlayer, the game rules, CCSBot and the bot manager have no arrays sized by player count.
- Bot names come from a 158-name table; past it they repeat, which is harmless.

### Cost

- **Memory.** About 70 KB more per slot, so about +1.1 MB at 33 and +3.3 MB at 65.
- **CPU.** Every bot checks every player's visibility with up to 5 traces (0x175C30, 0x174830), so bot think time
  grows with the square of the count. Expect about 4x at 33 and about 16x at 65 compared with 17. G8.2 measured 0.38
  ms of server frame at 17 players, which suggests roughly 1.5 ms and 6 ms; this has to be measured.

## 3. Answers

1. **Source of 17.** Section 1. It is the original game's compile-time MAX_CLIENTS in the engine, the HUD and the
   voice manager, plus the title's command line. The recompilation adds no limit of its own, but lacks a way to
   change instruction data.
2. **Dependents.** Section 2. These are the analysis files' tables.
3. **Origin.** The original game. The framework only lacks a tool.
4. **32 / 64 / 65 slots.**
   - **33** needs every item in "for any count above 17".
   - **65** adds the items in "only at 65". Those are small next to the frame and HUD relocation.
   - **No hard blocker was found for either.** Nothing fixed by the wire format stops below 65, apart from svc_pings
     and VoiceMask, which both ends can be changed for.
   - The work is the same for 33 and 65 if every table is sized by one constant N. So the plan builds for N = 65 and
     tests at 33 and 65.
5. **Protocol.**
   - **Required.** svc_pings goes from 5 to 7 bits. Voice masks are clamped (no voice in a local match). Both ends are
     this binary over loopback, so that is safe locally.
   - **System link and Live.** An original console rejects maxclients > 17 (0x2E8E1) and decodes svc_pings with 5
     bits. So an expanded match must never be offered or hosted over system link or Live, and the front end has to
     refuse that. Joining a 17-slot server from this build stays compatible only if the wider svc_pings is used for
     expanded matches only (the slot count in svc_serverinfo selects it).
6. **Plan.** Section 4.

## 4. Implementation plan

The rule throughout is **relocate, don't grow in place**. Each array sized for 17 moves to a new table sized for N in
unused guest memory, and only the code that addresses that array changes. Fields and objects around it keep their
offsets. Growing a struct in place would shift hundreds of unrelated field references, many of them through
registers that an address scan cannot find.

### Step 0: Tooling (framework; SDK patch plus our tools)

**Recompiler patch** (`patches/sdk/000N-data-remap.patch`): two new config forms.

- `[[imm]] at = VA, old = X, new = Y` replaces one instruction's immediate or displacement. Codegen refuses if the
  instruction does not hold X.
- `[[remap]] lo, hi, to` rewrites absolute displacements and immediates in [lo, hi) to `to + (v - lo)` at listed sites,
  or at every site in a function range. Every rewrite is logged.

**Audit tool** (`tools/slot_audit.py`), the safety net against silent misses.

- It lists every instruction that:
  - references a relocated range;
  - multiplies or adds by a relocated stride (0xFB, 0x24C, 0x4C, 0x28, 0x20C, 0x5010, ...);
  - or compares with 16 / 17 / 18 / 32 or 0x55110-style sizes in the engine, HUD and game ranges.
- It fails unless each one is classified in `config/player_slots.toml` as rewritten, not a player count, or reviewed.
- Its output is checked by a test.

**Guard pages.** A test-build option places a no-access page after each relocated table, so an overrun traps instead
of corrupting memory.

**Complexity: medium.** The recompiler patch is a few hundred lines; the manifest is the bulk.

### Step 1: Engine server and game DLL to N

- The constants and the client_t block, and the four loops bounded by 17.
- Relocate the lag compensation, ping and loss tables.
- Voice manager:
  - clamp ClientConnected to its maximum;
  - keep voice at most 32 players;
  - move the client_t voice bits out for 65.
- StatShot: grow the block to N+1 ints, and clamp the Send loop and the 16-bit masks so players past 16 are left out
  of that statistic rather than overflowing.
- Grow CSoundEnt's pool to 64 + N.
- Give trigger_hurt's mask a host-side side table through a hook (at 33 and up).

**Complexity: small to medium.** About 30 sites.

### Step 2: Engine client and HUD to N

- Frame player states move to a new [16][N] table. That means the 35 state-stride sites, and the frame-stride sites
  that reach a player state (the audit separates them from frame fields), plus the delta decoder's pointer walks.
- Relocate cl.players with its end-pointer loops, predicted_players, the model cache, g_PlayerExtraInfo,
  g_PlayerInfoList and the small HUD arrays.
- Scoreboard and voice status object: their embedded arrays are reached by object offsets, so they are relocated
  per site (or the object is allocated larger and the arrays moved to its end).
- The client's 17 checks: Bad maxclients, updateuserinfo, customization, the spectator wrap.

**Complexity: large.** Several hundred sites, every one classified by the audit. The frame ring is the riskiest part.

### Step 3: Protocol

- svc_pings uses 7 bits when maxclients > 17, and 5 bits otherwise, so a 17-slot match is unchanged on the wire.
- Hosting or joining system link and Live is limited to 17 slots. A server with more refuses the original
  client's connection.

**Complexity: small.**

### Step 4: Spawn capacity (needs a gameplay decision)

No map has more than 18 spawns a team, so 64 bots cannot join under the game's own team-full rule.

- **Option A (recommended): extra spawn points at map load.** The host creates additional `info_player_start` /
  `info_player_deathmatch` entities next to the map's own. Positions are tested with the game's hull trace, so nothing
  is stuck in walls. The assets are not modified. TeamFull then counts them, unchanged.
- **Option B: lift TeamFull.** Players then stack on occupied spawns, which G8.2 showed leaves two players in one spot.
  Not recommended.

**Complexity: medium.**

### Step 5: Menu, tests, measurements

**Menu**

- "Number of Bots" goes up to the verified maximum.
- The limit should be set by spawn capacity as well as slots. The map is chosen after the bot count, so the screen
  states the maximum, and the game reports bots that cannot join.

**Tests**

- Regression tests at 33 and 65 slots with guard pages on:
  - joining, movement, kills, rounds and team balance;
  - the scoreboard at 65 (the list widget's scrolling is not analysed yet);
  - collision On and Off.
- A 17-slot match must be unchanged: the existing suites, plus svc_pings still 5 bits.

**Measurements**

- Benchmarks of server and bot time and memory at 17, 33 and 65 slots.
- The guest heap's headroom at 65, measured.

## 5. Unknowns, to be settled by steps 0 to 2

- Pointer-based accesses to the frame ring's player states and to the scoreboard and voice objects. These are found
  per function, not by address scan. The audit lists the functions that compute such pointers.
- Whether the scoreboard list scrolls past its visible rows.
- Whether the guest heap has room at 65 slots.
- Whether 1024 entity states a frame and 256 packet entities hold at 65 players.

## 6. Recommended order and checkpoints

| Checkpoint | Work | Ends with |
|---|---|---|
| 1 | Step 0 | The tool, the manifest and its test. Nothing in the game changes; the existing suites pass. |
| 2 | Steps 1 to 3 at N = 33 | A 33-slot match (32 bots, on maps with spawns added under step 4, or on de_aztec / de_chateau with 34 to 35 points). The 17-slot suites unchanged. |
| 3 | N = 65 and step 4 | 64 bots plus the player, measured. |
| 4 | Step 5 | The menu, docs, report. |

Each checkpoint stops for approval.
