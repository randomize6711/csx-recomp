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

`vk_match_direct` checks the walk in game state (state probe works in the window build too). `vk_compare_direct`
repeats match_direct's steps at FPS=30 in the Vulkan window, for comparing its `in_match` shot with match_direct's
`snaps/in_match_2.tga` (tools/compare_images.py). Window key names are SDL's, with underscores for spaces
(`Left_Ctrl`).

The regression keeps booting through the front end (it covers the menus and the relaunch).

## Video

`intro_overlay.txt` (headless, FPS=30) snaps the three boot movies with the overlay composited into the reference
renderer's captures; `vk_intro.txt` takes the same frames from the Vulkan window. `expect image` also checks window
shots (`shots/NAME.bmp`).

## Rendering comparison

`compare_direct.txt` (headless) and `vk_compare_direct.txt` (Vulkan window, `--env CSX_RENDERER=vulkan`) take the
same moments of a direct de_airstrip start at FPS=30, `in_match` and `in_match_late` (45 s later); compare with
`python3 -I tools/compare_images.py build/run/snaps/in_match_late_2.tga build/wrun/shots/in_match_late.bmp OUT.png`.
Both include the title's gamma ramp. Expect about 25 dB PSNR: smoke, rain and bots move between runs.

## Rounds

`rounds_direct.txt` plays complete rounds from the direct start of any map fixture (`de_airstrip_beginner`,
`de_dust_beginner`, `cs_italy_beginner`, same settings as the front end's Beginner match): Terrorist, then each round
moves and fires, and waits for round ends (`count REGEX N`) on the game's own server log lines (`game: ...`,
src/game/game_log.cpp). About 6 to 13 minutes a map headless:

    python3 -I tools/drive.py tests/scenarios/rounds_direct.txt --timeout 900 --env FPS=30 \
        --launch tests/launch/de_dust_beginner [--run build/run_dust]

`vk_soak_direct.txt` leaves the Vulkan window running for eight rounds (real time) as a stability check (39.5
minutes, nine rounds on de_airstrip on 2026-10-19).
Map fixtures other than de_airstrip differ only in `releasemap.txt`'s `+map`.

## 3D sound distance

`regress_audio3d.txt` (in run_regression.sh; `--env` lines in a scenario header are passed to drive.py) checks that
XACT's distance level for a far sound reaches the mixer for the routed source. `tests/audio3d_check.sh [maps]` plays a
minute of each map's direct-start match (`audio3d_direct.txt`) with `CSX_AUDIO_LOG3D=1` (game-side probe: distance
and XACT's level per 3D update) and `XBE_LOG_3D=1` (mixer gain per routed 3D voice) and runs `tools/check_audio3d.py`:
level falling with distance per sound class, the level reaching the mixer, far voices at least 6 dB below near ones.

## Shadows and HUD colours

`regress_hud_colour.txt` checks the mean luminance of the resting health digits in the reference renderer's frame
(`expect region NAME X0 Y0 X1 Y1 MIN MAX`): about 84 with the game's MIN_ALPHA colour, about 120 when the colour set as
current vertex data is lost (SDK patch 0025).

## Decals

