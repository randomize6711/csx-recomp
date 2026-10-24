# N0: Counter-Strike Xbox engine netcode as it exists in the binary

Part of N0 (docs/pc_multiplayer.md). Static, read-only analysis of build/ghidra/default_flat.exe (guest VA = file offset + 0x10000), 2026-10-24. Nothing
was run or built. Every address was read in the disassembly unless it is marked "inferred". This document describes
the **original** engine; the recomp's own changes (protocol 47, 65 slots, bounds checks, larger loopback packets) are
in docs/networking.md and FINDINGS.md.

Layout: sections 0, 1, 3 and 7 (globals, server start, netchan, disconnects / map changes); then sections 1.x to 6.x
of the connection part (connectionless protocol, connecting, queries, system link, rcon: questions 2, 8, 9); then the
snapshot part (snapshots, client side, voice: questions 4, 5, 6).

## Summary of the most important facts

- The engine is HL1-2003 GoldSrc netcode, almost unchanged: an 8-byte header (seq with reliable bit 31 and fragment
  bit 30, ack with reliable-ack bit 31), a 1-bit reliable stream retransmitted implicitly, two fragment streams, rate
  pacing through cleartime, COM_Munge2 obfuscation, **no qport**, and no reordering (late or duplicate packets are dropped).
- Timeouts: sv_timeout 15 s, cl_timeout 10 s (300 with "+test"), sv_failuretime 0.5 s (snapshots stop after 0.5 s of silence).
- Rates: rate 4000 (clamped 1000..20000), cl_updaterate 20 (server 10..60), cl_cmdrate 30, ex_interp 0.1,
  sv_maxunlag 0.5. Frame ring 16 (8 in single player), at most 256 entities per snapshot, PVS culling only.
- The connect challenge is not validated (SV_CheckChallenge 0x98CE0 always returns 1). There is no player identity
  beyond the userid: "uniqueid" is always 4294967295 and there is no XUID in the engine.
- Map changes: `changelevel` is disabled ("E3 HACK", 0x331D24). Every map change relaunches the title (releasemap.txt
  plus XLaunchNewImage). The host tells the lobby peers through lobby message 0x0B, and they relaunch and rejoin
  through system-link discovery; there is no in-engine reconnect.
- System link is a separate front-end lobby protocol: UDP broadcast port 1101, VDP port 1102, TCP 1103 / 1104. The
  engine's game ports are 1001 (server) and 1000 (client). The info queries leak the session key (XNKID / XNKEY / XNADDR).
- A host's CL_Disconnect calls Host_ShutdownServer: the listen-server host leaving ends the match for everyone.

## 0. Conventions and key globals (verified)

All addresses are guest VAs in build/ghidra/default_flat.exe (file offset + 0x10000). "inferred" marks anything not read
in the code. cvar_t layout {name, string, flags, float value, next}; the value is at cvar + 0xC.

