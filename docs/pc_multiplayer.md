# PC multiplayer: architecture investigation and roadmap (N0, 2026-10-24)

Milestone N0 (investigation only, approved 2026-10-24); N1 (the PC transport and remote clients) approved 2026-10-25
and built: section 15 describes what exists, its tests, measurements and limits. Sections 1-14 are the N0 analysis
and plan as approved. Direction: ARCHITECTURE.md "Direction: PC first". Detailed evidence, with every
guest address, is in the analysis files this document cites:

- analysis/netcode_engine.md: the engine's netcode (server start, connectionless protocol, connecting, netchan,
  snapshots, delta, prediction, interpolation, lag compensation, voice, timeouts, map changes, system link, rcon).
- analysis/dedicated_server_browser.md: the host frame, the dedicated server remnants and the Xbox "Dedicated
  Server" host mode, server dependencies on the client, match configuration, system link discovery, the menus.
- analysis/splitscreen.md: local players, controllers, views, audio.
- docs/networking.md: what G8.2B already changed (protocol 47, 65 slots, validation, packet capacity).

Contents: 1 current architecture, 2 does remote play work today, 3 packet sizes and the transport, 4 options and the
recommendation, 5 snapshots and replication, 6 security, 7 dedicated server, 8 configuration, 9 server browser,
10 split-screen, 11 testing, 12 performance, 13 roadmap, 14 open questions and risks, 15 N1 as built.

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

