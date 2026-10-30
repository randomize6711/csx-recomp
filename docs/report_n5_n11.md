# Report: autonomous pass N4.8.1..N11 (2026-10-30)

Branch g8.2b (not merged). Linux only. Condition Zero: Deleted Scenes was not investigated or used: one ROADMAP
entry, "Future — Condition Zero: Deleted Scenes integration. Awaiting user specification." The DLC folder and the
original game files were only read (hash list: tests/dlc_sha256sums.txt).

## Playable build

- `./run_game.sh` builds (if needed) and starts `build/window/CounterStrike`, which boots from the intro movies.
- Verified on the final build (commit be4d28b) with `vk_custom_gamemode`, in headless gamescope:
  1. intro movies;
  2. front end, Custom Game, Match Options (Deathmatch, Low gravity);
  3. map list, de_aztec;
  4. the local server starts, the player joins a team, bots fight and respawn.

## Commits

| Commit | Content |
|---|---|
| e624919 | N4.8.1 Leave Match |
| 71cc8c0, 81038b0 | N5A Escape / Exit / PC Options screens / smooth scrolling, and its fix |
| c2fa21e | Continuation checkpoint, roadmap, DLC inventory and hashes |
| e97c3e2 | N5B..N10 (one combined commit: the features share the same files) |
| be4d28b | N11, the experimental 96-slot build |

## Features

### Fully implemented and tested

**N5 menus and input**
- Details screens, typed numbers and the explanation panel beside the graphics options.
- Settings categories: Video, Advanced Graphics, Mouse, Keyboard, Controller, Audio, Gameplay, HUD and Menus,
  Multiplayer, Mods.
