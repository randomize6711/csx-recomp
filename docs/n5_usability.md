# N5: menu usability, input, controller prompts, settings (as built)

The plan and its decisions: docs/n5_plan.md (approved 2026-10-30). Branch g8.2b, not merged. Each checkpoint below is
what was built, how it was checked, and what is left.

## N4.8.1: Leave Match (before N5)

docs/splitscreen.md section 19 (e624919).

## N5A: Escape, Exit Game, PC Options' screens, smooth scrolling

### Escape is Back

src/linux/window_input.cpp. In every menu screen Escape is the pad's B: back, cancel, No in a confirmation (it was
START outside the server browser's and the party's screens); Enter stays START. Playing, Escape is still START (the
pause menu). The press that opened a menu is not taken as B until it is let go (held from play into the pause menu, it
would have closed it again at once). Typing (a name, a password, the developer console) and press-a-key rebinding keep
their own Escape (stop typing, close the console, cancel the wait), as before. The controller is unchanged.

The title's screens have no B or START handlers of their own: the engine's default B goes back a screen, and on the
pause menu's root it returns to play (checked: vk_pause_escape).

### Exit Game

src/game/pc_menu.cpp ExtendExit. A seventh main menu row, Exit Game, after Change Profile (the title's main menu art
has a seventh, empty row slot there), written as the title's rows are (help line "Close Counter-Strike."), and a
confirmation of the title's style (SuperLauncher_PCConfirmExit: A closes the game, B goes back). The quit is
csx::AppQuit, as N4.8's Quit Game: a split-screen compositor ends with player 1, a local server with its owner.

### PC Options' screens

src/game/pc_menu.cpp Screens, HubFrontEnd, HubInGame. PC Options (the row in the title's Options, front end and
pause menu) is a screen of the title's Options style opening five screens:

| Screen | Settings (pc_settings.ini keys, unchanged) |
|---|---|
| Video | Display Mode, Resolution, Aspect Ratio, Frame Rate Limit, VSync |
| Graphics | Field of View, Xbox Graphical Effects, Auto Exposure, Bloom |
| Mouse | Mouse Sensitivity, Invert Mouse, Raw Mouse Input |
| Audio | Master Volume |
| Game | Match Server, Smooth Menu Scrolling (new) |

Each screen loads and saves its own rows (Accept saves and goes back to PC Options; Back cancels; X resets that
screen's rows). The keys and their values are as before: old settings files load as they are. PC Controls stays its
own row in Options. The screens' names: SuperLauncher_PC<screen> / InGame_PC<screen>; the hub keeps
SuperLauncher_PCOptions / InGame_PCOptions.

Not done: section headings inside PC Controls (the title's tables have no heading rows; new actions are grouped by
their labels' prefixes instead, N5C).

### Smooth menu scrolling

src/game/pc_scroll.cpp, two hooks in the title's table draw (0xCD030). When a list's first shown row moves by one or
two rows, its rows are drawn from where they were and ease into place in about a tenth of a second, the row they
uncover drawn too, the rows clipped to the list's area with the title's own D3D scissor. Larger jumps (a list wrapping
around) are not eased. The mouse's row positions are the table's own (the easing is only drawn). PC Options > Game >
Smooth Menu Scrolling turns it off (the original's whole-row steps).

### Tests

New: vk_menu_escape (Escape back through the front end's Options > PC Options > Video, Exit Game's confirmation:
Escape is No, A quits), vk_pause_escape (Escape opens the pause menu and, held, keeps it open; Escape back through the
pause menu's Options > PC Options > Graphics to play), vk_menu_scroll (PC Controls stepped past its last shown row: a
frame burst taken as the key is pressed, `windowburst NAME N KEY`, new in tools/drive.py, shows the rows easing up,
the old first row clipped at the list's top and the new last row coming in below it; looked at by hand). The game
code is generated from config/game.toml's hooks by tools/iterate.sh: tools/build_window.sh alone does not take in a
new hook.

Changed: the main menu's row count (6 to 7) in ten scenarios; the PC Options steps of the headless regress_pc_* and
the window vk_* scenarios that change a PC setting (now the hub, then the setting's screen, then back twice). Escape is
Back in the buy menu as in every menu: four scenarios that pressed Escape for the pause menu while the spawn's buy menu
was open now close it with Escape first: vk_loading, vk_local_server and vk_party_match where it opens at a
known step; vk_split_teams where a round's start may or may not have opened it, with a new drive step
(`closebuymenu [PREFIX]`: Escape only if the player's last menu state is a buy menu). vk_resolution's
four DOWNs had reached Aspect Ratio, not Resolution (the aspect change logs a resolution too, so it passed); it now
steps Resolution itself, one to the left (2560x1440 is a custom one, listed after the display's modes, Original to its
right).

Results (2026-10-30, g8.2b): the 9 headless scenarios that open PC Options (regress_bloom, regress_dynamic_gamma,
regress_pc_fov, regress_pc_menu_mouse, regress_pc_menu_mouse_fe, regress_pc_options, regress_pc_options_ingame,
regress_pc_frame_rate, regress_xbox_effects) pass on the final N5A build. Of the 46 window scenarios that press Escape,
open PC Options or count the main menu's rows, 41 passed on the build before smooth scrolling and 5 failed on their
own steps (above, changed); on the final build (smooth scrolling in) those 5 and 9 more of the menus' (vk_menu_escape,
vk_pause_escape, vk_menu_scroll, vk_menu_mouse, vk_menu_mouse_fe, vk_bind_capture, vk_browser_43,
vk_menu_pointer_pause, vk_bot_count_43) pass. The other 32 window scenarios ran on the build before smooth scrolling
only: N5G's complete regression runs them all on the last build. desk_hidpi (a desktop session's, not the headless
runner's) had its row count changed and was not run.

Fix after N5A (found reading the code for N5B): a profile's Profile Settings (Profiles > Y) took its PC rows (Mouse
Sensitivity, Invert Mouse, Raw Mouse Input, Field of View) from the first PC screen, which N5A made Video: the four rows
were gone (6 rows, the controller's). They are now found by key in any PC screen (10 rows again). New test
vk_profile_settings (passes).
