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
loop and the client loop). Measured in the M1 solo run (the guard copies the same regions each frame): 0.041 ms to copy
1 060 577 bytes (about 26 GB/s), so a switch is about 0.08 ms and two players about 0.33 ms a frame; the switch itself
is measured once M2 has a second context. The first copy at boot took 0.47 ms (cold pages). The largest
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

### 10.5 Ownership of the players' objects (2026-11-09, static)

- **The small client-range static objects** of 10.4, by their constructors: 0x553930, 0x5616D0, 0x561B88 and 0x5590A8
  are the front end's (constructors 0xB9180, 0xF30F0, 0xFE250, 0xF7780: shared until M4's menus); 0x553ED8 the sound
  system's (0x105340); 0x562148 and 0x5622F4 the server game's (0x114440, 0x1438D0); 0x5531A8 is never written after
  its constructor (0xAE430: constant); 0x552F78 is the front end's settings (written by 0xDAD40..0xE4340), its tail
  an identity matrix set once (0x73290). None is a player's.
- **Heap objects** (every malloc 0x22D86A in client code whose result is stored at a fixed address, scanned): the
  client entities (0x551890), the particle manager (0x552B8C), the VGUI viewport (0x552B7C), gHUD's own allocations
  and the client game's 0x552BB4, 0x552C70, 0x552C9C, 0x552CA8, 0x552CBC, 0x552CE4 are kept in per-player regions:
  each player gets its own from its own initialisation. Kept in shared memory, on purpose: the HUD sprite registry
  (0x551870: 128 entries, allocated once if null, never freed; a second player's SPR_Load of the same names finds the
  same entries: sprites load once), the client's user message list (0x5518AC: registered names and handlers; the
  server's ids per connection), the VGUI root panels (0x552028 / 0x55202C, drawn by SCR_UpdateScreen; each player's
  viewport is their child: M4), the file system (0x438C28), the message reading state (0x748404), the renderer's,
  the host's and the server's objects.
- **The initial state holds no heap pointer**: no static constructor of a per-player object reaches malloc except
  through atexit's registration (0x22C31D), checked for 0x23CA90, 0x23EB60 (gHUD), 0x23ECA0 (g_VoiceStatus).
- **Initialising a player** therefore allocates that player's objects only: its pointers start null, and the code
  seen frees only what its own pointers hold (the client entities are even reallocated without a free when the
  pointer is set: 0x2E303, a warning and a leak, never another player's block).
- **Destroying a player**: RemoveLast drops the saved copy and frees nothing (its objects leak). M2 adds the client's
  own shutdown run in that player's context, which frees through that player's pointers only.
- **For M2** (registration that must happen once): the client game's HookUserMsg, console commands and cvars from a
  second player's initialisation would register the same names again in the shared lists.

### 10.6 The server write guard (2026-11-09)

`CSX_CTX_GUARD=1` (off by default; the dedicated server never): hooks at 0x5D3CD and 0x5D3D2 copy every per-player
region before SV_Frame and compare after it; a region the server frame changed is logged with its address, the span
and the value before and after (five reports per region), and every 30 s a summary with the copy and compare times.
With one player it checks that the server frame never writes the host player's client state; with several players
(M2) it shows any server-phase write that would land in player 1's copy.

### 10.7 Two local-player assumptions for M4

Code that runs in the server's phase asks "who is the local player" and gets player 1, whatever the number of
local players. Both must answer per player (category 3) before several local players are verified:
1. **The session code's local player** (0xEA4E0 every frame from 0xE9C00; 0xECDA0, 0xEF610, 0xEE3A0 from the server
   game's events 0x11F040, 0x124390, 0x13A9D0, 0x149130): INDEXENT(cl.playernum + 1)'s user id against the event's
   player; the session object caches a value for the local player at +0xC7C. Needed: "is this any local player",
   and the cached value per player.
2. **A server player method reading the HUD** (0x11D930, reached through a vtable): reads gHUD + 0x1BC0 (0x563F90) for
   every player and, when it is 0, stamps the player with gpGlobals->time (+0x208). Needed: the field's meaning, and
   the value of the HUD of the player being processed when that player is local.

### 10.8 M1 solo run (2026-11-09, the second one the user permitted)

ctx_solo_smoke with the server write guard on (CSX_CTX_GUARD=1, CSX_SERVER_PROCESS=0, de_dust2, 16 bots, headless,
about 2.5 minutes), on commit 352aab1: passed.
- The initial state: 152 regions, 1 060 577 bytes a player, captured at boot in 0.47 ms.
- Both turn hooks ran from the first frame with one player; the HUD's video initialisation ran once, in player 1's
  context.
- The guard: 3 440 server frames checked, **none wrote the player's client state**; copy 0.041 ms and compare
  0.031 ms per frame on average.
- The match: the player joined (Terrorist), three rounds started, 17 kills; no trap, no engine error, no warning beyond
  the usual missing optional files.

## 11. M2: two local clients in one process (2026-11-09; compiled, not run)

Everything here is off unless `CSX_NATIVE_SPLIT=N` (2..4); off, the game runs the title's paths (the only always-on
hooks compare loopback ports, all 0 then: tests/native_split_test.py checks that they agree with the title). Nothing
of M2 has been run: no gameplay test without the user's permission.

