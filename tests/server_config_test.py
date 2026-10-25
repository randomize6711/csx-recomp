#!/usr/bin/env python3
"""Tests the dedicated server's configuration (src/server/server_config.h) on its own: builds and runs
tests/server_config_test.cpp (ranges, files, the command line, the password kept out of the log).

Usage: python3 -I tests/server_config_test.py   (a few seconds; clang++)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'server_config_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'server_config_test.cpp'),
                                os.path.join(ROOT, 'src', 'server', 'server_config.cpp'), '-o', exe],
                               capture_output=True, text=True)
        if build.returncode:
            print(build.stdout + build.stderr)
            sys.exit('build failed')
        run = subprocess.run([exe], capture_output=True, text=True)
        print((run.stdout + run.stderr).strip())
        sys.exit(run.returncode)


main()
