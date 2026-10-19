#!/bin/bash
# Audio output check (window build, Vulkan): the direct match on de_airstrip with the mixer output captured
# (CSX_AUDIO_WAV, null device so nothing plays), then tools/check_audio.py on it. Needs build/zcache
# (tools/make_zcache.sh) for the match's cached sound banks, and a display.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[ -d build/zcache ] || { echo "make build/zcache first: tools/make_zcache.sh"; exit 1; }
rm -f build/wrun/audio_check.wav
unshare -rn python3 -I tools/drive.py tests/scenarios/vk_match_direct.txt --exe build/window/CounterStrike \
    --run build/wrun --timeout 200 --env CSX_RENDERER=vulkan --env FPS=0 \
    --env CSX_AUDIO_WAV="$root/build/wrun/audio_check.wav" --launch tests/launch/de_airstrip_beginner
python3 -I tools/check_audio.py build/wrun/audio_check.wav --from 15
