#!/usr/bin/env python3
"""Tests tools/slot_audit.py on a small synthetic listing (each kind of reference it reports), then on the real
program: the committed inventory matches a fresh audit (no unexpected or missing references), the audit gives the
same output twice, every relocated table's references are patched (G8.2B checkpoint 2) and the generated patch file
matches its configs.

Usage: python3 -I tests/slot_audit_test.py   (about 30 s; exit status 0 when every case passes)
"""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUDIT = os.path.join(ROOT, 'tools', 'slot_audit.py')

MANIFEST = '''
[audit]
ranges = [[0x1000, 0x9000, "engine"]]
count_constants = [0x11]

[[table]]
name = "tab"
area = "engine"
what = "a table of 17 records of 0x10 bytes"
base = 0x500000
stride = 0x10
count = 17
also = [0x4FFFF0]
strides = [0x10]
plan = "relocate"
layout = "verified"

[[table]]
name = "obj"
area = "engine"
what = "an object's array reached through a pointer"
fields = [[0x100, 0x110]]
code = [[0x3000, 0x4000]]
plan = "grow-object"
layout = "inferred"

[[table]]
name = "ring"
area = "engine"
what = "player states in a frame ring"
base = 0x600018
stride = 0x20
count = 17
slack = 0
frame = { base = 0x600000, stride = 0x300, count = 4, field = 0x18 }
strides = [0x20, 0x300]
plan = "relocate-out"
layout = "verified"

[[site]]
at = 0x1100
role = "limit"
what = "a loop to 17"
evidence = "verified"

[[site]]
at = 0x8F00
role = "limit"
what = "not in the listing"
evidence = "verified"

[[unresolved]]
what = "something known to be open"
'''

# func, va, bytes, text, operands
LISTING = [
    (0x1000, 0x1000, 'mov eax, [0x00500020]', ['mem:1:-:-:0:500020']),              # tab direct, element 2
    (0x1000, 0x1006, 'mov eax, [ecx+0x00500110]', ['mem:1:ecx:-:0:500110']),        # tab end pointer
    (0x1000, 0x100C, 'mov eax, [ecx+0x004FFFF0]', ['mem:1:ecx:-:0:4FFFF0']),        # tab alias (base - stride)
    (0x1000, 0x1012, 'imul ecx, ecx, 0x10', ['imm:2:10:32']),                       # stride, verified (same function)
    (0x2000, 0x2000, 'imul edx, edx, 0x10', ['imm:2:10:32']),                       # stride, inferred (no table ref)
    (0x1000, 0x1100, 'cmp esi, 0x11', ['imm:1:11:32']),                             # the site
    (0x1000, 0x1200, 'cmp edi, 0x11', ['imm:1:11:32']),                             # an unreviewed candidate
    (0x1000, 0x1300, 'mov ecx, 0x11', ['imm:1:11:32']),                             # a block copy: pattern
    (0x1000, 0x1305, 'rep movsd', []),
    (0x3000, 0x3000, 'mov eax, [esi+0x104]', ['mem:1:esi:-:0:104']),                # obj field
    (0x3000, 0x3006, 'mov eax, [esi+0x200]', ['mem:1:esi:-:0:200']),                # not an obj field
    (0x5000, 0x5000, 'mov eax, [0x00600038]', ['mem:1:-:-:0:600038']),              # ring: player state 1 of frame 0
    (0x5000, 0x5005, 'mov eax, [0x00600310]', ['mem:1:-:-:0:600310']),              # ring: frame 1, field 0x10
    (0x5000, 0x500A, 'imul ecx, ecx, 0x300', ['imm:2:300:32']),                     # frame index: ecx frame-derived
    (0x5000, 0x5010, 'mov edx, [ecx+0x58]', ['mem:1:ecx:-:0:58']),                  # traced: player state via ecx
    (0x5000, 0x5016, 'mov ecx, 0x05', ['imm:1:5:32']),                              # ecx overwritten
    (0x5000, 0x501B, 'mov edx, [ecx+0x78]', ['mem:1:ecx:-:0:78']),                  # not traced any more
    (0x5000, 0x5021, 'add esi, 0x20', ['imm:1:20:32']),                             # a pointer walk by the state size
    (0x9000, 0x9000, 'cmp eax, 0x11', ['imm:1:11:32']),                             # outside the ranges: ignored
]


def write_listing(path, rows, patched=()):
    with open(path, 'w') as f:
        for func, va, text, ops in rows:
            f.write('\t'.join(['%08X' % func, '%08X' % va, '90', text] + ops + (['patched'] if va in patched else [])) + '\n')


