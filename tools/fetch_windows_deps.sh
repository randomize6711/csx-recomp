#!/bin/bash
# Downloads the Windows build's third-party binaries into build/deps (not in git), checked against the release digests:
#   SDL3 3.4.18 MinGW development package (the Linux build's system SDL is 3.4.18): headers, import library, SDL3.dll
#   DXC v1.10.2605.37 Windows release (the Linux build's DXC): dxcompiler.dll, its import library and headers
# The Vulkan loader (vulkan-1.dll) comes with the GPU driver on Windows; the build makes its import library itself.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
deps=$root/build/deps
mkdir -p "$deps"
cd "$deps"

fetch() {  # url file sha256
    if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c --quiet 2>/dev/null; then
        echo "download $2"
        curl -fL --retry 3 -o "$2.part" "$1"
        echo "$3  $2.part" | sha256sum -c --quiet
        mv "$2.part" "$2"
    fi
}

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-mingw.tar.gz \
    SDL3-devel-3.4.18-mingw.tar.gz 049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf
fetch https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.10.2605.37/dxc_2026_08_11.zip \
    dxc_2026_08_11.zip 18da85ccac4d1736209bad960f0a124928a5c5a6046882e331c605d66744a889

if [ ! -d SDL3-3.4.18 ]; then tar xzf SDL3-devel-3.4.18-mingw.tar.gz; fi
if [ ! -d dxc ]; then mkdir dxc && (cd dxc && unzip -q ../dxc_2026_08_11.zip); fi
echo "windows deps in $deps"
