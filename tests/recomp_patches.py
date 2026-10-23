#!/usr/bin/env python3
"""Tests xbe_recomp's [[patch]] instruction operand patches against assets/default.xbe: patches that apply change
exactly their instructions in the generated code and are recorded; patches that do not match the image, name no
instruction analysis decoded, or would change control flow are refused with a reason and nothing is generated; the
same input gives the same output twice; a config without patches generates what it did before patches existed.

Usage: python3 -I tests/recomp_patches.py   (about 40 s; exit status 0 when every case passes)
"""
import difflib, filecmp, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP = os.path.join(ROOT, 'out', 'build', 'linux-tools', 'xbe_recomp')
XBE = os.path.join(ROOT, 'assets', 'default.xbe')
GAME = os.path.join(ROOT, 'config', 'game.toml')
SHA = 'e7698b09ed7322b69cf4c9c6d3778bb0864a15594b30258b5637ddcf8ee8308f'

# instructions of the image (xbe_recomp --describe)
CMP_ESI = dict(at=0xA19F3, bytes='83 FE 11', insn='cmp esi, 0x11')                   # imm8, 32-bit compare
MOV_IMM = dict(at=0xA1A10, bytes='C7 05 EC 6C 63 00 11 00 00 00')                     # disp and imm
MOV_DISP = dict(at=0x9DC6A, bytes='A1 E8 6C 63 00')                                   # absolute disp only
CMP_AL = dict(at=0x1389B, bytes='3C 0D', insn='cmp al, 0x0D')                         # 8-bit operation
OR_M1 = dict(at=0x13DBF, bytes='83 C8 FF', insn='or eax, 0xFFFFFFFF')                 # -1
JL = dict(at=0x9DC78, bytes='7C A6')                                                  # branch
MID = dict(at=0xA1A11, bytes='05 EC 6C 63 00')                                        # inside MOV_IMM


def patch(base, operand, old, new, why='test', **extra):
    e = dict(base, operand=operand, old=old, new=new, why=why, **extra)
    lines = ['[[patch]]']
    for k, v in e.items():
        lines.append('%s = %s' % (k, ('"%s"' % v) if isinstance(v, str) else hex(v) if v >= 0 else str(v)))
    return '\n'.join(lines) + '\n'


def config(tmp, name, patches, sha=SHA):
    """tmp/configs/NAME/config/test.toml (the file name shows in the record, so runs to compare share it)"""
    d = os.path.join(tmp, 'configs', name, 'config')
    os.makedirs(d)
    text = 'includes = ["%s"]\n' % GAME
    if sha is not None:
        text += '[xbe]\nsha256 = "%s"\n' % sha
    path = os.path.join(d, 'test.toml')
    open(path, 'w').write(text + ''.join(patches))
    return path


def recomp(cfg, out):
    r = subprocess.run([RECOMP, cfg, XBE, out], capture_output=True, text=True)
    return r.returncode, r.stderr


def changed_lines(a, b):
    """lines that differ between two generated trees, as (file, text) of the second"""
    out = []
    cmp = filecmp.dircmp(a, b)
    cmp.diff_files = [f for f in cmp.common_files if not filecmp.cmp(os.path.join(a, f), os.path.join(b, f), shallow=False)]
    for f in cmp.diff_files:
        la, lb = open(os.path.join(a, f)).read().splitlines(), open(os.path.join(b, f)).read().splitlines()
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, la, lb, autojunk=False).get_opcodes():
            if tag != 'equal':
                out += [(f, l) for l in lb[j1:j2]]
    return sorted(out), sorted(cmp.left_only), sorted(cmp.right_only)


