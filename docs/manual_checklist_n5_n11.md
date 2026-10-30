# Manual testing checklist: N5..N11

Start the game with `./run_game.sh`. It boots from the intro movies; see docs/autonomous_pass.md for the build. Each
item says what to look for. N4.8's own outstanding manual checks are listed separately in
docs/multiplayer_frontend.md and are not repeated here.

## A. Alone (keyboard, mouse, one controller)

**Menus (N5)**
1. Front-end menus: Escape goes back one screen; Main Menu > Exit Game quits.
2. PC Options lists its categories: Video, Advanced Graphics, Mouse, Keyboard, Controller, Audio, Gameplay, HUD and
   Menus, Multiplayer, Mods. Long lists scroll smoothly (HUD and Menus > Smooth Menu Scrolling Off: row by row).
3. Advanced Graphics: the explanation panel beside the rows describes the highlighted one. Y (Back on the keyboard)
   opens Details.
4. Typing digits on a number row (Field of View, sensitivity, rates) types the value; Enter keeps it; out of range
   says "Use LO to HI".
5. Custom Game map list: type letters ("vos") to jump to a map; the controller's Y opens a search keyboard.
6. Prompts: with a controller, the menus' button glyphs match it (Xbox / PlayStation / Switch...); with the mouse,
   keyboard keys. Controller > Button Prompts forces a style.

**In a match**
7. Buying: `,` and `.` buy ammunition, F3 autobuy, F4 rebuy, the grenade keys select grenades. Radio menus (`radio1`
   to `radio3`) and the CZ lines (`csx_radio circleback`) play.
   - **Look at:** when a radio menu opens, the text was seen drawn several times on top of itself in one test shot.
8. Scoped sensitivity: zoomed AWP aim speed follows Mouse > Scoped Sensitivity.
9. Audio: Audio > Output Device lists devices; Audio in the Background Off silences the game when another window has
   focus.
10. HUD and Menus: the scoreboard Toggle mode; Net Graph On sits in the bottom right corner above the money; the
    Weapon Field of View (Advanced Graphics) changes only the gun.

**Game modes and gameplay (N6, N7)**
11. Custom Game: Inferno and Office are in the map list, once each, with previews. The match loads them.
12. Custom Game > Match Options > Game Mode Deathmatch: no round end, respawn after ~2.5 s with a glow and $16000,
    buy at spawn, timer at 0:00 harmless. Gun Game: a weapon each kill; reach the knife and win with it: "NAME wins
    Gun Game!" and the next map.
13. Gravity (Match Options): Very Low, Low, High. Jumps change.
14. F turns the flashlight on and off (the light shows in dark places). T sprays a logo where you look.
    - **Look at:** whether a logo is drawn (the server sprays; what this game's client draws without a custom logo
      is unverified).
15. Gameplay > Flashbang Effect Black: a flashbang blinds with black. Death Camera First Person: after dying the view
    sinks and rolls for 3 s, then the original camera.
16. Controller: Back + D-pad Up toggles the flashlight, Back + D-pad Down sprays; the choices in Controller >
    Flashlight / Spray Logo.

**HUD (N10)**
17. In a match, F7 opens the HUD editor:
    - Tab chooses an element (outlined in yellow); the arrows move it; `+` and `-` resize it;
    - Enter saves, Escape cancels;
    - the layout stays after a restart.

    HUD and Menus > HUD Scale changes all elements.

**Mods (N8)**
18. Make a mod (docs/n8_mods.md: `tools/make_mod.py`), turn it on in PC Options > Mods, restart. The AK-47 or the
    player model changes. Looks > Original Xbox turns every mod off.
    - **Look at:** whether a CS 1.6 or CZ model has the right textures. This game keeps model textures apart from
      the models, and the mod's model may pick up this game's texture of the same name.

## B. Extra controllers (split-screen)

19. Two to four controllers: split-screen. HUD and Menus > HUD Scale (Split Screen) and F7 in a split view edit the
    split layout (hud_split.*).
20. When three players leave, the last one gets a single window (N5F).
21. Each player's Flashbang Effect and Death Camera follow player 1's settings.

## C. Another person (same PC or LAN)

22. Host a Match (LAN), press F6 in the match: the invite code is copied. The friend pastes it with Ctrl+V into the
    server browser's Join by Address and joins (the key pinned). The password is still asked separately.
23. A Deathmatch or Gun Game party (Rules > Game Mode): both players respawn and climb the ladder.

## D. Another PC

24. csx-server on one PC with `game_mode deathmatch`, `map_rotation de_dust2,de_aztec`, `frag_limit 10`: play to the
    limit; the next map is de_aztec.
25. `python3 -I tools/csx_server_gui.py`, open the printed address: create a preset, Start, see the status and
    players, Kick, Ban, Change map, Stop.
26. Content mods: a server with a content mod and a client without it. The client is disconnected, and the log says
    why.

## E. External accounts (blocked until the user registers)

27. Steam invitations: need a Steam App ID (Steamworks partner registration).
28. Discord invitations: need a Discord application ID (developer portal).

## F. Hardware not available here

29. Wii Remote / Nunchuk: prompts only. Motion and pointer are not implemented.
30. Steam Controller: with Steam running, only the virtual controller acts (no double input); its prompts.
31. DualSense / Switch Pro: the touchpad / Share buttons as Flashlight or Spray choices.
32. A 4K monitor: HUD and menus at 4K.
33. Physical controllers in general: every controller item above was tested with virtual devices only.
