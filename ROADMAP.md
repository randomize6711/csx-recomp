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
- M8 — PC feature set complete
