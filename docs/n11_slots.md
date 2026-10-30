# N11 — More player and bot slots (65 stable; 96 and 128 experimental)

## Baseline

The stable build has **65 slots** (G8.2B: docs/player_slots_plan.md, FINDINGS.md "Player slots, 65"). It is
unchanged and is the only build the menus, the server browser and csx-server advertise.

## Audit for 96 and 128

| Area | At 96 | At 128 | Where |
|---|---|---|---|
| Player indexing, relocated tables | Generated from N (tools/slot_patches.py, config/guest_tables.toml counts N, N+1, N+2) | Same | `tools/slots_variant.py` |
| Voice masks | 3 dwords hold 96 players: unchanged layout | 4 dwords: the five masks' offsets in config/player_slots.toml (written for 3 dwords, 0xC apart) must be rewritten | **128 blocked until rewritten** |
| client_t listen bits | `kListenDwords = 3` holds 96 | needs 4 | `player_slots.cpp` |
| Entity storage | max_edicts = 0x311 + 15 x N = 2225 | 2705 | engine a0221 |
| Network serialization | Packet entities name an entity in **11 bits** (max 2048, SV_EmitPacketEntities 0x9D3F0). Above about 84 slots max_edicts passes 2048. Edicts are given out from the lowest free number, and a match uses about 640 on a large map plus about 4 per player (about 1030 at 96), so numbers above 2047 are not reached in practice. A cap of max_edicts at 2048 (the engine would then refuse an edict instead of sending a wrong number) is not made yet. | Same | analysis/player_slots_engine_server.md |
| Per-frame limits | 256 packet entities a client a frame (0x9DDED, extra ones are silently dropped); 1024 distinct entity states a frame (0x9562A, **Sys_Error** past it) | Tighter | Must be measured with a full server |
| Protocol compatibility | Protocol 48 (65-slot build: 47): the two builds refuse each other at connect | 49 | `slots_variant.py` PROTOCOL |
| Scoreboard, HUD, kill feed | Sized by N (G8.2B made them generic). Scoreboard rows scroll. | Same | `scoreboard.cpp` |
| Server browser | Shows a server's own max_players; nothing advertises 96 unless a 96 server runs | Same | `server_query.cpp` |
| Lobby / Custom Game | Bot lists derive from `slots::kMaxPlayers` | Same | `pc_menu.cpp` BotCounts |
| Bots, navigation, spawns | Spawn points reused (G8.2B step 4). Bot AI cost grows with N (FINDINGS "High population performance"). | Same | |
| Audio | CSoundEnt pool 64 + N (generated) | Same | |
| Rendering | Up to N-1 player models drawn; no per-player limit left in the renderer (studio state table N+1) | Same | |
| Memory | About 70 KB a slot: +2.2 MB at 96 | +4.4 MB at 128 | |
| Bandwidth | Grows with visible players. 65 slots needed 60 KB loopback datagrams (G8.2B). Remote clients' rate limits apply. | More | |

## Experimental build: 96 slots (built and tested 2026-10-30)

`python3 -I tools/slots_variant.py 96` builds a 96-slot game and server into their own folders, from its own
configuration. The stable 65-slot build never reads any of it.

| What | Where |
|---|---|
| Configuration (slot count, protocol 48, guest tables' sizes, regenerated operand patches) | `build/slots96/config` |
| The C++ side's guest table layout | `build/slots96/include/common/guest_tables_layout.h`, found before `src/` |
| Generated code | `build/gen96/game`: named `game` because the harness build compiles `src/<that name>` and leaves out its sibling folders |
| Binaries | `build/harness96/CounterStrike`, `build/server96/csx-server` |
| Compile flag | `CSX_MAX_PLAYERS=96` (`player_slots.h`) |

Found while building it:
- **The layout header.** The C++ code maps the guest tables from a header generated from the layout. With the
  65-slot header, the 96-slot build's tables (and the match's command line text in them) sat at the wrong addresses:
  the engine started without `-game cstrike` and never loaded a map. The variant now generates its own.
- **The generated-code folder name**, as above.

| Test | Result |
|---|---|
| `tests/scenarios/exp_server_96.txt`: the 96-slot csx-server, de_aztec, 95 bots | **PASS**: all 95 joined in 31 s; a minute of play with kills and round ends; no Sys_Error ("Too many entities"), guest trap or table overrun |
| `tests/scenarios/exp_bots_aztec_95.txt`: the 96-slot game (listen server), 95 bots and the player | **PASS**: 96 players at the start and after the round ends; teams 48/48; bots walk and fight; rounds end and restart; no errors |
| `tests/net/exp_protocol_96.txt` (`CSX_NET_SERVER_EXE=build/server96/csx-server`): the stable client joins the 96-slot server | **PASS**: refused by the engine with "This server is using a newer protocol ( 48 ) than your client ( 47 )"; no map loads. The refused client then sits at the console's "not connected" (the stable build never lists a 96 server, so its menus have no path to one). |

Not tested at 96: split-screen with four local players, remote human players over a real network, bandwidth and
CPU at 96 (the optimization phase is deferred), the window build at 96 (the variant builds the headless harness and
csx-server only).

## 128 slots: not built

- **Voice masks.** 129 bits need 4 dwords per mask. The five masks' offsets in config/player_slots.toml are written
  for 3 dwords (0xC apart), and the listen bits use `kListenDwords = 3`.
- **Short immediates.** Some patched compares may be signed 8-bit immediates (`cmp eax, 0x11`), which hold at most
  127. Every N-derived immediate must be checked for its encoding before 128.

`tools/slots_variant.py` accepts 96 only (its PROTOCOL table); 128 would also stop at its voice-mask check.

## Advertising

The menus, browser and docs for players offer 65. 96 exists only as this experimental build, run by hand, and
refuses (and is refused by) the stable build.
