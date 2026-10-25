#!/bin/bash
# Builds the Linux window front end (src/linux) into build/window/CounterStrike: the headless harness's host with an
# SDL3 window, every frame drawn by the reference renderer. Shares the generated code objects with tools/iterate.sh,
# which must have run once (it regenerates and compiles the game code).
#   tools/build_window.sh && (cd build/window && ./CounterStrike ../gamedata)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tools/apply_sdk_patches.sh > /dev/null
[ -f build/gen/game/x86_func_mapping.cpp ] || { echo "run tools/iterate.sh first"; exit 1; }
GEN_OPT=-O2 HOST_OPT=-O3 CXX="clang++ -include unistd.h -Wno-unused-command-line-argument $(pkg-config --cflags sdl3) $(pkg-config --libs sdl3) -I$root/third_party/Vulkan-Headers/include -lvulkan -ldxcompiler -lsodium" \
    bash sdk/harness/build.sh -r . -g build/gen/game -o build/window -n CounterStrike -G build/harness/gen \
    -x /harness/main_test.cpp -x /harness/null_backend.cpp -x /harness/pad_script.cpp -x /harness/null_audio.cpp