- Searchable map lists (keyboard letters; the controller's Y opens a keyboard).
- CS 1.6 buy aliases, autobuy/rebuy, grenade keys, radio menus, CZ radio lines (on our servers), scoped sensitivity.
- Prompts for 9 controller styles from detected devices; Steam virtual/physical de-duplication.
- Audio device choice, background audio, scoreboard toggle, weapon field of view, net_graph.
- One window again when player 1 is left alone in split-screen.

**N6 maps and modes**
- Inferno and Office listed (from the disc) with their previews.
- The title update's code changes are Live / system-link versions only; nothing needed reimplementing.
- Map import pipeline. Condition Zero's de_sienna_cz loads; the bots join and fight.
- Deathmatch (team): respawn, spawn protection, frag limit, buying at each spawn.
- Gun Game: 23 levels, configurable order, knife-kill win.
- Game modes in Custom Game's Match Options, the party Rules, csx-server and the console.

**N7 gameplay**
- First-person death camera.
- Gravity presets plus a typed custom value (server-authoritative).
- Flashlight (F, plus a controller chord) and spray (T, plus a controller chord); both are server side.
- White or black flashbang.

**N8 mods**
- Manifests, versions, dependencies, conflicts, load order, per-profile, Mods screen, "Original Xbox" switch.
- Mods replace files without touching the game folder.
- Content-mod check at signon (mismatched clients are disconnected).
- `tools/make_mod.py` for CS 1.6 / CZ skin packs (tested with CZ's AK-47 and GIGN).

**N9 online and server**
- Invite codes: F6 copies, Ctrl+V pastes into Join by Address, and the server's key is pinned.
- Server console admin commands, map rotation.
- `tools/csx_server_gui.py`: presets, start/stop, status, logs, players, kick/ban/unban, say, map, password, LAN /
  master visibility; local-only with a token.

**N10 HUD**
- HUD layout: 9 elements moved and resized, normal and split-screen layouts, per profile.
- In-game editor (F7) with safe bounds; HUD Scale settings.

**N11 slots**
- 65 stays the stable build.
- An experimental 96-slot build:
  - 95 bots on its server pass;
  - 96 players in its game pass;
  - protocol 48 refuses the stable client (47).

### Partly implemented

| Item | Done | Not done |
|---|---|---|
| Spray (N7) | The server sprays, logged | What this game's client draws without a custom logo is unverified (manual) |
| Skin packs (N8) | Models and sprites | Sounds: this game plays sounds from XACT wave banks; a bank rebuild is not written |
| HD UI (N10) | `tools/xpr_export.py` (890 textures decoded to PNG for Krita or any editor) | Import back into the game (needs a DXT encoder), HD artwork itself, a separate HD preset |
| Wii Remote (N5D) | Prompts, SDL's HIDAPI Wii driver on | Motion and pointer |
| Party prompts (N5D) | | The party screen's text prompts are not style-aware |
| Deathmatch (N6C) | Team deathmatch | Free-for-all (needs the game's team checks for damage, frags and the team-killer kick) |
| Half-Life (N8) | Investigation by layer (docs/n8_mods.md) | Nothing claimed; a separate game module is the user's decision |
| 128 slots (N11) | Audit | Not built (voice masks for 4 dwords, 8-bit immediates) |

### Not implemented

- **Ragdolls (N7).** Investigated: they need the Xbox studio renderer's bone setup found, Jolt vendored and a world
  mesh per map. Several days of work; recorded in TODO.md.

### Blocked (need the user)

- **Steam invitations** need a Steam App ID (a Steamworks partner registration).
- **Discord invitations** need a Discord application ID (developer portal).

Invite codes work in either service's chat meanwhile.

## DLC (N6A)

- **Source.** `~/Documents/Decomp/cs xbox/Counter-Strike DLC`: 37 files, SHA-256 in
  tests/dlc_sha256sums.txt, never modified.
- **Map packages.** The Inferno and Office packages are unlock tokens; the maps are on the disc. Both are now listed.
- **Title update.** A rebuild with the same content. Its game code differs only in addresses. Its behaviour changes
  (Live and system link versions) are replaced by the PC transport. Excluded: those Live changes, and the Xbox
  executable itself (never applied).

## Targeted test results (details: docs/autonomous_pass.md "Targeted test runs")

| Area | Tests | Result |
|---|---|---|
| Final build: front end to gameplay | vk_custom_gamemode | pass |
| Modes | regress_mode_deathmatch, regress_mode_gungame, vk_custom_gamemode | pass |
| Gameplay | regress_gameplay_n7, vk_death_camera | pass |
| Maps | vk_map_search, vk_import_map | pass |
| Mods | vk_mods | pass |
| HUD | vk_hud_layout, diag_hud_draws | pass |
| Regression of touched areas | vk_bind_capture, vk_menu_escape, vk_vsync, vk_profile_settings, vk_party_rules, vk_split_2, vk_split_teams, vk_local_server, vk_bot_count_43, vk_xbox_effects_169, vk_buy_radio, regress_pc_*, regress_player_collision_off | pass on the final or previous build |
| 96 slots | exp_server_96, exp_bots_aztec_95, exp_protocol_96 | pass |
| Unit | stuff_filter, server_config, profiles, guest_tables, slot_audit, server_gui (21 checks) | pass |

Earlier in the pass on the N5B..F build (before the N6 code): headless 13/13, window 26/31 (the 5 failures were
test expectations the new rows and panels moved, plus the Inferno duplicate, now fixed).

**No complete regression was run** (the user's instruction: only after their approval). **No physical controller
was used**: every controller path was tested with virtual SDL devices.

## Bugs found and fixed in the pass

- **Custom Game listed Inferno twice.** A row was inserted into the middle of the row before it.
- **Reports never reached the log.** The overlay and mod reports were logged before the game's log was open; they
  are now logged again later.
- **The map importer, found by playing Sienna CZ:**
  - the template's player models were dropped, and no `playermodels.txt` line was added;
  - prop models the entities name were missing;
  - braces inside quoted entity values corrupted the entity text;
  - the nav file's recorded BSP size made the bots refuse it.
- **The 96-slot build** first used the 65-slot guest table layout header, and its generated folder name hid the
  game sources.
- **A HUD-editor lock re-entry**, found in review, before it could deadlock.

## Known bugs and open points

- **Radio menu text.** In one window shot the radio menu's text was drawn several times on top of itself, right
  after it opened. Not investigated: possibly the title's own animation. Look at it in play.
- **Death camera.** The first-person camera can end up partly inside the player's own body model; it was softened
  (34 units, 70°). Judge it by eye.
- **Deathmatch HUD timer.** The round timer runs down to 0:00 and stays there (the server ignores it).
- **Objectives in Deathmatch and Gun Game.** The bomb and hostages still play out but end no round.
- **Gun Game pickups.** Weapons dropped by the dead can be picked up.
- **Mod models and textures.** A CS 1.6 / CZ model replaced through a mod may take this game's texture of the same
  name. This game keeps model textures in its XPR registry, not in the .mdl. Check by eye (checklist A18).
- **Refused 96-slot connections.** A stable client refused by a 96-slot server stays at "not connected" (no menu
  path leads there).

## Manual play-testing

docs/manual_checklist_n5_n11.md groups the checks:

| Group | Items |
|---|---|
| A. Alone | 1–18 |
| B. Extra controllers | 19–21 |
| C. Another person | 22–23 |
| D. Another PC | 24–26 |
| E. External accounts | 27–28 |
| F. Hardware not available | 29–33 |

N4.8's own outstanding manual validation stays in docs/multiplayer_frontend.md section 7.

## Remaining work

The ordered list is in TODO.md "Now", item 0:
- ragdolls;
- free-for-all Deathmatch;
- sound mods;
- HD artwork import;
- Wii motion;
- Steam and Discord (after the user's registrations);
- Half-Life as a separate module (the user's decision);
- 128 slots;
- the radio-menu observation.

Performance optimization and the complete regression wait for the user's approval after manual testing.
