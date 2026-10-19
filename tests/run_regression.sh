#!/bin/bash
# Builds the headless harness and runs the end-to-end regression scenarios. Exit status non-zero on any failure.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tools/iterate.sh 1 > /dev/null
status=0
for s in tests/scenarios/regress_*.txt; do
    echo "== $s"
    # 30 presented frames per second (CS presents every second vertical blank): presses held N frames last alike
    # a scenario that starts in the match names its launch fixture in its header ("--launch tests/launch/...")
    launch=$(grep -m1 -o -- '--launch tests/launch/[A-Za-z0-9_]*' "$s" || true)
    python3 -I tools/drive.py "$s" --timeout 600 --env FPS=30 $launch || status=1
done
exit $status
