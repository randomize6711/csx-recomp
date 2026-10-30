# N6C–N6E — Deathmatch, Gun Game and the mode registry

The code is in `src/game/pc_modes.cpp`, with hooks in `config/game.toml`. Everything runs on the server: csx-server, a
local server process, or a listen server. Network clients need nothing new, so a client of any version can play a mode
on our server.

## Choosing a mode

| Where | How |
|---|---|
| Custom Game (single player) | Match Options' last row, **Game Mode**: Classic, Deathmatch or Gun Game (`+pc_gamemode ...` on the match's command line, whatever Match Options is) |
| Party lobby (local / LAN) | Rules: **Game Mode** and **Frag Limit**, the last two rows; the lobby's summary line starts with the mode |
| csx-server | `game_mode classic\|deathmatch\|gungame`, `frag_limit N` in its config (`--help` lists them); a menu match line's own `pc_gamemode` wins |
| Console (server's, or the owner's routed to its local server) | `pc_gamemode MODE`, `pc_fraglimit N` |

The registry is the `kModes` table in pc_modes.cpp: `classic`, `deathmatch`, `gungame`. A mode stays in effect until
another is given. Every way of starting a match gives one.

## The game rules underneath

The game's rules object is CHalfLifeMultiplay. Its `Think` (0x147620) runs every server frame and is hooked at
0x147639, where the object is in `esi`.

| Change | How |
|---|---|
| No round is won or lost | `CheckWinConditions` (0x144E20) returns through its own early exit (0x144E44 → 0x145E43). All round endings go through it: team eliminations, the bomb exploding or being defused, hostages rescued. |
| The round's time running out does nothing | `Think` 0x147DC3 → 0x1482A8 |
| Respawn after 2.5 s | Condition: on a team (player +0x1E0), joined (+0x1FC == 0, as `ClientKill` checks), and `deadflag` (entvars +0x170) ≥ DEAD_DEAD. The player's `Spawn` (vtable +0) is then called directly: the team's spawn points and default items, as at a round start. Applies to bots too. |
| Spawn protection, 2 s | `takedamage` 0 plus a glow in the team's colour (`renderfx` 19 glow shell); both are restored after |
| Match end | Rules +0x2BC set to now, the game's own "end in a moment": `Think` calls `GoToIntermission` (vtable +0x110) after its delay, then the next map |
| Messages | The game's TextMsg (HUD_PRINTCENTER) via the engine's MessageBegin/WriteByte/WriteString/MessageEnd |

### Deathmatch

- Each spawn gives $16000 (`AddAccount` 0x11B710) and restarts the buy time: the rules' round start (+0x30) is set to
  now, and the game measures `mp_buytime` from it. A player can therefore buy in the spawn's buy zone after every
  death; autobuy and rebuy (F3/F4, N5C) work as usual.
- The match ends at `mp_timelimit` (Think's own check) or when a player reaches the **frag limit** (`pc_fraglimit`, 0
  for none).
- Teams are kept (team deathmatch). Friendly fire follows `mp_friendlyfire`.

### Gun Game

Ladder of 23 levels:

| Group | Weapons |
|---|---|
| Pistols | Glock, USP, P228, Desert Eagle, Five-SeveN, Dual Elites |
| Shotguns | M3, XM1014 |
| SMGs | TMP, MAC-10, MP5, UMP45, P90 |
| Rifles | Galil, FAMAS, AK-47, M4A1, SG552, AUG |
| Snipers and MG | Scout, AWP, M249 |
| Last | Knife |

- Each spawn and each level-up strips the player (`RemoveAllItems` 0x118DB0, keeping the suit). The player then gets
  the knife and the level's weapon (`GiveNamedItem` 0x120720) with full ammunition (`GiveAmmo`, vtable +0x70). Weapon
  names are the game's own strings, checked before use.
- Money is set to $0 at each spawn, so nothing can be bought.
- A kill is detected as the player's frags (entvars +0x164) going up, and moves the player up one level. A team kill
  or suicide loses a frag but no level.
- A kill at the knife level wins. Everyone sees "NAME wins Gun Game!" and the match ends.
- Players see their level as a centre text at each spawn and level-up.

## Limitations (documented, not hidden)

- **Objectives still play out but end no round.** A planted bomb explodes and hostages can be rescued. The bomb is
  given only at a round start, so in practice once per match.
- **The HUD's round timer** counts down to 0:00 and stays there. The server ignores it.
- **No free-for-all.** Deathmatch is team deathmatch.
- **Gun Game details:**
  - Weapons dropped by dead players can be picked up.
  - The HE-grenade level is left out, because re-giving grenades would leak weapon entities.
  - Bots use what they are given but do not know the ladder.
- **Mid-match changes.** A mode set in the middle of a match takes effect at once. Players already alive keep their
  items until they next spawn (Gun Game arms them at once).
- **Maps with no objective** (aim_, fy_ maps) are still refused by the map importer (N6B report "problems"). Allowing
  them for Deathmatch and Gun Game is future work.

## Tests

- `tests/scenarios/regress_mode_deathmatch.txt` (headless, fixture `tests/launch/de_aztec_dm`):
  - respawns happen and no round ends past the 1-minute round time;
  - kills keep happening;
  - `pc_fraglimit 2` typed at the console ends the match and the next map loads.
- `tests/scenarios/regress_mode_gungame.txt` (headless, `tests/launch/de_aztec_gungame`): levels are reached, respawns
  happen, no round ends, and every ladder weapon name resolves.
- A Gun Game win, spawn protection's glow and the lobby/Custom Game rows are on the manual checklist.
