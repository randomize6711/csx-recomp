# Split-screen: up to four local players (N4)

Milestone N4, approved by the user 2026-10-26: native split-screen for 1 to 4 local players, every one a real player
in its own server slot, offline with bots, on LAN and dedicated servers, through the browser and direct IP, and when
the split-screen PC hosts. The original Xbox game has no split-screen (analysis/splitscreen.md). This document is the
architecture decision (N4.0) and, as the work proceeds, what was built.

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
