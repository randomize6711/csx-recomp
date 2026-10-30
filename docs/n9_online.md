# N9 — Invitations, Steam/Discord and the dedicated-server GUI

## Invite codes (implemented)

`src/game/pc_invite.cpp` makes an invite code that works without any external service: `ADDRESS/KEY`, the server's
address and its 64-character identity key. For example:

```
192.168.1.20:27015/9f2c...e41a
```

**Making a code.** PC Controls > Copy Invite Code (F6, or the console's `pc_invite`) logs the code, prints it on the
console and copies it to the clipboard (the window takes it, `window_input.cpp`). Where it comes from:

| Match | Code |
|---|---|
| Joined through the server browser | The address and key that were joined (`CSX_BROWSER_JOIN`, `CSX_SERVER_KEY`) |
| Hosted with Host a Match (its local server) | This PC's network address (the first IPv4 address of an interface that is up, not loopback), the server's port and its key. The fourth word of `CSX_LOCAL_SERVER` marks a hosted server (`host`). |
| Offline (loopback only), or the in-game listen server (PC setting `server_process 0`) | No code; the reason is printed |

**Joining with a code.**
- Multiplayer > the server browser > Join by Address takes the code (typed, or pasted with Ctrl+V; the field takes
  up to 140 characters).
- The key is pinned as a favourite's is (`pc_browser.cpp` AddAddress), so a code cannot lead to a different server
  posing as the right one.
- The **password is not in the code**. It is told to friends separately, as before.
- Transport, encryption, identity checks and passwords are all unchanged.

**Reaching a server over the internet** also needs the host's port forwarded. The code carries the LAN address, so
friends outside the LAN replace the address part with the host's public address (documented limitation).

## Split-screen parties on remote servers

Already supported since N4.4 (docs/splitscreen.md §N4.4): player 1 joins any server (browser, direct address, now
also an invite code), and players 2–4 follow into their own slots of the same server. The server checks capacity
and its password like any other join. Nothing changed here.

## Steam invitations (blocked: external account)

Steam's invites and "Join game" need the Steamworks API running for a registered Steam App ID:
- `ISteamFriends::SetRichPresence("connect", ...)` and `InviteUserToGame`;
- the `GameRichPresenceJoinRequested_t` callback.

This game is not a Steam app. Using a sample App ID (480) or a Valve game's ID would misreport what the user plays.
The Steamworks SDK headers come from the partner site and need the user's Steam login. **Blocked** until the user
registers an App ID (a Steamworks partner account) and agrees to its terms.

Design ready for that point: load `libsteam_api.so` at runtime only when present and enabled (never required), set
`connect` rich presence to the invite code, and on a join request start the browser join with the code (key pinned).
Invite codes work in Steam chat meanwhile (paste the code).

## Discord invitations (blocked: external account)

The supported path is the Discord Social SDK (the Game SDK is deprecated and the RPC "join secret" flow is legacy).
It needs an application registered on Discord's developer portal (an application ID and, for the Social SDK, the
user's agreement to its terms).

**Blocked** until the user registers one. No credentials are embedded. Design: optional, loaded only when present;
activity with a party size and a join secret holding the invite code, answered with the same browser join. Invite
codes can be shared in Discord messages meanwhile.

## Dedicated-server GUI (implemented)

`tools/csx_server_gui.py` is optional, uses only Python's standard library and runs in the PC's web browser. It
listens on 127.0.0.1 only and prints an address with a secret token (new at each start). Every request without the
token gets 403.

| Feature | How |
|---|---|
| Presets | `HOME/presets/NAME.cfg`, csx-server's `--config` format, owner-only (0600). The password never goes back to the page; an empty field keeps it, and a checkbox clears it. |
| Start and stop | csx-server in `HOME/run/NAME` (its state folder) with `CSX_SERVER_STATUS`. Stop sends `quit` on the console, then SIGTERM, then SIGKILL. |
| Map, rotation, bots, mode, rules, password, LAN only, master servers | csx-server's keys, all of them (`tests/server_gui_test.py` checks the GUI's list against `server_config.cpp`). |
| Status | The status file (`ready PORT KEY`, `error ...`, `ended`) and the log's last lines (password arguments masked). |
| Players, kick, ban, unban, say, map, restart round | The server console's new `admin ...` (`server_admin.cpp`, as the owner with every permission; replies logged as `admin console:`). The player list is the last `admin status`. |
| Console | Any engine command, one line, no `;`. |

**Permissions.**
- Only the user who runs the GUI can reach it (localhost and the token).
- It reaches only the servers it started, through their console.
- Remote administration of a server still needs a key listed in `admins.cfg`; nothing about it changed.

**The headless server** runs exactly as before without the GUI.

### Map rotation (implemented, server side)

- csx-server `map_rotation a,b,c` (or `pc_rotation a,b,c` on the server's console).
- When a match ends, the game's ChangeLevel goes to the map after the current one. The hook is placed just before
  CHANGE_LEVEL, at 0x14A8BA. When the current map is not in the list, it goes to the list's first map.
- Without a rotation, the game behaves as before (`mapcycle.txt`, else the same map).

## Tests

- `tests/server_gui_test.py` uses a stand-in for csx-server, so no game binary runs. It covers:
  - the keys match csx-server's;
  - the server listens on 127.0.0.1 only;
  - requests without the token or with a wrong one are refused;
  - presets are owner-only and the password stays out of the page;
  - invalid names, quotes and `;` are refused;
  - start, status, admin status and the players list, and the console;
  - the password is masked in the log;
  - stop and delete.

  Result: **21 of 21 checks pass**.
- Invite codes, console admin, rotation: covered by the targeted runs listed in docs/autonomous_pass.md.
