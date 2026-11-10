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
        pointer_arguments()
    finally:
        EV.OBJECTS[:], fns, ng = saved
        EV.FUNCTIONS.clear()
        EV.FUNCTIONS.update(fns)
        EV.NOT_GUARDS.clear()
        EV.NOT_GUARDS.update(ng)


def pointer_arguments():
    """docs 15: a static address passed to a function: the fields the callee accesses through it (a register it never
    writes, a cdecl argument, through a further callee), the block helpers' ranges, and an object only taken"""
    G, G2, G3, G4, G5 = 0x18E000, 0x18E100, 0x18E200, 0x18E300, 0x18E400
    c = Code()
    # G: KeyDown-like, esi its argument (never written): reads [esi], writes [esi+8]
    c.func(G)
    c.ins(G, 'mov eax, [esi]', '{ uint32_t ea = (uint32_t)(ctx.esi); ctx.eax = (uint32_t)(X86_MEM32(ea)); }')
    c.via(G + 2, 'esi', 8)
    # G2: a cdecl argument: mov ecx, [esp+0x4]; mov [ecx+0x10], eax
    c.func(G2)
    c.ins(G2, 'mov ecx, [esp+0x04]', '{ uint32_t ea = (uint32_t)(ctx.esp + 0x4u); ctx.ecx = (uint32_t)(X86_MEM32(ea)); }')
    c.via(G2 + 4, 'ecx', 0x10)
    # G3: esi written first (not its argument): mov esi, 0x...; mov [esi+4], eax
    c.func(G3)
    c.const(G3, 'esi', 0x1234)
    c.via(G3 + 5, 'esi', 4)
    # G4: passes its esi on to G (a callee's callee), and indexes a table with ebx: [ebx+0x400720] is no field
    c.func(G4)
    c.call(G4, G)
    c.ins(G4 + 5, 'mov [ebx+0x400720], eax', '{ uint32_t ea = (uint32_t)(ctx.ebx + 0x400720u); X86_MEM32(ea) = '
                                             '(uint32_t)(ctx.eax); }')
    # G5: esi written only after a label (a jump lands there): before it, at the straight-line start, esi is the
    # argument; after the label not
    c.func(G5)
    c.via(G5, 'esi', 0xC)
    c.label(G5 + 3)
    c.via(G5 + 3, 'esi', 0x14)
    c.const(G5 + 6, 'esi', 0x99)
    # the callers, a player's frame
    c.func(P1)
    c.const(P1, 'esi', D + 0x2000)
    c.call(P1 + 5, G)
    c.push(P1 + 0x10, D + 0x2100)
    c.call(P1 + 0x15, G2)
    c.const(P1 + 0x20, 'esi', D + 0x2200)
    c.call(P1 + 0x25, G3)
    c.const(P1 + 0x30, 'esi', D + 0x2300)
    c.const(P1 + 0x35, 'ebx', 0x10)
    c.call(P1 + 0x3A, G4)
    c.const(P1 + 0x40, 'esi', D + 0x2400)
    c.call(P1 + 0x45, G5)
    # block helpers: Q_memset (edx dst, ecx count), Q_memcpy (eax dst, ecx src, edx count), memcpy (cdecl)
    c.const(P1 + 0x50, 'edx', D + 0x2500)
    c.const(P1 + 0x55, 'ecx', 0x30)
    c.call(P1 + 0x5A, 0x3DA50)
    c.const(P1 + 0x60, 'eax', D + 0x2600)
    c.const(P1 + 0x65, 'ecx', D + 0x2700)
    c.const(P1 + 0x6A, 'edx', 0x10)
    c.call(P1 + 0x6F, 0x3DAA0)
    c.push(P1 + 0x75, 0x20)
    c.push(P1 + 0x7A, D + 0x2900)
    c.push(P1 + 0x7F, D + 0x2800)
    c.call(P1 + 0x84, 0x22BF00)
    c.label(P1 + 0x90)
    c.const(P1 + 0x90, 'edx', D + 0x2A00)
    c.call(P1 + 0x95, 0x3DA50)  # ecx unknown after the label: the count unknown
    # an address only taken: by a player's code (unresolved), by the server's only (immutable)
    c.push(P1 + 0xA0, D + 0x2B00)
    c.call(P1 + 0xA5, 0x18E500)
    c.func(SRV)
    c.push(SRV, D + 0x2C00)
    c.call(SRV + 5, 0x18E500)
    cls = dict(CLS)
    cls.update({G: 'F', G2: 'F', G3: 'F', G4: 'F', G5: 'F', 0x18E500: 'F'})
    objs, errors, r = objects(c, cls)
    acc = r['accesses']

    def has(addr, kind, how, n=4):
        return any(a[3] == addr and a[2] == kind and a[5] == how and a[4] == n for a in acc)
    check(has(D + 0x2008, 'w', 'exact-call') and has(D + 0x2000, 'r', 'exact-call'),
          'pointer argument: a register the callee never writes: its fields accessed at the caller\'s address')
    check(has(D + 0x2110, 'w', 'exact-call'), 'pointer argument: a cdecl stack argument loaded from [esp+4]')
    check(not any(D + 0x2200 <= a[3] < D + 0x2300 and a[5] == 'exact-call' for a in acc),
          'pointer argument: a register the callee writes first is no argument (no false access)')
    check(has(D + 0x2308, 'w', 'exact-call'), 'pointer argument: through a callee\'s callee')
    check(not any(a[5] == 'exact-call' and a[3] >= 0x400720 and a[3] < 0x400800 for a in acc),
          'pointer argument: a large displacement is an index, not a field')
    check(has(D + 0x240C, 'w', 'exact-call') and not has(D + 0x2414, 'w', 'exact-call'),
          'pointer argument: after a label a register written later in the function is not the argument')
    check(has(D + 0x2500, 'w', 'block', 0x30), 'block helper: Q_memset with its destination and count known')
    check(has(D + 0x2600, 'w', 'block', 0x10) and has(D + 0x2700, 'r', 'block', 0x10),
          'block helper: Q_memcpy writes its destination and reads its source')
    check(has(D + 0x2800, 'w', 'block', 0x20) and has(D + 0x2900, 'r', 'block', 0x20),
          'block helper: memcpy\'s cdecl arguments from the pushes')
    check(any(u[3] == D + 0x2A00 and 'count unknown (Q_memset)' in u[5] for u in r['unresolved_blocks']),
          'block helper: a count not known is recorded unresolved, never guessed')
    o = at(objs, D + 0x2008)
    check(o and o.ownership == 'player' and o.size() == 4, 'pointer argument: a field written only by a player\'s call')
    o = at(objs, D + 0x2000)
    check(o and o.ownership == 'unresolved', 'pointer argument: the field only read, its address taken: unresolved')
    o = at(objs, D + 0x2B00)
    check(o and o.ownership == 'unresolved' and 'address taken' in o.issues,
          'an object a player\'s code only takes the address of: unresolved, not immutable')
    o = at(objs, D + 0x2C00)
    check(o and o.ownership == 'immutable', 'an object only the server takes, never written: immutable')


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
    # docs 15: the shared-once objects stay shared; the pointer scan's per-player objects are a player's
    for lo, hi, what in ((0x787EE0, 0x78A6E0, 'cl_efrags'), (0x380DA4, 0x380DA8, 'the material list pointer'),
                         (0x57F090, 0x57F0A4, 'the material list'), (0x4E50B0, 0x4E50F0, 'the secure-module machine'),
                         (0x552324, 0x552334, 'the allocator\'s block lists'), (0x6E8020, 0x6E876C, 'r_worldentity'),
                         (0x448AE8, 0x448AF0, 'the lagged-packet list'), (0x551D9C, 0x551DA0, 'the dead flag'),
                         (0x371918, 0x371924, 'the demo title\'s times')):
        check(not any(held(x) for x in range(lo, hi)), f'{what} (0x{lo:X}..0x{hi:X}) is not a player\'s (docs 15)')
    for lo, n, what in ((0x5432D8, 0xC, 'a camera kbutton'), (0x543640, 0xC, 'a kbutton'), (0x543768, 0xC, 'a kbutton'),
                        (0x551798, 0x60, 'the particle manager\'s CMiniMem'), (0x5BD8B4, 0x10, 'the particle object'),
                        (0x57EEF0, 0x1A0, 'the weather object'), (0x7493E0, 0x10, 'the server\'s module hash')):
        check(all(held(x) for x in range(lo, lo + n)), f'{what} (0x{lo:X}+0x{n:X}) is a player\'s (docs 15)')
    # the tempent pool's free list (CL_TempEntInit 0x3A8A0, run as a new player's own step, docs 15.6): a leaf whose
    # every write (its rep stosd, the stores, the link loop 0x3AEF2C..0x424A3C by 0x790) is in a player's region
    acc = json.load(open(os.path.join(M.OUT, 'accesses.json')))['accesses']
    writes = [a for a in acc if a[0] == 0x3A8A0 and a[2] == 'w']
    check(len(writes) == 4 and all(all(held(x) for x in range(a[3], a[3] + a[4])) for a in writes),
          f'CL_TempEntInit writes only a player\'s region ({len(writes)} writes)')
    check(all(held(x) for x in range(0x3AEF2C, 0x424A3C + 4, 0x790)), 'its link loop is in a player\'s region')
    body = [mn for fn, at_, mn, ops, _ in M.instructions() if fn == 0x3A8A0]
    check(body and 'call' not in body and body[-1] == 'ret', f'CL_TempEntInit calls nothing ({len(body)} instructions)')
    # the timer's once-per-process block (docs 15.7): the process runs FC870 up to FC8CC (the hook), its epilogue next
    ins = {at_: (mn, ops) for fn, at_, mn, ops, _ in M.instructions() if fn == 0xFC870 and at_ is not None}
    check(ins.get(0xFC87D) == ('jnz', '0x000FC8CC'), 'FC87D: the guard set skips the block to FC8CC')
    check(ins.get(0xFC8CC, ('',))[0] == 'cmp' and 'ebx' in ins.get(0xFC8CC, ('', ''))[1],
          'FC8CC: the block\'s end, where the timing starts (cmp ebx)')
    check([ins.get(a, ('',))[0] for a in (0xFC954, 0xFC955, 0xFC958)] == ['pop', 'add', 'ret'] and
          ins.get(0xFC955, ('', ''))[1].replace(' ', '') == 'esp,0x08', 'FC954: the epilogue (pop edi; add esp, 8; ret)')
    check(ins.get(0xFC875, ('', ''))[1].replace(' ', '') == 'esp,0x08' and ins.get(0xFC878, ('', ''))[0] == 'push',
          'the prologue (sub esp, 8; push edi) is what the epilogue undoes')
    block = [a for a in acc if a[0] == 0xFC870 and 0xFC870 <= a[1] < 0xFC8CC and a[2] == 'w']
    check(block and all(0x5C9640 <= a[3] and a[3] + (a[4] or 4) <= 0x5C9664 for a in block),
          f'the block writes only the timer\'s statics 0x5C9640..0x5C9664 ({len(block)} writes)')
    calls = [ops for at_, (mn, ops) in ins.items() if 0xFC870 <= at_ < 0xFC8CC and mn == 'call']
    check(calls == ['0x00200E3D'], f'the block calls only the frequency query ({calls})')
    qpf = [(mn, ops) for fn, at_, mn, ops, _ in M.instructions() if fn == 0x200E3D]
    check(('mov', 'dword ptr [eax], 0x2BB5C755') in qpf, 'the frequency is the constant 0x2BB5C755 (733333333)')
    others = {a[0] for a in acc if a[3] is not None and 0x5C9640 <= a[3] < 0x5C9664}
    check(others == {0xFC870}, f'only FC870 accesses the timer\'s statics ({sorted(hex(f) for f in others)})')
    for lo, what in ((0x5C9640, 'the period'), (0x5C9660, 'the guard')):
        check(any(a <= lo and lo + 4 <= a + m for a, m in prot), f'{what} (0x{lo:X}) is still protected')
        check(not held(lo), f'{what} (0x{lo:X}) is not a player\'s')
    check(not any(a <= 0x5C9648 < a + m for a, m in prot), 'the timer\'s running fields are shared, not protected')
    import tomllib
    hooks = tomllib.load(open(os.path.join(ROOT, 'config', 'game.toml'), 'rb')).get('hook', [])
    th = [h for h in hooks if h.get('call') == 'NativeSplitTimerBlockOnly']
    check(len(th) == 1 and th[0].get('at') == 0xFC8CC and th[0].get('on_true') == 0xFC954 and 'on_false' not in th[0],
          'the hook: at 0xFC8CC, on_true 0xFC954, otherwise the title\'s path')
    pool_facts(acc, prot, hooks)
    once_objects(prot)
    unresolved = sorted(o.lo for o in objs if o.ownership == 'unresolved')
    listed = open(os.path.join(ROOT, 'docs', 'native_split_review.md')).read()
    check(all(f'0x{a:X}' in listed for a in unresolved), 'every unresolved object is in the review list')


