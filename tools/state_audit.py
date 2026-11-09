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
  python3 -I tools/state_audit.py regions           the per-player regions the policy below proposes (C++ entries)
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


# the per-player region policy (docs/native_splitscreen.md 10.3): data written by client code and never touched by the
# server's is one player's state when its writers are in these parts of the image (by address; the engine's source
# files are linked in order: CL_* 0x1C000.., the view 0xB5000.., the client game's exports 0x18B000..), else shared
# (the renderer's resources, sound, the network, the host, the Xbox libraries, the C runtime, the menus: M4)
SWAP_CODE = [(0x1C000, 0x3A000, 'engine client'), (0xAC000, 0xB8000, 'engine view'), (0x18B000, 0x200000, 'client game')]
# 0x3A000.. is the console's command buffer (0x3A8A0..0x3A9C0: 0x424A3C, 0x4251A0/A4; Cbuf / Cmd follow at 0x3B000..):
# shared by every player, not a player's (docs/native_splitscreen.md 13.1)
# data shared by nature whatever writes it: never a player's
SHARED_DATA = [
    (0x551870, 0x10, 'the HUD sprite registry: its list (shared), size and count (ClientDLL_HudVidInit 0x1CAC0, 0x22C50)'),
]
# reviewed exceptions: objects whose every writer is one of these stay shared
SHARED_WRITERS = {
    0xB01F0: 'light style values from the server\'s style strings (a..z), the same for every player',
    0xB56A0: 'gamma / brightness ramps (3 x 4096) from the display settings',
    0xB583D: 'gamma / brightness ramps (the same function\'s second entry)',
    0xB5A00: 'gamma ramps (2 x 256) from the display settings',
    0x25BF0: "the client's playermove_t (0x78A6E0) and its physents, cleared and refilled before every prediction",
    0x25C4D: "the client's playermove_t (the same function's second entry)",
    # the Xbox application and video shell in the engine view's range (docs/native_splitscreen.md 13.7): one window,
    # one display, one command line for the process; written by main's start-up (0xBF470 -> 0xAD8A0), the video
    # object's methods and the shutdown, never in a player's turn
    0xAD8A0: 'the game shell (main 0xBF470 calls it): refresh rate 0x379FA8, start-up flags 0x5518A4 / 0x55194C, video',
    0xAD250: 'the command line parsed into com_argv 0x5EF4E0 (100 entries) and com_argc 0x6CE0C8',
    0xAE290: 'the video re-initialisation (0xAD250 again, the video-mode object)',
    0xAE370: 'the system and video objects shut down (0x379FA4, 0x379FAC); 0x55194C cleared',
    0xAF170: 'the window rectangle and centre from the video object 0x379FAC (0x6CE204, 0x6CE210..0x6CE220)',
    0xAF1F0: 'a system object made once (flag 0x553614; 0x553618 / 0x55361C by the XDK call 0x30A510)',
    0xAF870: 'the video-mode object 0x551FE8 released',
    0xAF8B0: 'the video-mode object 0x551FE8 made (572 bytes, vtable 0x339E68, 640x480x16)',
    0xAFD60: 'the display-mode list (0x551FEC..0x551FF4, the 512-byte text at 0x5EECA0; called by 0x491F0)',
    0xB0033: 'the display-mode list (0xAFD60\'s entries)',
    0xB004C: 'the display-mode list (0xAFD60\'s entries)',
    0xB00C0: 'the display-mode list (called by 0x491F0)',
    0xB00E9: 'the display-mode list (0xB00C0\'s entry)',
}

# the C++ runtime's atexit (0x22C31D): the functions registered with it are static destructors, run once at the
# process's exit (0x240160 frees the client game's pool-chunk vector 0x5BD8C8 and zeroes it). They do not make data
# shared: a region's writers are weighed without them (docs/native_splitscreen.md 13.7).
ATEXIT = 0x22C31D
ATEXIT_RE = re.compile(r'^\s*// [0-9A-F]{8} push 0x([0-9A-F]+)\s*$')


def exit_destructors():
    out = set()
    files = sorted(f for f in os.listdir(GEN) if re.match(r'x86_recomp\.\d+\.cpp$', f))
    for name in files:
        prev = None
        for line in open(os.path.join(GEN, name), encoding='utf-8', errors='replace'):
            m = INSN_RE.match(line)
            if not m:
                continue
            if m.group(2) == 'call' and prev and m.group(3).strip().lower() == f'0x{ATEXIT:08x}':
                out.add(prev)
            pm = ATEXIT_RE.match(line)
            prev = int(pm.group(1), 16) if pm else None
    return out


