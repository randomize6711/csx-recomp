#!/usr/bin/env python3
"""Tests the stufftext filter (src/game/stuff_filter.h, what a network server may run on its clients) on its own:
builds and runs tests/stuff_filter_test.cpp (allowed and refused commands, Cbuf_Execute's splitting: newlines inside
quotes, ';' outside them, unterminated text, quoted or prefixed names, overlong commands).

Usage: python3 -I tests/stuff_filter_test.py   (a second; clang++)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'stuff_filter_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'stuff_filter_test.cpp'),
                                '-o', exe], capture_output=True, text=True)
        if build.returncode:
            print(build.stdout + build.stderr)
            sys.exit('build failed')
        run = subprocess.run([exe], capture_output=True, text=True)
        print((run.stdout + run.stderr).strip())
        sys.exit(run.returncode)


main()
