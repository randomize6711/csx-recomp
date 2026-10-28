# PC multiplayer: architecture investigation and roadmap (N0, 2026-10-24)

Milestone N0 (investigation only, approved 2026-10-24); N1 (the PC transport and remote clients) approved 2026-10-25
and built: section 15 describes what exists, its tests, measurements and limits; N2 (the dedicated server, section 16)
and N3 (the server browser, section 17) since. Sections 1-14 are the N0 analysis and plan as approved. Direction:
ARCHITECTURE.md "Direction: PC first". Detailed evidence, with every guest address, is in the analysis files this
document cites:

- analysis/netcode_engine.md: the engine's netcode (server start, connectionless protocol, connecting, netchan,
  snapshots, delta, prediction, interpolation, lag compensation, voice, timeouts, map changes, system link, rcon).
- analysis/dedicated_server_browser.md: the host frame, the dedicated server remnants and the Xbox "Dedicated
  Server" host mode, server dependencies on the client, match configuration, system link discovery, the menus.
- analysis/splitscreen.md: local players, controllers, views, audio.
- docs/networking.md: what G8.2B already changed (protocol 47, 65 slots, validation, packet capacity).

Contents: 1 current architecture, 2 does remote play work today, 3 packet sizes and the transport, 4 options and the
recommendation, 5 snapshots and replication, 6 security, 7 dedicated server, 8 configuration, 9 server browser,
10 split-screen, 11 testing, 12 performance, 13 roadmap, 14 open questions and risks, 15 N1 as built, 16 N2 as built
(the dedicated server), 17 N3 as built (the server browser), 18 N4 as built (split-screen: docs/splitscreen.md).

## 1. Current architecture

Counter-Strike: Xbox runs the 2003 GoldSrc (Half-Life 1) netcode almost unchanged, under an Xbox front end that
does session discovery and joining itself. Three layers:

```
 front end (Xbox lobby, "SuperLauncher")    system link: UDP broadcast 1101, VDP 1102, TCP 1103/1104;
   discovery, join, map change, voice (XHV)  joins and map changes are title relaunches (releasemap.txt)
 ─────────────────────────────────────────
 engine (GoldSrc)                            connectionless packets (getchallenge, connect, info, players,
   server SV_*, client CL_*, netchan          rules, rcon), netchan (seq/ack, 1-bit reliable, fragments),
                                             delta snapshots, prediction, lag compensation
 ─────────────────────────────────────────
 sockets                                     Winsock-style UDP on ports 1001 (server) and 1000 (client);
   runtime XNet HLE (sdk/runtime/xdk/xnet.cpp) on host sockets, --instance N binds 127.0.0.N
```

| Area | How it works (original; addresses in analysis/netcode_engine.md) | PC port so far |
|---|---|---|
| Server start | The front end writes `+map <map> <options>` to Z:\releasemap.txt and relaunches the title; the boot code (0xBF470) builds `-game cstrike -maxplayers 17 ...`; Host_Map (0x5F520) spawns the server and runs "connect local" | maxplayers 65 (G8.2B); launch fixtures start a match directly |
| Listen server | Not a mode: the host's client is an ordinary client_t whose address type is loopback; the loopback channel is never rate limited | loopback packets to 60 KB (G8.2B) |
| Slots | SV_FindEmptySlot (0x99470): the first free client_t, ascending; nothing is reserved for the host; bots (SV_CreateFakeClient 0x79D70) take a slot with no netchan | 65 slots |
| Identity | userid: a counter per connection (0x3791E8). "uniqueid" is -1 for every client; no XUID reaches the engine. The lobby joins its own player list to userids by IP (0xEF8D0) | "*bot" key for fake clients |
| Connecting (remote) | getchallenge every 6 s (4 tries), then `connect <prot> <challenge> "<protinfo>" "<userinfo>"`; **the challenge is never checked** (SV_CheckChallenge 0x98CE0 returns 1 on every path); password (sv_password) for remote clients only; `connect <x>` ignores x and uses the address the lobby resolved (0x554334) | protocol 47 |
| Netchan | 8-byte header (seq with reliable bit 31 and fragment bit 30; ack with the reliable-ack bit); one reliable message in flight with a 1-bit sequence, resent implicitly when a later packet is acknowledged without it; two fragment streams (normal, file); late and duplicate packets dropped (no reordering); rate pacing by cleartime; **no qport**: a client is its IP and port | bounds checks (G8.2B) |
| Snapshots | Each client's frame: svc_time, client data, packet entities (PVS-culled by the game DLL's AddToFullPack, at most 256), its unreliable datagram (sounds, events, voice); delta-compressed against the frame the client acknowledged with clc_delta; ring of 16 frames; a client that stops acknowledging gets full updates | 4000-byte limit removed for loopback |
| Update rates | cl_updaterate 20 (server clamps 10..60), cl_cmdrate 30, rate 4000 B/s (clamped 1000..20000 in code), sys_ticrate 100 | |
| Prediction / interpolation | Local movement always predicted (no cl_predict); others drawn ex_interp (0.1 s) behind | 65 player states |
| Lag compensation | sv_unlag 1, sv_maxunlag 0.5 s: other players rewound to the shooter's view time; **the player in the last slot is never rewound** (0xAB06F: `n < maxclients`) | 65 truepositions |
| Voice | Engine clc/svc_voicedata relay exists but nothing seems to fill it on this build (inferred); Xbox voice is XHV in the front end (16 talkers), transport not traced | no PC voice |
| Timeouts | sv_timeout 15 s, cl_timeout 10 s; sv_failuretime 0.5 s (no snapshots to a silent client); front-end lobby players are never timed out (0x9C896) | |
| Disconnect | SV_DropClient sends svc_disconnect without the reason; a listen host disconnecting shuts the server down for everyone (CL_Disconnect 0x28F20 -> Host_ShutdownServer) | |
| Map change | `changelevel` is disabled ("CHANGELEVEL IS NOT SUPPORTED; E3 HACK", 0x60270); the game DLL's map change writes `+map` to releasemap.txt, tells the lobby peers (message 0x0B, five times) and relaunches; the clients relaunch 2-4 s later and rejoin through discovery. No engine reconnect | relaunch is an exec on Linux (sdk/runtime/kernel/boot.cpp) |
| Queries | info / details / infostring / players / rules / ping on the server port (1001), only while a multiplayer server runs; **they include the session's XNKID, XNKEY and host XNADDR** | |
| Rcon | rcon_password (default "": off), a failure ban; the rcon challenge is not checked either | |
| Client commands | clc_stringcmd: 23 engine commands allowed (0x379B60), the rest go to the game DLL; svc_stufftext runs any command on the client | |

Inherited unchanged: everything above except the "PC port so far" column. G8.2B changed capacity (65), the protocol
number, wire field widths, validation of indices and packet buffers (docs/networking.md).

## 2. Does remote PC multiplayer work today?

**No.** Tested between two separate processes (2026-10-24, headless builds, both in one private network namespace,
`--instance 1` hosting a System Link match, `--instance 2` searching):

