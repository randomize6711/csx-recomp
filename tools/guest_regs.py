#!/usr/bin/env python3
"""A guest function's calling contract read from its original instructions (the generated code's comments): the
registers it reads before writing them on some path from its entry (its register inputs, a backward liveness over its
own branches, the direct calls it makes folded in), the stack arguments it reads at [esp+4..] / [ebp+8..] where its
frame is plain, the bytes its ret pops, the callee-saved registers it changes and the indirect calls it makes (their
targets not known statically). docs/native_splitscreen.md 15.16: every guest call the cleanup makes is checked against
this (tests/guest_call_contract_test.py).

Usage: python3 -I tools/guest_regs.py 0x1BAD90 [...]   (needs build/gen/game)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGS = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
SUB = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx', 'dl': 'edx', 'dh': 'edx',
       'dx': 'edx', 'bl': 'ebx', 'bh': 'ebx', 'bx': 'ebx', 'si': 'esi', 'di': 'edi', 'bp': 'ebp', 'sp': 'esp'}
SAVED = ('ebx', 'ebp', 'esi', 'edi')  # callee-saved in the title's (MSVC) code
SEH_PROLOG, SEH_EPILOG = 0x22E53C, 0x22E577  # the C runtime's __SEH_prolog / __SEH_epilog: ebx, esi, edi, ebp saved
TOKEN = re.compile(r'\b(e[a-d]x|e[sd]i|e[bs]p|[a-d][lhx]|[sd]i|[bs]p)\b')
TWO = {'add', 'sub', 'and', 'or', 'xor', 'adc', 'sbb', 'shl', 'shr', 'sar', 'sal', 'rol', 'ror', 'rcl', 'rcr'}
WRITE = {'mov', 'movzx', 'movsx', 'lea'}
READ = {'cmp', 'test', 'bt', 'push'}
RMW = {'inc', 'dec', 'neg', 'not'}


def operands(s):
    m = s.split(' ', 1)
    mn, rest = m[0], (m[1] if len(m) > 1 else '')
    if mn in ('rep', 'repe', 'repz', 'repne', 'repnz', 'lock'):
        sub = rest.split(' ', 1)
        mn, rest = mn + ' ' + sub[0], (sub[1] if len(sub) > 1 else '')
    ops, depth, cur = [], 0, ''
    for c in rest:
        if c == '[':
            depth += 1
        elif c == ']':
            depth -= 1
        if c == ',' and depth == 0:
            ops.append(cur.strip())
            cur = ''
        else:
            cur += c
    if cur.strip():
        ops.append(cur.strip())
    return mn, ops


def regs_in(op):
    return {SUB.get(r, r) for r in TOKEN.findall(op)}


def full_reg(op):
    return op if op in REGS else None


def use_def(mn, ops):
    """(read, written: fully defined) registers of one instruction, its calls aside"""
    use, d = set(), set()
    mem = [o for o in ops if '[' in o]
    for o in mem:
        use |= regs_in(o[o.index('['):])
    dst = ops[0] if ops else ''
    src = ops[1:]
    base = mn.split(' ')[-1]
    if base in WRITE or base.startswith('set') or base.startswith('cmov'):
        for o in src:
            if '[' not in o:
                use |= regs_in(o)
        if base.startswith('cmov'):
            use |= regs_in(dst) if '[' not in dst else set()
        if full_reg(dst) and not base.startswith('cmov'):
            d.add(dst)
    elif base in ('xor', 'sub', 'sbb') and len(ops) == 2 and ops[0] == ops[1] and full_reg(dst):
        d.add(dst)
    elif base in TWO or base in RMW or base == 'xchg' or base == 'xadd':
        for o in ops:
            if '[' not in o:
                use |= regs_in(o)
        if full_reg(dst):
            d.add(dst)
        if base in ('xchg', 'xadd') and len(ops) > 1 and full_reg(ops[1]):
            d.add(ops[1])
        if base in ('shl', 'shr', 'sar', 'sal', 'rol', 'ror', 'rcl', 'rcr') and len(ops) > 1 and ops[1] == 'cl':
            use.add('ecx')
    elif base in READ:
        for o in ops:
            if '[' not in o:
                use |= regs_in(o)
        if base == 'push':
            use.add('esp')
            d.add('esp')
    elif base == 'pop':
        use.add('esp')
        if full_reg(dst):
            d.add(dst)
    elif base == 'imul' and len(ops) == 3:
        use |= regs_in(ops[1]) if '[' not in ops[1] else set()
        if full_reg(dst):
            d.add(dst)
    elif base == 'imul' and len(ops) == 2:
        use |= {r for o in ops if '[' not in o for r in regs_in(o)}
        if full_reg(dst):
            d.add(dst)
    elif base in ('mul', 'imul', 'div', 'idiv'):
        use |= {'eax'} | ({'edx'} if base in ('div', 'idiv') else set())
        use |= regs_in(dst) if '[' not in dst else set()
        d |= {'eax', 'edx'}
    elif base == 'cdq':
        use.add('eax')
        d.add('edx')
    elif base == 'leave':
        use.add('ebp')
        d |= {'esp', 'ebp'}
    elif base in ('movsb', 'movsw', 'movsd', 'cmpsb', 'cmpsd') and not ops:
        use |= {'esi', 'edi'} | ({'ecx'} if mn.startswith('rep') else set())
    elif base in ('stosb', 'stosw', 'stosd'):
        use |= {'edi', 'eax'} | ({'ecx'} if mn.startswith('rep') else set())
    elif base in ('scasb', 'scasd'):
        use |= {'edi', 'eax'} | ({'ecx'} if mn.startswith('rep') else set())
    elif base in ('lodsb', 'lodsd'):
        use.add('esi')
    else:  # the x87 and the rest: every register named is read, none defined
        for o in ops:
            use |= regs_in(o)
    return use, d


class Contract:
    def __init__(self, fn):
        self.fn, self.inputs, self.stack_args, self.pops, self.changes_saved = fn, set(), None, set(), set()
        self.indirect, self.calls, self.unknown_calls = set(), set(), set()


def analyse(funcs, fn, memo=None, depth=0):
    """the contract of the function at fn (memoised; the direct calls it makes folded in to a depth of 6)"""
    memo = {} if memo is None else memo
    if fn in memo:
        return memo[fn]
    c = Contract(fn)
    memo[fn] = c  # a recursion sees the partial contract
    ins = funcs.get(fn)
    if not ins:
        c.unknown_calls.add(fn)
        return c
    at = {a: i for i, (a, _) in enumerate(ins)}
    n = len(ins)
    use, defs, succ = [set() for _ in range(n)], [set() for _ in range(n)], [[] for _ in range(n)]
    seh = any(s == f'call 0x{SEH_PROLOG:08X}' for _, s in ins)
    saves = prologue_saves(ins)
    for i, (a, s) in enumerate(ins):
        mn, ops = operands(s)
        u, d = use_def(mn, ops)
        if i in saves:  # a callee-saved register pushed before anything touches it: saved, not read
            u = {'esp'}
        nxt = [i + 1] if i + 1 < n else []
        if mn == 'call':
            m = re.match(r'0x([0-9A-F]{8})$', ops[0]) if ops else None
            if m and int(m[1], 16) in (SEH_PROLOG, SEH_EPILOG):
                # its frame: reads only its caller's pushes and the ebp it saves; the prolog's ebp the frame's
                if int(m[1], 16) == SEH_PROLOG:
                    d.add('ebp')
            elif m:
                t = int(m[1], 16)
                c.calls.add(t)
                callee = analyse(funcs, t, memo, depth + 1) if depth < 6 else None
                if callee is None or callee.unknown_calls and t in callee.unknown_calls:
                    c.unknown_calls.add(t)
                else:
                    u |= callee.inputs - {'esp'}
                    c.indirect |= callee.indirect
            else:
                c.indirect.add(a)
                u |= regs_in(ops[0]) if ops else set()
            d |= {'eax', 'ecx', 'edx'}
            succ[i] = nxt
        elif mn == 'ret':
            succ[i] = []
            c.pops.add(int(ops[0], 16) if ops else 0)
        elif mn == 'jmp':
            m = re.match(r'0x([0-9A-F]{8})$', ops[0]) if ops else None
            if m and int(m[1], 16) in at:
                succ[i] = [at[int(m[1], 16)]]
            else:  # a tail call (or a jump table): its target's inputs
                if m:
                    t = int(m[1], 16)
                    c.calls.add(t)
                    callee = analyse(funcs, t, memo, depth + 1) if depth < 6 else None
                    if callee is not None:
                        u |= callee.inputs - {'esp'}
                        c.pops |= callee.pops
                        c.indirect |= callee.indirect
                else:
                    c.indirect.add(a)
                    u |= regs_in(ops[0]) if ops else set()
                succ[i] = []
        elif mn.startswith('j'):
            m = re.match(r'0x([0-9A-F]{8})$', ops[0]) if ops else None
            succ[i] = nxt + ([at[int(m[1], 16)]] if m and int(m[1], 16) in at else [])
        else:
            succ[i] = nxt
        use[i], defs[i] = u, d
        for r in d & set(SAVED):
            c.changes_saved.add(r)
    live = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = set()
            for j in succ[i]:
                out |= live[j]
            new = use[i] | (out - defs[i])
            if new != live[i]:
                live[i], changed = new, True
    c.inputs = live[0] - {'esp'} if n else set()
    # pushed and popped again on every path out: not a change the caller sees
    pushed = {r for _, s in ins for r in re.findall(r'^push (e[a-d]x|e[sd]i|ebp)$', s)}
    popped = {r for _, s in ins for r in re.findall(r'^pop (e[a-d]x|e[sd]i|ebp)$', s)}
    if any(s == 'leave' for _, s in ins):
        popped.add('ebp')
    c.changes_saved -= pushed & popped
    if seh:
        c.changes_saved -= set(SAVED)
    c.stack_args = stack_args(ins, lambda t: memo[t].pops if t in memo else None)
    return c


def prologue_saves(ins):
    """the indices of the first `push <callee-saved>` of each such register when nothing before it (in address order)
    writes the register and it is popped again somewhere: a save, not a read"""
    popped = {s[4:] for _, s in ins if re.match(r'^pop (ebx|ebp|esi|edi)$', s)}
    if any(s == 'leave' for _, s in ins):
        popped.add('ebp')
    written, seen, out = set(), set(), set()
    for i, (_, s) in enumerate(ins):
        mn, ops = operands(s)
        if mn == 'push' and ops and ops[0] in SAVED and ops[0] not in seen:
            seen.add(ops[0])
            if ops[0] not in written and ops[0] in popped:
                out.add(i)
            continue
        written |= use_def(mn, ops)[1]
    return out


def stack_args(ins, pops_of=lambda t: None):
    """the highest stack argument read (1 = [esp+4] at the entry), along the straight line of a plain frame (esp moved
    by push / pop / sub / add esp, or an ebp frame from `push ebp; mov ebp, esp`); None when the frame is not plain"""
    delta, ebp_at, most = 0, None, 0
    for a, s in ins:
        mn, ops = operands(s)
        if mn == 'push':
            delta -= 4
        elif mn == 'pop':
            delta += 4
        elif mn in ('sub', 'add') and ops and ops[0] == 'esp':
            try:
                v = int(ops[1], 16)
            except ValueError:
                return None
            delta += -v if mn == 'sub' else v
        elif mn == 'mov' and ops == ['ebp', 'esp']:
            ebp_at = delta
        elif mn in ('ret', 'jmp') or mn.startswith('j'):
            break
        elif mn == 'call' and ops and ops[0] == f'0x{SEH_PROLOG:08X}':
            ebp_at = -4  # its ebp: as `push ebp; mov ebp, esp` at the entry
        elif mn == 'call':
            p = pops_of(int(ops[0], 16)) if ops and ops[0].startswith('0x') else None
            if not p or len(p) != 1:
                break  # an indirect call or one whose pops are not known: the line ends
            delta += next(iter(p))
        for o in ops:
            m = re.search(r'\[esp\+0x([0-9A-F]+)\]', o)
            if m:
                off = int(m[1], 16) + delta  # from the entry's esp (the return address at 0)
                if off >= 4:
                    most = max(most, off // 4)
            m = re.search(r'\[ebp\+0x([0-9A-F]+)\]', o)
            if m and ebp_at is not None:
                off = int(m[1], 16) + ebp_at
                if off >= 4:
                    most = max(most, off // 4)
    return most


def main(argv):
    sys.path.insert(0, os.path.join(ROOT, 'tests'))
    import native_split_turns_test as T
    funcs, _ = T.load()
    memo = {}
    for a in argv:
        c = analyse(funcs, int(a, 16), memo)
        print(f'0x{c.fn:X}: inputs {sorted(c.inputs) or "none"}, stack args {c.stack_args}, ret pops {sorted(c.pops)}, '
              f'callee-saved changed {sorted(c.changes_saved) or "none"}, indirect calls {len(c.indirect)}, '
              f'calls {len(c.calls)}{", unknown " + str(sorted(hex(x) for x in c.unknown_calls)) if c.unknown_calls else ""}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
