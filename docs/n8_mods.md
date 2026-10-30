# N8 — Mods, skin packs and Half-Life compatibility

## Mod loader

Code: `src/common/mods.h/.cpp` (host side: manifests, resolution, overlay links), `src/game/pc_mods.cpp` (the
archive hooks), `src/common/map_imports.cpp` (the overlay), the Mods screen in `pc_menu.cpp`, and the converter
`tools/make_mod.py`.

### A mod

```
mods/ID/mod.ini
mods/ID/files/<the game's path, lower case>      e.g. files/models/v_ak47.mdl
```

`mod.ini` (lines `key = value`; `#` comments):

| Key | Meaning |
|---|---|
| `id` | Lower-case letters, digits and `_ - .`; must equal the folder's name |
| `name`, `author`, `description` | Shown in the Mods screen |
| `version` | Numbers and dots (`1.2.10` > `1.2.9`) |
| `kind` | `cosmetic` (this PC's looks only: weapon, player and sprite skins) or `content` (every player of a match must have it; checked) |
| `requires` | The mod interface version it needs. This build's is `0.8.0`. A mod needing a newer one is refused. |
| `depends` | `other_id`, `other_id >= 1.2` or `other_id = 1.2`, comma separated. Each must be enabled at a matching version. |
| `conflicts` | Same syntax. Such a mod enabled at the same time makes this one drop out. |

### What a mod may contain

Allowed: models (`.mdl`), sprites (`.spr`), textures (`.tga`, `.bmp`, `.wad`), menu artwork (`.xpr`), maps
(`maps/NAME.SXWad` with its `.json` report and `.nav`), and the game's string tables (`resource/*.txt`).

Refused: everything else. Configs (they run console commands), libraries, executables, scripts, links (they could
reach outside the mod), `..` paths and paths that are not lower case are all refused and logged.

**Nothing is ever downloaded.** A server never sends mods, so a remote server cannot run code or install anything.

### Enabling, order and profiles

- **Enabling.** PC Options > Mods lists every installed mod:
  - an On/Off row per mod (`mod.ID`);
  - a **Load Order** row per mod: First, Early, Normal, Late, Last (`mod_order.ID`: 10, 30, 50, 70, 90). A later
    mod's file wins.
  - **Looks:** Mods / Original Xbox (`mods_original`) switches every mod off at once without forgetting the choices.
- **Per profile.** The `mod.`, `mod_order.` and `mods_original` keys are profile-scoped (`profiles.cpp` Scoped), so
  each profile keeps its own mods.
- **When changes apply.** Mods apply at the game's start and at each title relaunch (a match's start), so a
  profile's choice takes effect with its next match.

### How the game sees a mod's files

1. **Resolution.** At start-up, `mods::Apply` scans the mods folder: `CSX_MODS`, else `mods/` beside the build
   (`build/mods`). It checks each enabled mod and resolves dependencies and conflicts, repeating until nothing more
   drops out. Every mod left out is logged with its reason.
2. **Overlay links.** The active mods' files are symlinked into the game's overlay folder (`RUN/gamedir`, the same
   overlay as imported maps) under `cstrike/`. A link is never made through a folder that is itself a link to the
   game's files, so **the game folder is never written**. A mod's map never replaces one of the game's own.
3. **Archive hooks.** The game reads its files from the current SXWad archive (`*0x552744`) and, for a name the
   archive does not hold, from the disk. `pc_mods.cpp` hooks the archive's "is it here" (0x1FC4F0 → answer at
   0x1FC5A8) and "open it" (0x1FC6B0, the lookup at 0x1FC79F → its not-found path 0x1FC7B0). For a name an active
   mod replaces, both answer "not here", and the game reads the linked loose file.

### Multiplayer compatibility

- **Cosmetic mods** change only this PC's drawing and are never checked. Hitboxes and damage come from the server's
  own models.
- **Content mods** are announced by a server at signon: `pc_server_mods "id@version:hash ..."`, where the hash is a
  BLAKE2b of the manifest and files.
  - A client whose own content mods differ logs both lists and disconnects.
  - `pc_server_mods` is on the stufftext allow list. A server without content mods sends `none`.
- A dedicated server enables mods through the `mod.ID` keys in the `pc_settings.ini` of its folder.

### CS 1.6 and Condition Zero skin packs (`tools/make_mod.py`)

```
python3 -I tools/make_mod.py SOURCE --id ID --name NAME --version 1.0 --game build/gamedata --out build/mods
```

`SOURCE` is a `cstrike/` or `czero/` style folder, or a skin pack's own folder. It is read only.

| Source file | What the converter does |
|---|---|
| Weapon models (`models/v_*.mdl`, `p_*`, `w_*`) | Copied to the same paths. These are GoldSrc studio models, version 10, the same format as this game's weapon models. |
| Player models (`models/player/CLASS/CLASS.mdl` and `CLASST.mdl`) | Copied to every map that uses the class (`models/player/MAP/CLASS.mdl`), because this game keeps a model set per map. CS 1.6's `urban` maps to `seal`. This game's own player models are version 11; version-10 replacements are loaded by the engine (see Tests). |
| Sprites (`sprites/*.spr`) | Copied. |
| Sounds | **Not taken.** This game plays its sounds from its XACT wave banks (`Media\Sound\*.xwb`, `.xsb`); the archives' `sound\*.wav` entries are empty placeholders. Sound mods need a wave-bank rebuild, not done in this pass. |

Condition Zero: Deleted Scenes (`czeror`) is refused by path.

## Tests

`tests/scenarios/vk_mods.txt` (window, `--env CSX_MODS=$PWD/build/mods --pc mod.cz_ak47_gign=1`) uses a mod made
from the user's own Condition Zero files: CZ's AK-47 view model and GIGN player model. It checks that:
- the mod is active (5 files);
- the AK-47 bought and held comes from the mod;
- window shots of the AK-47 and of the player in third person are taken for comparison by eye;
- there is no Host_Error.

## Half-Life compatibility (investigation; separate layers)

Nothing here is claimed as Half-Life compatibility. Status by layer:

| Layer | Status |
|---|---|
| 1. Map loading | Half-Life maps are GoldSrc BSP version 30, the same format this engine loads. `tools/import_map.py` packs one with its WAD textures. It refuses maps without Counter-Strike spawn points and objectives (a Half-Life map has `info_player_start` only). A Half-Life map could load as a walkable space; not attempted or tested. |
| 2. Asset conversion | Models (v10 `.mdl`), sprites and WAD textures load as they are (see the mod loader). Sounds need the wave-bank rebuild noted above. |
| 3. Entity support | The game DLL in default.xbe is Counter-Strike's. Half-Life's entities (monsters, `func_` logic beyond CS's, `scripted_sequence`, triggers to other maps) are not in it. Unknown classnames are removed by the engine at spawn. |
| 4. NPC AI | Not present. Half-Life's monsters would need a new game module. |
| 5. Weapons | Not present (Counter-Strike's weapons only). |
| 6. Gameplay systems | Single-player saves, level transitions (`trigger_changelevel` with landmarks), suit/HEV and health chargers: not present. |
| 7. Campaign scripting | Not present. |
| 8. Full-game compatibility | Not possible with this game's code. |

Design for a later pass: a separate game module (a second "game DLL" running on the same engine) built as an
isolated component. It would be selected by a launch mode, would never change the Counter-Strike module, and would
use legitimately supplied Half-Life content. This is a major architecture addition and is left for the user's
decision (ROADMAP.md).
