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

## Multiplayer (the main menu's Online Play)

| Before | After |
|---|---|
| LAN Matches, Internet Matches, Favorites, Recent Servers | **Server Browser**: the one browser, its tabs LAN, Internet, Favorites, Recent |
| Join by Address | **Direct-IP or Code**: an address (and port) or an invite code; Ctrl+V and F6 as before |
| Host a Match (the title's System Link host screen) | **Peer-to-Peer**: the party (map, mode, bots, rules, a LAN password, split-screen players) |

**Peer-to-Peer's architecture.**
- The match runs in this PC's local csx-server process, started for it, as every menu match does (N4.7).
- It is seen on the LAN. Friends join through the browser's LAN tab, Direct-IP or Code, or an invite code.
- There is no direct peer-to-peer networking, NAT traversal or relay.
- The label is the user's choice for "host a match for friends".

**What players no longer see.** The title's System Link host screen offered a Dedicated Server row, and it is no
longer reached from these menus. The browser's Host a Match (Y) now opens Peer-to-Peer.

**Unchanged.** csx-server, its GUI (tools/csx_server_gui.py), its configuration and the authenticated transport.

**Still in place.** The main menu's Multiplayer row (the party, N4.8) is still there, so Peer-to-Peer and it lead to
the same party.
