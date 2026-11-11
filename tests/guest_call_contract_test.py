#!/usr/bin/env python3
"""Static checks of the guest calls a leaving player's cleanup makes (docs/native_splitscreen.md 15.16), against the
original instructions in the generated code's comments (build/gen/game; tools/guest_regs.py). No game is started:
these show the calls are made as the title makes them, not that a run does them right (that is the runtime scenario
native_split_2p_leave's).

  - every kCleanupCalls entry (src/game/native_split_lifecycle.h): the registers it names are exactly the function's
    register inputs, its arguments the stack dwords it reads, its pops what its ret pops; it changes no callee-saved
    register; a vtable entry's slot 0 is the function and its constructor stores that vtable
  - the title's own callers of each register-input function set that register just before the call
  - RemovePlayer makes no guest call except through CallCleanup, which sets every register of a contract
  - the twelfth run's call (0x1BAD90 with edi, not esi) is refused by the same comparison
  - _Host_Frame's esi is 0 at the frame-end hook 0x5D54A (why the first two leaves could not have run 0x1BAD90)
  - the shared vectors 0xB52B0 grows through a constant header are those WatchSharedVectors watches, none in a
    player's regions
  - tools/guest_regs.py itself on made-up functions

Usage: python3 -I tests/guest_call_contract_test.py
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import guest_regs as G  # noqa: E402

failures = []
checks = 0
REG_BITS = {'kRegEax': 'eax', 'kRegEcx': 'ecx', 'kRegEdx': 'edx', 'kRegEbx': 'ebx', 'kRegEsi': 'esi', 'kRegEdi': 'edi'}


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print('FAIL', what)


def contracts():
    src = open(os.path.join(ROOT, 'src', 'game', 'native_split_lifecycle.h')).read()
    body = src[src.index('kCleanupCalls[] = {'):src.index('kCleanupCallCount')]
    out = []
    for m in re.finditer(r'\{CleanupFn::(\w+), "([^"]+)", (0x[0-9A-F]+), ([\w| ]+), (\d+), (\d+), (0x[0-9A-F]+|0), '
                         r'(true|false), (true|false)\}', body):
        regs = set() if m[4].strip() == '0' else {REG_BITS[r.strip()] for r in m[4].split('|')}
        out.append({'id': m[1], 'name': m[2], 'fn': int(m[3], 16), 'regs': regs, 'args': int(m[5]),
                    'pops': int(m[6]), 'vtable': int(m[7], 16)})
    enum = re.search(r'enum class CleanupFn \{([^}]*)\}', src)[1]
    return out, [e.strip() for e in enum.split(',') if e.strip()]


def mismatch(c, a):
    """what of a contract the function's own reading contradicts ('' when it agrees)"""
    why = []
    if a.inputs != c['regs']:
        why.append(f'registers {sorted(c["regs"])} but it reads {sorted(a.inputs)}')
    if a.stack_args != c['args']:
        why.append(f'{c["args"]} arguments but it reads {a.stack_args}')
    if a.pops != {c['pops']}:
        why.append(f'pops {c["pops"]} but its ret pops {sorted(a.pops)}')
    if a.changes_saved:
        why.append(f'changes callee-saved {sorted(a.changes_saved)}')
    return '; '.join(why)


