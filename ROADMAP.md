# Roadmap

| Phase | Goal | Exit criterion |
|---|---|---|
| 1 | Environment + framework analysis | Tools build on Linux; framework capabilities and gaps documented — **done** |
| 2 | CS XBE analysis | XDK build, sections, imports, library coverage documented — **done (initial)** |
| 2b | XDK 5659 enablement | `config/game.toml` with every host-replaced symbol either verified in Ghidra or deliberately left 0; `xdk/5659/layout.toml` offsets proven |
| 3 | Minimal recompilation | `xbe_recomp` completes; generated C++ compiles; count/list of trapping instructions |
| 4 | Initialization (headless harness, Linux) | Boots to the game's main loop without traps; files open from `D:\`; threads/TLS correct |
| 5 | Graphics + menus | Reference renderer frames show the front end; then Windows D3D12 build shows menus; Linux Vulkan backend started |
| 6 | Gameplay | Offline match with bots on several maps; audio (XACT), movies (XMV), system link between two instances |
| 7 | PC enhancements | Resolution/aspect, FOV, mouse+keyboard (raw), bindings, frame limiter, decoupled high-refresh presentation |
| 8 | Platform parity | Linux (Vulkan/SDL3) and Windows (D3D12) builds feature-equal; CI |
| 9 | Testing + optimization | Side-by-side comparison against xemu; regression captures; performance profiling |

## Milestones

- M1 — Toolchain verified (Linux tools build) ✅ 2026-10-19
- M2 — 5659 config complete and reviewed
- M3 — Generated code compiles and links in the harness
- M4 — First frame from the reference renderer
- M5 — Main menu on Windows
- M6 — Playable offline match
- M7 — Linux Vulkan renders the main menu
- M8 — PC feature set complete
