# Settings, fullscreen, Deathmatch, Gun Game and flashlight pass (2026-10-31)

Branch g8.2b (not merged). Linux. The user's seven requests after the Gun Game and Deathmatch play-tests. Each part:
what was found, what changed, and whether it was seen working in a running game. Nothing here was run in the game
yet: everything below is implemented, compiled and unit-tested, and waits for the play-test.

## 1. No PC Options

Every category is a row of the title's own Options, front end and pause menu (docs/menus.md):

| Front end Options | Pause menu Options |
|---|---|
| Video | Video |
| Sound Options (the title's rows, then the PC's audio rows) | Sound Options (the PC's audio rows; the title has none there) |
| Interface | Interface |
| Mouse | Mouse |
| Controller (the title's Control Options, then the PC's controller rows) | Controller |
| Keybinds | Keybinds |
| Multiplayer | Multiplayer |
| Mods | Mods |
| View Credits | |

- The PC Options hub and its code (`opens`, HubFrontEnd, HubInGame, HubChild) are gone.
- Online Options (Xbox Live status and Xbox voice) is left out: there is no service and no voice chat on a PC.
- Every pc_settings.ini key is unchanged, so saved settings and profiles carry over.
- No setting was removed or duplicated. The pause menu's Sound Options is a second screen over the same keys.
- The front end's rows sit 28 units apart (36 before), so nine fit above the help lines without a scrollbar.
- Code: src/game/pc_menu.cpp, src/game/pc_options_text.h. Test: tests/pc_options_text_test.py.

## 2. Net graph

- **Size.** New setting Interface > Net Graph Size: Small (the default), Medium, Original.
  - The engine draws a column per unit, net_graphwidth (300) wide, each net_scale (5) high.
  - Small is 150 columns at half the height; Medium is three quarters.
  - It is set with net_graph while the PC setting shows the graph. Net Graph Off leaves the console's own net_graph,
    net_graphwidth and net_scale alone.
  - There was no saved size preference before, so the new default reaches everyone. Original is the engine's size.
  - The numbers keep the console's font (8 units a character), so the text is not halved.
- **Place.** Bottom right corner of the player's own view: 10 units from its right edge and above the HUD's bottom row
  (money, health, ammunition). The graph's text starts at its left edge: its longest line is about 24 characters at
  Detail 1 and 46 at 2 and 3 (the server's sv line). The wider of graph and text is kept inside the view.
- **Aspect ratios and split screen.** The view rectangle is the player's own (each split-screen player's process
  draws its own viewport) and widescreen's, so 4:3, 16:9, 16:10 and ultrawide, and every split, follow it.
- **HUD editor.** It does not move the net graph (it never did), so there is no customisation to keep.
- Code: src/game/pc_hud.cpp, the setting in src/game/pc_menu.cpp.

## 3. Fullscreen lost when a match starts or ends

- **Cause (from the play-test's log).** A match's start and its end are the title's relaunch: on Linux the image is
  exec'd again (kernel::Relaunch). The window's display connection closed with the old image, so the compositor
  showed the desktop until the new image had a window.
  - That took 0.25 to 0.3 s from the exec, and the new window first came up tiled (595x326 in the log), going full
    screen about 0.1 s later.
  - On Hyprland that is the workspace without its full-screen window, then a tiled window: the "minimising".
  - Map changes, a server's disconnect and the party's returns are the same relaunch. The pause menu's Return to
    Game has none, so it was never affected.
- **Fix: a handover, as the Windows build already had.**
  - SDK patch 0045: at the relaunch a child is forked that keeps only the window's display connection (all other
    descriptors closed) and runs nothing else. The connection stays open, so the old window stays on screen with its
    last frame.
  - The window front end names that connection: SDL's Wayland display (wl_display_get_fd) or X11's
    (XConnectionNumber), in RELAUNCH_HOLD_FD.
  - The new image ends the child once its own window has shown two frames as Display Mode wants them (full screen, or
    a window), or after 3 s; the child also ends itself after 15 s.
  - One window is up at a time from the user's point of view. Nothing raises, focuses or recreates a window, and
    there is no loop, so Alt-Tab and the user's own focus changes are untouched.
  - The mouse capture follows the menus and gameplay as before (the new image's own).
- **Split screen** is unchanged: its compositor owns the window and never relaunches.
- Code: patches/sdk/0045-relaunch-holds-the-window.patch, src/linux/window_main.cpp (OpenHandover, FinishHandover,
  NameHeldConnection, EndHolder).
- **Limits.** If the compositor drops a client that stops reading (a full socket buffer, mainly from fast mouse
  motion), the old window closes early: that is the old behaviour, not worse. Hyprland's "not responding" check needs
  seconds of silence; a handover takes well under one.

## 4. and 5. Gun Game: no dropped weapons, no pickups

docs/n6_modes.md "Nothing dropped, nothing picked up".

- **Drops are avoided outright.** A death takes the game's own "drop nothing" way (no weapon box is made), and the
  drop key drops nothing. No weapons gather, so no lifetime has to be tied to a corpse.
- **Pickups refused on the server.** Weapon boxes, armouries and weapons lying in the world are refused. The mode's
  own weapons still come through: the game's GiveNamedItem marks them SF_NORESPAWN. This covers bots, split-screen
  players and remote clients, since these are the server's functions.
- **Kill validation** is unchanged, by the server's named weapon.
- **Other modes** keep their drops and pickups.

## 6. Deathmatch: grenades and free armour

docs/n6_modes.md "Grenades in the loadout" and "Free armour and helmet".

- The grenades held after the last grenade purchase are given at each respawn, held to the game's limits, after the
  strip.
- Every spawn gets full armour and a helmet, free, through the game's own item_assaultsuit (the HUD's armour type
  message included). Bots get it too.
- The buy menu's armour entries are refused by the game itself while armour and helmet are full.

## 7. Flashlight on the world

FINDINGS.md "Flashlight: objects lit, the world not".

- **Cause.** The Xbox BSP renderer replays the world, so no world lightmap was rebuilt for a dynamic light. Objects
  are lit by the model renderer from the light directly.
- **Fix.** The lightmaps of the world surfaces each light marks are rebuilt each frame as the engine's
  R_RenderDynamicLightmaps does, once more after the light leaves, and are compressed into their textures.
- **Unchanged.** The light's direction, radius and falloff stay the client's. The toggle and its key, and the
  multiplayer synchronisation (EF_DIMLIGHT from the server), are as before.
- **Unverified in a running game.**

## Tests run (no game started)

- Builds: the headless harness (with the regenerated game code), the window front end, csx-server.
- Unit tests: pc_modes_rules (26), pc_options_text (9), profiles (16), stuff_filter (14), server_config (38).
- Tool suites: recomp_patches, slot_audit_test, guest_tables_test.
- The window scenarios that went through PC Options were rewritten for the new Options rows, but not run.

## Manual checklist

1. **Options (main menu).**
   - The rows are Video, Sound Options, Interface, Mouse, Controller, Keybinds, Multiplayer, Mods and View Credits;
     there is no PC Options and no Online Options.
   - Each row opens its screen, and Back returns to Options.
   - The rows are readable and not overlapping the help line. Mouse and keys select them.
2. **Options (pause menu).** Video, Sound Options (Master Volume, Output Device, Audio in the Background), Interface,
   Mouse, Controller, Keybinds, Multiplayer and Mods.
3. **Saved settings.** A value set before this build (sensitivity, FOV, volume) is shown as it was.
4. **Net graph.**
   - Interface > Net Graph Always On: small, in the bottom right corner above the money/health/ammo.
   - Net Graph Size Original is the old size.
   - Detail 2 is still inside the screen.
   - Try a 4:3 and an ultrawide aspect, and a split-screen match (each player's own corner).
5. **Fullscreen.**
   - Display Mode Fullscreen: start a Single Player match, Custom Match, Deathmatch and Gun Game, then quit each to
     the main menu. The screen should not drop to the desktop or a tiled window.
   - Join a server from the browser and disconnect.
   - A map change in a match.
   - Windowed mode: the same, the window keeps its size.
   - Alt-Tab away during a loading: the game should not take the focus back.
6. **Gun Game.**
   - Kill a bot: nothing lies where it died.
   - The drop key does nothing. Walking over weapons (a map's, or a box from earlier) picks nothing up.
   - Levels still go up with the level's weapon, and the knife level wins.
7. **Deathmatch.**
   - Buy an AK-47, a Deagle, an HE grenade and two flashbangs; die. You respawn with all of them and full armour with
     a helmet (HUD armour 100, helmet icon), and the money is not reduced.
   - Throw a flashbang and die: two again, never three.
   - Buy a smoke after a respawn and die: the smoke is now in the loadout.
   - Bots spawn with armour.
8. **Flashlight.** Turn it on facing a wall, a floor and a ceiling. The lit circle should be on the surfaces and
   follow the view, and the screen should not brighten. Turn it off: no light should be left on the wall. Objects
   should still be lit.
