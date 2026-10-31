# Post-N11 correctness and UI repair pass (manual-testing bug fixes)

Branch g8.2b (not merged). Linux. The user's issues after their first play-test of the N5..N11 build. Each section:
the root cause found, the fix, the files and the targeted tests. Commits and results: docs/report_bugfix_pass.md.

## 1. Gameplay settings could not be clicked

- **Cause.** `PcBrowserScreen` (pc_browser.cpp) decided which PC screens are server lists, where a click only selects
  a row. It used a fixed list of the settings screens; Gameplay and Mods were not on it. Clicks there selected rows but
  never pressed the arrows.
- **Fix.** Every screen pc_menu.cpp declares as a settings screen is excluded (`PcSettingsScreen`).
- **Test.** `vk_settings_click`: PC Options' hub opens Gameplay by a click, the arrows change both rows, ACCEPT on
  the help bar saves them (pc_settings.ini).

## 2. and 3. Video and Advanced Graphics text outside its boxes

- **Cause.** The values' column was sized for the console's short values: "Native (1920x1080)", "Same as Field of
  View" and the longer labels were cut or ran past their arrows. The explanation panel's lines were counted in
  characters, not in the font's widths, and could run under the help text.
- **Fix (pc_menu.cpp).** Column widths per screen kind (55/45 beside the panel), the arrows image and table placed
  from the table's own size, text width measured with the font's character widths (`Units`), panel text wrapped to
  its width with "(more in Details)" when it does not fit, and shorter values ("Native", "Same as FOV",
  "Keyboard/Mouse", "Xbox Visual Effects"). The font size is unchanged. No setting was removed.
- **Tests.** Window shots at 1920x1080, 3840x2160, 2560x1080 and 1280x960 (looked at); `vk_auto_exposure`,
  `vk_bloom_43`, `vk_vsync`, `vk_vsync_play`, `vk_xbox_effects_169`, `vk_display_mode`, `vk_menu_mouse(_fe)` (each
  graphics setting still works from its row; their click points moved with the arrows); `regress_pc_menu_mouse`.

## 4. Xbox Live froze the game

- **Cause.** The main menu's Xbox Live row ran `!enterLive`, the Live sign-in, which has no service to reach.
- **Fix (pc_browser.cpp).** The row is **Online Play** and opens the PC's online menu (LAN, Internet, Favorites,
  Recent, Join by Address, Host a Match: the existing browser, not a copy). Back returns to the main menu.
  - The party code (pc_party.cpp) rewrites the *first* main-menu row whose command is exactly System Link's into the
    party's. Online Play now comes first, so it took the party and Multiplayer kept System Link. Online Play's command
    is now written differently.
- **Other Xbox-only entries.**
  - The main menu's and pause menus' Live status and gamertag lines ("Sign In failed", "Not Signed In") are drawn
    transparent, in place: the title fills them in by their order.
  - The pause menus' Friends List (Live friends) is **Copy Invite Code** (pc_invite.cpp, as F6), in the same place,
    so the rows below keep theirs.
- **Test.** `vk_online_play`.

## 5. Button prompts in Custom Match

- **Cause.** Prompts are drawn from the legend strip's name (Cancel_Accept_Reset...). Only three-button strips with
  six known words were converted; Custom Match's `Cancel_Accept_Reset_Configure` (and Change Profile's five-button
  strip, the server lists', Controller Options') kept the Xbox art.
- **Fix (pc_prompts.cpp, pc_prompts.h, pc_mouse.cpp).**
  - Words added: Configure, ControllerLayout, Join, RefreshResults, HostAMatch, Remove, New, Rename, REMOVE, Host.
    Each has the button its screen's handler uses.
  - Up to five buttons, laid out right to left by their words' widths.
  - Clicks on the help bar use the same layout (`PcLegendSlots`).
  - A screen at the title's 8-image limit (Change Profile) drops the invisible strip; a comment names the legend for
    the mouse.
  - The keyboard style's White and Black are keys 1 and 2, which the menus actually use (were "Page Up/Down").
