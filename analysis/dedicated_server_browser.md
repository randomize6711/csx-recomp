# N0: dedicated server + PC server browser (read-only investigation, 2026-10-24)

Part of N0 (docs/pc_multiplayer.md sections 7-9). Read-only analysis; "inferred" marks what was not verified.

All VAs verified in build/ghidra/default_flat.exe (VA = file offset + 0x10000; the ORIGINAL XBE, before the
build-time [[patch]]es, so 17-slot constants appear as 0x11). "inferred" = not verified.

## A1 Host frame / main loop / dedicated remnants
- Title main loop: 0xADD43..0xADE2C calls engine->Frame through vtable +0x1C on the CEngine object 0x5535B0
  (global ptr 0x379FA4; ctor 0xAECA0 sets vtable 0x339E20; static init 0x23CA10). After each frame [0x5522E8] != 0 ->
  tail jump to the fatal screen 0x108470 (0xADE25..0xADE32).
- CEngine::Frame 0xAEDA0 -> Host_Frame wrapper 0x5D5D0 (setjmp 0x6CE060, host_profile) -> _Host_Frame 0x5D2E0:
  Host_FilterTime 0x5CD00 (fps_max) -> Cbuf_Execute 0x3BB50 -> 0x1CC10 (client) -> [sv.active 0x5EF7C0] CL 0x2AC80 ->
  SV_Frame 0xA1D20 -> 0xA1CC0 (server send) -> CL_ReadPackets 0x28A40 -> CL prediction 0x32EB0/0x323B0 -> voice
  0x101FD0 -> 0x264E0, 0x298F0, 0x297D0, 0x2E230 loop, 0x269E0, 0xB70D0 -> client DLL frame [0x7D6CE8] only when
  cls.state not 0/1 (0x5D4D4..0x5D4F5) -> 0x33B10 (screen/update, inferred) -> 0x55E10 -> 0x2A7F0 -> 0x5D1E0 ->
  Host_Speeds 0x5CFF0 -> 0x30700 -> 0x5E410/0x5E740. Exact names of the client calls are inferred.
- cls.state = 0x7812E0 (values 0..5; 5 = active, e.g. 0xB4628). 0 = ca_dedicated: SV_SetMaxclients 0xA1960 sets
  cls.state = (g_bIsDedicatedServer == 0) at 0xA19DA..0xA19E4; with no -maxplayers and dedicated, maxclients 6 (0xA19CF).
- g_bIsDedicatedServer = 0x5EF670, written once at 0xAD569 from ecx = CEngine::Load's first argument (0xAED30, vtable
  0x339E24; call to 0xAD540 at 0xAED53). The only caller passes **0**: `push 0; call [edx+4]` at 0xADA64/0xADA66.
  PF_IsDedicatedServer = 0x7AF00 (returns [0x5EF670]); game DLL calls it via [0x527C6C].
- Dedicated code is still in the image: Host_Init 0x5DBE6 skips palette/VID/Draw/R/CL init when cls.state==0
  (0x5DBEC -> 0x5DD18); NET_OpenIP 0x6CFD0 Sys_Error "Couldn't allocate dedicated server IP port" (0x6D059) when
  dedicated; "Cannot play demos on a dedicated server." (0x645B7); infostring type 'd'/'l' from cls.state (0x9A7C5,
  0x9AD36); game DLL 0x143F18 execs servercfgfile ("server.cfg", cvar 0x37981C) when IsDedicatedServer, else
  spservercfgfile (0x338E10) for single player, else lservercfgfile ("listenserver.cfg", cvar 0x379830)
  (0x143F23..0x143FFF, ServerCommand "exec %s\n" via [0x527B3C]); string "VENGINE_DEDICATEDEXPORTS_API_VERSION001"
  (0x3398E0, used 0xAE293).
- Strings: no "-dedicated" in the image. sv_lan cvar 0x3797B8 ("0"), sv_password 0x3797A4, rcon_password 0x379600,
  sv_contact 0x379628, sv_visiblemaxplayers 0x37995C, sys_ticrate 0x377CE0 ("100.0"), mapcyclefile 0x3797F4.
  "hostname" (0x331184) is only looked up with Cvar_FindVar 0x41F20 (0x9A758, 0x5E960...) falling back to "" at
  0x372174; no static cvar_t carries that name (inferred: not registered in this build).
- **Xbox "Dedicated Server" host mode exists**: cvar mp_dedicated_server 0x377720 (default "0"), set on the match
  command line from FrontEndUI.txt (system link + Live host Custom Options: "Dedicated Server Yes/No",
  "+mp_dedicated_server 1", help "16 can play, but you can't play on this Xbox"; uidump lines 4096-4108, 7745-7757).
  Effects found: 0xAD98E strstr(cmdline, "+mp_dedicated_server 1") -> 0x379FA8 = 60 (else 30) -> presentation
  interval 0x80000001 vs 0x80000002 (0xAFA06..0xAFA3D); render path 0xB4560 skips the 3D view (0xB5E90) and the
  client HUD redraw ([0x7D6CAC]) when it is non-zero (0xB460A, 0xB4674); UTIL_HumansInGame 0x1606C0 subtracts the host
  (0x160787..0x1607A3); the bot manager 0x162580 adds bots without waiting for a human only when it is non-zero
  (0x162593..0x1625AE); pause menu InGame_RootDedicatedHost/Client (0xF3BB0, 0xCAA00); search reply counts the host
  only when it is 0 (0xEC007..0xEC016). The host's own client still exists (it occupies a slot: "16 can play").

