#!/usr/bin/env python3
"""Tests the region model's build verification without the game (docs/native_splitscreen.md 15.7):
- src/game/table_fingerprint.h (compiled with the real tables) and tools/check_tables.py hash alike;
- an executable compiled from an older table is found stale, though it compiled and links: a program built from a
  copy of client_context_regions.inc with one entry less, and one from a table with one size changed;
- a missing stamp and two differing stamps fail; the same stamp twice does not;
- sdk/harness/build.sh's rebuild rule counts the generated *.inc tables (patches/sdk/0046): a newer .inc than an
  object makes it recompile, as a newer .h does;
- the executables that exist (build/harness, build/window, build/server) hold this tree's tables.
Builds small programs with clang++ in a temporary folder; starts no game.

Usage: python3 -I tests/check_tables_test.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import check_tables as CT  # noqa: E402

failed = checks = 0


def check(ok, what):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print(f'FAIL {what}')


PROGRAM = r'''
#include <cstdio>
#include "game/table_fingerprint.h"
struct R { unsigned va, size; const char* what; };
constexpr R kRegions[] = {
#include "REGIONS"
};
constexpr R kProtected[] = {
#include "PROTECTED"
};
constexpr unsigned kAddresses[] = {
#include "ADDRESSES"
};
[[gnu::used]] const tablefp::Stamp a = {"CSXFP1:regions", tablefp::Ranges(kRegions),
    unsigned(sizeof(kRegions) / sizeof(kRegions[0])), tablefp::Bytes(kRegions)};
[[gnu::used]] const tablefp::Stamp b = {"CSXFP1:protect", tablefp::Ranges(kProtected),
    unsigned(sizeof(kProtected) / sizeof(kProtected[0])), tablefp::Bytes(kProtected)};
[[gnu::used]] const tablefp::Stamp c = {"CSXFP1:address", tablefp::Words(kAddresses),
    unsigned(sizeof(kAddresses) / sizeof(kAddresses[0])), 0};
int main() {
    std::printf("%016llX %u %u\n%016llX %u %u\n%016llX %u %u\n", (unsigned long long)a.hash, a.count, a.bytes,
                (unsigned long long)b.hash, b.count, b.bytes, (unsigned long long)c.hash, c.count, c.bytes);
}
'''


def build(tmp, name, regions, protected, addresses):
    """a program from these table files: (its bytes, its printed fingerprints)"""
    src = os.path.join(tmp, name + '.cpp')
    text = PROGRAM.replace('"REGIONS"', f'"{regions}"').replace('"PROTECTED"', f'"{protected}"')
    open(src, 'w').write(text.replace('"ADDRESSES"', f'"{addresses}"'))
    exe = os.path.join(tmp, name)
    r = subprocess.run(['clang++', '-std=c++20', '-O2', '-Wall', '-Werror', '-I', os.path.join(ROOT, 'src'), src, '-o',
                        exe], capture_output=True, text=True)
    check(r.returncode == 0, f'{name} builds ({r.stderr[:300]})')
    if r.returncode:
        return b'', []
    out = subprocess.run([exe], capture_output=True, text=True).stdout.split('\n')
    return open(exe, 'rb').read(), [tuple(int(x, 16) if i == 0 else int(x) for i, x in enumerate(line.split()))
                                     for line in out if line]


def stamp(name, fp):
    return (b'CSXFP1:' + name.encode()).ljust(16, b'\0') + struct.pack('<QII', *fp)


def main():
    game = os.path.join(ROOT, 'src', 'game')
    inc = {k: os.path.join(game, fn) for k, (fn, _) in CT.TABLES.items()}
    want = CT.expected()
    with tempfile.TemporaryDirectory() as tmp:
        # the real tables: C++ (compile time) and Python alike, and the program's bytes match
        data, printed = build(tmp, 'current', inc['regions'], inc['protect'], inc['address'])
        check(printed == [want['regions'], want['protect'], want['address']],
              f'table_fingerprint.h and check_tables.py hash alike ({printed} / {want})')
        check(CT.check(data, want) == [], f'a program compiled from the tree\'s tables matches ({CT.check(data, want)})')
        # an older table: one entry less (a regeneration since), one size changed; compiled and linked fine
        lines = open(inc['regions']).read().split('\n')
        entries = [i for i, line in enumerate(lines) if line.startswith('{0x')]
        older = os.path.join(tmp, 'older_regions.inc')
        open(older, 'w').write('\n'.join(lines[:entries[3]] + lines[entries[3] + 1:]))
        data, _ = build(tmp, 'stale_count', older, inc['protect'], inc['address'])
        p = CT.check(data, want)
        check(len(p) == 1 and p[0].startswith('regions: STALE') and f'{len(entries) - 1} entries' in p[0],
              f'a table with one entry less is stale ({p})')
        resized = os.path.join(tmp, 'resized_regions.inc')
        e = lines[entries[5]]
        m = re.match(r'\{0x([0-9A-F]+), 0x([0-9A-F]+),', e)
        lines2 = list(lines)
        lines2[entries[5]] = e.replace(f'0x{m.group(2)},', f'0x{int(m.group(2), 16) + 4:X},', 1)
        open(resized, 'w').write('\n'.join(lines2))
        data, _ = build(tmp, 'stale_size', resized, inc['protect'], inc['address'])
        p = CT.check(data, want)
        check(len(p) == 1 and p[0].startswith('regions: STALE'), f'a table with one size changed is stale ({p})')
        # an older protected table too: found per table
        plines = open(inc['protect']).read().split('\n')
        pe = [i for i, line in enumerate(plines) if line.startswith('{0x')]
        pold = os.path.join(tmp, 'older_protected.inc')
        open(pold, 'w').write('\n'.join(plines[:pe[0]] + plines[pe[0] + 1:]))
        data, _ = build(tmp, 'stale_protect', inc['regions'], pold, inc['address'])
        p = CT.check(data, want)
        check(len(p) == 1 and p[0].startswith('protect: STALE'), f'an older protected table is stale ({p})')
    # synthetic bytes: missing, doubled, conflicting
    good = b'x' * 7 + stamp('regions', want['regions']) + stamp('protect', want['protect']) + \
        stamp('address', want['address'])
    check(CT.check(good, want) == [], 'the three stamps: ok')
    check(CT.check(good + stamp('regions', want['regions']), want) == [], 'the same stamp twice: ok')
    bad = want['regions'][0] ^ 1, want['regions'][1], want['regions'][2]
    p = CT.check(good + stamp('regions', bad), want)
    check(len(p) == 1 and 'differ' in p[0], f'two different stamps of a table fail ({p})')
    p = CT.check(stamp('regions', want['regions']), want)
    check(len(p) == 2 and all('no stamp' in x for x in p), f'a missing stamp fails ({p})')
    check(CT.check(b'no stamps at all', want) != [], 'an executable without stamps fails')
    # the build rule (sdk/harness/build.sh, patch 0046): its `newest` command names a newer .inc
    script = open(os.path.join(ROOT, 'sdk', 'harness', 'build.sh')).read()
    line = [x for x in script.split('\n') if x.startswith('newest=')]
    check(len(line) == 1 and "-name '*.inc'" in line[0], 'build.sh counts *.inc as headers')
    check(os.path.exists(os.path.join(ROOT, 'patches', 'sdk', '0046-harness-rebuilds-on-included-tables.patch')),
          'the rule is the sdk patch 0046 (kept by tools/apply_sdk_patches.sh)')
    if len(line) == 1:
        with tempfile.TemporaryDirectory() as tmp:
            for d in ('rt', 'repo/src/game', 'h', 'gen', 'out'):
                os.makedirs(os.path.join(tmp, d))
            files = {'rt/a.h': 100, 'repo/src/game/x.h': 200, 'h/b.h': 150, 'gen/image_symbols.h': 120,
                     'repo/src/game/x.cpp': 210, 'out/x.o': 300, 'repo/src/game/table.inc': 400}
            for f, t in files.items():
                open(os.path.join(tmp, f), 'w').write('')
                os.utime(os.path.join(tmp, f), (1700000000 + t, 1700000000 + t))
            test = (f'RT={tmp}/rt REPO={tmp}/repo H={tmp}/h GEN={tmp}/gen\n{line[0]}\necho "$newest"\n'
                    f'o={tmp}/out/x.o f={tmp}/repo/src/game/x.cpp\n'
                    '[ "$o" -nt "$f" ] && [ "$o" -nt "$newest" ] && echo up-to-date || echo recompile\n')
            out = subprocess.run(['bash', '-c', test], capture_output=True, text=True).stdout.split()
            check(out[:1] == [f'{tmp}/repo/src/game/table.inc'], f'the newest is the regenerated table ({out})')
            check(out[1:] == ['recompile'], 'an object older than the table is recompiled')
            os.utime(os.path.join(tmp, 'repo/src/game/table.inc'), (1700000250, 1700000250))
            out = subprocess.run(['bash', '-c', test], capture_output=True, text=True).stdout.split()
            check(out[1:] == ['up-to-date'], f'an object newer than every header and table is kept ({out})')
    # the real executables
    exes = [e for e in CT.DEFAULT_EXES if os.path.exists(os.path.join(ROOT, e))]
    if not exes:
        print('note: no executable built: the real check is skipped')
    for e in exes:
        p = CT.check(open(os.path.join(ROOT, e), 'rb').read(), want)
        check(not p, f'{e} holds this tree\'s tables ({p})')
    print(f'check_tables_test: {checks} checks, {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
