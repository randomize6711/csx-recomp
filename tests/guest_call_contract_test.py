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
                         r'(-1|0x[0-9A-F]+), (true|false), (true|false)\}', body):
        regs = set() if m[4].strip() == '0' else {REG_BITS[r.strip()] for r in m[4].split('|')}
        out.append({'id': m[1], 'name': m[2], 'fn': int(m[3], 16), 'regs': regs, 'args': int(m[5]),
                    'pops': int(m[6]), 'vtable': int(m[7], 16), 'slot': -1 if m[8] == '-1' else int(m[8], 16),
                    'frees': m[9] == 'true'})
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


def xbe_refs(value):
    """every aligned dword of the XBE's sections equal to value (its address)"""
    d = open(os.path.join(ROOT, 'assets', 'default.xbe'), 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n, hdr = struct.unpack_from('<II', d, 0x11C)
    out, pat = [], struct.pack('<I', value)
    for i in range(n):
        _, va, _, ra, rs = struct.unpack_from('<IIIII', d, hdr - base + i * 56)
        j = d.find(pat, ra, ra + rs)
        while j >= 0:
            if (j - ra) % 4 == 0:
                out.append(va + j - ra)
            j = d.find(pat, j + 1, ra + rs)
    return out


def undefined_read_after(ins, at, regs=('eax', 'ecx', 'edx'), window=16):
    """a register of regs read after the call at `at` before the caller writes it (on the fall-through path), or None"""
    i = [a for a, _ in ins].index(at)
    live = set(regs)
    for a, s in ins[i + 1:i + 1 + window]:
        mn, ops = G.operands(s)
        use, dfn = G.use_def(mn, ops)
        m = re.match(r'(?:or (e[a-z]{2}), 0xFFFFFFFF|(?:xor|sub) (e[a-z]{2}), (e[a-z]{2}))$', s)
        if m and (m[1] or m[2] == m[3]):  # a constant whatever the register held: a write, not a read
            use = []
        if live & set(use):
            return f'0x{a:X} {s}'
        live -= set(dfn)
        if not live or mn in ('jmp', 'ret', 'call') or mn.startswith('j'):
            return None
    return None


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
    regs = T.regions()
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
    used = set(re.findall(r'CleanupFn::(\w+)', rp + body_of(src, 'void DetachViewport(X86Context& ctx, int k, int context)')))
    check(used == set(enum), f'RemovePlayer (and its DetachViewport) uses every contract ({sorted(set(enum) - used)} unused)')
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
    check(heads == {0x553844, 0x5620A8, 0x384B60, 0x384D28}, f'0xB52B0\'s constant vectors ({sorted(map(hex, heads))})')
    check(not any(T.in_regions(h + o, regs) for h in heads for o in (0, 4, 8)), 'none of them in a player\'s regions')
    watch = body_of(src, 'void WatchSharedVectors()')
    check(all(f'0x{h:X}' in watch for h in heads), 'WatchSharedVectors watches each of them')
    vgui_checks(funcs, cl, r32, table, src, memo, regs)
    print(f'guest_call_contract_test: {checks} checks, {len(failures)} failed')
    return 1 if failures else 0


def vgui_checks(funcs, cl, r32, table, src, memo, regs):
    """docs 15.17: the VGUI hierarchy as the instructions have it, the grow's wrapper and the detach's contract"""
    import native_split_turns_test as T
    lh = open(os.path.join(ROOT, 'src', 'game', 'native_split_lifecycle.h')).read()
    const = {m[1]: int(m[2], 16) for m in re.finditer(r'(k\w+) = (0x[0-9A-F]+)', lh)}
    # the Panel: its constructor reserves 4 children at +0x2C (16 bytes from 0xB52DA, the blocked block's size and
    # site); setParent / removeChild / addChild at slots 0x40 / 0x4C / 0x44 of every class built on it
    ctor = funcs[0x10A4E0]
    check(any(s == 'lea ebx, [esi+0x2C]' for _, s in ctor[:8]) and any(s == 'call 0x000B52B0' for _, s in ctor[:12]),
          'Panel 0x10A4E0 reserves its children vector at +0x2C through 0xB52B0')
    vts = [h - 0x40 for h in xbe_refs(0x10ACC0)]
    check(len(vts) >= 20 and all(r32(v + 0x4C) == 0x10ADC0 and r32(v + 0x44) == 0x10AD00 for v in vts),
          f'every one of the {len(vts)} panel vtables has removeChild 0x10ADC0 at 0x4C and addChild at 0x44')
    check(const.get('kPanelChildren') == 0x2C and const.get('kPanelParent') == 0x38 and
          const.get('kPanelSetParentSlot') == 0x40 and const.get('kPanelRemoveChildSlot') == 0x4C and
          const.get('kPanelSetParent') == 0x10ACC0 and const.get('kPanelRemoveChild') == 0x10ADC0 and
          const.get('kViewport') == 0x552B7C and const.get('kViewportVtable') == 0x35FD10 and
          const.get('kVectorGrow') == 0xB52B0, 'the lifecycle header\'s VGUI constants are the instructions\'')
    sp = [s for _, s in funcs[0x10ACC0]]
    check(sp[:9] == ['push esi', 'mov esi, ecx', 'mov ecx, [esi+0x38]', 'test ecx, ecx', 'jz 0x0010ACD0',
                     'mov eax, [ecx]', 'push esi', 'call [eax+0x4C]', 'mov eax, [esp+0x08]'] and
          'mov [esi+0x38], eax' in sp and 'add eax, 0x2C' in sp and 'call 0x000B5220' in sp,
          'setParent 0x10ACC0: removeChild on the old parent ([+0x38], slot 0x4C), then appended at the new one\'s +0x2C')
    # every call of slot 0x4C in the Panel's code: ecx the parent ([child+0x38]), the child pushed
    for fn in (0x10ACC0, 0x10AD00, 0x10AD60):
        ins = funcs[fn]
        at = [a for a, s in ins if s == 'call [eax+0x4C]']
        ok = bool(at)
        for a in at:
            prev = [s for b, s in ins if b < a][-5:]
            ok &= any(re.match(r'mov ecx, \[e[a-z]{2}\+0x38\]', s) for s in prev) and prev[-1].startswith('push e')
        check(ok, f'0x{fn:X}: its removeChild call is `mov ecx, [child+0x38]; ...; push child; call [eax+0x4C]`')
    rc = [s for _, s in funcs[0x10ADC0]]
    check(rc == ['push edi', 'mov edi, [esp+0x08]', 'xor eax, eax', 'add ecx, 0x2C', 'mov [edi+0x38], eax',
                 'mov [edi+0x3C], eax', 'call 0x000B51B0', 'pop edi', 'ret 0x04'],
          'removeChild 0x10ADC0: the child\'s parent and +0x3C cleared, 0xB51B0 on the parent\'s +0x2C, ret 4')
    rm = [s for _, s in funcs[0xB51B0]]
    check(not any(s.startswith('call') for s in rm) and 'dec [ecx]' in rm,
          '0xB51B0 (the remove) calls nothing (no allocation, no free): the entries moved down, the count less one')
    d = next(c for c in table if c['id'] == 'DetachChild')
    check(d['fn'] == 0x10ADC0 and d['slot'] == 0x4C and d['regs'] == {'ecx'} and d['args'] == 1 and d['pops'] == 4 and
          not d['frees'], 'the DetachChild contract: removeChild through slot 0x4C, ecx the parent, the child, ret 4, no free')
    cc = body_of(src, 'bool CallCleanup(X86Context& ctx')
    check('SlotMatches' in cc and 'c.slot' in cc, 'CallCleanup checks a slot contract\'s vtable entry before the call')
    dv = body_of(src, 'void DetachViewport(X86Context& ctx, int k, int context)')
    check('CleanupFn::DetachChild' in dv and 'DetachCheck' in dv and 'kernel::CallGuest' not in dv,
          'the viewport is detached only through the checked contract, after DetachCheck')
    rp = body_of(src, 'void RemovePlayer(X86Context& ctx, int k)')
    check(rp.find('DetachViewport(') >= 0 and rp.find('DetachViewport(') < rp.find('NamedElsewhere('),
          'RemovePlayer detaches the viewport before planning what of its own another names')
    # the viewport: VGui_Startup 0x1BAE90 (from 0x18B7E0, after gHUD's init) makes it once a context, sets its parent
    # to the engine's root panel; 0x552B7C (per player) never cleared: the title never deletes it
    vs = [s for _, s in funcs[0x1BAE90]]
    check(vs[vs.index('call [0x0054321C]') + 3] == 'mov esi, eax' and 'push 0xA84' in vs and 'call 0x001B03B0' in vs and
          vs[vs.index('mov [0x00552B7C], eax'):][:5] == ['mov [0x00552B7C], eax', 'mov edx, [eax]', 'push esi',
                                                         'mov ecx, eax', 'call [edx+0x40]'],
          'VGui_Startup 0x1BAE90: the root from [0x54321C] (esi), a new 0xA84-byte viewport, its setParent(root)')
    check([s for _, s in funcs[0x18B7E0]][:3] == ['mov ecx, 0x5623D0', 'call 0x001A6D70', 'call 0x001BAE90'],
          '0x18B7E0: the HUD\'s init, then VGui_Startup')
    stores = sorted(a for fn, ins in funcs.items() for a, s in ins if re.match(r'mov (dword ptr )?\[0x00552B7C\], ', s))
    check(stores == [0x1B03FA, 0x1BAF55], f'0x552B7C is written only by the viewport\'s making ({[hex(a) for a in stores]})')
    check(any(s == 'mov dword ptr [ecx], 0x35FD10' or s.endswith('], 0x35FD10') for _, s in funcs[0x1B03B0]),
          'the viewport\'s constructor 0x1B03B0 stores its vtable 0x35FD10')
    check(T.in_regions(0x552B7C, regs) and not T.in_regions(0x54321C, regs),
          'the viewport pointer is per player, the engine function table (the root\'s source) is not')
    # the grow's wrapper: its contract, and no caller reads eax / ecx / edx after it (a grow leaves them undefined:
    # CallGuest's put-back is one of the values the title's own grow may leave)
    a = G.analyse(funcs, 0xB52B0, memo)
    check(a.inputs == {'eax', 'ebx'} and a.stack_args == 0 and a.pops == {0} and not a.changes_saved,
          f'0xB52B0: eax and ebx in, no stack argument, a plain ret, callee-saved kept ({a.inputs}, {a.pops})')
    gi = [s for _, s in funcs[0xB52B0]]
    check(gi[:4] == ['push esi', 'mov esi, [ebx+0x04]', 'cmp eax, esi', 'jle 0x000B5336'],
          '0xB52B0 grows only past the capacity (signed: jle), as VectorGrowOwner decides')
    bad = []
    for caller, at in sorted(cl.get(0xB52B0, ())):
        r = undefined_read_after(funcs[caller], at)
        if r:
            bad.append(f'0x{caller:X}: {r}')
    check(len(cl.get(0xB52B0, ())) >= 28 and not bad,
          f'no caller of 0xB52B0 reads eax, ecx or edx after it before writing them ({bad[:4]})')
    toml = open(os.path.join(ROOT, 'config', 'game.toml')).read()
    check(re.search(r'at = 0xB52B0\ncall = "NativeSplitVectorGrow"\non_true = "ret"', toml) is not None,
          'the grow\'s hook at its entry, its on_true a plain ret (as 0xB52B0 returns)')
    vg = body_of(src, 'bool NativeSplitVectorGrow(X86Context& ctx, uint8_t*)')
    check('g_vector_scope' in vg and 'VectorGrowOwner' in vg and 'kernel::CallGuest(ctx, nsplit::kVectorGrow' in vg and
          'if (wanted <= cap) return false;' in vg, 'the wrapper: only a grow, decided by VectorGrowOwner, run in the scope')


if __name__ == '__main__':
    sys.exit(main())