### 11.1 Prerequisites settled (static)

- **cls's end, proved**: its last member is the 1 KB buffer at 0x78579C (copied whole at 0x29770), ending at
  0x785B9C; after it come separate client globals and cl_visedicts[512] at 0x785BC0..0x7863C0 (filled at 0x1CA01,
  its count 0x749D20 kept below 0x200). cls is 0x7812E0..0x785B9C (0x48BC bytes).
- **Registrations** (what a second run of the client's initialisation would repeat):
  - the client's pfnAddCommand (0x1BFD0 -> 0x3C8C0): a name already defined is ignored: idempotent;
  - the client's pfnHookUserMsg (0x1C120 -> HookUserMsg 0x2C690): the same name with the same handler returns the
    existing entry (0x2C6FB): idempotent;
  - the client's pfnRegisterVariable (0x1BEF0): allocates a new cvar_t every time and registers it (0x422A0); a
    name already registered is refused and the client keeps the unregistered copy, whose value nothing sets. While
    another player's initialisation runs, the hook returns the registered cvar (Cvar_FindVar 0x41F20);
  - the engine's Cmd_AddCommand (0x3C760) is Sys_Error (0xAC090, never returns) after host_initialized (0x551D7C):
    while another player's initialisation runs, it is skipped.
- **Process-wide against per-player initialisation**: Host_Init (0x5D9E0) loads the client (0x1C5D0: ClientDLL_Init
  0x1C6F0 fills cl_funcs, the engine's own message hooks, the client's Initialize 0x18B7A0 which copies the engine's
  functions to gEngfuncs 0x5430E8 and makes the particle manager), then CL_Init (0x2BA10: cls.userinfo keys, the
  engine client's cvars and commands, then HUD_Init 0x18B800 via 0x1CB40); each connect then runs 0x2E780 (the client
  state cleared, cl_entities allocated, HUD_VidInit). A player's state from the boot (M1) lacked the tables the
  process's initialisation fills (cl_funcs, gEngfuncs: called through, they would be null): a new player now starts
  from **player 1's regions at the end of Host_Init** (hook 0x5DD94), then runs only Initialize and HUD_Init in its
  own context; the per-connect part runs on its own turn as for player 1.
- **The danger in that copy, found in the code**: the copy holds player 1's heap pointers, and the initialisation
  frees what it finds: gHUD.Init (0x1A65A0) frees every node of the list at gHUD + 0 (0x1A6A10..0x1A6A26), 0x1BAE40
  frees the object at 0x552CA8 before making another. Hence the scrub (11.3).

### 11.2 The local clients and the built-in server

- **A local player** = a slot in native_split_core.h's Players (0..3; 0 is the game's own) + a client context
  (client_context.cpp) + a loopback channel (local_loopback.h). Its life: initialising -> connecting -> active ->
  leaving -> empty; a slot is reused when it rejoins.
- **Connection**: the built-in listen server (a native split match never starts csx-server: LocalServerWanted is
  false). The added player's client is disconnected (cls.state 1) while sv.active: on its own turn CL_CheckForResend
  connects it to "localhost" with no challenge, as the title connects its own player (analysis/netcode_engine.md
  2.1); SV_ConnectClient gives it its own slot (65 kept).
- **Identity**: its own client context (cls, cl, the client game's state), its own userinfo name ("Player N",
  Info_SetValueForKey 0x65760 on its cls.userinfo), its own server userid (g_userid, one per connect), and on the
  loopback its own address: the port is its slot (player 1 keeps port 0, the address it always had).
