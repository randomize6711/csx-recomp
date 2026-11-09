# Native split-screen: 2-4 local players in one game process

Branch `native-split` (from `checkpoint-before-native-split`, 2026-11-09). The user's brief chose a single-process
architecture over the multi-process compositor of N4 (docs/splitscreen.md, which stays as the fallback until the
replacement is verified by hand). Milestones M0-M8; this document is M0's audit and the plan built on it.

Status: **M0 done (static analysis only).** Nothing here has been run: no gameplay test was started (the brief allows
static analysis, compiling and non-gameplay unit tests only). Statements marked **(P)** are proved from the code,
**(I)** inferred (GoldSrc source, naming), **(R)** need a runtime check, which needs the user's permission.

## 1. Summary

Why the game holds one local player per process, in one sentence: **every piece of client state lives at a fixed
address** (the engine's `cl` and `cls`, the client game code's HUD, VGUI, input and prediction globals, the Xbox UI's
screen stack), and the code reaches it by absolute address, so a second player has nowhere of its own to live. The
server, by contrast, already runs once per frame, apart from the client (P), and holds up to 65 players.

The October rejection of the single-process option (docs/splitscreen.md section 2 A) rested on one hard blocker: "a
second world load in one process overruns the texture table". That finding (FINDINGS.md "PC multiplayer N1") is a
client **reconnecting** after `Mod_ClearAll` marked every model for reload, so each texture was uploaded again. Several
clients sharing **one** loaded map are a different case: the model cache is shared and a model already loaded is not
loaded again (I, to confirm (R)). The world itself is recorded once at load into one push buffer (0x552C04) and
replayed each frame with the camera in vertex constants (P, FINDINGS.md "In-game rendering"), which suits several
cameras. So this audit finds **no fundamental incompatibility**. It does find a large, careful job, with four areas
that need real restructuring (section 4, category 3-4): state that the client attaches to the shared world (static
entity fragments, decals, the recorded world), sound (one mixer and listener, every client starting the same sounds),
the menus (one Xbox UI screen stack, one VGUI viewport), and the presentation of several views in one frame.

**Smallest viable change (recommended):** a *client context switch*. Each local player gets a context: a private copy
of every per-player static region plus its own heap objects (allocated by its own initialisation) and its own network
endpoint. `_Host_Frame` runs the server once, then the client half once per context, switching contexts by copying
the per-player regions in and out (the code keeps using the same absolute addresses; only their contents change).
With one player the switch is a no-op, so solo play is untouched. Shared resources (world, models, textures, sounds,
the server, bots) are never copied. A later optimisation (M7) can switch whole pages by remapping the guest memory's
memfd instead of copying.

## 2. How a frame runs today (P)

`_Host_Frame` (0x5D2E0), in order (each step separated by the timing call 0xAC240):
1. Xbox UI frame (0xF3840, 0xF4CD0, 0xF79F0), the frame time (0x5CD00), the console's command buffer (0x3BB50,
   0x1CC10).
2. The client's input before the server: 0x2AC80 (at 0x5D3BD; again after it at 0x5D3F8).
3. **The server frame `SV_Frame` (0xA1D20, at 0x5D3CD)**: reading packets, physics, the game code (entities, bots),
   sending snapshots (analysis/netcode_engine.md).
4. The client half: 22 direct calls after 0x5D3CD (0x1CBD0, 0x264E0, 0x269E0, 0x28A40, 0x297D0, 0x298F0, 0x2A7F0,
   0x2E230, 0x30700, 0x323B0, 0x32EB0, 0x33B10, 0x55E10 `SCR_UpdateScreen`, 0x5CFF0, 0x5D1E0, 0x5D280, 0x5E410,
   0x5E740, 0xB70D0, 0x101FD0, ...): packets, prediction, entities, sound, the screen.

The listen server's own player is an ordinary client slot on the loopback (netcode_engine.md "Listen server"); since
N4.7 a match from the menus runs in a separate local server process (csx-server, 30 Hz tick) and the game joins it over
the PC transport. **The server half needs nothing new for native split-screen: it already serves 65 clients.**

## 3. The audit tool

`tools/state_audit.py` (static, no game run): reads the recompiler's output (every function's original instructions
are kept as comments), builds the direct call graph, every absolute reference into `.data` (0x36C8A0..0x7D6F20, read /
write / address taken), function pointers taken and vtables (code pointer runs in `.rdata` whose address code stores),
and classifies functions by reachability:
- **server**: `SV_Frame` and the game's `DLL_FUNCTIONS` (static table 0x37BDB0, 50 entries, copied to
  `gEntityInterface` 0x5EF1A0);