- **Prompt style.** Automatic and manual both apply at once. The style is per process, so each split-screen player's
  follows their own device or settings.
- **Left as the original.** Five legends: Xbox Live-only screens and the old system-link browse screen, none reachable.
- **Test.** `vk_prompts_legends` (keyboard, PlayStation and Series styles on Custom Match, a click on CONFIGURE BOTS,
  Change Profile's five, a click on NEW), `vk_prompts`.

## 6. Single-player pause

- **Cause.** The server already paused its simulation for the owner's pause menu (`pc_owner pause`, SV_Physics and
  the server clock skipped), but the clients were never told. Their own clock ran on, so animations, the HUD's round
  timer and client effects went on behind the menu, and the timer jumped.
- **Fix (server_admin.cpp).** The owner's pause sends `svc_setpause` in the reliable datagram, as the engine's own
  pause does (Host_Pause_f 0x63B80, through SZ_GetSpace). The clients' clocks stop and resume from the same moment.
- **Unchanged.** Split-screen and network matches are not paused: the server refuses when another human is
  connected, and unpauses when one connects.
- **Spam fix (net_session.cpp).** A split-screen player's or network client's menu no longer sends "pause" to a server
  that is not its own. It was refused every frame (seen in the user's logs).
- **Test.** `vk_local_server`: across a 7.4 s pause the client's clock moves 0.06 s (logged: "the client's clock at").

## 7. Deathmatch respawn HUD, crosshair and knife

- **Cause.** Just after a mode's respawn (the player's Spawn), the game strips the player once more: every item, the
  suit too, then only the default pistol back. Without the suit the client's HUD hides health, armour, ammunition and
  the crosshair; the knife was gone. Seen in the server's state: weapons a0020000 (suit, knife, pistol), then 0, then
  00020000.
- **Fix (pc_modes.cpp).** For 1.5 s after a mode's respawn, a stripped suit is put back with the knife (Gun Game: its
  level's weapons). Bots alike.
- **Test.** `vk_dm_respawn`: three deaths and respawns through the local server; window shots, weapon in hand.

## 8. Deathmatch economy and dropped weapons

- **Buy time.** No end: mp_buytime is raised to 99999 while the mode is on; the value before is put back when another
  mode is chosen. The earlier "round start reset" at each spawn is gone.
- **Money.** Set (not added) to $16000 at every spawn, humans and bots, logged. Was already so.
- **Weapons on the ground.**
  - The game's weapon boxes go after 20 s, server side, through their own Kill think, which also removes the weapons
    inside.
  - A box picked up is gone before (the game removes it). Held weapons are never boxes. The bomb's backpack stays.
  - `pc_weapon_stay N` sets the time (5..600), also on csx-server.
- **Not done.** Buying outside the buy zones (optional in the request).
- **Other modes.** Unchanged.
- **Test.** `regress_dm_economy`.

## 9. Window flicker on quitting a match

- **Cause.** A match's start and its Quit exec the game again (the title's relaunch). The new image's window opened
  as a window and went full screen only after its first frame (the Hyprland tiling workaround).
- **Fix (window_main.cpp).**
  - The window's state is kept in the environment the exec inherits (`CSX_WINDOW_STATE`: fullscreen, or windowed
    with its size).
  - A relaunched window opens full screen at once, or at the last window's size.
  - If the compositor did not make it full screen, the first frame asks again, as before.
  - Refresh rate and resolution: borderless desktop full screen keeps the desktop's.
- **Tests.** `vk_relaunch_window`, `vk_relaunch_windowed`. gamescope does not report SDL's full-screen flag, so only
  the window's decisions are checked. The Hyprland desktop needs the play-test.

## 10. Loading bar outside its frame

