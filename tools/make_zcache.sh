#!/bin/bash
# Makes build/zcache: the title's cache partition (Z:) as the front end fills it on boot (the HD sound banks and the
# piemenu textures it copies from the disc), for tools/drive.py --launch, whose direct starts skip the front end.
# Boots headless to the match once (about 2 minutes). The copies are game data: they stay under build/, never in git.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
unshare -rn python3 -I tools/drive.py tests/scenarios/regress_boot_to_spawn.txt --timeout 600 --env FPS=30 > /dev/null
rm -rf build/zcache
cp -r --reflink=auto build/run/hdd/Z build/zcache
rm -f build/zcache/releasemap.txt
echo "build/zcache: $(du -sh build/zcache | cut -f1)"
