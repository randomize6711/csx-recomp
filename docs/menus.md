# The PC menus' structure (2026-10-31, PC Options merged into Options the same day)

The PC settings and online menus after the consolidation pass. The code is in src/game/pc_menu.cpp (`Screens()`),
src/game/pc_scroll.cpp (scrollbar), src/game/pc_mouse.cpp (mouse) and src/game/pc_browser.cpp (online menu).

## Options (no PC Options since 2026-10-31)

Every setting category is a row of the title's own Options screen, front end and pause menu. There is no PC Options
hub any more. The code is `ExtendOptions` (src/game/pc_options_text.h, tested by tests/pc_options_text_test.py), and
the row order is `kOptionsOrder`.

| Row | Front end | Pause menu |
|---|---|---|
| Video | display, frame rate, field of view, effects | the same |
| Sound Options | the title's 7 rows, then the PC's audio rows | the PC's audio rows (the title has no Sound Options there) |
| Interface | gameplay effects, HUD, net graph, menus | the same |
| Mouse | sensitivity, scoped sensitivity, invert, raw input | the same |
| Controller | the title's Control Options (6 rows), then the PC's controller rows | the same |
| Keybinds | keys and mouse buttons | the same |
| Multiplayer | match server, network rates | the same |
| Mods | installed mods | the same |
| View Credits | the title's | (none) |

- **Left out.** Online Options (Xbox Live status, Xbox voice: no service, no voice chat on a PC).
- **Renamed.** The title's Control Options row and screen title are now "Controller".
- **The title's rows** keep their commands and help as written; the PC's rows carry their help lines.
- **Layout.** The front end's rows are 28 units apart (the title's 36), so the nine fit above the help lines, with no
  scrolling. The pause menu's eight fit as they are.
- **Back** from a category returns to Options.

| Before | After |
|---|---|
| Options > PC Options > Video | Options > Video |
| Options > PC Options > Mouse | Options > Mouse |
| Options > PC Options > Interface | Options > Interface |
| Options > PC Options > Multiplayer | Options > Multiplayer |
| Options > PC Options > Mods | Options > Mods |
| Options > Keybinds | Options > Keybinds |
| Options > Sound Options (front end only) | Options > Sound Options (both menus) |
| Options > Control Options | Options > Controller |

**Video**, in order:
1. Display Mode
2. Resolution
3. Aspect Ratio
4. VSync
5. Frame Rate Limit
6. Field of View
7. Weapon Field of View
8. Xbox Visual Effects
9. Auto Exposure
10. Bloom

The project has no refresh-rate, anti-aliasing, texture-filtering or shadow setting, so none is shown. The
explanation panel stays beside the list.

**Interface**, grouped in order:
- Gameplay effects: Flashbang Effect, Death Camera.
- HUD: HUD Scale, HUD Scale (Split Screen), Scoreboard, Net Graph, Net Graph Detail, Net Graph Size (Small, the
  default since 2026-10-31; Medium; Original). The HUD editor (F7 in a match) is unchanged.
- Menus: Smooth Menu Scrolling, Button Prompts (moved from Controller).

The match rules (Custom Match, the party's Rules) are not here.

**Keybinds** is PC Controls renamed. The screen ids, the `bind.*` keys and press-a-key rebinding are unchanged.

**Controller.** The title's Control Options (front end and pause menu, now named Controller) keeps its own six rows:
1. Inverted Look
2. Look Sensitivity
3. Fast Look
4. Left-Hand Weapons
5. Damage Vibration
6. Automatic Hints

Then come the PC's rows, Flashlight and Spray Logo (the pad's buttons for them). This is a PcScreen with `embed`:
- its rows go after the title's `embed_first` rows (`ExtendEmbedded`, applied last to the menu text);
- the title's Accept saves them first (`pc_options_save`), and entering loads them;
- the title's own commands then run as before, including the profile's controller capture (pc_party.cpp), which
  reads the first six rows.

There is no separate PC Controller page.

