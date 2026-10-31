# Report: post-N11 correctness and UI repair pass (2026-10-31)

Branch g8.2b, not merged. Linux. Details of each cause and fix: docs/bugfix_pass.md.

## Playable build

- `./run_game.sh` starts `build/window/CounterStrike` from the intro movies.
- The build matches commit fac8bf7, and so does `build/server/csx-server`.
- Your profiles survive from this launch on. The launches before this build had already deleted them, so create one
  once more.

## Per issue

| # | Issue | Root cause (short) | Commit | Test evidence |
|---|---|---|---|---|
| 1 | Gameplay menu not clickable | Gameplay and Mods were classed as server lists, where a click only selects | 754d273 | vk_settings_click |
| 2, 3 | Video and Advanced Graphics layout | Value column sized for the console's short values; panel text counted in characters, not font widths | 754d273 | Shots at 1080p, 4K, 21:9 and 4:3; the graphics settings' own tests |
| 4 | Xbox Live black screen | `!enterLive` sign-in with no service | 397b80f | vk_online_play |
| 5 | Prompts in Custom Match | Unknown legend words (Configure, ...) and only 3-button strips were converted | 24b1044 | vk_prompts_legends, vk_prompts |
| 6 | Single-player pause | Server paused, clients never told (no svc_setpause), so their clocks ran on | 3ce3954 | vk_local_server: the client clock moves 0.06 s across a 7.4 s pause |
| 7 | DM respawn HUD and knife | The game strips the suit just after our respawn, so the HUD hides health, ammo and crosshair, and the knife is gone | 20e227c | vk_dm_respawn (shots ×3) |
| 8 | DM economy and cleanup | Buy time was 1.5 min from each spawn; no weapon cleanup | bf44f5f | regress_dm_economy, regress_mode_deathmatch |
| 9 | Fullscreen flicker on quit | Each relaunch's new window opened windowed, then went fullscreen | c35323a | vk_relaunch_window, vk_relaunch_windowed |
| 10 | Loading bar outside frame | On wide screens the fill was not moved with its frame; the fraction could pass 1 | fb2507e | vk_loading_169, vk_loading |
| 11 | CZ radio does nothing | (a) binding a key already used left the new row unbound; (b) the radio text menu was garbled | 397b80f, fac8bf7 | vk_buy_radio, regress_pc_bind_capture, vk_bind_capture |
| 12 | Profiles not saving | `run_game.sh` deleted the hard disk (E:, the title's profiles) at every launch | b1da1a6 | vk_profile_persist_a/_b ("Profile: Ace" after the restart) |
| 13 | Keyboard profile names | Typing needed a click on the field; no Delete, Home or End | 5a27b45 | regress_pc_profile_name, vk_profile_persist_a |
| 14 | DM timer 0:00; radio text duplicated | Rounds never end in DM; text menu unsupported by the client | bf44f5f, 397b80f | regress_dm_economy, vk_dm_respawn shots (59:xx), vk_buy_radio |

Details of what changed for each issue (beyond the table):

- **1.** Clicks work on every PC settings screen.
- **2, 3.**
  - New column widths, text measured by the font's widths, and shorter values.
  - The panel ends with "(more in Details)" when its text doesn't fit.
  - Font size unchanged; no setting removed.
- **4.**
  - Xbox Live is now **Online Play** and opens the existing online menu. Back returns to the main menu.
  - The Live status lines are hidden, and the pause menus' Friends List is now Copy Invite Code.
- **5.**
  - Ten new words, up to 5 buttons laid out by their text widths.
  - Help-bar clicks use the same layout.
  - In the Keyboard/Mouse style, White and Black show as keys 1 and 2.
- **6.**
  - The server now sends svc_setpause, as the engine's own pause does.
  - Split-screen and network matches are not paused.
  - A split-screen player's menu no longer floods the server with "pause".
- **7.** The suit and knife (Gun Game: the level's weapons) are put back within 1.5 s of the respawn, for bots too.
- **8.**
  - mp_buytime is 99999 during Deathmatch.
  - Money is set to $16000 at every spawn.
  - Weapon boxes go after 20 s through their own Kill think (`pc_weapon_stay N`); the bomb's backpack stays.
- **9.** The window state rides in the environment across the exec, so the new window opens fullscreen at once, or at
  the last window's size.
- **10.** Both ends of the bar are mapped into the frame, and the end is clamped at the full bar.
- **11.**
  - The row you just bind now keeps the key.
  - Radio 1/2/3 are now title-style screens that include the CZ lines and do not pause the match.
- **12.** The E: partition is kept, and `pc_settings.ini` is written atomically.
- **13.**
  - The first letter key starts typing.
  - Backspace, Delete, Home, End and the arrows edit; Enter confirms; Esc cancels.
  - The controller's on-screen keyboard still works.
- **14.**
  - The timer shows the match's time left.
  - The radio menus replace the garbled text menu.

## Test runs (final build, targeted only)

- **Window (24/24 pass):**
  - New: vk_settings_click, vk_prompts_legends, vk_online_play, vk_profile_persist_a, vk_profile_persist_b,
    vk_dm_respawn, vk_relaunch_window, vk_relaunch_windowed, vk_loading_169.
  - Touched areas: vk_prompts, vk_buy_radio, vk_loading, vk_local_server, vk_bind_capture, vk_menu_mouse_fe,
    vk_menu_mouse, vk_auto_exposure, vk_bloom_43, vk_vsync, vk_vsync_play, vk_xbox_effects_169, vk_display_mode.
  - Smoke tests: vk_custom_gamemode, vk_split_2.
  - vk_buy_radio was run again after the last cosmetic change: pass.
- **Headless (7/7 pass):** regress_pc_profile_name, regress_pc_bind_capture, regress_dm_economy,
  regress_mode_deathmatch, regress_mode_gungame, regress_pc_menu_mouse, regress_pc_options.
- **Not run:** no comprehensive regression and no performance work, as instructed.
- **No physical controller was used:** controller paths were tested with virtual devices only.

## Manual verification still needed

1. Hyprland, Display Mode Fullscreen: start a match and quit it. The screen should not drop to a window. gamescope
   cannot show SDL's fullscreen state.
2. 16:9 desktop: the loading bar grows inside its frame.
3. Profiles: create one, change settings, quit, start `./run_game.sh` again. Then Change Profile, make a second one,
   switch between them, restart.
4. Create Profile with the keyboard: type at once, use Home, End and Delete, then Enter. Also try Esc, a duplicate
   name and a too-long name.
5. Custom Match and Change Profile prompts in Automatic mode with a real controller: plug one in and switch between
   it and the keyboard.
6. Pause alone in single player: bots, physics, timers and animations stop, and there is no jump on resume.
   Split-screen: the match goes on.
7. Deathmatch:
   - after several deaths, health, armour, ammo, the crosshair and the knife are there;
   - $16000 on every respawn;
   - buying works late in the match;
   - dropped guns vanish after about 20 s;
   - the timer counts the match down.
8. PC Controls: bind a CZ line to Z. Follow Me should show None at once. In a match the CZ line plays. Also use
   Radio 1/2/3, by mouse and with a controller.
9. Video and Advanced Graphics at your 4K desktop, judged by eye.

## Limitations and unresolved

- **Death camera clipping, CS 1.6 model textures, spray logos:** not changed. They aren't related to these fixes and
  aren't safe as small changes; they stay in the known list.
- **DM buying outside buy zones:** not implemented. It was optional; buying works at any time inside a buy zone.
- **Respawn fix (7):** puts back what the game strips, without removing the strip itself. In Gun Game the default
  pistol may also be in hand after a respawn.
- **Radio menus:** no number-key shortcuts. Use the mouse, arrows plus Space, or the controller.
- **Five legends keep the Xbox art:** they belong to Xbox Live or old system-link screens, none reachable.
- **Fullscreen at window creation on Hyprland (9):** unverified until play-tested. If Hyprland ignores it, the
  behaviour falls back to the previous one, with the flicker.

## Game and DLC files

- The DLC folder matches tests/dlc_sha256sums.txt.
- build/gamedata and the game folder are unchanged since the previous report (nothing newer).
- Runtime writes go to run folders only.