- The host's System Link > Host a Match reaches the match: the host relaunches the title into it (as on the
  console). After that relaunch the four lobby sockets cannot be bound ("xnet: bind 100 to 127.0.0.1:1101 failed
  (10048)" for 1101-1104): on Linux the relaunch is an `execl` (sdk/runtime/kernel/boot.cpp:208) that does not close
  the old image's sockets (only the Windows branch calls xnet::CloseSockets) and the sockets are not close-on-exec.
  The relaunched host therefore never answers a search: the second process's System Link Matches list stays at
  "Please wait / Searching" (build/runs/mp2 snapshots). The same failures are in every Linux play log after a
  relaunch (also the user's 64-bot session of 2026-10-24, a match end).
- With the leak worked around for the test (an LD_PRELOAD shim, outside the project, that made the sockets
  close-on-exec), the relaunched host binds its ports and **discovery works**: the second process lists the host
  ("NewPlayer000, 1/10, Airstrip", with game type, rounds and friendly fire in the details panel; the search and the
  answer are the 15- and 132-byte lobby packets on port 1101). **Joining fails**: "This game session is no longer
  available." The joining process makes no TCP connection and sends no unicast packet (its socket calls were
  logged), so the join stops inside its front end before any network traffic; the cause was not traced further
  (the plan replaces this path).
- Two more attempts to reach the engine's own connection: a client booted with `+connect 127.0.0.1:1001` on its
  launch line enters the lobby's rejoin logic and shows the same "no longer available"; a client in its own match
  given the console command `connect 127.0.0.1:1001` shuts its server down and then sends nothing to the address
  (consistent with CL_Connect_f using the lobby's resolved address 0x554334 instead of its argument; inferred).
- So no path connects two processes today. The engine's UDP connection between processes has therefore not been
  exercised at all; N1 starts by making it work directly (section 13).

How the test was run (for N1's scenarios): a holder process makes the namespace (`unshare -rn sh -c 'ip link set lo
up; exec sleep N'`); a copy of tools/drive.py starts the game with `nsenter --preserve-credentials -U -n -t <holder>`
instead of `unshare -rn`; `--env INSTANCE=1` / `2` give 127.0.0.1 / .2; instance 2 keeps its hard disk in `hdd2`,
which drive.py does not clear (a stale profile there changes the menu path) and where `--launch` fixtures are not
put (a symlink hdd2 -> hdd fixes both).

Even with the socket leak fixed, system link is not a PC transport: discovery is a LAN broadcast on 1101; the join
goes through Xbox-secured addresses (XNKID / XNKEY / XNADDR, emulated as plain IPs by the runtime), a TCP lobby, and a
title relaunch on both machines at every join and map change; the connection itself is the engine's UDP netchan with
4000-byte packets and address-based identity.

## 3. Packet sizes and the transport

### 3.1 Limits that remain (after G8.2B)

| Limit | Value | Where | Kind |
|---|---|---|---|
| Snapshot buffer, network client | 4000 bytes | SV_SendClientDatagram stack buffer 0x9DF30 | arbitrary (Xbox) |
| Snapshot buffer, loopback client | 60 KB | net_client_datagram (G8.2B) | ours |
| Netchan datagram | 4037 (network), 64 KB (loopback) | Netchan_Transmit 0x6990D | arbitrary |
| Reliable message (netchan.message) | 3990 | Netchan_Setup 0x69717 | arbitrary; larger reliable data is fragmented |
| Reliable payload before fragmenting | 1200 | 0x699E9 | structural (MTU-safe) |
| Fragment size | cl_dlmax 64..1024 (128 default, 1024 while signing on); fragment buffer 1304 | 0x98B20, 0x6A1A5 | structural |
| Packet with fragments | 1304 (unreliable part dropped if it does not fit) | 0x69EB0 | structural |
| UDP split (NET_SendLong) | datagrams over 1304 bytes split into parts of 1294, at most 5 parts, 4010-byte reassembly, one sequence slot per part count | 0x6CAB0, 0x6C1A0 | arbitrary (sized to the 4000 limit) |
| recvfrom | 4010 bytes (a 4010-byte datagram is dropped as oversize) | 0x6C37E | arbitrary |
| Loopback queue | 4 messages of 4040 per side (engine); whole datagrams on the host (G8.2B) | 0x440C00 | ours now |
| Client datagram (sounds, events, voice) | 4000 | client_t +0x25C0 | arbitrary |
| sv.datagram / reliable_datagram / multicast | 4000 / 4000 / 1024 | SV_SpawnServer | arbitrary |
| Packet entities per snapshot | 256 | 0x9DDED, client 0x24660 | structural (stack arrays both ends) |
| Entity states per server frame | 1024 | 0x9562A | structural |
| Entity number on the wire | 11 bits (2048) | delta header | structural |
| net_message / in_message (client) | 64 KB | 0x6BA580 | fine |
| Usercmds per packet | 28 (backup + new) | 0xAB8F5 | structural |
| Voice per packet | 2048 client, 4096 server | 0x5D120, 0xABCC0 | fine |
| rate | 1000..20000 B/s (clamped in code), default 4000 | 0x9E96C, 0x9C1E0 | arbitrary (2003 modems) |
| cl_updaterate | 10..60 (sv_min/maxupdaterate), default 20 | 0x98800 | arbitrary |

Structural limits are part of the message formats or both ends' arrays; arbitrary ones are buffer sizes and clamps
that only reflect the console's memory and 2003 bandwidth.

### 3.2 How fragmentation, ordering and loss work now

- **Two fragmentation layers.** (1) The netchan fragments *reliable* data larger than 1200 bytes into numbered
  fragments (two streams), sent one per packet, reassembled in order; a client that finds a hole asks to "retry" the
  whole connection (0x6A1D0). (2) NET_SendLong splits any *datagram* over 1304 bytes into up to 5 UDP parts; all
  parts must arrive or the datagram is lost; there is one reassembly per sequence slot, no timeout, no limit on
  half-built datagrams beyond the 5 slots.
- **Large snapshots** are unreliable datagrams: on the network they are capped at 4000 bytes (past that the snapshot
  overflows and goes without entities: G8.2B made that safe); a 4000-byte snapshot is 4 UDP parts, and losing any of
  them loses the whole snapshot. The delta system tolerates a lost snapshot: the client keeps acknowledging its last
  complete frame and the next snapshot is a delta from that (or a full update after 15 frames).
- **Ordering**: a packet older than or equal to the last one received is dropped; no reordering buffer.
- **Loss and retransmission**: unreliable data is never resent (the next snapshot supersedes it); the single reliable
  message is resent when a later packet is acknowledged without its bit; there is no timer and no RTT estimate.
- **Dropped fragments**: a lost reliable fragment is resent like any reliable; a lost split part loses its datagram.

### 3.3 The sizes to keep apart

| Term | Meaning | Today (network) | Proposed |
|---|---|---|---|
| Internal snapshot | the entities one client can see this frame, before serialization | up to 256 entities | unchanged |
| Serialized message | the bit-packed svc_* stream for one client frame | ≤ 4000 bytes | ≤ 64 KB build capacity; a byte budget per client (section 5) |
| Transport packet | one netchan packet (header, reliable part, unreliable part) | ≤ 4037 | ≤ 64 KB, carried as one *message* by the PC transport |
| UDP datagram | what goes on the wire | ≤ 1304 per split part | ≤ 1200-byte payload (path MTU safe, IPv4 and IPv6 with tunnels), never relying on IP fragmentation |
| Fragmented message | a transport packet over one datagram | 2-5 parts, all or nothing | up to 64 parts of ≤ 1200 bytes (64 KB), all or nothing for unreliable messages, with reassembly limits and timeouts |

Measured on the loopback with 64 bots (soak_bots64_nocollide, CSX_LOG_DATAGRAMS, the largest packet of each second
over 909 seconds): median 457 bytes, 90th percentile 813, 99th percentile 2.6 KB, largest 8.6 KB (a spectator at a
round restart). Most snapshots fit one datagram; full updates and round restarts need 2-8.

## 4. Options and the recommendation

| | A: modernize in place | B: replace the transport | C: larger redesign |
|---|---|---|---|
| What | Keep NET_SendLong / NET_GetLong, the netchan and system link; widen the split (more parts, larger buffers), check the challenge, add a qport-like field, fix the socket leak | Keep the engine's netchan, snapshots, delta, prediction and lag compensation; replace everything below the netchan (NET_SendPacket / NET_GetPacket) with a host-side PC transport; replace system-link discovery and joining with PC sessions (address, browser) | B, plus new replication: per-entity priorities, a new snapshot format, a new reliable stream, possibly a new client prediction model |
| Complexity | low-medium (guest-side patches in the engine's packet code) | medium (host C++ code, a few hooks at clear boundaries) | very high (rewrites game-visible messages that the recompiled game and client DLLs parse) |
| Performance | as today | host code; fewer copies; per-connection buffers | potentially best bandwidth |
| Reliability | keeps all-or-nothing split parts with no timeout, address identity, relaunch joins | message fragmentation with limits and timeouts, connection ids survive NAT rebinding, direct joins | depends on new code quality |
| Security | patches on an old base; XNet emulation stays in the path | stateless handshake cookie, validated headers, limits in one place | same as B |
| Latency / bandwidth | as today | as today plus a per-client byte budget (section 5) | best possible |
| Maintainability | spread over guest patches in several functions | one host module with tests | large new surface |
| 65-player engine | yes | yes | needs re-validation of everything |
| Dedicated server | needs a non-lobby path anyway | natural (direct address, no lobby) | natural |
| Split-screen | no help | local clients connect to a server like any other (section 10) | same |
| Tests | patch tests | the transport is testable alone (unit tests, impairment) plus process tests | everything |
| Gameplay regression risk | low | low: game messages and simulation unchanged | high |

**Recommendation: B**, with three targeted engine changes from C where the evidence asks for them (section 5:
a byte budget per snapshot, rate limits for modern links, the lag compensation slot fix) and none of A's widening of
the old split code.

Why not A: its pieces (split parts, the lobby, address identity, relaunch joins) are exactly the parts that are
unsafe or Xbox-specific; widening them keeps 2-3 layers of fragmentation and the Xbox session protocol.
Why not C: the engine's netcode is not the blocker. Delta snapshots, prediction and lag compensation are proven
GoldSrc behaviour (CS 1.6 ran 32 players online on the same design), the recompiled game DLL and client DLL speak
its messages, and replacing them risks the gameplay feel the project wants to keep.

### 4.1 The PC transport (design for N1)

Host C++ code (src/net/, new), below the engine's netchan:

- **Hook points**: NET_SendPacket (0x6CC70) for NA_IP / NA_BROADCAST addresses, and NET_GetPacket (0x6C7C0) after
  the loopback queue: the engine hands over whole netchan packets (or connectionless packets) and receives whole
  packets; NET_SendLong / NET_GetLong are no longer reached. The loopback path is unchanged. The engine's own UDP
  sockets (ports 1000 / 1001) are no longer opened through XNet; the transport owns one UDP socket per process
  (IPv4 and IPv6; configurable port, default 27015 for servers).
- **Messages**: each engine packet is one transport message. A message up to 1200 bytes is one datagram; a larger
  one (up to 64 KB) is split into numbered fragments of ≤ 1200 bytes (message id, index, count); reassembly keeps at
  most a few messages per connection, drops a message whose fragments do not all arrive within a short window
  (e.g. 250 ms), and enforces a per-connection memory cap. Unreliable messages are all or nothing, which the delta
  system already handles; reliability stays the netchan's.
- **Connections**: a handshake before the engine sees the client: client hello (protocol version, transport
  version), server cookie (stateless HMAC of the address and time: no state for spoofed sources), client echo,
  server accept with a connection id. Every datagram carries the connection id; the server maps it to the engine's
  client by a stable virtual address, so a NAT that changes the client's port (or an address change) does not break
  the connection (the engine has no qport). Rate limits on hellos per source.
- **Connectionless packets** (info, players, rules, getchallenge, connect) pass through the transport as today but
  with an anti-amplification rule: a reply is never larger than the request unless the requester completed a
  cookie round trip (as Source added in 2020).
- **Engine changes that go with it**: CL_Connect_f uses its argument (not the lobby's address 0x554334); the
  challenge is checked (SV_CheckChallenge), or the transport's cookie replaces it; the 4000-byte network snapshot
  limit becomes the loopback's 60 KB build capacity plus the byte budget (section 5); the info replies lose the
  XNKID / XNKEY / XNADDR fields; SV_DropClient sends its reason.
- **Map changes**: until an in-engine map change works (open question 1), a relaunching server keeps its listening
  socket's port (the socket leak fixed: close-on-exec or CloseSockets on Linux) and clients reconnect on their own to
  the last server (the address kept across their relaunch), without the lobby.
- **Libraries**: none required. ENet was considered (MIT, small): it duplicates the netchan's reliability and
  identifies peers by address; GameNetworkingSockets needs protobuf and OpenSSL; netcode.io needs a token backend.
  The transport proposed is a few hundred lines of host code with unit tests. Encryption is not in N1 (a later
  option: a key exchange in the handshake and AEAD per datagram).

## 5. Snapshots and replication

The existing system is sound for 65 players; the changes worth making are budgets and limits, not a new model.

| Topic | Today | Proposal |
|---|---|---|
| Delta compression | per client against its acknowledged frame (16-frame ring), field bitmasks from delta.lst | keep |
| Visibility | PVS per client (AddToFullPack); no distance or priority | keep PVS; add an entity priority only for the byte budget below |
| Size limit | 4000 bytes, overflow = no entities that frame | **byte budget per client per snapshot** (from the client's rate and update rate): fill entities by priority (the player's own data, nearby / visible players, then the rest) and stop at the budget; entities left out keep their old state on the client and are sent next frame. The delta must then be computed against what was actually sent (the frame records only included entities), which the engine's packet_entities_t already represents |
| Rates | rate default 4000, max 20000 B/s; updaterate 20 (max 60); cmdrate 30 | PC defaults: rate 100 KB/s (code clamp raised), cl_updaterate 60, cl_cmdrate 60, sv_maxupdaterate 100; server-side caps configurable |
| Server tick vs snapshots | the server simulates every host frame (sys_ticrate 100 cap); snapshots per client at its update rate | keep apart: a fixed server tick (e.g. 100) and per-client snapshot rates (30-100); the dedicated server sleeps between ticks |
| Reliable vs unreliable | 1-bit reliable stream, fragments for large reliable data; snapshots, sounds, events and voice unreliable | keep |
| Loss recovery | lost snapshot -> next delta from the last acknowledged frame; 15 frames -> full update | keep; the transport's message reassembly timeout bounds how long a lost fragment holds memory |
| Interpolation | ex_interp 0.1 s, forced to at least 1/updaterate | with 60 Hz snapshots ex_interp can default to ~0.033-0.05 s (2-3 snapshots) |
| Prediction | always on | keep |
| Lag compensation | sv_maxunlag 0.5 s, last slot never rewound | fix the slot bound (0xAB06F, 0xAB227); keep the rest |
| Bots | simulated on the server, no packets | unchanged (bots are not reduced or simplified to save traffic) |

Dedicated servers gain the most from: the fixed tick with sleeping, per-client budgets, the rate defaults, and not
running the client and renderer at all (section 7).

## 6. Security and robustness

| Risk found | Evidence | Safeguard (where) |
|---|---|---|
| Connect from a spoofed address | the challenge is never checked (0x98CE0) | transport cookie handshake (N1); check the challenge too |
| No player identity | uniqueid -1 for all; userid per connection | optional per-install key pair (Ed25519) proving the same player across sessions, for bans and admin lists; no accounts, no third-party service (N2) |
| Session key leak | info / details / infostring send XNKID, XNKEY, XNADDR | remove the fields (N1) |
| Query amplification | info replies ~1 KB to a 20-byte request | cookie before large replies; per-source rate limits (N1) |
| Malformed packets | split parts and fragments now checked (G8.2B); engine fragment checks are loose | the transport validates every header and length; fuzz tests (N1) |
| Reassembly memory | 5 fixed slots today | per-connection and global caps, timeouts (N1) |
| Flooding | none | per-source hello limits, connection limit per address, drop before allocation (N1) |
| Server-run client commands | svc_stufftext runs anything on the client | allowlist on the client for remote servers (N2) |
| Client commands to the server | 23 allowed engine commands incl. god, noclip, kick, pause | check each against sv_cheats / admin rights (N2) |
| Rcon | password only, challenge unchecked, failure ban exists | off by default; challenge checked; consider rcon only over the authenticated transport (N2) |
| Name and userinfo | "*bot" is the server's; connect userinfo not checked for * keys | strip * keys from client userinfo (N1) |
| Last-slot lag compensation, timeouts that never fire for "lobby players" | 0xAB06F, 0x9C896 | fix with the lobby removal (N1/N2) |

No anti-cheat software, no accounts.

## 7. Dedicated server

Findings (analysis/dedicated_server_browser.md):

- GoldSrc's dedicated path is still in the image: g_bIsDedicatedServer (0x5EF670), cls.state 0 (ca_dedicated);
  Host_Init skips video, renderer and client init for it; the game DLL execs server.cfg when dedicated. It is never
  enabled: the only caller of CEngine::Load passes 0 (0xADA64).
- The Xbox **Dedicated Server** host option (mp_dedicated_server 1, "16 can play, but you can't play on this
  Xbox") is a listen server that does not play: it skips the 3D view and HUD, lets the bot manager add bots without
  a human, and leaves the host out of the player counts. Its own client still occupies a slot.
- Map loading does not need D3D (the BSP loaders make no graphics calls); the GPU work at load is the client's.
- Boot initializes XACT, the D3D device and the front end before any match; the launch fixtures already skip the
  front end.
- The headless harness already runs the whole game without a window (null renderer, null audio), but it still runs
  the client, its rendering code and the sound system: with 64 bots the client frame is about 58% of the game
  thread (FINDINGS.md "High population performance").

Plan (N2): a `--dedicated` mode of the same executable (no separate engine build): CEngine::Load(1), so cls.state 0
and no client, video, renderer, sound or input initialization; no window, no Vulkan, no SDL audio; boot straight to
the configured map (the launch fixture mechanism); all 65 slots for remote players and bots; console on stdin and
rcon; logging to a file. The risk is that the title's own boot and main loop (0xAD540, 0xADD43) assume a client:
N2 starts with the mp_dedicated_server listen mode on the null backends (works today in principle, 64 slots) and
then removes the local client. Linux first; the same code builds for Windows (no window needed).

## 8. Server configuration

A plain text `server.cfg` (the engine execs it for dedicated servers already) holding cvars, plus command-line
overrides (`+map de_dust2 +maxplayers 32 +sv_password x`). Settings and their cvars:

| Setting | Cvar / command | Exists |
|---|---|---|
| Server name | hostname | used by name, not registered: register it |
| Map, rotation | +map, mapcyclefile | yes |
| Game mode | mp_gametype / map prefix | yes |
| Maximum players | maxplayers (1-65), maxhumanplayers | yes |
| Bots | bot_quota, bot_join_team, bot_difficulty, bot_allow_rogues | yes |
| Player Collision | pc_player_collision (host command, src/game/pc_collision.cpp) | yes; needs replication to clients for their prediction (TODO 1d) |
| Friendly fire, round time, start money, freeze/buy time, rounds, time limit, win limit | mp_friendlyfire, mp_roundtime, mp_startmoney, mp_freezetime, mp_buytime, mp_maxrounds, mp_timelimit, mp_winlimit | yes |
| Password | sv_password | yes |
| LAN only / internet | sv_lan (+ whether to announce to a master) | yes / new |
| Port | port (the transport's) | new semantics |
| Rates | sv_maxrate, sv_minrate, sv_maxupdaterate, sys_ticrate | yes |
| Logging | log on, mp_logfile, the runtime log | yes |
| Rcon | rcon_password | yes |

## 9. Server browser

- **UI**: the original System Link Matches screen (SuperLauncher_BrowseForLocalServers: Server Name / Players /
  Map Name columns, arrows, a details panel with game type, dedicated, rounds, friendly fire; X refresh, A join, Y
  host) is the visual basis. Its rows are filled by code (0xE8E00): host code feeds them from a host-side server
  list. New: tabs or a filter row (Internet / LAN / Favorites / History), sorting by column, ping and password
  columns (the table's widths are data in the menu text), Direct IP (the existing on-screen keyboard), add to
  favorites.
- **LAN discovery**: a broadcast `info` query (the GoldSrc way) on the LAN to the default port range; servers answer
  with the info reply (cleaned of the Xbox fields).
- **Internet discovery**: a small master server protocol (servers send a heartbeat with their port; clients get a
  list of addresses and query each server directly for details and ping). The master's address is a setting
  (replaceable, self-hostable, none by default); the reference master is a small program in this repository. No
  Steam or other services.
- **Favorites and history** in pc_settings (or a servers file next to it).
- **Server details**: the players and rules queries.

## 10. Split-screen

Finding (analysis/splitscreen.md): **the Xbox game has no split-screen or second local player.** It polls all four
controller ports only to choose one active pad (0x3852F8); `cl`, `cls`, gHUD, the client DLL and the loopback client
side exist once; there is one audio listener. What exists and helps: the renderer can draw several views per frame
into their own rectangles (V_RenderView's nextView loop, used by the spectator picture-in-picture).

Options:

1. **One process per local player (recommended)**: each local player is a full client process connecting to the
   server (local listen server or remote) through the N1 transport; a host compositor shows their frames in one
   window (Vulkan external memory or shared images from each process's renderer), routes each controller /
   keyboard-mouse to its process and mixes their audio. The game's code barely changes; costs about 45 MB of guest
   memory and one game thread per player.
2. One process with the client state swapped per player each frame: `cl`, `cls`, the client DLL's globals, the HUD
   and the menus are hundreds of KB of scattered state; any missed global becomes a cross-player bug. High risk.
3. A second loopback client side in one process: same problem as 2 on the client.

Dependencies: N1 (the transport, so two clients can join one server), N2 (a server that does not need its own
player: the host's process could be a dedicated server plus N client processes), rendering (a compositor and
per-process window-less rendering), input (per-device routing exists in the window front end only for one player).

## 11. Testing

| Requirement | How |
|---|---|
| Separate processes | two or more instances in one private network namespace (`unshare -rn`, loopback up), each with its own run folder: a drive.py variant that joins a shared namespace (used for section 2's test) |
| Dedicated server plus clients | the dedicated mode plus N headless clients in the namespace |
| LAN | the namespace's loopback addresses (127.0.0.N) and a veth pair between two namespaces for broadcast |
| Latency, loss, reordering, duplication | `tc netem` on the namespace's devices (works without root inside the user namespace: checked) plus a seeded impairment layer inside the transport for exact, repeatable tests |
| Fragment loss | the transport's impairment layer (drop chosen fragments) and unit tests |
| Malformed packets | transport unit tests and a fuzzer over the header parser; a scripted sender of bad datagrams against a running server |
| Disconnects, reconnects, shutdown | scenario steps that kill / restart a client or the server process |
| Map transitions | a server map cycle with connected clients that must come back |
| Mixed humans and bots, 65 slots | a protocol load generator: a host-side fake client that completes the handshake and connect, sends null usercmds and acknowledges snapshots (it only echoes sequence numbers: no entity decoding), so 64 of them plus one real client fill a server cheaply |
| Unit tests | transport fragmentation, reassembly limits, cookie, connection ids, rate limits (tests/ like net_buffers_test) |

Real multiplayer is not claimed until two processes have played a round together over the transport.

## 12. Performance

- **Server CPU**: a 65-bot match's server frame is 2-4 ms at -O2 (FINDINGS.md "High population performance"),
  dominated by bot line-of-sight traces; with humans instead of bots the bot cost goes and the per-client snapshot
  cost (SV_WriteEntitiesToClient: AddToFullPack over every entity, delta encoding) grows with clients x entities.
  Bots get no packets, so a 64-bot match does not measure this.
- **Bandwidth**: a typical loopback snapshot with 64 bots is ~0.5 KB (p90 0.8 KB); at 60 snapshots a second that is
  ~30 KB/s per client and ~2 MB/s (16 Mbit/s) upload for 64 remote clients; round restarts and full updates are
  bursts of 3-9 KB per client. A dedicated server on a home connection may need 30-40 Hz snapshots; that is the
  operator's choice through sv_maxupdaterate and rates.
- **Memory**: per client_t 0x5010 bytes plus 16 frames of 0x828 (~55 KB per client, ~3.5 MB for 65); the
  transport's reassembly caps add at most a few hundred KB.
- **Client**: interpolation cost is per visible entity (unchanged); 65 players visible is the worst case already
  measured locally.

Benchmarks needed before the final design values (N1): snapshot sizes and server CPU with 64 load-generator clients
(the per-client build cost), the byte budget's effect on entity latency, bandwidth at 30 / 60 / 100 Hz, the
transport's cost per datagram, and a dedicated server's frame time without the client.

## 13. Roadmap

Order kept as proposed: N1 transport, N2 dedicated server, N3 browser, N4 split-screen. N2 needs N1 (remote
clients), N3 needs N1 (queries) and benefits from N2 (servers to list), N4 needs N1 and benefits from N2.

### N1: PC transport and remote clients

- **Scope**: the host transport (section 4.1) under the netchan; direct connect by address (`connect host:port`,
  and the existing on-screen keyboard as a minimal "Direct IP" entry in System Link); the handshake cookie,
  connection ids, message fragmentation (≤ 1200-byte datagrams, 64 KB messages), reassembly limits; the 4000-byte
  network snapshot limit removed (60 KB capacity) with the byte budget per client; the challenge check; info replies
  without the Xbox fields; the Linux relaunch socket leak fixed; clients reconnect by themselves after a map-change
  relaunch; PC rate defaults and clamps; the lag compensation slot fix; Player Collision sent to clients.
- **Dependencies**: none beyond G8.2B.
- **Approach**: hooks at NET_SendPacket / NET_GetPacket and CL_Connect_f; host code in src/net with unit tests;
  the lobby path left unused (System Link > Host a Match then serves as "listen server" UI).
- **Risks**: the relaunch on map change (clients must reconnect cleanly); engine assumptions that the lobby
  exists (timeouts skipped for lobby players, 0xEF8D0 / 0xEF930 bookkeeping); the byte budget's delta bookkeeping.
- **Acceptance**: two processes play a full round over UDP through the transport (listen server + client),
  including a map change and a reconnect; 1 server + 1 real client + 63 load-generator clients for 10 minutes
  without errors; correct behaviour under 100 ms latency, 5% loss, reordering and duplication; malformed and
  flooding tests pass; no regression in the existing suites.
- **Tests**: transport unit tests and fuzzing; the multi-process scenarios (section 11); netem runs; the existing
  headless and window suites.
- **Complexity**: medium-high (the riskiest of the four).

### N2: Dedicated server

- **Scope**: `--dedicated` (cls.state 0, no window / Vulkan / audio / input), all 65 slots for others, server.cfg
  and command line, console on stdin, rcon (checked challenge), logging, map rotation (relaunch with reconnecting
  clients, or an in-engine map change if open question 1 allows), fixed tick with sleep, stufftext allowlist on
  clients, admin checks for privileged client commands, optional player keys for bans.
- **Dependencies**: N1.
- **Approach**: first mp_dedicated_server 1 on the null backends (64 slots, works with the existing host client),
  then CEngine::Load(1) and the title boot and loop changes it needs.
- **Risks**: Xbox title code that assumes a client when cls.state is 0; map rotation.
- **Acceptance**: a Linux dedicated server process with no window runs 64 bots plus 1 remote human (and 65 load
  clients), cycles maps, survives client disconnects and reconnects, uses well under one core at 100 Hz tick
  without players; Windows build starts and serves.
- **Tests**: headless dedicated scenarios, multi-process with clients, a 30-minute soak.
- **Complexity**: medium.

### N3: Server browser

- **Scope**: LAN discovery, direct IP, favorites, history, refresh, sorting, filters, details (players, rules),
  ping, password indicator; the master server protocol and a reference master; servers' heartbeat (opt-in).
- **Dependencies**: N1 (queries over the transport), N2 (servers to list).
- **Approach**: the System Link Matches screen fed by host code; query engine in host code; settings in
  pc_settings.
- **Risks**: menu table limits (rows, columns) of the original UI; amplification-safe queries.
- **Acceptance**: a LAN server appears within 2 s; direct IP and favorites join; an internet listing through a
  local master instance in tests; sorting and filters work with 100+ entries.
- **Tests**: window scenarios with screenshots, query unit tests, a master server test.
- **Complexity**: medium.

### N4: Split-screen

- **Scope**: 2 players first (horizontal / vertical), then up to 4; one client process per local player
  composited in one window; controller and keyboard-mouse assignment; audio mixing; joining local and online
  servers within the 65 slots.
- **Dependencies**: N1, N2 (or a listen server), a compositor in the window front end.
- **Approach**: section 10 option 1.
- **Risks**: frame sharing between processes (Vulkan external memory on Windows and Linux), input focus, audio
  latency, CPU cost of several game threads.
- **Acceptance**: two local players play a full round on one screen against bots and on a remote server; each
  has independent input, HUD and audio.
- **Tests**: window scenarios per layout, input routing tests.
- **Complexity**: high.

## 14. Open questions and risks

1. **Map change without relaunch**: `changelevel` was disabled for E3 ("E3 HACK"); whether the PC build can
   re-enable an in-engine map change (memory is no longer tight) decides whether clients must reconnect at every
   map change. To test early in N1 / N2.
2. **XHV voice transport** was not traced; PC voice (a codec, capture and playback) is a separate project.
3. **delta.lst** (in the SXWad packs) was not read: the field widths for 65-player snapshots and any tuning.
4. **The front-end lobby's player list** is used by the engine (timeouts skipped, idle checks, userid
   bookkeeping): removing the lobby needs these uses replaced.
5. **ca_dedicated** has never run on the Xbox; how much of the title's boot assumes a client is unknown until tried.
6. **Player Collision** is a host setting the clients' prediction does not know (TODO 1d): it has to be replicated.
7. **Identity**: whether optional player keys are wanted (bans and admins), or IP and name are enough for
   self-hosted servers.
8. **Port defaults**: 27015 (GoldSrc's) or keep 1001.
9. **Bandwidth defaults** for home-hosted 65-player servers (30 or 60 Hz).

## 15. N1 as built (2026-10-25)

### 15.1 Architecture

- **Transport** (src/net/transport.{h,cpp}, src/net/udp.cpp; libsodium 1.0.22): one UDP socket per engine socket
  (server: port 27015, `net_port` / CSX_NET_PORT; client: any port), under the engine's netchan (which keeps
  sequencing, acknowledgement and the reliable stream). Handshake: hello padded to 1200 bytes (no amplification) ->
  challenge with a stateless cookie (HMAC-SHA256 under a per-process random key, over address, port, nonce, time and
  the server's key; 15 s) ->
  connect (X25519 public key, optional Ed25519 identity signature over the handshake) -> accept (connection id,
  AEAD proof of the keys). Data: ChaCha20-Poly1305 IETF, the 64-bit packet number as nonce, the header as associated
  data, keys per direction from crypto_kx; a 64-number replay window; engine packets up to 64 KB fragmented into
  <= 1200-byte datagrams (57 fragments at most), reassembly 8 messages / 256 KB per connection, 1 s; keepalive 1 s,
  idle timeout 30 s; disconnect frames with a reason; per-source (20/s) and global (500/s) handshake rates; 128
  connections, 16 per address. Transport version 2 (rejects carry the handshake nonce).
- **Engine glue** (src/game/net_engine.cpp): UDP_OpenSocket (0x6CDE0) and the receive (0x6C340) replaced,
  NET_SendPacket (0x6CCCC), NET_CloseIP and NET_GetLocalAddress hooked; split packets unused; a network client's
  datagram built in 60 KB like the loopback's (net_buffers.cpp). Rates: rate 100000, cl_updaterate 30, cl_cmdrate 60
  (`net_rate`, `net_update_rate`, `net_cmd_rate`), sv_maxrate's cap raised to 1000000 (config/net_patches.toml).
- **Sessions** (src/game/net_session.cpp): `+connect host[:port]` at boot skips the system link lobby; `connect`
  takes its argument (not the lobby's address); map change = the server relaunches and tells its clients
  (MapChange), each client relaunches into `+connect`; a connect or retry in a process that already joined a server
  relaunches too (one world load per process, FINDINGS.md "PC multiplayer N1"); the menus' pause is skipped while
  network players are connected. run_game.sh `--host [--port P]` and `--connect HOST[:PORT]`.
- **Engine security** (src/game/net_security.cpp, src/game/stuff_filter.h): the connect challenge checked
  (SV_CheckChallenge 0x98CE0 replaced; the challenge table's time is the issue order), the rcon challenge checked,
  the info replies without XNKID / XNKEY / XNADDR, "*" keys stripped from a network client's userinfo (and *pid, the
  identity's fingerprint, added), kick / pause / setpause / unpause refused to network clients and the cheats unless
  sv_cheats (the engine's own parsed token), svc_stufftext from a network server limited to a list (split as
  Cbuf_Execute splits), the last slot lag compensated, drop reasons sent through the transport, Player Collision sent
  at signon.
- **Not built**: the protocol load generator of section 11 (every load client is a full headless game process), LAN
  broadcast (no discovery: direct connect only), IPv6, NAT traversal, the byte budget per snapshot (section 5), a
  dedicated server (N2), a browser (N3).

### 15.2 Tests and commands

| What | Command |
|---|---|
| transport unit tests (ASan, UBSan; simulated network) | `python3 -I tests/net_transport_test.py` |
| stufftext filter | `python3 -I tests/stuff_filter_test.py` |
| packet buffers | `python3 -I tests/net_buffers_test.py` |
| multi-process scenarios (all, or named) | `python3 -I tools/run_net_tests.py [tests/net/NAME.txt ...]` |
| traffic summary of a run | `python3 -I tools/net_summary.py build/nruns/NAME/ROLE/run.out` |
| scoreboard rows of a run | `python3 -I tools/check_scoreboard.py build/nruns/NAME/ROLE` |

The runner gives every process a verdict (PASS, FAIL, TIMEOUT, CRASH, NOT STARTED) and the test the worst of them,
written to build/nruns/NAME/result.txt (RUNNING while it runs, INTERRUPTED when stopped), and samples each game's CPU,
memory, threads and file descriptors (ROLE/resources.csv, ROLE/fds.txt). A full run takes about 25 minutes: start it
in the background, not inside a tool call with a time limit.

Results on 2026-10-25 (build at 0bfcdfb + the test changes of 44005a8):

| Scenario | Processes | Shows | Result |
|---|---|---|---|
| net_two_humans | host + 1 | connect, signon, teams, movement, firing, hit, damage, kill, score, round end and restart, ping | PASS (91 s) |
| net_two_humans_latency | host + 1, netem 125 ms ± 20 ms, 1% loss | the same under latency | PASS (102 s) |
| net_two_humans_lossy | host + 1, netem 40 ms, 5% loss, 2% duplicates, 10% reordering | the same under loss | PASS (94 s) |
| net_join_early | host + 3 joining while the host loads | the first-second challenge bug (2 of 3 refused before the fix) | PASS (39 s) |
| net_join_together | host + 3 joining at once | simultaneous signon | PASS (39 s) |
| net_disconnect | host + 3 in turn, one frozen | graceful leave, rejoin in the freed slot, timeout, server shutdown told | PASS (84 s) |
| net_reconnect | host + 3 | 3 reconnects in one process (connect, connect, retry), joins and quits across processes | PASS (73 s) |
| net_map_change | host + 1 | the server's relaunch, the client rejoins, no descriptor kept | PASS (225 s) |
| net_malformed | host + 1 + fuzzer | random and truncated datagrams with valid prefixes; no trap, no error | PASS (79 s) |
| net_load_8 | host + 8 | 9 players moving for a minute | PASS (142 s) |
| net_full_65 | host + 1, 63 bots, collision off | 65 slots, three forced restarts (6-9 KB snapshots fragmented) | PASS (83 s) |
| net_full_65_2remote | host + 2, 62 bots, collision off | 65 slots, both remote players move, scoreboards of 65 (tools/check_scoreboard.py: all checks), 39 kills, 3 round ends, pings and BOT labels | PASS (223 s) |

The suite run before the last fixes had net_two_humans time out twice: once an orphaned game from an earlier
stopped run took its host's step files, once its client sampled its position across the Game Commencing respawn
(both fixed in 44005a8; FINDINGS.md "PC multiplayer N1"). Unit tests: net_transport_test 86 checks,
stuff_filter_test 14, net_buffers_test 18,314, scoreboard_rows_test 1222, guest_tables_test, slot_audit_test and
recomp_patches all pass.

Offline regression on the same build: the headless suite (tests/run_regression.sh) 45 of 46, the one failure
regress_lagcomp_last_slot, whose shot at slot 65's bot missed because the bot was not in view (the scenario now brings
it in front first, b29b171; it passes); offline matches, 64 bots, Number of Bots, map changes, the sound and graphics
fixes in it. The window suite (tools/run_window_tests.py, the 27 default scenarios plus vk_scoreboard_169,
vk_bot_count_169 and vk_player_collision_169) 30 of 30: 65 players (32 CT, 33 T), Player Collision on and off,
scoreboard scrolling over 65 with BOT labels; tools/check_scoreboard.py on vk_scoreboard_169 passes once it checks the
header counts only while the board is open (97ed5bf: the hidden panel's counts are recounted when it opens, as in the
original). Descriptors stay flat across map changes (16 after a relaunch, 24 before 0bfcdfb); no game left running
after any run.

### 15.3 Measurements (headless, Linux, 16 threads, one machine)

Every client here is a full headless game process (simulation, prediction, the reference renderer's null path): no
protocol-only load generator exists yet (15.1), so "load" means real gameplay clients, at most 8 at once on this
machine. Traffic is the transport's (headers, tags and fragmentation included), from CSX_LOG_NET.

| Run | Server out | Server in | Fragmented | Largest message | Loss / replays / auth / malformed | Server frame | SV_SendClientMessages |
|---|---|---|---|---|---|---|---|
| net_load_8 (9 humans, no bots) | 14.5 KB/s (peak 27.6), 1.8 KB/s a client, 142 datagrams/s | 13.5 KB/s, 1.7 a client | 8 in 92 s | 1170 B | 0 / 0 / 0 / 0 | 0.97 ms mean, 2.12 max | 0.55 ms mean, 10.4 max |
| net_full_65 (1 remote, 63 bots) | 7.5 KB/s (peak 25.2) | 1.8 KB/s | 18 in 74 s | 5651 B | 0 / 0 / 0 / 0 | (not logged) | 0.21 ms mean, 1.1 max |
| net_full_65_2remote (2 remote, 62 bots) | 9.9 KB/s (peak 48.3), 4.9 a client | 3.8 KB/s | 39 in 218 s | 7061 B | 0 / 0 / 0 / 0 | 8.84 ms mean, 13.6 max | 0.26 ms mean, 1.7 max |

A remote client in the 65-player match sends 2.0 KB/s (30 commands a second) and receives 5.4-7.4 KB/s on average,
25 KB/s at peaks (round restarts); about 17-21 server messages a second arrive against cl_updaterate 30 (15.5).
Processes (resources.csv): the host 26% of a core with 8 clients, 48% with 62 bots and 2 remote players; each client
13-25%; memory 122-125 MB for a host, 105-117 MB for a client; 15 threads; descriptors 13-16, flat over every run,
also across the map change's relaunch (SDK patch 0042).

### 15.4 Security review (2026-10-25)

An independent review of src/net and the engine checks found eight defects; each was traced in the code, fixed, and
covered by a test that fails before the fix (tests/net_transport_test.py 11 new checks, tests/stuff_filter_test.py):
a use after free when a client reconnected while its old connection was closing; a closing connection kept alive by
its peer's packets and answered under an erased key; unauthenticated rejects; a rate table scanned on every hello
once full (spoofed floods); identity.key world-readable; a server able to run any command on its clients (stufftext
filter split differently from Cbuf_Execute); a client able to kick or pause through a parser difference; the
sodium_init result ignored. Verified correct: the libsodium use (crypto_kx keys per direction, low-order keys
refused, return values checked), nonces (packet numbers from 1, fresh keys per connection), the tag checked before any
use of the plaintext, the replay window, the cookie, bounds on every header and fragment, no amplification, no session
secret in logs or replies.

Remaining risks, by severity:

1. **No server authentication**: the server's key exchange key is new every launch and nothing pins it, so an
   on-path attacker can impersonate a server (and relay to the real one: rcon passwords, chat). Fine on a LAN or
   between friends on a trusted path; for the internet it needs server keys that persist and are pinned (trust on
   first use) or published (N2 / N3, with the browser).
2. **A malicious server** still runs the allowed commands (say, retry, slot changes...) and sends any game data the
   client parses: the engine's client-side parsers were not hardened beyond G8.2B's packet checks.
3. **Floods**: the global handshake budget keeps a flood from many spoofed sources cheap, but also lets it delay real
   clients while it lasts (the budget refills in seconds); no per-connection bandwidth cap beyond the engine's rate.
4. **Identity** proves only the same install (64-bit fingerprint in *pid); ban and admin lists should store the full
   key (N2).
5. **Rcon**: the challenge is checked, the password is the engine's (plain text over the encrypted link: safe from
   eavesdroppers off the path, not from item 1).
6. Minor: a re-sent accept reuses the nonce 0 Poly1305 key with another associated data (only nonce-0 accepts of
   that connection could be forged); a pre-challenge version reject is not authenticated (it can only end a handshake
   that has not been challenged yet).

Verdict: suitable for LAN play and play between friends over a trusted path (direct connect, optional identity); not
for open internet exposure until item 1 is solved, the client-side parsers are reviewed, and a dedicated server's
admin model exists (N2).

### 15.5 Known limitations

- Map changes relaunch the server and every client (open question 1 unchanged); a client in a process that already
  joined relaunches to join again.
- A listen server's own player takes a slot: 65 players are the host, remote humans and bots (2 remote humans need 62
  bots; 63 bots with 2 remote humans needs N2).
- Snapshot rate to a client in a 65-player match was ~17 messages a second against cl_updaterate 30 (section 15.3):
  the engine's rate and choke logic, not tuned in N1.
- A game ended by a signal (SIGINT from a test, Ctrl+C) leaves its server to time it out (20 s): the exit path that
  tells the peers runs on a normal exit (quit, closing the window) only.
- No LAN discovery: System Link's search finds nothing; joining is by address.

### 15.6 What N2 (dedicated server) depends on

ca_dedicated without a client in the title's boot (section 7); a server that does not relaunch per map, or relaunches
with its clients (as now); server keys that persist (15.4 item 1); admin rights for kick / pause / rcon instead of
"host only"; a fixed tick that sleeps; logging and config files.

### 15.8 N1 sign-off (2026-10-25)

The headless suite (tests/run_regression.sh, 46 scenarios) was run twice in full after the lag-compensation scenario
fix (b29b171), at 1a3081e:
- first run 44 of 46 while other network runs shared the machine: regress_pc_bindings (the buy menu opened 60 s late,
  so the pause-menu presses after it landed elsewhere) and regress_player_collision_on (the player stood on a bot held
  half-crouched, -201.9, outside the standing-head range);
- second run, nothing else running, 45 of 46: regress_decals (the walk to the sign ended 32 units short: the sign
  left the sampled box).
Each of the three passed every rerun alone (bindings 4 of 4, collision 5 of 5, decals 2 of 2 after the change). None
is a game regression: two were the scenarios' own assumptions, now removed (regress_decals puts the player on the
spot the pictures are taken from after its walk; regress_player_collision_on accepts a player on a crouched or
standing bot's head, -217..-178, still apart from the floor at -252); regress_pc_bindings has no fix (its log was
overwritten; it presses through the menus by count and is load sensitive).

65 slots, reconciled: net_full_65_2remote fills all 65 with the host's own player (the listen server's, userid 1),
2 remote humans (userids 2, 3) and 62 bots: 3 + 62 = 65 occupied, checked in the run (players: 65 of 65 before and
after the restarts, 3 human and 62 BOT "joined team" lines, the 2 remote pings, scoreboards of 65 rows). "2 remote +
62 bots = 64" left out the host's player. Two remote humans with 63 bots needs a server without its own player (N2).

### 15.7 Commits (branch g8.2b)

| Commit | What |
|---|---|
| df2bb0b | Build: libsodium |
| 998e639 | Transport (src/net), unit test, fuzzer |
| ea979d5 | The engine on the transport; SDK patch 0041 |
| 39acee2 | Multi-process network scenarios; run_game.sh --host / --connect |
| 612eb8e | Challenges in the server's first second; a second connect relaunches |
| 67d0078 | Runner verdicts, fresh run folders |
| 44d5e37 | Transport security review fixes (transport version 2) |
| e364175 | Stufftext and client-command checks parse as the engine does |
| 0bfcdfb | SDK patch 0042: a relaunch closes every descriptor |
| 44005a8 | No orphaned games; resources per process; scenario timing |
| b29b171 | regress_lagcomp_last_slot brings slot 65's bot in view |
| 97ed5bf | check_scoreboard: header counts while the board is open |

Nothing is merged into main.


## 16. N2 as built: the dedicated server (2026-10-25)

Approved by the user 2026-10-25 after the N1 sign-off (15.8). Branch g8.2b, not merged.

### 16.1 Architecture

csx-server is a second executable built from the same recompiled image (tools/build_server.sh: the harness build
without the window, Vulkan, the reference renderer or the test pad; src/server for its main, its configuration and
its host stand-ins). It links libc, libstdc++, libm and libsodium only: no SDL, no X11 or Wayland, no Vulkan, no audio
library, no input device.

- **The engine as GoldSrc's dedicated server**, a mode this title never used: CEngine::Load's flag is 1
  (src/game/dedicated.cpp DedicatedLoad), so cls.state stays ca_dedicated and Host_Init skips the palette, video,
  drawing, sound, voice and client. No slot is a local player's: all max_players (up to 65) are for remote players and
  bots. The boot fixes it took are in FINDINGS "PC multiplayer N2" (the game code's client part initialised and
  marked not loaded, the name cvar, LostHost, Host_Shutdown's launcher export, the exit).
- **What the title still runs**: its Xbox UI per-frame calls with nothing to show, a D3D device on a backend that
  keeps and draws nothing (src/server/server_host.cpp), the APU clock and DirectSound mixing to no device; pads read
  as unplugged.
- **The match** is the title's own: the front end's launch page and a command line (Z:\releasemap.txt) that csx-server
  writes from its settings (server_config.cpp CommandLine), as the console's Dedicated Server match option started
  one (mp_dedicated_server 1, plus bot_join_after_player 0 so bots play with no human). No Xbox lobby, no XNet
  session, no system link.
- **The transport** is N1's (src/net) with the server's identity (16.4); the server endpoint is the only one.
- **The tick**: a fixed 30 Hz host frame that sleeps to its deadline (dedicated.cpp DedicatedTick, 16.6).
- **Map changes** relaunch the process, as on a listen server: the title writes "+map NAME" and restarts into its
  image; csx-server comes back without arguments (SDK patch 0043: the running program, whatever its name), takes its
  settings from CSX_SERVER_SETTINGS (left in the environment at the first start) and the map from the title's line.
  Clients are told (MapChange) and rejoin by themselves.
- **Lifecycle**: start (settings checked, the emulated disk rebuilt, the launch page and line written, the identity
  loaded or made), map load, match (bots join at once), connections, rounds and map changes, shutdown by quit on its
  console, SIGINT or SIGTERM (the players are told "server shut down"), exit 0 when the title's main returns. A
  watchdog logs every thread's stack if host frames stop for 15 s (the recompiled functions are named by their guest
  address), which is how the boot was debugged.

### 16.2 Starting a server, joining it

    tools/iterate.sh 1 && tools/build_server.sh          # or ./run_server.sh --build ...
    ./run_server.sh --map de_dust2 --bots 20 --player-collision off --name "Friday night"
    ./run_server.sh --config config/server.cfg.example --port 27016
    build/server/csx-server --game GAME_DIR [--config FILE] [--KEY VALUE]...   # from the folder that keeps its state

run_server.sh runs it in build/server-run with build/gamedata. The folder keeps server_identity.key (owner-only),
admins.cfg, bans.txt, logs/ (server_NNN.log per run, admin.log), hdd/ (rebuilt each start). Its console is the
terminal: engine commands (status, say, sv_restartround), quit. Map changes come from the match's end (time_limit, max_rounds) or an admin's "admin map"; changelevel is disabled in this title and a "map" typed on the server's console was not tested.

Players join with `./run_game.sh --connect HOST[:PORT]` (add `--password P` for a passworded server and
`--server-key KEY` to pin the key instead of trusting it on first use). On the same PC: `--connect 127.0.0.1`.
Over a network: open UDP 27015 (or --port) on the server's firewall; set lan_only on to refuse addresses outside
private networks. There is no browser yet (N3): the address is typed.

### 16.3 Configuration

A file of "key value" lines (# comments; config/server.cfg.example), command-line `--key value` (or `--key-name
value`) after it overriding; every value is checked before anything starts and errors are listed with their line
(tests/server_config_test.py, 21 checks):

| Key | Default | Range / meaning |
|---|---|---|
| name | Counter-Strike Xbox PC server | hostname, 1-63 printable characters, no `"` `;` `\` |
| port | 27015 | UDP, 1-65535 |
| map | de_dust2 | a map in the game folder (checked) |
| max_players | 65 | 1-65 player slots for remote players and bots; the server takes none |
| bots | 0 | 0-max_players (bots join with no human present) |
| bot_difficulty | 1 | 0 easy, 1 normal, 2 hard, 3 expert |
| player_collision | on | off: players and bots pass through each other (crowded matches) |
| friendly_fire | off | on/off |
| round_time | 3 | minutes, 1-9 |
| freeze_time | 3 | seconds, 0-60 |
| buy_time | 1.5 | minutes, 0.25-9 |
| start_money | 800 | 800-16000 |
| max_rounds | 0 | rounds before the map ends, 0 none |
| time_limit | 30 | minutes before the map ends, 0 none |
| password | (none) | up to 32 characters, no spaces; logged only as "(set)" |
| lan_only | off | on: only 127/8, 10/8, 172.16/12, 192.168/16, 169.254/16 |
| snapshot_rate | 30 | sv_maxupdaterate, 10-100; the tick is 30, so more sends no more |

Nothing unsafe is reachable by default: no rcon (16.5), no administration without admins.cfg, no setting that runs
commands (names and passwords cannot hold the characters the engine's parser splits on).

### 16.4 Server identity and trust

- The server has an Ed25519 identity (server_identity.key, "CSXNID1" plus libsodium's 64-byte secret key, 0600,
  made on the first start, written whole or not at all; a player's identity.key is the same format). Its fingerprint
  is the whole public key (64 hex digits), logged at start ("net: server identity ...").
- Transport version 3 (c5fdc04): every accept carries the server's identity key and its signature over
  "CSXN server v1", both key exchange keys, the client's nonce and the connection id, inside the accept's AEAD
  associated data. The client verifies the signature (crypto_sign_verify_detached) before using the connection: a
  server without the key cannot finish a handshake as that server; a server without any identity accepts no one.
  No custom primitives: libsodium's crypto_kx, ChaCha20-Poly1305 (IETF), Ed25519.
- The client's trust: a pinned key (CSX_SERVER_KEY or PC setting net_server_key; run_game.sh --server-key) must
  match; otherwise trust on first use, known_servers.txt beside the player ("ADDRESS KEY" lines, 0600): the first
  connection remembers the key, a later one must match. A changed key is refused (ServerIdentityChanged) with both
  keys in the message and how to accept it (remove the line), never taken silently; PC setting net_tofu 0 refuses
  servers not known at all.
- Player and server identities are separate keys and files. The server sees each player's key (N1's optional player
  identity, now the full key, logged at connect, no longer put in userinfo); admins.cfg and bans.txt name players by
  it. No account service, no central infrastructure.
- Limits: TOFU trusts the first connection (pin the key for a first join over an untrusted network); a server's key
  is bound to the address the player typed, so one server behind two addresses is two entries; keys do not expire
  or rotate (a new key is a refused "changed" key until the line is removed).

### 16.5 Remote administration

- **Off by default.** Without admins.cfg in the server's folder no one is an admin; every attempt is refused.
- **Who**: admins.cfg lines "PLAYER_KEY PERMISSIONS" (PERMISSIONS "all" or a comma list of status, say, kick, ban,
  map, restart, bots, settings); reloaded when it changes. Authentication is the player's transport identity: the
  admin command arrives over that player's encrypted, authenticated connection, so there is no password to send,
  replay or guess.
- **How**: the client command `admin COMMAND ...` (typed in the console). Commands: help, status (slots, names,
  addresses, keys), say, kick SLOT [REASON], ban SLOT [MINUTES] (the player's key, or its address without one; bans.txt,
  checked at every connection), unban, map NAME (relaunches into a map of the game folder), restart [SECONDS],
  bots N and bot_difficulty N (from the next map: the title sets bots at a match's start), friendly_fire, round_time,
  password. Each is parsed and checked here, never handed to the engine's console as text; kicks run at the next tick.
- **Logged**: every command and refusal in the run log and logs/admin.log, with name, address and key (a refused
  "admin password ..." is logged without its argument).
- **Rate limited**: refusals over 5 a minute from one address are dropped unanswered (logged once a minute).
- **rcon is refused** from the network whatever its password (its password travels in plaintext in a connectionless
  packet); every refusal is logged. The listen server's own console keeps the engine's local commands.
- Not done: a separate admin protocol or tool (the in-game console is the interface), per-command arguments limits
  beyond the ones listed, two-person rules, audit log rotation.

### 16.6 Timing: simulation and snapshots

- **Why 17-21 snapshots a second** (N1): SV_SendClientMessages sends to a client when realtime + host_frametime
  reaches its next_messagetime, then sets it to realtime + host_frametime + 1/rate. With a 30 Hz tick and a 30 Hz
  rate the next frame lands exactly on the deadline, and the frame time's jitter decides whether it sends: about
  every other frame was skipped. GoldSrc's own servers ran 100+ frames a second, where this costs nothing.
- **Now**: the deadline advances by the interval from the previous deadline (src/game/server_tick.cpp, hooks at
  0x9E5AD/0x9E5D0 and the choke path 0x9E61E/0x9E655); a schedule more than an interval behind restarts from now
  (no bursts). This applies to the listen server too.
- **Simulation**: the host frame runs at a fixed 30 Hz on the dedicated server (dedicated.cpp: a deadline schedule
  with sleep_until, moved on rather than caught up after a long frame), not tied to rendering (there is none) or to
  the CPU's speed; game time advances by the measured frame time as GoldSrc does. Snapshots are scheduled separately
  per client by its rate (cl_updaterate up to sv_maxupdaterate, default 30) and its byte rate (rate, 100000 by
  default on PC clients): with 65 players a snapshot is up to 6 KB, so a client's byte rate, not the tick, holds some
  back at peaks (28.4 a second on average in net_ded_full_65).
- **Not changed**: the gameplay simulation rate (30 Hz, the console's). 60 or 128 Hz would need: the host tick
  constant raised (one line), the server frame to fit (7.8 ms mean, 17 ms peak with 63 bots and 2 players now: 128 Hz
  has 7.8 ms, so the bots' cost must drop first: TODO 1e2's native hull trace, spreading their visibility scans);
  the game DLL's timers checked for frame-count assumptions (the Xbox build was tuned at 30); bandwidth doubled or
  more per client (rate 100000 already limits a 65-player match), cl_updaterate and cl_cmdrate defaults raised;
  lag compensation's history length (sv_maxunlag, the frame backup count) rechecked; prediction checked against the
  shorter server steps (the client's movement already runs per command, at 60 commands a second).

### 16.7 Measurements (headless, Linux, one machine, 16 threads)

| Run | Server CPU (mean / peak of one core) | Memory | Server frame | Snapshots a second per client | Traffic |
|---|---|---|---|---|---|
| net_ded_one (1 client) | 2% / 2% | 89 MB | - | 30.0 | 2.2 KB/s out |
| net_ded_two_bots (2 clients, 4 bots) | 3% / 5% | 102 MB | - | 29.9 (19-31) | - |
| net_ded_load_8 (8 clients) | 4% / 5% | 100 MB | 0.86 ms mean, 1.47 max | 30.0 (21-31) | 21.1 KB/s out (peak 32.9), 2.6 KB/s a client; 13.6 KB/s in |
| net_ded_lossy (2 clients, 4 bots, 60 ms, 3% loss) | - | - | - | 28.9 | 6.0 KB/s out; 74 lost, 25 replays rejected |
| net_ded_full_65 (2 clients, 63 bots) | 23% / 39% | 101 MB | 7.81 ms mean, 16.96 max | 28.4 (21-31) | 11.9 KB/s out (peak 51.5), 6.0 KB/s a client; 73 fragmented, largest 5916 B |

Compared with the listen server (15.3): the 65-player listen host used 48% of a core and 122-125 MB with its own
client and renderer; the dedicated server 23% and 101 MB for the same match. File descriptors stay at 11 through
every run, map change included. Each client process (full game, headless) uses 16-25% of a core and 106-118 MB.

### 16.8 Tests

`python3 -I tools/run_net_tests.py tests/net/net_ded_*.txt` (the dedicated server: build/server/csx-server; a section
with `--server "OPTIONS"` runs it, `--file NAME=PATH` copies a fixture into its folder). Each is a set of real
processes in one network namespace, 127.0.0.k per process; the runner allows nine now (a server and 8 clients).

| Scenario | What it shows |
|---|---|
| net_ded_one | one client; the server has no player (playercount 1); snapshots 30 a second; quit exits 0 |
| net_ded_two_bots | 2 clients and 4 bots, Collision Off reaching the clients, round restarts from the server's console, scoreboard of 6 with pings and BOT |
| net_ded_load_8 | 8 clients moving for a minute, 8 of 65 slots occupied, all with pings |
| net_ded_full_65 | 2 clients and 63 bots: 65 occupied (2 humans, no host slot), kills, 3 round ends, fragmented 6 KB snapshots, both scoreboards 65 rows (check_scoreboard: all checks) |
| net_ded_bots_65 | 65 bots, no human: the match runs, rounds end |
| net_ded_lifecycle | disconnect, a rejoin in the freed slot, a frozen client timed out, the server stopped by SIGINT telling the last client |
| net_ded_map_change | the match's end relaunches the server into the next map with its settings and identity; the client rejoins with its remembered key |
| net_ded_admin_map | an admin's bots 3 and map de_dust2: the server comes back on de_dust2 with 3 bots, the admin rejoins |
| net_ded_trust | password refused / accepted, a changed server key refused, a pinned key accepted; the password never in a log |
| net_ded_admin | admin allowed (status, bots), refused (map without permission; a player not in admins.cfg), rate limited, kick; rcon refused and logged; admin.log |
| net_ded_malformed | tools/net_fuzz.py against the server, then a client plays |
| net_ded_lossy | 2 clients and 4 bots under 60 ms ± 15 ms, 3% loss, 1% duplicates, 5% reordering |
| net_ded_restart | three servers in a row on the same identity, each starts, runs and quits cleanly |

Configured capacity and occupancy are checked apart: the server logs max_players at start ("65 player slots"), the
players step counts the active client_t entries, playerping lines count the network ones (bots have none).
Unit tests: tests/net_transport_test.py 109 checks (TestServerIdentity: a signed accept, a forged signature refused,
a changed key refused, TOFU remembered; TestLanOnly; TestDisconnectDropsQueued), tests/server_config_test.py 21.

### 16.9 Known limitations

- **Bots are set at a match's start** (the title's rule, also on a listen server): bot_quota or bot_add later does
  not stick, so admin bots N applies from the next map.
- **Map changes relaunch** the server process (as N1's listen server); a few seconds without a server, clients rejoin
  by themselves.
- **The client side against a hostile server** is N1's (15.4 item 2): the engine's client parsers were not hardened
  further. Server authentication (16.4) now stops impersonation by anyone without the server's key.
- **Linux only so far**: the game-side code (dedicated.cpp, server_tick.cpp, server_admin.cpp, the transport) is
  portable; csx-server's main uses a few POSIX calls a Windows target would replace (setenv, sigaction, poll on stdin,
  the watchdog's backtrace), and the Windows relaunch keeps the command line, which it would need to account for. No
  Windows target is built.
- **Admin interface** is the in-game console's admin command; there is no stand-alone admin tool. admin.log grows
  without rotation.
- **Load**: at most 8 remote clients tested at once (each is a full game process here); 65-slot matches were tested
  with 2 humans and 63 bots. A protocol-only client for 64 remote humans does not exist yet.
- **No server query** for browsers or LAN discovery (N3).
- A **game client** ended by a signal still does not tell its server (N1 15.5); the server times it out (20 s).

### 16.10 What N3 (server browser) depends on

A connectionless server query answering name, map, players / max_players, bots, password yes/no, the identity key
(so a browser entry can be pinned before joining), LAN-only; LAN discovery by broadcast on 27015 and/or a configurable
list of master servers (optional, not a requirement: a user-hosted list); the browser's place in the title's System
Link screens (section 9); rate limits for the query (it is unauthenticated by nature); the password prompt in the
title's UI (now only CSX_NET_PASSWORD / run_game.sh --password).

### 16.11 Results on the final build (2026-10-26, at 10714d3)

- Network scenarios (tools/run_net_tests.py, all of tests/net): 24 of 24 (the 12 of N1 on a listen server, the 12
  dedicated), then net_ded_admin_map and net_ded_map_change again after 10714d3: pass. The listen server's 65-player
  match (net_full_65_2remote) now gets 29.8 snapshots a second per remote client (17-21 in N1).
- Unit tests: net_transport_test 109 checks, server_config_test 21, stuff_filter_test 14, net_buffers_test 18,314:
  all pass.
- Headless offline regression (tests/run_regression.sh): 46 of 46 (offline matches, 64 bots, Number of Bots, the 65
  slots, Player Collision on and off, scoreboard, the graphics and sound fixes).
- Window suite (tools/run_window_tests.py, the 27 default scenarios plus vk_scoreboard_169, vk_bot_count_169,
  vk_player_collision_169): 29 of 30 in the run, vk_player_collision_169 short of one "client 0's move" line in its
  second match (logged only when another body is near the player, which depends on where everyone is); it passed
  alone straight after (161 steps). Scoreboard over 65 with BOT labels and pings, the bot count to 64: pass.

### 16.12 Commits (branch g8.2b)

| Commit | What |
|---|---|
| c8e75de | N1 sign-off: regression results, 65 slots reconciled, two scenarios' assumptions removed |
| c5fdc04 | Transport v3: signed accept, persistent server identity, known servers, LAN-only |
| d68ff27 | csx-server, dedicated boot, tick, snapshot schedule, identity in use, administration, password; SDK patch 0043 |
| d9fdaf1 | Dedicated scenarios; runner --server, --file, a ninth process |
| 10714d3 | A relaunch's map ends at its NUL (admin map); net_ded_admin_map |
| (this) | Docs: N2 (this section, FINDINGS, PROGRESS, ROADMAP, TODO, networking.md) |

## 17. N3 as built: the server browser (2026-10-26)

Approved by the user 2026-10-26 after N2. Branch g8.2b, not merged. No split-screen (N4) work.

### 17.1 Architecture

```
 menus (the title's own UI code)            src/game/pc_browser.cpp: menu text added at load, the list's rows made
   Multiplayer > LAN / Internet / Favorites    with the game's row and cell functions, BackgroundText lines set,
   / Recent / Join by Address / Host           console commands "!pc_browser ..." from the screens' buttons
 ────────────────────────────────────────
 discovery thread                           src/net/browser.h: LAN broadcast, master lists, saved and typed
   (its own UDP socket, poll-woken)            addresses; queries paced and retried; answers checked; a view
                                               sorted and filtered for the menus
 ────────────────────────────────────────
 server side (listen and dedicated)         src/game/server_query.cpp: info and players answers on the game port
   on the game port's socket                   (src/net/query.h), master registration (src/net/master.h), a LAN
                                               listener beside a server bound to one address
 ────────────────────────────────────────
 master server (optional, self-hosted)      tools/master/master_main.cpp csx-master: registrations, expiry, lists
```

- **The query** (src/net/query.h) travels on the server's game port, in the transport's datagrams but outside any
  connection: transport.h Type 7 (query) and 8 (answer); the transport hands these to a handler
  (Endpoint::SetConnectionless) instead of the connection code. A query is padded to the size of the largest answer
  of its kind (info 256 bytes, players a full 1200-byte datagram) and only a query of exactly that size is answered,
  so a server never sends more than it received (no amplification of a spoofed source). Answers carry the client's
  random nonce and are signed (Ed25519) by the server's N2 identity over "CSXN info v1" and everything before the
  signature; the info answer carries the identity key. Info: name, map, game, engine protocol (47), transport
  version, players (humans), bots, slots, flags (password, dedicated, LAN only, Player Collision off, friendly fire),
  round time, time limit, max rounds, the key. Players (pages of up to 1200 bytes): name, score, ping, connected time,
  bot. Answers are rate limited: 8 a second per source address (a burst of 16), 400 a second in total; a LAN-only
  server answers private addresses only. Nothing secret is in an answer: no password (a flag), no key but the public
  identity, no address.
- **The server side** (src/game/server_query.cpp) builds the answer from the engine once a second on the game thread
  (never per query): active client_t entries split by fakeclient into players and bots (a dedicated server has no
  host player to count), maxclients, sv.name, sv_password, Player Collision, mp_friendlyfire / roundtime / timelimit /
  maxrounds by Cvar_FindVar; scores from the edicts' frags (edict + 0x1E4, as SVC_PlayerInfo 0x9AFB0 reads them), pings
  from the client's latency, connected time from its netchan. The name: this build registers no "hostname" cvar, so
  csx-server's name setting, else a listen server's PC setting net_server_name, else "<host player>'s match". The
  dedicated server waits for its next tick on its socket (ServerQueryWait), so an answer leaves when the query
  arrives; a listen server answers at its next frame (its ping in the list includes up to one frame).
- **LAN discovery**: the browser broadcasts an info query to every IPv4 interface's broadcast address, 127.255.255.255
  (this machine) and 255.255.255.255, on each port of net_lan_ports (27015-27020 and the player's own net_port). A
  server bound to all addresses (the normal case) receives it on its game socket; a server bound to one address (the
  tests' numbered instances, CSX_NET_BIND) opens a LAN listener on all addresses beside it (SO_REUSEADDR on both) and
  answers from its own socket, so the list shows the address the game connects to. While the LAN tab is open the
  broadcast repeats every 3 s; a server silent for 10 s leaves the list; the same server answering at two addresses is
  listed once (its key).
- **Master server** (src/net/master.h; tools/master/master_main.cpp, `tools/build_master.sh`): a server that names
  masters (csx-server `master HOST[:PORT],...`; a listen server's PC settings net_master_register 1 and net_master)
  sends a heartbeat from its game socket; the master answers with a cookie (HMAC-SHA256 under its random key of the
  source address, the nonce and a time, valid 30 s); the server registers with the cookie, its identity key and an
  Ed25519 signature over "CSXN master v1" and the packet. Only a source that receives at its address can register it
  (no registering someone else's address); the key is the server's own. The master lists the source address and key
  until heartbeats stop for its expiry (180 s, heartbeat 60 s, both settable); a server that quits sends a signed
  leave with its last cookie. Limits: 20000 servers, 32 an address, 10 datagrams a second an address (a burst of 20),
  2000 in total, `--public` to refuse private addresses. Browsers ask for pages of 30 (address, port, key) with a
  request padded to 1200 bytes. A LAN-only server registers with no master. No master is set by default; a master is
  an ordinary program anyone can run (no accounts, no Steam, no central service).
- **A listing is not trust**: the browser queries every listed server itself and drops one whose signed answer
  carries another key than the master listed (it is not that server); joining still goes through N2's checks.
- **Discovery thread** (src/net/browser.h, the game's src/game/pc_browser.cpp ThreadMain): its own UDP socket
  (broadcast allowed), polled with a 5 ms wait, so a ping is measured when the answer arrives, not at a frame; queries
  paced at 200 a second (a burst of 10), 64 unanswered at most, one retry after 1 s, then "no answer"; at most 4096
  servers and 137 master pages (4110 servers). The game thread copies the entries when they changed.

### 17.2 The menus

The original System Link menus are the base; no other UI framework, no overlay, nothing drawn by host code:

- **Multiplayer** (System Link renamed on the main menu and its screen, its help line "Play with others on your network
  or the internet."): LAN Matches, Internet Matches, Favorites, Recent Servers, Join by Address, Host a Match (the
  console's own, now hosting a PC listen server: CSX_MENU_HOST set by the relaunch from its screens, net_session.cpp).
- **The server list** (SuperLauncher_PCBrowser): the console's Xbox Live OptiMatch Sessions screen (its background
  Back_XBL_OptimatchResults with four column dividers, its highlight, table text scale and detail lines) with the
  System Link list's button bar (Y Host a Match, X Refresh Results, A Join, B Cancel). Columns Server Name (a "* "
  before a passworded server, "! " before one whose identity changed), Players (all players and bots / slots, up to
  65/65), Map, Ping. Tabs LAN / Internet / Favorites / Recent above the list (the current one dark, in brackets), a
  status (searching, how many, no master set), eight detail lines (game type from the map, players "2 + 63 bots of
  65", dedicated, password, round time and map time, Player Collision and friendly fire, address and LAN only, ping or
  the identity warning), the action lines Sort / Filters / Add to Favorites (Remove Favorite).
- **Navigation**: the pad as on the console (up / down, A join, B back, X refresh, Y host) plus LT / RT the tabs,
  Black the next sort order, White add or remove the favorite, Back the filters, left / right (d-pad or stick) the
  tabs; the keyboard as the menus' pad (arrows, Space or Enter join, R refresh) plus, on these screens only, Escape
  back, F5 refresh, Page Up / Page Down the tabs (src/linux/window_input.cpp); the mouse: hover selects, a click
  selects a server (a double click joins), a click on a column header sorts by it (again: reversed), the tabs and the
  action lines are clickable, the button bar's four buttons too (pc_mouse.cpp).
- **Sorting and filters**: Ping (unanswered last), Players (the fullest first), Server Name, Map; Normal / Reversed;
  stable (ties by name, then address). Filters (SuperLauncher_PCFilters, an options screen like PC Options: A saves, B
  cancels, X resets): Full Servers, Empty Servers, Password Protected (show / hide / only), Map (any or one of the
  title's maps), Location (Favorites and Recent: any / LAN / internet). Saved as PC settings browser_*.
- **Stable list**: the rows are rebuilt at most four times a second while answers arrive and keep their order when only
  pings change (a refresh, a sort or a change of the servers listed sorts again); the selection stays on the same
  server, or on the top row until the player moves it.
- **Join by Address** and **the password**: the profile name's keyboard screen (its 15-character limit raised to 63
  and 32 for these screens: hook 0xD04CD); the PC keyboard types into it at once (pc_mouse.cpp); the last address is
  offered again. The password is shown as typed (the title's edit box has no masking); it is never logged.
- **Messages** (SuperLauncher_PCMessage, as the console's HostFull / LostHost boxes), **Server Identity**
  (SuperLauncher_PCIdentity, Cancel / Accept), **Contacting** (Join by Address while its server answers, Cancel).
- **Widescreen**: the menus' 640x480 layout is the title's, centred at 16:9 and 21:9 as every menu (G7).

### 17.3 Joining, and back

1. The selected server's answer is checked: no answer ("does not answer", with the port to check), another protocol,
   full ("The server is full. 4 of 4 player slots are taken."): messages, nothing sent.
2. Its proven key against a pinned key (net_server_key: must match, else refused) and known_servers.txt (TOFU): a
   different remembered key, or a favorite's, opens Server Identity with both keys: B leaves it, A trusts the new key
   explicitly (known_servers.txt and the favorite rewritten). Never silently.
3. A passworded server asks for the password.
4. The key is remembered (TOFU, as a connection would), the server becomes a recent server, and the title relaunches
   into `+connect ADDRESS` as its own system link join did (net_session.cpp RequestJoin), with CSX_SERVER_KEY set to
   the key the list proved (the game connection must reach the same server), CSX_NET_PASSWORD (inside the encrypted
   connection, as N2's), CSX_BROWSER_JOIN.
5. A join that ends (refused password, banned, kicked, the server shut down, lost) does not show the console's
   LostHost box: the client relaunches to the front end (launch data 1, as the title's own end of a session), and the
   front end opens Multiplayer, the browser on the same tab and the message: "The password is not the server's.",
   "Disconnected: Server shutting down", "You are banned from this server." (from the server's printed refusal or
   the transport's close reason). Quitting a joined match from its menu also comes back to the browser.

### 17.4 Favorites and recent servers

PC settings (pc_settings.ini) `favorite.1` .. `favorite.64` and `recent.1` .. `recent.16`: "ADDRESS KEY NAME" (the
address as typed or listed, a host name allowed; KEY the 64 hex digits of the identity the server proved when it was
added, "-" if unknown; the name as last seen, cleaned). Added and removed from the list (White, the action line), on
the Favorites tab (queried directly, listed even when they do not answer, refreshed with X). A favorite is its key:
an answer at its address with another key is shown with "!" and joining asks (17.3 step 2); the favorite's key proven
at another address (LAN or a master) moves the favorite there (logged; the signature proves the same server). Recent
servers are added at each join, newest first.

### 17.5 Security

- Server authentication is N2's: TOFU or pinned keys, a changed key never taken silently; the browser checks the key
  before relaunching (its own signed answer) and pins it for the connection, which verifies it again in the handshake.
- Untrusted input audited (answers, master pages, LAN broadcasts from anyone): fixed-size bounded reads (wire.h
  Reader), a nonce that must be the browser's (a spoofer off the path cannot answer), the signature checked before
  use, counts that cannot be (players over slots, no slots, pages out of range) refused, strings cut to their bounds
  (63 name, 31 map, 15 game, 31 player name) and cleaned to printable ASCII without the characters the engine's
  command and info string parsers or the menus' localisation treat specially (`" ; \ % #`), entries capped (4096),
  master pages capped (137 x 30). Server text only ever becomes cell and line text (std::wstring assigned, no
  localisation lookup); every menu command is the browser's own, so a server's name cannot inject a command. Typed
  addresses are limited to letters, digits, '.', '-', ':'; passwords to what the engine's userinfo carries.
- The master server cannot be used to register another address (the cookie) or another server's identity (the
  signature); its list is not trusted (17.1). A hostile master can make browsers query up to 4110 addresses (256-byte
  queries, paced, once with a retry: about 2 MB per refresh), a bounded nuisance, documented.
- Rate limits: the query (8/s per source, 400/s total), the master (10/s per source, 2000/s total), the browser's own
  pacing (200 queries a second).
- **Not yet secure for the public internet**: the engine's client message parsers (svc_ messages, delta decoding,
  user messages from the game DLL) still trust the server they connected to beyond N1's index and buffer checks
  (15.4 item 2); a malicious server a player chooses to join could exploit a parser bug. The browser does not change
  that; it only decides which server is joined and proves it is the expected one.
- Fixed during N3: the menu's keyboard logged the typed text when typing ended (pc_mouse.cpp): never on the password
  screen now.

### 17.6 Profile walk fix (found by N3)

The title's profile save and load (stuffUItoPlayerData 0xDAD40 and its two counterparts) walk every screen's list rows
and keep one byte a row in a 128-byte block (on the stack at the save). The PC screens' lists (PC Options, PC
Controls) already used most of it; the browser's Filters screen ran past it and the stack cookie check ended the title
when the main menu loaded. PC screens are now skipped by these walks as the title skips PlayerMapVoting (hooks
0xDADC5, 0xDB809, 0xDB925; src/game/pc_menu.cpp PcProfileSkipScreen): their values are PC settings, and since they
come after every original screen the original rows keep their bytes.

### 17.7 Hosting and joining

    ./run_game.sh                                         # Multiplayer > LAN Matches / Internet Matches / Favorites ...
    ./run_server.sh --map de_dust2 --bots 20              # a dedicated server: found on the LAN with no setting
    ./run_server.sh --map de_dust2 --master master.example.org:27010   # also listed by that master server
    tools/build_master.sh && build/master/csx-master --port 27010 [--public]   # run a master server (UDP 27010)
    build/master/csx-query --lan [--players]              # what the browser sees, from the command line
    build/master/csx-query --master HOST[:PORT]           # a master's list, every server queried
    build/master/csx-query 192.168.1.20:27016             # one server

- **LAN**: start a dedicated server (`run_server.sh`) or host from the menus (Multiplayer > Host a Match); other
  players open Multiplayer > LAN Matches. Servers on ports 27015-27020 are found (PC setting net_lan_ports, e.g.
  "27015-27030", for more; the player's own net_port is always included). A firewall must let UDP in on the server's
  port (queries and the game share it) and broadcasts out on the players'.
- **Internet**: a master server is any machine running csx-master with UDP 27010 open; servers name it (`--master` /
  server.cfg `master`; a listen server: PC settings `net_master_register 1` and `net_master HOST[:PORT]`), players name
  it in PC setting `net_master` (pc_settings.ini; several separated by commas). With none set the Internet tab says so.
- **Direct**: Multiplayer > Join by Address (an address or name, ":port" if not 27015); `./run_game.sh --connect
  HOST[:PORT] [--password P] [--server-key KEY]` still works for tests and advanced use.
- **Favorites**: White (pad), "Add to Favorites" (mouse) or the 1 key (the pad's White on the keyboard) on a server;
  kept in pc_settings.ini as favorite.N lines.

### 17.8 Measurements (Linux, one machine, 16 threads; real processes in one network namespace)

| What | Result |
|---|---|
| LAN discovery latency | answers 0-2 ms after the broadcast from dedicated servers, 26-28 ms from a listen server at 30 frames a second (it answers at its next frame); the list shows them 0.27 s after the tab opens (the first rebuild); csx-query: first answer 0 ms, last 1 ms |
| LAN query traffic | 12 broadcast queries of 256 bytes per refresh (2 broadcast addresses x 6 ports), every 3 s while the LAN tab is open: about 1 KB/s out; each server answers 160-240 bytes |
| Internet list, 300 servers | 10 master pages (12 KB of requests, 11.7 KB of pages), 300 queries (77 KB) and answers (48 KB): all 300 listed 1.45 s after the request (csx-query), 1.6 s in the menu |
| Menu during discovery | 30.0 frames a second while 300 servers arrive (60 frames in 2.0 s); rows rebuilt at most 4 times a second |
| Client cost | about 5% of a core while 300 servers arrive; memory +1 MB for 300 entries (100.7 to 100.8 MB); one more thread (the discovery) |
| Server cost | an answer: 16 us to build and sign (Ed25519), 183 bytes for a typical info answer; at the 400 a second cap under 1% of a core; players: 65 long names in 3 pages |
| Browser cost per answer | 48 us to read and verify |
| Refresh | X sends the list request again; 300 servers re-listed in 1.6 s; the LAN tab re-broadcasts at once |
| Expiry | LAN: gone 10 s after its last answer (seen: Delta 10 s after it quit); master: a silent server dropped after its expiry (30 s in the test, 180 s by default), a quitting one at once (its signed leave) |

### 17.9 Tests

| Test | What it shows |
|---|---|
| tests/net_browser_test.py (103 checks, ASan + UBSan) | query and answer serialization; signatures (every byte changed refused); hostile text cleaned and bounded; impossible counts refused; 20000 random datagrams unanswered; rate limits (burst, per second, total over 1000 sources, LAN only); answers never larger than queries; LAN discovery of 3 servers on 3 ports, duplicates, a new server, expiry; favourites with another key flagged; no answer; 65 players over pages; the master: 100 registrations, 4 list pages, a listed key proven wrong dropped, a forged registration, 5000 random datagrams, the per-address limit, expiry, leaving; the view: every sort both ways, every filter, 4096 servers sorted 4 ways in 25 ms; saved-server format |
| tests/server_config_test.py (22) | the master setting's validation |
| net_browse_lan | two dedicated servers and a listen server hosted from the menus found by broadcast (one entry each), players 2/65, 0/65, 1/65 (the listen host's player), a server appearing and expiring, sorting with Black, filtering (password hidden), tabs |
| net_browse_join | join, password typed and accepted, a wrong password refused and back in the browser with the message, a full server refused before connecting, a server shutdown back in the browser; the password in no log |
| net_browse_favorites | a favorite added (with the key proved), shown, removed (pc_settings.ini written); a favorite with another key: "!", the identity warning, B; known_servers.txt with another key: warning, B, then A trusts, the join pins the key, the server becomes a recent server |
| net_browse_address | Join by Address: a server joined; no answer, not an address, a name that does not resolve: messages |
| net_browse_master | csx-master: registration, a LAN-only server not registering, the Internet tab and csx-query listing, a quitting server leaving, a frozen server expiring |
| net_browse_many | 300 servers through a master, the menu at 30 frames a second, scrolling, refresh |
| vk_browser_43, _169, _219 | the Vulkan window at 4:3, 16:9, 21:9: Multiplayer, the list, a click on a header sorts, a click selects a row (no join), a tab click, Page Up, the Sort line, Escape back |
| net_ded_malformed (net_fuzz) | now also every connectionless type, 6000 malformed queries and a 1000-query flood at the server |

### 17.10 Known limitations

- **Client-side hardening**: the engine's client parsers against a hostile game server (17.5) are N1's; the
  browser does not make a joined server trustworthy, only authenticated.
- **No NAT traversal**: an internet server needs its UDP port reachable (forwarded); the master lists the address it
  sees.
- **IPv4 only** (as the transport).
- **The password is shown as typed** on the keyboard screen (the title's edit box has no masking); never logged.
- **Listen server pings** include up to one of its frames (17.1).
- **The players list** is in the query (csx-query --players) but the menus show counts only (humans and bots in the
  details), not names.
- **Windows**: the browser and query code are portable; LAN broadcast addresses come from getifaddrs on Linux, on
  Windows only 255.255.255.255 and the loopback network are used; no Windows build was tested in N3.
- **A hostile master** can make a browser query up to 4110 listed addresses per refresh (17.5).
- **Master servers keep no state** across restarts and are not federated; a player lists several masters by hand.

### 17.11 What N4 (split-screen) depends on

Split-screen is one client process per local player (section 10): each joins through the same browser and connection
path (+connect relaunch, pinned key); local players of one PC joining one server count as separate players in its
answers. Nothing in N3 assumes one player per PC.

### 17.12 Results on the final build (2026-10-26)

- Network scenarios (tools/run_net_tests.py, all of tests/net): 31 of 31: the 25 of N1 and N2 (listen and dedicated
  servers, 65 slots, map changes, trust, admin, loss, malformed traffic with the fuzzer now also at the query) and
  the 6 browser scenarios; net_browse_join again after 2ee3883: pass.
- Unit tests: net_transport_test 109, net_browser_test 103, server_config_test 22, stuff_filter_test 14,
  net_buffers_test 18,314: all pass.
- Headless offline regression (tests/run_regression.sh): 46 of 46.
- Window suite (tools/run_window_tests.py: the 27 default scenarios plus vk_scoreboard_169, vk_bot_count_169,
  vk_player_collision_169, vk_browser_43, vk_browser_169, vk_browser_219): 33 of 33, no rerun. The intermittent
  Player Collision failure seen in N2 did not reappear.

### 17.13 Commits (branch g8.2b)

| Commit | What |
|---|---|
| 5005dbd | src/net: the query, the master protocol, the browser's discovery, wire.h; tools/master; net_browser_test |
| c5dd4a3 | Servers answer queries and register with masters (server_query.cpp); the dedicated tick waits on its socket; Host a Match hosts; csx-server master |
| 64b4210 | The browser in the title's menus (pc_browser.cpp); joins and the way back; favorites; the profile walk fix |
| c84bace | Tests: net_browse_* and vk_browser_* scenarios; drive.py newlog, mark, spawn; the fuzzer at the query |
| 2ee3883 | A chat line never taken for a server's refusal |
| (this) | Docs: N3 (this section, FINDINGS, PROGRESS, ROADMAP, TODO, networking.md) |

## 18. N4 as built: split-screen (2026-10-27)

Split-screen is documented in docs/splitscreen.md (the investigation, architectures A and B compared, the decision,
as built, layouts, input, audio, the setup screen, slots, performance, network, tests, limitations). In short:
architecture B, section 10 option 1 above: a compositor owns the window and starts one game process per local
player (up to four); each player is an ordinary client of player 1's server with its own identity, transport
connection and slot; the server side is unchanged (no local shortcut, protocol 47 and N1-N3's message versions
unchanged); players 2-4 follow player 1's join with the server's key pinned and the password from the environment.
Tests: net_split_ded_4, net_split_browse, net_split_listen, net_split_full (network) and the vk_split_* window
scenarios.

N4.6 (2026-10-27, docs/splitscreen.md section 16) changed two things every server does, local or not: a player
identity proved again from another address (a game restarted after a crash, a new port or network) closes its old
connection, and a connection that closes on a server drops its engine client at once (when replaced: inside the packet
read, so the replacing join finds the slot free) or at the next tick, instead of after the engine's own timeout. Only a
proved identity (the handshake's signature) replaces; anonymous clients never do; no protocol or version changed.
Tests: tests/net_transport_test.cpp (identity reconnect), vk_split_crash (65 slots after each restart). The split
benchmark against a dedicated server is tests/net/net_perf_split_ded_4.txt.

## 19. N4.7: a match from the menus in its own server (2026-10-28)

Documented in docs/splitscreen.md section 17. What it changes here:

- **Hosting.** Single Player, Multiplayer > Host a Match and split-screen matches from the menus run in csx-server
  (this document's section 16), which the game starts beside itself (local_server/, its own logs, identity and disk)
  and joins as a client over the PC transport; the title's listen server is kept as the fallback (PC Options > Match
  Server > In Game, or CSX_SERVER_PROCESS=0) and is unchanged. Offline the server binds 127.0.0.1 only; Host a Match
  binds the PC port and answers LAN discovery and queries itself (section 17.7's hosting, now by csx-server). All 65
  slots are the server's; the game's own player is a client like the others.
- **The match line.** The menus' "+map ... +cvar value" line reaches the server as CSX_SERVER_MATCH and is checked
  (src/server/server_config.cpp MatchLine): `+wait`, `+map`, `+maxhumanplayers`, `+teampreference` and `mp_` / `bot_`
  / `pc_` cvars with plain values only; nothing else (no `exec`, `;`, quotes, `connect`, `rcon_password`). The server
  sets its own slots (65), `mp_dedicated_server 1`, the snapshot rate, LAN, name and password after it.
- **Trust.** The game trusts its local server's key for that address while the server runs (not saved with the known
  servers). The server accepts `pc_owner` commands (restart, changemap, console, pause) only from the identity that
  started it (CSX_SERVER_OWNER_KEY), from 127.x; a console line is one printable command without `;` and goes to
  the engine as hex in the command, so it cannot carry a second command.
- **Lifecycle.** SIGTERM when the game leaves the match (the players are told the server shut down), SIGKILL after
  5 s, always reaped; the server quits within 250 ms when its parent (the game) is gone; a server that ends by itself
  sends the game back to its menus with a message. A failed start (no csx-server, the port taken) is a message in the
  game's menus.
- **Security fix (found while testing).** csx-server let any client run the game DLL's `changemap` (the host's pause
  menu command, 0x13F92C): the server relaunched into an empty map name. Network clients' `changemap` is now refused
  (src/game/net_security.cpp); the owner's Change Map goes through `pc_owner changemap`, which checks the map. Test:
  tests/net/net_ded_changemap.txt. Commit b98d769.
- **Tick.** A local server has the dedicated server's 30 Hz tick (section 16.6), not the listen server's frame-rate
  simulation; with 61-64 bots it runs 24-28 ticks a second (docs/splitscreen.md 17.4).
- **Windows.** The local server uses fork / exec, the parent watch and SIGTERM: Linux only, like csx-server.
