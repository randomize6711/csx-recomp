#!/usr/bin/env python3
"""Tests the conservative region model (tools/region_model.py, docs/native_splitscreen.md 14) without the game.

  1. the scanner and the ownership rules on synthetic recompiled code (no XBE needed): odd-sized and unaligned
     objects, block writes (rep stos / movs, known and unknown counts), indirect accesses through a register holding a
     constant and through an unknown register, address-only (pointer-only) objects, mixed shared / per-player
     writers, unknown sizes, overlapping and cut objects, no extension to the next address, a start-up value, and the
     initialisation-flag rule (a flag guarding a shared table);
  2. the model of your own XBE (needs build/gen/game and assets/default.xbe; skipped with a note without them): no
     errors, every static cvar (646, one-character defaults included) out of the regions, the fifth run's tables
     (0x3820D0, 0x548E80, 0x552C40 and its tables) shared and protected, the process's state out of the regions, the
     regions sorted, disjoint, never between two objects, and the generated files in step with the model.
No game is started.

Usage: python3 -I tests/region_model_test.py
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import region_evidence as EV  # noqa: E402
import region_model as M  # noqa: E402

failures = []
checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print('FAIL', what)


# ---- a tiny assembler of the recompiler's output: the comment the scanner reads, and the C it reads widths from ----
MACRO = {1: 'X86_MEM8', 2: 'X86_MEM16', 4: 'X86_MEM32', 8: 'X86_MEMF64'}


class Code:
    def __init__(self):
        self.lines = []

    def func(self, at):
        self.lines.append(f'X86_FUNC(__imp__sub_{at:08X}) {{\n')

    def ins(self, at, text, c):
        self.lines.append(f'\t// {at:08X} {text}\n')
        self.lines.append(f'\t{c}\n')

    def label(self, at):
        self.lines.append(f'loc_{at:08X}:\n')

    def read(self, at, addr, w=4, reg='eax'):
        self.ins(at, f'mov {reg}, [0x{addr:08X}]',
                 f'{{ uint32_t ea = (uint32_t)(0x{addr:X}u); ctx.{reg} = (uint32_t)({MACRO[w]}(ea)); }}')

    def write(self, at, addr, w=4, src='ecx'):
        self.ins(at, f'mov [0x{addr:08X}], {src}', f'{{ uint32_t ea = (uint32_t)(0x{addr:X}u); {MACRO[w]}(ea) = '
                                                     f'(uint32_t)(ctx.{src}); }}')

    def store(self, at, addr, value):
        self.ins(at, f'mov dword ptr [0x{addr:08X}], 0x{value:02X}',
                 f'{{ uint32_t ea = (uint32_t)(0x{addr:X}u); X86_MEM32(ea) = (uint32_t)(0x{value:X}u); }}')

    def cmp(self, at, addr):
        self.ins(at, f'cmp dword ptr [0x{addr:08X}], 0x00',
                 f'{{ uint32_t ea = (uint32_t)(0x{addr:X}u); uint32_t a = X86_MEM32(ea); zf = a == 0; }}')

    def const(self, at, reg, value):
        self.ins(at, f'mov {reg}, 0x{value:X}', f'{{ ctx.{reg} = (uint32_t)(0x{value:X}u); }}')

    def via(self, at, reg, disp, w=4):  # mov [reg+disp], eax
        self.ins(at, f'mov [{reg}+0x{disp:X}], eax', f'{{ uint32_t ea = (uint32_t)(ctx.{reg} + 0x{disp:X}u); '
                                                     f'{MACRO[w]}(ea) = (uint32_t)(ctx.eax); }}')

    def indexed(self, at, base, w=4):  # mov [eax*4+base], edx
        self.ins(at, f'mov [eax*4+0x{base:X}], edx', f'{{ uint32_t ea = (uint32_t)(ctx.eax * 4 + 0x{base:X}u); '
                                                     f'{MACRO[w]}(ea) = (uint32_t)(ctx.edx); }}')

    def push(self, at, value):
        self.ins(at, f'push 0x{value:X}', f'{{ uint32_t v = (uint32_t)(0x{value:X}u); X86_PUSH32(v); }}')

    def rep(self, at, op):
        self.ins(at, f'rep {op}', f'{{ x86_rep_{op[:4]}(ctx, base, {dict(b=1, w=2, d=4)[op[-1]]}); }}')

    def call(self, at, target):
        self.ins(at, f'call 0x{target:08X}', f'{{ X86_PUSH32(0x{at + 5:X}u); sub_{target:08X}(ctx, base); }}')


def scan(code):
    return M.scan([code.lines], write=False)


def objects(code, cls, evidence=(), cvars=(), hand=()):
    """the scan, the objects and their ownership (cls: function -> its classes, e.g. 'F' a player's frame)"""
    r = scan(code)
    EV.OBJECTS[:] = list(evidence)
    objs, errors = M.build_objects(r['accesses'], list(cvars), list(evidence), list(hand))
    icls = M.instruction_classes(r['accesses'], cls)
    for a in r['accesses']:
        icls.setdefault(('fns', a[1]), set()).add(a[0])
    M.classify_objects(objs, cls, icls)
    holder = M.holder_of(objs)
    for fn, fo, o in M.flag_conflicts(objs, r['stores'], holder):
        errors.append(f'flag 0x{fo.lo:X} guards 0x{o.lo:X} in 0x{fn:X}')
    return objs, errors, r


def at(objs, a):
    return next((o for o in objs if o.lo <= a < o.hi), None)


D = 0x500000  # a .data address of the synthetic image
P1, P2, SRV, START, SVC = 0x18C000, 0x18D000, 0x9C000, 0x5D9E0, 0xB17C0
CLS = {P1: 'F', P2: 'FI', SRV: 'S', START: 'P', SVC: 'FP'}


def synthetic():
    saved = (list(EV.OBJECTS), dict(EV.FUNCTIONS), dict(EV.NOT_GUARDS))
    EV.FUNCTIONS.clear()
    EV.NOT_GUARDS.clear()
    try:
        # odd-sized and unaligned: a byte at D+1 and a word at D+2 overlap nothing; a dword at D+5 and a byte at D+7
        # overlap: one 4-byte object at D+5 (its width), nothing extended to the next address (D+0x40)
        c = Code()
        c.func(P1)
        c.write(P1, D + 1, 1)
        c.write(P1 + 6, D + 2, 2)
        c.write(P1 + 12, D + 5, 4)
        c.write(P1 + 18, D + 7, 1)
        c.write(P1 + 24, D + 0x40, 4)
        objs, errors, _ = objects(c, CLS)
        check(at(objs, D + 1).size() == 1 and at(objs, D + 2).size() == 2, 'odd sizes: a byte and a word, each its width')
        check(at(objs, D + 5).lo == D + 5 and at(objs, D + 5).size() == 4, 'unaligned: an overlap merges, at D+5, 4 bytes')
        check(at(objs, D + 9) is None and at(objs, D + 0x3F) is None, 'no extension to the next referenced address')
        check(all(o.ownership == 'player' for o in objs), 'written only by a player\'s frame, extent proven: per-player')
        check(not errors, 'no errors')
        # block writes: rep stosd with edi / ecx known -> the exact range; with ecx unknown -> unresolved, recorded
        c = Code()
        c.func(P1)
        c.const(P1, 'ecx', 0x10)
        c.const(P1 + 5, 'edi', D + 0x100)
        c.rep(P1 + 10, 'stosd')
        c.label(P1 + 12)
        c.const(P1 + 12, 'edi', D + 0x200)
        c.rep(P1 + 17, 'stosd')  # ecx unknown after the label
        c.const(P1 + 19, 'esi', D + 0x100)
        c.const(P1 + 24, 'edi', D + 0x300)
        c.const(P1 + 29, 'ecx', 0x8)
        c.rep(P1 + 34, 'movsd')
        r = scan(c)
        blocks = [a for a in r['accesses'] if a[5] == 'block']
        check([D + 0x100, 0x40] in [[a[3], a[4]] for a in blocks if a[2] == 'w'], 'rep stosd: 0x10 dwords at D+0x100')
        check(any(u[3] == D + 0x200 and u[5] == 'count unknown' for u in r['unresolved_blocks']),
              'rep stosd with an unknown count: unresolved, not guessed')
        check([D + 0x300, 0x20, 'w'] in [[a[3], a[4], a[2]] for a in blocks] and
              [D + 0x100, 0x20, 'r'] in [[a[3], a[4], a[2]] for a in blocks], 'rep movsd: the copy\'s both sides')
        objs, errors, _ = objects(c, CLS)
        check(at(objs, D + 0x13F) is not None and at(objs, D + 0x13F).lo == D + 0x100, 'a block is one object, whole')
        # indirect accesses: through a register holding a constant (exact), through an unknown register (unbounded)
        c = Code()
        c.func(P1)
        c.const(P1, 'esi', D + 0x400)
        c.via(P1 + 5, 'esi', 0x10, 2)
        c.call(P1 + 8, P2)  # esi is callee-saved: still known
        c.via(P1 + 13, 'esi', 0x14, 4)
        c.indexed(P1 + 16, D + 0x500)
        objs, errors, r = objects(c, CLS)
        check(any(a[3] == D + 0x410 and a[4] == 2 and a[5] == 'exact-reg' for a in r['accesses']),
              'through a constant register: an exact access (D+0x410, 2 bytes)')
        check(any(a[3] == D + 0x414 and a[5] == 'exact-reg' for a in r['accesses']), 'esi kept across a call')
        o = at(objs, D + 0x500)
        check(o is not None and o.ownership == 'unresolved' and 'extent unknown' in o.issues,
              'indexed by an unknown register: its size unknown, unresolved (not per-player)')
        # pointer-only objects: an address taken (push) and nothing else: unbounded, unresolved when a player writes
        # next to it; a scalar whose address is passed on is not proven either
        c = Code()
        c.func(P1)
        c.push(P1, D + 0x600)
        c.write(P1 + 5, D + 0x700)
        c.push(P1 + 11, D + 0x700)
        objs, errors, _ = objects(c, CLS)
        check(at(objs, D + 0x600).source == 'unbounded' and at(objs, D + 0x600).ownership != 'player',
              'an address-only object: unbounded, never per-player')
        check(at(objs, D + 0x700).ownership == 'unresolved', 'a written scalar whose address escapes: unresolved')
        # mixed writers: a player's frame and a shared service; the server; the start-up and a player (a start-up
        # value a new player's steps do not redo)
        c = Code()
        c.func(P1)
        c.write(P1, D + 0x800)
        c.write(P1 + 6, D + 0x810)
        c.write(P1 + 12, D + 0x820)
        c.write(P1 + 18, D + 0x830)
        c.func(SVC)
        c.write(SVC, D + 0x800)
        c.func(SRV)
        c.write(SRV, D + 0x810)
        c.read(SRV + 6, D + 0x830)
        c.func(START)
        c.write(START, D + 0x820)
        c.write(START + 6, D + 0x840)
        objs, errors, _ = objects(c, CLS)
        check(at(objs, D + 0x800).ownership == 'unresolved', 'a player and a shared service write it: unresolved')
        check(at(objs, D + 0x810).ownership == 'unresolved', 'a player and the server write it: unresolved')
        check(at(objs, D + 0x820).ownership == 'unresolved', 'the start-up and a player write it: unresolved')
        check(at(objs, D + 0x830).ownership == 'unresolved', 'a player writes it, the server reads it: unresolved')
        check(at(objs, D + 0x840).ownership == 'process', 'only the start-up writes it: process (protected)')
        # overlapping evidence; accesses cutting an evidence object or a cvar: errors
        c = Code()
        c.func(P1)
        c.write(P1, D + 0x9FE, 4)  # D+0x9FE..0xA02 crosses the evidence object at D+0xA00
        objs, errors, _ = objects(c, CLS, evidence=[(D + 0xA00, 0x10, 'player', 't'), (D + 0xA08, 0x10, 'shared-mutable', 't')])
        check(any('overlaps' in e for e in errors), 'two evidence objects overlapping: an error')
        check(any('cut the evidence object' in e for e in errors), 'an access cutting an evidence object: an error')
        c = Code()
        c.func(P1)
        c.write(P1, D + 0xB10, 4)  # a cvar's next link (D+0xB00, 20 bytes): its +0x10
        objs, errors, _ = objects(c, CLS, cvars=[(D + 0xB00, 20, 'the static cvar t')])
        check(at(objs, D + 0xB10).ownership == 'registry', 'a static cvar\'s field is the registry\'s, whoever writes it')
        c = Code()
        c.func(P1)
        c.write(P1, D + 0xBFE, 4)  # crosses into the cvar at D+0xC00
        objs, errors, _ = objects(c, CLS, cvars=[(D + 0xC00, 20, 'the static cvar u')])
        check(any('cut the cvar object' in e for e in errors), 'an access cutting a cvar: an error')
        # the initialisation flag: f reads F, builds a table (rep stosd), stores 1 in F. A per-player flag over a
        # shared table (the start-up builds it too) is an error; flag and table with one ownership (evidence) is not
        c = Code()
        c.func(P1)
        c.cmp(P1, D + 0xD00)
        c.const(P1 + 7, 'ecx', 0x80)
        c.const(P1 + 12, 'edi', D + 0x1000)
        c.rep(P1 + 17, 'stosd')
        c.store(P1 + 19, D + 0xD00, 1)
        c.func(START)
        c.write(START, D + 0x1000)  # the start-up writes the table as well: not a player's
        objs, errors, _ = objects(c, CLS)
        check(any('flag 0x%X guards 0x%X' % (D + 0xD00, D + 0x1000) in e for e in errors),
              'a per-player flag guarding a shared table: an error')
        objs, errors, _ = objects(c, CLS, evidence=[(D + 0xD00, 4, 'process', 't'), (D + 0x1000, 0x200, 'process', 't')])
        check(not any('guards' in e for e in errors), 'flag and table shared together: no error')
        # unknown sizes in the evidence's absence: the review's; with evidence, its size and ownership
        c = Code()
        c.func(P1)
        c.indexed(P1, D + 0xE00)
        objs, _, _ = objects(c, CLS, evidence=[(D + 0xE00, 0x80, 'player', 'a loop bound of 32')])
        check(at(objs, D + 0xE7F).ownership == 'player' and at(objs, D + 0xE00).size() == 0x80,
              'an evidence entry gives the size and the ownership')
    finally:
        EV.OBJECTS[:], fns, ng = saved
        EV.FUNCTIONS.clear()
        EV.FUNCTIONS.update(fns)
        EV.NOT_GUARDS.clear()
        EV.NOT_GUARDS.update(ng)


def real():
    if not os.path.isdir(M.GEN) or not os.path.exists(M.XBE):
        print('note: build/gen/game or assets/default.xbe missing (your own XBE): the model check is skipped')
        return
    for step in (['state_audit.py', 'build'], ['region_model.py', 'scan']):
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', step[0]), step[1]], capture_output=True,
                           text=True)
        check(r.returncode == 0, f'{" ".join(step)} runs')
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        objs, errors, cls, icls = M.model()
    check(not errors, f'the model has no errors ({errors[:3]})')
    cvars = M.SA.static_cvars()
    check(len(cvars) == 646, f'646 static cvars ({len(cvars)})')
    names = {w.split()[-1] for _, _, w in cvars}
    check({'v_dark', 'cl_fixtimerate', 'v_iyaw_cycle'} <= names, 'cvars with one-character defaults found')
    inc = open(os.path.join(ROOT, 'src', 'game', 'client_context_regions.inc')).read()
    import re
    regs = [(int(a, 16), int(n, 16)) for a, n in re.findall(r'\{0x([0-9A-F]+), 0x([0-9A-F]+), "', inc)]
    model = json.load(open(os.path.join(M.OUT, 'model.json')))
    check(regs == [tuple(r) for r in model['regions']], 'client_context_regions.inc is the model\'s (regenerate)')
    for (a, n), (b, _) in zip(regs, regs[1:]):
        check(a + n <= b, f'regions sorted and disjoint at 0x{a:X}')

    def held(x):
        return any(a <= x < a + n for a, n in regs)
    for lo, n, what in cvars:
        check(not any(held(x) for x in range(lo, lo + n)), f'{what} is not a player\'s')
    for lo, hi, what in ((0x3820D0, 0x384B48, 'the allocator\'s statistics'), (0x548E80, 0x548F34, 'the studio interface'),
                         (0x543AD0, 0x5454D0 + 0x200, 'the material tables'), (0x552C38, 0x552C44, 'their count and flag'),
                         (0x379FA4, 0x379FB0, 'the system and video objects'), (0x551928, 0x55192C, 'cvar_vars'),
                         (0x551FD4, 0x551FF8, 'the video-mode object'), (0x553614, 0x553620, 'a system object made once'),
                         (0x5EECA0, 0x5EEEA0, 'the display-mode list'), (0x5EF4E0, 0x5EF670, 'com_argv'),
                         (0x6CE204, 0x6CE224, 'the window rectangle'), (0x6A84C8, 0x6A8FC4, 'the GL dispatch table')):
        check(not any(held(x) for x in range(lo, hi)), f'{what} (0x{lo:X}..0x{hi:X}) is not a player\'s')
    prot = model['protected']
    for lo, n, what in ((0x552C40, 4, 'the material tables\' flag'), (0x543AD0, 0x1A00, 'the texture-name table'),
                        (0x5454D0, 0x200, 'the texture-type table')):
        check(any(a <= lo and lo + n <= a + m for a, m in prot), f'{what} is protected')
    # shared but not protected: the studio interface (the fog reset 0x1CC5FF stores 0 in its first dword while playing)
    check(not any(a <= 0x548E80 < a + m for a, m in prot), 'the studio interface is shared, not protected')
    # every region is objects end to end: no byte between two objects (nothing extended)
    owners = {}
    for lo, n, own, src, roles, why in model['objects']:
        if own == 'player':
            for x in range(lo, lo + n):
                owners[x] = True
    loose = [a for a, n in regs if not all(owners.get(x) for x in range(a, a + n))]
    check(not loose, f'every region byte belongs to a per-player object ({[hex(a) for a in loose[:5]]})')


def main():
    synthetic()
    real()
    print(f'region_model_test: {checks} checks, {len(failures)} failed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
