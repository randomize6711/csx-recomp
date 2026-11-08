#!/usr/bin/env python3
"""Tests the dedicated server tool (docs/server_tool.md): its unit tests, then its scripts against the real csx-server
(tests/server_tool/*.txt, run by csx-server-tool --script), each in a private network namespace (unshare -rn: the
server's port is reachable from nowhere) with a data folder of its own.

  python3 -I tests/server_tool_test.py [tests/server_tool/NAME.txt ...]      (no argument: all)
Needs build/server_tool (cmake, docs/server_tool.md), build/server/csx-server and the game files in build/gamedata
(or GAME_DIR).
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, 'build', 'server_tool', 'csx-server-tool')
UNIT = os.path.join(ROOT, 'build', 'server_tool', 'csx-server-tool-tests')
GAME = os.environ.get('GAME_DIR') or os.path.join(ROOT, 'build', 'gamedata')


def main():
    scripts = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, 'tests', 'server_tool', '*.txt')))
    results = []
    r = subprocess.run([UNIT], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr)
    results.append(('unit tests', r.returncode == 0))
    for s in scripts:
        name = os.path.splitext(os.path.basename(s))[0]
        with tempfile.TemporaryDirectory(prefix='csx_tool_') as tmp:
            text = open(s).read().replace('game GAME', 'game ' + GAME)
            script = os.path.join(tmp, name + '.txt')
            open(script, 'w').write(text)
            start = time.time()
            env = dict(os.environ, CSX_TOOL_DUMP=os.path.join(tmp, 'output.txt'))
            r = subprocess.run(['unshare', '-rn', TOOL, '--script', script, '--data', os.path.join(tmp, 'data')],
                               capture_output=True, text=True, env=env, timeout=1200)
            ok = r.returncode == 0
            print(f'{"PASS" if ok else "FAIL"} {name} [{time.time() - start:.0f} s]')
            lines = r.stdout.splitlines()
            for l in lines if not ok else [l for l in lines if l.startswith('     ')]:
                print('   ', l)
            if not ok:
                keep = os.path.join(ROOT, 'build', 'server_tool', f'failed_{name}')
                shutil.rmtree(keep, ignore_errors=True)
                shutil.copytree(tmp, keep)
                print('    kept in', keep)
            results.append((name, ok))
    passed = sum(1 for _, ok in results if ok)
    print(f'{passed} of {len(results)} passed')
    return 0 if passed == len(results) else 1


sys.exit(main())
