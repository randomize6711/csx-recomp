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
   party survives a match and comes back to the lobby whole. The game alone in its window stays alone on the party
   screen (a party of one needs no compositor: its browser, Local Only and LAN matches run without its cost); the
   window then watches every controller, and another controller's A or START restarts it as the compositor, back on
   the party screen, where that controller joins with A (vk_party_alone).
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

## 5. As built (N4.8B-G)

### 5.1 Screens

| Screen | File | What |
|---|---|---|
| SuperLauncher_PCParty | pc_party.cpp | four panels (PLAYER N, device, profile, team, ready, the member's own button prompts); the leader's hint line |
| SuperLauncher_PCProfiles | pc_party.cpp | one member's profiles: select, Create New Profile, rename (X), settings (Y), delete (BACK, then a confirmation) |
| SuperLauncher_PCProfileName | pc_party.cpp | the title's profile keyboard (typed or on screen), 1-20 characters |
| SuperLauncher_PCProfile | pc_menu.cpp PcScreen "Profile" | a profile's settings: mouse sensitivity, invert, raw input, FOV, and the title's Controller Options |
| SuperLauncher_PCSplit | pc_menu.cpp PcScreen "Split" | Split Screen Options (two-player layout, player 1's controller), BACK on the party screen |
| SuperLauncher_PCPartyDestination | pc_lobby.cpp | Local Only, Host LAN Match, Server Browser (the title's main-menu look and help lines) |
| SuperLauncher_PCMatchMaps | pc_lobby.cpp | the title's Map Selection screen (its background, previews, kinds), its rows from the title's list and the folder's other maps |
| SuperLauncher_PCMatchModes | pc_lobby.cpp | Bomb Defusal, Hostage Rescue (the map's own only), the rules' name; Y Custom Rules |
| SuperLauncher_PCRules | pc_menu.cpp PcScreen "Rules" | the rules (16 rows, X defaults, Y rulesets) |
| SuperLauncher_PCMatchRulesets | pc_lobby.cpp | Default Rules, the saved ones, Save Current Rules As; load (A), rename (X), delete (BACK + confirmation) |
| SuperLauncher_PCLobby_<map> | pc_lobby.cpp | one a map (its preview is a fixed image of the screen): map, mode, rules, LOCAL ONLY / LAN, members (team, ready, leader), bots and slots, key rules; START starts |
| InGame_RootSinglePlayer (rebuilt) | pc_lobby.cpp PcLobbyPauseMenu | by player: see 5.6 |
| InGame_PCConfirmLobby, InGame_PCConfirmQuitGame | pc_lobby.cpp | the leader's confirmations |

Everything is the title's own UI (its text format, parsed, drawn, navigated and sounded by its code): its
backgrounds (Back_2ndary.1Pane, Back_Select_A_Map), legend bars (Back_Select, Cancel_Accept), highlight images, fonts,
sounds and map preview textures. Nothing of Halo's is used. Lines that change (panels, lists, the lobby) are the
screens' BackgroundText lines set at run time (at most 60 a screen, 8 images: the title's limits, 0xCDDE6 / 0xCDD56).

**Directions**: the title runs only its own table commands for the pad's directions, so the screens whose lines are
drawn by these commands have an unseen three-row table (the middle row a three-choice list): the title moves its
highlight or choice (its d-pad, stick, keyboard arrows and sounds), the frame reads the move as up / down / left /
right and puts the table back (PcProbeTable / PcProbeTake, pc_party.cpp). The first read after a screen opens is
not a press. The title's own screens' tables are never touched.

### 5.2 The party (compositor)

- Members, their controllers (with the controller's GUID, so a controller unplugged and plugged in again goes back to
  its player), profiles, teams and readiness live in the compositor (src/linux/compositor.cpp, Player.profile / team
  / party_ready / pad_guid) and outlive player 1's relaunches; player 1 reads them from State lines ("member N pad
  1|0|lost profile ID team T ready R", "kbm N", "focus N") and changes them with Setup "party set ..." / "party leave
  N" / "party focus N".
- While player 1 shows a party screen ("party on"), another member's controller buttons and left stick reach player 1
  as split::Msg::Party ("N a", "N left"...), a free controller's A or START joins (PartyJoin: the next slot), a
  member's B before it is ready leaves (PartyLeave: the members after it move up a place; only in the menus).
- **Menu focus**: a member that opens its Profiles has the menus (SetMenuFocus): its controller drives player 1's
  front end through player 1's virtual gamepad (always attached in player 1's process: a gamepad plugged in only then
  was seen by the title seconds late and its first presses lost), player 1's own controller, keyboard and mouse are
  held (the window's F11 / F1 stay). B on Profiles gives the menus back. So player 2 never moves player 1's menus.
- The keyboard and mouse stay player 1's (split_kbm); player 1 may play on a controller too (Split Screen Options,
  split_p1_pad). Four-controller-only parties: player 1 on the first controller.
- **The game alone in its window** (no compositor): player 1's panel is kept by pc_party.cpp itself; window_input.cpp
  opens every controller on a party screen, and one that is not player 1's (with Also a Controller, the first is
  player 1's) asks to join instead of being player 1's input: the window restarts as the compositor
  (CSX_FRONTEND_OPEN reopens the party screen; player 1's profile is kept in CSX_PC_PROFILE), and that controller's A
  joins. Each slot's last profile (party.profile_N) is taken again while it is free.
- Player 1's controller unplugged: player 1 keeps the menus through the keyboard; its controller back is its again. No
  leadership transfer was needed with the keyboard always player 1's (a controller-only player 1 without a keyboard
  plugs its controller back in).

### 5.3 Profiles

- src/common/profiles.{h,cpp}: profiles/<id>.ini in the folder of pc_settings.ini (the compositor's run folder for
  every local player: CSX_PROFILE_DIR), never the game's files; id 16 random hex digits (never reused; ids that are not
  that are refused, so no id names a path); name 1-20 characters (the same rule as the players' names: no quotes, `;`,
  `\`); values written whole (a temporary file renamed). Unit test tests/profiles_test.py.
- Profile-scoped settings (profiles::Scoped): mouse_sensitivity, mouse_invert, mouse_raw, fov, bind.*, pad.* (the
  title's Controller Options: invert, sensitivity, fast_look, left_hand, vibration, thumbstick, buttons, hints). A
  process plays as one profile (pc::UseProfile, CSX_PC_PROFILE): Get of a scoped key reads the profile (the PC's value
  when it has none), Set / Save write it there. No viewmodel FOV (the game has none).
- The title's Controller Options: the profile's pad.* values are put into the title's own screen (the front end's
  SuperLauncher_ControllerOptions, the pause menu's InGame_ControllerOptions, by row position) and its own commands
  run (`stuffUItoPlayerData`; in a match also `SetCurrentControllerOptions`), once each boot; a player's Accept on
  either screen saves them back to its profile (pc_party pad_capture).
- One profile is never two members' (the panel's left / right and Profiles skip or refuse another's). A new profile
  starts from the PC's current mouse and FOV settings. split_name_1..4 become profiles once (profiles_migrated).
- Players 2-4 are started with their profile (CSX_PC_PROFILE) and its name (CSX_PLAYER_NAME); player 1 plays as its
  member's profile, its name in a match the profile's.

### 5.4 Destinations, maps, modes, rules

- **Local Only**: the N4.7 offline local server (127.0.0.1, a free port), now unannounced (no LAN discovery listener,
  no master, no queries: server_query.cpp) and private: a password made at random for the match (CSX_PARTY_PASSWORD
  -> the server's CSX_SERVER_PASSWORD, never on a command line or in a log) that only the party's processes have
  (player 1's join, the compositor's Match "join ... PASSWORD" for players 2-4). net_party_private: another game on
  the machine connecting by address is refused (bad password).
- **Host LAN Match**: the N4.7 hosted local server (the PC port, every address, LAN discovery and queries answered),
  never registered with a master server for a party, an optional password (the lobby's Y; letters and digits).
- **Server Browser**: the N3 browser unchanged (SystemLinkSetup, SuperLauncher_SystemLink); its join refuses a server
  without a free slot for every member ("The server has room for N more players"); the other members follow player 1
  in as before (N4); the server's own rules apply.
- **Maps**: the title's Map Selection list as it is (18 maps: names, previews, "Demolition" / "Hostage Rescue"), then
  the game folder's map archives it leaves out (de_inferno, cs_office: no preview), by name; training maps not.
  Random is not offered (no map of its own). Each map's mode is the title's kind for it (its list), else its prefix.
- **Modes**: Bomb Defusal (de_), Hostage Rescue (cs_), only the map's own (the other refused with a line). The
  screen is a list so more modes need no new screen.
- **Rules** (16 rows): Bots (0-61, capped at 65 less the party's players at the start), Bot Difficulty, Bot Team,
  Friendly Fire, Round Time, Freeze Time, Buy Time, Starting Money, Rounds (mp_maxrounds), Win Limit, Time Limit,
  Team Balance (mp_limitteams), Auto Team Balance, C4 Timer, Player Collision, Spectators (allow_spectators). Every
  row is a cvar the server has, set on its command line as the title's own menus set theirs. **Not offered**: maximum
  money (the game has no such cvar; 16000 is in its code). Defaults: the title's Normal preset. Values are checked
  against the rows' choices (a value that is not one is the default) and again by the server's MatchLine.
- **Rulesets**: rulesets/<id>.ini (the profiles' store, its own folder and keys): Save Current Rules As (named),
  load, rename, delete (confirmed), Default Rules; the rules in use are PC settings party.rule.<cvar> and their name
  (party.ruleset); shown as the ruleset's name while unchanged, else Custom Rules.

### 5.5 Lobby and the match

- Every member readies again in the lobby (A; up / down its team while not ready; B not ready); START starts when
  every member is ready and none is editing its profile. The match line: `+map MAP +wait +bot_quota N` and the rules;
  the title relaunches into it from the lobby with a Single Player match's launch data (3), so N4.7's RelaunchNotify
  starts the local server (offline or hosted by CSX_PARTY_HOST) and the game joins it; players 2-4 follow (N4). Each
  member joins its lobby team itself when the title's team screen first opens (CSX_PARTY_TEAM: the screen's row
  commands slot1 / slot2 / slot5). Only in a lobby's match: player 1's "join" (kSplitMatch) ends with "party" when it
  has CSX_PARTY_LOBBY, and only then does the compositor give players 2-4 their lobby teams; in any other split match
  every player picks its team on the title's own screen (the full regression found players 2-4 auto-joined there).
- Bots: at most 65 less the party's players (SplitMatchFromMenus with CSX_PARTY_MATCH), so four players and 61 bots fill
  the 65 slots.

### 5.6 Pause menus by player

| Player | Rows |
|---|---|
| Players 2-4 (every split-screen match) | Return to Game, Match Statistics, Change Teams, Player Settings, Leave Match |
| Player 1 of a party match | Return to Game, Match Statistics, Change Teams, Player Settings, Restart Match, Change Map, Return to Lobby, End Match, Quit Game |
| Anyone else | the title's as before (N4.6 / N4.7) |

Players 2-4 never had a way to end the game (their Quit ended their own process); Change Map and Restart Match are
gone from their menus (a client's Change Map relaunched it into a map of its own). **Return to Lobby** (confirmed):
the leader's game relaunches to the front end (its local server stopped, players 2-4 end, the party kept) and opens
the match's lobby, every member not ready. **End Match**: the title's Quit (the front end's main menu, the party
kept). **Quit Game** (confirmed): the window closes (the compositor and every player end; the local server stops
with its game).

## 6. Performance

Measured on the test runs (headless gamescope 1920x1080, the tests' 30 frames/s cap; another session's GPU tests
shared the machine):
- **The party screens** start no process for the other members: with four members, two processes (the compositor 46
  MB, player 1 154 MB), 7% of one core in all, player 1 steady at its 30 frames/s (vk_party_pads, procs.csv over the
  party screen).
- **Starting a match from the lobby** (vk_party_match): the local server ready 0.7 s after START, player 1 joined
  at 1.5 s, player 2 started at 1.6 s and connected at 2.2 s, both in the game at 4.3 s: N4.7's path and times.
- **In the match** nothing differs from N4.7: the same local server (the bots in its process, not player 1's), the
  same compositor and players; N4.7's benchmarks hold (docs/splitscreen.md 17.4). One-player play without the party
  (Single Player from the main menu, the game alone in its window) runs no party code (vk_perf_aztec_64 in the
  regression).
- **Load and memory**: the front end's text grows by the party's screens (20 lobby screens, one a map: a few KB
  each; within the 512 KB the menu loader adds). The party screen reads its members' profile names from their small
  files on each redraw (the measured 7% above includes it; a cache is left for later, TODO).

## 7. Manual checks (need the user; not done)

Automated tests used virtual controllers only. Not performed, for the user (`./run_game.sh`, Main menu > Multiplayer):
1. Two to four physical controllers: each joins with A (or START) on the party screen; one leaves with B; one unplugged
   and plugged back keeps its player; a member's buttons move only its own panel.
2. Controller-only play: Split Screen Options (BACK) > Player 1 > Also a Controller; the whole flow on the controller.
3. Profiles: create one by controller on the on-screen keyboard and one typed; rename; edit its settings (Profiles > Y)
   and feel them in a match (look sensitivity, inverted look on the controller; mouse sensitivity on the keyboard);
   delete one (the confirmation); restart the game: they are still there.
4. A match from the lobby: Local Only and Host LAN Match (another PC on the network finds it in its LAN list and
   joins; with a password), Hostage Rescue on a cs_ map, Custom Rules (a ruleset saved and loaded), teams from the
   lobby.
5. Pause menus: player 2's has no Change Map, Restart Match or Quit (Leave Match leaves only it); player 1's Return to
   Lobby brings everyone back to the lobby; End Match; Quit Game closes the game.
6. Look: the screens next to the title's own (fonts, highlights, sounds, the map previews in the lobby).
7. Audio by ear: the menus' sounds on the new screens, and each split-screen player's sound in a party match (the
   tests use null or captured audio, never the speakers).

## 8. Known limitations

- No pre-match lobby with LAN players (5.5, pc_multiplayer.md 20).
- Map modes from the title's list or the map's prefix (no entity check before a map loads).
- The keyboard and mouse are player 1's (a keyboard player 2-4 is not offered).
- Team choice: Auto-Select, Terrorist, Counter-Terrorist; joining is the server's rules (a full or stacked team refuses
  as the title's own team screen does).
- Remote servers: the browser's rules are the server's; the party cannot change them; a server's own match flow (map
  changes) applies, no return to a local lobby.
- Linux only (the compositor, the local server); the profile, ruleset and lobby code is portable.

## 9. Regression

**Status: automated regression complete** (2026-10-30). Physical controllers, audio by ear, the look next to the
title's screens and a real LAN between two PCs remain manual checks (section 7), not done.

### 9.1 The complete suite on the final build

Binaries built from 90ba692 (the same build since: sha256 checked at each start, unchanged; 7f1b9ad changed one network
test only). Every test of every suite, none left out: the window suite is all 101 scenarios in tests/scenarios
(vk_*), 37 more than earlier runs (the vk_perf benchmarks, bloom, viewmodel/HUD, scoreboard 4:3 and 21:9, Xbox effects,
menus, match, title, intro, resolution, buy tap, the audit, compare and soak).

| Suite | Build | Result |
|---|---|---|
| Unit (tests/*_test.py) | 90ba692 | 9 / 9 |
| Headless (run_regression.sh, every regress_*) | 90ba692 | 46 / 46 |
| Network, first run (every tests/net) | 90ba692 | 38 / 39: net_split_browse (below) |
| Window (every vk_*) | 7f1b9ad | 100 / 101: vk_soak_direct not runnable by the runner (below); passed as documented |
| Network, second complete run | 7f1b9ad | 39 / 39 |

- **net_split_browse** (the first network run): the test's timing, not the game. N4.8's party steps put player 2's
  join about 30 s into a round the server's two bots were already playing; a late joiner is a spectator until the next
  round (the title's rule), and the test looked for its buy menu 10 s later. The test now waits for that buy menu (this
  round or the next) and its server stays up past the next round start (7f1b9ad). Alone: pass (an early join); in the
  second complete network run: pass.
- **vk_soak_direct**: its header names its launch as `tests/launch/<map>_beginner` (a scenario for a chosen map, run by
  hand: FINDINGS "Stability"), so the runner started nothing (0 steps). Run as documented, its steps unchanged
  (de_airstrip, real time, 3600 s limit): pass, 30.8 min, 9 rounds, no guest trap or Vulkan failure.
  audio3d_direct.txt and rounds_direct.txt are the other two hand-run scenarios (a chosen map); they are in no suite and
  were not run.
- **Conditions**: one test at a time. Another project's game runs (kuf_cod) were active almost throughout: the network
  suites and 104 of the window starts ran beside them after a capped wait (logged). The benchmarks' figures from this
  run are shared-load numbers, not performance results (section 6 has those). The window suite was stopped once by the
  session's low-memory guard (after 26 scenarios, none mid-run) and resumed with the 75 not yet run, each test waiting
  for 10 GB available; none had to.

### 9.2 Earlier: the first complete run (38691c6)

Unit 9/9, headless 46/46, network 39/39, window 27/27 + 17/17, split and party 15/20. The five split failures
(vk_split_teams, vk_split_pads_2/3/4, vk_split_kbm_3) were one N4.8 bug: players 2-4 joined their lobby team in a split
match that was not a lobby's (5.5). Fixed in e586adc; all five pass in 9.1.

### 9.3 Open intermittents

- vk_split_2_219's outside SIGKILL (N4.6-N4.7): not seen in either complete run (the SIGKILLs in the logs are the
  tests' own: frozen clients ended by the runner, the crash tests). Cause unknown; tracing it needs root (not done).
  Not closed.
- regress_bots_aztec_11's spawn check: passed in both complete runs since the stale-log fix (84d336a). Not declared
  closed on two passes.

## 10. Commits

3237f93 (design), 738cc98 (profiles), 2be7610 (loopback privacy), 20bc66e (the party's password), 5a7e546 (the
frontend), eb8ce34 (OwnerSlot), 5342de3 (keyboards, LAN and rules tests), 5c72156 (65-slot party test), 84d336a (test
logs), 015dd13 (browser password), 38691c6 (the party alone in its window), e586adc (the lobby team fix), 90ba692 (docs),
7f1b9ad (net_split_browse's timing), then these docs.
Branch g8.2b, not merged.
