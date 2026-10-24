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

## Graphics fidelity audit

`vk_audit_effects.txt` (Vulkan window, `CSX_VK_AUDIT=1`, 4:3) and its headless twin `audit_effects.txt` (reference
renderer, FPS=30) take the same shots in an empty match (`tests/launch/de_dust2_nobots`; also `de_aztec_nobots`,
`de_inferno_nobots`): four views at the spawn, a pistol shot (a burst of frames in the window), the three grenades
from the buy pie. Not a pass/fail check: compare the shots side by side and read the window's `audit:` log lines
(every render state, texture kind, small-target pass, additive draw and gamma ramp change; FINDINGS.md "Graphics
fidelity audit"):

    python3 -I tools/run_window_tests.py vk_audit_effects.txt
    python3 -I tools/drive.py tests/scenarios/audit_effects.txt --run build/wruns/audit_effects --env FPS=30 \
        --launch tests/launch/de_dust2_nobots

## Auto exposure

The title's dynamic gamma (FINDINGS.md "Dynamic gamma"), with `CSX_LOG_DYNGAMMA=1` (its measurement and ramp points
once a second; drive.py `exposure NAME`, `expect exposure`, `expect exposurediff`, `expect exposuretrack`):
`vk_auto_exposure.txt` (Vulkan: the late read-back, settling, floor / sky / back, the pause menu's identity ramp, after
a flashbang, PC Options' Auto Exposure Off and On by mouse), `vk_auto_exposure_rates.txt` (30, 120 and 240 frames per
second: the same points, the rates held) and `regress_dynamic_gamma.txt` (headless: the reference renderer's 8 bit
target, the setting). `CSX_DYNGAMMA_READBACK=0` leaves the read-back out (the earlier behaviour, for comparisons).

## Flashbang

The flashbang's white-out (FINDINGS.md "Flashbang"), with `CSX_LOG_FADE=1` (the title's screen fade: its start with
the hold and fade ends it was sent with, its alpha ten times a second, its end with how long it lasted in client and
real time; drive.py `fade NAME`, `expect fade`, `expect fadediff`): `vk_flashbang.txt` (Vulkan: white while it holds,
the fade's timing, the exposure, picture and HUD back afterwards (`expect regiondiff`), with Auto Exposure On, Off and
the brightness cvar at 1, the pause menu after it), `vk_flashbang_rates.txt` (30 and 60 frames per second with VSync,
120, 144 and 240 without: the same timing, white at each) and `regress_flashbang.txt` (headless: the reference
renderer's white-out and the fade's timing). The title opens the buy menu itself as the player spawns and closes it
20 s later; the tests buy after that (`wait log pc keys: menu T_BuyMenuRoot`, then 24 s).

Full-screen coverage (`expect coverage BASE SHOT MIN SPREAD`: in nine patches touching the frame's edges, the centre,
the edges' middles and the corners, the fade's opacity against the same view once the fade is over, full in the hold
and even part way through the fade; Auto Exposure off so the frames compare directly): `vk_flashbang_cover_43.txt`,
`_1610.txt`, `_169.txt` (fullscreen at an internal 3840x2160) and `_219.txt` in the Vulkan window, and
`regress_flashbang.txt`, `regress_flashbang_wide.txt` (16:9) and `regress_flashbang_ultrawide.txt` (21:9) headless.
The window shots and snaps are the game's frame, so the present's letterbox is not part of them.

## Water

