#!/bin/bash
# Interactive play: the Linux window build (SDL3, Vulkan renderer, SDL audio, keyboard/mouse/gamepad). By default the
# title boots as on the console: the intro movies, the title screen and the front end, from which a match is started
# in the game's own menus. --match goes straight into an offline match against bots instead, as the front end's
# Beginner match would start it. No scenario, no timeout: the game runs until the window is closed.
#   ./run_game.sh [--build] [--match [MAP]] [--bots N] [--nocollide] [--host [--port P]] [--connect HOST[:PORT]]
#   MAP       any map in build/gamedata (de_airstrip by default, de_aztec, de_dust2, cs_italy, ...)
#   --bots N  bot count, 0..64 (default 7, the Beginner match's; the front end's menu stops at 16); implies --match
#   --nocollide  Player Collision Off (the setting for crowded matches); implies --match
#   --host    the match takes network players: a listen server on UDP port 27015 (or --port P) of every interface
#             (docs/pc_multiplayer.md); implies --match. Not for the open internet yet (see the doc's limits)
#   --connect HOST[:PORT]  join a server (a --host match on this PC or another): boots straight into it, in its own
#             run folder (build/play-client), so it can run beside a --host on the same PC
#   With --host or --connect the game uses this PC's network (the other modes run without any network access).
#   --build   rebuild first (tools/iterate.sh 1, which also boots the headless harness for a second, then
#             tools/build_window.sh)
# Environment passed through: FPS (default 30, the console's rate; FPS=0 runs at the game's own 72 cap),
# CSX_MOUSE_SENS (default 3), CSX_MOUSE_INVERT=1, CSX_AUDIO=null (silent), CSX_RENDERER=software (reference renderer,
# about 3 frames/s in a match).
# Runs in build/play (its pc_settings.ini kept): logs in build/play/logs, the emulated hard disk in build/play/hdd
# (fresh each launch).
set -e
root=$(cd "$(dirname "$0")" && pwd)
cd "$root"

build=0
match=0
host=0
connect=
port=
map=de_airstrip
bots=7
collide=1
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build=1 ;;
        --match) match=1 ;;
        --bots) bots=$2; match=1; shift ;;
        --nocollide) collide=0; match=1 ;;
        --host) host=1; match=1 ;;
        --port) port=$2; shift ;;
        --connect) connect=$2; shift ;;
        -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
        *) map=$1; match=1 ;;
    esac
    shift
done

# every match fixture has the same launch page; the command line is the Beginner match's with the map, bots and
# collision substituted
launch=tests/launch/de_airstrip_beginner
if [ $match = 1 ]; then
    [ -f "build/gamedata/$map.SXWad" ] || { echo "no map $map (have: $(ls build/gamedata | grep -E '^(cs|de)_' | sed 's/.SXWad//' | tr '\n' ' '))"; exit 1; }
    case "$bots" in ''|*[!0-9]*) echo "--bots wants a number"; exit 1 ;; esac
    [ "$bots" -le 64 ] || { echo "at most 64 bots (65 player slots)"; exit 1; }
fi
[ -d build/gamedata ] || { echo "make build/gamedata first: cp -r --reflink=always <game dir>/. build/gamedata/"; exit 1; }

if [ $build = 1 ] || [ ! -x build/window/CounterStrike ]; then
    tools/iterate.sh 1 > /dev/null
    tools/build_window.sh > build/window_build.log 2>&1 || { echo "window build failed: build/window_build.log"; exit 1; }
fi

run=build/play
[ -n "$connect" ] && run=build/play-client
rm -rf "$run/logs" "$run/hdd"
mkdir -p "$run/hdd"
# the network: none (a private, empty network namespace) unless the run hosts or joins
sandbox="unshare -rn"
if [ $host = 1 ] || [ -n "$connect" ]; then
    sandbox="unshare -r"
    [ $host = 1 ] && export CSX_NET_HOST=1
    [ -n "$port" ] && export CSX_NET_PORT=$port
fi
if [ -n "$connect" ]; then
    # what a join leaves for the title: the launch page and "+connect HOST[:PORT]" (src/game/net_session.cpp)
    mkdir -p "$run/hdd/Z"
    [ -d build/zcache ] && { rm -rf "$run/hdd/Z"; cp -r build/zcache "$run/hdd/Z"; }
    cp tests/launch/connect_local/launch.bin "$run/hdd/launch.bin"
    printf '+connect %s\0' "$connect" > "$run/hdd/Z/releasemap.txt"
    export CSX_RENDERER=${CSX_RENDERER:-vulkan}
    unset CSX_TEST_INPUT_ONLY
    echo "joining $connect (log: $run/run.out); click the window to capture the mouse, F1 releases it, close the window to quit"
    cd "$run"
    exec $sandbox ../window/CounterStrike ../gamedata > run.out 2>&1
fi
if [ $match = 0 ]; then
    export CSX_RENDERER=${CSX_RENDERER:-vulkan}
    unset CSX_TEST_INPUT_ONLY
    echo "starting from the intro movies (log: $run/run.out); click the window to capture the mouse, F1 releases it, close the window to quit"
    cd "$run"
    # no network namespace access (offline, as the tests run it); the front end caches its own sound banks on Z:
    exec $sandbox ../window/CounterStrike ../gamedata > run.out 2>&1
fi
# what the front end leaves for its relaunch into a match: the launch page and the command line on the cache
# partition Z:, plus the sound banks it caches there (build/zcache, tools/make_zcache.sh; without it no weapon sounds)
if [ -d build/zcache ]; then
    cp -r build/zcache "$run/hdd/Z"
else
    mkdir -p "$run/hdd/Z"
    echo "note: no build/zcache (run tools/make_zcache.sh once): the match starts without weapon and player sounds"
fi
cp "$launch/launch.bin" "$run/hdd/launch.bin"
# the command line ends with a NUL, as the front end writes it: the collision option goes before it
extra=$([ $collide = 1 ] || echo "+pc_player_collision 0")
{ tr -d '\0' < "$launch/releasemap.txt" | sed -e "s/+map de_airstrip /+map $map /" -e "s/+bot_quota 7 /+bot_quota $bots /"
  printf '%s\0' "$extra"; } > "$run/hdd/Z/releasemap.txt"

export CSX_RENDERER=${CSX_RENDERER:-vulkan}
unset CSX_TEST_INPUT_ONLY
echo "starting $map with $bots bots$([ $collide = 1 ] || echo ", player collision off")$([ $host = 1 ] && echo ", taking network players on UDP port ${port:-27015}") (log: $run/run.out); click the window to capture the mouse, F1 releases it, close the window to quit"
cd "$run"
# no network namespace access (offline, as the tests run it) unless --host
exec $sandbox ../window/CounterStrike ../gamedata > run.out 2>&1
