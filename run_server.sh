#!/bin/bash
# The dedicated server (csx-server, docs/pc_multiplayer.md section 16): no window, graphics, audio or input; every one
# of up to 65 player slots for remote players and bots. Runs in build/server-run, which keeps its state between runs:
# server_identity.key (made on the first start, owner-only: the key clients remember), admins.cfg and bans.txt
# (remote administration, off without admins.cfg), logs/. Its console is this terminal (engine commands: status,
# say, sv_restartround ...; quit, or Ctrl+C, ends it and tells the players).
#   ./run_server.sh [--build] [--config FILE] [--KEY VALUE]...      (./run_server.sh --help: the settings)
#   e.g. ./run_server.sh --map de_dust2 --bots 20 --player-collision off --name "Friday night"
#   --build   rebuild first (tools/iterate.sh 1, then tools/build_server.sh)
# Players join with ./run_game.sh --connect THIS_PC[:PORT]. It listens on UDP 27015 (--port) of every interface:
# open that port in a firewall for players from elsewhere; lan_only on refuses addresses outside private networks.
set -e
OLDPWD_ROOT=$(pwd)
root=$(cd "$(dirname "$0")" && pwd)
cd "$root"
if [ "$1" = --build ]; then
    shift
    tools/iterate.sh 1 > /dev/null
    tools/build_server.sh > build/server_build.log 2>&1 || { echo "server build failed: build/server_build.log"; exit 1; }
fi
[ -x build/server/csx-server ] || { echo "no build/server/csx-server: ./run_server.sh --build"; exit 1; }
# a --config path is the caller's, not the run folder's
args=()
while [ $# -gt 0 ]; do
    if [ "$1" = --config ] && [ $# -gt 1 ]; then
        case "$2" in /*) args+=("$1" "$2") ;; *) args+=("$1" "$OLDPWD_ROOT/$2") ;; esac
        shift 2
    else
        args+=("$1")
        shift
    fi
done
mkdir -p build/server-run
cd build/server-run
exec ../server/csx-server --game ../gamedata "${args[@]}"
