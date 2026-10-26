#!/usr/bin/env python3
"""Tests the server browser's protocols and discovery (src/net/query.h, master.h, browser.h) on their own: builds and
runs tests/net_browser_test.cpp over a simulated network with broadcast and a simulated clock (query and answer
serialization, signatures, hostile text, malformed and random datagrams, rate limits, amplification bounds, LAN
discovery of several servers, duplicates, servers appearing and expiring, favourites and identity changes, players
pages, the master server's registration, cookies, limits, expiry and lists, sorting and filters over 4096 servers,
saved servers).

Usage: python3 -I tests/net_browser_test.py   (a few seconds; clang++, libsodium)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'net_browser_test')
        src = [os.path.join(ROOT, 'src', 'net', n) for n in ('transport.cpp', 'query.cpp', 'master.cpp', 'browser.cpp')]
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'net_browser_test.cpp')] +
                               src + ['-lsodium', '-o', exe], capture_output=True, text=True)
        if build.returncode:
            print(build.stdout + build.stderr)
            sys.exit('build failed')
        run = subprocess.run([exe], capture_output=True, text=True)
        print((run.stdout + run.stderr).strip())
        sys.exit(run.returncode)


main()