POOL = 0x562118


def pool_facts(acc, prot, hooks):
    """docs 15.8: the string-intern pool's layout, its writers and the narrow reclassification, from the generated code"""
    import re
    fns = {}
    for fn, at_, mn, ops, _ in M.instructions():
        if fn in (0x115C70, 0x115D10, 0x114680, 0x1148A0, 0x114C10, 0x22E1E7, 0x20192E, 0x114440, 0x1145C0) and at_:
            fns.setdefault(fn, []).append((at_, mn, ops))
    plain = M.pointer_summaries()
    wide = M.pointer_summaries(review=True)
    # the layout: what the constructor 0x115C70 writes through esi (0x23D170: mov esi, 0x562118)
    ctor = sorted({(o, w) for o, w, k in plain[0x115C70][('reg', 'esi')] if k == 'w'})
    check(ctor == [(0, 4), (4, 4), (8, 4), (0xC, 4), (0x10, 2), (0x12, 2), (0x14, 2), (0x16, 2), (0x18, 4), (0x1C, 4),
                   (0x20, 4), (0x24, 4)], f'the constructor lays out 0x28 bytes: 12 fields ({ctor})')
    c = dict((a, (mn, ops)) for a, mn, ops in fns[0x115C70])
    check(c.get(0x115C75) == ('mov', 'ecx, 0x115C20') and c.get(0x115C7C) == ('mov', 'ecx, 0x115BA0') and
          c.get(0x115CC4) == ('mov', 'dword ptr [esi+0x20], 0x100'), 'the less functions and the string grow 0x100')
    # the intern's writes through ebx: the wide reading's nine fields are exactly the evidence's, the model's own misses
    # the strings vector and the tree's root and count
    pool_w = sorted({(o, w) for o, w, k in wide[0x115D10][('reg', 'ebx')] if k == 'w'})
    ev = sorted((lo - POOL, n) for lo, n, _, _ in EV.REVIEWED_POOL)
    check(pool_w == ev, f'0x115D10 writes exactly the nine evidence fields ({pool_w} / {ev})')
    plain_w = {(o, w) for o, w, k in plain[0x115D10][('reg', 'ebx')] if k == 'w'}
    check({(0x10, 2), (0x12, 2), (0x18, 4), (0x1C, 4), (0x24, 4)}.isdisjoint(plain_w),
          'the model\'s own reading misses the strings vector, the root and the count (the seventh run\'s blind spot)')
    check(not {(0, 4), (0xC, 4), (0x20, 4)} & set(pool_w), 'the less function and the two grows are never written there')
    for x in (0x562130, 0x562135, 0x56213C):
        check(any(lo <= x < lo + n for lo, n, _, _ in EV.REVIEWED_POOL), f'the run\'s change 0x{x:X} is a reviewed field')
    check(all(own == 'shared-mutable' for _, _, own, _ in EV.REVIEWED_POOL), 'the fields are shared, not a player\'s')
    # narrow: nothing of the pool is protected any more, its constants are not reclassified, the neighbour stays protected
    check(not any(a < POOL + 0x28 and a + m > POOL for a, m in prot), 'no protected range left in the pool')
    check(any(a == 0x562148 and m == 4 for a, m in prot), 'the neighbour 0x562148 (the file system\'s vtable) stays protected')
    check(len(prot) == 1755 and sum(m for _, m in prot) == 29641 - 12, f'only the three ranges left the table ({len(prot)})')
    for lo, n in ((POOL, 4), (POOL + 0xC, 4), (POOL + 0x20, 4)):
        check(not any(e[0] == lo for e in EV.REVIEWED_POOL), f'0x{lo:X} (a constant field) is not reclassified')
    # its address: only the constructor, the destructor and the three interning callers take it
    takers = sorted({(a[0], a[1]) for a in acc if a[5] == 'taken' and POOL <= a[3] < POOL + 0x28})
    check([f for f, _ in takers] == [0x114680, 0x1148A0, 0x114C10, 0x23D170, 0x23FDA0],
          f'the pool\'s address is taken by the constructor, the destructor and 0x114680 / 0x1148A0 / 0x114C10 only '
          f'({[hex(f) for f, _ in takers]})')
    # 0x115D10 is the generic symbol-table add (ebx the table: 14 callers); the pool's address is never stored, so
    # only the three that take it pass this table, each in ebx straight into the call
    callers = sorted({fn for fn, at_, mn, ops, _ in M.instructions() if at_ and mn == 'call' and ops == '0x00115D10'})
    check({0x114680, 0x1148A0, 0x114C10} <= set(callers), f'the three intern this pool ({len(callers)} callers in all)')
    stores = json.load(open(os.path.join(M.OUT, 'accesses.json')))['stores']
    check(not [x for x in stores if POOL <= x[3] < POOL + 0x28], 'the pool\'s address is never stored into memory')
    for fn, load, call in ((0x114680, 0x1146DB, 0x1146E5), (0x1148A0, 0x11490D, 0x114912), (0x114C10, 0x114CAA, 0x114CAF)):
        seq = [(a, mn, ops) for a, mn, ops in fns[fn] if load <= a <= call]
        check(seq[0][1:] == ('mov', 'ebx, 0x562118') and seq[-1][1:] == ('call', '0x00115D10') and
              not any('ebx' in ops for _, _, ops in seq[1:-1]), f'0x{fn:X}: ebx = the pool, straight into the intern')
    # the lock 0x52784C (RtlEnterCriticalSection 277 = call [0x3210C0]) before the intern: 0x114680 and 0x1148A0 take
    # it; 0x114C10 does not (unresolved, region_evidence.py)
    def locked_before(fn, call_at):
        seq = [(a, mn, ops) for a, mn, ops in fns[fn] if a < call_at]
        push = [i for i, (a, mn, ops) in enumerate(seq) if mn == 'push' and ops == '0x52784C']
        return any(any(mn == 'call' and ops in ('[0x003210C0]', 'edi') for _, mn, ops in seq[i + 1:i + 3]) for i in push)
    check(locked_before(0x114680, 0x1146E5) and locked_before(0x1148A0, 0x114912), '0x114680 / 0x1148A0 hold the lock')
    check(not locked_before(0x114C10, 0x114CAF), '0x114C10 calls the intern without the lock (documented, unresolved)')
    # realloc moves a block itself (docs 15.8): RtlReAllocateHeap 0x205CFF on both paths, the hooks just after
    r = dict((a, (mn, ops)) for a, mn, ops in fns[0x22E1E7])
    check(r.get(0x22E265) == ('call', '0x00205CFF') and r.get(0x22E2B0) == ('call', '0x00205CFF'),
          'realloc calls RtlReAllocateHeap on its two paths')
    check(r.get(0x22E26A, ('',))[0] == 'mov' and r.get(0x22E2B5, ('', ''))[1].replace(' ', '') == 'eax,edi',
          'the hooks\' places: just after each call')
    check([mn for _, mn, _ in fns[0x20192E]] == ['mov', 'ret'], 'its heap is the process heap 0x5D46F8 (0x20192E)')
    rh = sorted(h['at'] for h in hooks if h.get('call') == 'NativeSplitHeapRealloc')
    check(rh == [0x22E26A, 0x22E2B5], f'the realloc hooks ({[hex(x) for x in rh]})')
    # the neighbour 0x562148: the file system object; its protected field +0 is the vtable 0x345BB0, stored only by its
    # constructor 0x114440 and destructor 0x1145C0
    vt = sorted({fn for fn, at_, mn, ops, _ in M.instructions() if at_ and '0x345BB0' in ops})
    check(vt == [0x114440, 0x1145C0], f'the vtable 0x345BB0 is stored only by 0x114440 / 0x1145C0 ({vt})')
    # intern_pool.h's layout is the evidence's
    h = open(os.path.join(ROOT, 'src', 'game', 'intern_pool.h')).read()
    k = {n: int(v, 16) for n, v in re.findall(r'k(\w+) = 0x([0-9A-F]+)', h)}
    check(sorted([k['Nodes'], k['NodesAlloc'], k['Root'], k['Count'], k['Free'], k['Next'], k['Strings'],
                  k['StringsAlloc'], k['StringsSize']]) == [lo - POOL for lo, _, _, _ in EV.REVIEWED_POOL] and
          k['Pool'] == POOL, 'intern_pool.h names the evidence\'s fields')