- **Cause.**
  - On a wide frame (the user's 16:9), the bar's frame is a screen rectangle, which the widescreen layer moves to the
    centred 640 columns. The bar's fill is drawn with its own vertices, x 74 to the end x in 640x480 terms, which were
    not moved: the fill sat left of the frame.
  - Also, the fill's end is fraction × width: a load past the count the title expects gave a fraction over 1.
- **Fix (pc_loading.cpp, hooks 0xC2F69 and 0xC2F18).** Both ends move by the side column; the end is held at the full
  bar's. The bar still grows as before.
- **Test.** `vk_loading_169`: the four loadings at 1920x1080, window shots looked at.

## 11. Condition Zero radio and the radio menus

- **Keybindings.** PC Controls keeps one key per action. A key bound to a row was kept by the row that *had* it, and
  the row just bound was cleared. Binding a CZ line to Z (Follow Me's default) silently left the CZ line unbound.
  - The user's settings show every radio row empty, including Follow Me's default.
  - Now the row just bound keeps the key: press-a-key shows the other row as None at once; Accept also prefers the
    changed row.
- **Radio menus.**
  - radio1/2/3 asked the server for CS 1.6's text menu (ShowMenu with "\w" colour codes). This game's client draws it
    as overlapping copies of its lines: the "radio text drawn multiple times".
  - The menus are now in-game screens in the title's style (InGame_PCRadio1..3).
  - Each lists the game's commands and the Condition Zero lines, for mouse, keys or controller. A row sends its
    command and closes the menu.
  - The menus open only for this game's own player, never for a client's command on a listen server, and do not pause
    a single-player match.
- **Older servers.** Fail gracefully as before: CZ lines are not sent to a server that did not announce them.
- **Test.** `vk_buy_radio`.

## 12. Profiles not saving

- **Cause.** `run_game.sh` deleted the run's whole hard disk (`hdd`) at every launch, and with it the E: partition
  that holds the title's profiles (E:\UDATA, E:\TDATA\...\CurrentProfile.dat).
  - The PC settings and the party's PC profiles (`profiles/`) were never deleted.
- **Fix.**
  - run_game.sh keeps E: and clears the rest of the hard disk as before.
  - `pc_settings.ini` is now written whole or not at all (a temporary file renamed over it). It was truncated and
    rewritten in place: a crash mid-save lost every setting. The party profiles already were.
- **Split-screen players.** Their settings are their PC profiles (`profiles/`, shared by the party), never deleted;
  their own hard disks only hold the match's launch data.
- **Test tools.** `tools/drive.py --keep` relaunches a run folder keeping E:, the PC settings and profiles;
  `run_window_tests.py` takes `--keep` and `run: NAME` from a scenario's header.
- **Tests.** `vk_profile_persist_a` / `_b`: a profile made by keyboard, a setting changed, the game restarted; the
  title starts on the main menu with "Profile: Ace" and the setting kept.

## 13. Keyboard entry of profile names

- **Typing.** On Create Profile and Rename Profile the first character key starts typing, with no click.
  - Until then Space, Enter and Escape keep their menu actions (the on-screen keyboard's A, Done, back), so a
    controller or the keys still work the on-screen keyboard.
- **Editing.** Backspace, Delete, Left, Right, Home and End; Enter confirms (Done); Escape cancels the screen.
  - SDL text input gives the layout's characters, Unicode included: Å, ö.
- **Validation.** The game's own: maximum length (extra characters refused with its error sound), and its "already
  exists" and "bad name" screens.
- **Saved.** At once, by the title.
- **Tests.** `regress_pc_profile_name` (Home, Delete, End), `vk_profile_persist_a` (a key starts typing, Escape
  cancels, a duplicate name gives the title's "already exists").

## 14. Related items

- **Radio text drawn several times.** Fixed with the radio menus (11).
- **Deathmatch timer, corrected 2026-10-31.** Its first version broke Deathmatch: nobody could walk. The round's
  length it set also replaced the freeze time's, so the freeze at the first round's start lasted the match's hour.
  In the user's local-server logs: no "Round_Start", no kill. It now waits for the rules' own m_bFreezePeriod
  (+0x04) to clear.
- **Deathmatch timer at 0:00.** The HUD's timer shows the match's time left: the round's length (rules +0x38) set to
  end with mp_timelimit once the round proper runs, the players sent the game's own timer message (0x11B780).
- **Death camera clipping, CS 1.6 model textures, spray logos.** Not changed in this pass (see the report).