- **Packet routing**: NET_SendPacket (0x6CC70) for a loopback address goes into the host's channels: from a client,
  tagged with the active player (a player's client only runs in its own turn: reported otherwise); from the server,
  to the player its destination's port names. NET_GetLoopPacket (0x6BAF0) reads the server's inbox (net_from = the
  loopback address with the sender's port) or the active player's inbox. Each inbox keeps the newest four packets of
  each sender, as the title's rings. NET_CompareAdr (0x6B3CD) and SV_ReadPackets (0x9C722) compare a loopback
  address's port, so the reconnect check of SV_ConnectClient (which would have given player 2 player 1's slot) and
  the packet-to-client match keep the players apart. Host_ShutdownServer (0x5C9D0) empties the channels.
- **The server frame** stays once per frame (SV_Frame between the players' input turns and their client halves); no
  second world, no second bot simulation.
- **Until M3 / M4** an added player's turn skips SCR_UpdateScreen (0x5D51B: nothing drawn over player 1's frame), the
  look routine (0x19E160: no stick or mouse look) and the pad's keys (0x19DFD0), and S_StartDynamicSound (0x100F10).
  Keyboard commands run from the console buffer at the frame's start, in player 1's context.

### 11.3 Memory ownership

- **Heap owners** (heap_owner.h; hooks at _heap_alloc's ret 0x22D83D, calloc's 0x235F2D, free 0x22C609): every block's
  owner is the player whose turn or initialisation allocated it, else the process. At the snapshot all blocks so far
  are the process's.
- **The scrub**: before a new player's copy is first switched in, every aligned dword of it pointing into a known heap
  block is cleared (and remembered); its initialisation then allocates its own objects and frees nothing of player
  1's; afterwards each cleared dword it left at 0 gets its value back (a shared object: a registered cvar, a
  registry) and is counted in the log line "N heap pointers of player 1 cleared, M replaced, K kept shared".
- **Foreign frees**: a player freeing another player's block, or a player other than the first freeing a block of the
  process's initialisation, is logged (five times, every time with CSX_CTX_GUARD=1). Frees outside the turns (the
  server, the console, a map change) are not attributed.
