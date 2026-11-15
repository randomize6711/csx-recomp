#!/bin/bash
# Builds the headless harness (build/harness/CounterStrike) exactly as tools/iterate.sh does, without booting it:
# SDK patches, the tools, the code regenerated from config/game.toml, the harness. Nothing is run (the native
# split-screen work's rule: a game run only with the user's permission for that run).
#   tools/build_harness.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tools/apply_sdk_patches.sh > /dev/null
cmake --build out/build/linux-tools > build/tools_build.log 2>&1 || { echo tools build failed; tail build/tools_build.log; exit 1; }
mkdir -p build/gen
out/build/linux-tools/xbe_recomp config/game.toml assets/default.xbe build/gen/game > build/recomp.log 2>&1 ||
    { echo recomp failed; tail build/recomp.log; exit 1; }
GEN_OPT=-O2 CXX="clang++ -include unistd.h -Wno-unused-command-line-argument -lsodium" bash sdk/harness/build.sh -r . -g build/gen/game -o build/harness -n CounterStrike -x /src/linux/ -x /src/vulkan/ -x /src/server/ \
    > build/harness_build.log 2>&1 || { echo harness build failed; grep error build/harness_build.log | head; exit 1; }
echo "built build/harness/CounterStrike"