- **client**: `_Host_Frame`'s client steps and every export `ClientDLL_Init` (0x1C6F0) stores in the engine's
  `cl_funcs` (0x7D6CA0..);
- the walk from the server stops at 13 hubs that reach nearly the whole image because they redraw the screen or end
  the session (`SCR_UpdateScreen` 0x55E10, the loading redraw 0x2C200 / 0x48F30, `Host_Error` 0x5BD30,
  `CL_Disconnect` 0x28F20, `Host_ShutdownServer` 0x5C9D0, the UI frame calls, ...): the server reaches client code only
  through them (a redraw while a map loads, an error).

Result (15 351 functions): client only 3 869, server only 367, both 2 773 (shared helpers: messages, cvars, models,
player movement, which the client also runs for prediction), unreached 8 342 (the Xbox libraries D3D / DSound / XNet /
XOnline, the C runtime, the front end menus, code reached only through pointers computed at run time). Data addresses
referenced directly: about 7 700; written by the client and untouched by the server: 2 140 addresses in 146 clusters;
written by the client and also touched by the server: 172 clusters, each to be reviewed (most are scratch state set
before every use, such as `pmove`, which is safe when the server and the clients run one after another).

Limits: indirect calls through registers are only followed through the tables above, and structures reached through
a base plus an offset (`cl.frames[i]`) show up only as their base. So the tool finds where per-player state is; the
extent of each structure comes from its clearing code (`CL_ClearState`'s `rep stosl` of 0xDC22 dwords, P) and the
earlier audits (analysis/player_slots_*.md). M1 turns its output into the exact region list.

## 4. Subsystems: classification

Categories: **1** already compatible; **2** per-player state separation (a copy per context is enough); **3** shared-
state restructuring (the state is attached to something shared, or must exist once); **4** significant architectural
change.

| Subsystem | Where (evidence) | Cat. | What it needs |
|---|---|---|---|
| Server simulation, bots, rules, 65 slots | `SV_Frame` 0xA1D20 once per frame (P); client_t per slot | 1 | nothing: each local player is a client slot, as today |
| Engine client state `cl` | 0x749D40..0x780DC8, 225 KB, cleared by `CL_ClearState` (P) | 2 | one copy per context |
| Engine connection state `cls` (state, signon, netchan, demo) | state 0x7812E0, signon 0x784800 (P); client-only cluster 0x78545C..0x785778 | 2 | one copy per context; extent to pin (M1) |
| Local player entity, view angles, player number | `cl.playernum` 0x771224, `cl.viewangles` in `cl` (P) | 2 | in `cl` |
| Client entities | `cl_entities` heap pointer 0x551890, malloc max_edicts x 0x74C (P) | 2 | each context allocates its own; the pointer is in the swapped region |
| Prediction | `predicted_players[17]` 0x3AE3D0 (P), `cl.predicted_frames` in `cl`, `pmove` 0x552C34 (P) | 2 | predicted_players and cl per context; `pmove` is scratch set before each use (I) |
| Input (keyboard / mouse / pad to commands) | one active pad 0x3852F8 (P); kbutton states and `in_*` in the client game code (I); the PC input layer (src/game/pc_keys.cpp, pc_mouse.cpp) | 2 (+ host) | per-context button states; the host routes each device to its context and sets the active pad on switch |
| Camera, view, FOV | `r_refdef` 0x6E7A20.. (cluster 0x6E77E0..0x6E7BA4, P), `ref_params`; `V_RenderView` 0xB5E90 already draws sub-viewports (`nextView`, spectator PIP; P) | 2 | per context; the viewport rectangle from the layout |
| HUD | `gHUD` 0x5623D0 (P), its elements' statics, `g_PlayerExtraInfo` 0x57E978, `g_PlayerInfoList` 0x5487B0 (P); user message hooks registered once | 2 / 3 | the data per context; registration (commands, message hooks) once, shared, because it maps names to the same functions |
| Viewmodel, crosshair, flashbang fade, net graph | client code and `cl` (viewent in `cl`, I); screen fade in the client game data (I); net graph host side (src/game/pc_netgraph*) | 2 | per context; host-side overlays drawn per viewport |
| Client effects: temp entities, beams, particles, dynamic lights | particle pool "Particleman" (memory_architecture.md, fixed pool), tempent / beam / dlight arrays (I) | 2 | per context (each client simulates the effects its own server messages start: correct, at N times the cost) |
| Static entities and their efrags in the world's leaves | `R_AddEfrags` links client static entities into the shared world's leaves (I, GoldSrc) | 3 | created once (by the first context) and shared; the others skip them |
| Decals | world surfaces' decal chains, one decal pool (I); infodecals from the map text (FINDINGS "Decals", P) | 3 | applied once: decal messages handled by one context only |
| The recorded world, lightmaps | push buffer recorded at load (0x552C04, P); lightmaps rebuilt by the PC dynamic light code (src/game/pc_dlights.cpp) | 3 | recorded once, replayed per view with that view's camera; dynamic lightmap rebuild once per frame from the union of the contexts' lights (later) |
| Models, sprites, textures, sounds (resources) | `Mod_ForName` cache shared by server and client (I); GL texture table (1500 records, P); the sound cache | 1 / 3 | shared; to check (R) that a second context's precache finds everything loaded and uploads nothing; the reconnect case (N1) must never happen to one context alone |
| Network connection | loopback with exactly two sides (0x440C00, P); the PC transport's one client endpoint (src/game/net_engine.cpp `g_ep[0]`) | 3 | the loopback extended to a channel pair per context, the context's index in the address, the server's match on it (section 5); the PC transport is not used between local players |
| Map change, connect, disconnect | the title relaunches the whole process for every map change (P); a reconnect in one process is the N1 blocker | 3 | all contexts leave and rejoin together (one relaunch); never one context alone |
| Audio | XACT with one 3D listener (0x3038A0, P); sounds started from server messages in each client (`S_StartDynamicSound` 0x100F10, P) | 4 | one mixer for all: sounds started once (one context's messages, or deduplicated), positioned for the nearest listener or mixed per listener; the multi-process gain 1/sqrt(N) as a first step |
| Menus: Xbox UI screens (buy, team select, pause, scoreboard, spectator) | one screen stack 0x5523D4 (P), screen objects on the heap | 4 | a stack per context (swapped pointer plus each context's own screen objects) and per-player focus; the front end stays player 1's |
| VGUI viewport (`gViewPort` 0x552B7C, the scoreboard panel, spectator panel) | heap object tree (P) | 4 | one viewport per context, created by that context's initialisation |
| Console | one command buffer (P) | 1 / 3 | shared; commands from a player's binds run in that player's context (the switch must wrap the buffer's execution of that player's commands) |
| Cvars | static `cvar_t` records with heap strings (I) | 3 | stay shared; the few per-player ones (name, sensitivity, binds, crosshair) set by the host at each switch, never copied (a copied string pointer could be freed by another context) |
| Presentation and the window | the guest's present ends one screen; the Vulkan backend (src/vulkan) owns one swapchain | 4 | each context renders into its own target at its viewport size; the last one presents; one device, shared textures (the multi-process compositor's layouts reused) |
| Threads | game thread, audio, timers (P) | 3 | context switches only on the game thread, at points where no other thread reads per-player data (the audio thread reads the mixer: shared) |
| Front end, profiles, guest identities | player 1's front end; profiles and guest names in pc_settings / the party code (src/game/pc_instant.cpp, pc_join*) | 1 | reused: Instant Action's party (START to join) decides the contexts |

## 5. Where the server runs: decided (the user, 2026-11-09)

**The title's own listen server, inside the one game process** (console-style split-screen: no csx-server process,
no second executable, no network between the local players). The local players are logical clients of that server:

- **Transport: the engine's loopback, extended to one channel per local player.** Today it has exactly two sides
  (`loopbacks[2]` at 0x440C00, stride 0x3F38; NET_SendLoopPacket 0x6BBA0, P), and SV_ReadPackets matches a loopback
  address by type alone (0x9C6F2..0x9C77B, P), so two loopback clients would be one. The extension: a channel pair
  per context, the context's index carried in the loopback address (its port field), the server's match on type and
  index. Each context is a client slot like any other (65 slots kept).
- **Timing.** The listen server simulates once per rendered frame, as the title always did (118-171 times a second
  measured on PC, docs/splitscreen.md 17.4), where the separate local server ticks at 30 Hz. Native split-screen
  therefore returns to the title's own timing; no rate is changed beyond that. Measured cost to keep in mind: with
  61-64 bots the bots' bursts become the players' frame spikes (99% frame interval 31-32 ms); 0-16 bots, no
  difference. Evaluated in M7 without changing gameplay.
- **Unchanged**: solo play keeps its Match Server setting (separate by default); online play, the dedicated server and
  65 slots are not touched. The multi-process split-screen stays as the fallback until the user has verified the
  native one by hand.

(The alternative considered in M0, the contexts joining the separate local server, is not used.)

## 6. Dependency map

```
 M1  context registry + switch (no-op for one player)
      |-- exact per-player region list (state_audit + clearing code + earlier audits)   [cl, cls, client game data,
      |                                                                                   predicted_players, r_refdef...]
      |-- client half of _Host_Frame wrapped in "for each context"
      |
 M2  second context connects ------------------------------------------------+
      |-- the loopback with a channel per context (section 5)                     |
      |-- per-context initialisation (client game init, VGUI viewport, HUD)       |
      |      registration once (commands, message hooks)          [cat. 3]       |
      |-- shared resources: a second precache uploads nothing      [cat. 3, R]   |
      |-- world-attached state once: static efrags, decals, the recorded world  [cat. 3]
      |-- map change: all contexts relaunch and rejoin together    [cat. 3]      |
      |                                                                          |
 M3  rendering: a target per context, the camera per context, the window layout <+
      |-- present: one per frame (the last context)                [cat. 4]
      |
 M4  input routing per context (devices to contexts, active pad on switch),
      HUD / viewmodel / flashbang / net graph per viewport, prediction checked,
      audio policy (one mixer, sounds once, listener choice)         [cat. 4]
      menus per context (Xbox UI stack, VGUI)                         [cat. 4]
      |
 M5  Instant Action: START joins add contexts; Start Match runs them in one process
 M6  three and four players (layouts exist in the compositor)
 M7  shared resources and performance (page remapping instead of copies, effects once)
 M8  regression; the multi-process code removed after the user's manual check
```

The order is forced in three places: the region list (M1) before any second context; the network endpoint and the
once-only rules for shared state (M2) before anything can be seen; rendering (M3) before input and HUD can be checked.

## 7. The lag the user reported (multi-process, 2026-11-08 play-test): what is known

From the play-test's logs (build/play/logs/split_001.log, 2 players, 60 bots, 3840x2160):
- Both player processes rendered 97-240 frames a second (median 166-175): not the frame rate.
- The server was the separate local server: a fixed 30 Hz tick that does not hold with 60 bots (17.4: 24-28 ticks,
  frames of 11-13 ms mean and 55-65 ms bursts), and `sv_maxupdaterate 30`. Every other player and every bot reaches
  a client at most 30 times a second, then waits the client's interpolation delay (about 0.1 s by default, I), and
  a slow server second stretches both.
- Not measured: input-to-photon latency, the compositor's added frame (it blits each player's newest finished frame
  and presents with FIFO), the interpolation actually in use.