def main():
    if not os.path.exists(XBE) or not os.path.exists(RECOMP):
        print('needs %s and %s (cmake --build out/build/linux-tools)' % (XBE, RECOMP))
        return 1
    failed = []

    def check(name, ok, detail=''):
        print('%s %s%s' % ('ok  ' if ok else 'FAIL', name, (': ' + detail) if detail and not ok else ''))
        if not ok:
            failed.append(name)

    tmp = tempfile.mkdtemp(prefix='recomp_patches_')
    try:
        # the reference: the game's config as it is, and the same without any patch through an including config
        base = os.path.join(tmp, 'base')
        rc, err = recomp(GAME, base)
        check('reference generates', rc == 0, err[-500:])
        nop = os.path.join(tmp, 'nopatch')
        rc, err = recomp(config(tmp, 'nopatch', [], sha=None), nop)
        same = rc == 0 and not any(changed_lines(base, nop))
        check('no patches: the same code as without the feature', same)
        record = open(os.path.join(nop, 'patches.txt')).read() if rc == 0 else ''
        check('no patches: an empty record', '0 operand patches' in record and len(record.splitlines()) == 2, record)

        # patches that apply
        good = [patch(CMP_ESI, 'imm', 0x11, 0x80, why='past an imm8 range'),
                patch(MOV_IMM, 'imm', 0x11, 0x41, why='imm beside a disp'),
                patch(MOV_DISP, 'disp', 0x636CE8, 0x636CF0, why='absolute disp'),
                patch(CMP_AL, 'imm', 0x0D, 0xFF, why='8-bit, top value'),
                patch(OR_M1, 'imm', -1, 0x7FFFFFFF, why='old given as -1')]
        g1, g2 = os.path.join(tmp, 'good1'), os.path.join(tmp, 'good2')
        rc, err = recomp(config(tmp, 'good', good), g1)
        check('five patches apply', rc == 0 and '5 instruction operand patches applied' in err, err[-800:])
        if rc == 0:
            lines, only_base, only_new = changed_lines(base, g1)
            code = [l for f, l in lines if not f.endswith('.txt')]
            expect = ['0x80u', '0x41u', '0x636CF0u', 'b = (uint8_t)(0xFFFFFFFFu)', '0x7FFFFFFFu']  # an 8-bit sign extended imm: as the image's own cmp al, 0xFF
            check('each patched value is in the generated code', all(any(x in l for l in code) for x in expect),
                  '\n'.join(code))
            marks = [l for l in code if '// [[patch]]' in l]
            check('each patched instruction is marked', len(marks) == 5, '\n'.join(marks))
            comments = [l.strip() for l in code if l.strip().startswith('// 000')]
            check('only the five instructions changed', len(comments) == 5 and len(code) == 15, '\n'.join(code))
            check('no file added or removed', not only_base and not only_new,
                  '%s %s' % (only_base, only_new))
            rec = open(os.path.join(g1, 'patches.txt')).read().splitlines()
            check('the record lists every patch by address', [l.split()[0] for l in rec[2:]] ==
                  ['0001389B', '00013DBF', '0009DC6A', '000A19F3', '000A1A10'], '\n'.join(rec))
            check('the record has before and after', '| cmp esi, 0x11 | cmp esi, 0x80 |' in '\n'.join(rec), '\n'.join(rec))
            check('the record names the config', all(l.endswith('| test.toml') for l in rec[2:]), '\n'.join(rec))
            rc2, _ = recomp(config(tmp, 'good_again', good), g2)
            check('deterministic: the same output twice', rc2 == 0 and not any(changed_lines(g1, g2)))

        # patches that are refused, each alone, then several together
        bad = {
            'wrong bytes': ([patch(dict(CMP_ESI, bytes='83 FE 12'), 'imm', 0x11, 0x41)], 'the image holds 83 FE 11'),
            'wrong text': ([patch(dict(CMP_ESI, insn='cmp edi, 0x11'), 'imm', 0x11, 0x41)], 'the instruction is "cmp esi'),
            'wrong old value': ([patch(CMP_ESI, 'imm', 0x12, 0x41)], 'the patch expects old = 0x12'),
            'new too wide': ([patch(CMP_AL, 'imm', 0x0D, 0x100)], 'does not fit the 8-bit operand'),
            'negative too wide': ([patch(CMP_AL, 'imm', 0x0D, -129)], 'does not fit the 8-bit operand'),
            'new equals old': ([patch(CMP_ESI, 'imm', 0x11, 0x11)], 'new equals old'),
            'branch': ([patch(JL, 'imm', 0, 1)], 'transfers control'),
            'no such operand': ([patch(MOV_DISP, 'imm', 0x11, 0x41)], 'no imm operand'),
            'index of another kind': ([patch(MOV_IMM, 'imm', 0x11, 0x41, index=0)], 'operand 0 is not an imm operand'),
            'inside another instruction': ([patch(MID, 'imm', 0x636CEC, 0x636CF0)], 'analysis never decoded'),
            'not code': ([patch(dict(at=0x3210C0, bytes='00'), 'imm', 0, 1)], 'not decodable code'),
            'no image hash': ([patch(CMP_ESI, 'imm', 0x11, 0x41)], 'needs [xbe] sha256', None),
            'another image': ([patch(CMP_ESI, 'imm', 0x11, 0x41)], 'unsupported image', '0' * 64),
            'two for one operand': ([patch(CMP_ESI, 'imm', 0x11, 0x41), patch(CMP_ESI, 'imm', 0x11, 0x42)],
                                    'two [[patch]] entries'),
            'missing field': (['[[patch]]\nat = 0xA19F3\noperand = "imm"\nold = 0x11\nnew = 0x41\n'], 'bad [[patch]] entry'),
            'unknown operand kind': ([patch(CMP_ESI, 'reg', 0x11, 0x41)], 'bad [[patch]] entry'),
        }
        for name, case in bad.items():
            pts, want = case[0], case[1]
            sha = case[2] if len(case) > 2 else SHA
            out = os.path.join(tmp, 'bad_' + name.replace(' ', '_'))
            rc, err = recomp(config(tmp, 'bad_' + name.replace(' ', '_'), pts, sha=sha), out)
            check('refused: ' + name, rc != 0 and want in err and not os.path.exists(out), 'rc %d: %s' % (rc, err[-400:]))
        out = os.path.join(tmp, 'bad_several')
        several = [patch(dict(CMP_ESI, bytes='83 FE 12'), 'imm', 0x11, 0x41), patch(MOV_DISP, 'disp', 0x1, 0x2),
                   patch(MOV_IMM, 'imm', 0x11, 0x41)]
        rc, err = recomp(config(tmp, 'bad_several', several), out)
        check('every failure reported at once', rc != 0 and '2 of 3 [[patch]] entries cannot be applied' in err and
              'the image holds' in err and 'the patch expects old = 0x1' in err and not os.path.exists(out), err[-600:])
    finally:
        shutil.rmtree(tmp)
    print('%d failed' % len(failed) if failed else 'all passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
