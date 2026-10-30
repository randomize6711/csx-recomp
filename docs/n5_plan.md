# N5 plan: menu usability, input, controller prompts, settings, quality of life

Proposed 2026-10-30 on g8.2b after N4.8 (automated regression complete; its manual checks postponed and kept in
docs/multiplayer_frontend.md section 7). **Not started: waiting for the user's approval.** Server performance work,
Windows porting and the merge of g8.2b stay out of N5.

Constraints for every checkpoint: the original CS Xbox look (the title's own screens, fonts, legends, sounds; screens
written in its UI text format, as PC Options is), settings only in the game's menus (no overlay, no host-drawn UI), the
networking, four-player split-screen and the 65 slots unchanged, nothing that only works against our own servers unless
marked so, each checkpoint its own commits with tests.

## 0. What exists (the inventory, 2026-10-30)

| Area | Present | Where |
|---|---|---|
| Menu keys | Keyboard is the pad in menus; Escape is START, except on the browser and party screens (B) | src/linux/window_input.cpp:600-640 |
| Scrolling | The title's tables move a row at a time (first row shown +0x120); wheel = d-pad | src/game/pc_mouse.cpp:427, FINDINGS "Mouse in the menus" |
| Quit | No Exit row in the main menu (Xbox); N4.8's Quit Game (pause) quits through csx::AppQuit | src/game/pc_lobby.cpp, window_main.cpp |
| Settings | Title's Options: Controller, Sound, Online, Credits; ours: PC Options (14 mixed rows: mouse, video, graphics, audio, server), PC Controls (28 bindings), hidden Filters, Split, Profile, Rules | src/game/pc_menu.cpp:110-349 |
| Help text | Two lines per row (the title's help table) | PcScreen.row_help |
| Player collision | Custom Options row and lobby rule; one help line | pc_menu.cpp:342, 470; pc_collision.cpp |
| Value entry | Fixed choice lists only; typed text exists for names and passwords (the title's keyboard, PC typing) | pc_mouse.cpp, pc_party.cpp |
| Buying | Title's buy pies (`buy weapon_x`, `buyequip`); ammo by the triggers inside the menu; `buyammo1/2` exist in the XBE, unbound; no autobuy/rebuy | InGameUI buy menus, pc_bindings.cpp |
| Radio | Every 1.6/CZ radio command exists (radio1-3, coverme ... enemydown, needhelp); 4 bound (Z X C V); the sound banks hold extra CZ lines (circleback, flankthem, meetme, matedown, ...) with no command | pc_bindings.cpp:119-122, hdcounterstrike.xsb |
| Grenades | slot4 cycles grenades (our fast-select hooks); weapon_hegrenade/flashbang/smokegrenade exist, unbound | pc_keys.cpp:241-264 |
| Zoom sensitivity | None for the mouse (the stick's zoom_sensitivity_ratio is bypassed); the zoom FOV is known each frame | mouse_look.cpp, pc_view.cpp:111 |
| Button prompts | Title legends are baked Xbox textures (Cancel_Accept...); our prompts are text (keyboard names for the keyboard member); no gamepad type read anywhere | pc_party.cpp:195, FINDINGS:885 |
| Audio | Master Volume (PC); the title's Sound screen (effects, voice, ambient, soundtrack 0-10) front end only; cvars sound_FXVolume / VoiceVolume / MusicVolume; device fixed to the default | sdl_audio.cpp:75-166 |
| Viewmodel FOV | None: the weapon is drawn with the world's projection | FINDINGS 745-762, "Weapon over the HUD" |
| net_graph | The engine's cvars and strings are in the XBE (net_graph, net_graphpos, cl_showfps); never tried | analysis/netcode_engine.md:245 |
| Scoreboard | 65-player board, Ping/BOT, scrolling; hold only (BACK / Tab) | scoreboard.cpp |
| One player left in split-screen | The compositor keeps compositing one full-window view (about 18% fewer frames/s than the plain window, an extra copy and present); player 1's frame keeps its boot aspect | compositor.cpp, docs/splitscreen.md 11/17 |

**A bug found by the survey (N4.8):** a player 2-4 that chooses Leave Match ends its process, but the compositor keeps
its slot configured, and player 1's next "join" (every map change relaunch sends one) respawns it
(compositor.cpp:646 OnMatch; 1265 told_end; g_configured never decremented). Leave Match therefore lasts only until
the next map. Proposed as the first fix of N5A (or as N4.8.1 before it), with a test (leave, then a map change).

## Status (2026-10-30)

N5A done (71cc8c0, 81038b0). N5B to N5F are built and tested with targeted runs, in the autonomous pass's combined
commit (docs/autonomous_pass.md). Results and limitations are in docs/n5_usability.md. Not done: Wii Remote motion
and pointer (prompts only), and the party screen's text prompts (not style-aware). N5G's complete regression is
deferred by the user's mandate: targeted tests only until they approve.

## 1. Checkpoints

### N5A: Escape navigation, smooth scrolling, Exit, reorganized settings

Work:
- **Escape is B (back / cancel) on every menu screen**; Enter stays START. In a match: Escape opens the pause menu
  (START, as now) and closes it again (B on its root). Typing fields keep Escape as "stop typing". The controller is
  unchanged. Documented key map in the screens' help bars where the title's legend allows.
- **Smooth scrolling** of long tables (maps, PC Controls, rules, servers, profiles): the table's first-shown row eases
  over a few frames instead of jumping (a pixel offset applied in the table draw 0xCD030 between the old and new first
  row; rows clipped to the table's rectangle). The wheel scrolls the list (not only the selection); keyboard and pad
  repeat stay the title's.
- **Exit** in the main menu: an "Exit Game" row (before the title's last row) with the title's confirmation style
  (Yes / No), quitting cleanly (the local server and split players end as with Quit Game). Not on the title screen.
- **Settings reorganized** under the title's Options (Controller, Sound, Online, Credits stay the title's):
  PC Options becomes a hub of the title's style with sub-screens: **Video** (display mode, resolution, aspect, frame
  rate, VSync), **Graphics** (Xbox effects, auto exposure, bloom, FOV), **Mouse** (sensitivity, invert, raw, later zoom
  sensitivity), **Audio** (N5E), **Game** (match server, later HUD / scoreboard), and PC Controls grouped (Movement,
  Combat, Weapons, Communication, Interface) with section labels. Same keys in pc_settings.ini and the profiles: no
  value lost, no migration needed. The pause menu's PC Options the same.

Acceptance:
- Escape backs out of every front-end and pause screen to its parent (a test walks every screen with Escape only),
  never quits or starts anything; the controller's buttons behave as before (existing tests unchanged).
- Long lists scroll without a jump (frame captures show intermediate offsets; no row drawn outside the table).
- Exit Game asks, then quits; No returns; no orphaned process (the runner's orphan check).
- Every former PC Options setting reachable in its new screen with its saved value; old pc_settings.ini files load.
- Within the limits: 60 BackgroundText lines and 8 BackgroundImages a screen (FINDINGS N4.8).

Risks: smooth scrolling hooks guest drawing code (medium: table layouts differ per screen; the 640x480 UI under
widescreen); Escape as B changes muscle memory and the tests' Escape steps (low; tests updated); deeper menus add a
press (mitigated by the hub keeping the old row names).

### N5B: descriptions, the collision explanation, numeric entry

Work:
- **Contextual descriptions**: every PC setting gets a clear two-line help (the title's help table) and, where two
  lines are not enough (graphics, collision, frame rate vs VSync, raw input), a **Details** page on Y in the title's
  style (a screen of BackgroundText: what it does, the original's value, the cost, interactions). Descriptions say what
  is measured (e.g. bloom's cost from the benchmarks), not guesses.
- **Player collision explained**: what Off changes (bodies pass through; shots, grenades, hitboxes, hostages, doors
  unchanged), when it applies (fixed for the match from its start), where it is offered (single player, Local Only,
  LAN hosts; not on remote servers).
- **Direct numeric entry** for numeric settings (sensitivities, FOV, frame rate limit, volumes, rules such as money,
  round time, bots): A on the value or a key opens the title's keyboard in number mode (digits, point, minus where
  valid) or PC typing; the value is validated against the setting's range and shown as "(custom)" when not a list
  choice (as Resolution's custom WxH). Lists keep their arrows.

Acceptance:
- Every PC row has a non-empty, accurate help; Details opens and returns with Escape / B.
- A typed sensitivity of 2.75 saves, applies (measured turn per count) and reloads; out-of-range and non-numeric input
  is refused with a message; rules typed in the lobby reach the match line (server_config_test).

Risks: the title's keyboard has no number layout (low: PC typing and a digits-only filter); custom values in profiles
and rulesets (low).

### N5C: buying, radio, grenades, scoped sensitivity

Work:
- **Ammo buying, 1.6/CZ style**: bindable Buy Primary Ammo (`buyammo1`), Buy Secondary Ammo (`buyammo2`), Buy Full
  Ammo (both repeated until full), and the buy menu's own ammo tabs kept. **Rebuy** (the last round's purchases again)
  and **Autobuy** (a saved list) as our own client feature: we record the `buy weapon_*` / `buyequip` commands sent and
  replay them; the server's rules (buy zone, time, money, team) decide, so it works on any server.
- **Radio**: every radio command bindable (the 1.6/CZ set is all in the game, with its sounds); Radio 1/2/3 keys open
  a radio menu in the title's style (a pie or table screen like the buy menus, since the old text menus' drawing is
  unverified on Xbox). The extra CZ lines in the sound bank (circleback, flankthem, meetme, matedown, ...) have no
  server command: offering them needs server-side code (our servers only): **proposed as optional**, after approval.
- **Independent grenade keys**: HE, Flashbang, Smoke, each a binding selecting that grenade through the same path as
  our fast-select (so the HUD's selection and last-weapon history stay right).
- **Scoped mouse sensitivity**: Zoom Sensitivity (ratio, default 1 = today's feel) applied while the zoom FOV is in
  effect, per zoom level optional; a profile key (added to the profile, party, split and menu lists).

Acceptance:
- Each new action rebindable in PC Controls (and its pad mapping where the title has a button), saved per profile.
- buyammo keys buy in the buy zone and refuse outside it (the server's message); Rebuy repeats a recorded loadout on
  the next round, team-specific weapons skipped; works on csx-server and a listen server.
- Every radio command plays its line to the team (bots react as to the pad's), radio menus navigable by keyboard,
  mouse, pad; split-screen players' radio stays per player.
- Grenade keys select the right grenade or nothing (none held) and the HUD shows it; last-weapon works after.
- At zoom, mouse counts turn by the ratio (measured), unzoomed unchanged; per split player.

Risks: radio menu screens are new UI (medium); rebuy timing against the buy time (low); CZ extra lines need server
changes (excluded unless approved).

### N5D: controller button prompts

Work:
- Read each controller's type (SDL_GetGamepadType: Xbox 360, Xbox One/Series, PS3/PS4/PS5, Switch Pro / Joy-Cons,
  Steam Controller / Deck, generic) and send it with the member's state to each player process.
- **Prompt styles**: Automatic (by the player's controller), PC (keyboard and mouse names), Original Xbox, Xbox 360,
  Xbox Series, PlayStation, DualSense, Switch, Steam Controller; a Controller Options or PC Options row, per profile.
- **Two layers**: our text prompts (party, lobby, browser, hints) use the style's names (A/B, Cross/Circle, B/A swapped
  for Nintendo's layout positions, Steam's A/B/X/Y); the title's baked legend images (Cancel_Accept...) are kept for
  Original Xbox and replaced per style only with **new legend art in the title's own style** (same size 640x69, font,
  layout), or, if no new art is wanted, a text legend over the title's legend background.

Decision needed (assets): the game only has Original Xbox legend art. PlayStation, Switch and Steam glyphs would be
new art drawn in the CS Xbox legend style (no copied console artwork), or text-only prompts. See section 3.

Acceptance:
- Automatic picks the right style per member when a controller is plugged in; four different controllers show four
  styles on their own panels and screens; keyboard shows key names.
- Every prompt we draw and every legend on the PC screens follows the style; the title's own screens' legends follow
  it if new legend art is approved.

Risks: legend images are per screen (8 images a screen), and the title's screens name them in their text (medium: a
text substitution at load per style); Nintendo's A/B positions (confirm/cancel semantics stay the title's); asset
approval.

### N5E: audio settings, viewmodel FOV, net_graph and the scoreboard

Work:
- **Audio** (a PC Options > Audio screen): master volume (exists), effects, voice, music volume (the title's own
  sound_* cvars, so the Xbox profile's Sound Options and ours agree: one value, two screens), output device (SDL's
  devices; reopened on change; split players follow player 1's), a split-screen balance option (the current 1/sqrt(N)
  gain shown and adjustable). The title's Sound screen stays.
- **Viewmodel FOV**: a separate FOV for the weapon (the original's 90 default). Needs research first: the weapon's
  draw has to be found (R_DrawViewModel under R_RenderView 0x4EF00) and given its own projection without changing
  culling, the HUD depth handling (FINDINGS "Weapon over the HUD") or scopes. **Research step with a go / no-go**: if
  it cannot be done cleanly in both renderers, it is dropped and documented.
- **net_graph**: first check that the engine's net_graph and cl_showfps draw at all on this build (the developer
  console); then a Game screen row (Off / 1 / 2 / 3, position left / centre / right via net_graphpos or its draw
  origin, inside the widescreen UI rules). An FPS display (cl_showfps) beside it. If the engine's graph does not draw,
  N5E reports it and does not draw one of our own (no host overlay).
- **Scoreboard**: Hold / Toggle (a Game row; BACK and Tab toggle), keep the scroll position while open, and optional
  "show on death".

Acceptance:
- Volumes change what is heard (captured levels measured, never on the speakers in tests), saved, the title's Sound
  screen shows the same values; device change without restart; split players' audio unchanged unless asked.
- Viewmodel FOV: at 120 world / 90 weapon the weapon matches the 90 picture and the world the 120 one (captures, both
  renderers), scopes unchanged; or a documented no-go.
- net_graph rows shown and placed as chosen at every aspect; or a documented no-go.
- Toggle scoreboard opens on one press and closes on the next (keyboard, pad, split players).

Risks: viewmodel FOV is guest renderer research (high); net_graph may be stripped (medium); the title's volume path to
XACT is untraced (medium); BACK shared by the pad's scoreboard and other uses (low).

### N5F: back to the single window when one local player is left

Recommended design (from the survey; nothing restarts mid-round):
1. **Fix first**: a player who leaves (Leave Match, or leaves the party) is removed from the configured players so it
   is not respawned (the N4.8 bug above); START on its controller can join again.
2. **At once, when one player is left**: the compositor shows player 1 alone at full window with no extra work (no
   clear, a 1:1 copy when sizes match), the audio gain back to a single player's.
3. **At player 1's next natural relaunch** (a map change, the end of the match, a return to the menus or the lobby,
   which already reconnect): player 1 restarts as the plain window instead of a split player: the same process (so
   the local server, whose owner check is the process id, stays), the same identity and slot, its server connection
   re-made as at every map change. The compositor gives up its window and waits for player 1, so the launcher and
   the process tree stay as they are.
4. Joining again afterwards works as from the party screen today (N4.8: a second controller restarts as the
   compositor) in the menus; mid-match joining after the handover is out of scope (documented).

Not proposed: switching player 1's rendering to the window mid-round in-process (its Vulkan device and SDL video are
fixed at boot: a re-init of every guest resource), or a new process (the local server would end with its parent).

Acceptance:
- Two players in a Local Only match; player 2 leaves: player 1 keeps playing without a pause or a disconnect; after the
  map change the game runs as the single window (one process, no compositor work, frame rate within 5% of the plain
  window's benchmark), same match, same score, the local server untouched; no orphaned process.
- Same for LAN host and remote server matches; a player leaving the party in the menus; a controller unplugged does
  not count as leaving.
- Four-player split-screen and its tests unchanged; the 65 slots unchanged.

Risks: window recreation on Wayland/gamescope (a brief window change at the map change; medium); fullscreen and
focus after the handover; the pause and the "split player" behaviours player 1 had (pause menu rows, bot room) must
switch to the single-player ones at the relaunch (medium); re-entering split-screen repeatedly (process tree depth,
tested).

### N5G: regression, documentation, commits

The complete suite (every unit, headless, network and window test, as N4.8's final run, the hand-run soak as
documented), the new tests from N5A-F, the benchmarks against N4.8's, docs (a docs/n5_usability.md as built, FINDINGS,
PROGRESS, ROADMAP, TODO, splitscreen.md for N5F), checkpoint commits; stop for approval.

## 2. Order, size and risk

| Checkpoint | Size | Risk | Depends on |
|---|---|---|---|
| N5A | medium | medium (smooth scrolling) | - (the Leave Match fix first) |
| N5B | medium | low | N5A (the new screens) |
| N5C | medium-large | medium (radio screens) | N5A (Controls groups) |
| N5D | medium | medium (legend art decision) | N5A |
| N5E | large | high (viewmodel FOV), medium (net_graph, volumes) | N5A |
| N5F | medium | medium (window handover) | the Leave Match fix |
| N5G | large (time) | - | all |

N5F could go before N5B if the single-window return matters more for play.

## 3. Decisions for the user

1. **Button prompt art (N5D)**: new legend art for PlayStation / Switch / Steam / Xbox 360 / Series drawn in the CS
   Xbox legend style (new files, not console makers' artwork), or text-only prompts with the title's legend images
   kept as they are.
2. **Condition Zero's extra radio lines (N5C)**: leave out (the full standard radio set is in), or add them for our
   servers only (server-side code).
3. **The Leave Match fix**: in N5A, or as a separate N4.8.1 commit before N5.
4. **N5F timing**: the handover at player 1's next natural relaunch (recommended: no disconnect), or immediately with a
   short reconnect (a fresh spawn mid-round).
5. **Escape as B everywhere (N5A)**: confirm (Enter stays START).

## 4. Future roadmap (not in N5, not started)

Added to ROADMAP.md: official map DLC, custom maps, Deathmatch, Gun Game, Condition Zero equipment, gameplay rules,
ragdolls, sprays, flashlight, modding support, Half-Life compatibility research, Steam / Discord invitations, a server
GUI, HD artwork, a HUD editor, larger matches (past 65), and the server performance work and the Windows port already
deferred.
