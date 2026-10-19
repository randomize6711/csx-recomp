#!/bin/bash
# One bring-up iteration on Linux: apply SDK patches, rebuild the tools, regenerate from config/game.toml,
# rebuild the headless harness and boot it for $1 seconds (default 40) without network access.
#   generated code: build/gen/game     harness: build/harness/csx_headless     run folder: build/run
#   game data:      build/gamedata, a copy-on-write (reflink) copy of the original folder, so nothing the
#                   runtime writes to D: can reach the original files
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
[ -d build/gamedata ] || { echo "make build/gamedata first: cp -r --reflink=always <game dir>/. build/gamedata/"; exit 1; }
tools/apply_sdk_patches.sh > /dev/null || exit 1
cmake --build out/build/linux-tools > build/tools_build.log 2>&1 || { echo tools build failed; tail build/tools_build.log; exit 1; }
mkdir -p build/gen
out/build/linux-tools/xbe_recomp config/game.toml assets/default.xbe build/gen/game > build/recomp.log 2>&1 ||
    { echo recomp failed; tail build/recomp.log; exit 1; }
# memory.cpp uses ftruncate without <unistd.h>: force it in rather than edit the SDK
CXX="clang++ -include unistd.h" bash sdk/harness/build.sh -r . -g build/gen/game -o build/harness -n csx_headless \
    > build/harness_build.log 2>&1 || { echo harness build failed; grep error build/harness_build.log | head; exit 1; }
mkdir -p build/run && cd build/run && rm -rf logs hdd cap
timeout -s INT "${1:-40}" unshare -rn ../harness/csx_headless ../gamedata > run.out 2>&1
echo "exit $?  log: build/run/logs"
