#!/bin/bash
# Applies patches/sdk/*.patch to the sdk submodule working tree; already applied patches are skipped.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
for p in "$root"/patches/sdk/*.patch; do
    if git -C "$root/sdk" apply --reverse --check "$p" 2>/dev/null; then echo "applied  $(basename "$p")"
    else git -C "$root/sdk" apply "$p" && echo "applying $(basename "$p")"; fi
done
