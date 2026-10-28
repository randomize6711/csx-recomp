# Multiplayer frontend: party, profiles, match lobby (N4.8)

Milestone N4.8, approved by the user 2026-10-28: a console-style multiplayer frontend for one to four local players
(Halo: Combat Evolved's Xbox multiplayer flow as the reference for how it works; Counter-Strike Xbox's own menus,
fonts, backgrounds, sounds and highlights for how it looks: nothing of Halo's is used). This document is the design
(N4.8A) and, as the checkpoints land, what was built.

## 1. What exists (the starting point)

- **Processes** (docs/splitscreen.md 5): the compositor (the window, every input device, the layout) and one game
  process per local player; player 1 runs the front end; players 2-4 are started only when player 1 hosts or joins a
  match, straight into `+connect` to its server, and ended when it is back in the menus. The compositor is the one
  process that lives through everything (player 1's relaunches, map changes, matches).
- **Menus**: the title's own UI text (FrontEndUI.txt / InGameUI.txt), extended at load by src/game/pc_menu.cpp and
  friends: screens written as the title's (Back_2ndary.1Pane, Secondary_Highlight, its fonts and sounds), their
  logic in `pc_*` console commands, their text lines (BackgroundText) set at run time (PcSetScreenLine). Nothing
  draws by itself: the title's UI code draws, navigates and sounds every screen.
- **Split Screen screen** (pc_split.cpp): Local Players 1-4, layout, player 1's controller, Player Names
  (`split_name_1..4`); START on a free controller adds a player (the compositor's StartToJoin).
- **Hosting** (N4.7, docs/splitscreen.md 17): a menu match runs in csx-server (local_server.cpp), offline on
  127.0.0.1 or hosted on the PC port; the game joins it.
- **The title's map list** (SuperLauncher_SinglePlayerMatchMapList): 18 maps and Random, each with its preview image
  (HighlightImage "de_airstrip"...), its name and its kind ("Demolition", "Hostage Rescue"). The game folder has 29 map
  archives (de_inferno, cs_office, the two training maps and others the list leaves out).
- **The title's profile** (an Xbox save: U:\<id>\data.bin, T:\CurrentProfile.dat on the emulated disk, made fresh by
  run_game.sh at each start): name and the Controller Options (Inverted Look, Look Sensitivity, Fast Look, Damage
  Vibration, Thumbstick and Button Configuration, Automatic Hints; Left-Hand Weapons in the front end's screen).
  Players 2-4 run with no title profile (its defaults). The PC settings (pc_settings.ini beside the game) hold mouse,
  FOV, bindings, display.
- **Rules the server has** (cvars in the executable): mp_roundtime, mp_freezetime, mp_buytime, mp_startmoney,
  mp_friendlyfire, mp_timelimit, mp_maxrounds, mp_winlimit, mp_limitteams, mp_autoteambalance, mp_c4timer,
  allow_spectators, bot_quota, bot_difficulty, bot_join_team, bot_allow_* (weapons), pc_player_collision.
  **No maximum money** (no mp_maxmoney: the 16000 cap is the game's code): not offered.

## 2. Decisions

1. **The party lives in the compositor.** The pre-match screens are player 1's (one front end, in player 1's
   process); no process is started for players 2-4 until the match starts (no extra processes for the lobby). The
   compositor keeps the party (members, devices, profiles, ready, team choice) through player 1's relaunches, so the
   party survives a match and comes back to the lobby whole. Multiplayer from the main menu, alone in the window,
   restarts the window as the compositor first (as Split Screen > Accept does today) and opens the party screen.
2. **Other players' input reaches the party screen as party messages.** While player 1 shows a party screen it
   tells the compositor ("party on"); the compositor then sends every other member's controller buttons (and a free
   controller's A / START, the keyboard's join key when the keyboard is not player 1's) to player 1 as
   split::Msg::Party (member, button), instead of to a player process that does not exist. Player 1's own devices
   drive the title's UI as always. When a member edits its profile (the keyboard, its settings screen) the
   compositor gives that member the **menu focus**: its controller drives player 1's front end as player 1's own
   controller would, player 1's devices are held, the other panels wait; the screen's Done or Back gives it back.
   So a player edits only its own profile, and player 2 never moves player 1's menus.
3. **Profiles are PC files** (profiles/<id>.ini beside pc_settings.ini, the writable folder the game runs in; never
   the game's files): a stable id (16 hex digits, random at creation), the name (shown; two profiles may share it),
   the profile-scoped PC settings (mouse sensitivity, invert, raw input, FOV, bindings) and the title's Controller
   Options (stored as the rows' values). A process plays as one profile: its PC settings read the profile's values
   for those keys and save its changes there (pc_settings.cpp gains a profile layer); its Controller Options are put
   into the title by the title's own screen and commands (InGame_ControllerOptions' rows, then
   `stuffUItoPlayerData; SetCurrentControllerOptions`, as its Accept does) when it boots, and read back into the
   profile when the player accepts that screen. No viewmodel FOV (the game has none).
   `split_name_N` becomes a profile on first start (migrated once, the setting kept).
4. **Player 1 leads.** Only player 1's devices operate the destination, browser, map, mode, rules, lobby Start,
   Restart / Change Map / End Match / Return to Lobby / Quit Game. Players 2-4 have their panel (join, leave,
   profile, team, ready) and, in a match, a pause menu without global actions (Leave Match instead of Quit).
5. **Local Only** is the N4.7 offline local server (127.0.0.1, no LAN or master) **and** private: the server takes
   only the party's own identities (each player's identity key, given to it at start), so even a process on this
   machine that is not in the party is refused. **Host LAN Match** is the N4.7 hosted server (PC port, LAN discovery),
   never registered with a master server, optional password. Both start from the lobby with the same rules.
6. **Matches start from the lobby** with a match line the frontend writes itself (map, rules, bots) and the title's
   own relaunch, as the Single Player screens' Start does: the N4.7 path (RelaunchNotify starts the local server and
   the game joins it) is unchanged, so restart, map change, the owner's pause and the 65 slots behave as in N4.7.
7. **Remote players in the pre-match lobby**: not in N4.8. The server exists only once the match starts; LAN players
   join the match itself (the browser, by address) as in N4.7. A shared pre-match lobby over the network would need a
   new protocol (a lobby session before the server): left for a later milestone, documented.
8. **Map metadata**: the title's own map list (names, previews, kinds) is the source; maps in the game folder that the
   list leaves out are offered by their prefix (de_ Bomb Defusal, cs_ Hostage Rescue). Each map's actual objectives are
   checked when the server loads it (the game rules' bomb target and rescue zone) and remembered (maps.ini), so a map
   whose kind is wrong in that guess is corrected after its first match. Modes: Bomb Defusal and Hostage Rescue only.

## 3. Flow

```
Main menu > Multiplayer
  PARTY (SuperLauncher_PCParty): four panels; PRESS A TO JOIN; each member: PROFILE (select / create / rename /
        edit / delete), TEAM, READY; B leaves (player 1: back to the main menu)
    player 1, A on Continue: DESTINATION (SuperLauncher_PCDestination)
      Local Only  ─┐
      Host LAN Match ─┴─> MAP (SuperLauncher_PCMaps: preview, kind, name) -> GAME MODE (SuperLauncher_PCModes; Y Custom
                          Rules: SuperLauncher_PCRules, rulesets load / save / rename / delete / defaults) -> LOBBY
                          (SuperLauncher_PCLobby: map preview, mode, ruleset, Local Only / LAN, players, teams, ready,
                          bots, slots, key rules; player 1 starts when every member is ready)
      Server Browser -> the N3 Multiplayer screens (LAN, Internet, Favorites, Recent, Join by Address); a join checks
                          the server has a free slot for every member
  in a match: player 1's pause menu Resume, Change Team, Player Settings, Restart Match, Change Map, Return to Lobby,
  End Match, Quit Game; players 2-4: Resume, Change Team, Player Settings, Match Statistics, Leave Match
  Return to Lobby: the server stopped, players 2-4 ended, player 1 back on the lobby with the party as it was
```

## 4. Checkpoints

N4.8B party and profiles; N4.8C destinations; N4.8D maps and modes; N4.8E rules; N4.8F lobby and start; N4.8G pause
menus and return to lobby; N4.8H regression. Each lands with its tests (window scenarios with virtual controllers)
and a commit.
