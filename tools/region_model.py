#!/usr/bin/env python3
"""Native split-screen's conservative memory ownership model (docs/native_splitscreen.md 14): which static data is one
local player's, from evidence only. Replaces state_audit.py's region rule (a region ran from each referenced address to
the next one, and an address range of code was taken for one subsystem: docs 13.8).

Reads the recompiler's output (build/gen/game/x86_recomp.*.cpp): every original instruction is kept as a
"// ADDRESS instruction" comment followed by its C, whose memory macros give each access's exact width (X86_MEM8 /
16 / 32 / 64 / F32 / F64) and direction (an assignment to the macro is a write). Without running anything:

  scan      every access to .data / .rdata, per instruction (build/region_model/accesses.json):
              - direct ([0xADDR]) and through a register holding a known constant in straight-line code
                (mov esi, 0xADDR; mov [esi+0x10], eax): an exact access, address and width;
              - indexed ([reg*4+0xADDR], [reg+0xADDR] with reg unknown): an access whose extent is not known: the
                object at ADDR is unbounded until evidence gives its size;
              - block writes and copies (rep stos / rep movs, and single stos / movs) with edi / esi / ecx known: the
                exact range; with any of them unknown: unresolved (recorded, never guessed); the same for a call of a
                block helper (BLOCK_HELPERS: Q_memset, Q_memcpy, memcpy) with its arguments known;
              - an address taken (push / mov reg / lea of 0xADDR): the object may be reached through a pointer;
  model     objects, ownership and the review list (build/region_model/model.json, review.md), and the generated
            region list src/game/client_context_regions.inc and the shared start-up state list
            src/game/client_context_protected.inc.

An object is never sized from the next address code names. Its extent is the union of its exact accesses (overlapping
accesses merge), a proven block range, a static cvar's 20 bytes, or an entry of the evidence table
(tools/region_evidence.py, each with its reason). An object is a player's only when its extent is proven and its
writers are the players' own code (the classes below); everything else is shared, and shared mutable state that a
player's code writes is listed as unresolved for review.
"""
import json
import os
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import region_evidence as EV  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'build', 'gen', 'game')
XBE = os.path.join(ROOT, 'assets', 'default.xbe')
OUT = os.path.join(ROOT, 'build', 'region_model')
AUDIT = os.path.join(ROOT, 'build', 'state_audit', 'index.json')
DATA_LO, DATA_HI = 0x36C8A0, 0x7D6F20
RDATA_LO, RDATA_HI = 0x3210C0, 0x36C890
CODE_LO, CODE_HI = 0x12000, 0x3210AC

FUNC_RE = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
INSN_RE = re.compile(r'^\s*// ([0-9A-F]{8}) (\S+)\s*(.*)$')
LABEL_RE = re.compile(r'^\s*loc_[0-9A-F]{8}:')
MEM_RE = re.compile(r'\[([^\]]*)\]')
MACRO_RE = re.compile(r'X86_MEM(8|16|32|64|F32|F64)\(|X86_PTR\((X86Xmm)|x87_(?:load|store)(80)|x86_(kernel)\(')
WRITE_RE = re.compile(r'X86_MEM(?:8|16|32|64|F32|F64)\(ea\)\s*=(?!=)|\*X86_PTR\(\w+, ea\)\s*=(?!=)|x87_store80\(')
REGS = ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp')
WIDTH = {'8': 1, '16': 2, '32': 4, '64': 8, 'F32': 4, 'F64': 8, 'X86Xmm': 16, '80': 10, 'kernel': 4}
CALLEE_SAVED = ('ebx', 'esi', 'edi', 'ebp')
# block helpers whose arguments a caller gives as constants (their own stores go through registers: the scan cannot
# see them inside): the destination, the source and the byte count, from registers or the cdecl stack (k: the k-th
# argument pushed last-first, 0 the first argument)
BLOCK_HELPERS = {
    0x3DA50: ('Q_memset', ('reg', 'edx'), None, ('reg', 'ecx')),                  # edx dst, eax value, ecx count
    0x3DAA0: ('Q_memcpy', ('reg', 'eax'), ('reg', 'ecx'), ('reg', 'edx')),        # eax dst, ecx src, edx count
    0x22BF00: ('memcpy', ('arg', 0), ('arg', 1), ('arg', 2)),                     # the C runtime's memcpy / memmove
}


def in_static(a):
    return DATA_LO <= a < DATA_HI or RDATA_LO <= a < RDATA_HI


def parse_mem(text):
    """[ecx+eax*4+0x1234] -> (registers with their scales, displacement); None when not of that shape"""
    regs, disp = [], 0
    for term in re.split(r'(?=[+-])', text.replace(' ', '')):
        if not term:
            continue
        sign = -1 if term[0] == '-' else 1
        t = term.lstrip('+-')
        m = re.fullmatch(r'(e[a-z]{2})(?:\*(\d))?', t)
        if m and m.group(1) in REGS:
            regs.append((m.group(1), int(m.group(2) or 1)))
            continue
        m = re.fullmatch(r'0x([0-9A-Fa-f]+)|(\d+)', t)
        if not m:
            return None
        disp += sign * int(m.group(1) or m.group(2), 16 if m.group(1) else 10)
    return regs, disp & 0xFFFFFFFF


def generated_files():
    files = sorted((f for f in os.listdir(GEN) if re.match(r'x86_recomp\.\d+\.cpp$', f)),
                   key=lambda f: int(f.split('.')[1]))
    for name in files:
        yield open(os.path.join(GEN, name), encoding='utf-8', errors='replace')


def instructions(sources=None):
    """(function, address, mnemonic, operands, C body) for every instruction, in the files' order; a label is
    yielded as (function, None, ...): straight-line code ends there. sources: iterables of lines (the generated files
    by default; the tests give their own)"""
    for lines in (generated_files() if sources is None else sources):
        cur, pend = None, None
        for line in lines:
            m = FUNC_RE.match(line)
            if m:
                if pend:
                    yield pend
                    pend = None
                cur = int(m.group(1), 16)
                continue
            if cur is None:
                continue
            m = INSN_RE.match(line)
            if m:
                if pend:
                    yield pend
                pend = [cur, int(m.group(1), 16), m.group(2).lower(), m.group(3).strip(), '']
                continue
            if LABEL_RE.match(line):
                if pend:
                    yield pend
                    pend = None
                yield [cur, None, '', '', '']
                continue
            if pend is not None:
                pend[4] += line
        if pend:
            yield pend


MAX_FIELD = 0x10000  # a field offset through a pointer argument (larger: the register is an index, not a pointer)
PTR_REGS = ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi')
ALL_WRITTEN = re.compile(r'\bx86_(?:trap|resume|priv|cpuid)\(ctx')


def _written(mn, body):
    """the registers an instruction writes (a call: the caller-saved ones, by the convention)"""
    w = {r for r in REGS if re.search(r'ctx\.' + r + r'\s*=(?!=)', body)}
    if mn == 'call' or 'x86_kernel(ctx' in body:
        w |= {'eax', 'ecx', 'edx'}
    if 'x86_rep_movs(ctx' in body or 'x86_rep_stos(ctx' in body:
        w |= {'ecx', 'edi', 'esi'}
    if ALL_WRITTEN.search(body):
        w |= set(REGS)
    return w


def _access_kind(mn, k, body):
    kind = 'w' if (k == 0 and WRITE_RE.search(body)) else 'r'
    return 'r' if mn in ('cmp', 'test', 'push', 'call', 'jmp') else kind


