# N10 — HUD editor, HUD scale and HD artwork

## HUD layout and editor (`src/game/pc_hud_layout.cpp`)

**How the HUD is found.** The client's HUD draws through the engine's functions for it, the `cl_enginefuncs` table
at 0x371610:

| Function | Address |
|---|---|
| SPR_Draw | 0x22F70 |
| SPR_DrawHoles | 0x22FF0 |
| SPR_DrawAdditive | 0x23070 |
| FillRGBA | 0x476C0 |
| DrawCharacter | 0xB0F40 |
| DrawConsoleString | 0x47060 |

Each is hooked at its entry, where the return address names the HUD element drawing. The element map is
`src/game/pc_hud_elements.inc`: the client functions' address ranges, mapped from `CSX_LOG_HUDDRAW=1` runs, which log
every new caller with its coordinates.

**What is applied.**
- **Position.** An element's place is added to the coordinates on the stack:
  `x' = corner + (x − corner) × scale + offset`. The corner is the screen corner nearest the element's last bounds,
  so a corner element stays in its corner when resized.
- **Size.** For sprites, the quad is scaled after the engine's clipping (0x47508 in the sprite draw 0x47470: width at
  [esp+0x20], height at [esp+0x24]). For filled boxes, the width and height arguments are scaled. Text characters
  move but keep the font's size.

**Settings.** All are profile-scoped (`profiles.cpp` Scoped).

| Setting | Use |
|---|---|
| `hud.KEY = DX DY SCALE` | A full screen |
| `hud_split.KEY = DX DY SCALE` | A split-screen player's view. The compositor copies player 1's to the other players, and a player's own profile still wins. |
| `hud_scale` / `hud_scale_split` | PC Options > HUD and Menus > HUD Scale (75–150%), applied on top of every element's own scale |

**The editor.** Opened in a match with PC Controls > Edit HUD Layout (F7) or `pc_hud_edit`. The chosen element is
outlined (FillRGBA at SCR_DrawNetGraph's entry, 0x6E4B0, drawn in 2D after the HUD each frame), and its name and
place appear as a centre text.

| Action | Keyboard | Controller |
|---|---|---|
| Choose an element | Tab / Shift+Tab | Bumpers |
| Move | Arrows (Shift for 10-unit steps) | D-pad |
| Resize | + / − | Triggers |
| Reset this element | R | Y |
| Reset all elements | Delete | |
| Save | Enter | A |
| Cancel (puts back the saved layout) | Escape | B |

**Safe placement.** After every move or resize the element's bounds are kept 2 units inside the screen.

**Readability.** The HUD draws in the engine's HUD coordinates, which the renderer scales to the window, so a layout
holds at every resolution and aspect. Scales are limited to 0.5–2.

## HD artwork (partial)

**Export: implemented.** `tools/xpr_export.py FILE.xpr OUT` decodes the menus' and HUD's textures (DXT1/3/5,
swizzled ARGB) to PNG. Tested: `cstrike_common.xpr` gives 890 textures.

These PNGs are the starting point for redrawn or upscaled artwork. Krita with its AI Diffusion plugin is an optional
production tool. The game never needs it, and its output must be reviewed for distorted text, wrong details and
artefacts before use.

**Import: not done.** A new XPR from the artwork, delivered as a cosmetic mod (`.xpr` is a mod asset type, N8). Open
questions:
1. The disk path the game uses for an XPR that is not in its archive (d:\media\textures\...), to check against the
   mod overlay's links.
2. Whether larger textures under the same names draw correctly. The menus give screen rectangles, so they should.
3. Memory: an uncompressed 2x copy of launcher.xpr would be about 300 MB, so a DXT encoder is needed (not written).

**Original / HD presets.**
- **Original Xbox:** PC Options > Mods > Looks > Original Xbox, plus HUD Scale 100%.
- **HD:** the HD artwork mods on, plus a larger HUD Scale if wanted.

There is no separate "HD" switch beyond these until HD artwork exists.

## Tests

- `tests/scenarios/vk_hud_layout.txt` (window):
  - the draw diagnostic maps elements;
  - a saved layout moves and resizes them (window shots);
  - the editor opens, moves an element, saves, and the setting is written.
- Split-screen layouts and controller editing are on the manual checklist.
