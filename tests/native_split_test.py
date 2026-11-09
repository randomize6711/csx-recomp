#!/usr/bin/env python3
"""Tests native split-screen's M2 parts without the game (docs/native_splitscreen.md): the local loopback channels
(src/game/local_loopback.h: two logical clients, routing, isolation, capacity, a player leaving), heap ownership and the
scrub of a new player's copy (heap_owner.h), the players' lifecycle and the registration policy (native_split_core.h),
and a context's removal (client_context_core.h). Builds and runs tests/native_split_test.cpp (clang++, sanitizers).
No game is started.

Usage: python3 -I tests/native_split_test.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'native_split_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'native_split_test.cpp'),
                                '-o', exe])
        if build.returncode:
            sys.exit('build failed')
        sys.exit(subprocess.run([exe]).returncode)


if __name__ == '__main__':
    main()
