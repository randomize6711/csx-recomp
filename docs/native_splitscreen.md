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

## 10. M1 progress (2026-11-09)

Built (compiled; not run: no gameplay test without the user's permission):
- `src/game/client_context_core.h`: the regions and the switch (copy the active player's regions out, the next
  player's in), testable on its own; `src/game/client_context.{h,cpp}`: the game's contexts and the region list.
- `_Host_Frame`'s turns: hooks at 0x5D3C2 (each player's CL_Move before SV_Frame; on_false 0x5D3B5) and 0x5D54A
  (each player's client half; on_false 0x5D3F0). The recompiler emits them as backward gotos in sub_0005D2E0. With
  one context both return true at once: no copy, no jump, the frame as before.
- First regions (232 KB a player, each proved from its clearing or loop code): predicted_players, g_PlayerInfoList,
  g_TeamInfo, g_PlayerExtraInfo, DM_PlayerState, cl. Inside cl the server's code reads cl.maxclients, cl.playernum
  and two frame fields (a listen server reading its own player's state): it runs while player 0 is active, so it reads
  the host player's values, as before.
- `tests/client_context_test.py`: the switch on a synthetic memory (21 checks), and the region list against the
  audit (no region holds data only the server's code touches).

Still to do in M1:
- the remaining per-player state: `cls` (its extent: 0x7812E0.. with netchan, signon, userinfo; fields to pin), the
  client game's other globals (gHUD 0x5623D0 and its elements, input buttons, the view, the client effects), the
  renderer's per-view state (r_refdef 0x6E7A20..), and the largest candidates of the audit (a 256 000-byte grid at
  0x57F0A8..0x5BD8A8 cleared by the client's effects code 0x1CC5D0: rebuilt each frame or kept, to decide);
- for each, persistent (switched) or scratch (shared, set before every use): the copy cost per switch is the sum of
  the switched regions;
- the heap objects the regions point to (cl_entities, VGUI, HUD lists): each context's own (M2's initialisation).

### 10.1 The server's reads of client memory, audited (2026-11-09, static)

Every reader of the client addresses the audit found touched by server-reached code. The server and everything outside
the players' turns run while player 0 is active, so a read there sees player 1's value.

| Field | Readers | Verdict with several local players |
|---|---|---|
| cl.maxclients 0x778A5C | 0x95CF0, 0x96280: `maxclients = sv.active ? svs.maxclients (0x636CE8) : cl.maxclients` (P); shared 0x1C150, 0x42580 | correct: the built-in server is always active in native split-screen, so the server's own count is used; every context holds the same value anyway |
| cl.time 0x74A290 | 0x24A90, a client particle allocator (a free list at 0x6A8198, the particle stamped with cl.time; P), reached from the server only through the player movement code's particle callback, which PM_Init (0x74830, DLL_FUNCTIONS entry at 0x37BE3C) hands to the shared movement code (P); the movement code uses it for its debug drawing (pm_showclip, I); also 0x698D0, 0x6A2C0 | correct for play; if the server's movement ever drew it, the particle would land in player 1's list only. To guard in M2 (a server-phase write to a per-player region is logged, below) |
| cl.intermission 0x74A27C (P: SCR_UpdateScreen tests == 1) | SCR_UpdateScreen 0x55E10 (inside each player's turn: its own), the front end / session 0xC8330, 0xE6260, 0xE9240, 0xE9C00 (frame start), 0xEC4D0 | correct: intermission is sent to every client at once, so every context holds the same value |
| cl.playernum 0x771224 | the session code: 0xEA4E0 (from 0xE9C00, every frame, outside the turns) and 0xEA380; 0xECDA0, 0xEF610 and 0xEE3A0, called from the server's game code (0x11F040, 0x124390, 0x13A9D0, 0x149130) when `gpGlobals` (0x527CE8) exists, i.e. with any server: "is this player the local player" = INDEXENT(cl.playernum + 1)'s user id (pfn 0x527BC0, 0x527C64) against the event's player; on a match the session object caches a value at +0xC7C (P) | **not sufficient**: only player 1 is ever "the local player". With several local players the check must consider every context's player number, and the session's local-player value (+0xC7C, and what reads it) must be per player. To do with the session code's per-player state (M4/M5); category 3 |

| gHUD + 0x1BC0 (0x563F90) | 0x11D930, a server-side player method (reached through a vtable, no direct caller): when the field is 0 it stamps the player with gpGlobals->time (+0x208) (P) | **not sufficient**: for every player the server consults player 1's HUD. The field's meaning and a per-player answer: M4 (category 3) |

General rule from this: code outside the players' turns that asks "who is the local player" gets player 1. Each such
place must be found and made per player or "any local player". Planned guard (M2): in a diagnostic mode the per-player
regions are write-protected while the server frame runs, so a server-phase write into client state is caught and
logged rather than silently landing in player 1's copy.

### 10.2 The 256 KB particle grid: persistent (2026-11-09, static)

0x57F0A8..0x5BD8A8 is a [40][40][40] array of pointers (P: 0x1CC5D0 clears it with rows of 0x1900 and steps of 0xA0;
0x1CC350 clears one 40 x 40 slice). 0x1DE6B0 creates an effect: picks a slice (index x 0x1900), clears it, and fills
its cells with particles allocated from the particle manager (object at *0x552B8C, its allocate method [+0x20] of
0x140 bytes). 0x1DECB0, later and from another call (both from 0x1F5CA0), walks the same slice's cells and pushes
those particles (writes their velocity at +0x80 and +0x3C). So a slice lives as long as its effect, across frames:
**persistent**. Its pointers lead into the particle manager's memory, so the grid belongs with the manager: per player
(each player's client simulates the effects its own server messages start, which is also what the server's
visibility rules intend). Cost: 256 000 bytes per switch, 63 whole pages: the first candidate for M7's page remapping.
0x1CC5D0 runs from the client exports 0x18B970 and 0x1A5EE0 (initialisation), so each player's context gets its grid
cleared by its own initialisation in M2.

### 10.3 The region list (2026-11-09)

One player's regions, 1 036 KB in 152 entries (tests/client_context_test.py prints the total):
- **Hand-checked whole structures** (src/game/client_context.cpp, each with its evidence): `cl` (225 KB), `cls`
  (0x7812E0..0x785BD0, the end being its last referenced field), `gHUD` with its embedded HUD elements
  (0x5623D0..0x569398, bounded by the next statically constructed object), `g_VoiceStatus` (0x56D6E8..0x576C78),
  `g_TeamInfo`.
- **The client's per-player arrays in the guest tables** (0x70000000+): the 65 player slots moved them out of `.data`
  (cl.frames' player states 261 KB, cl.players, predicted_players, the studio player cache, the HUD's player tables,
  the voice status arrays including the speaker icon entities 123 KB, ...). Their old `.data` places are unused. Taken
  from the generated layout by name, so they follow config/guest_tables.toml.
- **The audit's policy** (tools/state_audit.py `regions`, written to src/game/client_context_regions.inc): data
  written only by the engine client (0x1C000..0x3B000), the engine view (0xAC000..0xB8000) and the client game
  (0x18B000..0x200000), never touched by the server's code, clipped against the hand-checked entries; minus the
  reviewed shared exceptions: the light style table (0xB01F0), the gamma ramps (0xB56A0, 0xB5A00), the client's
  playermove_t and physents (0x25BF0: rebuilt before every prediction), and the Xbox model / BSP world renderer's
  data (code 0x195000..0x19B000: the world recorded once, its push buffer at 0x552C04).
- **Kept shared on purpose**: `g_StudioRenderer` (0x5693D0: state set per draw, and it may hold the handles of
  vertex programs loaded once into the 136 program slots; a second player's own initialisation would load them
  again); the renderer's per-view scratch (`r_refdef` 0x6E7A20.., recomputed from ref_params each view); the front
  end's objects (0x5590A8, the screen stack 0x5523D4: M4); the active pad 0x3852F8 (the host sets it per player in
  M4); the sound system; the server game's static objects (0x554208, 0x556E18, touched by the server's code).
- **The audit's blind spot**: it sees absolute addresses only. A static object's members reached through `this`
  are covered only by a hand-checked entry for the whole object (as gHUD, g_VoiceStatus); the C runtime's
  initializer table (0x36C8B0..0x36CD0C, 278 initializers, 66 objects constructed) lists every such object.

**A new player's context starts from the initial state** taken at boot (hook 0x202C1E, after the C runtime's static
constructors, before `main` 0xBF470; not in the dedicated server), not from a copy of player 1's: a copy would hold
player 1's heap pointers, and the second player's initialisation could free player 1's objects (a HUD reloading its
sprite list deletes the old one).

**Copy cost**: two copies of 1 036 KB per switch; two players switch four times a frame (two turns each in the input
loop and the client loop): about 8 MB copied a frame, roughly 1-2 ms on this machine (I, not measured). The largest
parts are page-aligned tables (cl.frames' player states, the voice heads, the particle grid): M7 maps their pages
per player instead of copying.

### 10.4 M1: what is left

- Static objects in the client game range not yet decided (0x552F78..0x553ED8, 0x5616D0, 0x561B88, 0x562148,
  0x5622F4; a few hundred bytes to 2 KB each): each object's class and owner.
- The heap objects reached from per-player regions (VGUI viewport, HUD lists, particle manager, cl_entities): created
  by each player's own initialisation (M2), checked to never be freed by another player's.
- A diagnostic write guard (mprotect of the per-player regions during the server frame) to catch server writes into
  client state, for the first runs of M2.
- One smoke run with the initial-state snapshot hook (boot-time copy of 1 MB; it changes nothing else), needing
  the user's permission.