def _functions(sources):
    fn, insns = None, []
    for ins in instructions(sources):
        if ins[0] != fn:
            if insns:
                yield fn, insns
            fn, insns = ins[0], []
        insns.append(ins)
    if insns:
        yield fn, insns


def pointer_summaries(sources=None, review=False):
    """per function: the fields it accesses through a pointer it is given, in a register (one it never writes, or
    before its first write in the straight-line start) or as a cdecl stack argument (loaded from [esp+N] / [ebp+N]):
    {function: {('reg', r) | ('arg', k): {(offset, width, kind)}}}, through the functions it calls with that pointer
    too. A caller passing a static address there accesses those fields of the object (scan: 'exact-call').
    review: the address-escape review's wider reading (tools/escape_review.py, docs/native_splitscreen.md 15.8), off for
    the model (its tables are unchanged): ebp is followed as a pointer register too in a function that does not use it
    as its frame (lea ebp, [ebx+0x18]; mov ecx, ebp: the string-intern pool's buffer), and a callee-saved register
    assigned once (its push / pop around the function aside) keeps its pointer past labels and jumps. That may name a
    field on a path the pointer does not reach: its facts are potential accesses, reported, never a classification."""
    direct, forwards = {}, {}
    for fn, insns in _functions(sources):
        if fn in BLOCK_HELPERS:
            continue
        written_any, first_write, first_label = set(), {}, len(insns)
        for i, (_, at, mn, ops, body) in enumerate(insns):
            if at is None:
                first_label = min(first_label, i)
                continue
            for r in _written(mn, body):
                written_any.add(r)
                first_write.setdefault(r, i)
        ebp_frame = len(insns) > 1 and insns[0][2:4] == ['push', 'ebp'] and insns[1][2:4] == ['mov', 'ebp, esp'] and \
            not any(mn not in ('pop', 'leave') and 'ebp' in _written(mn, body)
                    for _, at, mn, ops, body in insns[2:] if at is not None)
        pregs = PTR_REGS + (('ebp',) if review and not ebp_frame else ())
        # review: the callee-saved registers written once (not counting push / pop): their pointer holds after a label
        writes = defaultdict(int)
        for _, at, mn, ops, body in insns:
            if at is not None and mn != 'pop':
                for r in _written(mn, body):
                    writes[r] += 1
        single = {r for r in CALLEE_SAVED if review and writes[r] == 1}

        def kept(h):
            return {r: v for r, v in h.items() if r in single}
        held = {}      # register -> (pointer id, offset): straight-line facts
        pushed = []    # held pointers or None, pushed since the last call
        delta = 0      # bytes pushed since the entry (the straight-line start only; None after)
        summ, fw = defaultdict(set), []

        def entry_reg(r, i):
            return (r not in written_any) or (i < first_label and i < first_write.get(r, len(insns)))

        def holder(r, i):
            if r in held:
                return held[r]
            if r in PTR_REGS and entry_reg(r, i):  # ebp only once set here (never an entry frame)
                return (('reg', r), 0)
            return None
        for i, (_, at, mn, ops, body) in enumerate(insns):
            if at is None:
                held, pushed, delta = kept(held), [], None
                continue
            parts = [p.strip() for p in ops.split(',')] if ops else []
            widths = [WIDTH[next(g for g in x if g)] for x in MACRO_RE.findall(body)]
            w = widths[0] if widths else None
            if mn != 'lea':
                for k, p in enumerate(parts):
                    for mem in MEM_RE.findall(p):
                        pm = parse_mem(mem)
                        if not pm or len(pm[0]) != 1 or pm[0][0][1] != 1 or w is None:
                            continue
                        r, disp = pm[0][0][0], pm[1]
                        h = holder(r, i)
                        if h is None or disp >= MAX_FIELD:
                            continue
                        kind = _access_kind(mn, k, body)
                        summ[h[0]].add(((h[1] + disp) & 0xFFFFFFFF, w, kind))
                        if kind == 'w' and mn not in ('mov', 'movss', 'movsd', 'movq', 'movd', 'movaps', 'movups',
                                                      'fstp', 'fst', 'fistp', 'fist', 'pop') and not mn.startswith('set'):
                            summ[h[0]].add(((h[1] + disp) & 0xFFFFFFFF, w, 'r'))
            if mn == 'call' and re.fullmatch(r'0x[0-9A-Fa-f]+', ops):
                g = int(ops, 16)
                m = {('reg', r): holder(r, i) for r in pregs if holder(r, i)}
                m.update({('arg', k): v for k, v in enumerate(reversed(pushed)) if v})
                if m:
                    fw.append((g, m))
            # the new facts
            wr = _written(mn, body)
            new = None
            dst = parts[0] if parts else ''
            src = parts[1] if len(parts) > 1 else ''
            if mn == 'mov' and dst in pregs and src in REGS:
                new = holder(src, i)
            elif mn == 'mov' and dst in pregs and MEM_RE.fullmatch(src):
                pm = parse_mem(src[1:-1])
                if pm and len(pm[0]) == 1 and pm[0][0][1] == 1:
                    base, n = pm[0][0][0], pm[1]
                    if base == 'esp' and delta is not None and n >= delta + 4 and (n - delta - 4) % 4 == 0:
                        new = (('arg', (n - delta - 4) // 4), 0)
                    elif base == 'ebp' and ebp_frame and 8 <= n < 0x100 and n % 4 == 0:
                        new = (('arg', (n - 8) // 4), 0)
            elif mn == 'lea' and dst in pregs and MEM_RE.fullmatch(src):
                pm = parse_mem(src[1:-1])
                if pm and len(pm[0]) == 1 and pm[0][0][1] == 1 and pm[1] < MAX_FIELD:
                    h = holder(pm[0][0][0], i)
                    new = (h[0], (h[1] + pm[1]) & 0xFFFFFFFF) if h else None
            elif mn in ('add', 'sub') and dst in pregs and re.fullmatch(r'0x[0-9A-Fa-f]+', src):
                h = holder(dst, i)
                v = int(src, 16)
                new = (h[0], (h[1] + (v if mn == 'add' else -v)) & 0xFFFFFFFF) if h and v < MAX_FIELD else None
            if mn == 'push' and len(parts) == 1:
                pushed.append(holder(parts[0], i) if parts[0] in pregs else None)
                if delta is not None:
                    delta += 4
            elif mn == 'pop':
                if pushed:
                    pushed.pop()
                if delta is not None:
                    delta -= 4
            elif mn == 'call':
                pushed = []
            elif dst == 'esp':
                pushed = []
                if mn in ('add', 'sub') and delta is not None and re.fullmatch(r'0x[0-9A-Fa-f]+', src):
                    delta += int(src, 16) if mn == 'sub' else -int(src, 16)
                else:
                    delta = None
            if mn.startswith('j') or mn.startswith('ret'):
                held, delta = kept(held), None
            for r in wr:
                held.pop(r, None)
            if new is not None and dst in pregs:
                held[dst] = new
            elif dst in pregs and dst in wr:
                # written with something else: no longer the entry pointer here
                held.pop(dst, None)
        direct[fn] = summ
        forwards[fn] = fw
    # through the calls: a callee's fields at the caller's pointer (+ its offset); to a fixed point (capped)
    full = {f: {k: set(v) for k, v in s.items()} for f, s in direct.items()}
    for _ in range(8):
        changed = False
        for f, fw in forwards.items():
            for g, m in fw:
                for gid, fields in list(full.get(g, {}).items()):
                    h = m.get(gid)
                    if not h:
                        continue
                    tgt = full[f].setdefault(h[0], set())
                    for off, w, kind in fields:
                        x = ((h[1] + off) & 0xFFFFFFFF, w, kind)
                        if x[0] < MAX_FIELD and x not in tgt and len(tgt) < 4096:
                            tgt.add(x)
                            changed = True
        if not changed:
            break
    return {f: s for f, s in full.items() if s}


def scan(sources=None, write=True):
    acc = []          # [function, at, kind, address, width, how]: kind r / w / a; how exact / indexed / block / taken
    unresolved_blocks = []  # block writes whose target or count is not known
    jumps = []        # [function, at, table, stride]: a switch's jump table, a handler table (stride 0: found)
    loaded = {}       # register -> the table it was loaded from (straight-line code)
    stores = []       # [function, at, address, value]: a constant stored (mov [0xADDR], imm)
    unsized = 0       # memory operands into static data whose width the C does not show
    consts = {}
    pushed = []       # the values pushed since the last call (straight-line code): a constant, or None
    summaries = pointer_summaries(sources)
    via = 0           # accesses found through a callee's pointer argument
    for fn, at, mn, ops, body in instructions(sources):
        if at is None:  # a label: a jump lands here, registers unknown
            consts = {}
            loaded = {}
            pushed = []
            continue
        parts = [p.strip() for p in ops.split(',')] if ops else []
        # a block helper called with known arguments: its range written (and read), as a rep stos / movs's; with the
        # destination in static data and the count unknown, unresolved
        # a callee given a static address as a pointer argument: the fields it accesses there, exact
        if mn == 'call' and re.fullmatch(r'0x[0-9A-Fa-f]+', ops) and int(ops, 16) in summaries:
            for pid, fields in summaries[int(ops, 16)].items():
                base = consts.get(pid[1]) if pid[0] == 'reg' else (
                    pushed[-1 - pid[1]] if pid[1] < len(pushed) else None)
                if base is None or not in_static(base):
                    continue
                for off, w, kind in sorted(fields):
                    acc.append([fn, at, kind, (base + off) & 0xFFFFFFFF, w, 'exact-call'])
                    via += 1
        if mn == 'call' and re.fullmatch(r'0x[0-9A-Fa-f]+', ops) and int(ops, 16) in BLOCK_HELPERS:
            name, *args = BLOCK_HELPERS[int(ops, 16)]
            def value(arg):
                if arg is None:
                    return None
                if arg[0] == 'reg':
                    return consts.get(arg[1])
                return pushed[-1 - arg[1]] if arg[1] < len(pushed) else None
            dst, src, n = (value(a) for a in args)
            for a, kind, given in ((dst, 'w', True), (src, 'r', args[1] is not None)):
                if not given:
                    continue
                if a is not None and n is not None:
                    if n and (in_static(a) or in_static(a + n - 1)):
                        acc.append([fn, at, kind, a, n, 'block'])
                elif a is not None and in_static(a):
                    unresolved_blocks.append([fn, at, kind, a, None, 'count unknown (' + name + ')'])
        # block operations: rep stos / movs (the comment's mnemonic "rep", the operation its operand) and their
        # single forms (no operand: SSE's movsd has two)
        rep = mn in ('rep', 'repe', 'repz')
        op = ops if rep else (mn if not ops else '')
        if op in ('stosb', 'stosw', 'stosd', 'movsb', 'movsw', 'movsd'):
            w = {'b': 1, 'w': 2, 'd': 4}[op[-1]]
            n = consts.get('ecx') if rep else 1
            dst, src = consts.get('edi'), consts.get('esi')
            sides = [('edi', dst, 'w')] + ([('esi', src, 'r')] if op.startswith('movs') else [])
            for reg, a, kind in sides:
                if a is not None and n is not None:
                    if n and (in_static(a) or in_static(a + n * w - 1)):
                        acc.append([fn, at, kind, a, n * w, 'block'])
                elif a is not None:
                    if in_static(a):
                        unresolved_blocks.append([fn, at, kind, a, None, 'count unknown'])
                elif kind == 'w':
                    unresolved_blocks.append([fn, at, kind, None, n * w if n is not None else None, 'target unknown'])
            # after it: edi (esi) past the range, ecx 0 (rep); unknown when the count was
            for reg, a, kind in sides:
                if a is not None and n is not None:
                    consts[reg] = (a + n * w) & 0xFFFFFFFF
                else:
                    consts.pop(reg, None)
            if rep:
                consts['ecx'] = 0
            continue
        # a switch's jump table (jmp [reg*4+0xTABLE]): its cases are the function's own code
        # and a call through a table of handlers (call [reg*N+0xTABLE]: a message's or a command's handler)
        if mn in ('jmp', 'call'):
            m = re.fullmatch(r'\[e[a-z]{2}\*(\d)\+0x([0-9A-Fa-f]+)\]', ops)
            if m:
                jumps.append([fn, at, int(m.group(2), 16), int(m.group(1))])
        # a handler loaded from a table and called (mov eax, [ecx*4+0x371D00] ... call eax: the server message table)
        if mn == 'mov' and len(parts) == 2 and parts[0] in REGS:
            m = re.fullmatch(r'\[e[a-z]{2}\*\d\+0x([0-9A-Fa-f]+)\]', parts[1])
            if m:
                loaded[parts[0]] = int(m.group(1), 16)
            else:
                loaded.pop(parts[0], None)
        elif mn in ('call', 'jmp') and ops in loaded:
            jumps.append([fn, at, loaded.pop(ops), 0])
        # memory operands
        widths = [WIDTH[next(g for g in x if g)] for x in MACRO_RE.findall(body)]
        written = bool(WRITE_RE.search(body))
        for k, p in enumerate(parts):
            for mem in MEM_RE.findall(p):
                pm = parse_mem(mem)
                if not pm:
                    continue
                regs, disp = pm
                known = [r for r, s in regs if r in consts]
                unknown = [(r, s) for r, s in regs if r not in consts]
                ea = (disp + sum(consts[r] * s for r, s in regs if r in consts)) & 0xFFFFFFFF
                if mn == 'lea':
                    if not unknown and in_static(ea):
                        acc.append([fn, at, 'a', ea, None, 'taken'])
                    elif unknown and in_static(disp):
                        acc.append([fn, at, 'a', disp, None, 'indexed'])  # &table[i]: a pointer into the object
                    continue
                if mn == 'bound':
                    continue
                if not in_static(ea) and not (unknown and in_static(disp)):
                    continue
                w = widths[0] if widths else None
                if w is None:
                    unsized += 1
                kind = 'w' if (k == 0 and written) else 'r'
                if mn in ('cmp', 'test', 'push', 'call', 'jmp'):
                    kind = 'r'
                if unknown:
                    # [reg+0xADDR]: the register an index or a pointer: the object at the address unbounded
                    if in_static(disp) and not known:
                        acc.append([fn, at, kind, disp, w, 'indexed'])
                    elif in_static(ea):
                        acc.append([fn, at, kind, ea, w, 'indexed'])
                else:
                    acc.append([fn, at, kind, ea, w, 'exact' if not regs else 'exact-reg'])
                    if kind == 'w' and mn == 'mov' and len(parts) > 1 and re.fullmatch(r'0x[0-9A-Fa-f]+', parts[1]):
                        stores.append([fn, at, ea, int(parts[1], 16)])
                    if kind == 'w' and 'r' not in [x[2] for x in acc[-2:-1] if x[1] == at] and \
                            mn not in ('mov', 'movss', 'movsd', 'movq', 'movd', 'movaps', 'movups', 'fstp', 'fst',
                                       'fistp', 'fist', 'pop', 'fnstcw', 'fnstsw', 'stmxcsr') and \
                            not mn.startswith('set'):
                        acc.append([fn, at, 'r', ea, w, 'exact' if not regs else 'exact-reg'])  # read-modify-write
            if '[' not in p and k > 0 or (k == 0 and mn == 'push' and '[' not in p):
                m = re.fullmatch(r'0x([0-9A-Fa-f]+)', p)
                if m and in_static(int(m.group(1), 16)) and mn in ('push', 'mov'):
                    acc.append([fn, at, 'a', int(m.group(1), 16), None, 'taken'])
        # the stack's pushed values (a helper's cdecl arguments)
        if mn == 'push' and len(parts) == 1:
            pv = parts[0]
            pushed.append(int(pv, 16) if re.fullmatch(r'0x[0-9A-Fa-f]+', pv) else consts.get(pv) if pv in REGS else None)
        elif mn in ('call', 'pop') or (mn in ('add', 'sub', 'mov', 'lea') and parts and parts[0] == 'esp'):
            pushed = []
        # registers: constants through straight-line code
        dst = parts[0] if parts else ''
        if mn == 'mov' and dst in REGS and re.fullmatch(r'0x[0-9A-Fa-f]+', parts[1] if len(parts) > 1 else ''):
            consts[dst] = int(parts[1], 16)
        elif mn == 'lea' and dst in REGS:
            pm = parse_mem(MEM_RE.findall(parts[1])[0]) if len(parts) > 1 and MEM_RE.findall(parts[1]) else None
            if pm and all(r in consts for r, _ in pm[0]):
                consts[dst] = (pm[1] + sum(consts[r] * s for r, s in pm[0])) & 0xFFFFFFFF
            else:
                consts.pop(dst, None)
        elif mn == 'mov' and dst in REGS and len(parts) > 1 and parts[1] in REGS:
            if parts[1] in consts:
                consts[dst] = consts[parts[1]]
            else:
                consts.pop(dst, None)
        elif mn == 'xor' and dst in REGS and len(parts) > 1 and parts[1] == dst:
            consts[dst] = 0
        elif mn in ('add', 'sub') and dst in REGS and dst in consts and len(parts) > 1 and \
                re.fullmatch(r'0x[0-9A-Fa-f]+', parts[1]):
            v = int(parts[1], 16)
            consts[dst] = (consts[dst] + (v if mn == 'add' else -v)) & 0xFFFFFFFF
        elif mn == 'call':
            consts = {r: v for r, v in consts.items() if r in CALLEE_SAVED}
        elif mn.startswith('j') or mn.startswith('ret') or mn.startswith('loop'):
            consts = {}
            pushed = []
        else:
            # anything else writing a register (by the C: ctx.REG =, or a helper given the whole context)
            for r in list(consts):
                if re.search(r'ctx\.' + r + r'\s*=(?!=)', body) or re.search(r'\bx86_\w+\(ctx', body):
                    consts.pop(r)
    result = {'accesses': acc, 'unresolved_blocks': unresolved_blocks, 'unsized': unsized, 'stores': stores,
              'jumps': jumps}
    if not write:
        return result
    os.makedirs(OUT, exist_ok=True)
    json.dump(result, open(os.path.join(OUT, 'accesses.json'), 'w'))
    from collections import Counter
    print(f'{len(acc)} accesses to static data: ' +
          ', '.join(f'{n} {k}' for k, n in Counter(a[5] for a in acc).most_common()) +
          f'; {len(unresolved_blocks)} block operations unresolved; {unsized} operands of unknown width; {via} through '
          f'a callee\'s pointer argument')


# ---- the function classes (from state_audit.py's call graph: direct calls and function pointers taken) ----
import state_audit as SA  # noqa: E402

MAIN = 0xBF470  # main (called at 0x202C24 by the entry)
# the client game's exports (ClientDLL_Init 0x1C6F0 stores them in cl_funcs 0x7D6CA0..: slot by slot), by when the engine
# calls them. The per-player initialisation steps (native_split.cpp's InitOrder) and the exports outside a player's
# frame are named here with the reason; every other export is part of a player's frame.
PLAYER_INIT = {0x18B7A0: 'Initialize (cl_funcs[0]): a player\'s step', 0x18B800: 'HUD_Init (cl_funcs[1]): a player\'s step'}
NOT_FRAME_EXPORTS = {
    0x18B850: 'HUD_Redraw (cl_funcs[3]): drawn by SCR_UpdateScreen, a hub (an added player draws nothing until M3)',
    0x1A0F10: 'V_CalcRefdef (cl_funcs[19]): the view, under SCR_UpdateScreen',
    0x18F5D0: 'HUD_DrawNormalTriangles (cl_funcs[22]): drawing',
    0x18F5E0: 'HUD_DrawTransparentTriangles (cl_funcs[23]): drawing',
    0x18B830: 'HUD_Shutdown (cl_funcs[26]): the process\'s exit',
    0x193BF0: 'HUD_GetStudioModelInterface (cl_funcs[39]): called once by the engine\'s start-up (the studio interface)',
}
CLASS_NAMES = {
    'S': 'the server (SV_Frame and the game DLL\'s exports, stopping at the hubs)',
    'P': 'the process\'s start-up and its services (main, stopping at _Host_Frame, the players\' steps and the hubs)',
    'I': 'a player\'s initialisation (Initialize, HUD_Init: run for every player)',
    'F': 'a player\'s frame (the client half of _Host_Frame and the client game\'s frame exports, stopping at the hubs)',
    'H': 'only through a hub (the screen\'s redraw, the loading screen, an error, a disconnect, the Xbox UI)',
    'X': 'a static destructor (registered with atexit: the process\'s exit)',
    'C': 'a callback the start-up registers (a console command: run from the command buffer outside the players\' '
         'turns, player 1\'s state live)',
    'Z': 'only through a reviewed shared subsystem (region_evidence.FUNCTIONS)',
    'B': 'a static constructor (the C runtime\'s initializer tables 0x36C8B0..0x36CD28, run before main: their '
         'results are in the boot image every player starts from)',
    'U': 'not reached statically',
}


START_ROOTS = {
    MAIN: 'main (called by the entry at 0x202C24)',
    0xAED30: 'CEngine::Load (vtable 0x339E20 slot 1, called by main\'s shell through it: push 0; call [edx+4] at '
             '0xADA66; analysis/dedicated_server_browser.md): Host_Init 0x5D9E0 through 0xAD540',
}


INIT_LO, INIT_HI = 0x36C8B0, 0x36CD28  # the C runtime's initializer tables (0x206E5B / 0x206F40 call each entry)


def reach(index, roots, stop=(), takes=True):
    """the roots' static closure through direct calls (and function pointers taken, with takes), not going past the
    functions in stop (they are included, their callees not)"""
    funcs = index['funcs']
    stop = {f'{a:X}' for a in stop}
    first = {f'{r:X}' for r in roots}
    seen, todo = set(), list(first)
    while todo:
        a = todo.pop()
        if a in seen or a not in funcs:
            continue
        seen.add(a)
        if a in stop and a not in first:
            continue
        todo.extend(funcs[a]['calls'])
        if takes:
            todo.extend(funcs[a]['takes'])
    return seen


def switch_cases(index):
    """every switch's cases (jmp [reg*4+0xTABLE]: the table's run of the recompiler's function entries, read from the
    XBE): a function reaches its cases (the recompiler emits each case target as an entry of its own, with no caller)"""
    jumps = json.load(open(os.path.join(OUT, 'accesses.json'))).get('jumps', [])
    d, secs = SA.xbe_sections()
    starts = {int(a, 16) for a in index['funcs']}
    out = defaultdict(set)
    for j in jumps:
        fn, at, table = j[:3]
        stride = j[3] if len(j) > 3 else 4
        for v, vs, ra, rs in secs:
            if v <= table < v + min(vs, rs):
                zeros_ok = not stride
                if not stride:  # a table of structures: the distance of its first two handlers (null ones allowed)
                    hits = [i for i in range(0, 64) if table - v + 4 * i + 4 <= rs and
                            struct.unpack_from('<I', d, ra + table - v + 4 * i)[0] in starts]
                    if len(hits) < 2:
                        continue
                    stride = 4 * (hits[1] - hits[0])
                for k in range(1024):
                    o = table - v + stride * k
                    if o + 4 > rs:
                        break
                    t = struct.unpack_from('<I', d, ra + o)[0]
                    if t == 0 and zeros_ok:
                        continue
                    if t not in starts:
                        break
                    out[fn].add(t)
    return out


ENGINE_FUNCS, ENGINE_FUNCS_N = 0x371610, 0x78  # cl_enginefunc_t, as Host_Init hands it to Initialize (0x1C69F)


def engine_api(index):
    """the engine's functions the client game calls through pointers (cl_enginefunc_t and the API tables its fields
    point to: triangle, efx, event, demo, net, voice): a player's own calls, so a player's reach"""
    raw = boot_words()
    starts = {int(a, 16) for a in index['funcs']}
    out = []
    for k in range(ENGINE_FUNCS_N):
        v = struct.unpack('<I', raw(ENGINE_FUNCS + 4 * k, 4))[0]
        if v in starts:
            out.append(v)
        elif DATA_LO <= v < DATA_HI or RDATA_LO <= v < RDATA_HI:
            for j in range(128):
                b = raw(v + 4 * j, 4)
                t = struct.unpack('<I', b)[0] if b else 0
                if t not in starts:
                    break
                out.append(t)
    return out


def hud_methods(index):
    """the HUD elements' methods (region_evidence.HUD_VTABLES: six a vtable, called by gHUD's Init / VidInit / Redraw
    through it in a player's turn): a player's frame roots"""
    raw = boot_words()
    starts = {int(a, 16) for a in index['funcs']}
    out = []
    for vt in EV.HUD_VTABLES:
        for k in range(EV.HUD_METHODS):
            b = raw(vt + 4 * k, 4)
            t = struct.unpack('<I', b)[0] if b else 0
            if t in starts:
                out.append(t)
    return out


def function_classes():
    """each function's classes (CLASS_NAMES): S, P, I, F, X, B may combine; H and U stand alone"""
    index = SA.load()
    funcs = index['funcs']
    for fn, cases in switch_cases(index).items():
        f = funcs.get(f'{fn:X}')
        if f is not None:
            f['calls'] = sorted(set(f['calls']) | {f'{t:X}' for t in cases})
    hubs = set(SA.HUBS)
    exports = [int(t, 16) for t in funcs[f'{SA.CLIENT_INIT:X}']['takes']]
    server_roots = list(SA.ROOTS['server']) + [int(p, 16) for k in range(SA.SERVER_TABLE_N)
                                                for p in index['tables'].get(f'{SA.SERVER_TABLE + 4 * k:X}', [])]
    frame_roots = list(SA.ROOTS['client']) + [int(a, 16) for a in index['host_client']
                                               if int(a, 16) not in SA.NOT_CLIENT_STEPS]
    frame_roots += [e for e in exports if e not in PLAYER_INIT and e not in NOT_FRAME_EXPORTS]
    frame_roots += engine_api(index)
    frame_roots += hud_methods(index)
    players = set(PLAYER_INIT) | set(exports) | set(frame_roots)
    # the process's start-up: direct calls only (a callback it registers runs later, in a frame or a command), not
    # into a player's steps, the frame or the hubs
    P = reach(index, list(START_ROOTS), hubs | {SA.HOST_FRAME} | players, takes=False)
    P -= {f'{a:X}' for a in players}
    # a player's initialisation: direct calls only; the callbacks it registers (the HUD's message hooks, its commands)
    # run in a frame: they are the frame's roots
    # the reviewed shared subsystems (region_evidence.FUNCTIONS 'shared'): a player's reach stops there; what lies
    # only beyond them is the subsystem's (class Z)
    shared = {a for a, (r, _) in EV.FUNCTIONS.items() if r == 'shared'}
    I = reach(index, list(PLAYER_INIT), hubs | shared, takes=False)
    frame_roots += [int(t, 16) for f in I for t in funcs[f]['takes']]
    S = reach(index, server_roots, hubs)
    F = reach(index, frame_roots, hubs | shared)
    Z = reach(index, sorted(shared), hubs)
    H = reach(index, list(hubs) + list(NOT_FRAME_EXPORTS), ())
    X = reach(index, sorted(SA.exit_destructors()), hubs, takes=False)
    inits = [int(p, 16) for a in range(INIT_LO, INIT_HI, 4) for p in index['tables'].get(f'{a:X}', [])]
    B = reach(index, inits, hubs, takes=False)
    # the callbacks the start-up registers (console commands, cvar callbacks): run from the command buffer, outside
    # the players' turns (player 1's state live), or by a later step
    C = reach(index, sorted({int(t, 16) for f in P for t in funcs[f]['takes']}), hubs)
    cls = {}
    for f in funcs:
        c = ''.join(k for k, st in (('F', F), ('I', I), ('P', P), ('S', S), ('X', X), ('B', B), ('C', C)) if f in st)
        if f in Z and 'F' not in c and 'I' not in c:
            c += 'Z'
        if not c and f in H:
            c = 'H'
        cls[int(f, 16)] = c or 'U'
    return cls


# ---- objects ----
def merge(intervals):
    """overlapping intervals merged (touching ones are not: two adjacent variables stay two objects)"""
    out = []
    for lo, hi in sorted(intervals):
        if out and lo < out[-1][1]:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def hand_regions():
    text = open(os.path.join(ROOT, 'src', 'game', 'client_context.cpp'), encoding='utf-8').read()
    body = text[text.index('ClientContextRegions() {'):]
    body = body[:body.index('kGenRegions')]  # the hand-checked entries end where the generated ones are appended
    return [(int(a, 16), int(a, 16) + int(n, 16), w) for a, n, w in
            re.findall(r'\{0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+), "([^"]*)"', body)]


def boot_words():
    """the XBE's initialised bytes by address (the boot image of .data / .rdata): bytes(va, n) or None (bss)"""
    d, secs = SA.xbe_sections()

    def raw(va, n):
        for v, vs, ra, rs in secs:
            if v <= va < v + vs:
                o = va - v
                return d[ra + o:ra + o + n] if o + n <= rs else bytes(n)
        return None
    return raw


ROLE_NAMES = {
    'player': "a player's code (its frame or initialisation, not the server's or the start-up's)",
    'service': "a service of the start-up and the players both (the allocator, the cvars, the files, the console)",
    'server': "the server's code (also run by others)",
    'startup': "the process's start-up only",
    'hub': 'only through a hub (drawing, loading, an error, a disconnect)',
    'exit': 'a static destructor only',
    'boot': 'a static constructor only (before the boot image)',
    'console': 'a console command or callback the start-up registered (outside the players\' turns)',
    'unknown': 'not reached statically (an indirect call)',
}


def role(c):
    if 'S' in c:
        return 'server'
    if 'P' in c:
        return 'service' if ('F' in c or 'I' in c or 'Z' in c) else 'startup'
    if 'F' in c or 'I' in c:
        return 'player'
    if 'C' in c:
        return 'console'
    if 'Z' in c:
        return 'shared'
    if c and set(c) <= {'X', 'B'}:
        return 'boot' if 'B' in c and 'X' not in c else ('exit' if 'B' not in c else 'boot')
    return {'H': 'hub'}.get(c, 'unknown')


class Obj:
    def __init__(self, lo, hi, source):
        self.lo, self.hi, self.source = lo, hi, source  # source: how its extent is known
        self.acc = []          # its accesses
        self.anchors = []      # (at, function, how) indexed / taken accesses: reached through an index or a pointer
        self.ownership = None
        self.why = ''
        self.issues = []

    def size(self):
        return self.hi - self.lo


def build_objects(acc, cvars, evidence, hand):
    """objects from the accesses' exact extents; the evidence's, the static cvars' and the hand-checked regions' extents
    taken whole; an anchor outside every proven extent makes an unbounded object"""
    exact = [(a[3], a[3] + a[4]) for a in acc if a[5] in ('exact', 'exact-reg', 'exact-call', 'block') and a[4] and
             DATA_LO <= a[3] < DATA_HI]
    proven = [(lo, hi, 'access') for lo, hi in merge(exact)]
    whole = [(lo, lo + n, 'cvar') for lo, n, _ in cvars] + [(lo, lo + n, 'evidence') for lo, n, _, _ in evidence] + \
            [(lo, hi, 'hand') for lo, hi, _ in hand]
    errors = []
    # a whole object absorbs the access extents inside it; one cut by its edge is an error (a structure cut)
    objs = []
    for lo, hi, src in sorted(whole):
        objs.append(Obj(lo, hi, src))
    objs.sort(key=lambda o: o.lo)
    for i in range(len(objs) - 1):
        if objs[i].hi > objs[i + 1].lo and not (objs[i].source == 'hand' or objs[i + 1].source == 'hand'):
            errors.append(f'0x{objs[i].lo:X}+0x{objs[i].size():X} ({objs[i].source}) overlaps '
                          f'0x{objs[i + 1].lo:X}+0x{objs[i + 1].size():X} ({objs[i + 1].source})')
    starts = [o.lo for o in objs]
    import bisect

    def holder(a):
        k = bisect.bisect_right(starts, a) - 1
        return objs[k] if k >= 0 and objs[k].lo <= a < objs[k].hi else None
    free = []
    for lo, hi, src in proven:
        h1, h2 = holder(lo), holder(hi - 1)
        if h1 and h1 is h2:
            continue
        cut = [h for h in (h1, h2) if h]
        if cut:
            for h in cut:
                if h.source != 'hand':
                    errors.append(f'accesses 0x{lo:X}..0x{hi:X} cut the {h.source} object 0x{h.lo:X}+0x{h.size():X}')
            # the parts outside the whole objects stay objects of their own
            segs = [(lo, hi)]
            for h in objs:
                segs = [x for a, b in segs for x in ((a, min(b, h.lo)), (max(a, h.hi), b)) if x[1] > x[0]]
            free += segs
        else:
            free.append((lo, hi))
    for lo, hi in free:
        objs.append(Obj(lo, hi, 'access'))
    objs.sort(key=lambda o: o.lo)
    starts[:] = [o.lo for o in objs]
    for a in acc:
        if not (DATA_LO <= a[3] < DATA_HI):
            continue
        if a[5] in ('exact', 'exact-reg', 'exact-call', 'block') and a[4]:
            for b in range(a[3], a[3] + a[4]):
                o = holder(b)
                if o is not None and (not o.acc or o.acc[-1] is not a):
                    o.acc.append(a)
                if o is not None:
                    b = o.hi  # noqa: the rest of this object's bytes add nothing
            continue
        o = holder(a[3])
        if o is None:
            o = Obj(a[3], a[3] + (a[4] or 1), 'unbounded')
            objs.append(o)
            objs.sort(key=lambda x: x.lo)
            starts[:] = [x.lo for x in objs]
        o.anchors.append(a)
        o.acc.append(a)
    return objs, errors


def instruction_classes(acc, cls):
    """an instruction's classes: the union of every generated function holding it (the recompiler emits a function's
    tail again under each continuation entry: 0x800FD is 0x80000's own code), unreached entries left out when a
    reached one holds it"""
    holders = defaultdict(set)
    for a in acc:
        holders[a[1]].add(a[0])
    out = {}
    for at, fns in holders.items():
        cs = {cls.get(f, 'U') for f in fns}
        known = cs - {'U'}
        out[at] = ''.join(sorted(set(''.join(known)))) if known else 'U'
        if 'H' in out[at] and len(out[at]) > 1:
            out[at] = out[at].replace('H', '')
    return out


OWNERSHIP = {
    'player': 'per-player: one copy a local player, swapped with its context',
    'connection': 'per-connection: one a server client (in the server\'s tables, not swapped)',
    'registry': 'shared registry: built once, every player\'s (a static cvar)',
    'process': 'process-owned shared: set by the start-up, then constant (protected: a player\'s steps must not '
               'change it)',
    'immutable': 'shared immutable: no code writes it after the boot image',
    'shared-mutable': 'shared mutable: written while the game runs, one copy for the process',
    'server': 'server-owned: written by the server\'s code',
    'unresolved': 'unresolved: shared for now (not swapped), listed for review; not proven safe',
}
COMPATIBLE = {'player', 'hub', 'exit', 'boot', 'console'}  # writers a player's object may also have


def writer_role(at, icls):
    """an instruction's role; the evidence table's function roles first"""
    for f in icls.get(('fns', at), ()):
        if f in EV.FUNCTIONS:
            return EV.FUNCTIONS[f][0]
    return role(icls.get(at, 'U'))


def classify_objects(objs, cls, icls):
    """each object's ownership (OWNERSHIP) and the reason, from its writers' roles and the proof of its extent; an
    evidence entry's ownership stands, with its reason"""
    evidence = {(lo, lo + n): (own, why) for lo, n, own, why in EV.OBJECTS}
    for o in objs:
        writers = {a[0] for a in o.acc if a[2] == 'w'}
        readers = {a[0] for a in o.acc if a[2] == 'r'}
        roles = {writer_role(a[1], icls) for a in o.acc if a[2] == 'w'}
        server_reads = any('S' in icls.get(a[1], 'U') for a in o.acc if a[2] == 'r')
        o.writers, o.readers, o.roles, o.server_reads = writers, readers, roles, server_reads
        if o.source == 'evidence':
            o.ownership, o.why = evidence[(o.lo, o.hi)]
            o.why = 'evidence: ' + o.why
            continue
        if o.lo in getattr(EV, 'AT', {}):
            o.ownership, o.why = EV.AT[o.lo]
            o.why = 'evidence: ' + o.why
            continue
        if o.source == 'hand':
            o.ownership, o.why = 'player', 'hand-checked whole structure (client_context.cpp)'
            continue
        if o.source == 'cvar':
            o.ownership, o.why = 'registry', 'a static cvar_t (the shared cvar list: its links never a player\'s)'
            continue
        if not writers:
            # its address taken by a player's code: the callee may write it through the pointer where the pointer scan
            # does not follow (a vtable call, a stored pointer, a loop): not proven constant (docs 15)
            takers = {a[0] for a in o.acc if a[2] == 'a' and writer_role(a[1], icls) == 'player'}
            if takers:
                o.ownership = 'unresolved'
                o.why = ('no write found, but a player\'s code takes its address (' +
                         ', '.join(f'0x{f:X}' for f in sorted(takers)[:4]) +
                         '): a write through the pointer the scan does not follow is not ruled out')
                o.issues.append('address taken')
                continue
            o.ownership, o.why = 'immutable', 'no code writes it (its boot value)'
            continue
        if 'player' not in roles:
            if roles <= {'boot'}:
                o.ownership, o.why = 'immutable', 'written only by static constructors (its boot-image value)'
            elif roles <= {'startup', 'exit', 'boot'}:
                o.ownership, o.why = 'process', 'written by the process\'s start-up (or its exit) only'
            elif 'server' in roles:
                o.ownership, o.why = 'server', 'written by the server\'s code'
            elif 'service' in roles or 'shared' in roles:
                o.ownership, o.why = 'shared-mutable', 'written by shared code (services, a shared subsystem)'
            elif roles <= {'hub', 'exit', 'startup', 'boot', 'console'}:
                o.ownership, o.why = 'shared-mutable', 'written only through a hub or a console command (an added ' \
                                                       'player runs none in M2)'
            else:
                o.ownership, o.why = 'shared-mutable', 'its writers are not reached statically (no player\'s code ' \
                                                       'is known to write it)'
            continue
        other = roles - COMPATIBLE
        if other:
            o.ownership = 'unresolved'
            o.why = 'written by a player\'s code and by ' + ', '.join(sorted(other))
            o.issues.append('mixed writers')
            continue
        if server_reads:
            o.ownership, o.why = 'unresolved', 'written by a player\'s code, read by the server\'s'
            o.issues.append('server reads')
            continue
        if o.anchors and not all(any(b[5] == 'block' and b[3] <= a[3] < b[3] + b[4] for b in o.acc)
                                 for a in o.anchors):
            kinds = sorted({a[5] for a in o.anchors})
            o.ownership = 'unresolved'
            o.why = 'a player\'s code writes it; its extent is not proven (reached ' + ' and '.join(kinds) + ')'
            o.issues.append('extent unknown')
            continue
        o.ownership = 'player'
        o.why = 'written only by players\' code; extent from its exact accesses' + (
            ' and block ranges' if any(a[5] == 'block' for a in o.acc) else '')


def holder_of(objs):
    import bisect
    starts = [o.lo for o in objs]

    def holder(a):
        k = bisect.bisect_right(starts, a) - 1
        return objs[k] if k >= 0 and objs[k].lo <= a < objs[k].hi else None
    return holder


def flag_conflicts(objs, stores, holder):
    """an initialisation flag (a function reads A, then stores a constant other than 0 in it) guards the tables the
    same function builds (a block it clears or copies): flag and tables have one ownership, else a new player's steps
    rebuild a shared table (or skip its own one). An object the function merely writes is not a table it builds: the
    flag's other writes are weighed by the ownership rules"""
    reads = defaultdict(set)
    for o in objs:
        for a in o.acc:
            if a[2] == 'r' and a[5] in ('exact', 'exact-reg', 'exact-call'):
                reads[a[0]].add(a[3])
    out = []
    for fn, at, ea, v in stores:
        if not v or ea not in reads[fn]:
            continue
        fo = holder(ea)
        if fo is None or fo.source == 'hand':
            continue
        built = set()
        for o in objs:
            if o is fo or o.source == 'hand':
                continue
            if any(a[0] == fn and a[2] == 'w' and a[5] == 'block' for a in o.acc):
                built.add(o)
        for o in built:
            if (fo.lo, o.lo) in EV.NOT_GUARDS:
                continue
            if (fo.ownership == 'player') != (o.ownership == 'player') and (fn, fo, o) not in out:
                out.append((fn, fo, o))
    return out


def model(write=False):
    acc = json.load(open(os.path.join(OUT, 'accesses.json')))
    cls = function_classes()
    hand = hand_regions()
    cvars = SA.static_cvars()
    objs, errors = build_objects(acc['accesses'], cvars, EV.OBJECTS, hand)
    icls = instruction_classes(acc['accesses'], cls)
    for a in acc['accesses']:
        icls.setdefault(('fns', a[1]), set()).add(a[0])
    classify_objects(objs, cls, icls)
    holder = holder_of(objs)
    for fn, fo, o in flag_conflicts(objs, acc['stores'], holder):
        errors.append(f'the initialisation flag 0x{fo.lo:X} ({fo.ownership}) guards 0x{o.lo:X}+0x{o.size():X} '
                      f'({o.ownership}) in 0x{fn:X}')
    # the hand-checked structures' insides: what the scan sees there (their edges and any shared state inside)
    inside = []
    for lo, hi, what in hand:
        sub, _ = build_objects([a for a in acc['accesses'] if lo <= a[3] < hi or (a[4] and a[3] < hi and a[3] + a[4] > lo)],
                               [], [], [])
        classify_objects(sub, cls, icls)
        for o in sub:
            if o.lo < lo or o.hi > hi:
                errors.append(f'0x{o.lo:X}+0x{o.size():X} crosses the edge of the hand-checked {what} 0x{lo:X}..0x{hi:X}')
            elif o.ownership not in ('player', 'immutable'):
                inside.append((what, o))
    from collections import Counter
    c = Counter(o.ownership for o in objs)
    print('objects:', dict(sorted(c.items())))
    b = Counter()
    for o in objs:
        if o.source != 'hand':
            b[o.ownership] += o.size()
    print('bytes (not counting the hand-checked structures):', dict(sorted(b.items())))
    print(f'{len(inside)} objects inside the hand-checked structures written by shared code (review)')
    print('errors:', len(errors))
    for e in errors[:60]:
        print('  ', e)
    if write:
        if errors:
            print('not written: the model has errors (fail closed)')
            return objs, errors, cls, icls
        write_outputs(objs, inside, cls, icls, acc, cvars)
    return objs, errors, cls, icls


def fmt_fns(fns, icls, n=6):
    fns = sorted(fns)
    return ', '.join(f'0x{f:X}' for f in fns[:n]) + (f' (+{len(fns) - n})' if len(fns) > n else '')


def pointers_in(o, raw, stores):
    """whether an object holds pointers: its boot bytes name the image, or code stores an image address in it, or
    code takes its address (it may be filled through a pointer)"""
    out = []
    b = raw(o.lo, o.size()) if o.size() <= 0x10000 else None
    if b:
        for k in range(0, len(b) - 3, 4):
            v = struct.unpack_from('<I', b, k)[0]
            if 0x10000 <= v < DATA_HI:
                out.append('boot value 0x%X at +0x%X' % (v, k))
                break
    for fn, at, ea, v in stores:
        if o.lo <= ea < o.hi and 0x10000 <= v < DATA_HI:
            out.append('0x%X stores 0x%X' % (at, v))
            break
    if any(a[5] == 'taken' for a in o.anchors):
        out.append('its address is taken')
    return '; '.join(out) or 'none seen'


def timing(roles):
    order = ['boot', 'startup', 'player', 'service', 'server', 'console', 'hub', 'shared', 'unknown', 'exit']
    names = {'boot': 'static constructors', 'startup': 'the start-up', 'player': 'a player\'s frame or steps',
             'service': 'shared services', 'server': 'the server', 'console': 'console commands', 'hub': 'hubs',
             'shared': 'a shared subsystem', 'unknown': 'unreached code', 'exit': 'the exit'}
    return ', '.join(names[r] for r in order if r in roles) or 'never written'


def write_outputs(objs, inside, cls, icls, acc, cvars):
    raw = boot_words()
    hand = hand_regions()

    def in_hand(o):
        return any(lo <= o.lo and o.hi <= hi for lo, hi, _ in hand)
    # the per-player regions: the player objects outside the hand-checked structures, adjacent ones joined (joining
    # touching objects adds no byte)
    regs = []
    for o in objs:
        if o.ownership != 'player' or o.source == 'hand' or in_hand(o):
            continue
        why = 'evidence' if o.source == 'evidence' else ('block' if any(a[5] == 'block' for a in o.acc) else 'exact')
        if regs and regs[-1][1] == o.lo:
            regs[-1][1] = o.hi
            regs[-1][2].add(why)
            regs[-1][3] |= o.writers
        else:
            regs.append([o.lo, o.hi, {why}, set(o.writers)])
    total = sum(e - a for a, e, _, _ in regs)
    lines = [
        '// Generated by tools/region_model.py regions (docs/native_splitscreen.md 14): the per-player objects outside the',
        '// hand-checked structures of client_context.cpp. An object is here only with its extent proven (its exact',
        '// accesses, a block clear / copy, or an entry of tools/region_evidence.py) and every writer a player\'s own code;',
        '// adjacent objects are joined, nothing is extended. Each entry names its proof and some of its writers.',
        f'// {len(regs)} regions, {total} bytes ({total / 1024:.1f} KB). Regenerate; do not edit.']
    for a, e, why, ws in regs:
        lines.append(f'{{0x{a:X}, 0x{e - a:X}, "{"+".join(sorted(why))}: written by '
                     f'{" ".join(f"{w:X}" for w in sorted(ws)[:4])}"}},')
    open(os.path.join(ROOT, 'src', 'game', 'client_context_regions.inc'), 'w').write('\n'.join(lines) + '\n')
    # the protected start-up state: process-owned objects (set by the start-up, constant after), the static cvars'
    # name and next fields (the shared list's links); a new player's steps must leave every byte as it was
    prot = []
    for o in objs:
        if o.ownership == 'process' and o.source != 'hand' and not in_hand(o) and o.roles - {'exit'}:
            prot.append((o.lo, o.size(), 'start-up state: ' + ('evidence' if o.source == 'evidence' else
                                                                 'written by ' + fmt_fns(o.writers, icls, 3))))
    for lo, n, what in cvars:
        prot.append((lo, 4, what + ': name'))
        prot.append((lo + 16, 4, what + ': next'))
    prot.sort()
    pb = sum(n for _, n, _ in prot)
    lines = [
        '// Generated by tools/region_model.py regions (docs/native_splitscreen.md 14): shared state the process\'s start-up',
        '// sets once (no player\'s code writes it) and the static cvars\' list links. native_split.cpp compares every byte',
        '// before and after a new player\'s initialisation steps: a change fails the player (a shared table rebuilt, a',
        f'// list relinked). {len(prot)} ranges, {pb} bytes. Regenerate; do not edit.']
    for lo, n, what in prot:
        lines.append(f'{{0x{lo:X}, 0x{n:X}, "{what}"}},')
    open(os.path.join(ROOT, 'src', 'game', 'client_context_protected.inc'), 'w').write('\n'.join(lines) + '\n')
    # the static addresses the code takes (push / mov / lea of an address in .data or .rdata): a start-up value equal
    # to one is a link (a list head, a static object's pointer, a string), any other value a number
    taken = sorted({a[3] for a in acc['accesses'] if a[5] == 'taken'})
    lines = ['// Generated by tools/region_model.py regions (docs/native_splitscreen.md 14): every static address (.data, .rdata)',
             '// the code takes as an immediate (push / mov / lea): a value equal to one is a link to a static object.',
             f'// {len(taken)} addresses, sorted. Regenerate; do not edit.']
    for k in range(0, len(taken), 8):
        lines.append(' '.join(f'0x{t:X},' for t in taken[k:k + 8]))
    open(os.path.join(ROOT, 'src', 'game', 'client_context_addresses.inc'), 'w').write('\n'.join(lines) + '\n')
    # the review list: every unresolved object, and the shared writes inside the hand-checked structures
    rows = []
    for o in objs:
        if o.ownership != 'unresolved':
            continue
        rows.append(o)
    def row(o, where=''):
        readers = {a[0] for a in o.acc if a[2] == 'r'}
        cleanup = sorted({w for a in o.acc if a[2] == 'w' for w in [a[0]] if writer_role(a[1], icls) in ('exit', 'hub')})
        proposed = 'shared (unresolved: not swapped)'
        unc = '; '.join(o.issues) or '-'
        return (f'| 0x{o.lo:X} | {o.size()}{"+" if o.source == "unbounded" else ""} | {fmt_fns(readers, icls)} | '
                f'{fmt_fns(o.writers, icls)} | {timing(o.roles)} | {fmt_fns(cleanup, icls) or "-"} | '
                f'{pointers_in(o, raw, acc["stores"])} | {proposed} | {where}{o.why} | {unc} |')
    head = ('| Address | Size | Read by | Written by | Initialisation timing (writers) | Cleanup (exit / hub writers) | '
            'Pointers | Proposed ownership | Evidence | Remaining uncertainty |\n|---|---|---|---|---|---|---|---|---|---|')
    out = ['# Native split-screen: the region model\'s review list', '',
           'Generated by `python3 -I tools/region_model.py regions` (docs/native_splitscreen.md 14). Every object whose',
           'ownership the static evidence does not settle. An unresolved object stays shared (not swapped): that is not',
           'a proof that sharing it is safe. Sizes with `+` are unbounded (reached through an index or a pointer: only',
           'the first access\'s width is known). Function roles: see CLASS_NAMES / ROLE_NAMES in the tool.', '',
           f'## Unresolved objects ({len(rows)})', '', head]
    out += [row(o) for o in rows]
    out += ['', f'## Shared writes inside the hand-checked structures ({len(inside)})', '',
            'Objects inside a hand-checked per-player structure that shared code (the server, services, the start-up) '
            'writes: the structure is a player\'s as a whole (its evidence in client_context.cpp); each is listed for '
            'the edge and overlap review.', '', head]
    out += [row(o, f'inside {what}: ') for what, o in inside]
    open(os.path.join(ROOT, 'docs', 'native_split_review.md'), 'w').write('\n'.join(out) + '\n')
    # the model, for the tests
    json.dump({'objects': [[o.lo, o.size(), o.ownership, o.source, sorted(o.roles), o.why] for o in objs],
               'regions': [[a, e - a] for a, e, _, _ in regs], 'protected': [[lo, n] for lo, n, _ in prot]},
              open(os.path.join(OUT, 'model.json'), 'w'))
    print(f'{len(regs)} regions, {total} bytes: src/game/client_context_regions.inc; {len(prot)} protected ranges, '
          f'{pb} bytes: src/game/client_context_protected.inc; {len(rows)} unresolved: docs/native_split_review.md')


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'scan':
        scan()
    elif cmd == 'model':
        model()
    elif cmd == 'regions':
        model(write=True)
    else:
        print(__doc__)