## A2 Server dependencies on client/graphics/audio
- Mod_LoadBrushModel ~0x4AC00 (version check 0x4AC12) and its lump loaders 0x499B0, 0x49C20, 0x4A9B0, 0x49900, 0x491F0,
  0x4AA60, 0x49CE0, 0x4A0A0 (faces; 0x57480 -> 0x56D70 subdivide), 0x4A8E0, 0x4A490, 0x4A2D0, 0x4A600, 0x49AF0, 0x4A7B0:
  no D3D / GL-layer calls (call lists checked). GPU work at map load is the client's world renderer recording push
  buffers (0x1986A6, FINDINGS "missing world") -> skipped with no client / cls.state 0 (inferred).
- D3D device: Direct3D_CreateDevice at 0xAF250 inside video init 0xAF1F0 (loads launcher/loadscreen .xpr), reached
  from 0xAF0A0 (vtable entry 0x339EB4) via 0xAF829. Swap callers 0xAD001 (thread 0xACFE0, ref 0xAD636), 0xAF29B..,
  0xC2BA8 (level loading screen 0xC2AC0), 0xC3E0A. XACT init + dspimage before the device (FINDINGS "Headless boot").
- Front end / UIX: skipped entirely by the launch fixtures (Z:\releasemap.txt + launch.bin; tests/launch/README.md).
- Headless harness: NullBackend (sdk/harness/null_backend.cpp:25-45) only counts draws unless REF/snap; null audio
  pulls the mixer every 10 ms (null_audio.cpp). The guest still runs the client, its renderer code, the GL layer,
  D3D HLE state, XACT/DSOUND guest code and the sound thread (0x103E60). Measured cost with 64 bots (FINDINGS "High
  population performance"): server frame 40% of the game thread, client frame 58% of which Vulkan recording ~35%; so
  headless (no Vulkan) leaves ~23% client-side guest work, and mp_dedicated_server 1 removes the 3D view + HUD guest
  rendering (0xB4604) too (inferred size).

## A3 Host player / 0 local clients
- Xbox dedicated mode keeps a local client (slot used, not a player). With 65 slots: 64 remote/bots + the host's
  client. True 0-client hosting = cls.state 0 path (CEngine::Load(1)): never exercised on Xbox; Host_Init's skip is
  there but the title main (0xAD540 Sys init, the UI loop around 0xADD43, 0x1055D0, 0xBEE90...) is Xbox UI code that
  assumes a client (inferred risk).
- Bots: without mp_dedicated_server 1 the bot manager waits for a human (0x1625A2..0x1625AE).
- Map change = relaunch (FINDINGS 2224); Linux kernel::Relaunch execl() (sdk/runtime/kernel/boot.cpp:204-208) does not
  call xnet::CloseSockets (only the Windows branch does, boot.cpp:184) and sockets are created without CLOEXEC
  (xnet.cpp:483) -> ports 1000/1001/1101-1104 may stay held by the leaked fds after a map change (inferred, untested).

## A4 Match configuration
- Command line in Z:\releasemap.txt (NUL-terminated), e.g. tests/launch/de_aztec_bots64_ct/releasemap.txt:
  +map, +mp_match_option_preset, +wait, +bot_quota, +bot_join_team, +mp_autoteambalance, +mp_limitteams,
  +teampreference, +bot_difficulty, +bot_allow_rogues, +mp_player_voting, +mp_dedicated_server, +mp_timelimit,
  +mp_maxrounds, +mp_winlimit, +mp_roundtime, +mp_freezetime, +mp_buytime, +mp_startmoney, +mp_friendlyfire,
  +mp_c4timer, +bot_auto_vacate, (+pc_player_collision, a host command: src/game/pc_collision.cpp:30).
- Builder 0xF0800 (mp_buytime, mp_dedicated_server, mp_freezetime, mp_friendlyfire, mp_gametype, map/random,
  maxhumanplayers ...); presets at 0x33F6D8..0x33FDB0. Title template "-maxplayers 17 +maxplayers 17" at 0x33AA60.
- Cvar structs: mp_c4timer 0x37D524, mp_timelimit 0x37D484, mp_autoteambalance 0x37D59C, mp_limitteams 0x37D588,
  mp_friendlyfire 0x37D498, mp_maxrounds 0x37D6DC, mp_match_option_preset 0x37D768, mp_forcecamera 0x37D600,
  mp_tkpunish 0x37D5B0, mp_hostagepenalty 0x37D5C4, bot_quota 0x37BF90, bot_quota_match 0x37BFA4, bot_difficulty
  0x37BF54, bot_join_team 0x37C080, bot_allow_rogues 0x37BFCC, bot_auto_vacate 0x37C0A8, maxhumanplayers 0x3786E8
  ("16"), teampreference 0x371BA4, mp_player_voting 0x377734, mp_dedicated_server 0x377720, sv_maxspeed 0x379BFC,
  pausable 0x377E24, mp_logfile 0x3791C0. mp_roundtime/mp_startmoney/mp_freezetime/mp_buytime/mp_winlimit are used by
  name (0x143B01 etc.); their struct addresses not looked up.
- Exec in boot: Host_Init execs valve.rc (0x5DD25 -> Cbuf 0x3B9D0); game rules ctor execs listenserver.cfg (above);
  autoexec.cfg string 0x32D9DC (used 0x3C07E). Whether these files exist in the packs: not checked.

## A5 OS specifics
- Runtime app layer sdk/runtime/app/* (Win32: --instance via command line, settings TOML with join_ip) is excluded
  from harness/window builds (sdk/harness/build.sh:36); Linux uses app_headless.cpp (INSTANCE env, default Settings:
  join_ip always empty -> no F10/Join IP on our builds; no "F10" handler found in the tree).
- Relaunch: Windows CreateProcess + handover + CloseSockets (boot.cpp:172-201); Linux execl (204-208).
- select: on Linux compat/winsock2.h renames select so the recompiled XDK select runs (FINDINGS 497).
- No network namespace on Windows; Linux tests use unshare -rn (run_game.sh) -> a server must run outside it.

## B1 System link discovery
- Network object 0x554208. Commands (Cmd_AddCommand ~0xE99B5..0xE9A0E): SystemLinkSendBrowseRequest 0xE9090 ->
  0xF12F0; SystemLinkStopBrowseRequest 0xE90A0; SystemLinkRefreshServerList 0xE9000 (re-sends every 500 ms:
  0xE902B, then refills the table via 0xE8E00); SystemLinkConnect 0xE8DF0 -> 0xF1540 (writes T:/Z:\releasemap.txt
  "+connect 0.0.0.0\n", relaunches 0xBF0E0); SystemLinkSetup 0xE90F0; Start/EndSession 0xE2D20/0xE6370.
- Sockets (0xEBB49..0xEBDB1): +0x140 UDP port 1101 (0x44D) SO_BROADCAST ("Broadcast socket open"); +0x144 VDP (proto
  0xFE) port 1102; +0x148 TCP 1103; +0x14C TCP 1104. Engine NET_OpenIP: cvars port "1001" (0x37874C), clientport
  "1000" (0x378774), ip "localhost" (0x378710).
- Message header: u16 length, u8 type at +2; sizes by type in 0xE88B0 (type 0 -> 0x0D, 1 -> 0x82, ...).
  Browse request 0xEBDF0: type 0, 8-byte nonce (+0x6F0/+0x6F4), u32 version 0x54 or 0x43C; sendto 255.255.255.255:1101
  (0xEBEBA). Reply 0xEBEE0: type 1: nonce, host XNADDR (0xE9120, 36 bytes), XNKID (+0xE8/+0xEC), key (+0xF0..), XNADDR
  (+0x100), player count (+1 when mp_dedicated_server 0), maxhumanplayers, flags (mp_dedicated_server,
  mp_player_voting, bot_quota/bot_quota_match), mp_maxrounds left, 0x37CA88 (round time, inferred), preset, friendly
  fire, 12 bytes from +0x6C8 (map, inferred); broadcast to 1101 (0xEC245).
- UI: SuperLauncher_BrowseForLocalServers (uidump 8713-8811): table of 3 columns Server Name 58% / Players 17% / Map
  Name 25%, TableVerticalWrap with arrows, details as BackgroundText (Game Type, Dedicated Server, Round Time
  Remaining, Rounds Remaining, Match Options, Friendly Fire); X refresh, A connect, Y host, B back. Rows filled by code
  (0xE8E00/0xF12F0 -> UI row/cell helpers 0xDC2B0/0xDC440, inferred).

## B2 GoldSrc connectionless queries (present)
- SV_ConnectionlessPacket 0x9BE50: ping, 'i', log, getchallenge 0x99D90, challenge 0x9A150, info 0x9AA80(0) /
  details 0x9AA80(1) ('m' reply, 'd'/'l', 'w', + Xbox XNADDR), infostring 0x9A480 (protocol, address, players,
  proxytarget, lan, max, gamedir, description, hostname, map, type, password, os, secure, xbox_key_id, xbox_key,
  xbox_host_address, mod, modversion, svonly, cldll), players 0x9AFB0, rules 0x9B230, connect 0x994D0, pstat, rcon
  0x9BBC0. Answer only when sv.active and maxclients > 1 (0x9A4C8..0x9A4E3). Client side: NET_ParseServerList string
  0x3329A8.

## B3 Menus
- FrontEndUI.txt / InGameUI.txt text screens (FINDINGS "Original menus"); PC screens injected as text at load
  (hooks 0xCDA2E, 0xCDB1A, commands via 0x3CA25; src/game/pc_menu.cpp:437-470 FindTable/ListCell). Rows are static
  text; the system link list is the only dynamic table found (refilled by code), sorting not present (inferred).
