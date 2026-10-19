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

## regress_player_control (about 130 s)

Boot to an offline Beginner match on de_airstrip as a Terrorist, wait out freeze time, then check game state through
src/game/state_probe.cpp (position 0x3AE3E0, yaw 0x3AE3F0, Glock shot counter 0x3A2988): idle stays still; 90 frames
of forward stick move 400..1100 units along the yaw (measured 765.1, within 0.0 degrees); 20 frames of right stick turn
at least 30 degrees (measured 129.3); 45 more frames move along the new yaw; three trigger presses fire three shots.

Both scenarios run at FPS=30 (tests/run_regression.sh).

## Other scenarios

`to_keyboard`, `to_menu`, `to_single_player`, `to_match`, `to_airstrip`, `to_spawn` stop at intermediate screens and
are useful while investigating; `keyboard_drawlist` and `menu_drawlist` are meant to be run with `--env CAP=60`.

## Straight into the match

`match_direct.txt` (headless) and `vk_match_direct.txt` (Vulkan window) start at the map load with `--launch
tests/launch/de_airstrip_beginner` (tests/launch/README.md):

    python3 -I tools/drive.py tests/scenarios/match_direct.txt --env FPS=30 --launch tests/launch/de_airstrip_beginner
    python3 -I tools/drive.py tests/scenarios/vk_match_direct.txt --exe build/window/CounterStrike --run build/wrun \
        --env CSX_RENDERER=vulkan --env FPS=0 --launch tests/launch/de_airstrip_beginner

The regression keeps booting through the front end (it covers the menus and the relaunch).

## Window build scenarios (manual, open a window on the desktop)

`window_to_menu.txt` and `window_to_match.txt` drive `build/window/CounterStrike` (tools/build_window.sh) with SDL key
events and save the window's output: `python3 -I tools/drive.py tests/scenarios/window_to_menu.txt --exe
build/window/CounterStrike --run build/wrun`. Not part of run_regression.sh (they need a display).
