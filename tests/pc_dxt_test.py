#!/usr/bin/env python3
"""Tests the lightmaps' DXT1 and DXT4/5 decoding (src/game/pc_dxt.h) on its own: builds and runs
tests/pc_dxt_test.cpp in a temporary folder. No game is started.

Usage: python3 -I tests/pc_dxt_test.py   (a few seconds; clang++)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'pc_dxt_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'pc_dxt_test.cpp'),
                                '-o', exe])
        if build.returncode:
            sys.exit('build failed')
        sys.exit(subprocess.run([exe]).returncode)


if __name__ == '__main__':
    main()
