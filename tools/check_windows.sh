#!/bin/bash
# Syntax-checks the Windows-only runtime sources (the ones the Linux harness leaves out) against the MinGW-w64
# headers with clang. A compile check, not a test: nothing here runs them. Needs mingw-w64-headers and the CMake
# linux-tools configure (for the ImGui and SDL sources it fetched).
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
deps=out/build/linux-tools/_deps
flags="--target=x86_64-w64-mingw32 -std=c++20 -fsyntax-only -w -Isrc -Isdk/runtime -Ibuild/gen/game
       -I$deps/imgui-src -I$deps/imgui-src/backends -I$deps/sdl3-src/include -I$deps/tomlplusplus-src/include
       -I/usr/x86_64-w64-mingw32/include -DUNICODE -D_UNICODE"
status=0
for f in sdk/runtime/app/*.cpp sdk/runtime/core/input.cpp sdk/runtime/gpu/d3d12.cpp sdk/runtime/audio/xaudio2.cpp; do
    if out=$(clang++ $flags "$f" 2>&1); then echo "ok   $f"; else echo "FAIL $f"; echo "$out" | grep error | head -5; status=1; fi
done
exit $status
