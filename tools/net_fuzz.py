#!/usr/bin/env python3
"""Builds tools/net_fuzz.cpp (with the transport, src/net) into build/nettools/net_fuzz when its sources are newer,
then runs it: malformed traffic against a running server (tests/net/net_malformed.txt).

Usage: python3 -I tools/net_fuzz.py HOST PORT SECONDS [SEED]
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    out = os.path.join(ROOT, 'build', 'nettools', 'net_fuzz')
    srcs = [os.path.join(ROOT, p) for p in ('tools/net_fuzz.cpp', 'src/net/transport.cpp', 'src/net/udp.cpp',
                                            'src/net/transport.h')]
    if not os.path.exists(out) or any(os.path.getmtime(s) > os.path.getmtime(out) for s in srcs):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        r = subprocess.run(['clang++', '-std=c++20', '-O2', '-Wall', '-I', os.path.join(ROOT, 'src')] +
                           [s for s in srcs if s.endswith('.cpp')] + ['-lsodium', '-o', out], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
    os.execv(out, [out] + sys.argv[1:])


main()