- **Leaving** (RemovePlayer; the diagnostic CSX_NATIVE_SPLIT_LEAVE_AFTER=S removes the last player S seconds after it
  joined): its server slot dropped with SV_DropClient (0x5C600; never CL_Disconnect, which calls Host_ShutdownServer),
  its loopback packets forgotten, its client entities freed (its own block, checked), its context removed (the later
  ones renumbered), its slot emptied. Its other objects (the particle manager, the VGUI viewport, the HUD's lists) are
  not freed: their destructors are not run; their blocks are counted ("N kept, B bytes") and handed to the process.
  The shared map, textures, models, sounds and the server are never touched.
- **Context switch**: 0.059 ms a switch measured on a synthetic 1 060 577-byte layout (tests/native_split_test.py),
  about 0.24 ms a frame for two players.

### 11.4 Not verified, and the risks before M3

- **Nothing of M2 ran.** The second player's initialisation in its context, its auto-connect, the server accepting a
  second loopback client, the host loopback, the scrub and the leave path need one supervised run (permission).
- The scrub is a heuristic: a dword equal to a heap address by chance is cleared and restored (harmless); a pointer
  stored unaligned, or inside a heap object reached from the regions, is not seen; realloc is not tracked.
- HUD_Init run again may touch shared state not yet known (the VGUI root panel's children: a second viewport is
  created at its HUD_VidInit and becomes the shared root's child: M4).
- Only dynamic sounds are gated for the added player: static and ambient sounds its messages start may play twice.
- The client effects are simulated per player (correct per player's view, at N times the cost).
- The two local-player assumptions (10.7) still answer player 1.
- A player's leave leaks its objects but the client entities until their destructors are run (M4 / M8).
- A map change relaunches the whole process (the title's way): every context is rebuilt and the added players rejoin
  after player 1 is in (the environment keeps CSX_NATIVE_SPLIT).

### 11.5 The M2 run (2026-11-09, permitted once): crashed; cause found; fixed, not re-run

native_split_2p (CSX_NATIVE_SPLIT=2, CSX_CTX_GUARD=1, de_dust2 without bots, one headless process, the built-in
server): **SIGSEGV at 5.8 s**, before player 2 connected.
- What ran: the initial state at the end of Host_Init (0.18 s: 153 regions, 1 060 565 bytes, 17 323 heap blocks);
  player 1 in its match (server slot 0, userid 1, client state 5, 33 packets sent and read by the server, 34 to it and
  read); player 2 added at 2.15 s in context 1 (Initialize and HUD_Init ran, 1.65 ms; client state 1, waiting for its
  turn). No packet read outside its turn, none dropped; no separate server.
- **The ownership report**: player 2's initialisation freed 44 blocks of the process's initialisation (returns
  0x1A6A1D x19: gHUD.Init's list; 0x1A7829 / 0x1A7849 / 0x1A7858 x18 and 0x1A7AF9: the destructor 0x1A7800 and the
  deleting destructor 0x1A7AE0 of the object kept at 0x552C70 (vtable 0x35EB78, made by 0x1A7F70); 0x1BAE59: the object
  at 0x552CA8; 0x3C658 x3 and 0x42172: engine strings). The scrub at the add had found **one** heap pointer.
- **The crash** (core dump): in player 1's client update, 0x1A7650 <- 0x1A79E0 <- 0x1A7B80 <- 0x18B8A0 (a client
  export) <- ClientDLL_UpdateClientData 0x1CBD0 <- _Host_Frame: player 1 walked the lists of the 0x552C70 object that
  player 2's initialisation had freed.
- **Why the scrub missed them**: it ran when player 2 was added, against the blocks alive then. Between the initial
  state (0.18 s) and the add (2.15 s) player 1's own code had freed and remade objects those pointers named (its map
  load, its connect: the 0x552C70 object among them), and the addresses were reused: the initial state's pointers no
  longer named live blocks of theirs, so they were left, and player 2's initialisation freed what lived there now.

The fix (separate commit):
- **The scrub runs when the initial state is taken**, against the blocks alive then: every heap pointer leaves the
  initial state at once (the copy every new player starts from), recorded with the block's allocation serial.
- **A cleared pointer is given back only to the same allocation**: after the new player's initialisation, when it left
  the pointer at 0 and that very block (address and serial) is still alive; a block freed or an address reused since
  stays empty.
- **The safety net**: a free by player 2..4 (in its initialisation or its turns) of a block that is not its own is not
  done and is logged as an error every time: a leak at worst, never a block freed under its owner (player 1 or the
  process). Frees of player 1 and outside the turns are unchanged.
- Unit tests: the stale snapshot case (a freed node, a reused address, a live shared cvar) and the skip rule
  (native_split_test: 62 checks).

Not re-run: another gameplay test needs the user's permission.

### 11.6 The second M2 run (2026-11-09, permitted once): hung; the scrub's real cause found; fixed, not re-run

native_split_2p on commit 8620a80 (the snapshot-time scrub, the shared-kind restore, the skip of foreign frees, the
independence check; same configuration): **no frame drawn past the loading, the log stops at 3.17 s, timeout at
300 s** (a hang, no crash).
- What ran: the initial state at 0.165 s (153 regions, 17 323 blocks, **1 heap pointer cleared**); player 1 in its
  match; player 2 added at 2.14 s; **player 2 connected**: `"Player 2<2><4294967295><>" entered the game` at 3.07 s
  (its own HUD video initialisation at 2.47 s); its own objects all differed from player 1's (the independence check:
  HUD list 0x01FFE3E0 against 0x00D702C0, particle manager 0x02033D50 against 0x00C52170, the 0x552C70 object
  0x0200EF60 against 0x00EF8C60, ...).
- **The ownership report, a failure**: player 2's initialisation tried to free player 1's blocks (the same 44 places as
  the first run); every free was skipped and logged as an error, as designed. But the destructors had already run on
  player 1's objects (0x1A7800 empties their lists and resets their vtable to the base class 0x35EB80; gHUD.Init
  unlinked player 1's HUD list): player 1 kept destroyed objects, and the game hung. The skipped frees did not make it
  safe, and the test was right to count them as failures.
- **The cause of both runs**: the scrub read the copy in steps of 4 from offset 0. The copy is the regions laid end to
  end, and regions start at any address (0x78A943, 0x742FCF, ...): after the first region whose size is not a
  multiple of 4, every later region's pointers were read across two dwords and never recognised. Hence one pointer
  cleared in both runs, player 1's pointers (gHUD + 0 = 0x00D702C0, 0x552C70 = 0x00EF8C60) left in player 2's
  copy, and player 2's initialisation destroying player 1's objects. The unit tests had only aligned layouts.

The fix (separate commit): the scrub reads each region at its own guest alignment (heap_owner.h Span); the game
builds the spans from the region list, and logs an error if they do not add up to the initial state's size. Unit
tests with an odd-sized region before a pointer and a region starting at an odd address (native_split_test: 69
checks). Not re-run: another gameplay test needs the user's permission.

### 11.7 The third M2 run (2026-11-09, permitted once): hung; stopped for an architectural review

native_split_2p on commit 3a42502 (scrub at guest alignment, pre-destructor checks, shared scratch): **the log stops
at 3.17 s, timeout at 300 s (a hang), with cross-player frees reported**.
- What worked: the scrub cleared **133** heap pointers from the initial state (1 before the fix); player 2's own
  initialisation replaced 131 with its own objects, gave 1 shared registration back and left 1 of player 1's objects
  empty (0x551FE8, 572 bytes, allocated at 0xAF8C4); its HUD list, particle manager and the four client objects were
  its own; **no pre-destructor check fired** (its initialisation no longer reached player 1's objects); player 2
  connected ("Player 2<2>... entered the game" at 3.07 s); player 1: server slot 0, userid 1.
- What failed: during player 2's initialisation 2 frees of process blocks (a cvar string allocated outside the range
  classed as cvar scratch; 0x1815DE); during its **connect** (in its own turn), frees of blocks player 1 had allocated
  during *its* connect: the delta descriptions' registry (the svc handler 0x2FAE0 frees and rebuilds it, 0x45670;
  dozens), the client buffer at 0x551EF4 (0x2E2A0, from the connect 0x2E780), a front end block (0xC8B10). Every one
  was skipped and logged as an error; the game then hung after 3.17 s (no core: a hang).
