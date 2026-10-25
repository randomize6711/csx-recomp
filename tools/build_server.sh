#!/bin/bash
# Builds the Linux dedicated server (src/server, docs/pc_multiplayer.md "N2") into build/server/csx-server: the game's
# engine and game DLL with the server's own stand ins (no window, no Vulkan, no SDL, no pad script, nothing drawn) and
# the harness's settings and silent audio mixer (no audio device). Shares the generated code objects with
# tools/iterate.sh, which must have run once (it regenerates and compiles the game code).
#   tools/build_server.sh && (mkdir -p build/srv && cd build/srv && ../server/csx-server --game ../gamedata)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tools/apply_sdk_patches.sh > /dev/null
[ -f build/gen/game/x86_func_mapping.cpp ] || { echo "run tools/iterate.sh first"; exit 1; }
GEN_OPT=-O2 HOST_OPT=-O2 CXX="clang++ -include unistd.h -Wno-unused-command-line-argument -lsodium" \
    bash sdk/harness/build.sh -r . -g build/gen/game -o build/server -n csx-server -G build/harness/gen \
    -x /harness/main_test.cpp -x /harness/null_backend.cpp -x /harness/pad_script.cpp -x /harness/reference_renderer.cpp \
    -x /src/linux/ -x /src/vulkan/