| Global | VA | Evidence |
|---|---|---|
| realtime (double) | 0x6CE040 | Netchan_Transmit 0x69F3B, SV_CheckTimeouts 0x9C86C |
| host_frametime (double) | 0x6CE0E0 | SV_SendClientMessages 0x9E495 |
| svs.dll_initialized / clients / maxclients / maxclientslimit / spawncount | 0x636CE0 / E4 / E8 / EC / F0 | SV_SetMaxclients 0xA1960, map cmd 0x600E2, pfnChangeLevel 0x7A9BB |
| sv.active | 0x5EF7C0 | Host_ShutdownServer 0x5C9D0, SV_SpawnServer 0x9FF0B / 0xA0474 |
| host_client | 0x6CDFEC | SV_SendClientMessages 0x9E413 |
| cls.state | 0x7812E0 (0 dedicated, 1 disconnected, 3 connected, 5 active) | SV_SetMaxclients 0xA19E4, CL timeout 0x28C7A, CL_Disconnect 0x2901F |
| cls.netchan | 0x7812E8 (the client's netchan_t; Netchan code compares chan == 0x7812E8 to tell the client from the server) | 0x699F2, 0x6A0A2, 0x6A1F9 |
| net_from (netadr_t, 20 bytes) | 0x6BA5E0 | Netchan_Process 0x6A31F |
| net_message (sizebuf: flags 0x6BA584, data 0x6BA588, cursize 0x6BA590) | 0x6BA580 | 0x6A3C2, 0x695CE |
| msg_readcount / msg_badread | 0x748404 / 0x748524 | 0x6A36F, 0x6A378 |
| demo playback flag | 0x785458 | Netchan_Transmit 0x69F7D (skips the send), Netchan_Process 0x6A2C3 |
| ip_sockets[NS_CLIENT=0, NS_SERVER=1] | 0x551E20, 0x551E24 | NET_OpenIP 0x6CFD0 |
| net thread enabled / its critical section | 0x551E14, 0x551E18 (Enter/Leave via 0x3210C0 / 0x3210C4, cs 0x6BA5C0) | 0x6C7CD |
| Sys_Error / Con_Printf / SV_BroadcastPrintf (inferred name) | 0xAC090 / 0x40790 / 0x5C460 | |
| Cvar_RegisterVariable / Cmd_AddCommand | 0x422A0 / 0x3C760 | 0x2BD17, 0x64E05 |

client_t (stride 0x5010) fields verified here: +0x0 active, +0x4 spawned, +0x8 fully_connected, +0xC connected,
+0x20 netchan (netchan.remote_address +0x24, last_received float +0x3C, rate double +0x48, cleartime double +0x50,
message sizebuf +0x7C, flags +0x80), +0x2540 chokecount, +0x2548 fakeclient, +0x254C proxy (*hltv), +0x25C0 datagram
sizebuf, +0x3580 next_messagetime (double), +0x3588 next_messageinterval (double), +0x3590 send_message,
+0x3594 skip_message, +0x3598 frames ptr, +0x3578 drop time, +0x4B9C edict, +0x4BA4 userid (inferred from use as a key
into the front end's player list, 0xEF930), +0x4BC8 userinfo[256], +0x4D10 name, +0x4D30 topcolor, +0x4D34 bottomcolor,
+0x4EFC lw (cl_lw), +0x4F00 lc (cl_lc), +0x4F04 physinfo[256].

netchan_t (size 0x2520: Netchan_Setup clears 0x948 dwords at 0x696B3) is the 2003 HL1 layout:
+0x0 sock, +0x4 remote_address (netadr_t: type +0, ip +4, port +0x12), +0x18 player_slot, +0x1C last_received,
+0x20 connect_time, +0x28 rate, +0x30 cleartime, +0x38 incoming_sequence, +0x3C incoming_acknowledged,
+0x40 incoming_reliable_acknowledged, +0x44 incoming_reliable_sequence, +0x48 outgoing_sequence,
+0x4C reliable_sequence, +0x50 last_reliable_sequence, +0x54 connection_status, +0x58 pfnBlocksize,
+0x5C message (sizebuf, maxsize 3990), +0x70 message_buf, +0x1008 reliable_length, +0x100C reliable_buf,
+0x1FA4 waitlist[2], +0x1FAC reliable_fragment[2], +0x1FB4 reliable_fragid[2], +0x1FBC fragbufs[2],
+0x1FCC frag_startpos[2], +0x1FD0 frag_length[2], +0x1FD4 incomingbufs[2], +0x1FDC incomingready[2],
+0x20E8 tempbuffer, flow[2] at +0x20F0 (out) / +0x2308 (in), 32 samples of 16 bytes each, index at +0x22F0 / +0x2508.

## 1. Server start, maxclients, listen server, launching a match

### 1.1 How a match is launched (front end -> relaunch -> engine)
- Two programs in one XBE: the front end runs as `-game launcher` and a match runs as `-game cstrike`. At boot
  (0xBF470) the title calls XGetLaunchInfo (0x201E35, writing 0x552080 / launch data 0x5259E0), reads the
  "release map" file (0xBEF10: opens T:\releasemap.txt 0x331F1C and Z:\releasemap.txt 0x331F08), and builds the
  engine command line:
  - if the launch string contains "+map" (0x339B8C, test at 0xBF697) or "+connect" (0x33AAC8, 0xBF6A9):
    `-dev -console +set developer 1 -game cstrike -maxplayers 17 +maxplayers 17 +hideradar +hostname XBOX %s`
    (0x33AA60, sprintf at 0xBF6C0; 65 in the recomp by patch) and sets 0x551854 = 1 ("launched into a game");
  - otherwise the front-end template `... -game launcher ...` (0x33A9F0).
  - "+test" (0x33AAF8) sets 0x551EFC (it raises cl_timeout and sv_timeout to 300 s: 0x2BD3D, 0xA2414).
- The front end starts a hosted match by writing `+map %s %s` (0x33F2B4: map, then the option string, e.g. the
  presets at 0x33F2E8.. "+mp_match_option_preset N +mp_timelimit ...") to Z:\releasemap.txt (function 0xE2D20,
  "SuperLauncher_LocalServer*") and relaunching. A joining system-link client writes `+connect 0.0.0.0\n` (0x340674,
  in 0xF1540 "SuperLauncher_BrowseForLocalServers", also 0xF6690 / 0xF6A70); the "0.0.0.0" is only a placeholder: CL_Connect_f
  (0x29CD0) ignores its argument unless it is "local" and connects to the host address the system-link lobby resolved
  (string at 0x554334); 0xF1770 / 0xEA690 search for the session again after the relaunch (section 8, "Join and launch").
- Relaunch = 0xBF0E0: stops sounds (0x106380), fills the LAUNCH_DATA (0xC00 bytes, a copy of 0x5259E0) and calls
  XLaunchNewImageA (0x201D12) at 0xBF266. No Host_ShutdownServer / SV_DropClient call was found on this path (the
  callers of 0x5C9D0 do not include it).

### 1.2 Engine side: map, SV_SpawnServer, maxclients
- `map` console command (Cmd_AddCommand at 0x64E28 -> handler 0x5FD50): builds the map name (map list / "random"
  0x331DC8), then:
  - if 0x551854 (launched into a game) -> 0x5F750 (write `+map %s` 0x331EFC to Z:\releasemap.txt, delete
    T:\releasemap.txt, relaunch via 0xBF0E0);
  - else if svs.dll_initialized (0x636CE0) -> same relaunch (0x600F6);
  - otherwise (first map in this boot, game DLL not yet loaded) 0x5E0A0 then CL_Disconnect, "map change failed: '%s'
    not found on server." (0x331D88) if the BSP is missing, `sv_lan` check, then Host_Map core 0x5F520.
  So in this build every map change after the first is a title relaunch, as FINDINGS says.
- Host_Map core 0x5F520(map, loadgame, ...): Host_ShutdownServer(0) (0x5C9D0), SV_SpawnServer(map) (0x9FEF0),
  load / spawn entities (0x7B930), SV_ActivateServer(1) (0x9FC00); then, unless dedicated (cls.state == 0),
  Cmd_ExecuteString("connect local") (0x331F30 at 0x5F72F): the local player joins through the loopback like any
  client.
- `changelevel` (handler 0x60270) only prints "WARNING: CHANGELEVEL IS NOT SUPPORTED; E3 HACK" (0x331D24).
  The game DLL's pfnChangeLevel (0x7A9B0) first calls the front end's 0xE7040 (writes `+map ...` to
  Z:\releasemap.txt and calls the session object's map-change method 0xECA00, which tells the lobby peers and
  relaunches: section 7.3), then would queue "changelevel %s" (0x333AE0) / "changelevel2 %s %s" (0x333ACC).
- SV_SetMaxclients 0xA1960: `-maxplayers` (COM_CheckParm 0x3F160 with 0x3378E0) -> atoi; with no parameter
  maxclients = 1 (or 6 if the dedicated flag 0x5EF670 is set); cls.state = 1 unless dedicated (0xA19E4);
  clamp 1..17 (0xA19F3 / 0xA19F8, 65 in the recomp), maxclientslimit 17 (0xA1A10), client_t block malloc 0x55120
  (0xA1A0B); SV_UPDATE_BACKUP (0x379970) = 8 if maxclients == 1 else 16, mask 0x379974 (0xA1A1A..0xA1A27);
  `deathmatch` (0x331128) set to 1 if maxclients > 1 else 0 (0xA1AD7).
- `maxplayers` console command: "maxplayers cannot be changed while a server is running." (0x332FCC).
- SV_SpawnServer 0x9FEF0: bumps svs.spawncount (0x636CF0, 0xA0002), sv.datagram / reliable_datagram 4000 (0xA0303),
  multicast/spectator 1024, signon 0x8000 (0xA039F), num_edicts = maxclients + 1 and each client_t edict at
  +0x4B9C (0xA03C0 loop), sets sv.active (0xA0474).
- "Listen server": not a separate mode. The host's engine runs SV_Frame (0xA1D20, called from Host_Frame at
  0x5D3CD) and the client in one process; the host player's client_t is an ordinary slot whose
  netchan.remote_address.type == NA_LOOPBACK (1). Differences for that slot: Netchan_CanPacket always true unless
  net_chokeloop (0x69730); host_limitlocal (0x377DD4, default 0) can force a send every frame (0x9E461); the
  scoreboard ping is 0.
- SV_Frame 0xA1D20 order: 0xA1BD0, SV_ReadPackets 0x9C680, physics (0xA6470), 0x96DA0, 0xA7C50, 0xB6F20,
  SV_CheckTimeouts 0x9C860, 0xA6FF0, SV_SendClientMessages 0x9E3F0 (0xAC240 between each step: inferred to be a
  timing / service hook).

## 3. Netchan (transport layer) - verified

### 3.1 Sockets and addresses (NET_*)
- Plain Winsock UDP (socket / bind / sendto / recvfrom imports; on the console the socket layer is XNet's, so system
  link traffic is XNet-secured below this layer). No qport: the server finds a packet's client by comparing the
  whole address (SV_ReadPackets 0x9C6F2..0x9C77B: type, 4 IP bytes, port; a loopback address matches by type only).
- Ports: server socket from `ip_hostport` / `hostport` / `port` (cvars 0x378724 / 0x378738 / 0x37874C, defaults
  0 / 0 / **1001**), client socket from `ip_clientport` / `clientport` (0x378760 / 0x378774, defaults 0 / **1000**),
  bound on `ip` (0x378710, "localhost"). NET_OpenIP 0x6CFD0; UDP_OpenSocket 0x6CDE0 sets SO_BROADCAST (0x333150),
  FIONBIO, optionally SO_REUSEADDR ("-reuse"). Command-line: -port, -noip, -netsleep, -netthread, -clockwindow
  (0x332F6C..0x332F98, parsed in 0x6D530). "NET Ports: server %i, client %i" (0x333088).
- Address types: 1 NA_LOOPBACK, 2 NA_BROADCAST, 3 NA_IP (NET_SendPacket 0x6CC70: 1 -> NET_SendLoopPacket 0x6BBA0;
  2/3 -> NET_SendLong 0x6CAB0 on ip_sockets[sock]; anything else Sys_Error "NET_SendPacket: bad address type").
  Send errors WSAEWOULDBLOCK / ECONNREFUSED / ECONNRESET (and EADDRNOTAVAIL for broadcast) are ignored.
- Receive (NET_GetPacket 0x6C7C0): loopback queue first (0x6BAF0), then recvfrom of at most 0xFAA (4010) bytes
  (0x6C37E); a datagram of exactly 4010 bytes is dropped as "NET_QueuePacket: Oversize packet" (0x6C417); a
  datagram starting 0xFFFFFFFE goes to NET_GetLong 0x6C1A0 (split reassembly, up to 5 parts). fakelag / fakeloss /
  fakebw cvars (0x37879C 0.0, 0x3787C4 0.0, 0x3787B0 100000.0) act in 0x6BD50 / 0x6BF20 ("Server must enable cheats to
  activate fakebw", 0x3333E0).
- Loopback: two queues of 4 messages x 0xFC8 (4040) bytes at 0x440C00 (stride 0x3F38 per side, 0x6BBC3); the writer
  never blocks: when the reader falls more than 4 behind, the oldest messages are skipped (0x6BB0D). Received
  loopback packets get net_from.type = 1.
- Split datagrams (NET_SendLong / NET_GetLong): see docs/networking.md / FINDINGS "Packet overflow" (1304-byte
  threshold, 1294-byte parts, header 0xFFFFFFFE).

### 3.2 Packet header (Netchan_Transmit 0x698D0)
```
 u32 seq  = outgoing_sequence | (send_reliable << 31) | (has_fragments << 30)     (0x69CF0..0x69D47)
 u32 ack  = incoming_sequence | (incoming_reliable_sequence << 31)                (0x69CEA..0x69CFA)
 if bit 30: for each of 2 streams (0 normal, 1 file):
     u8 present; if present: u32 fragid (count<<16 | index), u16 start, u16 length   (0x69DB2..0x69E42)
 reliable payload (reliable_length bytes from reliable_buf)                        (0x69E48)
 unreliable payload if it fits                                                     (0x69EAA)
 padded with svc_nop (1) to at least 16 bytes                                      (0x69F0C..0x69F33)
```
- Bytes 8.. are obfuscated with COM_Munge2 (0x40240) keyed on (outgoing_sequence - 1) & 0xFF (0x69F89) and
  unmunged with 0x40330 in Netchan_Process (0x6A3DC) on (seq & 0xFF). Not a security measure.
- Mod hooks: 0x43EB40 (outgoing, called before the send at 0x69FB4) and 0x43EB44 (incoming, 0x6A392); a packet with
  bit 30 of the *ack* word set is rejected unless the incoming hook exists (0x6A38C..0x6A3A3). Both are inferred
  to be NULL in this build (HL's m_pfnProcessOutgoingNet / m_pfnProcessIncomingNet).
- Sizes: message (reliable queue) 3990 (0xF96, Netchan_Setup 0x69717); the datagram buffer 4037 (0xFC5, 0x6990D);
  when fragments are sent the packet is capped at 1304 (0x518, 0x69EB0) and an unreliable payload that does not fit
  is silently left out (0x69EBF). MAX_RELIABLE_PAYLOAD 1200 (0x4B0): a reliable message larger than that is turned
  into fragments (0x699E9, 0x69B5E).

### 3.3 Sequencing, acks, loss handling (Netchan_Process 0x6A2C0)
- Address check: a packet from an address other than remote_address is ignored (NET_CompareAdr 0x6B3C0 at 0x6A351;
  skipped during demo playback).
- `last_received = realtime` for every accepted packet (0x6A375) - this drives the timeouts (section 7).
- `seq <= incoming_sequence` -> the packet is **dropped**: "%s:duplicate packet %i at %i" (0x332CA0) when equal,
  "%s:out of order packet %i at %i" (0x332C7C) when older, printed only with net_showdrop (0x3786AC, default 0).
  There is no reordering buffer: late packets are lost data (snapshots are self-correcting through the delta ack;
  reliable data is retransmitted).
- Loss: net_drop = seq - incoming_sequence - 1 (stored 0x6CA720), "%s:Dropped %i packets at %i" (0x332C5C).
- Reliable ack: if reliable_ack == reliable_sequence and incoming_acknowledged + 1 >= last_reliable_sequence the
  pending reliable is acknowledged and reliable_length = 0 (0x6A691..0x6A6A1).
- State update: incoming_sequence = seq, incoming_acknowledged = ack, incoming_reliable_acknowledged = reliable_ack,
  incoming_reliable_sequence ^= 1 when the packet carried reliable data (0x6A6B5..0x6A6C0).
- Returns true only if more than 16 bytes remain to parse (0x6A844).

### 3.4 Reliable delivery and retransmission (Netchan_Transmit)
- One reliable message in flight per channel, a 1-bit alternating sequence (reliable_sequence +0x4C toggled at
  0x69A60).
- Retransmission is implicit: when incoming_acknowledged > last_reliable_sequence and incoming_reliable_acknowledged
  != reliable_sequence (the peer has acknowledged a later packet without the reliable bit flipping), send_reliable
  is set and the stored reliable_buf is sent again in the next packet (0x6996E..0x6998B). There is no timer; resends
  happen at the channel's packet rate.
- New reliable data is taken only when reliable_length == 0 (0x6998F): pending fragments first (0x6A880 moves waiting
  fragment buffers up), otherwise netchan.message is copied to reliable_buf (0x69A75..0x69ADD); a message > 1200 is
  fragmented first (Netchan_CreateFragments 0x6A910).
- `last_reliable_sequence = outgoing_sequence - 1` when reliable data goes out (0x69E9C).
- A reliable message that overflows netchan.message (flag 2 at +0x60): Netchan_Transmit prints "%s:Outgoing message
  overflow" and sends nothing (0x6991E..0x6996D); the server drops such a client ("%s overflowed" 0x338140,
  "WARNING: reliable overflow for %s" 0x33811C, SV_DropClient(cl, 0, "Reliable channel overflowed" 0x338100) in
  SV_SendClientMessages 0x9E4BA..0x9E546).

### 3.5 Fragment streams
- Two streams: 0 = normal (large reliable messages: signon, SV_New_f's userinfo, etc.), 1 = file (uploads/downloads;
  the client's "File fragment received with no filename" 0x332B4C).
- Fragment buffer: 0x648 bytes with a 1304-byte (0x518) payload buffer (0x6A17D..0x6A1A5).
- Fragment (chunk) size comes from the channel's pfnBlocksize:
  - server: SV_GetFragmentSize 0x98B20 (passed at 0x5C8A0 / in SV_ConnectClient): 1024 while the client is not yet
    active+spawned+connected+fully connected; afterwards the client's `cl_dlmax` userinfo (0x32C0A0) clamped 64..1024,
    128 if absent;
  - client: 0x27280: 128 unless cls.state == 5 (active), then cl_dlmax (cvar 0x371C6C, default "128") clamped
    16..1024.
- Validation on receipt (0x6A240, the engine's own): fragid count and index <= 25000 (0x61A8), start <= 0x4000 and
  length <= 0x800; the recomp adds its own bounds check (docs/networking.md section 3).
- Missing fragment: Netchan_CheckForCompletion 0x6A1D0: on the client a hole makes it print "Netchan_CheckForCompletion:
  Lost/dropped fragment would cause stall, retrying connection" and execute "retry" (0x6A201..0x6A213); on the
  server it just waits.
- Incoming completed fragments are processed in SV_ReadPackets: incomingready (+0x1FFC) -> Netchan_CopyNormalFragments
  0x6AD60 then SV_ExecuteClientMessage 0xABEC0; file stream -> Netchan_CopyFileFragments 0x6AE40 -> SV_ProcessFile
  0x9C280 (custom decal uploads only, "Ignoring non-customization file upload" 0x338250).

### 3.6 Rate limiting and choke
- Netchan_Setup 0x696A0: rate = 9999.0 (double 0x3666E0), outgoing_sequence = 1, message maxsize 3990, flags
  FSB_ALLOWOVERFLOW.
- After each send: cleartime = max(cleartime, realtime) + (bytes + 46) * (1 / rate) (0x6A03E..0x6A06B; the 46-byte
  overhead 0x2E is also used in the flow statistics). If sv_lan (0x3797B8, default 0) is set and sv_lan_rate
  (0x3797CC, default 20000.0) > 1000, 1 / sv_lan_rate is used instead (0x69FF6..0x6A033; guarded by sv.active
  0x5EF7C0).
- Netchan_CanPacket 0x69730: true if cleartime < realtime; a loopback channel is always allowed (and cleartime reset)
  unless net_chokeloop (0x3786D4, default 0).
- Server rate per client: userinfo `rate` clamped 1000..20000 (0x9E96C..0x9E988) -> netchan.rate; then
  SV_CheckRate 0x9C1E0: sv_maxrate (0x37968C, default 0 = off) caps it, at most 20000; sv_minrate (0x379678, default 0)
  floors it, at least 1000. Client cvar `rate` default "4000" (0x371C1C, flags USERINFO|ARCHIVE); the client's own
  channel rate is set by a `rate`-style console handler 0x2B950 (1000..20000, "rate" 0x32C0D0).
- Send pacing (SV_SendClientMessages 0x9E3F0): a client gets a packet when
  host_frametime + realtime >= next_messagetime (+0x3580) and it is fully connected (0x9E495); next_messagetime =
  realtime + host_frametime + next_messageinterval (+0x3588 = 1 / cl_updaterate, clamped by sv_min/maxupdaterate:
  0x9EA24..0x9EA45, 0x98800); while signing on a client is answered on every packet it sends (0x9C7B4).
- Choke: when the rate check fails (cleartime >= realtime) the packet is skipped and chokecount (+0x2540) increments
  (0x9E60A); if the channel has fragments waiting (+0x1FDC), one Netchan_Transmit is still made without charging the
  rate (cleartime saved and restored around it, 0x9E61E..0x9E673; 0x3799CC = 0 lets Netchan_Transmit skip an
  empty packet, 0x69CCA).
- sv_failuretime (0x37977C, default 0.5): if nothing has been received from a client for longer, the server stops
  sending it snapshots until it hears from it again (0x9E554..0x9E56C) - an important behaviour for lossy links.
- Flow statistics (Netchan_UpdateFlow 0x69770): 32-sample rings per direction, recomputed every 0.1 s
  (0x365D90): kbytes/s and an average (0.33 / 0.67 smoothing at 0x366108 / 0x366110); net_graph cvars
  0x3787D8.. Debug cvars: net_showpackets (0x378698; prints "%s --> sz=%i seq=%i ack=%i rel=%i tm=%f" 0x332D60 /
  "<--" 0x332CC0), net_showdrop (0x3786AC), net_log (0x378684).

## 7. Disconnects, timeouts, shutdown, map transitions

### 7.1 Timeouts
- Server: SV_CheckTimeouts 0x9C860 (every SV_Frame): for each active/connected/spawned client that is not a fake
  client, if netchan.last_received < realtime - sv_timeout -> SV_BroadcastPrintf("%s timed out") (0x3381B4) and
  SV_DropClient(cl, 0, "Timed out") (0x3381A8). sv_timeout 0x379768 default **15** (300 with "+test", 0xA2414).
  Exception (surprising): clients whose name matches a record of the front end's player list (0xEFD80: vector at
  0x554244..0x554248, 0x60-byte records, +0x40 == 0) or the name at 0x5548E8 are never timed out (0x9C896); the game
  DLL uses the same test (0x11DB78) for its idle check. Inferred: the list holds the console's local / session players.
- Client: CL frame 0x28A40: when cls.state >= 3 and not a demo, realtime - cls.netchan.last_received (0x781304) >
  cl_timeout (0x371A3C, default **10**, 300 with "+test") -> "Connection to server timed out." (0x32BA74) and
  CL_Disconnect.

### 7.2 Disconnect
- SV_DropClient 0x5C600 (cl, crash, fmt, ...): if not crash and not a fake client, svc_disconnect (2) is written to
  netchan.message and a final packet with the single byte 2 is sent through Netchan_Transmit (0x5C655..0x5C6B6):
  **no reason string goes to the client** (the reason is only in the server's "Dropped %s from server\nReason:  %s"
  0x330F80). Then the game DLL's ClientDisconnect (via 0x5EF1E0) when spawned, connection-time statistics
  (0x636D4C / 0x636D50, for drops after 60 s), Netchan_Clear, active/spawned/connected/fakeclient/proxy cleared,
  name/userinfo/physinfo cleared, SV_FullClientUpdate to everyone (0x9CAC0 into sv.reliable_datagram 0x62C3E4),
  the front end is told (0xEF930(kicked = reason == "Kicked" 0x330F78), keyed by client +0x4BA4), frames freed
  (0x956E0).
- Client side CL_Disconnect 0x28F20: when connected it sends three unreliable 13-byte packets
  `clc_stringcmd "dropclient"` (byte 3 + 0x32BA5C) without waiting for an ack (0x28FC2..0x29011), sets cls.state = 1,
  then **Host_ShutdownServer(0)** (0x2902C): a listen-server host that disconnects ends the server for everyone.
- Host_ShutdownServer 0x5C9D0: sv.active = 0, clears the loopback queues (0x6BC20 on 0x448AC8 / 0x448AF0), drops every
  active/connected client with "Server shutting down" (0x330F60; each gets svc_disconnect), CL_Disconnect, clears sv
  (0x11906 dwords at 0x5EF7C0) and svs.clients, "Server shutdown" (0x330F4C), closes the log.

### 7.3 Map transitions (relaunch)
- Host side: the game DLL's end-of-map changelevel goes through pfnChangeLevel 0x7A9B0 -> front end 0xE7040, which
  writes `+map <map>` to Z:\releasemap.txt (0xE719C..0xE71E2) and calls the session object 0x554208's method 0xECA00:
  five times, with Sleep(100) between (0x200B04), it sends lobby message type 0x0B carrying the map index
  (0x5F830 maps the name to an index in the list at 0x331EF0..) through 0xED270 (to every peer of the session), then
  0xFC710(1), then relaunches with the session's launch data (0x5259E0 copied, the session key fields +0xE8 / +0xEC
  at launch data +0x895 / +0x899, map name at 0x5265E0) via 0xBF0E0.
- Remote clients: the lobby dispatcher (jump table 0xEDC2C, types 2..0x11; type 0x0B -> 0xEF2E0) calls 0xE9580,
  0xFC710(1), Sleep(4000 or 2000 by the message's second byte) so that the host comes back first, then
  (unless session +0xCA0) relaunches with the same launch data and the new map name (0x5FA20 into 0x5265E0) via 0xBF0E0.
- So there is no in-engine reconnect (no svc_reconnect / "reconnect" use on map change): the engine connection is
  abandoned (no svc_disconnect is sent on this path) and every machine reboots the title; the clients then join again
  from scratch: after the relaunch 0xF1770 / 0xEA690 search again for the session whose host id matches (up to 8 tries)
  and connect to the address it resolves (section 8, "Join and launch").
  The engine's own `reconnect` command exists (registered at 0x64E8A, handler 0x60680) but this path does not use it.

# Part B: connection, queries, system link, rcon / client commands (Q2, Q8, Q9)

Static reading of build/ghidra/default_flat.exe (guest VAs). "inferred" = not proved in the code here.
Conventions: netadr_t is 0x14 bytes {type, ip[4], ipx[10], port(be16) at +0x12}; type 1 = NA_LOOPBACK,
2 = NA_BROADCAST, 3 = NA_IP. net_from = 0x6BA5E0, net_message = sizebuf at 0x6BA580 (data ptr 0x6BA588,
cursize 0x6BA590). Cmd_Argc 0x4253E8, Cmd_Argv table 0x4251A8. The indirect call `*0x378558` / `*0x37855c`
seen before every Argv read goes to 0x1141E0, which is a bare `ret` (no-op hook; ignore).
SZ_GetSpace 0x3ECE0, Con_Printf 0x40790, Sys_Error 0xAC090, NET_SendPacket 0x6CC70 (ebx = socket, 1 =
NS_SERVER, 0 = NS_CLIENT), NET_CompareAdr 0x6B3C0, NET_AdrToString 0x6B520, Info_ValueForKey 0x65290,
Info_SetValueForKey 0x65760, Info_RemoveKey 0x65390, Q_atoi 0x3DC80, Q_strncasecmp 0x3DC30 (eax = 99999 = full
compare), Q_strcmp 0x3DBD0. Engine and front end are the same XBE; the front end (system link lobby) code is in
0xE0000-0xF8000.

## 1. Connectionless packets (header 0xFFFFFFFF)

### 1.1 Server: SV_ConnectionlessPacket 0x9BE50
Called from SV_ReadPackets 0x9C69D when the first long is -1 (0x9C6B8). Tokenizes (0x3C620) and dispatches on
argv[0] (first char or whole word), in this order:

| match | handler | reply / action |
|---|---|---|
| "ping" or 'i' alone (A2A_PING) | 0x99D10 SVC_Ping | 6-byte OOB `FF FF FF FF 'j' 00` (A2A_ACK) |
| 'l' (A2A_PRINT) | inline 0x9BEDC | Con_Printf the rest of the packet |
| 'j' alone (A2A_ACK) | inline 0x9BF10 | Con_Printf "A2A_ACK from %s" (0x3382B0) |
| 's' alone (M2A_CHALLENGE) | 0x9B3C0 | finds the sender in the master list (0x551D80, linked by +0x2C), stores MSG_ReadLong as that master's challenge (+0x8); heartbeat path, vestigial |
| 'K' alone | 0xB2890 | OOB block file transfer state machine (tokens BLOCK/FINISH/ACCEPT/FILE/ABORT built on the stack at 0xB2898..; state at 0x4E50B4..0x4E50D8). Purpose not traced (inferred: HL's OOB upload/transfer). The client dispatches 'N' to the same function (0x2825C) |
| "log ..." | inline 0x9BF9E | only if sv_logrelay (cvar 0x3796A0, default "0") != 0 and len > 4: print the text (log relay) |
| first byte '3'..'6' (0xB68C0) | 0xB6B90 | WON auth protocol (C2S_AUTHREQUEST1 etc.); its function table 0x5EF130..0x5EF15C has no static writer (inferred: null on Xbox, so dead) |
| "getchallenge" | 0x99D90 SVC_GetChallenge | `FF FF FF FF 'A' "00000000 %u %i"` (fmt 0x338520): challenge, authprotocol |
| "challenge" | 0x9A150 | rcon challenge: `"%c%c%c%cchallenge %s %u\n"` (0x338504), i.e. "challenge rcon <n>" |
| "info" | 0x9AA80(0) SVC_Info | 'C' reply |
| "details" | 0x9AA80(1) | 'm' reply |
| "infostring" | 0x9A480 SVC_InfoString | "infostringresponse" + info string |
| "players" | 0x9AFB0 SVC_PlayerInfo | 'D' reply (2048-byte buffer, see player_slots_engine_server.md) |
| "rules" | 0x9B230 SVC_RuleInfo | 'E' reply |
| "connect" | 0x994D0 SV_ConnectClient | |
| "pstat" | `*0x43EB74(...)` | callback pointer, 0 in the image (inferred: set at run time or never) |
| "rcon" | 0x9BBC0 SV_Rcon | |
| anything else | 0x9B480 | only when sv.active and maxclients > 1: hands the packet to the game DLL's pfnConnectionlessPacket (`*0x5EF254`) with a 0x7FC-byte reply buffer, sends the reply if one is produced. HL's "bad connectionless packet" print is gone (0x9C185..0x9C1C8 calls NET_AdrToString and drops the result). |

Second, smaller dispatcher 0xA1B00, called from 0xA1CC0 when NO server is active and the client is not connected
(sv.active 0x5EF7C0 == 0, cls.state 0x7812E0 == 0, 0x5EF7A0 != 3, socket 0x551D7C open): answers only
"getchallenge" (0x99D90), "challenge" (0x9A150) and "rcon" (0x9BBC0).

### 1.2 Client: CL_ConnectionlessPacket 0x27CE0

| match | handler | meaning |
|---|---|---|
| "challenge" | 0x277F0 | rcon challenge answer → sends "rcon %u \"%s\" %s" (0x32BEF8, built in 0x276C0) |
| "get" | 0x27950 | answers queries for clientaddress / serveraddress / wonid / userid ("unknown variable") |
| 'B' (S2C_CONNECTION) | 0x272D0 | connection accepted (below) |
| 'G' | inline 0x27DCD | multicast-auth challenge (only cls.state 2, argc 4): sets cls.challenge 0x7847CC, cls.authprotocol 0x7847D0; only authprotocol 2 is accepted (else "Invalid multicast authentication type") → 0x27AE0 |
| 'A' (S2C_CHALLENGE) | inline 0x27E95 | only in cls.state 2; argc 3 or 5; cls.challenge = atoi(argv1), cls.authprotocol = atoi(argv2) (0xFF → 2); authprotocol != 1 → CL_SendConnectPacket 0x29310; 1 (WON cert) → WON path 0xB7220 or "The server requires that you be validated through WON.net" + disconnect |
| '3'..'6' | 0xB6D00 | WON auth (dead, as on the server) |
| 'l' | inline | print |
| '8' | inline 0x27FF6 | only in state 2: "BADPASSWORD" → "Invalid server password.", disconnect (0x28F20) |
| 'L' | inline 0x28086 | redirect: stuff "connect %s" (0x32BD28, Cbuf_AddText 0x3B970), print "Redirecting connection to %s." |
| '9' (S2C_CONNREJECT) | inline 0x280DC | state 2 (or -steam): print the reason, disconnect |
| 'i' | inline 0x28140 | reply 'j' (A2A_ACK) |
| 'j', 'D', 'E', 'f' | 0x68D60 Net_APIProcess | server-browser responses (net_api) |
| 'h' | 0x26AB0 | MOTD print |
| 'y' | 0x26BF0 | master server list ("end-of-list"/"more-in-list") |
| 'N' | 0xB2890 | OOB file transfer (see 'K') |
| "requests" | 0x26EB0 | user list from a master ("%i users") |
| else | 0x275C0, else "Unknown command:\n%c\n" |

## 2. Connecting

### 2.1 Client side
- `connect <server>` = CL_Connect_f 0x29CD0. **Unless the argument is "local", the address actually used is the
  string at 0x554334** (when non-empty): 0x29D11..0x29D65 copies 0x554334 instead of argv[1]. 0x554334 is
  front-end lobby object 0x554208 + 0x12C, written by XNetInAddrToString (0x262FE8, call at 0xF026F) with the
  host's secure IN_ADDR obtained from XNetXnAddrToInAddr (0xF0225). So "+connect 0.0.0.0" (the front end's
  launch line) is a placeholder: the engine connects to the lobby's resolved host address. Resets cls state:
  servername 0x784804, cls.state 0x7812E0 = 2, retry count 0x7847C8 = 0, cls.challenge 0x7847CC = 0,
  0x785788 = 0 (cls.passive / multicast), opens the client IP socket (NET_Config 0x6CFD0) if needed.
- CL_CheckForResend 0x298F0 (every frame):
  - **Listen server / loopback**: if cls.state == 1 and sv.active (0x5EF7C0): servername "localhost",
    state 2, authprotocol 2 and CL_SendConnectPacket 0x29310 immediately; no getchallenge.
  - Remote: resends "getchallenge" (`%c%c%c%cgetchallenge\n` 0x32B764, or "getchallenge steam" with -steam)
    every cl_resend seconds (cvar 0x371ADC, default "6.0", clamped to 1.5..20 at 0x29A3F..0x29A76); port 0 →
    "1001"; after 4 tries (0x7847C8 >= 4) "#GameUI_ConnectionFailed" and state 1.
- CL_SendConnectPacket 0x29310: `"%c%c%c%cconnect %i %i \"%s\" \"%s\""` (0x32B8C4, 0x800-byte buffer):
  protocol (immediate 0x2E = 46 at 0x296C1; G8.2B patches it to 47 per docs/networking.md), cls.challenge,
  protinfo, userinfo (cls.userinfo 0x785148). protinfo keys: `prot` = authprotocol, `unique` = -1, `raw` =
  0x29220's output, `cdkey` (only when authprotocol != 2). 0x29220 = MD5 (state 0x67452301..) of a 64-byte stack
  buffer that this function never fills (only its terminator at +0x40 is zeroed) → 32 hex chars (inferred:
  arbitrary but well-formed; there is no CD key on Xbox). authprotocol 1 (WON) asks `*0x5EF13C/*0x5EF154`.
- 'B' accepted → 0x272D0: argv1 → 0x7847D4 (atoi of the server's id string), argv2 → 0x7847D8 (userid),
  argv3 → 0x7847DC (address string, "trusted address"); Netchan_Setup(NS_CLIENT, cls.netchan 0x7812E8, ...)
  0x696A0; cls.state = 3; writes "new" into the netchan message (0x27555).

### 2.2 Server: SVC_GetChallenge 0x99D90
- Table: 1024 entries x 0x1C at 0x63B0C0..0x6420D8: {netadr 0x14, challenge +0x14, time +0x18}. Looks up the
  sender (NET_CompareBaseAdr inline: type, IP; loopback matches by type), else replaces the oldest (time) entry.
- New challenge = (RandomLong(0,0xFFFF) << 16) | RandomLong(0,0xFFFF) (0x7AB40), time = (int)realtime
  (0x6CE040).
- Reply `FF FF FF FF "A00000000 %u %i"`: challenge, authprotocol: 2 (PROTOCOL_HASHEDCDKEY) when sv_lan != 0
  (cvar 0x3797B8, default "0") or when WON auth is unavailable (0x3FF60 sets 0x551FCC = 1 when the WON pointers
  0x5EF158/0x5EF15C are null; no static writer → inferred always 2 on Xbox), else 1.

### 2.3 Server: SV_ConnectClient 0x994D0 (`connect <prot> <challenge> "<protinfo>" "<userinfo>"`)
Order of checks (reject = SV_RejectConnection 0x98910: `FF FF FF FF '9' <reason>` via NET_SendPacket, then
clears the sender's WON auth slot 0xB69F0/0xB6ED0):
1. argc < 5 → "Insufficient connection info".
2. **Protocol** SV_CheckProtocol 0x98C60: argv1 must equal 0x2E (46, compare at 0x98C78; patched to 47 in this
   project). Lower → "This server is using a newer protocol ( %i ) than your client ( %i )..." (0x338748); higher →
   "older protocol ... contact the server administrator at %s" with sv_contact (cvar 0x379628, default "").
3. **Challenge** SV_CheckChallenge 0x98CE0: **does not check the challenge.** It receives only the address (the
   argv2 value is read at 0x9959B but never passed); returns 1 on every path (with -steam, or when 0x551FCC == 0
   and sv_lan == 0, or after an unused class-B/reserved-address test for NA_IP). The challenge table 0x63B0C0 is
   referenced only by 0x99D90 and 0x9A150 (all references checked). So getchallenge is a formality and a
   connect is accepted from any (spoofed) source address.
4. **protinfo** SV_CheckKeyInfo 0x98E70: `prot` 1..3 else "Invalid connection."; `unique` = uniqueid; if != -1
   and banned (SV_CheckForBan 0x98B90) "You have been banned from this server."; `raw` required (authprotocol 2:
   exactly 32 chars) else "Invalid authentication certificate length."; authprotocol != 2 needs a 32-char
   `cdkey` else "Invalid hashed CD key."; the hashed key goes to client+0x4CD0 (32 bytes). Port forced to
   atoi("1000") (0x98FB2, vestigial).
5. userinfo (argv4) copied (0x3FF max).
6. Reconnect detection: if not local (0x5CF10) and authprotocol not 2/3 and uniqueid valid → 0x99000 finds a
   connected, non-fake client with +0x4BA8 == 0 and +0x4BB0 == uniqueid; "CD key already in use." when it is
   another player. Otherwise (the Xbox case: authprotocol 2) the loop 0x997A4 marks a reconnect when any client's
   netchan address (+0x24) equals the sender (NET_CompareAdr: IP and port).
7. **Password / name / HLTV** SV_CheckUserInfo 0x991D0: if not loopback and sv_password (cvar 0x3797A4,
   default "", flags 0x24) is non-empty and not "none" and userinfo "password" differs → log "%s: password
   failed", reply `FF FF FF FF '8' "BADPASSWORD"` (0x98A40). Then removes "password" from userinfo, takes "name"
   (default "unnamed" 0x32C118, '%' → ' ', trimmed 0xB0740), dedups names "(%d)name" (0x99060, loop bounded 17 →
   raised to 65 by the project), "*hltv": 1 = proxy (count 0x95B50 vs sv_proxies cvar 0x3797E0 default "1":
   "Proxy slots are full."), 3 → "Please connect to HLTV master proxy.", other → "Unknown HLTV client type.".
8. WON cert check 0x98DD0 (skipped for authprotocol 2).
9. Slot: reconnect → the same slot; game DLL pfnClientDisconnect (`*0x5EF1E0`) on the old edict if it was
   active/spawned, print "%s:reconnect". New → SV_FindEmptySlot 0x99470: first client_t with active (+0),
   spawned (+4) and connected (+0xC) all 0, else "Server is full.". Slot order is ascending; nothing reserves
   slot 0 for the host (the listen player just connects first).
10. Set up (host_client 0x6CDFEC): +0x4BA8 = 0 (id type), +0x4BB0 = uniqueid; SV_ClearClient-ish 0xA7760;
    frames: free 0x956E0, malloc(SV_UPDATE_BACKUP (0x379970) * 0x828) → +0x3598; edict = EDICT_NUM(slot+1)
    (0x7BA30) → +0x4B9C; `*0x43EB6C(slot)` hook (0 in the image); **Netchan_Setup 0x696A0(NS_SERVER,
    &client->netchan (+0x20), adr, slot index, client, SV_GetFragmentSize 0x98B20)** - no qport;
    +0x3588 = 0.05 (0x365FD0, next message interval) and +0x3580 = realtime + 0.05; +0x2544 = -1
    (delta_sequence); +0x2550 cleared (0x34 bytes); +0x25B0 = -1.0; hashed cdkey → +0x4CD0;
    active = spawned = 0, connected (+0xC) = 1, +0x10 = 0, +0x8 = 0; **userid +0x4BA4 = g_userid (0x3791E8)++**;
    0xEF8D0(ip bytes, userid) writes the userid into the front-end lobby's player record whose IP matches
    (vector 0x554244..0x554248, 0x60-byte records, IP at +0x4C, userid at +0x44); reply via Netchan_OutOfBandPrint
    0x694C0: `"%c %s %i \"%s\""` (0x338564) = 'B', id string, userid, address string; log
    `"%s<%i><%s><>" connected, address "%s"` (0x33853C); userinfo → +0x4BC8 (0x100 max), SV_ExtractFromUserinfo
    0x9E6C0; client datagram sizebuf at +0x25C0 (name ptr = +0x4D10, flags +0x25C4 = 1 allow overflow, data
    +0x25D4, maxsize +0x25CC = 0xFA0 = 4000).
- SV_GetFragmentSize 0x98B20: userinfo "cl_dlmax" clamped 64..1024; absent → 128; client not fully
  connected → 1024.
- SV_ReadPackets 0x9C69D matches a packet to a client by the full netadr (type, 4 IP bytes, port at +0x12;
  loopback by type only), then Netchan_Process (0x6A2C0). No qport: a NAT port change breaks the connection.

### 2.4 Player identifiers on the wire
- Slot index: svc_serverinfo playernum byte (see player_slots_engine_server.md), entity = slot + 1.
- userid: g_userid 0x3791E8, incremented per connect (0x99B3B) and per fake client (0x79E5C); sent in the 'B'
  reply and (inferred, HL) in svc_updateuserinfo.
- "uniqueid" string SV_GetIDString 0xA2B90 (on client+0x4BA8): type 0 → "%u" of +0x4BB0 (the protinfo
  `unique`, -1 for every Xbox client, so "4294967295" for everyone); type 1 → "STEAM_%u:%I64u" /
  "STEAM_ID_LOOPBACK" / "STEAM_ID_PENDING"; else "UNKNOWN". **There is no persistent per-player identity**
  (no XUID in the engine path; Xbox Live identity lives only in the front end).
- The front-end lobby keeps its own player list (0x554244 vector) and joins it to engine userids by IP (0xEF8D0);
  the engine calls 0xEF930 from SV_DropClient (per the Q7 fork).

### 2.5 Bots (fake clients): SV_CreateFakeClient 0x79D70
First free slot (same test as 0x99470), NULL if full; frees frames; zeroes the client_t (0x1404 dwords =
0x5010); name → +0x4D10; userid = g_userid++; active = spawned = +8 = connected = 1; **fakeclient +0x2548 = 1**;
edict = slot + 1, pev->netname, pContainingEntity, **pev->flags (+0x224) = 0x2008 (FL_CLIENT | FL_FAKECLIENT)**;
userinfo name, model "gordon", topcolor, bottomcolor; SV_ExtractFromUserinfo. No Netchan_Setup (netchan zeroed:
address type 0); nothing is sent to a bot (inferred: SV_SendClientMessages skips fakeclient). The project hooks
0x79F91 to add "*bot\1".

### 2.6 Loopback vs remote summary
Loopback: no getchallenge, challenge 0, adr.type 1, no password check (0x991E7), protocol and protinfo still
checked, NET_SendPacket routes NS 1/0 to the loopback queue (0x6CC7D → 0x6BBA0). Remote: getchallenge (not
verified), password, address+port identity.

## 3. Server info queries (Q8, engine side)
- **SVC_Info 0x9AA80** (only sv.active and maxclients > 1; 1400-byte buffer 0x578): 'C' ("info") or 'm'
  ("details"); address (net_local_adr, or "LOOPBACK" when 0x551E1C), hostname (cvar "hostname"), map (sv.name
  0x5EF7F0), gamedir, game description (`*0x5EF210`), players (connected client count), maxclients, protocol,
  server type 'd'/'l' and os 'w', password flag (sv_password set and != "none"), then **three raw binary
  fields as strings: the lobby's XNKID (8 bytes), XNKEY (16) and the host XNADDR (36, from 0xE9120)**
  (0x9ADDE..0x9AE3C, each NUL-terminated after its length), then mod info (0x9A3A0).
- **SVC_InfoString 0x9A480** (sv.active, maxclients > 1, not 0x551E1C): `FF FF FF FF "infostringresponse"`
  + an info string: protocol, address, players, proxytarget, lan, max, gamedir, description, hostname, map, type,
  password, os, secure, **xbox_key_id, xbox_key, xbox_host_address** (raw bytes from the lobby object
  0x5542F0.. = 0x554208+0xE8 (XNKID) / +0xF0 (XNKEY) and the XNADDR, inserted as info values 0x9A883..0x9A931:
  any 0x00 or '\\' byte in them truncates/corrupts the string - inferred bug), mod, modversion, svonly, cldll.
- **SVC_PlayerInfo 0x9AFB0** ('D', 2 KB buffer) and **SVC_RuleInfo 0x9B230** ('E', cvars with FCVAR_SERVER;
  "none" for empty password) are the HL formats (fields not re-verified here).
- So the engine's GoldSrc queries work over UDP on the engine port, and they leak the session key.

## 4. Xbox system link (front end, not GoldSrc)

### 4.1 Sockets (lobby object 0x554208, setup 0xEBA50, "SystemLinkSetup" etc. string table at 0xE99BA..)
XNetStartup params at 0xEBAA8: size 0x0C, flags 1 (XNET_STARTUP_BYPASS_SECURITY) when +0xC6C == 0 (not Live)
else 0, private pool 0x18 pages, enet rx queue 0x10, ipfrag 8, max sockets 0x80, recv/send buffers 32 KB; WSAStartup
0x202. Sockets (all non-blocking, FIONBIO 0x8004667E):
| obj field | socket | port | use |
|---|---|---|---|
| +0x140 | UDP (2,2,17), SO_BROADCAST | 0x44D = **1101** | discovery broadcast ("Broadcast socket open") |
| +0x144 | VDP (2,2,0xFE) | 0x44E = **1102** | lobby datagrams ("Direct socket open") |
| +0x148 | TCP (2,1,6) | 0x44F = 1103 | lobby client→host stream |
| +0x14C | TCP | 0x450 = 1104 | lobby host listen (inferred) |
Engine game traffic is separate: UDP_OpenSocket 0x6CDE0 via NET_OpenIP 0x6CFD0; server port = ip_hostport /
hostport / **port (cvar 0x37874C, default "1001")**, client port = ip_clientport / **clientport (0x378774,
default "1000")**, bind address cvar ip (0x378710, "localhost"); SO_BROADCAST set, -reuse → SO_REUSEADDR.

### 4.2 Lobby message format
`u16 len | u8 type | body`, len = fixed size for the type from 0xE88B0 (jump table 0xE89D4) plus a variable part
from 0xE8A20 (only type 7: 2 + count(byte at +3) x 10); bytes on the wire = len + 2. Fixed sizes: 0:0x0D,
1:0x82, 2:0x2A, 3:0x552, 4:2, 5:0x4F, 6:0x0A, 7:2+10n, 8:0x0A, 9:0x0B, 10:4, 11:3, 12:2, 13:2, 14:3, 15:0x11,
16:2, 17:2. Receive: broadcast socket via recv 0xED510 → 0xED9A0 (types 0, 1); VDP socket via recvfrom 0xED5B0
(0x554-byte buffer) → 0xED9D0 (types 2..0x11, jump table 0xEDC2C). Send: 0xED270 (host: TCP to each peer;
client: socket +0x148; else UDP sendto on +0x144).
Handlers (receive): 2 → 0xEE3A0 (uses XNetInAddrToXnAddr: host learns a joiner), 3 → 0xEE820 (XnAddrToInAddr),
4 → 0xEEC30, 5 → 0xEEC50, 6 → 0xEEE40, 7 → 0xEF130, 8 → 0xEF550 ("Player %> is now on Team %d"), 9 → 0xEF220,
10 → 0xEDC70 (back to menu), 11 → 0xEF2E0 (game launch / map change, sent 5x by 0xECA00 per the Q7 fork),
12 → 0xEF3C0, 13 → 0xEF3E0, 14 → 0xEF410 (PlayerMapVoting), 15 → 0xEF500 ("#Game_someone_new_is_here"),
16 → sets 0x5523F9, 17 → stops sounds and loads (0x106380, 0xBEE90). Senders (first type built): 0xE9C00:16,
0xE9FA0:17, 0xEC270:2, 0xEC4D0:3, 0xEC770:4, 0xEC7C0:5, 0xEC830:6, 0xEC980:9, 0xECA00:11, 0xECB10:12,
0xECBA0:13, 0xECCE0:14, 0xECD40:15, 0xECDA0:8. Reservation flow 0xEE04A ("SUCCESSFUL SEND OF RESERVATION
CANJOIN"). Exact semantics of 2..7, 9, 12, 13, 16 not decoded (inferred: join request/accept, player list, chat).

### 4.3 Discovery
- **Search** (client) 0xEBDF0: type 0, 0x0F bytes to 255.255.255.255:1101 on the broadcast socket:
  `[u16 0x0D][u8 0][8-byte nonce = obj+0x6F0][u32 version: 0x54 when obj+0xC6C (Live) else 0x43C]`.
- **Host answer** 0xEDD80 → 0xEBEE0: only if obj+0xD4 (hosting), player count != maxhumanplayers (cvar 0x3786E8,
  default "16") and the version dword matches. Broadcast (not unicast) to 255.255.255.255:1101, type 1,
  0x84 bytes. Body (offset from packet start):
  | off | size | content |
  |---|---|---|
  | +0x03 | 8 | requester's nonce (echo) |
  | +0x0B | 8 | host instance id (obj+0x6F0) |
  | +0x13 | 8 | XNKID (obj+0xE8, from XNetCreateKey 0xF0D40) |
  | +0x1B | 16 | XNKEY (obj+0xF0) |
  | +0x2B | 36 | host XNADDR (obj+0x100, XNetGetTitleXnAddr 0xF12A8) |
  | +0x4F | 12 | obj+0x6C8 (session settings blob, inferred) |
  | +0x67 | 16 | host name (gamertag table 0x56191C or the wide nickname 0x552F78 converted) |
  | +0x77 | 1 | players = lobby players (vector +0x3C, 0x30 each) + joiners (+0x6BC, 0x10 each) - requester if listed + 1 if mp_dedicated_server (0x377720) == 0 |
  | +0x78 | 1 | maxhumanplayers |
  | +0x79 | 1 | flags (0x5523B8 bit 0, 0x5523BC bit 1) |
  | +0x7A | 1 | mp_dedicated_server > 0 |
  | +0x7B | 1 | mp_player_voting (0x377734) > 0 |
  | +0x7C | 1 | bot_quota (0x37BF90) or bot_quota_match (0x37BFA4) > 0 |
  | +0x7D | 1 | rounds left = mp_maxrounds (0x37D6DC) - rounds played (0x552ACC), or 0xFF |
  | +0x7E | 4 | game DLL value 0x37CA88 (inferred: map / game mode id) |
  | +0x82 | 1 | mp_match_option_preset (0x37D768) |
  | +0x83 | 1 | mp_friendlyfire (0x37D498) |
- **Client session list** 0xEDE70 (non-host only): accepts the answer only when the echoed nonce equals its own
  obj+0x6F0; stores a 0x7C-byte entry (host id, XNKID, XNKEY, XNADDR, settings, name, counts) through 0xF18D0
  into the vector at obj+0x18 (begin +0x1C, end +0x20), at most **50** (0x32) entries.

### 4.4 Join and launch
- Host: 0xF07D0 sets mp_* cvars, map, maxhumanplayers, bot_quota..., XNetCreateKey(obj+0xE8, obj+0xF0) and
  XNetRegisterKey (0xF0D40/0xF0D49).
- Client: 0xF0070 (via 0xEB490): XNetRegisterKey(host XNKID, XNKEY), XNetXnAddrToInAddr(host XNADDR, XNKID,
  &obj+0x124), XNetInAddrToString → obj+0x12C (= 0x554334, the address CL_Connect_f uses), TCP connect to the
  host (0x262FE8 region), copies the session entry to 0x526FD8.
- Game launch: the front end writes "+connect 0.0.0.0\n" to T:\releasemap.txt and Z:\releasemap.txt (0xF166A,
  0xF68FA, 0xF6B96) and relaunches (0xBF0E0, XLaunchNewImage). After the relaunch 0xF1770 restores the instance
  id (obj+0x6F0 from launch data 0x525CC9) and the wanted host id (obj+0x6F8 from 0x525D0D); if the command line
  (0x552068) holds "+connect" and "0.0.0.0" it sets rejoin mode (obj+0xC70); 0xEA690 then re-broadcasts the search
  and picks the session whose host id matches obj+0x6F8 (up to 8 tries, 0x5BD9BC, then state 9 = failure), and
  re-registers the key so 0x554334 holds the host's address again. A debug path joins the session whose XNADDR
  IP "%d.%d.%d.%d" appears on the command line ("Connecting to host %d (%s)", 0xEAE16). The title template
  0x33AA60 ("-game cstrike -maxplayers 17 ...") and "+connect" (0x33AAC8) are assembled at 0xBF470.

## 5. Rcon and remote commands (Q9)
- Rcon challenge 0x9A150: same 1024-entry table; reply "challenge rcon %u".
- SV_Rcon 0x9BBC0 → validation 0x9BA70: argc >= 3, rcon_password (cvar 0x379600, default "") non-empty, ban
  rate limit 0x9B9F0 (sv_rcon_banpenalty 0x3799B8 "0", sv_rcon_minfailures / maxfailures / minfailuretime:
  "Banning %s for rcon hacking attempts" + addip), then compares argv[2] to rcon_password (failure recorded by
  0x9B6B0). **argv[1] (the challenge) is never checked** (the table is not read; see 2.3). Command length
  < 0x1FF. On success: skips 3 tokens, Cmd_ExecuteString(text, src_command) 0x3CA10 with console output
  redirected to the sender (0x6391A4 = 2, address 0x636DE8, flush 0x9B570). Default empty password = rcon off.
  Client side: rcon command 0x2A280 ("challenge rcon" request, rcon_address / rcon_port / rcon_password).
- **clc_stringcmd** SV_ParseStringCommand 0xAB4E0: SV_IsClientCommandAllowed 0xAA680 compares the first token
  (case-insensitive) with the table 0x379B60 (NULL-terminated): status, god, notarget, fly, name, noclip, kill,
  pause, spawn, new, sendres, dropclient, kick, ping, dlfile, nextdl, setinfo, showinfo, sendents, fullupdate,
  setpause, unpause, setheadnumber. Allowed → Cmd_ExecuteString(text, src_client = 0); otherwise the text is cut
  to 127 chars, tokenized and passed to the game DLL's pfnClientCommand (`*0x5EF1EC`) with the client's edict
  (say, buy, vban, jointeam, ... are game DLL commands). Whether "kick"/"pause"/"god" refuse src_client is up to
  each command (inferred from HL: they check cmd_source / privileges; not verified here).
- **svc_stufftext** (svc 9, handler 0x30A80): MSG_ReadString → Cbuf_AddText 0x3B970 with no filtering: the
  server can run any client console command.

## 6. Unanswered / not verified
- Semantics of lobby types 2..7, 9, 12, 13, 16 and the TCP lobby stream protocol (0xED270 sends; receive side on
  TCP not traced). The 12-byte settings blob at obj+0x6C8. Value 0x37CA88.
- The XNet security model as emulated by this recomp's HLE (whether XNetXnAddrToInAddr returns the real IP).
- SVC_PlayerInfo / RuleInfo byte layouts not re-read; the 'K'/'N' OOB transfer's purpose.
- Where SV_DropClient tells the lobby (0xEF930, from the Q7 fork, not re-checked here).

# Part C: snapshots, client side, voice (questions 4, 5, 6)

Static reading of build/ghidra/default_flat.exe (original XBE code, before the recomp's patches/hooks). Guest VAs.
"inferred" = not proven from the code here (HL1 SDK-era structure or naming only).

Globals used throughout: svs.clients 0x636CE4, svs.maxclients 0x636CE8, host_client 0x6CDFEC, realtime 0x6CE040 (double),
host_frametime 0x6CE0E0 (double), sv.time 0x5EF7D0 (double), sv.active 0x5EF7C0, net_message data/cursize 0x6BA588/0x6BA590,
msg_readcount 0x748404, msg_badread 0x748524. client_t stride 0x5010.

client_t fields seen in this part (offsets from client_t):
| off | field | evidence |
|---|---|---|
| +0x00/+0x04/+0x08/+0x0C | active / spawned / fully connected / connected (inferred names) | 0x9E430-0x9E43C, 0x9E487-0x9E493 |
| +0x20 | netchan_t; +0x24 remote address type (1 = NA_LOOPBACK, inferred) | 0x9E476, 0x9E599 |
| +0x3C | netchan.last_received (float) | 0x9E554 |
| +0x48 | netchan.rate (double, bytes/s) | 0x9E994, 0x9C224 |
| +0x50 | netchan.cleartime (double) | 0x9E5A4, 0x9E5FA |
| +0x58 / +0x5C / +0x68 | netchan incoming_sequence / incoming_acknowledged / outgoing_sequence | 0xAB812, 0xABEC0, 0x9DCFB |
| +0x80 | netchan.message flags (bit 2 = overflowed) | 0x9E4BA |
| +0x2540 | chokecount | 0x9E60A |
| +0x2544 | delta_sequence (-1 = no delta) | 0xAB550, 0xABF5A, 0x99A5E |
| +0x2548 | fakeclient (bots get no packets) | 0x9E442 |
| +0x254C | proxy (HLTV, from userinfo "*hltv") | 0x9EAFA |
| +0x2550 | lastcmd (usercmd_t, 0x34 bytes) | 0xABBB3 |
| +0x25A0 | latency (float, s) | 0xABF43 |
| +0x25A4 | packet_loss (float, %) | 0xABA1E |
| +0x25B0 | next ping time (HLTV only) | 0x9DA00 |
| +0x25C0 | unreliable datagram sizebuf (data +0x25C8, maxsize +0x25CC, cursize +0x25D0) | 0x9DFE8 |
| +0x3578 | connection start time | 0xABF13 |
| +0x3580 | next_messagetime (double) | 0x9E5CA |
| +0x3588 | next_messageinterval (double, 1/updaterate) | 0x9EA45 |
| +0x3590 / +0x3594 | send_message / skip_message | 0x9E47B, 0x9E44E |
| +0x3598 | frames (client_frame_t[SV_UPDATE_BACKUP], 0x828 bytes each) | 0x9DD05 |
| +0x4B9C / +0x4BA0 | edict / pViewEntity | 0x9DD1C, 0x9DD2F |
| +0x4EFC / +0x4F00 | lw (cl_lw) / lc (cl_lc) | 0x9EA7B, 0x9EABA |
| +0x5004 | voice loopback flag (bit 7 of clc_move's loss byte) | 0xAB86F |
| +0x5008 | voice listen bits (patched to a separate table in G8.2B) | 0xABDF8 |

client_frame_t (0x828): +0x0 senttime (double, set to realtime when built: 0x97745), +0x8 ping_time (float, -1 at build:
0x9774A), +0x3FC packet_entities_t (num_entities at +0x3FC, entity state pointers from +0x420; states are pointers into the
shared refcounted pool 0x95300-0x95660).

## 4. Snapshots

### 4.1 When a snapshot is sent: SV_SendClientMessages 0x9E3F0
Called once per server frame. Order:
1. SV_UpdateToReliableMessages 0x9E080 (per-client name/userinfo updates, copies sv.reliable_datagram 0x62B434.. and
   sv.datagram 0x62C3E4.. into each client's netchan.message / datagram).
2. Resets the per-frame unique entity-state count 0x551F30 (limit 1024: Sys_Error at 0x9562A).
3. For each client slot (0..maxclients-1):
   - skipped if not active/connected, or fakeclient (+0x2548: bots get no packets), or skip_message (+0x3594, cleared).
   - `host_limitlocal` (cvar 0x377DD4, default "0") == 0 and the client is loopback -> send_message every frame (0x9E461).
   - else send_message when `realtime + host_frametime >= next_messagetime` (0x9E495-0x9E4AE).
   - netchan.message overflowed -> clear it and the datagram, "%s overflowed" (0x338140 / 0x33811C), drop client
     (0x5C600 with 0x338100), cleartime = 0, send_message = 1 (0x9E4BA-0x9E546).
   - `sv_failuretime` (cvar 0x37977C, default "0.5"): if `realtime - netchan.last_received > sv_failuretime`, the snapshot
     is withheld (send_message = 0, 0x9E554-0x9E56C): a client that has not been heard from for 0.5 s gets no snapshots.
   - Rate gate (Netchan_CanPacket inlined): loopback with `net_chokeloop` (0x3786D4, default "0") == 0 always passes
     (cleartime = realtime); otherwise passes when `netchan.cleartime < realtime`.
     - passes: next_messagetime = realtime + next_messageinterval + host_frametime (0x9E5AD-0x9E5CA); a spawned, fully
       connected client gets SV_SendClientDatagram 0x9DF30, others an empty Netchan_Transmit (0x9E678).
     - fails: chokecount++ (+0x2540, reported to the client in svc_choke/"choke" stats). Xbox/2003 addition (inferred from
       HL1 comparison): if the netchan has a reliable fragment waiting (+0x1FDC, netchan +0x1FBC: per-stream "waiting"
       flag set at 0x6A229), it sends that reliable now anyway, with the global 0x3799CC ("send even if empty", default 1)
       cleared, and restores cleartime afterwards (0x9E61E-0x9E673): reliable data is not starved by choke.
4. Tail call 0x9DEF0: clears per-edict effects bits (`&= ~0x22`, edict +0x198: EF_MUZZLEFLASH|EF_NOINTERP style one-frame
   effects, inferred).

### 4.2 Update rate
- Client userinfo `cl_updaterate` parsed in SV_ExtractFromUserinfo (0x9E6C0; key at 0x9E9FA): value < 10 -> 10; if > 0,
  next_messageinterval = 1.0 / value (0x9EA24-0x9EA45). No upper bound here.
- SV_CheckUpdateRate 0x98800 (called at 0x9EB0E): interval 0 -> 0.05 (20 Hz). `sv_maxupdaterate` (0x37963C, default
  "60.0") and `sv_minupdaterate` (0x379650, default "10.0"): a value under 0.001 (but not 0) is reset to "30.0" / "1.0";
  then interval is clamped to [1/sv_maxupdaterate, 1/sv_minupdaterate].
- Client cvar `cl_updaterate` (0x371C30) default "20", flags 3 (archive | userinfo). Client side (0x2AB50): if
  cl_updaterate < 10 it is reset to "20" with "cl_updaterate minimum is %f, resetting to default (20)" (0x32B510).
- So default: 20 snapshots/s, server maximum 60/s. Loopback with host_limitlocal 0: every server frame.

### 4.3 Rate (bandwidth)
- Userinfo `rate` (client cvar 0x371C1C default "4000", flags 3) clamped 1000..20000 bytes/s into netchan.rate
  (0x9E965-0x9E994).
- SV_ClampRate 0x9C1E0 (from 0x9EB15): `sv_maxrate` (0x37968C, default "0" = off) caps rate at min(sv_maxrate, 20000);
  `sv_minrate` (0x379678, default "0") raises it to max(sv_minrate, 1000).
- Netchan (0x6A005): if `sv_lan` (0x3797B8, default "0") != 0 and `sv_lan_rate` (0x3797CC, default "20000.0") > 1000 the
  per-byte time is 1/sv_lan_rate, otherwise 1/netchan.rate (cleartime accounting is the parent netchan section's).

### 4.4 Building the packet: SV_SendClientDatagram 0x9DF30
4000-byte stack sizebuf "Client Datagram" (0x9DF54, overflow allowed). Content in order: svc_time (7) + float sv.time;
SV_WriteClientdataToMessage 0x976F0 (frame.senttime = realtime, ping_time = -1, svc_clientdata delta against the
delta_sequence frame, weapon data when lw); SV_WriteEntitiesToClient 0x9DCE0; then the client's unreliable datagram
(+0x25C0) unless the message overflowed ("WARNING: msg overflowed for %s" 0x338150 drops the entities;
"WARNING: datagram overflowed for %s" 0x338170 drops the datagram). Netchan_Transmit(&client->netchan, size, data) at 0x9E052.
(The recomp's G8.2B change enlarges the loopback buffer; see FINDINGS "Packet overflow".)

### 4.5 SV_WriteEntitiesToClient 0x9DCE0 (per-client visibility)
- frame = client->frames[netchan.outgoing_sequence % SV_UPDATE_BACKUP] (0x9DCFB-0x9DD28).
- gEntityInterface.pfnSetupVisibility(viewent +0x4BA0, edict +0x4B9C, &pvs, &pas) via *0x5EF234 (0x9DD38).
  gEntityInterface (DLL_FUNCTIONS) is at 0x5EF1A0, copied from the game DLL's static table 0x37BDB0 (0x32 dwords,
  0xACCFC-0xACD0B): SetupVisibility = 0x1422E0, UpdateClientData = 0x1431F0, AddToFullPack = 0x142520,
  CreateBaseline = 0x142950, RegisterEncoders = 0x142ED0, AllowLagCompensation = 0x18D460 (returns 1).
- 0x95250 (per-frame entity state prep, inferred), 0x9D9F0 SV_ShouldUpdatePing: HLTV proxies every 2 s; everyone else
  SV_CalcPing 0x9C900 then `lastcmd.buttons & IN_SCORE (0x8000)`: **pings (svc_pings) are sent only while the player
  holds the scoreboard** (0x9DA30-0x9DA3A).
- Loop e = 1 .. sv.num_edicts-1 (0x62B418): for e <= maxclients the client must be active or spawned and not a proxy
  (0x9DDB8-0x9DDEB); full pack capacity **MAX_PACKET_ENTITIES 256** (`cmp $0x100` 0x9DDED; 256 x 0xFB states on the stack,
  0xFB2C frame); each candidate goes through gEntityInterface.pfnAddToFullPack(state, e, ent, host, hostflags = lw != 0,
  player, pSet) via *0x5EF23C (0x9DE1E). Past 256 the rest are silently dropped.
- 0x952D0 copies the full pack into the frame (shared refcounted states, 0x95300-0x95660).
- SV_EmitPacketEntities 0x9D3F0 with delta flag = (delta_sequence != -1) (0x9DE64-0x9DEA5).
- SV_EmitEvents 0x9CC20 (count capped 31 per update, see analysis/player_slots_engine_server.md), then SV_EmitPings
  0x9DBE0 if the ping test passed.

### 4.6 Game DLL AddToFullPack 0x142520 (PVS culling)
- Skip if EF_NODRAW (edict +0x198 bit 7) unless it is the host itself; skip if no modelindex (+0x134) or empty model
  string (+0x138 + gpGlobals->pStringBase 0x3791EC); skip FL_SPECTATOR (flags +0x224 & 0x4000000) unless host.
- PVS: for ent != host, a per-host visibility cache (0x142480 / 0x142440 reset it when the host's PVS leaf changed;
  0x1424E0 lookup), otherwise engine CheckVisibility (*0x527C98)(ent, pSet); result cached in 0x5283F0[host*0x3A3 + e]
  (931 entries per host: cache sized for the original edict count, inferred). Not visible -> 0.
- FL_SKIPLOCALHOST (flags & 0x100) with hostflags&1 and owner == host -> skip (client-side weapons predicted locally).
- groupinfo (+0x2C0) filtering via *0x527CBC follows (0x142608).
- Only PVS; there is no distance/priority culling and no limit per type besides 256 total.

### 4.7 Delta against the acknowledged frame
- SV_UPDATE_BACKUP 0x379970 / mask 0x379974: static 8/7, set to `maxclients == 1 ? 8 : 16` in SV_SetMaxclients
  (0xA1A1A) and SV_SpawnServer (0xA0117); frames allocated by 0x951E0 (malloc(UPDATE_BACKUP*0x828) per client).
  With 65 slots: 16.
- The client asks for a delta with clc_delta (4) + byte sequence; handler 0xAB550 stores it in delta_sequence
  (badread -> -1). SV_ExecuteClientMessage 0xABEC0 resets delta_sequence to -1 at the start of every client packet
  (0xABF5A), so only a packet carrying clc_delta enables delta for the next snapshot.
- SV_EmitPacketEntities 0x9D3F0: delta -> `svc_deltapacketentities` (0x29), u16 entity count, byte delta_sequence;
  from-frame = frames[delta_sequence % SV_UPDATE_BACKUP] (0x9D40D-0x9D46B). No delta -> `svc_packetentities` (0x28),
  u16 count, everything against baselines (0x9D471-0x9D49B). The server does **not** check the age of the requested
  frame itself (the byte is taken mod 16); the age check is on the client (below).
- Entity header SV_WriteDeltaHeader 0x9D1B0: 1 bit "next entity" (delta update, number = previous + 1) or "remove" flag;
  otherwise 1 bit + 6-bit number delta (1..63) or 1 bit + 11-bit absolute number; then (not removed) 1 bit custom
  (beam) entity; if sv.instanced_baselines count (0x62B428) != 0, 1 bit + 6-bit instanced baseline index; for full
  updates, 1 bit + 6-bit baseline offset ("from an earlier entity"). End of list: 16 zero bits (0x9D98A, per
  analysis/player_slots_engine_server.md). Player entities (1..maxclients) use the player delta 0x551F08, others the
  entity delta 0x551F0C (same analysis file).
- Client fallback, CL_ParsePacketEntities 0x242F0: for a delta packet reads the from-sequence byte; if
  `(incoming_sequence 0x781320 - from) & 0xFF >= CL_UPDATE_MASK (0x371CF4 = 15)` the packet's entities are flushed
  (CL_FlushEntityPacket 0x23510, which zeroes cl.validsequence 0x74A05C at 0x235BC) and parsing stops (0x24531-0x24565).
  A full update received while delta was expected prints "WARNING: delta on full update" and zeroes validsequence
  (0x248EA-0x248EF). A full update while waiting for a demo start sends the stringcmd "fullupdate" (0x244A5-0x244D3).
  Overflows of 256: "CL_ParsePacketEntities: newindex == MAX_PACKET_ENTITIES" (0x2466A, 0x247C3, 0x24A1A).
- Client sends clc_delta only when cl.validsequence != 0, `cl_nodelta` (0x371A8C, default "0") is 0, state is active (5)
  and not recording a demo needing a full frame (0x2B231-0x2B2A3). "WARNING: Connection Problem" (0x32B49C): when
  `outgoing_sequence - incoming_acknowledged >= 15` and `realtime - last_received > 15.0` (0x366BA0), validsequence = 0
  (0x2B123-0x2B191): the next snapshot is a full (uncompressed) one.
- So the uncompressed fallback is: the client stops sending clc_delta (validsequence 0); the server, seeing no
  clc_delta in that packet, sends svc_packetentities against baselines.
- CL_UPDATE_BACKUP 0x371CF0 = 16 (static, never written), mask 0x371CF4 = 15. Client frame ring cl.frames at 0x74A2A0
  (stride 0x1B60; the player states moved to a guest table in G8.2B).

### 4.8 delta.lst encoding (what the code shows)
- delta.lst loaded by the server at 0xA1F32 (string 0x33789C); parser strings DELTA_ParseDescription / DEFINE_DELTA /
  DEFINE_DELTA_POST, encoders "none" / gamedll funcname / clientdll funcname, types DT_BYTE, DT_SHORT, DT_FLOAT,
  DT_INTEGER, DT_ANGLE, DT_TIMEWINDOW_8, DT_TIMEWINDOW_BIG, DT_STRING, DT_SIGNED (0x32E8C0-0x32E980). Descriptions are sent
  to clients with svc_deltadescription (14, client handler 0x2FAE0). The file itself lives in the SXWad packs and was not
  read here.
- DELTA_WriteDelta 0x44840: marks changed fields (0x44230), if any changed (or forced): builds the field bitmask (0x444B0),
  optional custom encoder callback, writes 3 bits = mask byte count, then that many 8-bit mask bytes (0x448A9-0x448CF),
  then each set field (0x44530): field descriptor stride 0x44 (type flags at +0, bit 31 = DT_SIGNED; offset +0x24, bit
  count +0x2C, multiplier +0x30): floats are value x multiplier rounded and written with that many bits (signed writer
  0x3E350 if DT_SIGNED); DT_TIMEWINDOW_8 writes (sv.time*100 - value*100) in 8 signed bits (0x445C3-0x445EE, 100.0 at
  0x365E64 / 0x365E78). Unknown types: "unknown send field type" (0x44735).
- Stats: delta_stats / delta_clear console commands (0x32E600 / 0x32E5F4).

## 5. Client side

### 5.1 Usercmd packet: CL_SendMove 0x2AC80 (the 0x2CFEB site is a different function, 0x2CFA0)
- cls.state 0x7812E0; state 3 (connected, not active) and not playing a demo: only an empty Netchan_Transmit of
  cls.netchan 0x7812E8 (0x2ACC5-0x2ACDD).
- numbackup = `cl_cmdbackup` (0x3719D8, default "2", flags archive) clamped 0..8 (0x2ACFD-0x2AD1B).
- 2048-byte buffer (0x2AD44). Command slot cl.commands[outgoing_sequence (0x781330) % CL_UPDATE_BACKUP], stride 0x4C at
  0x7658A0 (senttime, receivedtime -1, heldback etc.).
- CL_CreateMove through the client DLL (*0x7D6CD8), then lerp_msec (0x2AB50, see 5.3) and weapon select byte.
- Writes `clc_move` (2), a length byte and a checksum byte (placeholders), a byte = packet loss % (0..100, from
  0x785618) | 0x80 if `voice_loopback` (0x37B194) is on; cmd.msec = min(frametime*1000, 255).
- CL_PredictMove 0x32EB0 (5.2).
- `cl_cmdrate` (0x371B2C, default "30", flags archive; NOT userinfo): < 10 -> reset to "10" (0x2AF0B-0x2AF2B).
  Gate: unless loopback with host_limitlocal 0 (or 0x778A5C == 1), if `realtime < next cmd time (0x785248)` or
  Netchan_CanPacket (0x69730) fails, the command is held back: commands[seq].heldback = 1, outgoing_sequence++ (no
  packet) (0x2AF5E-0x2AF9E). Otherwise next cmd time = realtime + 1/cl_cmdrate.
- Packet body: byte numbackup, byte numnew = outgoing_sequence - last outgoing command (0x78524C), capped at 16; then
  numbackup + numnew usercmds delta-coded against the previous one (MSG_WriteUsercmd 0x3E080, oldest first)
  (0x2B002-0x2B094). Length byte (<= 255) and checksum = COM_BlockSequenceCRCByte 0x41120 over the body with the
  sequence; body then munged with COM_Munge 0x40050 keyed by the sequence (0x2B096-0x2B0FD).
- clc_delta (4) + byte validsequence (4.7), voice (CL_AddVoiceToDatagram 0x5D120), then cls.datagram (0x783808) appended if
  it fits, Netchan_Transmit (0x2B200-0x2B20C).

### 5.2 Server parse: SV_ParseMove 0xAB710 (clc_move handler)
- Only one clc_move per packet (flag 0x551F90, reset in SV_ExecuteClientMessage 0xABED6).
- frame = frames[incoming_acknowledged % SV_UPDATE_BACKUP]; reads length, checksum, loss byte (bit 7 -> client +0x5004
  voice loopback, low 7 bits -> packet_loss +0x25A4), numbackup, numnew; un-munges with COM_UnMunge 0x40140.
- **CMD_MAXBACKUP**: numbackup + numnew must be 0..28 (`cmp $0x1D`, 0xAB8F5), else "SV_ReadClientMessage: too many cmds
  %i sent for %s/%s" (0x339420) and the client is dropped with "CMD_MAXBACKUP hit" (0x33940C) (0xABC35-0xABC89).
- Usercmds read by MSG_ReadUsercmd 0x3EC30 (0x34 bytes each). Badread -> "Client %s:%s sent a bogus command packet"
  (0x3393E0). Checksum mismatch -> silently ignored (badread set, no message) (0xAB9B8-0xAB9FA).
- Paused (0x5EF7C4) / single player non-moving cases only copy view angles (0xABA24-0xABA72).
- net_drop (0x6CA720) = packets lost since the last one; if < 24: lost commands beyond the backup are replayed with
  lastcmd, then the backup commands, then the new ones, each through SV_RunCmd 0xA9550 with its sequence (0xABAF6-0xABBAB).
  24 or more lost: only the new commands run. lastcmd saved (+0x2550). frame.ping_time -= cmd.msec * 0.0005 (half the
  last command's duration; 0x366878) (0xABBCA-0xABBFE).
- Lag compensation is applied inside SV_RunCmd (SV_SetupMove 0xAAAC0 at 0xA9659, SV_RestoreMove 0xAB3A0 at 0xAA670).

### 5.3 Interpolation
- `ex_interp` (0x371938) default "0.1", flags 0.
- 0x2AB50 (each command): min interp = max(1, 1000/cl_updaterate) ms; ex_interp*1000 + 1 below it -> "ex_interp forced up
  to %i msec"; above 100 ms -> "ex_interp forced down to %i msec" (0x32B4F0 / 0x32B4CC), cvar rewritten; the value (ms,
  clamped to [min, 100]) is smoothed by 0x2AAB0 (slew at 0.05 per host frame? 0x366210 = 0.05, inferred) and stored
  as the usercmd's lerp_msec (0..100).
- Entities are drawn at cl.time - ex_interp (0x23820 -> 0x265D0 CL_FindInterpolationUpdates, inferred name);
  CL_LerpPoint 0x26450 divides by ex_interp when > 0.001 (0x365EC0).

### 5.4 Prediction
- No cl_predict / cl_nopred cvar exists (not in the strings): local player movement is always predicted in multiplayer.
- CL_PredictMove 0x32EB0: only when active (5), not playing a demo, not paused (0x74A27C), cl.validsequence != 0, and
  `outgoing_sequence - incoming_acknowledged + 1 < 15` (too many unacknowledged commands -> no prediction this frame)
  (0x32EBF-0x32F23). Starts from the last received frame's own player state (cl.frames[parsecount 0x74A064] player
  state 0xFB and client data / weapon data 0x880 bytes) into the predicted ring (stride 0xB4C at 0x765D60), then runs
  the unacknowledged commands. Prediction error print "pred. error %.3f units" (0x32D01C, in 0x323B0).
- `cl_lw` (0x371C44, default "1", flags archive|userinfo): client-side weapons (server: client +0x4EFC, used as
  AddToFullPack hostflags and in SV_SetupMove). `cl_lc` (0x371C58, default "1", archive|userinfo): lag compensation wanted
  (+0x4F00).

### 5.5 Lag compensation (server)
- Cvars: `sv_unlag` (0x379C4C) "1", `sv_maxunlag` (0x379C60) "0.5", `sv_unlagpush` (0x379C74) "0.0",
  `sv_unlagsamples` (0x379C88) "1" (all flags 0).
- SV_ComputeLatency 0xAA700 (from SV_ExecuteClientMessage 0xABF3E): average of the last sv_unlagsamples (clamped to
  1..min(16, SV_UPDATE_BACKUP)) acknowledged frames' ping_time, stored in client +0x25A0. ping_time per frame =
  realtime - frame.senttime - next_messageinterval (0xABEC0-0xABEEF), 0 in the first 2 s of the connection when negative.
- SV_SetupMove 0xAAAC0: clears truepositions (0x4E46C0, 17 x 0x58 in the original, a guest table now); requires
  gEntityInterface.AllowLagCompensation (*0x5EF264 -> 0x18D460, always 1), sv_unlag != 0, the client's lw AND lc,
  maxclients > 1, client active. Saves every other active player's origin/mins/maxs (unrolled x4 loop).
  latency clamped to 1.5 s (0x365F28) and to sv_maxunlag (negative sv_maxunlag reset to 0 via 0x421F0);
  lerp = lastcmd.lerp_msec/1000 clamped to <= 0.1 s (0x365EAC) and >= next_messageinterval;
  target = realtime - latency - lerp + sv_unlagpush, at most realtime (0xAAF0B-0xAAFE9).
- Walks the client's frame ring back from outgoing_sequence-1 for the newest frame with senttime <= target; if the
  target is more than 1.0 s (0x365D48) before that frame's senttime, gives up (truepositions cleared, no rewind)
  (0xAB12A-0xAB161). Interpolates player positions between that frame and the next newer one (fraction clamped 0..1;
  no interpolation across a teleport, 0xAA960, or EF_NOINTERP), moves each rewound player's edict and relinks it
  (0xB79C0); SV_RestoreMove 0xAB3A0 puts them back.
- **Observation:** both loops accept entity numbers `1 <= n < maxclients` (`cmpl 0x636CE8; jge`, 0xAB06F-0xAB07A and
  0xAB227-0xAB22D): the player in the last slot (entity number == maxclients, i.e. player 65) is never rewound. Same
  bound in both places (HL1 off-by-one, inferred); worth fixing for PC multiplayer.

## 6. Voice

- svc table (0x371CF8, {id, name, handler}): svc_voiceinit 0x34 -> 0x31B70, svc_voicedata 0x35 -> 0x31B80.
  clc table (0x379CC8, {id, name, handler}): clc_bad 0, clc_nop 1, clc_move 2 -> 0xAB710, clc_stringcmd 3 -> 0xAB4E0,
  clc_delta 4 -> 0xAB550, clc_resourcelist 5 -> 0xA7DF0, clc_tmove 6 (no handler), clc_fileconsistency 7 -> 0xA80E0,
  clc_voicedata 8 -> 0xABCC0. SV_ExecuteClientMessage dispatches ids 0..9 (0xABFA6), unknown -> drop "Bad command
  character in client command".
- Client send, CL_AddVoiceToDatagram 0x5D120 (from CL_SendMove): when active and the voice system is initialised
  (0x5526F0) and not muted (0x5526B8), Voice_GetCompressedData 0x102440 into a 2048-byte buffer; written to the
  **unreliable** cls.datagram as clc_voicedata (8), u16 length, data, if it fits.
- Server, SV_ParseVoiceData 0xABCC0: u16 length, > 4096 (0x1000) -> "SV_ParseVoiceData: invalid incoming packet."
  (0x3393B0) and ignored; data copied to a 4 KB stack buffer; relayed only if `sv_voiceenable` (0x379CB0, default "1",
  flags 5) != 0; for each receiver (loop bound 17 x 0x5010 at 0xABE8E in the original, raised for 65) whose bit is set
  in the sender's listen mask, or the sender itself (then length 0 unless the sender's voice-loopback flag is set),
  and active/connected: appended to the receiver's unreliable datagram (+0x25C0) as svc_voicedata (0x35), byte sender
  slot index, u16 length, data, only if `cursize + len + 6 < maxsize`, else silently dropped (0xABDCB-0xABE87).
  So voice rides in the snapshot datagram, unreliable, up to 4000 bytes per client per snapshot.
- Client receive, 0x31B80: byte index, u16 length clamped to 8192 (0x2000), entity = index + 1 checked against
  0x778A70; own entity -> local talking ack (0x101F40); otherwise a channel (0x1026A0 find / 0x102620 assign) and
  Voice_AddIncomingData 0x102760. 5 channels of 0x5688 bytes at 0x3855B8..0x3A0660 (0x102124).
- svc_voiceinit: written into the signon buffer (0x62DBC0) by 0x9F7E4 with the codec name: "" when maxclients <= 1,
  otherwise the `sv_voicecodec` string (0x379C9C, default "voice_miles"). The client handler 0x31B70 only reads the string
  (MSG_ReadString 0x3EA90) and ignores it: no codec negotiation.
- The engine voice objects (record 0x5526E8, codec 0x5526EC) are only ever read or cleared by absolute address in the
  listing (0x101EFF, 0x101F18, ...); I found no instruction that stores a live pointer there (inferred: the HL voice
  capture/codec path is not populated on this build, so CL_AddVoiceToDatagram sends nothing).
- Xbox voice: the front end creates an XHV engine (0xE9A50: statically linked XOnline/XHV calls 0x20DC02, 0x20D938 x2
  (processing modes from 0x322124 / 0x3220B8), 0x20D93D; "XHVEngine: Initialized (%d)" 0x340A94) with 16 remote talkers
  (0x10 in the init block at 0xE9B77, inferred field). How XHV chat data reaches the network (engine clc_voicedata or a
  separate XNet path) was not traced: unanswered.
- Client cvars: voice_enable (0x37B1F8) "1", voice_scale (0x37B180) "1", voice_loopback (0x37B194) "0", voice_modenable
  (HUD, 0x362018). Server: sv_voiceenable "1", sv_voicecodec "voice_miles".

## Not answered here
- Exact SetupVisibility (0x1422E0) behaviour (PAS / spectator view) not read.
- The delta.lst field list (it is in the SXWad data, not the code).
- XHV voice transport (see 6).
- 0x95250's exact role (per-frame packet entity setup, inferred).
