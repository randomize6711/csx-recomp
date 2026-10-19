#!/bin/bash
# Cross-builds the Windows x86-64 executable (SDL3 window, Vulkan renderer, SDL audio) from Linux with Clang targeting
# MinGW-w64 and packages it in build/windows/dist (CounterStrike.exe, SDL3.dll, dxcompiler.dll, licenses, launcher).
# The generated code is the Linux recompiler's (tools/iterate.sh must have run); cmake/windows/CMakeLists.txt has the
# source list, cmake/toolchains/x86_64-w64-mingw32-clang.cmake the toolchain.
#   tools/build_windows.sh            then run build/windows/dist on Windows: run_game.bat "C:\path\to\game"
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[ -f build/gen/game/x86_func_mapping.cpp ] || { echo "run tools/iterate.sh first"; exit 1; }
tools/apply_sdk_patches.sh > /dev/null
tools/fetch_windows_deps.sh > /dev/null
[ -f build/windows/build.ninja ] ||
    cmake -S cmake/windows -B build/windows -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/x86_64-w64-mingw32-clang.cmake" > /dev/null
ninja -C build/windows
rm -rf build/windows/dist
cmake --install build/windows --prefix build/windows/dist > /dev/null
echo "built build/windows/dist"
