#!/bin/bash
# Generates shaders with the runtime's HLSL generator and compiles each to SPIR-V with DXC, as the Vulkan backend would.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
mkdir -p build/vk_probe
clang++ -std=c++20 -O1 -Isdk/runtime -Isdk/harness/compat tools/vk/dxc_probe.cpp sdk/runtime/gpu/shaders.cpp \
    -o build/vk_probe/probe 2>&1 | grep -E 'error|undefined' || true
[ -x build/vk_probe/probe ] && build/vk_probe/probe
# b0/b1 -> bindings 0/1, t0..3 -> 2..5, s0..3 -> 6..9, all in set 0
flags="-spirv -HV 2018 -fspv-target-env=vulkan1.3 -fvk-b-shift 0 0 -fvk-t-shift 2 0 -fvk-s-shift 6 0 -O3"
for f in build/vk_probe/*.hlsl; do
    n=$(basename "$f" .hlsl)
    case $n in vs_*) p=vs_6_0 ;; ps_*) p=ps_6_0 ;; gs_*) p=gs_6_0 ;; esac
    if dxc $flags -T $p -E main "$f" -Fo build/vk_probe/$n.spv > build/vk_probe/$n.log 2>&1; then
        echo "ok   $n ($(stat -c %s build/vk_probe/$n.spv) bytes)"
    else
        echo "FAIL $n"; head -8 build/vk_probe/$n.log
    fi
done
