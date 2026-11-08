#!/bin/bash
# Builds the dedicated server tool (docs/server_tool.md) and, with "package", the portable dedicated server package.
#   tools/build_server_tool.sh [linux]          build/server_tool/csx-server-tool (+ its unit tests)
#   tools/build_server_tool.sh windows          build/server_tool_win/csx-server-tool.exe (Clang + MinGW-w64, the Windows
#                                               build's SDL3 from build/deps: tools/fetch_windows_deps.sh)
#   tools/build_server_tool.sh package          build/dist/csx-dedicated-server-linux-x86_64/ (+ .tar.gz): the tool,
#                                               csx-server (tools/build_server.sh first), README; no game files
# ImGui is the SDK build's copy when it exists (out/build/linux-tools/_deps/imgui-src, v1.91.0), else fetched.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
mode=${1:-linux}
imgui=()
[ -f out/build/linux-tools/_deps/imgui-src/imgui.h ] && imgui=(-DIMGUI_DIR="$root/out/build/linux-tools/_deps/imgui-src")

case "$mode" in
linux)
    cmake -S server_tool -B build/server_tool -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ "${imgui[@]}" > /dev/null
    cmake --build build/server_tool
    echo "built build/server_tool/csx-server-tool"
    ;;
windows)
    tools/fetch_windows_deps.sh > /dev/null
    sdl=$root/build/deps/SDL3-3.4.18/x86_64-w64-mingw32
    cmake -S server_tool -B build/server_tool_win -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/x86_64-w64-mingw32-clang.cmake" \
        -DCMAKE_FIND_ROOT_PATH="/usr/x86_64-w64-mingw32;$sdl" -DSDL3_DIR="$sdl/lib/cmake/SDL3" "${imgui[@]}" > /dev/null
    cmake --build build/server_tool_win
    cp "$sdl/bin/SDL3.dll" build/server_tool_win/
    echo "built build/server_tool_win/csx-server-tool.exe (+ SDL3.dll)"
    ;;
package)
    [ -x build/server/csx-server ] || { echo "no build/server/csx-server: tools/build_server.sh first"; exit 1; }
    "$0" linux > /dev/null
    name=csx-dedicated-server-linux-x86_64
    out=build/dist/$name
    rm -rf "$out" "build/dist/$name.tar.gz" "build/dist/$name.tar.gz.sha256"
    mkdir -p "$out/bin/lib" "$out/licenses" "$out/maps"
    cp build/server_tool/csx-server-tool build/server/csx-server "$out/bin/"
    strip "$out/bin/csx-server-tool" "$out/bin/csx-server"
    # the two libraries a distribution may lack or have in another version (zlib and ISC licenses)
    for lib in libSDL3.so.0 libsodium.so.26; do
        src=$(ldd build/server_tool/csx-server-tool build/server/csx-server | awk -v l="$lib" '$1 == l {print $3; exit}')
        [ -n "$src" ] || { echo "$lib not found"; exit 1; }
        cp -L "$src" "$out/bin/lib/$lib"
    done
    cp /usr/share/licenses/sdl3/LICENSE "$out/licenses/SDL3.txt"
    cp /usr/share/licenses/libsodium/LICENSE "$out/licenses/libsodium.txt"
    cp LICENSE "$out/licenses/csx-recomp.txt"
    cp sdk/THIRD_PARTY.txt "$out/licenses/THIRD_PARTY.txt"
    cp server_tool/package/README.txt "$out/README.txt"
    cp config/server.cfg.example "$out/server.cfg.example"
    printf 'Imported maps (tools/import_map.py) go here: NAME.SXWad with its NAME.json report.\n' > "$out/maps/README.txt"
    cat > "$out/csx-server-tool" <<'LAUNCH'
#!/bin/sh
# the server tool with this package's libraries (the server it starts inherits them)
here=$(cd "$(dirname "$0")" && pwd)
LD_LIBRARY_PATH="$here/bin/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$here/bin/csx-server-tool" "$@"
LAUNCH
    chmod +x "$out/csx-server-tool"
    # never game files: the package is checked for anything that looks like one
    if find "$out" \( -iname '*.xbe' -o -iname '*.sxwad' -o -iname '*.xpr' -o -iname '*.xwb' -o -iname '*.xsb' \
            -o -iname '*.uix' -o -iname '*.iso' -o -iname '*.xmv' \) | grep -q .; then
        echo "game files found in the package: refused"; exit 1
    fi
    (cd build/dist && tar czf "$name.tar.gz" "$name" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
    echo "packaged build/dist/$name.tar.gz ($(du -h "build/dist/$name.tar.gz" | cut -f1))"
    ;;
*)
    echo "usage: $0 [linux|windows|package]"; exit 2 ;;
esac
