#!/bin/bash
# 3D sound distance attenuation on the given maps (default: de_airstrip de_dust cs_italy), headless: a minute of each
# direct-start match with the 3D logs on (tests/scenarios/audio3d_direct.txt), then tools/check_audio3d.py on the log.
# Needs build/zcache (tools/make_zcache.sh) and the harness (tools/iterate.sh).
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
status=0
for map in "${@:-de_airstrip de_dust cs_italy}"; do
    for m in $map; do
        echo "== $m"
        python3 -I tools/drive.py tests/scenarios/audio3d_direct.txt --timeout 400 --env FPS=30 --env XBE_LOG_3D=1 \
            --env CSX_AUDIO_LOG3D=1 --launch "tests/launch/${m}_beginner" --run "build/run_audio3d" > /dev/null || status=1
        python3 -I tools/check_audio3d.py "$(ls build/run_audio3d/logs/*.log | tail -1)" | grep -v "^     " || status=1
    done
done
exit $status
