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

| Item | Status | Where |
|---|---|---|
| N4.8.1 Leave Match | done (e624919) | docs/splitscreen.md 19 |
| N5A Escape, Exit Game, PC Options' screens, smooth scrolling | done (71cc8c0; fix 81038b0) | docs/n5_usability.md |
| N5B Details, collision explanation, typed numbers | built, tests running | docs/n5_usability.md |
| N5B searchable map selection; explanation panel beside graphics options; settings categories (Keyboard, Controller, HUD, Multiplayer) | to do | |
| N5C ammo, autobuy/rebuy, radio menus, CZ radio (our servers), grenade keys, scoped sensitivity | to do | |
| N5D controller identification, prompt styles (9 incl. Wii), Steam Controller, Wii Remote | to do | |
| N5E audio, viewmodel FOV, net_graph, scoreboard | to do | |
| N5F single window when one local player is left | to do | |
| N6A official DLC (inventory first, read only) | to do | |
| N6B map import pipeline | to do | |
| N6C Deathmatch, N6D Gun Game, N6E mode registry | to do | |
| N7 death camera, gravity, sprays, flashlight, flashbang colour, ragdolls | to do | |
| N8 mod loader, skin packs, Half-Life compatibility | to do | |
| N9 Steam / Discord invitations, server GUI | to do | |
| N10 HD UI, HUD editor | to do | |
| N11 96 / 128 slots | to do | |
| Final: playable build, report, manual checklist | to do | |
