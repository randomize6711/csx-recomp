# Networking: the PC protocol and the road to PC multiplayer (G8.2B, 2026-10-23)

Direction (ARCHITECTURE.md "Direction: PC first"): one PC network protocol, no Xbox compatibility mode, designed for
future online play and dedicated servers. The game's own netcode (GoldSrc's: delta-compressed snapshots over a
reliable/unreliable channel, client prediction, server lag compensation) is kept where it works and changed where
the 65-slot expansion or safety needs it. A rewrite is not proposed: nothing found is a blocker (section 4).

## 1. What G8.2B changed (protocol 47)

| Item | Before (Xbox, protocol 46) | Now | Where |
|---|---|---|---|
| Protocol version | 46 | **47** at every check and writer: the connect packet, svc_serverinfo, CL_Parse_Version, SV_CheckProtocol and its messages, the server info string, demos (their network protocol and checks), the version strings | config/player_slots.toml "the protocol" |
| Player slots | 17 (client refuses more: "Bad maxclients") | 65 on both ends | engine server and client changes |
| svc_pings | slot in 5 bits (32 players at most), the client indexed cl.players with it unchecked | slot in 8 bits; the client checks it against maxclients | 0x9DC42, 0x31A00, hook 0x31A22 |
| VoiceMask (user message) | two 32-bit masks | two masks of 3 dwords (96 bits) | game DLL host code, client host code |
| vban (client command) | one hex dword | up to 3 hex dwords | same |
| StatShot (user message) | two 16-bit masks (players past 16 dropped) | two masks of 3 dwords, then three bytes | same |
| Player indices in other messages | bytes (ScoreInfo, TeamInfo, DeathMsg, Radar, ...) | unchanged (bytes hold 255) | |
| Bots in userinfo | the client had no mark of a bot (the engine's fake clients carry name, model, colours) | the server adds "*bot\1" to a fake client's userinfo (FL_FAKECLIENT), sent like any key: the scoreboard's BOT on every client; a client cannot set a * key | hook 0x79F91, src/game/scoreboard.cpp |
| Malformed indices | several HUD handlers wrote tables with an unchecked byte | Radar, DeathMsg, the field of view message and the two talk messages check the index (src/game/player_slots_hud.cpp); ScoreInfo, TeamInfo, ScoreAttrib, Spectator check against 65 | |

An original console or a build before G8.2B is refused at connect (protocol 46 against 47) instead of misreading the
wider fields. System link and Xbox Live are not kept compatible with the console; they are not a PC transport.

## 2. What the expansion relies on and keeps

- **Entity numbers** on the wire are 11 bits (2048 edicts); max_edicts at 65 slots is 0x311 + 15 x 65 = 1760.
- **Packet entities** per client frame: 256 (the server's stack buffer at 0x9DDED, the client's checks at 0x24660,
  0x247BB, 0x24A10). 64 visible players plus the map's entities stay below it on the maps measured (FINDINGS.md); past
  it entities are dropped silently, not fatally.
- **Entity states per server frame**: 1024 distinct entities (0x9562A, Sys_Error past it): measured at 65 players.
- **Reliable / unreliable buffers**: fragmented reliable messages, SV_New_f's 64 KB signon of every client's
  userinfo: room for 65. The unreliable packet (the snapshot) did not have room: see the next item.
- **Packet capacity and datagram size** (FINDINGS.md "Packet overflow"): building a packet and sending it are separate
  limits. A loopback client's packet is built in 60 KB and sent as one loopback datagram of up to 64 KB (net_message's
  size, and a demo message's): a round restart's snapshot for a spectator with 65 players measured 6 to 9 KB, past
  the 4000 bytes the engine built it in. A network client keeps the engine's 4000-byte packet and 4037-byte datagram,
  which NET_SendLong splits into 1294-byte parts and NET_GetLong reassembles up to 4010 bytes: a larger snapshot over
  UDP is not sent as one datagram; it overflows safely and the client gets that frame without its entities, as the
  engine always did (section 4, item 3). The bit writer that built the snapshot no longer writes past any buffer.
- **Prediction and lag compensation**: the client predicts against every player's state in its frame ring (now 65
  per frame); the server rewinds every client's position (truepositions, now 65). Unchanged in behaviour.