def xbe_r32():
    d = open(os.path.join(ROOT, 'assets', 'default.xbe'), 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n, hdr = struct.unpack_from('<II', d, 0x11C)
    secs = []
    for i in range(n):
        o = hdr - base + i * 56
        _, va, _, ra, rs = struct.unpack_from('<IIIII', d, o)
        secs.append((va, ra, rs))

    def r32(va):
        for v, ra, rs in secs:
            if v <= va < v + rs:
                return struct.unpack_from('<I', d, ra + va - v)[0]
        return None
    return r32


def sets_before(ins, at, reg, window=8):
    """the register written (fully) on the straight line into the call at `at` (conditional branches passed on their
    fall-through, the path that reaches the call), within the window"""
    i = [a for a, _ in ins].index(at)
    for a, s in reversed(ins[max(0, i - window):i]):
        mn, ops = G.operands(s)
        if mn in ('jmp', 'call', 'ret'):
            return False
        if reg in G.use_def(mn, ops)[1]:
            return True
    return False


def body_of(src, head):
    i = src.index(head)
    depth, j = 0, src.index('{', i)
    for k in range(j, len(src)):
        depth += src[k] == '{'
        depth -= src[k] == '}'
        if depth == 0:
            return src[j:k + 1]
    return src[j:]


def analyser_self_test():
    funcs = {
        0x1000: [(0x1000, 'push esi'), (0x1001, 'mov eax, [esi+0x10]'), (0x1004, 'pop esi'), (0x1005, 'ret')],
        0x2000: [(0x2000, 'push edi'), (0x2001, 'mov edi, [esp+0x08]'), (0x2005, 'mov eax, [edi]'), (0x2007, 'pop edi'),
                 (0x2008, 'ret 0x04')],
        0x3000: [(0x3000, 'mov esi, ecx'), (0x3002, 'call 0x00001000'), (0x3007, 'ret')],
        0x4000: [(0x4000, 'test ebx, ebx'), (0x4002, 'jz 0x00004008'), (0x4004, 'mov eax, [ebx]'), (0x4006, 'xor ebx, ebx'),
                 (0x4008, 'ret')],
        0x5000: [(0x5000, 'push ebp'), (0x5001, 'mov ebp, esp'), (0x5003, 'mov eax, [ebp+0x0C]'), (0x5006, 'xor esi, esi'),
                 (0x5008, 'leave'), (0x5009, 'ret')],
    }
    memo = {}
    a = {fn: G.analyse(funcs, fn, memo) for fn in funcs}
    check(a[0x1000].inputs == {'esi'}, 'analyser: esi saved and read: an input; the save is not a read')
    check(a[0x2000].inputs == set() and a[0x2000].stack_args == 1 and a[0x2000].pops == {4},
          'analyser: a stack argument through a saved register, ret 4')
    check(a[0x3000].inputs == {'ecx'}, 'analyser: a callee\'s input set by the caller first is not the caller\'s')
    check(a[0x4000].inputs == {'ebx'} and a[0x4000].changes_saved == {'ebx'}, 'analyser: ebx read, then changed unsaved')
    check(a[0x5000].stack_args == 2 and a[0x5000].changes_saved == {'esi'} and 'ebp' not in a[0x5000].changes_saved,
          'analyser: an ebp frame\'s argument 2; leave restores ebp; esi changed')


def main():
    analyser_self_test()
    gen = os.path.join(ROOT, 'build', 'gen', 'game')
    if not os.path.isdir(gen):
        print('note: build/gen/game missing: the checks against the original instructions are skipped')
        print(f'guest_call_contract_test: {checks} checks, {len(failures)} failed')
        return 1 if failures else 0
    import native_split_turns_test as T
    funcs, _ = T.load()
    cl = T.callers(funcs)
    r32 = xbe_r32()
    table, enum = contracts()
    check(len(table) == len(enum) and [c['id'] for c in table] == enum, 'the table has every CleanupFn, in order')
    memo = {}
    for c in table:
        a = G.analyse(funcs, c['fn'], memo)
        why = mismatch(c, a)
        check(not why, f'{c["name"]} 0x{c["fn"]:X}: {why}')
        for caller, at in sorted(cl.get(c['fn'], ())):
            for reg in c['regs']:
                check(sets_before(funcs[caller], at, reg), f'{c["name"]}: the title\'s call at 0x{at:X} sets {reg} first')
        if c['vtable']:
            check(r32(c['vtable']) == c['fn'], f'{c["name"]}: slot 0 of vtable 0x{c["vtable"]:X} is 0x{c["fn"]:X}')
            stores = [(fn, a2) for fn, ins in funcs.items() for a2, s in ins
                      if re.match(rf'mov dword ptr \[e[a-d]x\], 0x{c["vtable"]:X}$', s)]
            check(bool(stores), f'{c["name"]}: a constructor stores vtable 0x{c["vtable"]:X}')
    # the twelfth run's call, as the comparison sees it
    d = next(c for c in table if c['fn'] == 0x1BAD90)
    bad = dict(d, regs={'edi'})
    a = G.analyse(funcs, 0x1BAD90, memo)
    check(a.inputs == {'esi'} and 'edi' not in a.inputs, '0x1BAD90 reads esi (lea ebx, [esi+0x414]), never edi')
    check(bool(mismatch(bad, a)), 'the twelfth run\'s contract (edi) is refused by the comparison')
    check(bool(mismatch(dict(d, regs=set()), a)), 'a contract with no register is refused too')
    # the source: RemovePlayer calls the title only through CallCleanup; CallCleanup sets each register it may name
    src = open(os.path.join(ROOT, 'src', 'game', 'native_split.cpp')).read()
    rp = body_of(src, 'void RemovePlayer(X86Context& ctx, int k)')
    check('kernel::CallGuest' not in rp, 'RemovePlayer makes no guest call but through CallCleanup')
    used = set(re.findall(r'CleanupFn::(\w+)', rp))
    check(used == set(enum), f'RemovePlayer uses every contract ({sorted(set(enum) - used)} unused)')
    cc = body_of(src, 'bool CallCleanup(X86Context& ctx')
    for bit, reg in REG_BITS.items():
        check(re.search(rf'c\.regs & nsplit::{bit}\) ctx\.{reg} = object', cc) is not None,
              f'CallCleanup sets {reg} when a contract names it')
    check('MeetsContract' in cc and 'VtableMatches' in cc and 'kernel::CallGuest(ctx, c.fn' in cc,
          'CallCleanup checks the contract and the vtable, and calls the contract\'s function (no target from memory)')
    # _Host_Frame (0x5D2E0): esi zeroed at 0x5D2E4, not written again before the frame-end hook at 0x5D54A
    hf = funcs[0x5D2E0]
    i0 = [a2 for a2, _ in hf].index(0x5D2E4)
    i1 = [a2 for a2, _ in hf].index(0x5D54A)
    check(hf[i0][1] == 'xor esi, esi', '_Host_Frame zeroes esi at its start')
    writes = [a2 for a2, s in hf[i0 + 1:i1] if 'esi' in G.use_def(*G.operands(s))[1]]
    check(not writes, f'nothing in _Host_Frame writes esi before 0x5D54A ({[hex(w) for w in writes]})')
    called = {int(s[5:], 16) for a2, s in hf[i0 + 1:i1] if re.match(r'call 0x[0-9A-F]{8}$', s)}
    kept = [t for t in called if 'esi' in G.analyse(funcs, t, memo).changes_saved]
    check(not kept, f'every function _Host_Frame calls before it keeps esi ({[hex(t) for t in kept]})')
    # 0xB52B0's shared vectors: the constant headers its callers pass, outside every player's region, watched
    heads = set()
    for caller, at in cl.get(0xB52B0, ()):
        ins = funcs[caller]
        i = [a2 for a2, _ in ins].index(at)
        for _, s in ins[max(0, i - 4):i]:
            m = re.match(r'mov ebx, 0x([0-9A-F]+)$', s)
            if m:
                heads.add(int(m[1], 16))
    regs = T.regions()
    check(heads == {0x553844, 0x5620A8, 0x384B60, 0x384D28}, f'0xB52B0\'s constant vectors ({sorted(map(hex, heads))})')
    check(not any(T.in_regions(h + o, regs) for h in heads for o in (0, 4, 8)), 'none of them in a player\'s regions')
    watch = body_of(src, 'void WatchSharedVectors()')
    check(all(f'0x{h:X}' in watch for h in heads), 'WatchSharedVectors watches each of them')
    print(f'guest_call_contract_test: {checks} checks, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
