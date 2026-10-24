# Player slots past 17: investigation and plan (2026-10-23, revised for G8.2B)

Goal (G8.2B): **65 player slots** for any mix of humans and bots, with networking designed for future PC
multiplayer. The baseline is G8.2's first part: 16 bots in the original 17 slots, committed on branch g8.2.

The project is PC first (ARCHITECTURE.md "Direction: PC first"), which shapes this plan:
- **65 is the only target.** There is no 17-slot legacy mode and no intermediate 33-slot milestone.
- **Original Xbox networking compatibility is dropped.**
- **Player Collision stays independent of the player count.**
- **The maps' own spawn points are reused** when there are more players than spawn points.

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
  at 33, so it becomes a byte (step 3).
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
   - **No hard blocker was found.** 65 needs every item in section 2, including the ones that only matter at 65.
   - Every table is sized by one constant N = 65 (`kMaxPlayers`). 33 is not a separate milestone, but a 33-player
     match is still a useful extra test case.
5. **Protocol.** It is designed for PC multiplayer between builds of this port. Original Xbox compatibility is dropped.
   - **Player slot fields become a full byte everywhere**, room for 255 slots; svc_pings' 5-bit field is the one
     that has to change. Every other player index on the wire is already a byte.
   - **Voice masks are sized for N on both ends**, not clamped, so voice can work in future online play.
     That covers VoiceMask, the ban mask sender and the server's voice tables. StatShot's 16-bit masks are widened
     the same way.
   - **The protocol version is raised.** Both ends already check it (the server's SV_CheckProtocol, the client's
     CL_Parse_Version: strings at 0x3277C0 and 0x31BE98). An original console or an older build is then refused
     cleanly instead of misreading packets.
   - **System link and Live are not kept compatible with the console.** Future PC online play needs its own
     transport and matchmaking; that is a later milestone, not this one.
   - **One wire format** for every match, with no per-match 5-bit mode.
6. **Plan.** Section 4.

## 4. Implementation plan

The rule throughout is **relocate, don't grow in place**. Each array sized for 17 moves to a new table sized for N in
unused guest memory, and only the code that addresses that array changes. Fields and objects around it keep their
offsets. Growing a struct in place would shift hundreds of unrelated field references, many of them through
registers that an address scan cannot find.

### Step 0: Tooling (done in checkpoint 1, FINDINGS.md "Player slot tooling")

- **`[[patch]]` instruction operand patches** (SDK patch 0036): one instruction, one operand, old and new value,
  checked against the image's sha256, the bytes, the text and the old value; refused for control transfers and
  addresses analysis never decoded; recorded in patches.txt and marked in the generated code. The `[[remap]]` range
  form of the first plan was dropped: every relocated reference is listed as its own patch, generated from the audit
  and the guest table layout, so a reviewer sees each one.
- **The audit** (tools/slot_audit.py, config/player_slots.toml, analysis/player_slots_inventory.json): every
  reference to the 20 tables, 76 classified sites, every unreviewed count constant, the frame ring's register trace,
  the unresolved list; `--check` against the committed inventory, `--require-patched` per table.
- **Guest tables** (config/guest_tables.toml, tools/guest_tables.py, src/common/guest_tables.cpp): the relocated
  tables' home, fixed addresses in a reserved region with a guard page after each table and a fault report; host
  code's Element() index check.
- **CSX_LOG_MEMORY=1** for the memory measurements.

### Step 1: Engine server and game DLL to N

- The constants and the client_t block, and the four loops bounded by 17.
- Relocate the lag compensation, ping and loss tables.
- Voice manager: relocate its tables to [N] with ceil(N/32) = 3 dwords per mask, raise its limit to N, give
  ClientConnected a bounds check, and move the client_t listen bits (64 today) out to an N-bit table.
- StatShot: grow the block to N+1 ints, the Send loop to N, and its masks to N bits.
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
- The client's 17 checks: Bad maxclients, updateuserinfo, customization, the spectator wrap, and the ones the audit
  found: the ScoreInfo, TeamInfo and ScoreAttrib handlers' index bounds (0x1B5890, 0x1B5A20, 0x1D5AAA), CVoiceStatus's
  clears and loop (0x1B7E52, 0x1B7FCE, 0x1B91AC).

**Complexity: large.** Several hundred sites, every one classified by the audit. The frame ring is the riskiest part.

### Step 3: Protocol

- svc_pings' slot becomes 8 bits. VoiceMask, the ban mask and StatShot carry N-bit masks.
- The client's limits are raised to N: Bad maxclients, updateuserinfo, customization.
- The protocol version is raised. One format for every match.
- The menu's server options, which offer up to 17 players for a hosted game (0xE3DE0), offer up to N. Hosting over
  the console's system link and Live transport is left as it is today; PC online play is a later milestone.

**Complexity: small to medium.**

### Step 4: Spawn capacity: the original spawn points reused

No map has more than 18 spawns a team, and the game treats a team as full at its spawn count. So 64 bots cannot
join under that rule.