## 3. Validation added

- svc_pings: a slot at or past maxclients is ignored (logged once).
- HUD messages that write per player tables: the index must name a player slot, otherwise the message is dropped
  (logged once per message kind).
- Guest tables: every relocated per player table ends at a guard page; a write past its end faults and is reported
  with the table and element.
- The prediction's player physents: never more than the 600 physents (a full array leaves the player out).
- Split packets (NET_GetLong): a part's count (1 to 5), index (below the count), length (1295 bytes at most) and place
  (inside the 4020-byte reassembly buffer) are checked; the engine checked none and a crafted part wrote far past the
  buffer. A bad part drops the packet.
- Netchan fragments: each fragment's offset and length must lie inside the received message (at most 1304 bytes, the
  second after the first), or the packet is dropped.
- Message buffers: the bit writer checks every byte against its buffer; SZ_GetSpace refuses a negative length and
  answers a request larger than a whole overflowing buffer with a scratch area (FINDINGS.md "Packet overflow").

## 4. Opportunities for PC multiplayer (not done in G8.2B)

Investigated in N0 (2026-10-24): docs/pc_multiplayer.md has the current architecture, the remaining limits, the
recommended design (a host transport under the engine's netchan) and the N1-N4 roadmap; it supersedes the list below
where they differ.

Ordered by value for online play; none blocks the 65-slot single player match.

1. **Transport.** The engine's netchan runs on the Xbox socket layer (XNet HLE): a PC UDP transport with NAT traversal
   (or a relay) and a listen / dedicated server mode is the first need. Matchmaking (a master server or a lobby
   service) replaces Xbox Live.
2. **Dedicated server.** The engine already separates the server frame (SV_Frame) from the client; a headless build
   that skips the client, the renderer and audio is a build configuration, plus console / rcon commands.
3. **Bandwidth.** Snapshots are delta-compressed against the client's acknowledged frame (CL_UPDATE_BACKUP 16 on the
   client, SV_UPDATE_BACKUP 16 in multiplayer). With 64 players the packet entity list per frame grows: per-client
   rate control (rate, cl_updaterate) exists in the engine; a priority by distance / visibility for player entities
   and a larger MAX_PACKET_ENTITIES would scale further. A 65-player snapshot passes 4000 bytes at a round restart
   (6 to 9 KB measured): over UDP that needs either a larger split reassembly (more than 5 parts, a buffer of the new
   size on both ends, the 4010-byte limit raised at NET_GetLong and recvfrom) or a snapshot that stops adding entities
   at the datagram's size and records only what it sent (so the next delta is computed against what the client has).
   The loopback match already sends it whole (section 2).
4. **Snapshot compression.** The delta description (delta.lst in the SXWad packs) could be tuned for 65 players
   (quantized angles and origins are already 8-16 bits); an entropy coder on the datagram is a further step.
5. **Interpolation and prediction.** The client interpolates other players between frames (ex_interp) and predicts
   its own movement; with internet latency the interpolation delay and the lag compensation window
   (sv_maxunlag) need tuning, not new code.
6. **Voice.** The Xbox communicator path (XVoice, the 16-player positional voice records at 0x1045B0) has no PC device;
   PC voice would be a new codec and capture path. The voice masks (who hears whom) are already 65-player.
7. **Security.** Every message handler that indexes a table with a value from the wire should check it (section 3
   covers the ones that write tables; the remaining reads were found in TextMsg's player names and the death
   notice's name lookups, which read the info list: bounded by its guard page, but not checked yet). A connecting
   client's own userinfo should be checked for * keys too ("*bot" is the server's to set; setinfo refuses them, the
   connect string was not checked).
