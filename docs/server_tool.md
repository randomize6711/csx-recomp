# Dedicated Server Tool (csx-server-tool)

A desktop application that runs and administers the real dedicated server (`csx-server`, docs/pc_multiplayer.md
section 16), styled after the 2003-2006 Steam client's olive-green tools. Branch `server-tool`.

## M1: investigation and architecture (2026-11-08)

### What already exists

| Part | State before this work |
|---|---|
| `csx-server` (N2) | A headless executable built from the same recompiled image (`tools/build_server.sh`). GoldSrc's dedicated mode: no window, GPU, audio device or input. Links libc, libstdc++, libm and libsodium only. 65 universal slots for remote players and bots, a fixed 30 Hz tick, the PC transport (ChaCha20-Poly1305, Ed25519 server identity, TOFU on clients), LAN-only, password, server query for the browser, master registration, map rotation, game modes. Map changes re-exec the process (same PID, same pipes). |
| Console | stdin lines run as engine commands. `admin COMMAND` on stdin runs as the owner with every permission (server_admin.cpp, `cl 0`). |
| Admin commands | status, say, kick, ban, unban, map, restart, bots N / bot_difficulty N (from the next map), friendly_fire, round_time, password. Validated, logged to logs/admin.log, rate limited for remote admins, rcon refused. |
| Settings | `server_config.cpp`: 24 keys with ranges, a pure parser (no game code), the match's command line. |
| Logs | `logs/server_NNN.log`, the last 30 runs kept (the SDK's logx). Every line also on stderr. |
| Tests | 13 dedicated scenarios in tests/net (`tools/run_net_tests.py`): clients, 65 slots, map change, admin, lifecycle, lossy network, fuzzing. |
| Old GUI | `tools/csx_server_gui.py`: a local web page (Python's http.server) with presets, start/stop, console and admin commands. No telemetry: it shows log tails. |
| Windows | `csx-server` is Linux only (POSIX calls in server_main.cpp; the relaunch). The game itself cross-builds for Windows (MinGW + SDL3 + Vulkan). |

What is missing for a management application:
- **Structured live data.** `admin status` gives names, addresses and keys only. There is no team, score, deaths,
  ping, tick rate, traffic or uptime in machine-readable form.
- **Live rotation changes.** The rotation is only set at a match's start (`+pc_rotation`).
- **admin.log** grows without a limit.

### Decisions

1. **Two processes.** `csx-server` stays the dedicated server. `csx-server-tool` is a separate GUI that starts it as a
   child process. A crash of either does not take the other down.
2. **IPC: the child's stdin and stdout/stderr pipes.** The tool is the server's parent and the only holder of those
   pipes, so the channel is local and authenticated by construction: no port is opened, nothing can connect to it.
   - Commands: lines on stdin (engine commands and `admin ...`, the owner's commands, which existed already).
   - Telemetry: with `CSX_SERVER_TELEMETRY=1` the server writes one line a second on **stdout**:
     `@csx status {JSON}` (format below). Log lines stay on stderr. Without the variable nothing changes.
   - Map changes re-exec the server in place, so the pipes and the PID survive them.
3. **GUI framework: Dear ImGui (v1.91.0, as the SDK fetches it) on SDL3 with SDL_Renderer.** SDL3 is already the
   project's platform layer on Linux and Windows, so the same code builds natively on Linux and cross-builds for
   Windows with the existing MinGW toolchain and deps. No Electron, no Python, no Qt. One small executable. SDL3 also
   provides the child process API with pipes (SDL_CreateProcessWithProperties), the tray icon, file dialogs and
   opening folders on both systems. The classic look is drawn with ImGui's style colours plus hand-drawn bevels.
4. **The tool reuses `server_config.cpp`** (pure C++, no game code) for the keys, their ranges and validation, so the
   GUI can never offer a value the server would refuse.
5. **No fake values.** Everything shown comes from the telemetry, the process (CPU and memory from the OS by PID) or
   files. A value that is not available is shown as "n/a".
6. **Portable layout.** The tool keeps its data next to itself (`csx-server-tool-data/`) when that folder is
   writable, otherwise in the user's config folder. Profiles are server.cfg files. Each profile has its own server
   folder (identity key, admins.cfg, bans.txt, logs).

### Limits found (not invented around)

- **Bots are set at a match's start** (the title's own rule, docs/pc_multiplayer.md 16.9). Bot count and difficulty
  apply from the next map. The tool offers "Apply now" which reloads the current map with them. There is no live
  "add one bot" in this title.
- **No per-team bot counts** (no setting exists for it in this build; TODO 1e).
- **Moving a player to another team** has no server command in this title. Not offered.
- **Tick rate** is fixed at 30 Hz (16.6). It is shown measured, not editable.
- **Public discovery** exists only through user-hosted master servers (`master` setting, N3). There is no central
  list. The "Public" option is that setting.
- **Windows server:** `csx-server` has never been built for Windows. See M5.
- The mockup the brief mentions was not attached to the request and could not be found on disk. The theme is based
  on the well-known 2004 Steam palette (olive greens, grey-green panels, gold selection text).

### Telemetry format (`@csx status`, version 1)

One line a second on stdout, once the engine has started:

```
@csx status {"v":1,"state":"running","time":1762600000,"uptime":125.4,"map":"de_dust2","next_map":"de_aztec",
 "frames":3762,"tick_hz":30.0,"frame_ms":0.9,"frame_ms_max":2.1,"max_players":65,"humans":1,"bots":10,
 "port":27015,"key":"<64 hex>","password":false,"lan_only":false,"game_mode":"classic",
 "net":{"connections":1,"in_bps":1520,"out_bps":2310,"in_total":182000,"out_total":277000},
 "players":[{"slot":1,"name":"Player","team":"CT","score":3,"deaths":1,"ping":24,"bot":false,
            "address":"192.168.1.5:27005","key":"<64 hex>","seconds":95}]}
```

- `state`: `loading` until the map is active, then `running`.
- `uptime`: seconds since the server was started, across map changes (the first start's time is kept in the
  environment that the relaunch inherits).
- `team`: `T`, `CT`, `SPEC` or `-` (unassigned), from CBasePlayer +0x1E0. `score` is entvars.frags (+0x164 of pev)
  and `deaths` CBasePlayer +0x724, the values the game's own ScoreInfo message sends (0x1489BC..0x1489ED).
- `ping` in milliseconds for network players, `null` for bots.
- `net` counters are the server endpoint's transport statistics.

## The application (M3, M4)

`csx-server-tool` (server_tool/): Dear ImGui 1.91.0 on SDL3 with SDL_Renderer. One window, a menu bar (File, Server,
View, Tools, Help), a sidebar (Dedicated Servers > Counter-Strike Xbox Dedicated Server with the profile; Server
Management > the pages), the page, a status bar.

| Page | What it does (all from the telemetry, the OS or the files; nothing simulated) |
|---|---|
| Overview | Start / Stop / Restart; status (Offline, Starting with its stage, Online, Stopping, Error with the reason), uptime, map, players, bots, PID. Server information: name, this machine's IPv4 addresses, port, current and next map, slots, LAN only, public listing (master servers), password. Statistics: CPU (% of one core) and memory from the OS by PID, tick rate and frame time, traffic in and out, players; four graphs of the last 10 minutes. A banner asks for the game folder when it is missing or wrong. |
| Configuration | General (name, password, max players 1-65, default map from the scanned maps, port, LAN only, master servers), Gameplay (mode, round, freeze and buy time, start money, friendly fire, collision, flashlights, gravity, max rounds, time limit, frag limit, Gun Game ladder), Advanced (snapshot rate, bots, rotation, crash restart policy, logging). Every value goes through `server::Set` (server_config.cpp), so the tool cannot save a value the server refuses. Each setting is marked [live], [next map] or [restart]. A description box explains the setting under the pointer. Save Changes / Revert. |
| Maps | The game folder's maps plus playable imported maps (never a fixed list). In Rotation checkboxes, the rotation in order (Move Up, Move Down, Remove, Clear), saved rotations (DATA/rotations), Set as Starting Map, Change Map Now (with a confirmation). Current and next map from the server. |
| Players | The live table (slot, name, team, score, deaths, ping, type, time; sortable), the selected player's address, ping, identity key (Copy Key), Kick with a reason, Ban (5 min to permanent), Unban by key or IP, Say. Bots are not kicked here (the Bots page). |
| Bots | Bots and humans in the match, the quota (Add Bot, Remove Bot, Remove All, a slider to the slot count), difficulty, Apply Now (reloads the current map with them). What the game cannot do is listed as not available. |
| Console | The server's output (wall-clock time, colours by severity, the tool's own lines in blue), search, filters, Copy Console, Clear Console, auto-scroll (pauses when scrolled up, resumes at the end), right-click Copy Line, a command line with history (Up/Down). 50 000 lines kept; the filtered view is rebuilt only when lines arrive or a filter changes, and drawn with a clipper, so a busy server stays cheap. |
| Logs | This session (live) and every log file of the profile's server (server_NNN.log, admin.log): severity filters, search, Copy, Export (save dialog), Clear, Reload, Open Logs Folder. A file's lines get wall-clock times from the "server: wall clock" line the server now writes at each start. |

Dialogs: confirm Stop (says how many players are connected), Exit with a running server (Stop and Exit, Minimize to
Tray, Cancel), Change Map, the crash prompt (Restart Server, Show Console, Close), New / Duplicate / Delete Profile,
Validate Game Files, About. File pickers are SDL's (portal / zenity on Linux, the system's on Windows); without one,
the tool falls back to the path field (game folder) or DATA/exports (exports).