def run(*args):
    r = subprocess.run([sys.executable, '-I', AUDIT] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def main():
    failed = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('ok  ' if ok else 'FAIL', name, (': ' + detail) if detail and not ok else ''))
        if not ok:
            failed.append(name)

    with tempfile.TemporaryDirectory(prefix='slot_audit_') as tmp:
        man, lst, out = os.path.join(tmp, 'm.toml'), os.path.join(tmp, 'l.tsv'), os.path.join(tmp, 'inv.json')
        open(man, 'w').write(MANIFEST)
        write_listing(lst, LISTING)
        rc, text = run('--manifest', man, '--listing', lst, '--out', out)
        check('synthetic audit runs', rc == 0, text)
        inv = json.load(open(out))
        tabs = {t['name']: t for t in inv['tables']}
        kinds = {(r['va'], r['kind'], r['evidence']) for r in tabs['tab']['references']}
        check('direct reference, verified, with its element', ('00001000', 'direct', 'verified') in kinds and
              [r['element'] for r in tabs['tab']['references'] if r['va'] == '00001000'] == [2], str(kinds))
        check('end pointer', ('00001006', 'end-pointer', 'verified') in kinds, str(kinds))
        check('alias address', ('0000100C', 'alias', 'verified') in kinds, str(kinds))
        check('stride beside a table reference: verified', ('00001012', 'stride', 'verified') in kinds, str(kinds))
        check('stride alone: inferred', ('00002000', 'stride', 'inferred') in kinds, str(kinds))
        okinds = {(r['va'], r['kind']) for r in tabs['obj']['references']}
        check('object field through a pointer: inferred, only its offsets', okinds == {('00003000', 'field')}, str(okinds))
        check('unpatched direct references counted', tabs['tab']['unpatched'] == 3 and tabs['tab']['patched'] == 0)
        fr = inv['frame_ring']
        ab = {(r['va'], r['kind']) for r in fr['absolute']}
        check('ring: absolute player state and frame field told apart',
              ab == {('00005000', 'playerstate'), ('00005005', 'frame-field')}, str(ab))
        check('ring: the player state element', [r['element'] for r in fr['absolute'] if r['kind'] == 'playerstate'] == [1])
        tr = {(r['va'], r['kind']) for r in fr['frame_register_accesses']}
        check('ring: access through a frame-derived register traced, until it is overwritten',
              tr == {('00005010', 'playerstate')}, str(tr))
        check('ring: pointer walk by the state size', [r['va'] for r in fr['pointer_walks']] == ['00005021'])
        sites = {s['va']: s for s in inv['sites']}
        check('site found', sites['00001100']['found'] and sites['00001100']['role'] == 'limit')
        check('missing site reported unresolved', not sites['00008F00']['found'] and
              any('00008F00' in u['what'] for u in inv['unresolved']))
        cands = {c['va']: c for c in inv['candidates']}
        check('a classified site is not a candidate', '00001100' not in cands)
        check('candidate outside the ranges ignored', '00009000' not in cands)
        check('unreviewed 17 listed as unresolved', cands['00001200']['status'] == 'unreviewed' and
              any('00001200' in u['what'] for u in inv['unresolved']))
        check('block copy classified by pattern', cands['00001300']['status'].startswith('pattern: block copy'))
        check('manifest open items carried', any(u['what'] == 'something known to be open' for u in inv['unresolved']))

        # --check: a reference the committed inventory does not have, and one it has that is gone
        lst2 = os.path.join(tmp, 'l2.tsv')
        write_listing(lst2, LISTING[1:] + [(0x7000, 0x7000, 'mov eax, [0x00500040]', ['mem:1:-:-:0:500040'])])
        rc, text = run('--manifest', man, '--listing', lst2, '--out', '-', '--check', out)
        check('--check: unexpected reference reported', rc != 0 and 'unexpected reference' in text and '00007000' in text, text)
        check('--check: vanished reference reported', 'reference gone' in text and '00001000' in text, text)
        rc, text = run('--manifest', man, '--listing', lst, '--out', '-', '--check', out)
        check('--check: the same listing passes', rc == 0, text)

        # --require-patched
        rc, text = run('--manifest', man, '--listing', lst, '--out', '-', '--require-patched', 'tab')
        check('--require-patched fails while references are unpatched', rc != 0 and 'not patched' in text, text)
        lst3 = os.path.join(tmp, 'l3.tsv')
        write_listing(lst3, LISTING, patched={0x1000, 0x1006, 0x100C})
        rc, text = run('--manifest', man, '--listing', lst3, '--out', '-', '--require-patched', 'tab')
        check('--require-patched passes once every direct reference is patched', rc == 0, text)

        # the real program
        real1, real2 = os.path.join(tmp, 'r1.json'), os.path.join(tmp, 'r2.json')
        committed = os.path.join(ROOT, 'analysis', 'player_slots_inventory.json')
        rc, text = run('--out', real1, '--check', committed)
        check('real program: matches the committed inventory', rc == 0, text[-1500:])
        rc2, _ = run('--out', real2)
        check('real program: the same inventory twice', rc2 == 0 and open(real1).read() == open(real2).read())
        import tomllib
        relocated = [t['name'] for t in tomllib.load(open(os.path.join(ROOT, 'config', 'player_slots.toml'), 'rb'))['table']
                     if t.get('relocate') and t.get('frame') is None]
        args = ['--out', '-']
        for name in relocated:
            args += ['--require-patched', name]
        rc, text = run(*args)
        check('real program: every relocated table is patched (%d tables)' % len(relocated), rc == 0, text[-1500:])
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'slot_patches.py'), '--check'],
                           capture_output=True, text=True)
        check('the generated patch file matches its configs', r.returncode == 0, r.stdout[-800:])
    print('%d failed' % len(failed) if failed else 'all passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
