#!/usr/bin/env python3
"""Tests the scoreboard's row window and Ping text (src/game/scoreboard_rows.h) on their own: builds and runs
tests/scoreboard_rows_test.cpp (empty, 1, 16, 17, 32, 33, 65 players, one team of 64 or 65, scrolling to both ends,
clamping when players leave or change teams, the team header kept in sight, the "more below" count, every player
reachable; BOT, "-" and measured pings; the userinfo's *bot key).

Usage: python3 -I tests/scoreboard_rows_test.py   (a few seconds; clang++)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'scoreboard_rows_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-Wall', '-Werror', '-I', os.path.join(ROOT, 'src'),
                                os.path.join(ROOT, 'tests', 'scoreboard_rows_test.cpp'), '-o', exe],
                               capture_output=True, text=True)
        if build.returncode:
            print(build.stdout + build.stderr)
            sys.exit('build failed')
        run = subprocess.run([exe], capture_output=True, text=True)
        print(run.stdout.strip())
        sys.exit(run.returncode)


main()
