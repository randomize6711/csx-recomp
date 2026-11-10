#!/usr/bin/env python3
"""check_tables.py [EXE...]: does each executable hold the region model's tables of this source tree?

The tables (src/game/client_context_regions.inc, _protected.inc, _addresses.inc) are hashed at compile time from the
arrays the executable uses (src/game/table_fingerprint.h) into 32-byte stamps: "CSXFP1:<table>" (16 bytes, NUL
padded), the FNV-1a 64 hash (little-endian words: the entry count, then each entry's numbers), the entry count and
the bytes covered. This tool computes the same hash from the source tree's tables and finds the stamps in each
executable: every table's stamp present, all its copies alike, and equal to the tree's. A stale object (compiled
from an older table, whatever the timestamps said), a missing stamp or a doubled different one is a failure.

Without arguments: the Linux executables that exist of build/harness/CounterStrike, build/window/CounterStrike and
build/server/csx-server (none existing is a failure). Exit status 0 when every executable matches, 1 otherwise.
Read only: nothing is run or changed.
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, 'src', 'game')
TABLES = {  # stamp name: (file, kind)
    'regions': ('client_context_regions.inc', 'ranges'),
    'protect': ('client_context_protected.inc', 'ranges'),
    'address': ('client_context_addresses.inc', 'words'),
}
DEFAULT_EXES = ('build/harness/CounterStrike', 'build/window/CounterStrike', 'build/server/csx-server')
MARKER = b'CSXFP1:'
BASIS, PRIME, MASK = 0xCBF29CE484222325, 0x100000001B3, (1 << 64) - 1


def word(h, v):
    for i in range(4):
        h ^= (v >> (8 * i)) & 0xFF
        h = (h * PRIME) & MASK
    return h


def code_lines(text):
    return [line for line in text.splitlines() if not line.lstrip().startswith('//')]


def parse(text, kind):
    """a table's entries: [(va, size)] for ranges, [va] for words"""
    body = '\n'.join(code_lines(text))
    if kind == 'ranges':
        return [(int(a, 16), int(n, 16)) for a, n in re.findall(r'\{0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+), "', body)]
    return [int(a, 16) for a in re.findall(r'0x([0-9A-Fa-f]+)', body)]


def fingerprint(entries, kind):
    """(hash, count, bytes) as table_fingerprint.h computes it"""
    h = word(BASIS, len(entries))
    if kind == 'ranges':
        for va, size in entries:
            h = word(word(h, va), size)
        return h, len(entries), sum(size for _, size in entries) & 0xFFFFFFFF
    for va in entries:
        h = word(h, va)
    return h, len(entries), 0


def expected(game=GAME):
    out = {}
    for name, (fn, kind) in TABLES.items():
        out[name] = fingerprint(parse(open(os.path.join(game, fn)).read(), kind), kind)
    return out


def stamps(data):
    """every stamp in an executable's bytes: {name: [(hash, count, bytes), ...]}"""
    found = {}
    at = data.find(MARKER)
    while at >= 0:
        if at + 32 <= len(data):
            raw = data[at:at + 16]
            name = raw[len(MARKER):].split(b'\0', 1)[0].decode('ascii', 'replace')
            if raw[len(MARKER) + len(name):] == b'\0' * (16 - len(MARKER) - len(name)):
                found.setdefault(name, []).append(struct.unpack_from('<QII', data, at + 16))
        at = data.find(MARKER, at + 1)
    return found


def check(data, want):
    """the problems of one executable's bytes against the expected fingerprints ([] when it matches)"""
    problems = []
    got = stamps(data)
    for name, fp in want.items():
        copies = got.get(name, [])
        if not copies:
            problems.append(f'{name}: no stamp (built before the fingerprint, or without the table)')
        elif len(set(copies)) > 1:
            problems.append(f'{name}: {len(copies)} stamps that differ: {sorted(set(copies))}')
        elif copies[0] != fp:
            h, n, b = copies[0]
            problems.append(f'{name}: STALE: compiled {n} entries, {b} bytes, {h:016X}; the tree has {fp[1]} entries, '
                            f'{fp[2]} bytes, {fp[0]:016X}')
    for name in got:
        if name not in want:
            problems.append(f'{name}: a stamp of an unknown table')
    return problems


def main(argv):
    exes = argv or [e for e in DEFAULT_EXES if os.path.exists(os.path.join(ROOT, e))]
    if not exes:
        print('check_tables: no executable to check')
        return 1
    want = expected()
    for name, (h, n, b) in want.items():
        print(f'tree {name}: {n} entries, {b} bytes, {h:016X}')
    bad = 0
    for exe in exes:
        path = exe if os.path.isabs(exe) else os.path.join(ROOT, exe)
        problems = check(open(path, 'rb').read(), want)
        print(f'{exe}: ' + ('ok' if not problems else 'FAIL'))
        for p in problems:
            print(f'  {p}')
        bad += bool(problems)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