**The team-full test changes.** TeamFull (0x146E00) and its three inline copies, at 0x13B3CC, 0x162C0F and 0x162693,
stop counting spawn points. A team is full only when the slots are; mp_limitteams and team balance stay as they are.

**When a team has more players than spawn points, the original points are reused.** No points are generated, and the
assets are not modified. The game's own spawn choice already passes over occupied points and then falls back to one
(the fallback is inferred from the SDK, around 0x11EB00; to be checked).

**Player Collision stays independent of the count.** Decided in checkpoint 2 (the user, 2026-10-23): players placed
on a shared point may overlap, and may die there with collision On; that is accepted, because Player Collision Off is
the setting for crowded maps. No hull-tested placement is added. The Xbox build's spawn choice (EntSelectSpawnPoint
0x11EAA0) does not telefrag: when every point of a team is occupied it collects them and picks one (0x125BF0).

**As built (checkpoint 2):** a hook after the game rules count the map's points (0x14837E) logs the real counts and
sets the counts TeamFull compares with to the slot count (a team without points stays closed).

### Step 5: Menu, tests, measurements

**Menu**

- "Number of Bots" offers 0 to 64. The maximum is the slots minus the humans in the match, and the screen states it.
- Spawn points no longer limit the count (step 4).

**Tests**

- Regression tests with 65 slots and guard pages on, at 64 bots and at a smaller count:
  - joining, movement, kills, rounds and team balance;
  - the scoreboard at 65 (the list widget's scrolling is not analysed yet);
  - collision On and Off.
- The existing suites must pass with 65 slots. Their 17-player expectations, such as regress_bots_aztec_24's
  "17 of 24", are updated to the new capacity. That is the feature changing, not a test being weakened, and each such
  change is listed in the report.

**Measurements**

- Benchmarks of server and bot time and memory at 16, 32 and 64 bots.
- The guest heap's headroom at 65, measured with CSX_LOG_MEMORY=1. The heap is in the 2 GB user range, not the 64 MB
  pool (docs/memory_modernization.md): 37 MB committed with 16 bots today, about +3.3 MB expected at 65.

## 5. Unknowns, to be settled by steps 0 to 2

- Pointer-based accesses to the frame ring's player states and to the scoreboard and voice objects. These are found
  per function, not by address scan. The audit lists the functions that compute such pointers.
- Whether the scoreboard list scrolls past its visible rows.
- Whether the guest heap has room at 65 slots.
- Whether 1024 entity states a frame and 256 packet entities hold at 65 players.

## 6. Recommended order and checkpoints

| Checkpoint | Work | Ends with |
|---|---|---|
| 0 | This investigation and plan | Done (8749f94) |
| 1 | Step 0, the memory investigation | Done (b4841f7, 9fd2633, b2c4029, 9dbe16b): tools, tests, docs/memory_modernization.md |
| 2 | Steps 1 to 4 at N = 65 | 64 bots plus the player in a match, with guard pages on and the audit clean |
| 3 | Step 5 | The menu, tests, benchmarks, docs and report |

Each checkpoint stops for approval.

## 7. Checkpoint 2 as built (2026-10-23)

FINDINGS.md "Player slots, 65" has the details and the measurements. Summary of how the plan was carried out:

1. **Audit open items settled**: the 34 compares of 17 classified (config/player_slots.toml; four were player
   limits: the spectator overview, the HUD's field of view reset, the Xbox communicator voice playback, left as it
   is); the server's protocol version writer is 0x970F0; the client's MAX_PACKET_ENTITIES checks are 0x24660,
   0x247BB, 0x24A10; the 33 functions that compute frame addresses reviewed one by one (the manifest's `reviewed`).
2. **Patch generator**: tools/slot_patches.py (as proposed; it also takes the hand-classified [[change]] entries).
3. **Server, game DLL, client, HUD** to 65, the frame ring's states in cl_frame_playerstates [16][65].
4. **Protocol** 47 (docs/networking.md).
5. **Spawns**: the team-full test no longer counts spawn points (section 4, step 4 "As built").
6. **Gates**: `slot_audit.py --require-patched` passes for every relocated table; the guest tables (21) are mapped
   with guard pages in every run; patches.txt records the 401 patches.
7. **Out of memory reports**: not needed by the measurements (the heap peaked far below its room, FINDINGS.md);
   moved to the memory modernization milestone (TODO 1f).

Found beyond the plan: g_StudioRenderer's per player state (+0x40B0, [17], indexed by entity number - 1) and the
HUD's per player field of view (gHUD +0xAC) and spectator positions; CVoiceStatus's speaker icon entities [17] x
0x74C and its fifth bit vector (+0x958C); CL_LinkPlayers' 17 x 0x74C bound.

What checkpoint 3 adds (section 6): the menu's Number of Bots to 64 (the slots minus the humans), the window tests
of the scoreboard and HUD at 65, the benchmarks at 16, 32 and 64 bots.
