# The PC menus' structure (2026-10-31)

The PC settings and online menus after the consolidation pass. The code is in src/game/pc_menu.cpp (`Screens()`),
src/game/pc_scroll.cpp (scrollbar), src/game/pc_mouse.cpp (mouse) and src/game/pc_browser.cpp (online menu).

## PC Options

| Before | After |
|---|---|
| Video | **Video**: its rows and Advanced Graphics' |
| Advanced Graphics | (in Video) |
| Mouse | Mouse |
| Keyboard (PC Controls) | **Keybinds** |
| Controller | its rows in the title's **Control Options** (Options > Controller) |
| Audio | Audio |
| Gameplay | (in Interface) |
| HUD and Menus | (in Interface) |
| — | **Interface** |
| Multiplayer | Multiplayer |
| Mods | Mods |

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
- HUD: HUD Scale, HUD Scale (Split Screen), Scoreboard, Net Graph, Net Graph Detail. The HUD editor (F7 in a match)
  is unchanged.
- Menus: Smooth Menu Scrolling, Button Prompts (moved from Controller).

The match rules (Custom Match, the party's Rules) are not here.

**Keybinds** is PC Controls renamed. The screen ids, the `bind.*` keys and press-a-key rebinding are unchanged.

**Controller.** The title's Control Options (front end and pause menu) keeps its own six rows:
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

**Saved settings.** Every setting keeps its pc_settings.ini key (and its profile scope), so existing settings and
profiles carry over unchanged. Only where a row is shown changed.

## Scrollbar

One component for every PC settings list, including the Controller rows in Control Options.

**Drawing.** At the end of the title's table draw (hook 0xCD1E2, pc_scroll.cpp `DrawBar`), a list that has more rows
than it shows gets a 5-unit track and a thumb, 4 units right of the list:
- colours: the front end's blue or the pause menu's gold, drawn with the engine's FillRGBA (blending is switched back
  on after it);
- thumb length: the share of rows shown; thumb place: the first shown row's;
- nothing is drawn when every row fits.

The title already limits the rows drawn to the table's area. Smooth scrolling clips them while they ease.

**Mouse.**
- A click on the track selects the row at that height, and the list scrolls to show it.
- Pressing on the bar and moving drags: the row follows the pointer's height until the left button is let go.
- The wheel, the keys and the pad move the selection as before, and the list keeps it in view.
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
