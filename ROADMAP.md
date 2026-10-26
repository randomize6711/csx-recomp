# Roadmap

| Phase | Goal | Exit criterion |
|---|---|---|
| 1 | Environment + framework analysis | Tools build on Linux; framework capabilities and gaps documented — **done** |
| 2 | CS XBE analysis | XDK build, sections, imports, library coverage documented — **done (initial)** |
| 2b | XDK 5659 enablement (in progress) | `config/game.toml` with every host-replaced symbol either verified in Ghidra or deliberately left 0; `xdk/5659/layout.toml` offsets proven |
| 3 | Minimal recompilation | `xbe_recomp` completes; generated C++ compiles; count/list of trapping instructions — **done** |
| 4 | Initialization (headless harness, Linux) | Boots to the game's main loop without traps; files open from `D:\`; threads/TLS correct — **done** |
| 5 | Graphics + menus (menus done headless) | Reference renderer frames show the front end; then Windows D3D12 build shows menus; Linux Vulkan backend started |
| 6 | Gameplay | Offline match with bots on several maps; audio (XACT), movies (XMV), system link between two instances |
| 7 | PC enhancements | Resolution/aspect, FOV, mouse+keyboard (raw), bindings, frame limiter, decoupled high-refresh presentation |
| 8 | Platform parity | Linux and Windows builds feature-equal (same SDL3 / Vulkan front end); CI |
| 9 | Testing + optimization | Side-by-side comparison against xemu; regression captures; performance profiling |

## Milestones

- M1 — Toolchain verified (Linux tools build) ✅ 2026-10-19
- M2 — 5659 config complete and reviewed
- M3 — Generated code compiles and links in the harness ✅ 2026-10-19 (boots to Direct3D_CreateDevice)
- M4 — First frame from the reference renderer ✅ 2026-10-19 (title screen "Please press START")
- M5 — Main menu reached and rendered (headless reference renderer) ✅ 2026-10-19; on Windows: pending
- M6 — Offline match initialises: de_airstrip, spawn, HUD, bots active ✅ 2026-10-19 (headless); player control verified 2026-10-19
- M7 — Linux Vulkan renders the main menu ✅ 2026-10-19
- M7b — Complete in-game scene (world, models, view model) in both renderers ✅ 2026-10-19 (de_airstrip spawn)
- M7c — Windows x86-64 executable cross-built (Clang + MinGW-w64), runs under Wine: menus, match, audio, mouse ✅
  2026-10-19; on Windows itself: pending
- M7d — Linux play-test fixes: 3D sound distance attenuation, character shadows and HUD colours ✅ 2026-10-19; lamp
  glows fixed by the same change (seen in the de_airstrip tunnel); decals (polygon offset) ✅ 2026-10-19
- M7e — PC Options inside the original menus (mouse sensitivity, invert, display mode, master volume) ✅ 2026-10-19
- M7f — Keyboard and mouse bindings in the original menus (PC Controls) ✅ 2026-10-19
- M7g — Field of view (PC Options) ✅ 2026-10-19; widescreen 16:10 / 16:9 / 21:9 with the original UI centred ✅
  2026-10-20
- M7h — The original developer console reachable (PC Controls key, typing, history, completion) ✅ 2026-10-20
- M7i — Native-resolution rendering (Vulkan) with the display's modes in PC Options ✅ 2026-10-20
- M7j — Mouse navigation in the original menus (tables, settings, help bars, buy pies) ✅ 2026-10-20; checked in the
  window at every aspect with audible hover sounds, the profile name's keyboard ✅ 2026-10-20
- M7k — Press-a-key rebinding in PC Controls (keys, mouse buttons, wheel, every keyboard layout) ✅ 2026-10-20
- G1 — Graphics fidelity audit: the title's effects against both renderers, ranked (FINDINGS.md) ✅ 2026-10-21
- G2 — Dynamic gamma (auto exposure) in Vulkan and in the reference renderer, Auto Exposure in PC Options ✅ 2026-10-21
- G3 — remaining confirmed differences: the flashbang's white fade ✅ 2026-10-21 (full screen at every aspect); de_aztec's
  water (its cube texture in Vulkan) ✅ 2026-10-22; G4 — the cube texture and Auto Exposure checks ✅ 2026-10-22;
  G5 — the bloom: GAUSSIANCUBIC, the composite's scissor, Bloom in PC Options ✅ 2026-10-22; G6 — Vulkan validation
  cleanup, Auto Exposure Off from the map's first frame ✅ 2026-10-22; G6.1 — the weapon over the HUD at a
  wide FOV (Vulkan's depth clamp) ✅ 2026-10-23; G7 — Xbox Graphical Effects, the master over Bloom and Auto
  Exposure ✅ 2026-10-23
- G8.1 — Custom Game > Player Collision (single player Custom Options) ✅ 2026-10-23
- G8.2 — Custom Game > Number of Bots: 0 to 16 in the original 17 slots ✅ 2026-10-23; G8.2B, 65 player slots
  (any mix of humans and bots), planned in docs/player_slots_plan.md: checkpoint 1 (recompiler [[patch]], the player
  slot audit, guest tables, the memory investigation) ✅ 2026-10-23; checkpoint 2 (the 65 slots: 64 bots and the
  player, protocol 47, the scoreboard's pages) ✅ 2026-10-24, waiting for approval; checkpoint 3: the menu to 64 bots ✅
  2026-10-24 (with the XACT / DPC race fix), the benchmarks and high population performance (-O2, the round restart
  wait) ✅ 2026-10-24, the scoreboard for 65 (Ping, BOT, scrolling) ✅ 2026-10-24; waiting for the user's play-test
- N0 — PC multiplayer architecture investigation (docs/pc_multiplayer.md) ✅ 2026-10-24, waiting for approval of the
  architecture; then N1 — PC transport and remote clients ✅ 2026-10-25 (signed off), N2 — dedicated server
  (csx-server: no host slot, server identity, remote administration, 30 snapshots a second) ✅ 2026-10-25, waiting
  for approval; N3 — server browser, N4 — split-screen (one client process per local player), each after the user's
  approval
- M8 — PC feature set complete
