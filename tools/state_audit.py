#!/usr/bin/env python3
"""Native split-screen state audit (docs/native_splitscreen.md, M0): which code touches which static data.

Reads the recompiler's output (build/gen/game/x86_recomp.*.cpp: every function's original instructions are kept as
"// ADDRESS instruction" comments) and builds, without running anything:
  - functions (by their X86_FUNC start) and the direct call graph (call / tail jmp to another function's start);
  - every absolute reference into .data (0x36C8A0..0x7D6F20): [0xADDR], [reg+0xADDR], [reg*N+0xADDR] (a read, or a
    write when the operand is the destination of mov / arithmetic / set / inc / dec / pop / stos, ...), and an
    immediate address taken (push 0xADDR, mov reg, 0xADDR, lea);
  - function pointers stored in .data / .rdata at boot (their raw bytes in the XBE) and taken as immediates: tables of
    callbacks (the client DLL's exports, the engine's function tables, vtables) reach code the call graph does not.

Then classifies every function by the roots it is reachable from (--roots below: the server frame, the client half of
_Host_Frame, ...) and every data address by the classes of the functions touching it.

Usage:
  python3 -I tools/state_audit.py build             the index (build/state_audit/index.json)
  python3 -I tools/state_audit.py classify          classes by reachability (build/state_audit/classes.json + summary)
  python3 -I tools/state_audit.py who ADDR [ADDR2]  the functions touching ADDR (or the range ADDR..ADDR2), by class
  python3 -I tools/state_audit.py calls FUNC        a function's callees and callers
Static analysis only: indirect calls through registers are not followed except through the pointer tables above, so
reachability is a lower bound; "unreached" means no static path from a root was found, not that it never runs.
"""
import json
import os
import re
import struct
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'build', 'gen', 'game')
XBE = os.path.join(ROOT, 'assets', 'default.xbe')
OUT = os.path.join(ROOT, 'build', 'state_audit')
DATA_LO, DATA_HI = 0x36C8A0, 0x7D6F20
RDATA_LO, RDATA_HI = 0x3210C0, 0x36C890
CODE_LO, CODE_HI = 0x12000, 0x3210AC

# the roots: each class is everything statically reachable from its entries (FINDINGS / analysis references)
ROOTS = {
    # SV_Frame (analysis/netcode_engine.md: SV_ReadPackets, physics, SV_SendClientMessages)
    'server': [0xA1D20],  # + the game's DLL_FUNCTIONS (SERVER_TABLE) entries
    # _Host_Frame's client half (0x5D2E0, the calls after SV_Frame 0xA1D20 at 0x5D3CD) and its client input before
    # it (0x2AC80 at 0x5D3BD); filled from the frame by 'build' (every direct call of 0x5D2E0 after 0x5D3CD)
    'client': [0x2AC80],
    # the console's command buffer (shared by every player: commands land in one buffer)
    'console': [0x3BB50],
}
HOST_FRAME, SV_FRAME_CALL = 0x5D2E0, 0x5D3CD
# the game's static DLL_FUNCTIONS (copied to gEntityInterface 0x5EF1A0; analysis/netcode_engine.md): 0x32 entries
SERVER_TABLE, SERVER_TABLE_N = 0x37BDB0, 0x32
# ClientDLL_Init (0x1C6F0) stores the client's exports in the engine's cl_funcs (0x7D6CA0..): every pointer it takes
CLIENT_INIT = 0x1C6F0
# _Host_Frame's calls that are not the client's own steps: the timing service between every step (0xAC240) and
# 0xA1CC0 (right after SV_Frame)
NOT_CLIENT_STEPS = {0xAC240, 0xA1CC0}
# hubs: functions that reach nearly the whole image (about 3400 functions) because they redraw the screen or end the
# session: SCR_UpdateScreen 0x55E10, the loading screen's redraw 0x2C200 / 0x48F30, Host_Error 0x5BD30, CL_Disconnect
# 0x28F20, Host_ShutdownServer 0x5C9D0, the Xbox UI's frame calls 0xF3840 / 0xF4CD0 / 0xF79F0, and 0x8B9E0, 0xB8EF0,
# 0xBF0E0, 0x725E0 (each reaching the screen's redraw). The server reaches the client only through them (a redraw
# while a map loads, an error): its walk stops there; the client's goes through.
HUBS = {0x2C200, 0x28F20, 0x5BD30, 0x48F30, 0x8B9E0, 0x5C9D0, 0xB8EF0, 0xBF0E0, 0x55E10, 0xF79F0, 0xF4CD0, 0xF3840,
        0x725E0}

