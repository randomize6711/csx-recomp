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
- **No buy menu by itself at a respawn.** The buy menu opens on the client when the server answers impulse 53
  (0x120D22, SrtBuyMenu). Requests within 1 s of a mode's respawn are refused there (hook `PcModesBuyMenuRequest`);
  the player's own buy key (B, or the menu's Buy) works after that, and the ammunition keys as before.
- **Loadout.** Each purchase (impulses 20..52, hooks 0x120D07 / 0x120D0E: the weapon bits before and after) records the
  primary and the pistol bought, per player, bots too. At each respawn, 0.6 s after it (see the limitation below),
  the player gets the knife, the pistol bought (else the team's: USP or Glock) and the primary bought. A weapon picked
  up from the ground never enters the loadout (only purchases are seen). A team change clears it. Not in Gun Game.
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
- Money is set to $0 at each spawn, so nothing can be bought, and the buy menu never opens (impulse 53 refused).
  The level's weapon is given at the respawn, with no menu.
- **Kills are checked by the weapon the server names for them.** The rules' DeathNotice (0x149740, hook 0x149825)
  names the killer, the victim and the weapon (the weapon in hand for a direct kill, "grenade" for an HE grenade).
  The kills are queued and handled in the next rules Think, each on its own and in order (several in one frame each
  count; a win stops the rest). A kill counts only if its weapon is the level's (`pcmodes::LevelKill`,
  src/game/pc_modes_rules.h):
  - a picked-up weapon's kill, a grenade kill, a team kill, a suicide and a death by the world count for nothing
    (logged: "does not count");
  - on the knife level only a knife kill wins: a gun kill never does.
  Frags are no longer used for levels.
- A knife kill at the knife level wins. Everyone sees "NAME wins Gun Game!" and the match ends.
- Players see their level as a centre text at each spawn and level-up.

### Unlimited reserve ammunition (both modes)

Four times a second, each living player's firearms (primary and pistol, `pcmodes::UnlimitedReserve`) have their
ammunition's reserve (player +0x620, m_rgAmmo) set to its maximum. The magazine is untouched: firing and reloading
work as ever. Grenades, the bomb and the knife are excluded. Humans and bots alike. Classic is unchanged. The ammunition
indexes are learnt from GiveAmmo's return values.

## Limitations (documented, not hidden)
- **The respawn's late strip.** Just after a mode's respawn, the game strips the player once more (the suit too) and
  gives back the default pistol; the cause is not found. The respawn's equipment (suit, Gun Game weapons, Deathmatch
  loadout) is therefore given once, 0.6 s after the spawn, rather than re-applied whenever it is lost.
- **The automatic buy menu** is refused on the server by its timing (1 s after a respawn); where the client's request
  comes from is not pinned down. A buy key pressed in that first second is refused too.

- **Objectives still play out but end no round.** A planted bomb explodes and hostages can be rescued. The bomb is
  given only at a round start, so in practice once per match.
- **The HUD's round timer** counts down to 0:00 and stays there. The server ignores it.
- **No free-for-all.** Deathmatch is team deathmatch.
- **Gun Game details:**
  - Weapons dropped by dead players can be picked up; their kills do not count for a level.
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
- `tests/pc_modes_rules_test.py` (no game): Gun Game's kill rule, the weapons' slots, unlimited reserve's weapons.
- A Gun Game win, spawn protection's glow and the lobby/Custom Game rows are on the manual checklist.
