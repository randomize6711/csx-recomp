#!/usr/bin/env python3
"""Tests the bounded bit writer and the network checks (src/game/net_buffers.h) on their own: builds and runs
tests/net_buffers_test.cpp (the same bits as the engine's writer; writes at exactly the capacity, one bit past it,
after an overflow, from a full buffer, nothing ever written past the buffer; bit counts out of range, saturated values;
SZ_GetSpace's lengths; split packet headers and netchan fragments, valid and malformed).

Usage: python3 -I tests/net_buffers_test.py   (a few seconds; clang++)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'net_buffers_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-Wall', '-Werror', '-I', os.path.join(ROOT, 'src'),
                                os.path.join(ROOT, 'tests', 'net_buffers_test.cpp'), '-o', exe],
                               capture_output=True, text=True)
        if build.returncode:
            print(build.stdout + build.stderr)
            sys.exit('build failed')
        run = subprocess.run([exe], capture_output=True, text=True)
        print(run.stdout.strip())
        sys.exit(run.returncode)


main()
