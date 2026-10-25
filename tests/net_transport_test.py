#!/usr/bin/env python3
"""Tests the PC network transport (src/net/transport.h) on its own: builds and runs tests/net_transport_test.cpp over
a simulated network and clock (the handshake; messages of every size to 64 KB, never a datagram over 1200 bytes; loss,
duplication, reordering; fragment loss and reassembly limits; replays, tampering, spoofed and stale cookies, address
binding; identity; timeouts, disconnects, rejects; the hello rate limit; random datagrams; 64 clients).

Usage: python3 -I tests/net_transport_test.py   (a few seconds; clang++, libsodium)
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'net_transport_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'net_transport_test.cpp'),
                                os.path.join(ROOT, 'src', 'net', 'transport.cpp'), '-lsodium', '-o', exe],
                               capture_output=True, text=True)
        if build.returncode:
            print(build.stdout + build.stderr)
            sys.exit('build failed')
        run = subprocess.run([exe], capture_output=True, text=True)
        print((run.stdout + run.stderr).strip())
        sys.exit(run.returncode)


main()
