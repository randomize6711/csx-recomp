#!/bin/bash
# Interactive play: the Linux window build (SDL3, Vulkan renderer, SDL audio, keyboard/mouse/gamepad) started directly
# in an offline match against bots, as the front end's Beginner match would start it. No scenario, no timeout: the
# game runs until the window is closed.
#   ./run_game.sh [--build] [MAP]     MAP: de_airstrip (default), de_dust, cs_italy (fixtures in tests/launch)
#   --build   rebuild first (tools/iterate.sh 1, which also boots the headless harness for a second, then
#             tools/build_window.sh)
# Environment passed through: FPS (default 30, the console's rate; FPS=0 runs at the game's own 72 cap),
# CSX_MOUSE_SENS (default 3), CSX_MOUSE_INVERT=1, CSX_AUDIO=null (silent), CSX_RENDERER=software (reference renderer,
# about 3 frames/s in a match).
# Runs in build/play: logs in build/play/logs, the emulated hard disk in build/play/hdd (fresh each launch).
set -e
root=$(cd "$(dirname "$0")" && pwd)
cd "$root"

build=0
map=de_airstrip
for a in "$@"; do
    case "$a" in
        --build) build=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) map=$a ;;
    esac
done

launch=tests/launch/${map}_beginner
[ -d "$launch" ] || { echo "no match fixture $launch (have: $(ls tests/launch | grep _beginner | sed 's/_beginner//' | tr '\n' ' '))"; exit 1; }
[ -d build/gamedata ] || { echo "make build/gamedata first: cp -r --reflink=always <game dir>/. build/gamedata/"; exit 1; }

if [ $build = 1 ] || [ ! -x build/window/CounterStrike ]; then
    tools/iterate.sh 1 > /dev/null
    tools/build_window.sh > build/window_build.log 2>&1 || { echo "window build failed: build/window_build.log"; exit 1; }
fi

# what the front end leaves for its relaunch into a match: the launch page and the command line on the cache
# partition Z:, plus the sound banks it caches there (build/zcache, tools/make_zcache.sh; without it no weapon sounds)
run=build/play
rm -rf "$run/logs" "$run/hdd"
mkdir -p "$run/hdd"
if [ -d build/zcache ]; then
    cp -r build/zcache "$run/hdd/Z"
else
    mkdir -p "$run/hdd/Z"
    echo "note: no build/zcache (run tools/make_zcache.sh once): the match starts without weapon and player sounds"
fi
cp "$launch/launch.bin" "$run/hdd/launch.bin"
cp "$launch/releasemap.txt" "$run/hdd/Z/releasemap.txt"

export CSX_RENDERER=${CSX_RENDERER:-vulkan}
unset CSX_TEST_INPUT_ONLY
echo "starting $map (log: $run/run.out); click the window to capture the mouse, F1 releases it, close the window to quit"
cd "$run"
# no network namespace access (offline, as the tests run it)
exec unshare -rn ../window/CounterStrike ../gamedata > run.out 2>&1
