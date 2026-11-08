#!/bin/bash
# Puts the Khronos Vulkan-Headers the window and Windows builds compile against in third_party/Vulkan-Headers (not in
# git): tag v1.4.363, checked against its commit. Nothing is done when the folder is already there.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
dir=$root/third_party/Vulkan-Headers
tag=v1.4.363
commit=6802bb4733b63ed5efd3adb308a6c885ef180ea1
[ -f "$dir/include/vulkan/vulkan.h" ] && exit 0
mkdir -p "$root/third_party"
git clone -q --depth 1 -b "$tag" https://github.com/KhronosGroup/Vulkan-Headers.git "$dir"
got=$(git -C "$dir" rev-parse HEAD)
[ "$got" = "$commit" ] || { echo "Vulkan-Headers $tag is $got, expected $commit"; rm -rf "$dir"; exit 1; }
echo "Vulkan-Headers $tag in third_party/Vulkan-Headers"