Quality of life: the window's size, position, maximized state, the page and the Configuration tab are remembered;
profiles; import and export of a server.cfg; Start Minimized; a tray icon (status, Show, Start/Stop, Exit) and Close to
Tray where the system has a tray; Confirm Before Stopping; crash detection with an optional bounded restart policy.

### The look

The palette is the 2004 Steam client's (olive #4A5642 window, #3E4636 panels, #96892F selection, gold #C4B550
headings, light text #DEE4D6), square corners, etched group boxes (a sunken outer and raised inner one-pixel bevel),
raised buttons that sink when pressed, tabs with a gold line on the selected one, a near-black console with muted green
text. Fonts: Tahoma (Windows), else DejaVu Sans / Liberation Sans / Noto Sans (Linux), else ImGui's own pixel font; the
console in a monospaced font. The application calls itself "Counter-Strike Xbox Dedicated Server" and does not use
Steam's name, logo or artwork.

### Files (DATA)

DATA is `csx-server-tool-data/` beside the tool (beside bin/ in a package) when writable, else the per-user folder
(`~/.local/share/csx-recomp/csx-server-tool/`, `%APPDATA%\csx-recomp\csx-server-tool\`); `--data DIR` chooses another.

    DATA/tool.ini                      window, page, game folder, server program, profile, preferences
    DATA/profiles/NAME/server.cfg      csx-server's own format, owner-only (the password); server.cfg.bak the previous one
    DATA/profiles/NAME/tool.ini        crash policy
    DATA/profiles/NAME/server/         the server's folder: logs/, server_identity.key, admins.cfg, bans.txt, hdd/
    DATA/rotations/NAME.txt            saved rotations

Every write is a new file renamed over the old one.

### The IPC

The tool starts `csx-server --game GAME --config DATA/profiles/NAME/server.cfg` in the profile's server folder with
`CSX_SERVER_TELEMETRY=1`. stdin: one command a line (engine commands, `admin ...` as the owner, `quit`). stdout
and stderr (merged): the log lines and one `@csx status {JSON}` line a second. Nothing listens on a port for the tool;
only the parent holds the pipes. Stop: `quit` (the players are told), SIGTERM after 15 s, a kill after 30 s. A map
change re-executes the server in place (same PID, same pipes); the tool follows it from the log ("net: relaunching
into", "server: ready").

## Platforms and packaging (M5)

| | Linux | Windows |
|---|---|---|
| csx-server-tool | built, tested | cross-built (MinGW, static runtimes, SDL3.dll); unit tests and screenshots under Wine; not run on a real Windows PC |
| csx-server | built, tested | **not built** (below) |
| Package | `tools/build_server_tool.sh package`: build/dist/csx-dedicated-server-linux-x86_64.tar.gz | none yet |

**Why there is no Windows server yet.** csx-server's main uses POSIX calls (setenv, sigaction, poll on stdin, the
watchdog's backtrace, getppid), and the SDK's relaunch on Windows starts a new process with CreateProcess without
inheriting handles and ends the old one: the tool would lose the server's pipes and its process at every map change.
A Windows server needs (1) the POSIX parts of server_main.cpp ported, (2) a csx-server target in cmake/windows, and (3)
either a relaunch that keeps the console handles and lets the tool follow the new process, or the first process
staying alive as the parent of its relaunches. That is engine-side work that could only be tested under Wine here,
so it was not started in this milestone.

## Tests (M6)

| Test | What |
|---|---|
| `build/server_tool/csx-server-tool-tests` (58 checks; 57 under Wine) | JSON, telemetry lines, logs, crash policy, SHA-256, settings and profiles (owner-only, backups), rotations, map scanning, game folder checks, how settings apply |
| `python3 -I tests/server_tool_test.py` | the unit tests, then tests/server_tool/*.txt through `--script` against the real csx-server in a private network namespace: lifecycle, crash, refused, repeat |
| `tests/net/net_tool_human.txt` (run_net_tests.py) | the tool with a real game client: the player in the telemetry (ping, address, key), the tool's Kick |
| `tests/net/net_ded_telemetry.txt` + `tests/server_telemetry_check.py` | the telemetry with a remote client, every line validated |
| `tests/server_config_test.py` | the server's settings; two new checks for the relaunch's none/default |

## Verified and not verified

Verified on this machine (Linux, one PC, loopback and network namespaces):
- The server starts, runs, changes maps (admin map, through the tool), keeps its PID and pipes over relaunches, and
  stops cleanly (exit 0) from the tool, five times in a row and with restarts.
- Telemetry with bots and with a real remote player (ping, address, key, teams, score, deaths, traffic).
- Live rotation, bots from the next map, Apply Now's map reload, say, kick, admin status from the tool.
- Crash detection, two automatic restarts with doubling delays, giving up after the limit.
- Refused starts (missing game folder, missing map, rotation map missing, bots over the slots).
- The GUI's pages rendered offscreen with a live server (Overview, Players, Console, Bots, Maps, Configuration).
- The package extracted outside the workspace runs the server through its launcher with its own libraries.

Not verified:
- **Internet play** through the tool (only loopback in a namespace; LAN and internet hosting use the same server as
  before, whose own LAN tests are in docs/pc_multiplayer.md).
- **Interactive GUI use by a person** (mouse and keyboard on a real desktop): only offscreen rendering and the script
  mode, which drives the same controller, were run. The tray icon and file pickers depend on the desktop and were not
  exercised.
- **Windows**: the tool only under Wine; no Windows server.
- Ban through the tool with a real player (kick was tested; ban uses the same server command, tested in
  net_ded_admin).
