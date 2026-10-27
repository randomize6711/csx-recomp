# Split-screen: up to four local players (N4)

Milestone N4, approved by the user 2026-10-26: native split-screen for 1 to 4 local players, every one a real player
in its own server slot, offline with bots, on LAN and dedicated servers, through the browser and direct IP, and when
the split-screen PC hosts. The original Xbox game has no split-screen (analysis/splitscreen.md). This document is the
architecture decision (N4.0) and what was built (sections 5-13).

## 1. What the engine assumes (evidence)

| Area | What the title has | Consequence |
|---|---|---|
| Client state | One `cl` (0x749D40..0x780DC8, 225 KB), one `cls` (state 0x7812E0, signon 0x784800), cl_entities (0x551890), one predicted player state set (0x771224), one interpolation history | four clients in one process need four copies of every client global, found by audit |
| Client game code | The client DLL is compiled into the same image as the server game code; its globals (gHUD 0x5623D0, the HUD's 18 + 65 player tables, weapons prediction, the view, VGUI) are static data | the same, in the game DLL's data |
| Network | One client socket and endpoint per process (net_engine.cpp ip_sockets[NS_CLIENT]); one loopback pair (0x440C00, exactly two sides) | a second local client has no socket of its own |
| Worlds | One world load per process: a second map load overruns the GL texture table (1500 slots) into com_argv (N1, FINDINGS "PC multiplayer N1"); every connect after the first relaunches the process | four clients in one process would load the map four times: **blocked** |
| Process restarts | A map change, a join, a return to the menus relaunch the whole process (exec) | a relaunch for one player would end the other three |
| Camera and rendering | One r_refdef / ref_params; V_RenderView can draw several views per frame into sub-rectangles (the spectator's picture-in-picture, nextView) but from one client's state | several views of one client, not of several clients |
| HUD | One gHUD, one scoreboard, one set of user-message handlers | per process |
| Input | One active pad (0x3852F8) chosen by START; gameplay keys and the VGUI cursor read that pad only | per process: one player's device each |
| Audio | One DirectSound 3D listener (0x3038A0) | per process: one listener each |
| Menus | One xbui screen stack (0x5523D4) | per process |
| Memory | One guest address space of 64 MB-era layout (pools, guest heap) per process | per process: ~110 MB host memory measured (16.7) |
| Threads | One game thread (the title's main loop), audio and timer threads | one game thread per player |

## 2. Architectures compared

**A. One process, four client contexts.** Every client global (cl, cls, the client DLL's data, gHUD, VGUI, sound,
menus, prediction, the view) swapped or duplicated per player each frame; four network sockets in one engine; four
world loads in one process. The last is a hard blocker (the texture table, section 1) that only an engine rewrite of
the renderer's resource management removes; any global missed in the audit becomes a cross-player bug (one player's
view, ammo or HUD in another's viewport); a map change relaunches everyone at once. Rejected.

**B. One process per local player, composited.** Each local player is an ordinary client process of the game, exactly
as N1-N3's remote players are (tests already run 8 of them against one server): its own client state, socket,
identity, prediction, interpolation, HUD, menus, audio listener and relaunches. What is new is outside the engine:

- a **compositor** (the window front end in a compositor mode, no guest): owns the window, the Vulkan swapchain,
  every input device and the layout; starts and supervises the player processes;
- the player processes render **off screen** (the Vulkan backend without a surface) into images exported to the
  compositor (VK_KHR_external_memory_fd, opaque fds over a Unix socket), which it draws into their viewports;
- input is routed per device: keyboard and mouse to the player that owns them, each controller to its player (a
  virtual SDL gamepad in that player's process, so the title's own input path is unchanged);
- audio: each process plays its own stream (its listener at its own player), the system mixer (PipeWire) adds them;
  the compositor sets each player's gain.

| Concern | B |
|---|---|
| Shared presentation | zero-copy: exported Vulkan images on the same GPU (device UUID matched), three per player, ready / release messages |
| Input routing | compositor owns SDL devices; serialized SDL events to each player's process; controllers as virtual gamepads there |
| Audio mixing | one stream per player, mixed by the system; gain 1/sqrt(players) by default |
| Frame sync | each player renders at its own rate (frame_rate setting); the compositor presents at the display's rate the newest finished frame of each |
| Memory | about 110 MB host memory per player process (+ its GPU targets at viewport size) |
| Crash isolation | a player process that dies leaves the others playing; the compositor shows it and can start it again |
| Map changes | unchanged: the server's MapChange makes each player process relaunch itself (exec keeps its socket to the compositor) |
| Server identity | unchanged: each player process verifies the server (pinned key from the first player's join) |
| Slots | each local player is one client_t: 4 local players + 61 bots = 65 |

**Hybrid not needed.** B has no player-count limit (the compositor's layouts go to 4; more would be a layout).

## 3. Decision

B. The game code is touched only where a player process differs from today's window process (off-screen output,
input from the compositor, the first player telling the compositor where it plays, players 2-4 leaving when their
match ends); the server side does not know a client is local (section 11 of the brief: no shortcuts).

## 4. Implementation plan

- **N4.1 foundation**: the IPC protocol (src/linux/split_ipc.h: Unix SOCK_SEQPACKET, messages with fds); the
  compositor (src/linux/compositor.cpp: SDL window, Vulkan device, swapchain, imported images, layouts for 1-4,
  device assignment, process supervision); the player side (vk_backend off-screen export, window_main without a
  window, input from the socket, virtual gamepads); `./run_game.sh --splitscreen N`.
- **N4.2 two players**: player 1 hosts on 127.0.0.1 (offline with bots) or joins; player 2 joins the same server;
  layouts horizontal / vertical.
- **N4.3 three and four**: layouts (3: one wide view over two; 4: 2x2), per-viewport aspect (Hor+ beyond 2.5:1 for
  the wide views, letterboxed below 4:3), HUD at viewport size, audio gain, the setup screen in the menus (players,
  layout, devices, Start to join), names.
- **N4.4 network**: player 1's join (browser, direct IP, password, pinned key) followed by players 2-4; split-screen
  listen hosting reachable from other PCs; map changes; reconnects; disconnects.
- **N4.5**: performance at 1-4 players (empty and 61 bots), the full regression, docs.

## 5. As built: processes and protocol

```
 window (SDL, Vulkan swapchain, every input device)
 └─ compositor  (build/window/CounterStrike, CSX_SPLIT_COMPOSITOR=1; src/linux/compositor.cpp)
     ├─ player 1  same exe, run folder           ── front end, menus, hosts or joins
     ├─ player 2  same exe, <run>/split2         ── +connect to player 1's server
     ├─ player 3  same exe, <run>/split3
     └─ player 4  same exe, <run>/split4
        each: Unix SOCK_SEQPACKET socket to the compositor (CSX_SPLIT_FD, kept across the title's relaunches by
        SDK patch 0044), 3 exported Vulkan images (opaque fds), its own audio stream, its own network client
```

- **Start**: `./run_game.sh --splitscreen N` (N = 1..4), or in the game: main menu > **Split Screen** (the last row),
  Local Players 2-4 and Accept. Alone in its window the game then re-executes itself as the compositor (back on the
  main menu, StartFromWindow); under the compositor the change applies at once. Split-screen stays optional: without
  it nothing of this runs (one process, its own window, as before).
- **Messages** (src/linux/split_ipc.h): player to compositor Hello, Images (with the frame's aspect), Ready, Capture,
  Match ("host ADDR KEY", "join ADDR KEY PASSWORD|-", "menu"; players 2-4: "ended TEXT" / "refused TEXT"), Log,
  Setup (the setup screen; "display MODE vsync V"); compositor to player Size (viewport pixels and logical size, the
  match and menu aspects), Event (a serialized SDL event), Release, Pad, Focus, State, Quit. One packet each, at most
  4 KB and 4 descriptors; the local socketpair is the only channel (nothing listens: no new attack surface).
- **Rendering**: a player has no surface or swapchain: SDL's offscreen video driver and a hidden window; the Vulkan
  backend (vk_backend.cpp, split mode) renders as always and copies each shown frame into one of three exported
  images (VK_KHR_external_memory_fd, dedicated allocations, released to VK_QUEUE_FAMILY_EXTERNAL); a notifier thread
  waits its fence and sends Ready, so the game thread never waits. The compositor imports them on the same GPU (device
  UUID, CSX_SPLIT_GPU), acquires them from the external queue family, blits the newest finished frame of each player
  into its viewport, and releases the frame it replaced. A player renders at its own rate; the compositor presents at
  the display's (FIFO, MAILBOX with VSync off).
- **Following player 1**: player 1 plays the front end. When it hosts (ServerQueryStart: "host") or joins (the client
  endpoint's Connected event: "join" with the server's key and the password in use) the compositor starts players
  2..N in their folders with launch data that boots straight into `+connect ADDR` (the title's own direct connect),
  the server's key pinned (CSX_SERVER_KEY: it must be that server, as N3's browser joins), the password
  (CSX_NET_PASSWORD, environment only, in no log), the name (`split_name_N` in pc_settings.ini, default "Player N"),
  and a copy of player 1's display settings. Each has its own identity key. Back in the menus ("menu") they end.
- **Map changes** need nothing new: the server's MapChange makes each player process relaunch itself and rejoin
  (N1's reconnect), keeping its compositor socket. Players 2-4 whose match ended (the server gone, a refusal) exit;
  the compositor starts them again for player 1's next match.
- **Offline matches**: Single Player with split-screen on hosts its server on loopback only (CSX_SERVER_LOOPBACK:
  127.0.0.1, no LAN listener, no query port) so the other local players can join it as clients; Multiplayer > Host
  a Match hosts on the network as before, other PCs join it too (a split-screen listen server). The server does not
  know which clients are local: same transport, encryption, identity and password checks as any remote client.

## 6. Layouts, aspect and HUD

| Players | Layout (2-pixel gaps) |
|---|---|
| 1 | the whole window |
| 2 | top and bottom (default) or side by side (Split Screen > Two Players, `split_layout`) |
| 3 | player 1 across the top, players 2 and 3 side by side below |
| 4 | 2x2 |

Each player renders at its viewport's pixel size (Native resolution) with its own aspect: pc_wide.cpp's Hor+ widens
the 3D view and keeps the menus and HUD in the middle 640x480. For split-screen players the aspect cap is 4:1 (2.5:1
alone), so two players top and bottom at 16:9 (32:9 halves, 3.56:1) fill their viewports; a viewport narrower than 4:3
(side by side at 16:9: 8:9) shows the 4:3 frame letterboxed, as the HUD and menus need the 4:3 middle. The aspect is
set when a process boots (the title creates its device once); when the layout changes during a match (a fourth player
joining three) a frame wider than its new viewport fills it with the sides cropped (split::FrameRect: the HUD and menus
stay whole) until the player's next boot (map change, next match) takes the new aspect. The HUD is the title's own at
each viewport's size: at four players on 1920x1080 each view is 959x539, the HUD (laid out for 480 lines) is drawn at 539 lines,
half its size full screen alone; health, armour, money, ammunition, the timer and the radar stay readable (screenshots in build/wruns/vk_split_4).
Checked at 16:9, 4:3 (1440x1080: 4:3 quarters, no bars) and 21:9 (2560x1080: two 4.7:1 halves, slightly
pillarboxed at the 4:1 cap), windowed, resized (1280x720) and full screen, with nothing restarting.

## 7. Input

- **Keyboard and mouse**: player 1's (`split_kbm`). Mouse coordinates go into its viewport's space (its menus'
  pointer), relative motion when it plays. The compositor captures the mouse when that player asks (Capture: playing,
  not in a menu or the console), with raw relative motion as before. F11 / Alt+Enter full screen and F1 the capture
  toggle are the compositor's; player 1's PC Options > Display Mode and VSync reach the compositor (Setup "display").
- **Controllers**: each is assigned in connection order to the next player without one (2, 3, 4; player 1 too with
  Split Screen > Player 1 > Also a Controller). In that player's process it is a virtual SDL gamepad
  (SDL_AttachVirtualJoystick), so the title's own controller path (one active pad, START) is unchanged; players do not
  see the host's real devices (they are the compositor's), so no input reaches two players. Players read joystick
  events without window focus (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS).
- **Hotplug**: a controller unplugged is detached from its player (the player keeps its slot and plays on when it is
  back); plugged in again it goes to the first player without one.
- **Start to join**: START on a controller with no player adds a local player (up to four), into player 1's match if
  it plays one. START on a player's own controller after its match ended for it (refused, dropped) tries again.
- Combinations tested: keyboard and mouse with 1, 2 and 3 controllers (vk_split_2, _3, _4, _kbm_3); only controllers,
  2, 3 and 4 of them (vk_split_pads_2/3/4, N4.6: section 16.2).

## 8. Audio

Each player process plays its own stream (its own listener at its own player: footsteps, gunfire and voice are
positioned for that player) through SDL to the system mixer (PipeWire), which adds them. Each stream's gain is
1/sqrt(players) (CSX_AUDIO_GAIN times Master Volume) so four streams together stay near one stream's loudness. There is
no per-viewport panning: the sound of all players comes out of the same speakers, as on a console's split-screen.
A player's menu sounds are its own. The compositor plays nothing.

## 9. Setup screen

Main menu > **Split Screen** (pc_split.cpp, the PC menu framework of pc_menu.cpp, the title's own look): Local
Players (1 (Off), 2, 3, 4), Two Players (Top and Bottom, Side by Side), Player 1 (Keyboard and Mouse, Also a
Controller), and four status lines, one a player: its device(s) as the compositor sees them, "press START on a
controller to join" for a free slot, "off" past the count. Accept saves and applies. Duplicate assignment cannot
happen: a device belongs to one player (the compositor's table), START on an assigned controller is that player's.
Names: Y opens Player Names (N4.6, section 16.1); saved as `split_name_1..4` in pc_settings.ini.

## 10. Slots and bots

Every local player is a separate client of the server: its own client_t, entity, team, score, scoreboard row. An
offline or menu-hosted split-screen match leaves room for four local players: bots are capped at 61 (65 slots). The
title sets its bots at a match's start and never removes one for a joining player (bot_kick has a known crash, TODO
1e3), so this is what lets a controller's START, or a map change with more players, find a slot. Verified as
occupancy (the server's client_t count, the `players` step): 4 local players + 61 bots = 65; 3 + 61 = 64, then the
fourth player joined with START = 65 (vk_split_4, vk_split_3). A server that is full refuses a joining player as any
client (net_split_*: the refusal reaches the compositor, the player's slot is given back).

## 11. Performance (N4.5)

Machine: AMD Ryzen 7 5700X3D (8 cores, 16 threads), Radeon RX 9070 XT (RADV, Mesa 26.2.5), 32 GB; Linux, headless
gamescope 1920x1080 (the window 1728x972), VSync off, Frame Rate Limit 240, Native resolution. de_airstrip from
Single Player > Custom, player 1 held at one view for 60 s (setview), the others standing at their spawns in their
buy menus. tests/scenarios/vk_perf_split_*.txt, summarised by tools/split_perf.py (frame intervals of every frame,
the renderer's GPU time, the server frame, the compositor's per-process CPU and memory lines, net stats).

| Run | Player | frames/s | 1% low | 0.1% low | 99% interval | GPU ms | server frame |
|---|---|---|---|---|---|---|---|
| alone (no compositor), 64 bots | 1 | 192.7 | 33.6 | 26.0 | 24.8 ms | 0.24 | 1.45 ms |
| compositor, 1 player, 61 bots | 1 | 145.6 | 24.9 | 21.2 | 36.1 ms | 0.61 | 2.52 ms |
| 2 players, 61 bots | 1 (host) | 128.6 | 30.9 | 26.6 | 30.4 ms | 0.58 | 2.97 ms |
| | 2 | 152.9 | 97.4 | 83.5 | 9.5 ms | 0.96 | |
| 3 players, 61 bots | 1 (host) | 98.1 | 29.0 | 26.1 | 31.5 ms | 0.95 | 3.55 ms |
| | 2 / 3 | 123.8 / 164.1 | 79.2 / 102.6 | 69.2 / 91.0 | 11.8 / 9.2 ms | 1.04 / 0.83 | |
| 4 players, 61 bots | 1 (host) | 93.5 | 22.5 | 17.1 | 35.6 ms | 0.84 | 4.39 ms |
| | 2 / 3 / 4 | 123.0 / 152.8 / 123.0 | 82.6 / 93.1 / 79.9 | 72.5 / 78.0 / 67.4 | 11.4 / 10.0 / 11.4 ms | 1.03 / 0.84 / 1.04 | |
| 2 players, no bots | 1 / 2 | 164.1 / 133.0 | 107.9 / 91.7 | 95.6 / 81.7 | 8.5 / 10.3 ms | 0.93 / 1.09 | 0.23 ms |
| 4 players, no bots | 1 / 2 / 3 / 4 | 156.4 / 126.7 / 154.1 / 127.3 | 108.6 / 89.1 / 106.5 / 92.2 | 94.5 / 75.8 / 93.5 / 83.2 | 8.7 / 10.5 / 8.9 / 10.4 ms | 0.69 / 0.85 / 0.69 / 0.84 | 0.36 ms |

| Run | CPU (all processes, % of one core) | Memory resident | Network (each client, out + in) | Host's server |
|---|---|---|---|---|
| compositor, 1 player, 61 bots | 163% (2 processes) | 221 MB | | |
| 2 players, 61 bots | 267% (3) | 406 MB | 7.6 KB/s | 7.6 KB/s |
| 3 players, 61 bots | 365% (4) | 564 MB | 7.2-7.8 KB/s | 15.1 KB/s |
| 4 players, 61 bots | 460% (5) | 719 MB | 7.5-8.1 KB/s | 23.0 KB/s |
| 2 players, no bots | 257% (3) | 405 MB | 5.4 KB/s | 5.4 KB/s |
| 4 players, no bots | 459% (5) | 716 MB | 5.4-6.0 KB/s | 16.9 KB/s |

- **Every player stays above 60 frames/s on average in every run**, and players 2-4 (clients) above 67 in their
  0.1% lows. Player 1 hosts the server: with 61 bots its lows are the bots' thinking bursts (the same pattern alone:
  1% low 33.6, FINDINGS "High population performance"); its server frame grows from 1.45 ms alone to 4.39 ms with
  four players, from CPU contention (five busy processes on eight cores).
- **One-player mode is unchanged**: split-screen off, nothing of it runs. vk_perf_aztec_64 on this build: 90.2
  frames/s, 1% low 21.6, server frame 3.66 ms, against 84.0 / 19.3 / 3.82 recorded before N4 (run-to-run spread
  75.7-88.3 frames/s): within noise.
- **The compositor with one player** costs about a quarter of the frame rate against the game alone (145.6 against
  192.7 at these rates; GPU 0.61 against 0.24 ms, recording 2.25 against 0.81 ms): the copy into the exported image,
  the notifier and a second process presenting. It matters only above the display's rate.
- **Simulation**: the server runs in real time at every player count (its frame well under the frame interval); no
  frame skipping, no slowed clock; every effect stays on in every player (fog, rain, the flashbang, Auto Exposure;
  Bloom and the PC settings copied from player 1); bots keep their difficulty and count (61 with room for four).
- **Latency**: the local players are loopback clients of player 1's server, with the same interpolation as remote
  players (the engine's interpolation; a loopback round trip); each player's own input goes straight into its own
  process (one SDL event over the socket from the compositor). Not measured end to end (no capture rig).
- **Audio**: the benchmarks (as every window test) run with the null audio device (CSX_AUDIO=null), so they include
  no mixing cost; four real streams were not measured here and need the play-test.

## 12. Network (N4.4)

Each local player is an ordinary client with its own identity key, transport connection (encrypted, the server
authenticated by its key) and slot; nothing on the server knows a client is local, and no check is relaxed for it.
Players 2-4 get the server's key pinned from player 1's join (a server answering with another key is refused, as N3's
browser joins) and the password player 1 used (environment only; the compositor logs whether there is one). The IPC
between the compositor and its players is a socketpair inherited by the children: nothing listens, no protocol on the
network changed (protocol 47, N1-N3's versions unchanged).

| Scenario (tools/run_net_tests.py, real processes in a private network namespace) | Result |
|---|---|
| net_split_ded_4: csx-server with 2 bots and a password; one PC with four local players joins by address; 6 clients; the match's end changes the map: all four rejoin by themselves, 6 clients again (no slot left behind); the password in no log | PASS |
| net_split_browse: Multiplayer > LAN Matches > a password server, the password typed on the keyboard screen; player 2 follows with the key pinned and the password; 4 clients; the password in no log | PASS |
| net_split_listen: two local players host from Multiplayer > Host a Match (the server on the network, answering queries); another PC (a separate process at 127.0.0.2) joins by address: 3 clients; it leaves: 2 (its slot freed); the local players play on | PASS |
| net_split_full: a full server (csx-server --max-players 3, 2 bots) refuses player 2 ("The server is full" reaches the compositor; player 2 keeps its place, START retries); a player added by START is refused too and gives its slot back; player 1 plays on (3 clients) | PASS |

## 13. Tests

Window scenarios (tools/run_window_tests.py, headless gamescope, Vulkan, test controllers as virtual SDL gamepads
through the compositor's `pad` request; tools/drive.py `pad N attach|detach|button NAME SECONDS|axis NAME VALUE`):

| Scenario | What it shows |
|---|---|
| vk_split_2 | 2 players offline (top and bottom, 32:9 views), each picks its team with its own device; player 2's controller unplugged and plugged in again |
| vk_split_2v | side by side (4:3 frames letterboxed in 8:9 halves), hotplug, the window resized to 1280x720 and made full screen |
| vk_split_2_219 | 2 players in a 21:9 window (4:1 cap) |
| vk_split_3 | 1 player at start; the Split Screen screen sets 3 (controllers given to players 2 and 3); 3 + 61 bots = 64 occupied; a third controller's START joins a fourth player into the running match: 65 occupied, the 2x2 layout, player 1's view filled (cropped sides) |
| vk_split_4 | 4 players offline, 64 bots asked: 61, 65 occupied; four independent views, HUDs, buy menus |
| vk_split_4_43 | the same in a 4:3 window |
| vk_split_4_audio | four real audio streams (PipeWire), silent (Master Volume 0) |
| vk_split_play | Player Collision Off reaching player 2; both scoreboards at once; a flashbang whites out only the thrower's view (its whole 32:9 frame); death and spectating while the other plays; a round restart; Display Mode from player 1's PC Options drives the window |
| vk_perf_split_* | the benchmarks of section 11 (no limits asserted) |

Network scenarios: section 12. Unit tests unchanged (the IPC is exercised by every scenario above).

## 14. Limitations and what is left

(Updated by N4.6, section 16: team changes, controller-only play, names and crash recovery are done.)

- **Rumble**: a player's Damage Vibration goes to its virtual gamepad, not to the physical controller behind it.
- **Player 1's frame pacing with many bots**: player 1 hosts the match; the bots' AI bursts are its slowest frames
  (16.7). A separate server process would fix it; it needs a decision.
- **Layout changes during a match** crop the wider players' views until their next boot (section 6).
- **The compositor costs** about a quarter of the frame rate against the game alone at high frame rates (section 11).
- **Bots for local players**: an offline split-screen match takes at most 61 bots (room for four local players);
  bot_kick's crash (TODO 1e3) stops the server from making room by removing one.
- **Audio** is one mix of all players (no per-viewport panning); four streams were measured (16.5), not listened to.
- **Physical controllers** were not used in the tests (virtual SDL gamepads only).
- **Linux only**: the compositor and the player side use Unix sockets, SCM_RIGHTS and VK_KHR_external_memory_fd; a
  Windows port needs named pipes or sockets with DuplicateHandle and VK_KHR_external_memory_win32 (not started, as
  asked).

## 15. Results on the final build and commits (2026-10-27)

Run sequentially on the build of 0b17948 (the code is unchanged since; later commits are tests and docs):
- Network scenarios (tools/run_net_tests.py): 34 of 35 in the full run; net_split_full failed there on its own
  test (the server's `players` step lists only clients that joined a team, and player 1 was still at TeamSelect:
  2 listed for 3 connected). The test now has player 1 pick its team; rerun alone: PASS (3 clients). So 35 of 35,
  one of them by a rerun after the test's fix.
- Headless offline regression (tests/run_regression.sh): 46 of 46.
- Window suite: the 27 default scenarios, the 6 extra (vk_scoreboard_169, vk_bot_count_169,
  vk_player_collision_169, vk_browser_43 / 169 / 219), the 7 split scenarios (vk_split_2, 2v, 2_219, 3, 4, 4_43,
  play): 40 of 40. vk_split_play passed again in a second run alone (for its pause screenshot).
- vk_split_4_audio and the 7 vk_perf_split_* benchmarks: run alone earlier on this code (section 11), not repeated.
- Unit tests (transport, browser, server config, stuff filter, net buffers, scoreboard rows, slot audit, guest
  tables): all pass (109, 103, 22, 14, 18,314 and 1,222 checks; the slot audit and guest tables suites pass).

| Commit | What |
|---|---|
| cf196a1 | N4.0: the investigation and the decision (this document's sections 1-4) |
| 04724b8 | N4.1-N4.3: the compositor and player processes, layouts, input, the setup screen |
| df4eb24 | Tests: vk_split_2 / 4; drive.py pad step; net runner --window sections |
| 7af1caa | START joins a running match, refusals handled, the 61-bot reserve, views filled after a layout change |
| 962f4d9 | Tests: setup screen and START-join, side by side, hotplug, dedicated and listen; benchmarks, tools/split_perf.py |
| 3cf8562 | The password out of the compositor's log; player 1's Display Mode and VSync drive the window |
| b7d5c8a | Tests: the browser with a password, 4:3 and 21:9, resize and full screen |
| 0b17948 | The compositor keeps the full screen state it asked for |
| ddecec5 | Tests: vk_split_play, vk_split_4_audio |
| (this) | Docs (this section, PROGRESS, FINDINGS, ROADMAP, TODO, pc_multiplayer.md 18); net_split_full |

## 16. N4.6: completion (2026-10-27)

Approved by the user 2026-10-27 after N4 was reported incomplete: player names in the setup menu, controller-only
play tested, team changes, crash recovery, a four-stream audio check, player 1's frame pacing, the full regression.
The architecture is unchanged.

### 16.1 Player names

Main menu > Split Screen, **Y** (Q on the keyboard: the screen's status line says so) opens **Player Names**
(SuperLauncher_PCSplitNames, src/game/pc_split.cpp): the PC screens' panel, System Link's table and highlight, a row a
player with its name beside it (Back / Select). A on a row opens the title's own profile keyboard (the same screen as
Join by Address and the password: the controller's d-pad and A, the mouse on its keys, or typed on the keyboard, which
types into it at once). Done saves `split_name_N` in pc_settings.ini; an empty name gives the default back (player 1:
its profile's name, players 2-4: "Player N").
- **Valid names**: 1-20 characters of letters, digits, spaces and `- _ . ! ? ( ) [ ] * + = : ' @ # &`, not only spaces
  (PcSplitNameValid). Quotes, `;`, `\`, `%`, `$` and control characters are refused: a name goes into a console
  command (`name "..."`) and a server's userinfo. A refused name keeps the keyboard open with "A character in that name
  cannot be used." and typing goes on. The edit box takes at most 20 characters (PcEditBoxRoom).
- **Where it is used**: players 2-4 get `split_name_N` read from player 1's settings file when the compositor starts
  them (CSX_PLAYER_NAME), checked again by the player before it goes into its `name` command at its connect; player 1
  uses `split_name_1` (if set) after the title has set the profile's name at a match's start (the hook at 0xADCE5),
  in split-screen only. Existing `split_name_2..4` values are read as before; a name that is not valid any more is not
  used (logged) and the player plays as "Player N".
- **Local Players** on the Split Screen screen now shows the players the compositor has (the `--splitscreen N` start
  or a START join), not only the saved setting: Accept no longer drops a running split-screen to 1 player.
- Test: vk_split_names (typed names, a refused name, the on-screen keyboard by keys, the names in the server's log).

### 16.2 Controllers

Player 1 plays with a controller alone with Split Screen > Player 1 > Also a Controller (`split_p1_pad 1`): the first
controller is player 1's, the menus, the browser and the match all work from it (the keyboard and mouse stay player 1's
too: nothing else can use them). Tested with **virtual** controllers only: SDL virtual gamepads made by the compositor
(the tests' `pad` requests), which reach the players exactly as connected controllers do (SDL events in the compositor,
then the player's own virtual gamepad). **No physical controller was used**: one physical-controller check is in the
manual list (16.6).

| Scenario | Devices | Checked for every player, each in its own process (`@N state`) |
|---|---|---|
| vk_split_pads_2 | 2 controllers, no keyboard/mouse player | menus from the intro to the match on controller 1 only; team choice; four rounds in which each player moves (left stick), aims (right stick), shoots (right trigger: one round off its own Glock) and changes weapon (Y twice: away and back), each a different action at once: every player changes only in its own action; scoreboard (Back) and pause menu (START) each its own |
| vk_split_pads_3 | 3 controllers | the same, and controller 3 unplugged and another plugged in: it goes to player 3, who moves with it |
| vk_split_pads_4 | 4 controllers | the same with four |
| vk_split_kbm_3 | keyboard and mouse (player 1) + 3 controllers | the same, player 1 with W, the mouse (relative motion), the left button, 3 / 2 (weapon), Tab, Escape |

Found and fixed on the way: a virtual gamepad's triggers are full-range joystick axes that rest at raw 0, which SDL
reads as half pulled: the tests' controllers made their players duck and fire. The compositor's test controllers now
start with the triggers released and take trigger values in the gamepad's 0..32767. (A physical controller's driver
reports its own released value; this was a test-controller defect.)

### 16.3 Team changes

The title has team changing in a running match: its system link pause menus (InGame_RootHost, InGame_RootClient and the
dedicated and Live variants) have **Change Teams**: TeamSelect_StartMenu (Auto-Select, Counter-Terrorist, Terrorist,
Spectator) → InGame_ConfirmSwitchTeams → `ChangeTeamsConfirmed` (0xE7C00), which runs `switch_to_auto`,
`switch_to_counter_terrorist`, `switch_to_terrorist` or `switch_to_spectator`; the server's game code (ClientCommand,
0x140175..) hands them to its team choice (0x13DB30) with the normal rules. The pause menu is picked by the game mode
(0x5259E0, at 0xCA8F0): 3 is Single Player (InGame_RootSinglePlayer, no Change Teams), 4 System Link, 5 Live. Every PC
match runs in mode 3, so the row was missing. It is now added to InGame_RootSinglePlayer after Match Statistics (the
same place as in the system link menus) in network and split-screen matches only (a direct connect, Multiplayer > Host
a Match, split-screen: PcNetworkMatch); a Single Player match alone keeps its menu as it was. No new gameplay code:
the title's screens and the server's rules.
- Rules seen: a change is refused when it would stack the teams (mp_limitteams) or fill one; a player alive when it
  changes dies (the round's death) and spectates until the next round; **Spectator** is refused unless the player is
  dead and allow_spectators is on (0x13E045, "#Cannot_Be_Spectator").
- Test: vk_split_teams (player 2 by its controller T → CT mid-round, dies and spectates, player 1 untouched; player 1
  refused Spectator while alive, Spectator when dead with allow_spectators 1, then Terrorist; a round restart, 1 CT and
  1 T, 2 clients throughout).

### 16.4 Crash recovery (players 2-4)

A player 2-4 whose process ends without saying why (no "ended"/"refused" first: a crash or a kill) during player 1's
match (src/linux/compositor.cpp Crashed):
- the compositor stays and the others play on; its exported images are released at once (logged); its viewport keeps
  its place with a message drawn by the compositor ("PLAYER N STOPPED (signal S)" / "RESTARTING..." or "PRESS START TO
  REJOIN", SDL's 8x8 debug font scaled up), "PLAYER N / JOINING..." while a player has no picture yet;
- its controller stays its (input to a dead player is dropped; START on it rejoins at once);
- it is restarted after 3 s, at most 3 times in 120 s; a fourth crash in that time is not restarted (no restart loop):
  START on its controller rejoins it. A new match resets the count;
- every player process gets PR_SET_PDEATHSIG (SIGKILL): if the compositor dies, the players die with it; at a normal
  close the compositor waits for each (SIGTERM, then SIGKILL) and logs how many are left (0).

Fixed on the way (found by these tests):
- **The server kept a crashed player's slot** until the engine's own timeout (about 11 s; the transport's 30 s): the
  restarted player, a new connection from a new port, was refused as "The server is full" in a full match. Now (a) a
  player identity proved again from another address closes the old connection (src/net/transport.cpp: only a proved
  identity, the handshake's signature; anonymous clients are never replaced) and (b) a transport connection that
  closes on a server drops its engine client: at once when it was replaced (inside the packet read, as a client's own
  disconnect is handled, so the replacing join finds the slot free), at the next tick otherwise
  (csx::DropNetworkClient, src/game/server_admin.cpp). This applies to any network client, not only local ones.
- **Players 2-4 crashed on their own normal exit** (a refused or ended match): `std::exit` ran the static destructors
  while the game's threads ran; the process died with SIGSEGV or SIGABRT up to a minute later (seen in N4's
  net_split_full log). They now end with `_Exit` after flushing the log.
- **A SIGSEGV sent by another process was ignored once** by the guest-table fault reporter (it returns to re-run a
  faulting access, and there is none): a sent SIGSEGV now ends the process.
- Test: vk_split_crash: four players and 61 bots (65 slots). Player 2 in a round (SIGKILL); players 3 and 4 at once
  (SIGSEGV, SIGABRT); player 3 during a map change; player 4 four times in two minutes (the fourth not restarted, its
  slot freed, START rejoins); 65 occupied after every rejoin, no "server is full"; last, player 1 killed: the
  compositor ends with every player, "0 player processes left running".

### 16.5 Audio (four streams)

How it works (unchanged from N4, section 8, plus a limiter): every player's process mixes its own sound (its listener
at its own player) and plays it as its own stream; the system mixer (PipeWire) adds the streams. Each stream is at
Master Volume x 1/sqrt(players). New in N4.6: each split-screen stream is also held under 1/players of full scale by a
peak limiter (src/linux/sdl_audio.cpp: a 10 ms chunk louder than that is turned down at once, the gain recovers over
about 100 ms), so the sum of the streams cannot pass full scale; sound below the ceiling keeps its level; one player
alone has no limiter.

How it was checked (vk_split_4_sound, tools/split_audio.py): four players and 3 bots, each stream at **full** Master
Volume through SDL's dummy output device (nothing reaches the speakers; the stream is not muted) and captured as the
device took it (CSX_AUDIO_WAV), every game sound each process started logged (CSX_LOG_SOUNDS: S_StartDynamicSound and
S_StartStaticSound, where this build plays weapon fire; menu sounds). The run: pause menus, three shots each, reloads,
footsteps, radio (coverme, enemyspot), a death and spectating, a map change, shots after it.

| Measured (the four streams, 345-424 s each) | Before the limiter | With the limiter |
|---|---|---|
| a stream's level (seconds with sound) | -31 to -35 dBFS | -36 to -39 dBFS |
| a stream's peak | -6.0 dBFS (the game's mix clips at full scale, x 0.5) | -12.0 dBFS (the ceiling, 1/4) |
| the four streams added (what the system mixer plays) | peak +0.5 dBFS, **82 samples clipped** | peak -2.6 dBFS, 0 clipped |
| late device callbacks | 0 | 0 |

- **Heard**: every gunshot a player fired on its own entity (found as the entity of its own radio sentences) has an
  onset in its own stream (rise of 25-35 dB); 95-100% of footsteps, reloads, radio and menu sounds have one; distant
  sounds (explosions across the map, ricochets) are correctly faint and not counted.
- **Radio**: a radio message plays for its team only (player 1's coverme in players 1-3, the Terrorists, not in player
  4, a CT).
- **Positional**: sounds of other players 30+ degrees off a listener's view, louder in the channel on their side:
  player 2: 14 of 15, player 3: 3 of 4 (the before-limiter run; see below and 16.8).
- **Duplicates**: the two weapons picked up at spawn each play items/gunpickup2.wav in the same instant (not a
  duplicate); the dying player's own process started player/die2.wav twice within 20 ms once (not investigated: for
  the listening check). A radio sentence named "!MRAD_" (no message) is started once per player at a round start (the
  title's; not investigated).
- **Not established by the tests**: how it sounds. The dummy device paces itself (47,600 frames a second against the
  log's clock: the analysis measures each capture's rate), not a real device's timing; N4's vk_split_4_audio ran four
  real PipeWire streams at Master Volume 0 with no late callbacks. Nobody has listened: 16.6 is the checklist.
- Found and fixed on the way (controllers): each player's own virtual gamepad took the compositor's trigger values as
  raw axes, so a released trigger read half pulled: a semi-automatic weapon fired once per hold and never again until
  the trigger passed half way (three pulls, one shot), and the left trigger was held. Real controllers go through the
  same path. Fixed in src/linux/split_player.cpp (and the tests' controllers in the compositor); vk_split_pads_* fire
  three shots in a row.
- The limiter run's positional shots (player 3, controller 2) did not fire: that run was before the trigger fix, and
  player 3's trigger, read as half pulled after its earlier shots, never released. The regression run (16.8) repeats
  vk_split_4_sound with both fixes.
### 16.6 Manual checks (need the user: a listener, physical controllers)

Not done by the automated tests: nobody has listened to split-screen audio, and no physical controller was used.
Start: `./run_game.sh --splitscreen 4` (or main menu > Split Screen), Master Volume at a comfortable level, headphones
or speakers. About 15 minutes.

Listening (four players, a Single Player match with a few bots):
1. Each player in turn fires its pistol alone: one clear shot each time, not doubled, no crackle.
2. All four fire together (hold the triggers): loud but not distorted or crackling (the limiter holds the sum under
   full scale; listen for pumping: the level dipping audibly after loud shots).
3. Walk one player while the others stand: its footsteps are heard (each player's sounds are heard by all: one mix).
4. A player standing to another's left fires: the shot comes more from the left (each stream is positioned for its own
   player; with four streams mixed, this is weaker than alone).
5. Reload (X / R), radio (a radio command from the pie menu): heard once; a radio is heard only by that team's players.
6. Take damage (a bot's fire, a fall): pain sounds; a player dies: its death sound once (an automated run logged one
   death sound started twice within 20 ms on the dying player's own process: listen for a doubled death sound).
7. Open one player's pause menu: its menu sounds, the others' game sound goes on.
8. A player dead and spectating: it still hears the match.
9. A map change (the match's end): sound stops during loading and comes back for all four; no stuck or looping sound.
10. Two players only: louder per player than with four (gain 1/sqrt(players)); one player alone: as before split-screen.

Physical controllers:
11. Two or more real controllers: each goes to the next player in order (2, 3, 4; player 1 first with Split Screen >
    Player 1 > Also a Controller); unplug one and plug it back: same player; START on a spare one adds a player.
12. Player 1 with only a controller: the intro, menus, Split Screen > Player Names (the on-screen keyboard), a match.
13. Rumble (Damage Vibration): not routed to the players' controllers in split-screen (not implemented; see TODO).

### 16.7 Player 1's frame pacing (performance investigation)

Same machine and method as section 11 (VSync off, limit 240, 1920x1080, de_airstrip from Single Player > Custom,
player 1 held at one view for 60 s, the others at their spawns), plus two new benchmarks: vk_perf_split_4p_16 and
net_perf_split_ded_4 (the same match on csx-server with 61 bots, the server holding player 1 at the same view; all four
players clients). Player 1's frame times (frames/s, 1% and 0.1% lows from every frame interval; interval median, 99th
percentile and worst):

| Run | Player 1 | interval median / 99% / worst | Players 2-4 (frames/s; 1% low) |
|---|---|---|---|
| 1. one player, no split-screen, 61 bots | 176.1, 30.2, 25.6 | 4.19 / 29.8 / 50.5 ms | |
| compositor, one player, 61 bots | 124.1, 35.4, 31.6 | 6.45 / 26.4 / 33.2 ms | |
| 2. two players, 61 bots | 118.5, 31.8, 28.1 | 7.05 / 29.3 / 37.6 ms | 138.2; 88.9 |
| 3. three players, 61 bots | 125.8, 27.7, 23.5 | 5.79 / 33.7 / 50.5 ms | 130-149; 60 |
| 4./7. four players, 61 bots | 91.9, 28.5, 24.5 | 8.03 / 31.8 / 42.1 ms | 111-125; 42-47 |
| 5. four players, no bots | 144.1, 95.9, 80.7 | 6.77 / 9.7 / 13.0 ms | 127-163; 87-109 |
| 6. four players, 16 bots | 133.8, 82.9, 67.1 | 7.20 / 11.0 / 17.7 ms | 127-157; 84-101 |
| 8. four players on a dedicated server, 61 bots | **117.5, 77.0, 66.5** | 8.30 / 11.7 / 15.6 ms | 122-139; 80-86 |

(Each a single run; frame rates vary between runs by up to about 10%: two later runs of four players with 61 bots gave
player 1 97.4 / 29.8 and 98.1 / 30.2, players 2-4 126-160 / 81-104.) CPU across the processes: 335-465% of one core
(16 hardware threads: not saturated); memory 407-721 MB; the dedicated server itself 34% of a core.

**Why player 1's pacing is worse**: player 1's game hosts the match. Its game thread runs the server frame (the bots'
AI and navigation, game rules, the network) inside its own frames, and the bots' AI comes in bursts: player 1's worst
server frame in each second is 23-34 ms with 61 bots (median of the seconds 23 ms), 3 ms with 16 bots, under 1 ms with
none. Those bursts are player 1's 1% lows (28.5 frames/s = 35 ms). The same bursts appear with no split-screen at all
(one player, 61 bots: 1% low 30.2, worst server frames up to 30 ms). Measured, not assumed:
- **Not the compositor**: with no bots or 16 bots player 1 paces like the others (1% low 96 and 83); the compositor
  costs frame rate only at high rates (one player: 124 against 176 frames/s, both above any display's refresh here).
- **Not network or shared-image synchronisation**: players 2-4 use the same images and fences and are clients of the
  same server, and keep 1% lows of 80-109 except with 61 bots, where their lows fall too (more entities to draw and
  interpolate; 42-104 between runs), never to player 1's.
- **Not CPU contention**: 4.6 cores of 16 busy; the server's cost per second is the same alone and with four players
  (about 300-360 ms of CPU a second: 1.7 ms x 176 frames against 3.6 ms x 92).
- **The dedicated server fixes it**: the same match on csx-server gives player 1 a 1% low of 77.0 frames/s (from 28.5)
  and a 99th percentile of 11.7 ms (from 31.8): all four players pace alike.

**Optimized**: the window's input poll looked for the tests' scripted-press file (building its path and a failed open)
at every call, several times a frame: 8% of player 1's game-thread samples in the sampling profile (CSX_PROFILE, four
players, 61 bots); now every 20 ms (0.01%). This applies to every window build, one player too; the frame-rate change
is within run-to-run variation (vk_perf_aztec_64 after it: 80.8 and 87.9 frames/s, before N4: 84.0, range 75.7-88.3).

**Not done (needs a decision)**: removing the bots' bursts from player 1's frames means not hosting the match in
player 1's game: offline and Host a Match split-screen matches would start csx-server (the same title code and the
menu's match line) and player 1 would join it like players 2-4. That changes how those matches run (a server process
whose life follows player 1's match, player 1's Restart Match / Change Map and server console commands going to that
server, Host a Match's network presence) and is left for approval. Meanwhile a dedicated server gives it today
(csx-server, players join by address or the browser). Bot intelligence, effects and timing were not changed.

### 16.8 Full regression (the final build, c89b5a6; 2026-10-27)

One run, every suite one after another, nothing built or run beside it (13:35-18:45):
- Unit tests: 8 of 8 (net_transport 115 checks, net_browser 103, net_buffers 18314, scoreboard_rows 1222,
  server_config 22, stuff_filter 14, slot_audit, guest_tables).
- Headless offline regression (tests/run_regression.sh): 46 of 46.
- Network (tools/run_net_tests.py, every scenario of tests/net): 35 of 36. net_split_ded_4 and net_perf_split_ded_4
  (csx-server with 61 bots and four split-screen players: 65 slots) passed. **Failed: net_split_full**, a defect of
  the test: the split side's script ends about 20 s in and closes, the server counts its players 60 s after the first
  refusal, by when player 1 had timed out (15 s): it passed before only when the run was slow. The test now keeps the
  split side until the server has counted. Rerun alone with the fix: PASS (3 clients).
- Window, the 27 default scenarios: 26 of 27. **Failed: vk_auto_exposure_rates**: the exposure at 240 frames/s
  against 30 differed by +0.020 (allowed -0.02..0.02; +0.018 at 120). Nothing of the renderer or exposure changed in
  N4.6. Rerun alone: PASS (+0.017 and +0.016): an intermittent at the edge of its tolerance (earlier margins were not
  recorded); the scenario's limit is tight for run-to-run variation.
- Window extra: 7 of 7 (vk_scoreboard_169, vk_bot_count_169, vk_player_collision_169, vk_browser_43 / 169 / 219,
  vk_perf_aztec_64: 83.3 frames/s, 1% low 21.6; before N4 84.0, range 75.7-88.3: single-player not regressed).
- Split-screen: 15 of 16 (vk_split_2, 2v, 3, 4, 4_43, play, names, teams, pads_2, pads_3, pads_4, kbm_3, crash,
  4_sound, 4_audio: four players and 61 bots, 65 slots). **Failed: vk_split_2_219** (2560x1080): during the intro
  movie, 21.8 s in, the compositor and player 1 both stopped logging and 13 s later the compositor ended by SIGKILL.
  No out-of-memory kill, no GPU error in the kernel log, no other test or command running, and the compositor's own
  display watchdog did not log (it would have after 10 s). Not explained. Rerun alone three times: PASS each time.
  Recorded as an unexplained intermittent (TODO 1e5).
- tools/split_audio.py on that run's vk_split_4_sound: PASS (mix of four streams -2.6 dBFS peak, 0 clipped; own
  gunfire 9/9, 4/4, 10/10, 4/4 heard; radio heard by the team only; no late callbacks). Left / right: player 2 12 as
  expected and 7 the other way, player 3 5 and 1: inconclusive (overlapping sounds within the quarter second), not
  counted as a pass.

Not a full-suite pass: the three failures above were rerun alone after the run, and those reruns are listed as reruns.

### 16.9 Commits (branch g8.2b, not merged)

| Commit | What |
|---|---|
| 36d10dc | Network: a closed or replaced connection frees its slot at once (identity replacement; DropNetworkClient) |
| 6482d5e | Player names (Split Screen, Y: the profile keyboard; pc_settings.ini split_name_1..4); Change Teams in the pause menu |
| 8ed2944 | Crash recovery for players 2-4 (notice, restart limit, process reaping); controller fixes (triggers, START) |
| fca4389 | Audio: per-stream limiter; sound logging (CSX_LOG_SOUNDS); tools/split_audio.py |
| 2b83464 | Tests: names, teams, pads 2/3/4, kbm_3, crash, 4_sound; drive.py `@N` |
| 2542e20 | The scripted-input poll every 20 ms; GPU waits with a timeout, the compositor closes when the display is gone |
| c89b5a6 | Window test runner: per-scenario timeout, gamescope's process group ended; perf scenarios |
| (this) | Docs (16, PROGRESS, FINDINGS, ROADMAP, TODO, pc_multiplayer.md); net_split_full's timing |
