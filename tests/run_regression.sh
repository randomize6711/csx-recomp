#!/bin/bash
# Builds the headless harness and runs the end-to-end regression scenarios. Exit status non-zero on any failure.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
tools/iterate.sh 1 > /dev/null
status=0
for s in tests/scenarios/regress_*.txt; do
    echo "== $s"
    python3 -I tools/drive.py "$s" --timeout 600 || status=1
done
exit $status