FUNC_RE = re.compile(r'^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)')
INSN_RE = re.compile(r'^\s*// ([0-9A-F]{8}) (\S+)\s*(.*)$')
MEM_RE = re.compile(r'\[([^\]]*)\]')
HEX_RE = re.compile(r'0x([0-9A-Fa-f]+)')
WRITE_MN = {'mov', 'movzx', 'movsx', 'add', 'sub', 'and', 'or', 'xor', 'inc', 'dec', 'neg', 'not', 'shl', 'shr', 'sar',
            'sal', 'rol', 'ror', 'adc', 'sbb', 'xchg', 'pop', 'fstp', 'fst', 'fistp', 'fist', 'fnstsw', 'fnstcw',
            'movss', 'movsd', 'movaps', 'movups', 'movq', 'movd', 'cmpxchg', 'xadd', 'stmxcsr'}
SET_PREFIX = ('set', 'cmov')


def in_data(a):
    return DATA_LO <= a < DATA_HI


def in_code(a):
    return CODE_LO <= a < CODE_HI


def xbe_sections():
    d = open(XBE, 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    n = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    out = []
    for i in range(n):
        fl, v, vs, raw, rs = struct.unpack_from('<IIIII', d, sh + i * 56)
        out.append((v, vs, raw, rs))
    return d, out


def build():
    funcs = {}           # start -> {'calls': set, 'refs': {addr: kinds}, 'end': last insn}
    cur = None
    files = sorted(f for f in os.listdir(GEN) if re.match(r'x86_recomp\.\d+\.cpp$', f))
    for name in files:
        for line in open(os.path.join(GEN, name), encoding='utf-8', errors='replace'):
            m = FUNC_RE.match(line)
            if m:
                cur = int(m.group(1), 16)
                funcs.setdefault(cur, {'calls': set(), 'refs': defaultdict(set), 'end': cur, 'vt': set()})
                continue
            if cur is None:
                continue
            m = INSN_RE.match(line)
            if not m:
                continue
            at, mn, ops = int(m.group(1), 16), m.group(2).lower(), m.group(3)
            f = funcs[cur]
            f['end'] = max(f['end'], at)
            if mn in ('call', 'jmp') and ops.startswith('0x'):
                t = int(ops, 16)
                if mn == 'call' or not (cur <= t <= f['end'] + 0x40):
                    f['calls'].add((t, at))
                continue
            # destination operand: the first one, written by the mnemonics above (cmp / test only read)
            parts = [p.strip() for p in ops.split(',')] if ops else []
            for k, p in enumerate(parts):
                for mem in MEM_RE.findall(p):
                    for h in HEX_RE.findall(mem):
                        a = int(h, 16)
                        if in_data(a):
                            write = k == 0 and (mn in WRITE_MN or mn.startswith(SET_PREFIX)) and len(parts) >= 1
                            if mn in ('push', 'cmp', 'test', 'fld', 'fild', 'fcom', 'fcomp', 'fadd', 'fsub', 'fmul',
                                      'fdiv', 'fsubr', 'fdivr', 'call', 'jmp'):
                                write = False
                            if mn.startswith('rep') or mn.startswith('stos') or mn.startswith('movs'):
                                write = True
                            f['refs'][a].add('w' if write else 'r')
                if '[' not in p:
                    for h in HEX_RE.findall(p):
                        a = int(h, 16)
                        if in_data(a) and (mn in ('push', 'mov', 'lea') or k > 0):
                            f['refs'][a].add('a')  # its address taken
                        elif in_code(a) and mn in ('push', 'mov') and a != cur:
                            f['calls'].add((a, -at))  # a function pointer taken (negative site: not a call)
                        elif RDATA_LO <= a < RDATA_HI and mn in ('push', 'mov', 'lea'):
                            f['vt'].add(a)  # a table in .rdata (a vtable when it starts a run of code pointers)
    # function pointers in the image's initialised data (.data's raw part, .rdata): tables of callbacks
    d, secs = xbe_sections()
    starts = set(funcs)
    table_ptrs = defaultdict(list)  # data address -> function
    for v, vs, raw, rs in secs:
        if not (0x3210C0 <= v < DATA_HI):
            continue
        for off in range(0, rs - 3, 4):
            p = struct.unpack_from('<I', d, raw + off)[0]
            if p in starts:
                table_ptrs[v + off].append(p)
    # a table whose address code takes: its run of code pointers from there (a vtable's methods) is taken with it
    for a, f in funcs.items():
        for t in f['vt']:
            k = 0
            while k < 512 and (t + 4 * k) in table_ptrs:
                for q in table_ptrs[t + 4 * k]:
                    f['calls'].add((q, -1))
                k += 1
    host_client = sorted({t for t, s in funcs.get(HOST_FRAME, {'calls': set()})['calls'] if s > SV_FRAME_CALL and t in starts})
    os.makedirs(OUT, exist_ok=True)
    index = {
        'funcs': {f'{a:X}': {'end': f'{v["end"]:X}', 'calls': sorted({f'{t:X}' for t, s in v['calls'] if s > 0}),
                             'takes': sorted({f'{t:X}' for t, s in v['calls'] if s < 0}),
                             'refs': {f'{r:X}': ''.join(sorted(k)) for r, k in v['refs'].items()}}
                  for a, v in funcs.items()},
        'tables': {f'{a:X}': [f'{p:X}' for p in ps] for a, ps in table_ptrs.items()},
        'host_client': [f'{a:X}' for a in host_client],
    }
    json.dump(index, open(os.path.join(OUT, 'index.json'), 'w'))
    nrefs = sum(len(v['refs']) for v in funcs.values())
    print(f'{len(funcs)} functions, {nrefs} function/data references, {len(table_ptrs)} code pointers in data, '
          f'{len(host_client)} client calls in _Host_Frame after SV_Frame: {", ".join(index["host_client"])}')


def load():
    return json.load(open(os.path.join(OUT, 'index.json')))


def reach(index, roots, stop=()):
    """Direct calls only (a function pointer taken is not a call): the roots' static call closure, not going past
    the functions in stop (they are included, their callees not)."""
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
        todo.extend(funcs[a]['takes'])  # a function pointer taken (a think, a callback, a vtable's methods)
    return seen


def classify(extra_client=(), extra_server=()):
    index = load()
    roots = {k: list(v) for k, v in ROOTS.items()}
    roots['client'] += [int(a, 16) for a in index['host_client'] if int(a, 16) not in NOT_CLIENT_STEPS] + list(extra_client)
    roots['server'] += list(extra_server)
    roots['server'] += [int(p, 16) for k in range(SERVER_TABLE_N) for p in index['tables'].get(f'{SERVER_TABLE + 4 * k:X}', [])]
    roots['client'] += [int(p, 16) for p in index['funcs'][f'{CLIENT_INIT:X}']['takes']]
    sets = {k: reach(index, v, HUBS if k != 'client' else ()) for k, v in roots.items()}
    funcs = index['funcs']
    cls = {}
    for a in funcs:
        tags = [k for k in ('server', 'client', 'console') if a in sets[k]]
        cls[a] = '+'.join(tags) if tags else 'unreached'
    data = defaultdict(lambda: defaultdict(set))  # addr -> class -> kinds
    for a, f in funcs.items():
        for r, kinds in f['refs'].items():
            data[r][cls[a]].update(kinds)
    out = {'functions': cls, 'data': {r: {c: ''.join(sorted(k)) for c, k in v.items()} for r, v in data.items()}}
    json.dump(out, open(os.path.join(OUT, 'classes.json'), 'w'))
    from collections import Counter
    print('functions by class:', dict(Counter(cls.values())))
    kinds = Counter()
    for r, v in data.items():
        c = set(v)
        has_s = any('server' in x for x in c)
        has_c = any('client' in x for x in c)
        written_c = any('client' in x and 'w' in k for x, k in v.items())
        written_s = any('server' in x and 'w' in k for x, k in v.items())
        kinds[('server ' if has_s else '') + ('client ' if has_c else '') + ('unreached ' if 'unreached' in c else '') +
              ('(client writes) ' if written_c else '') + ('(server writes)' if written_s else '')] += 1
    print('data addresses by who touches them:')
    for k, n in kinds.most_common():
        print(f'  {n:6d}  {k.strip() or "console only"}')


def who(a, b=None):
    index = load()
    classes = json.load(open(os.path.join(OUT, 'classes.json')))['functions'] if os.path.exists(
        os.path.join(OUT, 'classes.json')) else {}
    lo, hi = a, (b if b else a)
    for f, v in sorted(index['funcs'].items(), key=lambda x: int(x[0], 16)):
        hits = [(r, k) for r, k in v['refs'].items() if lo <= int(r, 16) <= hi]
        if hits:
            print(f'{f:>8} {classes.get(f, "?"):22s} ' + ' '.join(f'{r}:{k}' for r, k in sorted(hits)[:8]))


def calls(a):
    index = load()
    f = index['funcs'].get(f'{a:X}')
    if not f:
        print('no function at', hex(a))
        return
    print('calls:', ' '.join(f['calls']))
    print('takes:', ' '.join(f['takes']))
    print('callers:', ' '.join(k for k, v in index['funcs'].items() if f'{a:X}' in v['calls']))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'build':
        build()
    elif cmd == 'classify':
        classify()
    elif cmd == 'who':
        who(int(sys.argv[2], 16), int(sys.argv[3], 16) if len(sys.argv) > 3 else None)
    elif cmd == 'calls':
        calls(int(sys.argv[2], 16))
    else:
        print(__doc__)
