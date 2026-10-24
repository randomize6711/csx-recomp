# Launch fixtures: straight into a match

The front end starts a match by rebooting the title (HalReturnToFirmware, quick reboot) after writing two things:

- the XAPI launch data page, which names the image (`\Device\CdRom0;default.xbe`); the host keeps it as
  `hdd/launch.bin` across the relaunch, and src/common/launch_capture.cpp copies it to `launch_seen.bin` in the run
  folder before boot consumes it;
- `Z:\releasemap.txt` on the cache partition, the match's console command line (`+map de_airstrip +bot_quota 7
  +bot_difficulty 0 ... +mp_startmoney 6000`). Without it the relaunched title opens the front end again (verified
  2026-10-19: launch.bin alone boots to FrontEnd.sxwad).

`tools/drive.py --launch DIR` puts `DIR/launch.bin` and `DIR/releasemap.txt` there before starting, so a scenario
begins at the map load (about 45 s headless to a moving player instead of about 2 minutes through the menus).

| Fixture | Made from | Contents |
|---|---|---|
| de_airstrip_beginner | regress_boot_to_spawn (Single Player, de_airstrip, Beginner defaults) | the launch page (only the image path and binary fields; no profile name) and releasemap.txt |
| de_aztec_2bots | de_aztec_nobots with "+bot_quota 2 +bot_join_team ct +mp_autoteambalance 0 +mp_limitteams 0" | two Counter-Terrorist bots against the player (the player collision tests; no collision setting: the title's own) |
| de_aztec_2bots_nocollide | de_aztec_2bots with "+pc_player_collision 0" at the end, as Custom Options > Player Collision Off gives it | the same, player collision Off |
| de_aztec_bots7, _bots11, _bots16, _bots24 | de_aztec_nobots with "+bot_quota N" (7, 11, 16, 24) and "+mp_roundtime 1 +mp_freezetime 3" | bot counts on de_aztec (16 / 18 spawn points): the original default, the original's most, the 17 slots full, more asked than the slots hold |
| de_dust2_bots16 | the same on de_dust2 with "+bot_quota 16" | 8 / 8 spawn points: the sixteenth bot is refused (team full) |
| de_aztec_bots64, _bots64_nocollide | de_aztec_bots16 with "+bot_quota 64" (and "+pc_player_collision 0" at the end) | 64 bots and the player: the 65 player slots full (G8.2B) |
| de_aztec_bots64_ct | de_aztec_bots64 with "+bot_join_team ct +mp_autoteambalance 0 +mp_limitteams 0 +teampreference 1" after the quota, as Bot Team > Counter-Terrorist gives it | 64 bots on one team against the player (G8.2B: mp_limitteams 0 allows 64) |
| de_dust2_bots64 | de_dust2_bots16 with "+bot_quota 64" | 64 bots on 8 / 8 spawn points (each point reused by four players) |
| de_dust2_bots16_nocollide | de_dust2_bots16 with "+pc_player_collision 0" at the end | the same, player collision Off |

The profile the front end creates (E:\UDATA, E:\TDATA) is not needed for the match. Started this way, the player spawns
at the same position as after the full boot (regress_player_control's 765.1 unit run reproduced exactly).
