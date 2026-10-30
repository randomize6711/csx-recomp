# Autonomous implementation pass N4.8.1..N11 (continuation checkpoint)

The user's mandate (2026-10-30): implement every requested feature of N4.8.1..N11 in order, without stopping per
milestone; each feature implemented, committed, tested with targeted tests, documented, then the next. Blockers are
documented and the work goes on with the independent features. Stop only for privileged system changes, destructive
operations, security-sensitive decisions, unavoidable major architectural changes, credentials or purchases. After
N11: a playable Linux build checked to reach gameplay, one comprehensive report with a manual test checklist, then wait.
No performance optimization and no complete regression until the user approves them after manual testing.

Excluded entirely: **Condition Zero: Deleted Scenes** (no investigation, no content, no placeholder; one ROADMAP entry).
Standard Condition Zero radio, skins, maps and cosmetic packs stay in scope.

Branch g8.2b only (no merge to main), no Windows work, original game files and the DLC folder
(`~/Documents/Decomp/cs xbox/Counter-Strike DLC`, a path with spaces) never modified, no leaked source,
networking authentication and server privacy never weakened, offline play, four-player split-screen, profiles,
the separate server and the CS Xbox look kept.

## How to resume

Read this file, then the newest milestone document named in the table, then `git log --oneline -20`. Working rules
(also in the user's memory notes): never run game or host binaries bare (tools/run_window_tests.py, tools/drive.py,
tools/run_net_tests.py, run_game.sh); no builds while a test run is going; hooks in config/game.toml need
tools/iterate.sh (regenerates the game code) before tools/build_window.sh; `pkill -f` never with a pattern in your own
command line; other projects' processes (kuf_cod, halo*) never signalled; tests wait for 10 GB of free memory; real
audio never on the speakers; commits as randomize6711 with the Co-Authored-By trailer; " m sdk" dirty is expected (SDK
changes only through patches/sdk/*.patch).

## Status

All N5B..N10 code is in one combined commit: the features share files (pc_menu.cpp, config/game.toml, csx.h,
window_input.cpp, server_config.cpp), so the hunks could not be split per feature. N11 has commits of its own.
Test results are in "Targeted test runs" below.

| Item | Status | Where |
|---|---|---|
| N4.8.1 Leave Match | done (e624919) | docs/splitscreen.md 19 |
| N5A Escape, Exit Game, PC Options' screens, smooth scrolling | done (71cc8c0; fix 81038b0) | docs/n5_usability.md |
| N5B Details, collision text, typed numbers, the graphics panel, categories, map search | done, tested | docs/n5_usability.md |
| N5C ammo, autobuy/rebuy, radio menus, CZ radio (our servers), grenade keys, scoped sensitivity | done, tested | docs/n5_usability.md |
| N5D controller detection, 9 prompt styles, Steam Controller dedupe | done; tested with virtual devices only | docs/n5_usability.md |
| N5D Wii Remote motion/pointer | not done (prompts and SDL's HIDAPI Wii driver only) | docs/n5_usability.md |
| N5E audio device, background audio, scoreboard toggle, viewmodel FOV, net_graph | done, tested | docs/n5_usability.md |
| N5F single window when one local player is left | done, tested | docs/splitscreen.md 20 |
| N6A official DLC (Inferno, Office from the disc; the title update's code changes are Live / system link versions only: nothing to reimplement) | done, tested | docs/n6_dlc.md |
| N6B map import pipeline (Sienna CZ imported) | done, tested | docs/n6_maps.md |
| N6C Deathmatch (team; respawn, protection, frag limit, buy at each spawn) | done, tested; free-for-all not done | docs/n6_modes.md |
| N6D Gun Game (23 levels, configurable order, win) | done, tested; the win itself is checked by hand | docs/n6_modes.md |
| N6E mode registry (Match Options, party Rules, csx-server, console) | done, tested | docs/n6_modes.md |
| N7 death camera, gravity, sprays, flashlight, flashbang colour, controller chords | done (spray: server side; what the client draws is checked by hand) | docs/n7_gameplay.md |
| N7 ragdolls | investigated, not implemented | docs/n7_gameplay.md |
| N8 mod loader (manifests, versions, dependencies, order, per profile, Mods screen, content-mod check) | done, tested | docs/n8_mods.md |
| N8 CS 1.6 / CZ skin packs (tools/make_mod.py) | done for models and sprites; sounds not (wave banks) | docs/n8_mods.md |
| N8 Half-Life compatibility | investigated by layer; nothing claimed | docs/n8_mods.md |
| N9 invite codes, console admin, map rotation, server GUI | done, tested | docs/n9_online.md |
| N9 Steam / Discord | blocked: they need the user's developer registrations | docs/n9_online.md |
| N10 HUD layout, editor, HUD scale, split layouts | done (element map from a diagnostic run) | docs/n10_hud.md |
| N10 HD artwork | export tool only; import, HD preset artwork not done | docs/n10_hud.md |
| N11 65 stable; 96 experimental (built and tested: 95 bots on a server, 96 players in the game, protocol 48 refusing 47); 128 not built (voice masks, short immediates) | done for 96 | docs/n11_slots.md |
| Final: playable build, report, manual checklist | done 2026-10-30: build verified to reach gameplay (vk_custom_gamemode), docs/report_n5_n11.md, docs/manual_checklist_n5_n11.md. **The pass is complete; waiting for the user.** | |

## Targeted test runs (2026-10-30)

| Run | Build | Result |
|---|---|---|
| 1 | N5B..F + first N6/N7 code | Headless 7 of 8: regress_gameplay_n7's spray step looked down with +lookdown, which did not move the view (test fixed: setview). Window 26 of 31. The 5 failures were expectations my changes moved: Match Options' rows, the graphics panel's coordinates, the radio sound's log race, the map counts. **A real bug was among them: Inferno listed twice, fixed.** |
| 2 | all N6..N10 code | Headless 7 of 7 (regress_gameplay_n7, diag_hud_draws, regress_mode_deathmatch, regress_pc_bind_capture, regress_pc_options, regress_pc_menu_mouse, regress_player_collision_off). Window 13 of 15. vk_mods and vk_import_map failed on report lines logged before the game's log was open (fixed: logged again later). |
| 3 | final build | Headless regress_mode_gungame passes. Window: vk_hud_layout, vk_mods, vk_death_camera and vk_split_teams pass. vk_import_map failed, and playing Sienna CZ found four import bugs: no player models, a missing prop model, the entity parser corrupting decal names, and the nav file's recorded BSP size (fixed in tools/import_map.py: docs/n6_maps.md). |
| 4 | final build | vk_import_map passes: Sienna CZ loads, the bots join and fight. |
| Unit | | stuff_filter 14/14, server_config 38/38, profiles 16/16, guest_tables, slot_audit, server_gui 21/21: all pass |

Earlier runs on the N5B..F build (before the N6 code): headless 13/13, and window tests up to the point they were
stopped (see run 1).

Window tests are run off the desktop in headless gamescope. Controllers are virtual (SDL) only: **no physical device
was used** in any test.

## Findings for later items (investigated, not built yet)

- **N5D menu images.** The menus' images are named textures in XPR1 files (tools/sxwad.py: lists and extracts the
  SXWad archives, zlib; `xpr FILE` lists an XPR1's names). The shared ones are in d:\media\textures\<lang>\launcher.xpr
  (inside FrontEnd.SXWad: 102 named DXT textures: every legend strip, Cancel_Accept_Reset ... 1024x128 DXT1 with the
  buttons drawn in, Arrows_Selected, MainMenu_Highlight, the map previews, Up_Arrow, Left_Trigger ...); each screen
  may also have Z:\media\piemenuui\<screen>.xpr. The UI loads textures into a registry: 0xC4E80(registry 0x553E78,
  path, 0, 0) (0xC40B0 formats "d:\media\textures\%s.xpr"); 0x72840 looks one up by name (edx 5). A generated XPR1
  of prompt glyphs could be loaded into the same registry from a hook after the launcher's load (0xAF6FB..0xAF72A).
- **N6A map previews.** launcher.xpr already has previews named cs_office, de_inferno (the DLC maps) and cs_downed
  (no cs_downed map in the game folder); the loading screens are in FrontEnd.SXWad too
  (media\textures\English\loadscreens\cs_office-1.xpr).
- **N5E viewmodel.** The weapon model is drawn in 0x4D300 (r_drawviewmodel's value 0x3772C8 read there; 0x4D650 also
  reads it) with the world's projection (MYgluPerspective 0x4DF80 in R_SetupGL); the projection reaches the draws
  through SetTransform and vertex shader constants, so a viewmodel FOV must set the game's projection around that draw.
- **N5E net_graph.** SCR_DrawNetGraph-like code at 0x6E4B0 reads net_graph (value 0x3787E4); 0x6E2D0 places it by
  net_graphpos (0x378820: 1, 2, else); cl_showfps' value 0x3719D0. Whether they draw is to be checked in a match.
- **N5C client money.** The HUD's money (the Money message, handler 0x1AE160) is at 0x552C74; the RoundTime message's
  client handler is 0x1D4A90. The radio function is 0x117250 (player in ebx; id, text, pitch; pops them); the radio
  commands' checks are 0x13E600 (player +0x314 next radio time, +0x318 messages left). client_t::edict +0x4B9C,
  edict +0x7C the CBasePlayer.