- Context switches: 2 at the add, 0.27 ms each (the first, cold copy); the players never reached their turns together.

## 12. Architectural review of the snapshot and pointer scrub (2026-11-09, after three failed runs)

### 12.1 What the runs showed

Each run met a new kind of state that inferring ownership from raw memory got wrong:
1. Pointers missed (the copy's alignment), so player 2's initialisation freed player 1's objects (run 1, a crash).
2. A blocked free is too late: the destructor had already emptied player 1's lists and reset its vtable (run 2, a hang).
3. Shared engine state rebuilt by **every** client's connect (the delta descriptions' registry, a front end block)
   and engine buffers that both the server's and the client's code use (0x551EF4: written by the connect 0x2E2A0 and
   by 0x93180) are not a player's and not cleanly the process's either: attributed by "whose turn allocated it", they
   look like player 1's, and player 2's connect rebuilding them looks like an ownership violation (run 3, a hang).

The common cause: the approach copies player 1's whole initialised state and then tries to find, by scanning raw
memory and by attributing allocations to turns, what in it is player 1's. The title was written for one client:
much of its "client" state is in fact global engine state that a second client's code path rebuilds. Every run so far
found a category the inference did not cover; the next run would likely find another.

### 12.2 Options

| Option | How | For | Against |
|---|---|---|---|
| A. Continue the snapshot and scrub | add each new category as found (more shared sites, more pre-destructor checks) | small steps; the parts that work stay | open-ended; each category needs a run to find; the safety nets turn errors into leaks or half-destroyed objects |
| B. Explicit per-player construction | player 2's regions start from the boot image (no heap pointers to inherit, nothing to scrub); the tables the process's initialisation fills and that are the same for every player (cl_funcs, gEngfuncs, the client game's other constant tables) leave the per-player set or are copied explicitly (only values in the code range); player 2 then runs an explicit list of per-player initialisation steps (the client's Initialize, HUD_Init, the per-player part of CL_Init) with registrations once | no inherited objects at all; what a player has is what its own steps made; checkable statically (every per-player region written by process initialisation must be either constant-shared or in a step) | needs the inventory of process-initialisation writes into per-player regions; a step missed leaves zeros (fails loudly, not silently) |
| C. Shared-once connect | the connect handlers that rebuild process-wide state (delta descriptions, resource and precache registration, model and sprite loading, front end) run for the first client only; the others reuse it; buffers like 0x551EF4 become per player or once | removes run 3's failure class at its source; also the N1 rule (shared resources loaded once) made explicit | needs the list of connect-time handlers and which state each rebuilds (static, svc handler table) |
| D. Ownership by allocation site | each allocation site classified statically (per-player object, shared registry, scratch) instead of by turn; the frees and destructions checked against the class | diagnostics that mean what they say (no "player 1's" delta registry) | only diagnostics: fixes nothing by itself |
| E. Virtualise fixed-address globals | the recompiler rewrites client code's accesses to per-player globals as base + offset, one bank per player (no copies) | no copying; switching is a base change | stored absolute pointers into per-player statics (the HUD list's nodes point at gHUD's members) would need rebasing; a compiler-level change across the client code; much larger risk now |

### 12.3 Recommendation

**B + C, with D for the diagnostics; not E now; stop extending A.** The single process, the shared world and the built-in
server stay; the client contexts (copy-swap, which the runs did not fault) stay; what changes is how a player's
context is made and what a second connect is allowed to rebuild:
1. **Construction (B)**: from the boot image, with an explicit per-player initialisation list; the process's constant
   tables shared. Static inventory first: every write into a per-player region reachable from Host_Init, classed
   constant-shared / per-player step / unknown (the unknowns reviewed before any run).
