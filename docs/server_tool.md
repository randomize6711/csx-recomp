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
