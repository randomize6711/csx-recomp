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

The profile the front end creates (E:\UDATA, E:\TDATA) is not needed for the match. Started this way, the player spawns
at the same position as after the full boot (regress_player_control's 765.1 unit run reproduced exactly).