# every static cvar_t of the image (char *name; char *string; int flags; float value; cvar_t *next: 20 bytes), found
# by its shape in the XBE's data: the cvar list is a shared registry (its next links above all: a link swapped with a
# player cuts every later cvar off the others' list), so no part of a static cvar is a player's
def static_cvars():
    d = open(os.path.join(ROOT, 'assets', 'default.xbe'), 'rb').read()
    base = struct.unpack_from('<I', d, 0x104)[0]
    nsec = struct.unpack_from('<I', d, 0x11C)[0]
    sh = struct.unpack_from('<I', d, 0x120)[0] - base
    secs = [struct.unpack_from('<IIIII', d, sh + i * 56)[1:5] for i in range(nsec)]

    def raw(va, n):
        for v, vs, ra, rs in secs:
            if v <= va < v + vs:
                return d[ra + va - v:ra + va - v + n] if va - v + n <= rs else bytes(n)
        return None

    def text(va, least):  # printable text of at least `least` characters at va, else None
        b = raw(va, 40)
        if not b:
            return None
        s = b.split(b'\0')[0]
        return s.decode() if len(s) >= least and all(32 <= c < 127 for c in s) else None
    out = []
    for v, vs, ra, rs in secs:
        if not (DATA_LO <= v < DATA_HI):
            continue
        for a in range(v, v + min(vs, rs) - 20, 4):
            n, s, f, _, nx = struct.unpack('<IIIfI', raw(a, 20))
            if nx and not (DATA_LO <= nx < DATA_HI):  # next: unlinked at boot, or another static cvar
                continue
            nm = text(n, 2)   # a name: an identifier of two characters or more
            st = text(s, 0) if s else None  # its default string: any text, "0" and "" too
            if nm and st is not None and f < 0x10000 and re.fullmatch(r'[a-z_][a-z0-9_]+', nm) and \
                    re.fullmatch(r'[-0-9.a-zA-Z_ ]*', st):
                out.append((a, 20, f'the static cvar {nm}'))
    return out


# code inside a swapped part whose data is shared: the Xbox renderer in the client game's range (the BSP world
# renderer, jcab_render_bsp: the world recorded once at load into the push buffer kept at 0x552C04, FINDINGS "In-game
# rendering"; the model renderer 0x1953xx..0x195720). An object written only by these stays shared; one written by
# these and by other client code is listed for review (and switched).
SHARED_CODE = [(0x195000, 0x19B000, 'the Xbox model and BSP world renderer')]


def regions():
    index = load()
    data = json.load(open(os.path.join(OUT, 'classes.json')))['data']
    writers = defaultdict(set)
    for f, v in index['funcs'].items():
        for r, k in v['refs'].items():
            if 'w' in k:
                writers[int(r, 16)].add(int(f, 16))
    addrs = sorted(int(a, 16) for a in data)
    exits = exit_destructors()
    out = []
    review = []
    for i, a in enumerate(addrs):
        v = data[f'{a:X}']
        server = any('server' in c for c in v)
        client_w = any('client' in c and 'w' in k for c, k in v.items())
        if server or not client_w:
            continue
        ws = writers[a] - exits
        if writers[a] and not ws:
            continue
        if any(lo <= a < lo + n for lo, n, _ in SHARED_DATA):
            continue
        if ws and all(w in SHARED_WRITERS for w in ws):
            continue
        in_shared = [w for w in ws if any(lo <= w < hi for lo, hi, _ in SHARED_CODE)]
        if ws and len(in_shared) == len(ws):
            continue
        if in_shared:
            review.append((a, sorted(ws)))
        if not ws or not all(any(lo <= w < hi for lo, hi, _ in SWAP_CODE) for w in ws):
            continue
        end = addrs[i + 1] if i + 1 < len(addrs) else DATA_HI
        part = next(n for lo, hi, n in SWAP_CODE if lo <= min(ws) < hi)
        if out and out[-1][1] == a and out[-1][2] == part:
            out[-1][1] = end
            out[-1][3] |= ws
        else:
            out.append([a, end, part, set(ws)])
    # clipped against the hand-checked regions of src/game/client_context.cpp (they are whole structures)
    text = open(os.path.join(ROOT, 'src', 'game', 'client_context.cpp'), encoding='utf-8').read()
    body = text[text.index('ClientContextRegions() {'):]
    body = body[:body.index('#include')]
    hand = [(int(a, 16), int(a, 16) + int(n, 16)) for a, n in re.findall(r'\{0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+), "', body)]
    hand += [(lo, lo + n) for lo, n, _ in static_cvars()]  # and against the static cvars (shared)
    clipped = []
    for a, e, part, ws in out:
        segs = [(a, e)]
        for hs, he in hand:
            segs = [x for lo, hi in segs for x in ((lo, min(hi, hs)), (max(lo, he), hi)) if x[1] > x[0]] \
                if any(lo < he and hi > hs for lo, hi in segs) else segs
        clipped += [(lo, hi, part, ws) for lo, hi in segs]
    total = sum(e - a for a, e, _, _ in clipped)
    lines = ['// Generated by tools/state_audit.py regions (docs/native_splitscreen.md 10.3), clipped against the hand-checked',
             "// regions of client_context.cpp: data written only by the engine client, the engine view and the client game's",
             "// code, never touched by the server's, minus the reviewed shared exceptions (SHARED_WRITERS). Each entry names the",
             f'// functions writing it. {len(clipped)} regions, {total} bytes ({total / 1024:.1f} KB). Regenerate; do not edit.']
    for a, e, part, ws in clipped:
        lines.append(f'{{0x{a:X}, 0x{e - a:X}, "{part}: written by {" ".join(f"{w:X}" for w in sorted(ws)[:4])}"}},')
    open(os.path.join(ROOT, 'src', 'game', 'client_context_regions.inc'), 'w').write('\n'.join(lines) + '\n')
    print(f'{len(clipped)} regions, {total} bytes ({total / 1024:.1f} KB): src/game/client_context_regions.inc')
    for a, ws in review:
        print(f'  review: 0x{a:X} written by the shared renderer and by other client code: {" ".join(f"{w:X}" for w in ws)}')


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
    elif cmd == 'regions':
        regions()
    elif cmd == 'calls':
        calls(int(sys.argv[2], 16))
    else:
        print(__doc__)
