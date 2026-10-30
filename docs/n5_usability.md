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

## N5B: Details, the collision explanation, typed numbers

### Details (Y)

src/game/pc_menu.cpp (ExtraOf, DetailsScreen, OpenDetails, ShowDetails, DetailsFrame). Every PC settings screen
(PC Options' five, PC Controls, Server Filters, Split Screen Options, Profile Settings; front end and pause menu) takes
Y on a row: SuperLauncher_PCDetails / InGame_PCDetails, a screen of the menu's own style (the front end's message
screens with the Back legend; the pause menu's overlay) with the row's name and its description, wrapped. Custom
Rules keeps Y for Rulesets (N4.8): Details is BACK there (Tab on the keyboard). A or B (Escape) goes back; the screen
comes back as it was (its unsaved choices and the highlighted row: they are kept when Y is pressed and put back once
the screen shows again, since its entry loads the saved values). A hint under each list says so ("Y Details",
"BACK Details"), and on a number's row the range that can be typed.

The descriptions are in one table by setting key (the profile's and the rules' copied rows have them too): what the
setting does, the original's value, what changes with it, from the code and FINDINGS.md (e.g. Bloom: "on most maps it
adds little: mainly a bright sky glows", G5's measurement; Frame Rate: movement keeps its speed at every limit,
pc_frame.cpp's carried milliseconds). No cost figure is given where none was measured. A row without its own
description shows its help lines. Every PC row has a help line (checked: none empty).

Not done: the hint and the Details legend name the pad's buttons (Y, BACK, B) also for the keyboard (Q, Tab,
Escape), as the title's own legends do; N5D's prompt styles change them.

### Player collision explained