**Sound.** The title's Sound Options (front end, `SuperLauncher_Sound`) keeps its own seven rows, then come the PC's
audio rows: the same `embed` mechanism as Control Options (the PcScreen "Audio", `embed = "Sound"`, `embed_first = 7`). Accept saves the PC rows (`pc_options_save`) and then runs the title's own Accept
(`stuffUItoPlayerData`). The pc_settings.ini keys are unchanged, so saved audio values carry over. The pause menu has
no Sound Options screen in this title: there the same rows are a screen of their own (`InGame_PCSound`, a copy of the
PcScreen with `in_game_only`), so the audio settings are reached from both menus.

**Saved settings.** Every setting keeps its pc_settings.ini key (and its profile scope), so existing settings and
profiles carry over unchanged. Only where a row is shown changed.

## Scrollbar

One component for every PC settings list, including the Controller rows in Controller (the title's Control Options).
The Options screen itself needs none: its nine rows fit.

**Drawing.** At the end of the title's table draw (hook 0xCD1E2, pc_scroll.cpp `DrawBar`), a list that has more rows
than it shows gets a 5-unit track and a thumb, 4 units right of the list:
- colours: the front end's blue or the pause menu's gold, drawn with the engine's FillRGBA (blending is switched back
  on after it);
- thumb length: the share of rows shown; thumb place: the first shown row's;
- nothing is drawn when every row fits.

The title already limits the rows drawn to the table's area. Smooth scrolling clips them while they ease.

**Mouse.** The scrollbar moves only the rows shown, never the selection:
- A click on the track shows the rows at that height (the thumb centred on the pointer); nothing is selected or
  highlighted.
- Pressing on the bar captures the mouse: moving drags the view until the left button is let go, even off the bar.
  The release ends the drag and activates nothing.
- Hovering over the bar highlights nothing; hovering a row selects it as before.
- The wheel scrolls the view three rows a notch (the selection stays where it was, perhaps out of sight). A list with
  no bar keeps the wheel's earlier behaviour (Up/Down).
- The keys and the pad move the selection as before, and the list brings it back into view.
- How: the view is kept per table with the selection it was set at (`PcScrollView`, pc_scroll.cpp). The title's own
  scroll step (hook 0xCD15E) would bring the selection back into view each frame; while the selection is unchanged
  it is given the view's rows instead. A change of selection (keys, pad, a row clicked) drops the view.
- Row hit-tests use the table's own first shown row, so they stay right after scrolling.
- Arrow clicks on a row are not affected: the bar is outside the rows.

## Main menu (redesigned 2026-10-31, branch g8.2b-menu-redesign)

Backup of the menu before the redesign: tag `menu-backup-2026-10-31`, branch `menu-backup/g8.2b-before-redesign`, and
`~/Work/csx-recomp-menu-backup/` (its README says how to restore it).

Checkpoint of the redesign before solo and split screen were unified: tag `menu-checkpoint-redesign-b56134b`,
branch `menu-backup/redesign-b56134b`, `~/Work/csx-recomp-menu-backup/checkpoint-redesign-b56134b/`.

| Main menu | Opens |
|---|---|
| Instant Action | every local match, solo or split screen (src/game/pc_instant.cpp) |
| Multiplayer | Server Browser, Host Game, Direct-IP / Code |
| Options | the title's Options (Video, Sound Options, Interface, Mouse, Controller, Keybinds, Multiplayer, Credits) |
| Extras | Training, Change Profile (the title's own rows) and Mods |
| Exit Game | its confirmation |

Every hub is in the title's front end style (its main menu's background, header and logo, highlight, sounds, the
row's help lines at the bottom, the help bar's buttons). Back returns to the main menu. A single click activates a
hub's button (pc_browser.cpp no longer takes the hubs for server lists, which needed a double click).

### Instant Action

**Left: the match setup**, in this order. The rows work over the title's own match screens, which keep every
setting:
1. **Start Match:** the title's SinglePlayerStartSession, with Single Player's choice on Custom so Match Options
   count; the map is the map list's selection. The only row that starts a match.
