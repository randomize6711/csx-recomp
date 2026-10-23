#!/usr/bin/env python3
"""Tests the guest tables: tools/guest_tables.py's layout (each table aligned, ending at a page with a guard page
after it, the same output twice, refusals with reasons, --check against the header) and src/common/guest_tables.cpp
on the runtime's guest memory (tests/guest_tables_test.cpp: mapping, alignment, zeroing, index checks, a region in
use refused, faults past the end and before the start reported with the table and element). Also that the
production header matches config/guest_tables.toml.

Usage: python3 -I tests/guest_tables_test.py   (about 20 s with the build; Linux)
"""
import os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'tools', 'guest_tables.py')
PAGE = 0x1000

TEST_LAYOUT = '''
[region]
base = 0x70000000
size = 0x00100000
[[table]]
name = "player_states"
elem = 0xFB
count = 1040
align = 1
what = "16 frames x 65 players of 0xFB bytes"
[[table]]
name = "players"
elem = 0x24C
count = 65
what = "a record per player"
[[table]]
name = "pings"
elem = 2
count = 65
align = 2
[[table]]
name = "masks"
elem = 16
count = 3
align = 16
'''


def gen(*args):
    r = subprocess.run([sys.executable, '-I', GEN] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    failed = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('ok  ' if ok else 'FAIL', name, (': ' + detail) if detail and not ok else ''))
        if not ok:
            failed.append(name)

    rc, out = gen('--check')
    check('the production header matches config/guest_tables.toml', rc == 0, out)
    with tempfile.TemporaryDirectory(prefix='guest_tables_') as tmp:
        cfg, hdr = os.path.join(tmp, 't.toml'), os.path.join(tmp, 'test_layout.h')
        open(cfg, 'w').write(TEST_LAYOUT)
        rc, out = gen('--config', cfg, '--header', hdr)
        check('the test layout generates', rc == 0, out)
        first = open(hdr).read()
        rc, _ = gen('--config', cfg, '--header', hdr)
        check('the same header twice', rc == 0 and open(hdr).read() == first)
        rc, out = gen('--config', cfg, '--header', hdr, '--check')
        check('--check passes on a current header', rc == 0, out)
        open(hdr, 'a').write('// edited\n')
        rc, out = gen('--config', cfg, '--header', hdr, '--check')
        check('--check fails on a changed header', rc != 0 and 'does not match' in out, out)
        gen('--config', cfg, '--header', hdr)
        rows = [l for l in first.splitlines() if l.strip().startswith('{"')]
        ok = True
        prev_end = 0x70000000
        for l in rows:
            m = re.match(r'\s*\{"\w+", 0x([0-9A-F]+)u, 0x([0-9A-F]+)u, (\d+)u, (\d+)u\}', l)
            va, elem, count, align = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), int(m.group(4))
            end = va + elem * count
            ok &= va % align == 0 and end % PAGE == 0 and (va & ~(PAGE - 1)) >= prev_end + PAGE
            prev_end = end
        check('each table aligned, ending at a page, a guard page before it', ok and len(rows) == 4, first)

        refusals = {
            'elem not a multiple of align': ('[region]\nbase = 0x70000000\nsize = 0x100000\n[[table]]\nname = "a"\nelem = 6\ncount = 2\nalign = 4\n', 'not a multiple of align'),
            'duplicate name': ('[region]\nbase = 0x70000000\nsize = 0x100000\n[[table]]\nname = "a"\nelem = 4\ncount = 2\n[[table]]\nname = "a"\nelem = 4\ncount = 2\n', 'two tables named'),
            'region full': ('[region]\nbase = 0x70000000\nsize = 0x10000\n[[table]]\nname = "a"\nelem = 0x1000\ncount = 16\n', 'region is full'),
            'region outside user space': ('[region]\nbase = 0x80000000\nsize = 0x10000\n', 'user range'),
            'region off 64 KB': ('[region]\nbase = 0x70001000\nsize = 0x10000\n', '64 KB'),
        }
        for name, (text, want) in refusals.items():
            p = os.path.join(tmp, 'bad.toml')
            open(p, 'w').write(text)
            rc, out = gen('--config', p, '--header', os.path.join(tmp, 'bad.h'))
            check('refused: ' + name, rc != 0 and want in out, out)

        exe = os.path.join(tmp, 'guest_tables_test')
        cmd = ['clang++', '-std=c++20', '-O1', '-g', '-include', 'unistd.h', '-I', tmp, '-I', os.path.join(ROOT, 'src'),
               '-I', os.path.join(ROOT, 'sdk', 'runtime'), os.path.join(ROOT, 'tests', 'guest_tables_test.cpp'),
               os.path.join(ROOT, 'src', 'common', 'guest_tables.cpp'), os.path.join(ROOT, 'sdk', 'runtime', 'kernel', 'memory.cpp'),
               os.path.join(ROOT, 'sdk', 'runtime', 'core', 'log.cpp'), '-o', exe, '-lpthread']
        r = subprocess.run(cmd, capture_output=True, text=True)
        check('the C++ test builds', r.returncode == 0, r.stderr[-3000:])
        if r.returncode == 0:
            r = subprocess.run([exe], capture_output=True, text=True)
            print(r.stdout, end='')
            check('the C++ test passes', r.returncode == 0, r.stderr[-1500:])
    print('%d failed' % len(failed) if failed else 'all passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
