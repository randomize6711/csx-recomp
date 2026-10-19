#!/bin/bash
# Puts the sdk submodule working tree at its commit plus patches/sdk/*.patch, in order.
# The expected tree is built in a scratch index; when the working tree already matches it nothing is touched (so
# builds stay incremental), otherwise the patched files are reset to the commit and every patch is applied again.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
sdk=$root/sdk
patches=("$root"/patches/sdk/*.patch)
index=$(mktemp)
trap 'rm -f "$index"' EXIT
export GIT_INDEX_FILE=$index
git -C "$sdk" read-tree HEAD
for p in "${patches[@]}"; do git -C "$sdk" apply --cached "$p"; done
if git -C "$sdk" diff --quiet; then
    echo "sdk patches applied (${#patches[@]})"
    exit 0
fi
unset GIT_INDEX_FILE
# every file changed in the working tree, so a patch taken out of patches/sdk is undone too
git -C "$sdk" checkout -- .
for p in "${patches[@]}"; do git -C "$sdk" apply "$p" && echo "applying $(basename "$p")"; done
