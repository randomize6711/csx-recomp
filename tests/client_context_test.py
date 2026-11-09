#!/usr/bin/env python3
"""Tests native split-screen's client contexts without the game (docs/native_splitscreen.md, M1):
  1. src/game/client_context_core.h on a synthetic memory (tests/client_context_test.cpp, clang++ with sanitizers);
  2. the region list in src/game/client_context.cpp against the static audit (tools/state_audit.py): sorted, not
     overlapping, and holding no data address that only the server's code touches (the audit needs build/gen, the
     generated code of your own XBE; skipped with a note when it is missing).
No game is started.

Usage: python3 -I tests/client_context_test.py
"""
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def core_test():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'client_context_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'client_context_test.cpp'),
                                '-o', exe])
        if build.returncode:
            return 'core: build failed'
        return '' if subprocess.run([exe]).returncode == 0 else 'core: failed'


def regions():
    text = open(os.path.join(ROOT, 'src', 'game', 'client_context.cpp'), encoding='utf-8').read()
    body = text[text.index('ClientContextRegions() {'):]
    body = body[:body.index('};')]
    return [(int(a, 16), int(n, 16), w) for a, n, w in re.findall(r'\{0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+), "([^"]+)"\}', body)]


def audit_test():
    regs = regions()
    if not regs:
        return 'regions: none parsed from client_context.cpp'
    errors = []
    for (a, n, w), (b, _, v) in zip(regs, regs[1:]):
        if b < a + n:
            errors.append(f'regions overlap or are unsorted: {w} / {v}')
    if not os.path.isdir(os.path.join(ROOT, 'build', 'gen', 'game')):
        print('note: build/gen/game missing (your generated code): the audit check is skipped')
        return '; '.join(errors)
    for step in ('build', 'classify'):
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'state_audit.py'), step],
                           capture_output=True, text=True)
        if r.returncode:
            return f'state_audit {step} failed: {r.stderr[-400:]}'
    data = json.load(open(os.path.join(ROOT, 'build', 'state_audit', 'classes.json')))['data']
    for addr, classes in data.items():
        x = int(addr, 16)
        for a, n, w in regs:
            if a <= x < a + n:
                only_server = all('server' in c and 'client' not in c for c in classes if c != 'unreached')
                if only_server and any(c != 'unreached' for c in classes):
                    errors.append(f'{w} holds {addr}, touched only by the server: {classes}')
    total = sum(n for _, n, _ in regs)
    print(f'{len(regs)} regions, {total} bytes ({total / 1024:.1f} KB) per player')
    return '; '.join(errors)


def main():
    failures = [f for f in (core_test(), audit_test()) if f]
    for f in failures:
        print('FAIL', f)
    print('client_context_test:', 'FAIL' if failures else 'ok')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