2. **Select Map:** the title's map list, with its search. Choosing a map (one click, A or Enter) only selects it and
   returns here.
3. **Game Mode:** Match Options, on its Game Mode row.
4. **Bot Settings:** Match Options, on Number of Bots (Bot Skill Level and Bot Team below; Y the bots' weapons).
5. **Settings:** Custom Options (time and round limits, money, friendly fire, collision). Its Accept sets the rules
   to Custom.

Match Options' and Custom Options' Accept return here. Back from a match returns here too, on Start Match (the
title went back to Single Player's difficulty screen).

**Right: one near-opaque panel** (blue-grey border, light grey inside, dark text).
- LOCAL PLAYERS: `P1  -  Ready (profile)`, then for P2 to P4 either `Ready (profile or Guest N)`, `Controller
  disconnected`, or `Press Start to Join`.
- Below a thin rule, the match as set, read from the title's own tables: Map, Mode, Bots (and their skill).
- A hint line, "START on a controller joins, BACK leaves", which notes "All four player slots are taken." for five
  seconds when a fifth controller asks.
- It is redrawn as soon as any of it changes.

**Joining (no popup, no other screen).** A free controller's START on Instant Action takes the next slot at once:
- **Under split screen** (the compositor): its PartyJoin.
  - A controller that was a member before gets its own slot back, keeping its profile.
  - A new one plays as the profile its slot had last, when that profile exists and no other player has it,
    otherwise as a guest. No saved profile is changed.
  - Its START reaches no menu. A member's own START does nothing; its BACK leaves (PartyLeave, before a match only).
  - Every slot taken: player 1 is told and the panel says so.
- **The game alone in its window:** a controller that is not player 1's own (the one driving the game) asks with
  START. The window restarts as the compositor, back on Instant Action, with its choices kept (map, Match Options,
  Bot Options, Custom Options: CSX_INSTANT_STATE). That controller joins at once (CSX_SPLIT_JOIN_GUID).
  - Player 1's own START stays the menus' A.
  - The window restarts here (the existing split screen start), so the screen goes to the desktop for a moment.
- A press of A on Instant Action is the menus', so A never joins there.

**Start Match with joined players:**
- The compositor starts players 2-4 into player 1's match (the existing split screen: its layouts for 2, 3 and 4
  players).
- One player is a normal full-screen match.
- Number of Bots is held so players and bots fit the 65 slots (`join::ClampBots`).

**Multiplayer:**
- Server Browser and Direct-IP / Code are as before.
- Host Game opens the party with Host LAN Match highlighted: the match in this PC's csx-server, others on the
  network join.
- The separate Split-Screen row is gone; Instant Action covers it.

**What left the main menu:** Single Player (its screens are Instant Action's), the party's own row, and Training and
Change Profile (now in Extras). Mods left Options for Extras. No screen, mode, map, setting, profile or networking
was removed.

**Code.**
- `pcmenu::MainMenuLayout`, `HubScreen` and `HubRow` (src/game/pc_options_text.h), tested by
  tests/pc_options_text_test.py.
- The joining rules are in src/game/pc_join_rules.h (tests/pc_join_rules_test.py).
- src/game/pc_instant.cpp has the hub, its panel and its commands.
- src/game/pc_party.cpp has `InstantInput` (the joins and leaves) and the alone game's restart.
- src/linux/compositor.cpp has "party join on|off" and its START join; src/linux/window_input.cpp has the alone
  game's request.

## Multiplayer (the main menu's Multiplayer; Online Play before the redesign)

| Before | After |
|---|---|
| LAN Matches, Internet Matches, Favorites, Recent Servers | **Server Browser**: the one browser, its tabs LAN, Internet, Favorites, Recent |
| Join by Address | **Direct-IP or Code**: an address (and port) or an invite code; Ctrl+V and F6 as before |
| Host a Match (the title's System Link host screen) | **Host Game**: the party, Host LAN Match highlighted (map, mode, bots, rules, a LAN password) |
| (the party, the main menu's Multiplayer) | (split screen is Instant Action's: START on a controller joins there) |

**Host Game's architecture** (Peer-to-Peer before the redesign).
- The match runs in this PC's local csx-server process, started for it, as every menu match does (N4.7).
- It is seen on the LAN. Friends join through the browser's LAN tab, Direct-IP or Code, or an invite code.
- There is no direct peer-to-peer networking, NAT traversal or relay.
- The label is the user's choice for "host a match for friends".

**What players no longer see.** The title's System Link host screen offered a Dedicated Server row, and it is no
longer reached from these menus. The browser's Host a Match (Y) opens the party.

**Unchanged.** csx-server, its GUI (tools/csx_server_gui.py), its configuration and the authenticated transport.

**The party.** Multiplayer > Host Game opens it (Host LAN Match highlighted; its Local Only is still there).

## Map search (2026-10-31)

Both map lists have a search field above their list. These are Custom Game's Map Selection
(`SuperLauncher_SinglePlayerMatchMapList`), where single player matches, Custom Match, Deathmatch and Gun Game pick
their map, and the party's (`SuperLauncher_PCMatchMaps`), used by Peer-to-Peer and Host LAN Match. The code is in
src/game/pc_mapsearch.cpp; the search itself is in src/game/pc_mapsearch_filter.h, tested by
tests/pc_mapsearch_filter_test.py.

- **Where.** The field is in the header, on the "Map Selection" title's line, between the title and the background's
  COUNTER-STRIKE logo: 232..382 x 50..70 of the 640x480 menu layout. The logo starts at x 402. The 2D layout keeps
  those 640 columns at every aspect ratio, so the field keeps its place there too. It has a blue-grey border (two
  whitebox rectangles, well within the title's 8 images a screen) and its [x] at the right end. The list and the
  preview do not move. A screen whose title leaves no room (mapsearch::PlaceInHeader) gets the field above its list
  instead.
- **The field.** It shows "Search maps..." until something is typed.
  - A letter, digit, - or _ typed on the list starts typing in it, and so does a click on it.
  - Backspace, Delete, Left, Right, Home and End edit it. Ctrl+A selects all, and Ctrl+C, Ctrl+X and Ctrl+V copy,
    cut and paste.
  - Enter and Escape leave the field. Enter keeps the highlighted map. Neither starts a match nor closes the menu: a
    second Escape goes back, and A or Enter then starts the match as before.
  - [x] beside the field clears it.
  - While typing, every key is the field's: no menu shortcut or game binding fires. Up and Down still move the
    list's highlight.
- **Filtering.**
  - As it is typed, the list shows only the maps whose name or file name contains the search, case-insensitively:
    "dust" shows Dust and Dust II.
  - DLC maps and the game folder's maps are rows like the title's, so they are searched alike.
  - Each list only ever has the maps its screen offers.
  - The help line says "N of M maps", or "No maps found".
  - The names are indexed once when the menu is loaded, so a key never scans the disk; 5000 names filter in well
    under a millisecond.
- **The rows.**
  - The title's own table rows (their names, previews and values) are kept once a visit, and the matching ones are
    written in their place.
  - The selection stays on its map while that map is shown, else it moves to the first shown; it is scrolled into
    view, and the preview and description follow it.
  - The scrollbar (pc_scroll.cpp) now covers these lists too: dragging it or the wheel moves only the rows in view.
- **Choosing a map.** A, Enter or a double click run the screen's own command through `pc_mapsearch accept`.
  - With no map shown, nothing happens.
  - Otherwise the whole list is put back with the selection on that map's own row, and then the screen's command
    runs. The title's session start and the party's map step (which reads the row's index) see the list as they made
    it, so a map that was filtered out can never be started.
- **Controller.** Y opens the title's virtual keyboard for the same search. The search is optional: the D-pad and A
  choose maps as before.