- The native architecture removes the process boundaries and the compositor, but not the server's tick: to be
  evaluated separately (server tick, update rate, render rate, interpolation), as the brief requires. Raising the
  tick also raises the bots' cost per second, which is what breaks the 30 Hz tick today.

## 8. Risks and open questions

- **(R) Shared precache**: a second context's connect must find every model, sprite and sound loaded and upload no
  texture (the N1 overrun). Static reading says so for models; HUD sprites, the sky and detail textures to check.
- **(R) Init twice**: the client game's initialisation registers commands and message hooks; run once per context it
  would register twice. Plan: the first context's registrations are kept, the others' skipped by hooks.
- **Heap objects reached from per-player regions** (VGUI panels, HUD lists, xbui screens) must be each context's own:
  created by that context's init, never copied. A copied pointer to a shared heap object would be cross-talk.
- **Copy cost**: per-player static state is several hundred KB (cl alone 225 KB); copying in and out per context
  per frame is about 1-2 MB a frame for two players, a fraction of a millisecond; page remapping (M7) removes it.
- **Threads**: the audio thread must never read per-player data mid-switch (the mixer is shared; to verify which
  per-player data it reads, if any).
- **Xbox UI and VGUI** are the largest single pieces (category 4); the scoreboard, spectator and team menus already
  have PC-side code (src/game/scoreboard.cpp, pc_mouse.cpp) that assumes one viewer.

## 9. Next: M1

1. The exact per-player region list: each cluster of section 3 resolved to its structure and extent (from its
   clearing / init code), written to a table (config or source) with the evidence per entry, plus the heap pointers
   inside the regions.
2. `src/game/client_context.{h,cpp}`: contexts (regions, saved copies, endpoint slot), `Switch(k)` (copy out, copy in),
   a count of one by default; the client half of `_Host_Frame` wrapped as a loop over contexts (one iteration today).
3. A unit test (non-gameplay): the region list does not overlap the server's data or the shared resources the tool
   classifies, and the switch round-trips memory exactly (on a synthetic guest memory).
Single-player behaviour is unchanged in M1 (one context, no copies).
