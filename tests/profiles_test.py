#!/usr/bin/env python3
"""Tests the saved player profiles (src/common/profiles.h) on their own: builds and runs
tests/profiles_test.cpp in a temporary folder.

Usage: python3 -I tests/profiles_test.py   (a few seconds; clang++)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'profiles_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'profiles_test.cpp'),
                                os.path.join(ROOT, 'src', 'common', 'profiles.cpp'), '-o', exe],
                               capture_output=True, text=True)
        if build.returncode:
            print(build.stdout + build.stderr)
            sys.exit('build failed')
        run = subprocess.run([exe, tmp], capture_output=True, text=True)
        print((run.stdout + run.stderr).strip())
        sys.exit(run.returncode)


main()
