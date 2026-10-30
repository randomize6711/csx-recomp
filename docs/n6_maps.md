# N6A/N6B — Official downloadable maps and map import

Status: implemented. In-game verification of the imported map is listed under "Tests" with its result.

## N6A — Inferno and Office

The Xbox Live packages for these two maps hold no map data. They are unlock tokens (contentmeta "Inferno Map",
"Office Map"; see docs/n6_dlc.md). The maps, their previews (launcher.xpr's `de_inferno` and `cs_office`) and their
loading screens are already on the game disc.

The title lists them only when the packages are installed. Its Custom Game list and the pause menu's Change Map list
each hold 18 maps without them.

`src/game/pc_dlc.cpp` (`ExtendDlcMaps`) adds a row for each map whose archive is in the game folder:
- Inferno goes after Havana and Office goes after Militia, the title's alphabetical places.
- Each row is copied from an existing row of the same kind (Fastline's for a bomb map, Havana's for a hostage map), so
  the labels, layout and preview position match.

The party's map list (`pc_lobby.cpp`) and the map search (`pc_mapsearch.cpp`, N5B) read the title's list, so both show
the two maps as well. The DLC folder is never read at runtime. Its hashes are checked only by
`tests/dlc_sha256sums.txt`.

## N6B — Importing a GoldSrc map

```
python3 -I tools/import_map.py BSP --nav NAV --wad WAD_DIR --gfx GFX_DIR --game build/gamedata --out build/maps
```

**Source material.** The source files (a Counter-Strike 1.6 or Condition Zero map, its .nav file, its WADs and its sky
textures) are read only. The game folder is never written. The output is two files:
- `OUT/NAME.SXWad`, the archive;
- `OUT/NAME.json`, a report of what was found and whether the map is `"playable"`.

**What the importer does:**
- It checks the BSP: version 30, lumps in bounds, and the entities (spawn points of both teams, bomb targets or
  hostages and rescue zones, buy zones, the sky).
- It embeds every texture the map takes from WADs into the BSP, as a compiler's `-wadinclude` would. The engine's
  texture loader (`Mod_LoadTextures`, 0x49200) uses embedded pixels when a texture is not in the menus' registry.
- It copies the template archive's shared files (models, sounds, sprites, configs). It does not copy the template map's
  own BSP, nav, PAS, light config, textures, detail textures or loading screens.
- It merges the template's XPR with the map's six sky faces (`skies/<sky>bk.tga` and the others, swizzled A8R8G8B8).
- It adds the nav file (bots navigate only with one) and writes zlib entries, keeping the template's placeholder
  entries.

**Where imported maps live.** They stay outside the game folder: in `CSX_MAPS`, or by default in `maps/` beside the
build (`build/maps`). An empty `CSX_MAPS` means none; the tests use that by default (`tools/drive.py`).

**Overlay at startup.** When at least one map's report says it is playable, `src/common/map_imports.cpp`
(`GameDirWithImports`) builds an overlay folder, `RUN/gamedir`, at each start:
- It contains symlinks to each of the game folder's files and to each playable map's archive and report.
- If a map has the same name as one of the game's own, the game's map wins.
- A marker file, `.csx_overlay`, makes a relaunch reuse the same folder.

The window build (`window_main.cpp`) and csx-server (`server_main.cpp`, before the map existence check) both run from
the overlay.

**Menu rows.** `pc_dlc.cpp` lists each playable imported map at the end of the lists: before Random in Custom Game and
after the last map in Change Map. Its label comes from the file name (`de_sienna_cz` → "Sienna CZ") and it uses the
Random map's preview, since an imported map has no preview of its own.

## What a map from another game needs (found by playing Sienna CZ, 2026-10-30)

- **Player models.** This game picks a map's player (and hostage) models from `playermodels.txt`, one line per map.
  A map it does not find gets `models\player\default\CLASS.mdl`, which no archive has, and the client drops back
  to the menus. The importer therefore:
  - keeps the template's `models\player\TEMPLATE\*` (earlier they were left out with its other own files);
  - adds a line for the imported map with the template's models.

  The template is `de_dust2` for a bomb map and `cs_italy` for a hostage map (its hostage model).
- **Models and sprites the map's entities name** (props, decorations) are taken from the source's folders (the
  BSP's `..`, `--wad`, `--gfx`). One found nowhere makes the map unplayable (the engine stops at a missing model),
  unless `--drop-missing-models` leaves those entities out (noted in the report). Sienna CZ's eight fern props
  (`models/pi_fern.mdl`) are in no installed folder: left out.
- **Entities are parsed with quotes respected.** A decal's texture name such as `{siteA` holds a brace; splitting on
  braces corrupted the entity text and the server hung loading it.
- **The navigation file's BSP size.** A version 5 `.nav` records the size of the BSP it was made for, and the bots
  refuse it when the sizes differ. The importer sets it to the rebuilt BSP's size; the geometry is the same.

## Imported so far

| Map | Source | Report |
|---|---|---|
| de_sienna_cz | the user's own Condition Zero install (`czero/maps`, read only), `--drop-missing-models` | 16 CT / 16 T spawns, 2 bomb targets, 2 buy zones, 97 textures embedded, sky "sienna" (6 faces), nav present (size fixed), template de_dust2's player models, 8 fern props left out; playable: loads, bots join and fight (vk_import_map) |

Deleted Scenes (`czeror`) is never read (see ROADMAP.md).

## Tests

- `tests/scenarios/vk_import_map.txt` (window, `--env CSX_MAPS=$PWD/build/maps`): the overlay is logged, Sienna CZ is
  listed and searchable (22 maps), the match loads it, the bots join and play, and a window shot shows the world.
  **Passes.**
- `vk_map_search` (21 maps with Inferno and Office, no imports).

## Limitations

- Maps need a nav file for bots. Without one the report notes it and the map is still playable, but bots stand still.
- A map whose WADs are missing is not playable (its report lists the missing textures).
- Imported maps' detail textures and loading screens are not converted.
- The map's custom sounds are not imported (this game plays sounds from its wave banks).
- `materials\<map>.mat` (texture materials: footstep sounds) is not imported, so the map uses the defaults.