`regress_decals.txt` walks into de_airstrip's Terrorist tunnel and checks the far "B" sign at three view angles
0.3 degrees apart (`expect darkfrac NAME X0 Y0 X1 Y1 LUM MIN MAX`: share of the box's pixels darker than LUM): the
sign's ink fills about 0.57 of the box with the polygon offset, 0.00-0.46 and varying without it (SDK patch 0026).

## PC Options

`regress_pc_options.txt` boots through the front end to Options > PC Options (the screen added to the game's own menu
text, src/game/pc_menu.cpp), seeded with `--pc mouse_sensitivity=4` (drive.py writes the run folder's
pc_settings.ini; without --pc a run starts with none), steps the setting to 6, accepts, checks the save in the log and
that Control Options still opens. `regress_pc_options_ingame.txt` does the same from the direct match through the
pause menu (START > Options > PC Options). run_regression.sh passes `--pc` lines of a scenario header like `--env`
ones.

## PC key bindings

`regress_pc_bindings.txt` starts a match with Jump saved on Tab (`--pc bind.jump=TAB`) and sends engine key events
(`pckey key KEYNUM DOWN`, `pckey cmd TEXT`; src/game/pc_keys.cpp polls the run folder's pckey file): Tab jumps
(`expect rose A B MIN`), Space does not, the pad's A still jumps, Mouse 1 fires, R reloads, B opens the buy menu, then
the pause menu's PC Controls restores the defaults (X) and saves (A) and Space jumps. In the window build `click
BUTTON [SECONDS]` and `wheel N` send mouse buttons and wheel notches through SDL like `key`.

## Field of view

`regress_pc_fov.txt` starts a match with FOV 110 (`--pc fov=110`), buys an AWP through the buy menu (+buymenu, the
pie's directions), zooms to its 40 and 10 and back (the game's own FOVs, logged by src/game/pc_view.cpp as "pc view:
fov N"), then sets 115 in the pause menu's PC Options and sees it applied.

## Widescreen

`regress_pc_widescreen.txt` (`--pc aspect=16:9`) checks the 854x480 frame (854x960 back buffer), the Hor+ angles that
src/game/pc_wide.cpp logs for the projection and the culling planes ("pc wide: view fov 90 -> 106.3", "culling fov",
also the AWP's 40 and 10), the scope's black sides (`expect darkfrac` on both side columns) and the pause menu's
backdrop reaching the edges (`expect region`). `regress_pc_widescreen_ultrawide.txt` (`--pc aspect=21:9 --pc
fov=110`) does the 1138x480 frame with the PC FOV (110 -> 137.0), the team menu's and the spectator's backdrops at
both edges. Every other scenario runs at 4:3 (Automatic is 4:3 headless).

## Developer console

`regress_pc_console.txt` opens the game's console with its default key (engine key 96 through the binding), types
through `pckey con TEXT` and edits with `pckey conkey NAME` (enter, tab, up, down, backspace, pgup, pgdn, toggle,
close; src/game/pc_keys.cpp hands them to src/game/pc_console.cpp): echo runs ("pc console: ] echo ..." logged),
sv_gra completes to sv_gravity, Up recalls the history, a typed toggleconsole closes it, the pad's A still jumps,
and the pause menu's PC Controls rebinds it. `regress_pc_console_wide.txt` (`--pc aspect=21:9 --pc
bind.console=F2`) opens it with the saved key only and sets sv_gravity. `vk_console.txt` (not in the suite: the
Vulkan window, keyboard through SDL, `type TEXT` for SDL text input) checks that W and the mouse do not reach the
game while it is open, Escape, the mouse capture and the console over the pause menu.

## Mouse look

`regress_mouse_look.txt` (part of run_regression.sh, starts in the match through its `--launch` header line) turns
the view with `look RIGHT UP` steps, which add degrees through the same hook the window's mouse uses, and checks the
view angles the title keeps (`expect yaw/pitch A B DEG TOL`): exact changes, the -89 degree pitch clamp, 100 small
steps adding up exactly (no acceleration), and the stick still turning. `vk_match_direct.txt` sends relative mouse
motion to the window (`mouse DX DY`) and checks the result at the default sensitivity.

## Audio

`tests/audio_check.sh` runs `vk_match_direct` with the mixer output captured (`CSX_AUDIO_WAV`, null device) and
checks it with `tools/check_audio.py`: not silent, at least one second placed clearly to a side (3D sounds), under
0.5% of samples at full scale, all finite. Needs `build/zcache` (`tools/make_zcache.sh`, about 2 minutes once: the
cache partition the front end fills, with the HD sound banks; a direct start without it has no weapon sounds).
To hear the game in a window run, pass `--env CSX_AUDIO=sdl` (drive.py defaults window runs to the null output).

## Window build scenarios (manual, open a window on the desktop)

`window_to_menu.txt` and `window_to_match.txt` drive `build/window/CounterStrike` (tools/build_window.sh) with SDL key
events and save the window's output: `python3 -I tools/drive.py tests/scenarios/window_to_menu.txt --exe
build/window/CounterStrike --run build/wrun`. Not part of run_regression.sh (they need a display).

## Windows build

`tests/windows_check.sh` cross-builds (tools/build_windows.sh), checks the executable (64-bit PE; imports only Windows'
own DLLs, the packaged SDL3.dll / dxcompiler.dll and the driver's vulkan-1.dll; no MinGW runtime DLL; the functions it
overrides are the Linux window build's plus the XDK select) and, with Wine and a display, runs vk_intro,
vk_match_direct and vk_match on the Windows executable (`drive.py --wine`, prefix build/wineprefix, run folder
build/winrun, network namespace as for Linux runs). `--no-wine` skips the runs. Wine is not Windows: a pass shows the
executable and its Windows code paths work under Wine.

