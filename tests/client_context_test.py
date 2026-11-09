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
    """client_context.cpp's hand-checked entries plus the generated client_context_regions.inc, sorted"""
    text = open(os.path.join(ROOT, 'src', 'game', 'client_context.cpp'), encoding='utf-8').read()
    body = text[text.index('ClientContextRegions() {'):]
    body = body[:body.index('};')]
    body += open(os.path.join(ROOT, 'src', 'game', 'client_context_regions.inc'), encoding='utf-8').read()
    found = [(int(a, 16), int(n, 16), w) for a, n, w in re.findall(r'\{0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+), "([^"]+)"\}', body)]
    # the guest tables it names (guest_tables::k_NAME), from the generated layout
    loop = text[text.index('for (size_t k : {'):]
    loop = loop[:loop.index('})')]
    layout = open(os.path.join(ROOT, 'src', 'common', 'guest_tables_layout.h'), encoding='utf-8').read()
    for name in re.findall(r'guest_tables::k_(\w+)', loop):
        m = re.search(r'\{"' + name + r'", 0x([0-9A-F]+)u, 0x([0-9A-F]+)u, (\d+)u', layout)
        if not m:
            raise SystemExit(f'guest table {name} not in the layout')
        found.append((int(m.group(1), 16), int(m.group(2), 16) * int(m.group(3)), name))
    return sorted(found)


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
    # docs/native_splitscreen.md 13.7 (the fourth M2 run's findings): no static cvar is a player's (its next link is
    # the shared cvar list's); the application and video shell is the process's; the client game's pool object
    # (0x5BD8C4..0x5BD8E0, its chunk vector included) is a player's whole, not in part
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import state_audit
    def held(x):
        return next((w for a, n, w in regs if a <= x < a + n), None)
    for lo, n, what in state_audit.static_cvars():
        for x in range(lo, lo + n):
            if held(x):
                errors.append(f'{held(x)} holds 0x{x:X}, part of {what} (shared)')
                break
    for lo, hi, what in ((0x379FA8, 0x379FAC, 'the refresh rate'), (0x379FA4, 0x379FB0, 'the system and video objects'),
                         (0x5518A4, 0x5518A8, 'a start-up flag'), (0x55194C, 0x551958, 'the shutdown flags'),
                         (0x551928, 0x55192C, 'the cvar list head (cvar_vars)'),
                         (0x551FD4, 0x551FF8, 'the video-mode object and the display settings'),
                         (0x553614, 0x553620, 'a system object made once'),
                         (0x5EECA0, 0x5EEEA0, 'the display-mode list'), (0x5EF4E0, 0x5EF670, 'com_argv'),
                         (0x6CE0C8, 0x6CE0CC, 'com_argc'), (0x6CE204, 0x6CE224, 'the window rectangle and centre')):
        for x in range(lo, hi):
            if held(x):
                errors.append(f'{held(x)} holds 0x{x:X}, {what} (the process\'s)')
                break
    # 0x5BD8C4 (its list), the chunk vector's begin / last / end 0x5BD8CC..0x5BD8D8 (0x5BD8C8, the vector's empty
    # allocator, is never read or written), 0x5BD8D8 (a second list), the pool's sizes and blocks 0x5517F8..0x551810
    for lo, hi in ((0x5BD8C4, 0x5BD8C8), (0x5BD8CC, 0x5BD8E0), (0x5517F8, 0x551810)):
        for x in range(lo, hi):
            if not held(x):
                errors.append(f'0x{x:X} (the client game\'s pool object) is not in a player\'s regions')
                break
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
