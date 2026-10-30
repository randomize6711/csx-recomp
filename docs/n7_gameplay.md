# N7 — Gameplay customization and physics

Code: `src/game/pc_gameplay.cpp`, plus the settings rows in `pc_menu.cpp`, the controller chords in
`src/linux/window_input.cpp` and the keyboard bindings in `src/common/pc_bindings.cpp`.

## Status

| Item | Status |
|---|---|
| Optional first-person death camera | Implemented. PC Options > Gameplay > Death Camera: Original / First Person. |
| Gravity presets | Implemented. Very Low 200, Low 500, Normal 800, High 1200: Custom Game Match Options and the party Rules. |
| Custom numeric gravity | Implemented. Party Rules accepts a typed value from 50 to 4000; also csx-server `gravity` and the console's `pc_gravity N`. |
| Classic spray logo | Server side works: `impulse 201` sprays (the game's own code, `decalfrequency`, `player/sprayer.wav`). What the client draws is listed under Limitations. |
| Spray binding T | Implemented: PC Controls > Spray Logo, default T. |
| Controller spray binding | Implemented: PC Options > Controller > Spray Logo. Default Back + D-pad Down; choices are the Back + D-pad chords, Share, Paddles 1–4 and Touchpad. |
| Classic flashlight | Implemented: `impulse 100`, the game's own flashlight. The server must allow it (`mp_flashlight`, 0 in the title): every PC match turns it on, and a Rules row, Flashlights, can turn it off. |
| Flashlight binding | F on the keyboard. Controller: PC Options > Controller > Flashlight, default Back + D-pad Up. |
| White/black flashbang | Implemented. PC Options > Gameplay > Flashbang Effect: White / Black. |
| Optional ragdolls / lifetime | **Not implemented.** Investigated; see Ragdolls below. |

## How each works

**Gravity.**
- `pc_gravity N` runs `sv_gravity N` on the server. The engine sends the value to every client in the move
  variables, so the server's value is authoritative and client prediction follows it.
- A menu match line always carries `+pc_gravity`, so a later match never inherits an earlier one's value.

**Flashlight and spray.**
- These are the game's own impulses. The server-side flashlight check (`FAllowFlashlight`) reads `mp_flashlight`.
- The game's spray code traces 128 units ahead from the eyes, sets the player's next spray time (player +0x854) and
  plays the sound.
- The server logs both (`pc gameplay: NAME's flashlight on/off`, `NAME sprayed a logo`) from the rules' Think hook.
- Multiplayer: both are server-side entity state (EF_DIMLIGHT, a decal message), so every client sees the same.

**Controller chords.**
- With Back held (which also shows the scoreboard), a D-pad direction runs the action and is not passed to the
  game, so every original button keeps its action.
- The extra buttons (Share, Paddles, Touchpad) work on player 1's own controller only. A split-screen player gets a
  virtual controller from the compositor without them, but the chords work there.

**Flashbang.**
- The engine's ScreenFade handler (`V_ScreenFade` 0xB6500) is hooked once it has copied the colour (0xB6557).
- With Black, a white fade (255 255 255 with alpha over 200: the flashbang's) becomes black. Duration and strength
  are unchanged.
- Only this player's own view changes.

**Death camera.**
- Hooked after the engine's call to the client's `V_CalcRefdef` (0xB5F39, the ref_params at esp+0x10).
- While alive, the eye origin and angles are kept.
- After death, for 3 s: the eyes sink 34 units (eased over 0.6 s), the pitch levels out and the view rolls to 70°,
  as Half-Life's dead view does. The original camera then takes over.
- Only the client changes; nothing is sent to the server.

## Ragdolls (investigated, not implemented)

The request: optional client-side ragdolls, for example with Jolt, a configurable lifetime, hit registration and
collision unchanged, and the original death behaviour when off.

What a client-side ragdoll needs here:
1. **The studio renderer's bone setup.** The Xbox renderer's `R_StudioSetupBones` and its bone-transform array must
   be found so that the bones of a dead player's entity can be replaced. Its location in default.xbe is not yet found.
2. **The skeleton.** The studiohdr's bones (parent, default position and rotation) are readable from the loaded
   model (IDST v10/v11). Bodies and constraints would be built per major bone.
3. **World collision.** A Jolt mesh shape built from the client's world model (BSP faces), once per map.
4. **Corpse lifetime.** The client keeps a dead player's entity until it respawns or the server sends a body-queue
   copy. A ragdoll outliving it needs a client-side copy of the entity.
5. **The library.** Jolt (MIT) would be vendored into the project. No system package is needed, but it is a new
   dependency of about 30 MB of sources.

Steps 1 and 4 need reverse engineering of the Xbox studio renderer that was not done in this pass. Steps 2, 3 and 5
are straightforward. Hit registration and player collision are server-side and would stay untouched. Estimate:
several days. Recorded in TODO.md.

## Tests

- `tests/scenarios/regress_gameplay_n7.txt` (headless, fixture `de_aztec_n7`):
  - gravity 400 is set on the server;
  - the flashlight goes on and then off;
  - a spray happens looking down;
  - a bought flashbang thrown at the player's feet gives the black fade.
- `tests/scenarios/vk_death_camera.txt` (window): window shots alive, falling, fallen and after 3 s, looked at by
  hand. The log confirms the camera ran.
- Controller chords: manual (no physical controller in the automated runs).