de_aztec's river (FINDINGS.md "Water"), its alpha from a cube texture (a Fresnel term), from three fixed views over it:
`setview X Y Z YAW PITCH` holds the player at an origin with view angles (the server's copy, every 20 ms, no velocity;
`setview off` lets go), since noclip does not work and no spawn sees the river. `regress_water.txt`, `_wide.txt` (16:9)
and `_ultrawide.txt` (21:9) headless: the reference renderer's alpha for the water at one pixel in each view (`expect
probealpha NAME X Y MIN MAX`: the first SRCALPHA draw at that back buffer pixel in each of the snap's frames, from the
renderer's `PROBES` lines; the run sets `--env PROBES=X,Y --env PROBE_WIDTH=W`) against the cube's formula, and the
mean chroma of a box of water (`expect chroma NAME X0 Y0 X1 Y1 MIN MAX`: how much of what lies under it shows through).
`vk_water_43.txt`, `_169.txt` and `_219.txt`: the same views in the Vulkan window, the chroma within the reference's
bounds (before the cube texture was bound: below them), the cube texture's creation logged. Auto Exposure off in all.

## Bloom

The title's bloom and PC Options' Bloom (FINDINGS.md "Bloom"), at de_aztec's white sky with the player held there
(`setview`): `regress_bloom.txt` (headless, the reference renderer) starts with Bloom Off (`--pc bloom=0`) and expects
the map's light config's 0.5 taken back to 0, On giving 0.5 back and the glow across the frame's upper right quarter
(`expect regiondiff`; before SDK patch 0034 the composite was clipped to the top left corner), and a console intensity
(0.3) kept through Off and On. `vk_bloom_43.txt` (windowed, the Bloom row turned off with the mouse),
`vk_bloom_169.txt` (full screen at an internal 3840x2160) and `vk_bloom_219.txt` (144 frames per second, no VSync) do
the same in the Vulkan window, expect the back buffer's downsample sampled with GAUSSIANCUBIC (logged once), and part
way through a flashbang's fade with Bloom On no seam at the old corner (`expect seam NAME X Y MAX`: the box from the
top left to X, Y no more than MAX brighter across its right and bottom edges; it was 10 to 32 levels). All four start
with Auto Exposure Off; the window tests also expect the map's `cl_dynamicgamma 1` taken back and no adjustment.

## Auto Exposure at the start

Each map's light config sets `cl_dynamicgamma 1` (FINDINGS.md "Auto Exposure at the start"). `regress_ae_startup.txt`
(headless, de_aztec, started with Auto Exposure and Bloom Off) expects the map's values taken back as the config runs,
no adjustment (`CSX_LOG_DYNGAMMA=1` logs one a second while it runs: `expect quiet`), the frame at a held view equal to
the one after Auto Exposure is chosen Off again, and the four combinations of the two settings changed while playing,
each saved. `vk_ae_maps.txt` (the Vulkan window, Off saved) goes from the front end to de_airstrip and through Change
Map to de_aztec, each a relaunch of the title: in each instance Off is in the cvar before the first post processed
frame (`expect before A B`: this instance's first line matching A comes before its first matching B; B is the bloom's
GAUSSIANCUBIC line, logged once per instance) and no adjustment runs; On and Off then work while playing.

## The weapon over the HUD

`vk_viewmodel_hud_43.txt`, `_169.txt` and `_219.txt` (the Vulkan window at FOV 120, de_aztec, the player held; FINDINGS.md
"Weapon over the HUD"): the Glock and the knife in hand (`expect weapon`), a K&M SMG bought through the buy menu, at
rest and after a burst, its reload and a switch part way, then a CV-47. The HUD is depth tested at 0 after the view;
the weapon's parts close to the eye must not hide it. `expect bluefrac NAME X0 Y0 X1 Y1 MIN MAX` (the share of a box's
pixels whose blue exceeds red by 12) measures the HUD's blue-grey numbers over the dark weapon in the bottom right
box: as in the reference renderer (0.149, 0.090, 0.044 at 4:3, 16:9, 21:9), about half before the fix.

## Xbox Graphical Effects

The master over Bloom and Auto Exposure (FINDINGS.md "Xbox Graphical Effects"). `expect effects BLOOM GAMMA` waits up
to 5 s for the last "pc effects" line (CSX_LOG_EFFECTS=1: cl_hdrbloom and cl_dynamicgamma logged each time either
changes) to read those values. `regress_xbox_effects.txt` (headless, the master saved Off): the eight combinations of
the three settings while playing, the saved values untouched, every master Off frame equal to the master On frame
with both effects Off. `vk_xbox_effects_43.txt` and `_219.txt` (FOV 120, the master saved Off): both off from the first
frame, the HUD over the weapon (`expect bluefrac`, vk_viewmodel_hud's bounds) and the flashbang's white-out
(`expect coverage`) unchanged by the master, On and Off again while playing. `vk_xbox_effects_169.txt` (from the front
end): the relaunch into de_airstrip, the pause menu's row (the mouse), console values, Change Map with the master Off.

## Player collision

Custom Game > Player Collision (FINDINGS.md "Player collision"). The probe puts bodies where a test wants them:
`place N X Y Z` (client N: 0 the player, then the bots) and `players NAME` (every client's origin and health), checked
with `expect apart`, `bodymoved`, `bodyz`, `health`, `healthdrop` and `spawnapart`. `regress_player_collision_on.txt`
(tests/launch/de_aztec_2bots, no setting: the title's own) and `_off.txt` (de_aztec_2bots_nocollide, "+pc_player_collision
0"): two bots held with bot_stop 1; the player walks at a bot (On: stops 32 units from it; Off: walks through, as far
as on open floor), is dropped on its head (On: stands on it; Off: falls through), a bot is dropped on the other's head
(the same), a shot and an HE grenade hurt the bot either way, a wall stops the player either way, a round restart
spawns everyone apart and keeps the setting, and typing the command during the match is refused.
`vk_player_collision_43.txt`, `_169.txt` and `_219.txt` (the window, FOV 120): the Custom Options' tenth row, Off by
the mouse (43, 219) or the keys (169), the match started with it; 169 also keeps it through Change Map, Quit and the
next front end, then Reset gives On.

## Bot count

Custom Game > Number of Bots, 0 to 16 (FINDINGS.md "Bot count": 17 player slots). Fixtures tests/launch/de_aztec_bots7,
_bots11, _bots16, _bots24 and de_dust2_bots16, _bots16_nocollide (1 minute rounds, 3 s of freeze time).
`regress_bots_aztec_7.txt`, `_11`, `_16`, `regress_bots_dust2_16.txt` (8 spawn points a team: since G8.2B all 16
join, two players share a point), `regress_bots_dust2_16_nocollide.txt` and `regress_bots_aztec_24.txt` (since G8.2B
all 24 join; before it the 17 slots took 16): the players
counted (`expect playercount`), spawned apart (not with collision Off, where bodies may stand in each other), the bots
walking (`expect bodiesmoved A B MIN N`), kills and two round ends in the game's log, the teams within one
(`expect teams MAXDIFF MIN`, from the log's "joined team" lines), the spawn points and edicts logged (`spawns NAME`).
`vk_bot_count_43.txt`, `_169.txt` and `_219.txt` (the window, FOV 120): the Match Options list to 64 bots (0 to 16,
then 20 to 64 in fours), a match on de_aztec with 65 players, the play and scoreboard shots, Bot Team on one team not
capped (since G8.2B), the choice kept through Quit and the next front end, whose match puts the 64 bots on one team. CSX_LOG_SERVERTIME=1 logs the server frame's time once a second (benchmarks).

65 player slots (G8.2B, FINDINGS.md "Player slots, 65"): `regress_bots_aztec_64.txt` and `_64_nocollide.txt` (64 bots
and the player on de_aztec, collision On and Off), `regress_bots_dust2_64.txt` (64 bots on 8 / 8 spawn points): 65
players, the teams 32 and 33, the bots walking, kills, round ends, no guest trap and no guest table overrun (the bots
join one every 0.3 s or so: 25 s are given). `regress_bots_aztec_64_oneteam.txt` (64 bots on the Counter-Terrorists, the
player a Terrorist: Bot Team on one team, mp_limitteams 0 no longer stopping at 21; the XACT event queue race found
by it, SDK patch 0038). `vk_perf_bots16.txt` / `_bots64.txt` (the window, VSync Off, collision On): measurements only.

`regress_xact_dispatch_level.txt` (64 bots on one team, the most sound in one place; FINDINGS.md "High population
performance"): every change of XACT's event heap holds the DISPATCH_LEVEL lock (src/game/xact_check.cpp), no two
threads are inside it at once, its count stays within its capacity, no thread waits while holding the lock, and the
threads did meet at the lock (CSX_LOG_LOCKS=1). It fails with the lock off (CSX_DISPATCH_LOCK=0: "changed without the
DISPATCH_LEVEL lock" at once), its own check that it sees the race's cause.

## Benchmarks

`vk_perf_aztec_16.txt`, `_32`, `_64` and `vk_perf_dust2_16.txt`, `_32`, `_64` (the window at 1920x1080, VSync Off,
limit 240, Player Collision Off: fixtures tests/launch/de_aztec_bots16_nocollide ... de_dust2_bots64_nocollide): the
player joins and is held at one view for 60 s; nothing asserted. `python3 -I tools/perf_summary.py build/wruns/vk_perf_*`
summarises each run: mean frames/s, 1% and 0.1% lows and interval percentiles from every frame (CSX_LOG_INTERVALS),
the renderer's GPU and CPU recording time, the server frame (CSX_LOG_SERVERTIME), the DISPATCH_LEVEL lock
(CSX_LOG_LOCKS), memory, each thread's CPU use (CSX_PROFILE thread times). `tools/run_window_tests.py --exe PATH --tag
TAG` runs another build into build/wruns/<scenario>_<TAG> (comparisons, repeats). A sampling profile: the same
scenario with CSX_PROFILE=$RUN/profile.txt and no CSX_PROFILE_RATE=0, then `tools/profile_report.py EXE PROFILE RUN.OUT
--from setview [--tree FUNC | --callers FUNC]`. CSX_LOG_TRACES=1 counts the engine's TraceLine by caller.

## Scoreboard

The scoreboard for 65 players (FINDINGS.md "Scoreboard"). `python3 -I tests/scoreboard_rows_test.py` (seconds, no game
run): the row window on its own (empty, 1, 16, 17, 32, 33, 65 players, one team of 64 or 65, spectators, blank rows,
scrolling to both ends, clamping when players leave or change teams, the header kept in sight, the "more below" count,
every player reachable) and the Ping text (BOT, "-", measured) and the userinfo's *bot. `vk_scoreboard_169.txt`,
`_43.txt`, `_219.txt` (the window, 64 bots, collision Off, CSX_LOG_SCOREBOARD=1): the Ping column and 25 lines, the
first page (players 1-23, "42 more"), the wheel (3 rows a notch), Page Down, End (the last player), Home, a 31-character
name and a bot's name for the player ("(1)Walt"), the player's team change at the start, every bot BOT and the player
never; shots of every page. Then `python3 -I tools/check_scoreboard.py build/wruns/vk_scoreboard_*`: in every logged
snapshot each section in the game's order (score down, deaths up), the headers' counts, BOT for bots only, one row per
player, the window's numbers, and every player on screen at some point.

## Recompiler instruction patches (about 10 s, no game run)

`python3 -I tests/recomp_patches.py` recompiles assets/default.xbe with test configs that include config/game.toml:
five `[[patch]]` entries that apply (an imm8 compare given a value past its encoding, an imm beside a disp, an
absolute disp, an 8-bit operation's top value, an old value written as -1) change exactly those instructions in the
generated code, are marked there and are listed in patches.txt; the same input gives the same output twice; without
patches the code is what it was before the feature. Sixteen refusals, each alone (wrong bytes, text or old value, a
new value too wide, new equal to old, a branch, no or another operand, inside another instruction, not code, no or
another image hash, two patches for one operand, missing fields, an unknown operand kind) and three together, each
reported with its reason and nothing generated.

## Player slot audit (about 35 s, no game run)

`python3 -I tests/slot_audit_test.py` runs tools/slot_audit.py on a synthetic listing that holds one of each finding
(a table's direct, end-pointer and alias references, strides beside a table reference and alone, object fields
through a pointer, the frame ring's absolute player states and frame fields, a player state reached through a
frame-derived register until that register is overwritten, a pointer walk, a classified site, a missing site, an
unreviewed 17, a block copy recognised by its pattern), then `--check` (an unexpected and a vanished reference fail,
the same listing passes) and `--require-patched` (fails until every direct reference is patched). On the real
program: the committed analysis/player_slots_inventory.json matches a fresh audit, two audits are identical, every
relocated table's references are patched (`--require-patched`), and config/player_slots_patches.toml is what
tools/slot_patches.py makes of its configs.

## Guest tables (about 20 s with the build, no game run, Linux)

`python3 -I tests/guest_tables_test.py`: the production header src/common/guest_tables_layout.h matches
config/guest_tables.toml; tools/guest_tables.py lays out a test config (a 1040 x 0xFB table with align 1, 65 x 0x24C,
65 x 2, 3 x 16 with align 16) with every table aligned, ending at a page and a guard page before it, the same header
twice, `--check` failing on an edited header, and five refusals with their reasons. Then it builds
tests/guest_tables_test.cpp against the runtime's guest memory (sdk/runtime/kernel/memory.cpp): eight bad layouts
and an overlap refused, a region already in use refused, the layout mapped (aligned, zeroed, writable to the last
byte, guard pages uncommitted, the region owned by the guest allocator), Element() for the last index and refusing
the next and a huge one, and faults (in child processes) one byte and one element past a table's end and before its
first page, each reported with the table, while an access inside a table and a fault elsewhere are not.

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

## Buy menu taps

The buy menu opens only for a tap of +buymenu (0.05 to 0.5 s), and the title's answer to it can be lost to its own
half-second settings command (FINDINGS.md "Buy menu taps"). Every buy step uses `pckey tap +buymenu SECONDS`:
pc_keys.cpp presses it just after the HUD's settings check and releases it SECONDS later on the host's steady clock,
logs "pc keys: tap +buymenu held N s", and drive.py fails a hold outside SECONDS to SECONDS + 0.1.
`regress_buy_tap.txt` (headless) and `vk_buy_tap.txt` (Vulkan window, VSync off) tap 0.08 and 0.35 s (the menu opens)
and 0.7 s (it does not) at 30, 60, 120 and 240 frames per second.

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

## Native resolution

`vk_resolution.txt` (Vulkan window, not in the headless suite: the renderer is the Vulkan backend's) starts at 16:9 and
2560x1440 and checks the render targets the backend logs ("colour target ... 854x960 drawn at 2560x1440", the glow's
427x240 at 1280x360) and the presented shot's size, then changes Resolution in the pause menu and sees the next
frame drawn at the new size. The window now reads the harness's `cmd` file, so `press` steps drive the pad in
window scenarios too.

## Mouse in the menus

The headless scenarios send the pointer in the menus' 640x480 (`pckey pointer X Y`), clicks (`pckey mclick 1|2|11`:
left, right, left double) and wheel notches (`pckey mwheel N`) to src/game/pc_mouse.cpp; it logs each screen with its
rows ("pc mouse: screen NAME, 7 rows from 160 128, 26 high, 340 wide"), which gives the coordinates to point at.
`regress_pc_menu_mouse.txt` (in game: team, buy pie, pause menu, PC Options) and `regress_pc_menu_mouse_fe.txt`
(front end: main menu, PC Options with the help bar, PC Controls with the wheel). `vk_menu_mouse.txt` drives the
Vulkan window with SDL mouse events: `point X Y` moves the pointer to a menu point through the window's own mapping,
`click 1|3|11` clicks there.

## Window tests in a headless gamescope

`python3 -I tools/run_window_tests.py [SCENARIO ...]` runs window scenarios in the Vulkan window build, each in its own
`gamescope --backend headless` (nothing on the desktop; only the scenario's input reaches the title; no audio device),
with the size and options its header gives, in build/wruns/<scenario>; the default list is the mouse, binding and
console scenarios. `vk_menu_pointer_*.txt` move the pointer to raw window pixels (`pointat WX WY`) and expect the menu
point computed by hand for that window and aspect. `vk_menu_mouse_fe.txt` captures the audio (CSX_AUDIO_WAV) with
every menu sound logged (CSX_LOG_MENU_SOUNDS=1) and checks the hover sound in it (`expect menuaudio`,
tools/check_menu_audio.py). `vk_bind_capture.txt` drives PC Controls' capture with SDL keys, buttons and the wheel;
`key Å#[` sends the key a Swedish layout has at the [ position.

## Press-a-key rebinding and the profile name (headless)

`regress_pc_bind_capture.txt` uses `pckey bindstart` (Enter on a row), `pckey bindkey KEYNUM` (the engine key the
window would send) and `pckey bindcancel` (Escape), then relaunches into a match to check the saved keys.
`regress_pc_profile_name.txt` uses `pckey name TEXT` (typed text) and `pckey namekey backspace|left|right|enter|escape`.
`expect count REGEX N` checks an exact count (that a press did nothing).