Custom Rules' Player Collision and Custom Game's Custom Options row: "On: players and bots block each other. Off: their
bodies pass through; shots and grenades still hit." Its Details (Custom Rules): what Off changes (bodies pass through,
nobody stands on another player), what it does not (shots, the knife, grenades and their blasts, hitboxes and damage;
walls, doors and hostages still block), when it applies (set for the whole match from its start), where (this PC's
server: Custom Game, Local Only, LAN; sent to the players who join, as N4's signon does; another server keeps its own).
From FINDINGS.md "Player collision" and its tests.

### Typed numbers

src/game/pc_menu.cpp (Typing, SetCustom, SetCell), src/linux/window_input.cpp (NumberChar). On a number's row a digit
or a point (the digit row by position, as bindings take it, so every layout; the keypad; point or comma) starts typing
its value in the row (shown as the list's text with a cursor, "50_"); Backspace, Enter (kept), Escape (cancelled); the
keyboard goes nowhere else meanwhile (no menu move, no Accept). A value outside the row's range, or not a number, is
refused: the row shows "Use 0.1 to 20" for two seconds. A value equal to one of the list's is that choice; another is
the row's own value: one more choice at the list's end ("2.75 (custom)"), passed over by the arrows until a value is
typed (the title's enable byte for the choice), its label written into the game's own string. Accept saves it as any
choice (pc_settings.ini, the profile, the party's rules); a saved value that is no list choice is shown so when the
screen opens. The arrows and the pad work as before (the slider-like lists are unchanged); typing is the keyboard's.

| Setting | Range | | Setting | Range |
|---|---|---|---|---|
| Mouse Sensitivity | 0.1 to 20 | | Buy Time | 0.25 to 9 min |
| Field of View | 60 to 130 (whole) | | Starting Money | 800 to 16000 |
| Frame Rate Limit | 30 to 1000 (whole) | | Rounds, Win Limit, Time Limit | 0 to 999 |
| Master Volume | 0 to 100 (whole) | | Team Balance | 0 to 32 |
| Bots | 0 to 61 | | C4 Timer | 10 to 90 s |
| Round Time | 1 to 9 min | | Freeze Time | 0 to 60 s |

The ranges are those the code that applies them accepts (Field of View: pc_view.cpp's 60 to 130; the rules: the
dedicated server's own, src/server/server_config.cpp; C4 Timer: CS's own limits). The lobby takes a saved rule that is
a number of its range as well as a list value (pc_lobby.cpp Rules()). Mouse Sensitivity's typed value is applied as
any value: the window reads it as a number.

Fixed on the way: the shown choice's text swap (press-a-key's "Press a key") gave the string its own length as
capacity; MSVC's strings keep fewer than 8 characters in place, so a short text ("50_") was read from the wrong
place. The capacity is now the block's (63).

### Tests

New: vk_menu_numbers (pause menu, PC Options > Mouse: Invert Mouse changed, Y on Mouse Sensitivity opens Details,
Escape comes back with Invert Mouse still changed; 50 refused; 2.75 typed and Accepted: pc_settings.ini
mouse_sensitivity=2.75 and mouse_invert=1; in play 1000 counts turn the view -60.500 degrees, counts x 0.022 x 2.75;
the screen opened again shows 2.75 as its own value; Left steps to 10; Escape leaves it unsaved). vk_party_numbers
(Custom Rules: Starting Money typed 1234 and Buy Time 0.75, BACK on Player Collision opens its Details and comes back
with both kept; Accept, Start: the local server's match line has +mp_buytime 0.75 +mp_startmoney 1234).

### The explanation panel, the categories, map search (added to N5B after the user's mandate of 2026-10-30)

- **Explanation panel** (pc_menu.cpp PcScreen::panel, TableLayout, PanelLines, HintFrame): Video and Advanced
  Graphics (the graphics options) show their list narrower at the right and, at its left, the highlighted row's name
  and description (its Details text in narrower lines), changing as the highlight moves; front end and pause menu.
- **Categories**: PC Options' screens are Video, Advanced Graphics (the former Graphics), Mouse, Keyboard (PC
  Controls), Controller (N5D), Audio, HUD and Menus (Smooth Menu Scrolling; N5E adds its displays), Multiplayer (Match
  Server, moved from Game, and the network rates: Updates per Second / net_update_rate, Commands per Second /
  net_cmd_rate, Bandwidth / net_rate, applied when joining a server, typed 10..100, 10..100, 10000..1000000). Gameplay
  comes with N7's settings. The keys are unchanged.
- **Map search** (src/game/pc_mapsearch.cpp): Custom Game's map list and the party's. The keyboard's letters and
  digits make a search there (Backspace takes one back): the highlight goes to the first map whose name or file name
  holds it and the list scrolls to it; the help line says "Search: vos   Vostok" or that no map has it. A controller's
  Y opens the profile keyboard for the same search (Done finds it). On those two screens the letters are the search's,
  not the pad's W A S D / Q / R. Test: vk_map_search.

## N5C: buying, radio, grenades, scoped sensitivity

- **Ammunition** (PC Controls, appended rows): Buy Primary Ammo `,` (buyammo1), Buy Secondary Ammo `.` (buyammo2): the
  game's own commands.
- **Autobuy and Rebuy** (src/game/pc_buy.cpp; F3, F4): the Xbox buy menu buys with "impulse <item id>" in the next move
  (its purchase, 0xD8060: the item looked up by name, 0xDEC60; the client's check, 0xCC260: money, team, limits), not
  with a string command; the server decides as for the menu (money, buy zone, buy time, team). Autobuy and Rebuy do
  those same three steps without the menu open, one purchase a move (150 ms apart), each watched for the HUD's money
  (0x552C74) to drop. Autobuy goes down pc_settings.ini "autobuy" (default: M4A1, AK-47, FAMAS, Galil, P90, MP5,
  Kevlar and Helmet, Kevlar, defuser): what this player may not buy is passed over, the first primary weapon, pistol
  and armour that go through end their kind. Rebuy repeats what went through between the last two round starts (the
  client's RoundTime message, a hook at 0x1D4A90) and what the buy menu bought (a hook at 0xD8060). `buyitem ITEM` and
  CS 1.6's aliases (ak47, m4a1, vesthelm, hegren, flash, sgren, deagle, ...) buy one item the same way.
- **Grenades**: HE Grenade, Flashbang, Smoke Grenade (weapon_hegrenade / _flashbang / _smokegrenade), unbound.
- **Radio**: the three radio menus (radio1..3) and every standard radio line (Cover Me ... Enemy Down) as PC Controls
  rows, unbound (the Xbox defaults Z X C V stay). **Condition Zero's lines** (src/game/pc_radio.cpp): circle back,
  flank them, meet me, teammate down, I'm hit, need fire assistance, hit need assistance, bomb planted, bomb defused,
  hostage down: the sound bank has them (hdcounterstrike.xsb radio\circleback ...), no command played them.
  `csx_radio NAME` (the CZ rows) is a client string command over the authenticated connection; the server (this
  game's code: csx-server or a listen server) takes it in SV_ParseStringCommand's hook with its sender (host_client,
  never a name the client gives), checks the player as the game's radio commands do (next radio time, messages left
  this round) and plays it through the game's radio function (alive, to the teammates). A server says it serves them
  at signon ("pc_server_features radio2", on the client's stufftext list); a client sends csx_radio only to such a
  server or to the server in its own process, else logs that it is not served and sends nothing. The lines' texts
  are written from the sounds' names (the original's #-strings for them are not in this game).
- **Scoped Sensitivity** (PC Options > Mouse; mouse_zoom_sensitivity, profile-scoped): through a scope (the game's
  zoom: scr_fov under 59) the mouse's turn is multiplied by it; 1 (default) turns as before; typed 0.05..5.
- **Typed sensitivity**: N5B's typed numbers (Mouse Sensitivity 0.1..20, Scoped 0.05..5).

Not done / limits: the radio menus' look in this game (radio1's menu) is the game's own; checked in vk_buy_radio's
window shots only. A buy refused by the server (outside the buy zone or time) is logged as not bought.

## N5D: controller identification and button prompts

- **Identification** (src/linux/pad_style.cpp): each controller's style from SDL's type, USB vendor and product and its
  name (logged once: "input: controller ... prompts"): Xbox 360, Xbox One / Series, PlayStation 3 / 4, DualSense,
  Switch Pro / Joy-Con / GameCube, Steam (SDL's type, or Valve's vendor), the original Xbox controller through an
  adapter (Microsoft's 0202, 0285, 0287, 0288, 0289), Wii Remote (Nintendo 0306 / 0330, or "Wii" in the name); any
  other gamepad shows Xbox 360 buttons (SDL lays every gamepad out as one). A split-screen player's virtual
  controller carries the real one's style in the compositor's attach message.
- **Automatic / Manual** (PC Options > Controller > Button Prompts, prompt_style, profile-scoped): Automatic follows the
  device last used (a key or mouse button: the keyboard's; a controller's button or a stick pushed far: that
  controller's); a choice keeps one style.
- **Artwork** (tools/make_prompts.py): new glyphs for the nine styles in the original legends' manner (a round button
  with a darker rim and a light top, a bold letter or symbol; bars for shoulders, triggers, Back / Start), drawn by the
  script at build time (build/window/prompts/csx_prompts.xpr, 164 64x64 textures; PNGs beside it to look at); the
  keyboard's are key caps of the keys the menus take (Space, Esc, R, Q, Tab, Enter, Page Up / Down, the arrows). A
  button a controller lacks is drawn as its name on a plain bar (the text fallback). Krita AI Diffusion or other
  generated art is not used.
- **Reusable rendering** (src/game/pc_prompts.cpp): the artwork goes into the menus' own texture registry beside the
  launcher's (a hook after its load, 0xAF69F); the menus draw CSXP_<button> textures whose pixels are the chosen
  style's (copied, then marked for upload: no menu text changes when the style does). Legend strips (launcher.xpr's
  Cancel_Accept_Reset ... with Xbox buttons drawn in) are replaced, where their words are known (Accept, Back, Cancel,
  Continue, Select, Reset, Overwrite), by a glyph and the title's own text each, right to left as the strip was: 121
  legends in the front end; 14 others (Xbox Live's, system link's) keep the original's art. Hints that name a button
  use the style's word for it ("Triangle Details", "2 Details"). Test: vk_prompts (each style's legend looked at).
- **Steam Controller**: SDL's HIDAPI driver for it (Bluetooth) is turned on; Steam Input's virtual controller (Valve
  vendor, product 11FF) is preferred over a Valve controller read directly when both are there (one would act twice);
  Steam's own remapping stays Steam's. **Wii Remote / Nunchuk**: SDL's HIDAPI Wii driver is turned on (the Remote's
  buttons and the Nunchuk's stick as a gamepad); motion and the pointer: SDL gives the accelerometer as a sensor and no
  infrared pointer, so pointer aiming is not done; motion aiming is not done (no hardware here to tune or check it).
- **Not checked with hardware**: no physical controller was used; the identification is from SDL's documented types
  and the vendors' ids. The party screens' text prompts (N4.8) keep their own keyboard / pad wording.

## N5E: audio, scoreboard, the weapon's field of view, net_graph

- **Audio** (PC Options > Audio; src/linux/sdl_audio.cpp): Master Volume (as before); **Output Device** (System
  Default, or a playback device SDL lists at the start: pc_settings.ini audio_device, its name; a saved device not
  connected is kept and shown so; a change reopens the output on it at once, the mixer going on); **Audio in the
  Background** (On; Off silences the game while another window has the keyboard; split screen: the compositor's
  window). The parts (effects, voices, ambient, soundtrack) stay the title's own Options > Sound (saved with the
  title's profile, as on the Xbox): not duplicated.
- **Scoreboard** (PC Options > HUD and Menus > Scoreboard: Hold / Toggle; src/linux/window_input.cpp Poll): Toggle
  makes a press of BACK (Tab, the controller's Back) keep the scoreboard up until the next press, in play (the
  Scoreboard screen passes the controls through, so it is play too); the menus' BACK is unchanged.
- **Weapon Field of View** (PC Options > Advanced Graphics, last row; viewmodel_fov; src/game/pc_viewmodel.cpp):
  investigated first, as the plan asked. The title draws the first-person weapon (R_DrawViewModel 0x4D300, from
  R_RenderView at 0x4EFC2) with the world's projection (R_SetupGL's MYgluPerspective, 0x4DF80). Go: around that call
  the projection matrix is pushed, loaded with the world's perspective at the weapon's own vertical angle (its
  horizontal degrees at 4:3, as Field of View's) and popped after (the GL layer's matrix functions); the world's
  view is untouched. Checked in vk_viewmodel_netgraph's window shots: at 60 the pistol is drawn larger, at 110
  smaller, the world the same in all three (the log gives the vertical angles: 46.83 and 93.93 against the world's
  73.74). "Same as Field of View" (default) leaves the title's draw alone; typed 50..130.
- **net_graph** (PC Options > HUD and Menus > Net Graph: Off / Always On / With Scoreboard, net_graph_mode; Net Graph
  Detail: 1..3; src/game/pc_hud.cpp): investigated first: the engine's net_graph draws in a match (SCR_DrawNetGraph
  0x6E4B0: the frame rate, in / out bytes and packets, the server's update rate, loss and choke, the latency columns;
  levels 1..3 as in GoldSrc; window shots). On the local server the latency is near 0, so its columns are a flat line
  there: their look with real latency is a manual check. net_graphpos puts it over the HUD (1: the view's bottom
  right less 55 / 79 units); a hook after its placement (0x6E613) puts it in the view's bottom right corner above the
  HUD's bottom row. The setting sets net_graph only when what it wants changes (the console's net_graph still works
  with Off); cl_showfps is left as it is (its own counter in the top left, as before).
