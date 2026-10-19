# Tests

`tests/run_regression.sh` builds the headless harness (Linux) and runs every `tests/scenarios/regress_*.txt` with
`tools/drive.py`. A scenario drives the game through the harness's controller `cmd` file while watching the log,
takes snapshots with the reference renderer and checks log lines and image statistics. Game pixels are never
stored in the repository; an image check only measures luminance spread (a blank or flat frame fails).

Requirements: `build/gamedata` (a reflink copy of the game folder), the tools built with the `linux-tools` preset.

## regress_boot_to_spawn (about 90 s)

Boot → title screen → START → notice → profile keyboard → Done → main menu → Single Player → Beginner → Airstrip
→ title relaunch with launch data → de_airstrip loads → team selection → spawned player with HUD and buy menu.

What each step depends on (evidence in FINDINGS.md):

| Step | Runtime behaviour it exercises |
|---|---|
| boot to title | APU sample counter (apu_clock.cpp), 5659 layouts, DSOUND 3D voice data (0003), offline XOnlineGetUsers, EOF reads fail before queueing (0006), ETHREAD.ExitStatus (0007), overlay status (0004), GPU time pointer (0005) |
| `multisample type 2012` log | back buffer scale from MultiSampleType (0009) |
| title image | harness clears reach memory (0008) |
| keyboard / menu images | screen-space text not clipped (0011), pretransformed super sample scale (0010), pretransformed z in depth units (0012) |
| relaunch into de_airstrip | harness binary named as GameInfo::images (`CounterStrike`), launch data page |
| spawned image, no trap | in-game D3D/DSOUND/XACT paths on a real map |

Negative check (2026-10-19): with patch 0007 taken out of patches/sdk the scenario fails at its first step
(`wait draws==1`, 0 draws after 600 s: the main thread waits forever for the cache worker's exit code).

## Other scenarios

`to_keyboard`, `to_menu`, `to_single_player`, `to_match`, `to_airstrip`, `to_spawn` stop at intermediate screens and
are useful while investigating; `keyboard_drawlist` and `menu_drawlist` are meant to be run with `--env CAP=60`.
