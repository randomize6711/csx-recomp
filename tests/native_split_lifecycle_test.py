#!/usr/bin/env python3
"""Tests native split-screen's M2 lifecycle without the game (docs/native_splitscreen.md 15.12): the console's
routing (src/game/native_split_lifecycle.h: each player's lines in its own context, the process's and the server's
once), the front end's commands, a server shutdown asked in a turn, player 1's departure and the front end's
hand-over, the clients watched (a kick, a failed connect), sessions of up to four players joining and leaving, a
partial initialisation, a leaving player's cleanup (heap_owner.h's reference plan) and its bound, the voice channels.
Builds and runs tests/native_split_lifecycle_test.cpp (clang++, sanitizers). No game is started.

Usage: python3 -I tests/native_split_lifecycle_test.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'native_split_lifecycle_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'native_split_lifecycle_test.cpp'),
                                '-o', exe])
        if build.returncode:
            sys.exit('build failed')
        sys.exit(subprocess.run([exe]).returncode)


if __name__ == '__main__':
    main()