2. **Connect (C)**: an inventory of the connect-time handlers (the svc_ handlers and CL_ParseServerInfo's calls) and
   of the state each rebuilds; process-wide state built by the first client only; per-player connect state (cl_entities,
   0x551EF0/0x551EF4 if per player) made per player.
3. **Diagnostics (D)**: allocation sites classed statically; the existing pre-destructor checks and the skipped frees
   stay as assertions that fail a test.
4. Only then one supervised run (permission).

Retained from M2 as is: the loopback channels, the port in the address, the registration rules (cvars reused, the
fatal Cmd_AddCommand skipped), the context switch, the turns in _Host_Frame, the leave path's server side.

## 13. M2 revision: explicit construction, shared-once connect, ownership by kind (2026-11-09; compiled, not run)

The user approved B + C + D of section 12. Static inventory first; no gameplay run yet.

### 13.1 Start-up inventory (process initialisation against a player's regions)

Method (tools/state_audit.py index): everything Host_Init (0x5D9E0) reaches by direct calls, plus the two client entry
points it calls through cl_funcs (Initialize 0x18B7A0, HUD_Init 0x18B800); minus what those two reach (they are run
again per player); every write (and every address taken, a possible write through it) into a per-player region.

| Operation | Writes | Class | Handling |
|---|---|---|---|
| ClientDLL_Init 0x1C6F0 (cl_funcs 0x7D6CA0), Initialize's copy of the engine's functions (gEngfuncs 0x5430E8) | function tables | 1 process-wide | not in the per-player regions: shared (the M2 worry that the boot image lacks them was wrong) |
| CL_Init 0x2BA10, 0x2C141..0x2C1A6 | cl list heads 0x749DC4/C8 = 0x749D44, 0x749E4C/50 = 0x749DCC; cls.datagram 0x783808..0x783818 (name 0x32AFF4, data 0x78381C, maxsize 4000); 0x785BA0/A4 = 0 | 3 per player | explicit step ClientState (the values are the player's own addresses or constants) |
| CL_Init's userinfo keys 0x2BA3C..0x2BBA4 (Info_SetValueForKey on cls.userinfo 0x785148) | text | 3 per player | explicit step Userinfo: player 1's text, then the player's own name |
| SV_SetMaxclients 0xA19E4 (from Host_InitLocal 0x5BE30) | cls.state = 1 | 3 per player | explicit step ClientState |
| CL_Init's cvars, commands, file load 0x3F540 + parse 0xB0A30, the free of [0x551FF8], the front end 0xD7710 | shared state | 1 process-wide | not run again |
| the console's command buffer 0x3A8A0..0x3A9C0 (0x424A3C, 0x4251A0/A4) | console | 1 process-wide | **was wrongly per player** (the engine client range ran into it): the range now ends at 0x3A000 |
| the HUD sprite registry 0x551870..0x55187C (0x22C50, 0x1CAC0) | registry | 2 shared resource | **its count was wrongly per player**: listed as shared data |
| the view: 0xB1390, 0xB14B0 (0x3820D8..) | view state | 3 per player | also run in each player's own turns: redone per player |
| the view: 0xB6E40 (zeroes 0x5E13A0.., 0x5E13E0 x 0x700 dwords), 0x1C5D0 (0x785794 = 0) | zeros | - | the boot image is already zero there |
| Netchan set-up of cls.netchan (0x698D0, &0x7812E8) | connection | 4 per connection | done by the connect (0x272D0) |
| the client game's Initialize 0x18B7A0 (its particle manager 0x552B8C) and HUD_Init 0x18B800 (gHUD, the objects 0x552C70, 0x552C9C, 0x552CA8, 0x552BB4) | objects | 3 per player | explicit steps Initialize, HudInit; their registrations answered once (cvars: the registered one; Cmd_AddCommand skipped; commands and message hooks idempotent) |

### 13.2 Explicit player construction (AddPlayer; native_split_core.h InitOrder)

1. **BootImage**: the context from the boot image (taken at 0x202C1E, after the static constructors, before main: no
   heap object exists then); its heap pointers counted: there must be none.
2. **ClientState**: CL_Init's per-player writes and cls.state (13.1).
3. **Userinfo**: player 1's userinfo text (plain text, no pointer), its own name.
4. **Initialize**, 5. **HudInit**: in its own context, as the player's own initialisation.
6. **Check** against player 1's regions at the end of Host_Init (taken at 0x5DD94 as a reference only): every heap
   pointer there (an object player 1's initialisation made) must be, in the new player's state, its own object or a
   shared registry's entry; one it lacks (a step missing) or one naming another's object (an inheritance) **fails the
   initialisation**: its context is removed, its slot emptied, no player is added again this session, and the errors
   say which address and which block.
Nothing is copied from player 1 but the userinfo text; no pointer is restored. The scrub of section 11 is retired.

### 13.3 The connect: shared-once and per-connection (the svc_ table at 0x371CF8, 56 entries)

| Handlers | Class | Handling |
|---|---|---|
| svc_deltadescription 0x2FAE0 | 2 shared resource (the delta registry: the server's and every client's encoding) | built once: a player after the first, for a name already registered, parses into its own slot (guest table native_split_delta_slots; hook 0x2FBA4): it reads the message, the registry stays player 1's (run 3's errors) |
| svc_serverinfo 0x30A90 (-> 0x2E780) | 4 per connection + 5 shared mutation | its cl_entities, HUD_VidInit: its own; the renderer's transparent-entity list (0x551EF4, realloc'd by every connect, 0x93180: rebuilt every frame) is **shared scratch**; Host_ClearMemory is not called while the server runs (CL_ClearState 0x28E6A: `if (!sv.active)`), so no shared cache is flushed by a second connect |
| svc_resourcelist, svc_customization, svc_filetxferfailed, svc_resourcerequest | 4 per connection (cl's lists) + 2 shared (the model cache: Mod_ForName finds loaded models) | per player; model reloading by a second connect not seen in run 3 (to watch) |
| svc_newusermsg | 2 shared registry (the ids are the server's, the same for all) | idempotent |
| svc_packetentities, svc_deltapacketentities, svc_clientdata, svc_spawnbaseline, svc_setangle, svc_addangle, svc_lightstyle, svc_updateuserinfo, svc_pings, svc_setpause, svc_signonnum, svc_time, svc_version, svc_setview, svc_weaponanim, svc_crosshairangle, svc_choke, svc_centerprint, svc_intermission, svc_finale, svc_cutscene, svc_restore, svc_hltv, svc_director, svc_sendextrainfo | 4 per connection (cl, cls, the client game's data) | per player |
| svc_event, svc_event_reliable, svc_temp_entity, svc_particle | 4 per connection (effects) + 5 shared mutation (the engine's particle list 0x6A8198, decals on the shared world) | effects per player; particles and decals made once per player: duplicates (cosmetic; M4 / M7) |
| svc_sound, svc_spawnstaticsound, svc_stopsound, svc_soundfade, svc_roomtype, svc_cdtrack, svc_voiceinit, svc_voicedata | 5 shared mutation (one mixer) | player 2's dynamic sounds gated (M4: a policy) |
| svc_decalname | 5 shared table (the decal names 0x742FCF) | idempotent (the same names) |
| svc_newmovevars, svc_timescale | 5 shared mutation (engine movement variables, host timescale) | the same values for every client (the server's); to verify |
| svc_print, svc_stufftext | 5 shared mutation (the console buffer) | **risk**: a stufftext for player 2 runs from the shared console buffer at the frame's start, in player 1's context (M4) |
| svc_disconnect | 4 per connection, but CL_Disconnect calls Host_ShutdownServer | **risk**: a disconnect sent to player 2 (a kick) would end the server; the leave path forgets player 2's packets first (M4: a per-player disconnect) |

### 13.4 Ownership by kind (heap_owner.h Kind; native_split.cpp kSiteRules)

A block's kind comes from the code that allocated it (malloc's caller's return address, or calloc's): **shared
registry** (cvar_t 0x1BF2A, user message 0x2C716, command 0x3C92A, sprite registry 0x1CB0D, delta descriptions
0x2FB00..0x2FCB2 and their fields 0x44800..0x44A00), **shared scratch** (command arguments 0x3C70D / 0x3C7EE, cvar
strings 0x42000..0x43000, the transparent-entity list 0x931A5, the memory pools' entries 0xC8F30 and 0xC9130, released
by level by whoever runs), else **owned**: by
the player whose turn or initialisation allocated it, else by the process (the server, the console, start-up). A shared
kind is anyone's to free or destroy (the title's own code manages it that way); an owned block only its owner's. Before
the four known destructive sites (0x1A7F7E, 0x1B667A, 0x1BAE4B, gHUD.Init 0x1A6A0A) the object's owner is checked and
another player's is not destroyed (an error); a free of another's owned block by player 2..4 is not done (an error).
Generation-aware identity kept (address, serial, size, site). The engine's general allocators 0xC8E90 (zeroed: the
cvar strings, the file loader, the client's), 0xC9170 and 0xC91A0 are helpers: a hook at their ret gives the block
its helper's caller's site and kind (an allocation's kind is what asked for it); a first version classed the whole
0xC8000..0xC9300 range as shared, which would have hidden most of the engine's ownership errors. A leaving player's objects are released by the title's
own code in its context (client entities, the 0x552C70 / 0x552C9C objects by their deleting destructors, the 0x552CA8
object, the HUD list, its delta slot); what it still owns is reported as a leak (an error, with the sites).

### 13.5 What remains of the snapshot and scrub

- **Retired**: the scrub and the restore of pointers (removed from heap_owner.h and native_split.cpp; in git history
  at 3a42502); a player is never made from player 1's state.
- **Kept, changed in role**: the boot image (M1's capture point 0x202C1E) is the state a player starts from; player 1's
  state at the end of Host_Init (0x5DD94) is kept only as the reference of the check; HeapPointers (the census, which
  changes nothing) verifies; the skip of a foreign free and the pre-destructor checks stay as assertions (errors).

### 13.6 Risks before a run

- The pre-destructor checks cover the four sites seen; other destructive paths of HUD_Init or the connect would show as
  errors (a skipped free) rather than be prevented.
- svc_stufftext and svc_disconnect for player 2 (13.3) are not per player yet.
- Particles, decals and static sounds are made once per player (duplicates).
- The leave path's release of objects (13.4) has never run; the particle manager's destructor is not identified (it
  will show as a leak).
- The two local-player assumptions of 10.7.

### 13.7 The fourth M2 run (2026-11-09, permitted once): player 2 refused by its own checks; causes found; fixed, not re-run

native_split_2p on commit 3559246 (the revision: boot-image construction, shared-once delta, ownership by kind),
one process, built-in server, CSX_NATIVE_SPLIT=2, CSX_CTX_GUARD=1, de_dust2 without bots, headless, hard timeout
180 s: **FAIL (timeout waiting for player 2)**. No crash, no hang: the game ran its frames to the end.
- Player 1: the boot image kept (151 regions, 1058617 bytes); at the reference 17323 process blocks, 133 heap pointers
  in player 1's regions; the match loaded; the server write guard checked 4495 server frames, **0 wrote client
  state** (copy 0.04 ms, compare 0.03 ms a frame).
- Player 2 (2.11 s): context 1 added; its initialisation ran (Initialize, HUD_Init; one Cmd_AddCommand skipped as
  designed) and then **failed its checks, as designed**: not added, slot never asked for, 171 of its blocks
  (667482 bytes) left behind (logged). Three errors:
  1. *missing 0x371970 (shared registry block):* 0x371960 is the static cvar `cl_fixtimerate`; a generated region
     (0x37196C, 8 bytes: its value) ran on into its `next` link. The cvar list is sorted on insertion, so that link
     named a cvar registered later: player 1's list link was swapped out with player 1. Cause: the generator ends a
     region at the next address any code names, so it takes in bytes reached only through pointers. A second static
     cvar (`arrowlt` 0x380D98, its value) was also in a region.
  2. *missing 0x551FE8 (owned, 572 bytes, 0xAF8C4):* the video-mode object (vtable 0x339E68, 640x480x16), made by
     main's own start-up (main 0xBF470 -> 0xAD8A0 -> 0xAF8B0) after the boot capture and before Host_Init: the
     13.1 inventory covered Host_Init only. The "engine view" range 0xAC000..0xB8000 holds the Xbox application and
     video shell too; 10 of its 21 generated regions had no writer in a player's turn: the refresh rate 0x379FA8,
     the flags 0x5518A4 / 0x55194C, the video-mode object and its fields 0x551FD4..0x551FF8, a system object made
     once 0x553614 / 0x55361C (0xAF1F0), the display-mode list 0x5EECA0 (512 bytes), com_argv 0x5EF4E0 (0xAD250),
     the window rectangle 0x6CE204 / 0x6CE210 (0xAF170). All process state (one window, one display).
  3. *a blocked free of a process block (0x1815DE):* std::vector<4-byte>'s grow (0x1814C0) freeing the old buffer of
     the vector at 0x5BD8C8, the client game's pool-chunk list: the pool 0x1F9B90 (0x5517F8.., made by Initialize
     0x18B7A0 -> 0x18B710 -> 0x1F9290 -> 0x1F8E40 -> 0x1F9EC0) is per player, its chunk vector was not: the
     vector's static destructor 0x240160 (atexit, from 0x23F300) writes it and lies outside the client game's range,
     so the generator left the vector shared while the pool around it (0x5BD8C4, 0x5BD8D8) was a player's.
- Fixes (tools/state_audit.py; regenerated client_context_regions.inc: 130 -> 118 generated regions, 139 in all,
  1057617 bytes a player): every static cvar_t (161, found by shape in the XBE) is clipped out of the regions;
  the shell's 13 writers are reviewed shared writers (SHARED_WRITERS, each named); functions registered with atexit
  (0x22C31D) do not count as a region's writers (the pool object 0x5BD8C4..0x5BD8E0 is now a player's whole).
  tests/client_context_test.py checks all three (with the old list it reports the 12 entries).
- The inventory now covers main's start-up as well: everything main reaches by direct calls (not through Host_Init,
  _Host_Frame or the hubs) writes into the regions only the view 0x3820D8 (redone in each turn) and the addresses
  of cl / cls already in 13.1.
- What the failure confirmed: the explicit construction fails closed. Player 2 did not start from player 1's
  pointers, nothing of player 1's was freed or destroyed (the one free was skipped and logged), player 1 played on.
- Not verified (player 2 never connected): checks 3-9 of the run (slots, userids, packets per channel, the delta
  slot, ownership in the turns); the leave path; frame timing with two players.
