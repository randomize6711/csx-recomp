# csx-recomp

An unofficial native PC port of **Counter-Strike** for the original Xbox (2003), made by statically recompiling the
game's `default.xbe` from x86 to C++ and linking it with a host implementation of the Xbox kernel and XDK libraries.
It runs on Linux (SDL3 + Vulkan) and can be cross-built for Windows.

It is built on [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) (the `sdk/` submodule), with this project's
changes to it kept as patches in `patches/sdk/`.

> [!IMPORTANT]
> **This repository contains no game files.** No XBE, maps, textures, sounds, movies, keys or recompiled game code
> are included, and none may be added. You need your own legally acquired copy of the game to build and play.
> Counter-Strike is a trademark of Valve Corporation; Xbox is a trademark of Microsoft. This project is not
> affiliated with or endorsed by either.

## Features

- Native 64-bit executable, Vulkan renderer, SDL3 audio, window and input.
- Native resolution, widescreen, uncapped frame rate (the game's simulation timing is untouched), raw mouse input,
  keyboard and mouse bindings, controllers.
- A redesigned PC front end: Instant Action (solo or split screen; extra controllers join with START), Multiplayer
  (server browser, host game, direct IP / code), Options, Extras (training, profiles, mods).
- Up to 4-player split screen, PC online play over UDP with a dedicated server (`csx-server`, with a GUI in
  `tools/csx_server_gui.py`), up to 65 player slots (humans and bots).
- Extra modes (Deathmatch, Gun Game), map search, the downloadable content maps (Inferno, Office, ...) from your own
  copy of the DLC, a developer console and a net graph.

Progress and design notes are in `PROGRESS.md`, `ROADMAP.md`, `TODO.md`, `ARCHITECTURE.md`, `FINDINGS.md` and
`docs/`.

## Required game version

The recompiler and its build-time patches are written against **one exact `default.xbe`**. `config/game.toml` records
its SHA-256, and the build's `[[patch]]` entries are checked against it, so other builds of the game are not
supported.

| Field | Required value |
|---|---|
| Title | Counter-Strike (Microsoft Game Studios / Valve / Ritual) |
| Title ID | `4D530036` (**MS-054**) |
| Region (XBE certificate) | `0x4` (rest of world, **PAL / Europe**) |
| Certificate version | `0x2` |
| XBE build timestamp | 2003-10-22 14:24:48 UTC |
| XDK libraries | 1.0.**5659** |
| `default.xbe` size | 3 973 120 bytes |
| `default.xbe` SHA-256 | `e7698b09ed7322b69cf4c9c6d3778bb0864a15594b30258b5637ddcf8ee8308f` |
| `default.xbe` SHA-1 | `3848adbd0905f102d523eb91f9655b8091c110bb` |
| `default.xbe` MD5 | `d9c12961b32bd7ab3654363246053172` |

Check yours with `sha256sum default.xbe`, or `python3 tools/xbe_info.py default.xbe` for its title ID, timestamp and
library versions.

The disc's game partition (the files you extract from the disc or an XISO) is about 1.6 GB and contains
`default.xbe`, `dashupdate.xbe`, `downloader.xbe`, `update.xbe`, `FrontEnd.SXWad`, the `CacheTree-*.SXWad` language
files, the map files (`cs_*.SXWad`, `de_*.SXWad`) and the `Media` folder. For reference, the trimmed XISO this was
developed with (game partition only, 1 676 148 736 bytes) has SHA-1 `c071daaae1957608776a13b0558fa5809111b716`.
Other dumps of the same disc (full Redump images, other XISO tools) have different image hashes, but the files on
them are what matters: the `default.xbe` hash above is the one to check.

**Optional: the downloadable content.** The DLC maps are loaded from your own copy of the Xbox Live content package.
`tests/dlc_sha256sums.txt` lists the SHA-256 of each file of the supported package, and `docs/n6_dlc.md` describes
how it is imported.

## Building on Linux

### Prerequisites

- Clang and LLD, CMake, Ninja, Python 3, `pkg-config`
- SDL3, libsodium, the Vulkan loader and headers, the DirectX Shader Compiler (`libdxcompiler`)
- `unshare` (util-linux): the scripts run the game in a private network namespace unless network play is asked for

On Arch Linux:

    sudo pacman -S --needed clang lld cmake ninja python pkgconf sdl3 libsodium vulkan-icd-loader vulkan-headers \
        directx-shader-compiler util-linux

Zydis, toml++, Dear ImGui and (for the SDK's own tools) SDL3 are fetched by CMake.

### 1. Clone with the SDK

    git clone --recursive https://github.com/randomize6711/csx-recomp.git
    cd csx-recomp

### 2. Add your game files

    mkdir -p assets build
    cp /path/to/your/game/default.xbe assets/default.xbe
    sha256sum assets/default.xbe          # must match the table above
    cp -r --reflink=auto /path/to/your/game/. build/gamedata/

`assets/` and `build/` are ignored by git: game files never end up in a commit. `build/gamedata` is a copy because
the runtime writes to the game's `D:` drive; `--reflink=auto` makes it free on file systems that support it (Btrfs,
XFS).

### 3. Build

    tools/apply_sdk_patches.sh                       # puts sdk/ at its commit plus patches/sdk/*.patch
    cmake --preset linux-tools
    cmake --build out/build/linux-tools              # the recompiler and its tools
    tools/iterate.sh                                 # recompiles the XBE, builds the headless harness, boots it 40 s
    tools/build_window.sh                            # the game: build/window/CounterStrike
    tools/build_server.sh                            # the dedicated server: build/server/csx-server
    tools/make_zcache.sh                             # the sound banks the game caches (weapon and player sounds)

The recompiled code is written to `build/gen/` from your XBE on every build. It is derived from the game and must
never be published.

### 4. Play

    ./run_game.sh                    # boots like the console: intro movies, title screen, menus
    ./run_game.sh --match de_dust2   # straight into a match against bots
    ./run_game.sh --splitscreen 2    # split screen (players 2..4 on controllers)
    ./run_server.sh                  # a dedicated server

Every option (`--bots N`, `--host`, `--connect HOST[:PORT]`, `--build`, ...) is described at the top of
`run_game.sh`.

Click the window to capture the mouse, F1 releases it. Settings and profiles are kept in `build/play/`.

## Building for Windows

The Windows executable is cross-built from Linux with Clang and MinGW-w64 from the same sources (SDL3 + Vulkan).
After the Linux build above, which provides the generated code, run `tools/build_windows.sh`. It downloads SDL3, DXC
and libsodium (checked against their release digests) into `build/deps` and packages the game in
`build/windows/dist`. On Windows, run `run_game.bat "C:\path\to\game"` from there, pointing at your game files. It has
been tested under Wine. Testing on real Windows PCs is still to be done.

## Tests

`tests/` holds unit tests (`tests/*_test.py`), scripted game scenarios and network tests (`tools/run_net_tests.py`,
`tools/run_window_tests.py`). The scenario and window tests boot the game and so need your game files.

## License

The code in this repository is under the MIT License (`LICENSE`). The SDK in `sdk/` has its own MIT license and
third-party notices (`sdk/LICENSE`, `sdk/THIRD_PARTY.txt`). The license covers this project's code only, not the
game, its files or anything generated from them.