def once_objects(prot):
    """docs 15.9: the escape review's two remaining protected candidates are written once per process, before any
    player: their protection stays (a change in a player's steps still fails it)"""
    seqs = {}
    for fn, at_, mn, ops, _ in M.instructions():
        seqs.setdefault(fn, []).append((at_, mn, ops))
    # 0x553868 (the HUD font's object): +0x70..+0x7C written by B9420 only from the font load BC900 (ecx 0x553868)
    bc = [(a, mn, ops) for a, mn, ops in seqs[0xBC900] if a]
    check(('mov', 'ecx, 0x553868') in [(mn, ops) for _, mn, ops in bc] and
          any(mn == 'call' and ops == '0x000B9420' for _, mn, ops in bc), 'the font load passes 0x553868 to 0xB9420')
    check(any(a == 0xBCB16 and mn == 'mov' and ops == 'byte ptr [0x00552049], 0x01' for a, mn, ops in bc),
          'the font load sets the shared loaded flag 0x552049 (at the join of its paths)')
    sets = [(fn, a) for fn, seq in seqs.items() for a, mn, ops in seq if a and '[0x00552049]' in ops and
            mn == 'mov' and ops.startswith('byte ptr')]
    check(sets == [(0xBC900, 0xBCB16)], f'nothing else writes the flag, nothing clears it ({sets})')
    unguarded = []
    for fn, seq in seqs.items():
        for i, (a, mn, ops) in enumerate(seq):
            if a and mn == 'call' and ops == '0x000BC900':
                pre = [x for x in seq[max(0, i - 8):i] if x[0]]
                if not (any('[0x00552049]' in o for _, _, o in pre) and any(m == 'jnz' for _, m, _ in pre)):
                    unguarded.append(fn)
    check(unguarded == [0xAF1F0], f'every font load is guarded by the flag but the start-up\'s once-only system '
                                  f'object 0xAF1F0 ({[hex(f) for f in unguarded]})')
    for lo in (0x5538D8, 0x5538DC, 0x5538E0, 0x5538E4):
        check(any(a <= lo < a + m for a, m in prot), f'0x{lo:X} (the font\'s bundle) stays protected')
    # 0x552F3C (the loading overlays' deque): its map +4 / +8 grow only on the first push ever: 0xC2AC0 pushes one
    # (0x12AE0 -> push_back 0x12EC0) and pops it on both exits (jmp 0x12B20); the pop resets the offset when empty
    c2 = [(a, mn, ops) for a, mn, ops in seqs[0xC2AC0] if a and 0xC2AC0 <= a < 0xC2BC0]
    check(any(mn == 'call' and ops == '0x00012AE0' for _, mn, ops in c2) and
          [ops for _, mn, ops in c2 if mn == 'jmp' and ops == '0x00012B20'] == ['0x00012B20'] * 2,
          'the overlay pushes once and pops on both exits')
    pushers = sorted({fn for fn, seq in seqs.items() for a, mn, ops in seq if a and mn == 'call' and
                      ops in ('0x00012AE0', '0x00012EC0')})
    check(pushers == [0x12AE0, 0xC2AC0], f'nothing else pushes ({[hex(f) for f in pushers]})')
    pop = {a: (mn, ops) for a, mn, ops in seqs[0xC2AC0] if a and 0x12B20 <= a < 0x12B8A}
    check(pop.get(0x12B65) == ('mov', 'dword ptr [0x00552F48], 0x00'), 'the pop resets the offset when empty')
    for lo in (0x552F40, 0x552F44):
        check(any(a <= lo < a + m for a, m in prot), f'0x{lo:X} (the deque\'s map) stays protected')


def main():
    synthetic()
    real()
    print(f'region_model_test: {checks} checks, {len(failures)} failed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
