#!/usr/bin/env python3
"""escape_review.py: the address-escape review of the ownership model (docs/native_splitscreen.md 15.8). Report only:
it classifies nothing and changes no table.

The model (tools/region_model.py) calls a static object "start-up state" (protected: a new player's steps must leave
every byte) when the writes it sees are a static constructor's and its exit destructor's. It sees a write through a
pointer only where its straight-line reading follows the pointer; the seventh run (docs 15.6) found one it did not:
the string-intern pool 0x562118, whose growth is written through ebp and through registers held past a branch.

This review takes every object a static constructor (the C runtime's initializer tables) initialises and asks where
its address goes besides that constructor and its destructor:
  - functions receiving it (in a register or as a stack argument of a call; a call through a pointer included),
  - stored into memory (mov [0xADDR], imm: then anyone reading that pointer reaches the object),
  - returned (mov eax, imm; ret: a getter);
and, for each call it is passed to, the fields the callee writes there, read the review's wider way
(region_model.pointer_summaries(review=True): ebp followed too, a callee-saved register assigned once kept past a
label). Those are potential indirect writes: a field written on a path the pointer may not reach is possible.

Per object: its address and known size (the constructor's fields), constructor and destructor, the receivers, the
known direct writes (the scan's), the potential indirect writes (marking those the model's own reading misses), each
field's current classification and protection, a priority (the run evidence; receivers in a player's initialisation or
frame, or reached only through pointers) and a confidence with the questions left open.

Usage: python3 -I tools/escape_review.py [--json PATH] [--md PATH]
  default build/region_model/escape_review.json and docs/native_split_escape_review.md. Needs build/region_model/
  accesses.json and model.json (region_model.py scan / regions) and the state audit's index.
"""
import json
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import region_model as RM  # noqa: E402
import region_evidence as EV  # noqa: E402

ROOT = RM.ROOT
SA = RM.SA
ATEXIT = 0x22C31D
REGS = RM.PTR_REGS + ('ebp',)
# protected start-up state a run saw change in a new player's steps (docs 15.6, 15.8): the review's first priority
RUN_OBSERVED = {
    0x562130: 'the seventh run: player 2\'s initialisation changed it (4 bytes)',
    0x562135: 'the seventh run: player 2\'s initialisation changed it (1 byte)',
    0x56213C: 'the seventh run: player 2\'s initialisation changed it (2 bytes)',
}


def in_data(v):
    return RM.DATA_LO <= v < RM.DATA_HI


def walk(sources=None):
    """every place a static .data address goes, from straight-line constants: passings (fn, at, callee or None for an
    indirect call, ('reg', r) | ('arg', k), value), stores (fn, at, where, value), returns (fn, at, value)"""
    passings, returns = [], []
    fn_cur, consts, pushed = None, {}, []
    for fn, at, mn, ops, body in RM.instructions(sources):
        if fn != fn_cur:
            fn_cur, consts, pushed = fn, {}, []
        if at is None:
            consts, pushed = {}, []
            continue
        parts = [p.strip() for p in ops.split(',')] if ops else []
        dst = parts[0] if parts else ''
        src = parts[1] if len(parts) > 1 else ''
        if mn == 'call':
            callee = int(ops, 16) if re.fullmatch(r'0x[0-9A-Fa-f]+', ops) else None
            for r in REGS:
                v = consts.get(r)
                if v is not None and in_data(v):
                    passings.append((fn, at, callee, ('reg', r), v))
            for k, v in enumerate(reversed(pushed)):
                if v is not None and in_data(v):
                    passings.append((fn, at, callee, ('arg', k), v))
            pushed = []
            for r in ('eax', 'ecx', 'edx'):
                consts.pop(r, None)
            continue
        if mn.startswith('ret'):
            v = consts.get('eax')
            if v is not None and in_data(v):
                returns.append((fn, at, v))
            consts, pushed = {}, []
            continue
        new = None
        if mn == 'mov' and dst in REGS and re.fullmatch(r'0x[0-9A-Fa-f]+', src):
            new = int(src, 16)
        elif mn == 'mov' and dst in REGS and src in REGS:
            new = consts.get(src)
        elif mn == 'lea' and dst in REGS and RM.MEM_RE.fullmatch(src):
            pm = RM.parse_mem(src[1:-1])
            if pm and not pm[0]:
                new = pm[1]
            elif pm and len(pm[0]) == 1 and pm[0][0][1] == 1 and pm[0][0][0] in consts:
                new = (consts[pm[0][0][0]] + pm[1]) & 0xFFFFFFFF
        elif mn in ('add', 'sub') and dst in REGS and dst in consts and re.fullmatch(r'0x[0-9A-Fa-f]+', src):
            d = int(src, 16)
            new = (consts[dst] + (d if mn == 'add' else -d)) & 0xFFFFFFFF
        if mn == 'push' and len(parts) == 1:
            p = parts[0]
            pushed.append(int(p, 16) if re.fullmatch(r'0x[0-9A-Fa-f]+', p) else consts.get(p) if p in REGS else None)
        elif mn == 'pop':
            if pushed:
                pushed.pop()
        elif dst == 'esp':
            pushed = []
        for r in RM._written(mn, body):
            consts.pop(r, None)
        if new is not None and dst in REGS:
            consts[dst] = new
    return passings, returns


def constructors(index):
    """the static constructors (the initializer tables' entries) and the destructor each registers with atexit"""
    inits = sorted({int(p, 16) for a in range(RM.INIT_LO, RM.INIT_HI, 4) for p in index['tables'].get(f'{a:X}', [])})
    dtor = {}
    for f in inits:
        info = index['funcs'].get(f'{f:X}', {})
        if f'{ATEXIT:X}' in info.get('calls', []):
            dtor[f] = [int(t, 16) for t in info.get('takes', [])]
    return inits, dtor


def field_writes(summary, pid):
    return sorted({(off, w) for off, w, kind in summary.get(pid, ()) if kind == 'w'})


def review(write=True, json_path=None, md_path=None):
    index = SA.load()
    acc = json.load(open(os.path.join(RM.OUT, 'accesses.json')))
    model = json.load(open(os.path.join(RM.OUT, 'model.json')))
    cls = RM.function_classes()
    plain = RM.pointer_summaries()
    wide = RM.pointer_summaries(review=True)
    passings, returns = walk()
    inits, dtors = constructors(index)
    initset = set(inits)
    # the protected ranges and the model's objects (by address)
    prot = [(int(a, 16), int(n, 16)) for a, n in re.findall(
        r'\{0x([0-9A-F]+), 0x([0-9A-F]+), "', open(os.path.join(ROOT, 'src', 'game', 'client_context_protected.inc')).read())]
    objs = sorted((o[0], o[0] + o[1], o[2]) for o in model['objects'])
    evidence = {lo: (n, own, why) for lo, n, own, why in EV.OBJECTS if n}

    def protected(lo, hi):
        return [(a, n) for a, n in prot if a < hi and a + n > lo]

    def ownership(lo, hi):
        return [(a, b - a, own) for a, b, own in objs if a < hi and b > lo]

    # the objects: each constructor's pointer passings (fields from the callee's wide summary) and direct writes
    found = {}
    for fn, at, callee, pid, v in passings:
        if fn not in initset or callee is None or callee == ATEXIT:
            continue
        fields = wide.get(callee, {}).get(pid, set())
        if not fields:
            continue
        size = max(off + w for off, w, _ in fields if off < RM.MAX_FIELD)
        o = found.setdefault(v, {'base': v, 'size': 0, 'ctor': fn, 'ctor_callee': callee, 'how': 'passed'})
        o['size'] = max(o['size'], size)
    for a in acc['accesses']:
        fn, at, kind, addr, w, how = a
        if fn in initset and kind == 'w' and w and how in ('exact', 'exact-reg') and in_data(addr):
            if not any(o['base'] <= addr < o['base'] + o['size'] for o in found.values()):
                found.setdefault(addr, {'base': addr, 'size': w, 'ctor': fn, 'ctor_callee': None, 'how': 'direct'})
    objects = sorted(found.values(), key=lambda o: o['base'])
    for o in objects:
        o['dtor'] = dtors.get(o['ctor'], [])
    # where each object's addresses go, outside its constructor and destructor
    taken = defaultdict(list)
    for a in acc['accesses']:
        if a[5] == 'taken':
            taken[a[3]].append((a[0], a[1]))
    stores = [s for s in acc['stores'] if in_data(s[3])]
    by_value = defaultdict(list)
    for p in passings:
        by_value[p[4]].append(p)
    ret_by = defaultdict(list)
    for r in returns:
        ret_by[r[2]].append(r)
    keys = sorted(by_value)
    import bisect

    def values_in(lo, hi, table, keys_sorted):
        i = bisect.bisect_left(keys_sorted, lo)
        out = []
        while i < len(keys_sorted) and keys_sorted[i] < hi:
            out.extend(table[keys_sorted[i]])
            i += 1
        return out
    tkeys, rkeys = sorted(taken), sorted(ret_by)
    report = []
    for o in objects:
        lo, hi = o['base'], o['base'] + o['size']
        own_fns = {o['ctor']} | set(o['dtor'])
        recv = [(fn, at) for fn, at in values_in(lo, hi, taken, tkeys) if fn not in own_fns and fn not in initset]
        st = [s for s in stores if lo <= s[3] < hi and s[0] not in own_fns]
        rt = [r for r in values_in(lo, hi, ret_by, rkeys) if r[0] not in own_fns]
        if not recv and not st and not rt:
            continue
        ps = [p for p in values_in(lo, hi, by_value, keys) if p[0] not in own_fns and p[0] not in initset]
        indirect = []   # (fn, at, callee, field offset, width, missed by the model)
        unresolved = []
        for fn, at, callee, pid, v in ps:
            if callee is None:
                unresolved.append(f'0x{fn:X} passes 0x{v:X} to a call through a pointer at 0x{at:X} ({pid[1]})')
                continue
            for off, w in field_writes(wide.get(callee, {}), pid):
                f = (v - lo + off) & 0xFFFFFFFF
                if f >= RM.MAX_FIELD:
                    continue
                missed = (off, w) not in field_writes(plain.get(callee, {}), pid)
                indirect.append((fn, at, callee, f, w, missed))
        for s in st:
            unresolved.append(f'0x{s[0]:X} stores 0x{s[3]:X} into 0x{s[2]:X} at 0x{s[1]:X}: whoever reads that pointer '
                              f'reaches the object (not followed)')
        for r in rt:
            unresolved.append(f'0x{r[0]:X} returns 0x{r[2]:X} (0x{r[1]:X}): its callers\' use is not followed')
        direct = sorted({(a[0], a[3] - lo, a[4], a[5]) for a in acc['accesses']
                         if a[2] == 'w' and a[4] and lo <= a[3] < hi and a[5] != 'taken'})
        known_w = {(f, w) for _, f, w, _ in direct}
        fields = []
        for a, n, own in ownership(lo, hi):
            pr = protected(a, a + n)
            pot = sorted({(fn, callee, m) for fn, at, callee, f, w, m in indirect if f < a - lo + n and f + w > a - lo})
            ev = evidence.get(a)
            fields.append({'at': a, 'size': n, 'ownership': own, 'protected': bool(pr), 'evidence': ev[1] if ev else None,
                           'evidence_why': ev[2] if ev else None,
                           'potential': [(f'0x{fn:X}', f'0x{c:X}', m) for fn, c, m in pot],
                           'observed': [RUN_OBSERVED[x] for x in RUN_OBSERVED if a <= x < a + n]})
        prot_hit = [f for f in fields if f['protected'] and f['potential']]
        classes = sorted({cls.get(fn, 'U') for fn, _ in recv} | {cls.get(s[0], 'U') for s in st})
        reach_player = any(('I' in c or 'F' in c or c == 'U') for c in classes)
        observed = any(f['observed'] for f in fields)
        priority = 'run' if observed else 'high' if reach_player and prot_hit else 'medium' if reach_player else 'low'
        if st or rt or any(p[2] is None for p in ps):
            confidence = 'low'
        elif prot_hit or [f for f in fields if f['ownership'] in ('immutable', 'process') and f['potential'] and
                          not f['evidence']]:
            confidence = 'medium'
        else:
            confidence = 'high'
        questions = list(unresolved)
        for f in prot_hit:
            questions.append(f'protected 0x{f["at"]:X}+{f["size"]}: potential writes by '
                             + ', '.join(f'{fn} via {c}{" (missed by the model)" if m else ""}'
                                         for fn, c, m in f['potential'][:6]) + ': are they reached in a player\'s steps?')
        missed_unprot = [f for f in fields if not f['protected'] and f['ownership'] == 'immutable' and f['potential']]
        for f in missed_unprot:
            questions.append(f'immutable 0x{f["at"]:X}+{f["size"]} has potential writes ({f["potential"][0][0]} via '
                             f'{f["potential"][0][1]}): not compared today, not constant')
        report.append({
            'base': f'0x{lo:X}', 'size': o['size'], 'how_sized': ('the constructor\'s callee 0x%X\'s fields' % o['ctor_callee'])
            if o['ctor_callee'] else 'the constructor\'s direct write',
            'constructor': f'0x{o["ctor"]:X}', 'destructor': [f'0x{d:X}' for d in o['dtor']],
            'receivers': sorted({f'0x{fn:X} ({cls.get(fn, "U")})' for fn, _ in recv}),
            'passed': sorted({f'0x{fn:X} -> {"0x%X" % c if c else "a pointer call"} ({pid[0]} {pid[1]})'
                              for fn, at, c, pid, v in ps}),
            'stored': [f'0x{s[0]:X}: [0x{s[2]:X}] = 0x{s[3]:X}' for s in st],
            'returned': [f'0x{r[0]:X}' for r in rt],
            'direct_writes': [f'0x{fn:X} +0x{f:X}/{w} ({how})' for fn, f, w, how in direct],
            'potential_writes': sorted({f'+0x{f:X}/{w} by 0x{fn:X} via 0x{c:X}' + (' (missed by the model)' if m else '')
                                        for fn, at, c, f, w, m in indirect if (f, w) not in known_w or m}),
            'fields': fields, 'protected_with_potential_writes': len(prot_hit),
            'receiver_classes': classes, 'priority': priority, 'confidence': confidence, 'questions': questions,
        })
    # every protected range (a constructor's or not): the writes other code may make into it through a pointer it
    # passes (the wide reading), excluding the range's own writers (the start-up's and the exit's, as the model saw them)
    targets = []
    for fn, at, callee, pid, v in passings:
        if callee is None:
            continue
        pw = set(field_writes(plain.get(callee, {}), pid))
        for off, w in field_writes(wide.get(callee, {}), pid):
            targets.append(((v + off) & 0xFFFFFFFF, w, fn, callee, (off, w) not in pw))
    targets.sort()
    tkeys2 = [t[0] for t in targets]
    writers = defaultdict(set)
    for a in acc['accesses']:
        if a[2] == 'w' and a[4]:
            writers[a[3]].add(a[0])
    protected_hits = []
    for a, n in prot:
        own = set().union(*(writers.get(x, set()) for x in range(a, a + n))) if n <= 64 else set()
        i = bisect.bisect_left(tkeys2, a - 16)
        hits = []
        while i < len(targets) and targets[i][0] < a + n:
            t, w, fn, callee, missed = targets[i]
            if t + w > a and fn not in own and fn not in initset:
                hits.append((fn, callee, missed))
            i += 1
        if hits:
            classes = sorted({cls.get(fn, 'U') for fn, _, _ in hits})
            reach_player = any(('I' in c or 'F' in c or c == 'U') for c in classes)
            protected_hits.append({
                'range': f'0x{a:X}+{n}', 'writers': sorted({f'0x{fn:X} ({cls.get(fn, "U")}) via 0x{c:X}'
                                                             + (' (missed by the model)' if m else '')
                                                             for fn, c, m in hits}),
                'priority': 'high' if reach_player else 'low',
                'observed': [RUN_OBSERVED[x] for x in RUN_OBSERVED if a <= x < a + n]})
    order = {'run': 0, 'high': 1, 'medium': 2, 'low': 3}
    report.sort(key=lambda r: (order[r['priority']], -r['protected_with_potential_writes'], r['base']))
    summary = {
        'constructed objects': len(objects),
        'address escapes (flagged)': len(report),
        'flagged with protected fields that have potential writes': sum(1 for r in report if r['protected_with_potential_writes']),
        'protected ranges with potential writes': sum(r['protected_with_potential_writes'] for r in report),
        'by priority': {k: sum(1 for r in report if r['priority'] == k) for k in order},
        'by confidence': {k: sum(1 for r in report if r['confidence'] == k) for k in ('high', 'medium', 'low')},
        'protected ranges (any start-up state) with potential writes by other code': len(protected_hits),
        'of them reached by a player\'s initialisation or frame, or only through pointers': sum(
            1 for h in protected_hits if h['priority'] == 'high'),
    }
    if write:
        json_path = json_path or os.path.join(RM.OUT, 'escape_review.json')
        md_path = md_path or os.path.join(ROOT, 'docs', 'native_split_escape_review.md')
        json.dump({'summary': summary, 'objects': report, 'protected': protected_hits}, open(json_path, 'w'), indent=1)
        open(md_path, 'w').write(markdown(summary, report, protected_hits))
    return summary, report, protected_hits


def markdown(summary, report, protected_hits=()):
    out = ['# Native split-screen: the address-escape review',
           '',
           'Generated by `tools/escape_review.py` (docs/native_splitscreen.md 15.8). Report only: nothing here is a '
           'classification. Each object below is initialised by a static constructor and its address reaches other '
           'code; "potential" writes are read the review\'s wider way (ebp followed, a register assigned once kept past '
           'a branch) and may name a path the pointer does not take. Regenerate; do not edit.',
           '',
           '## Summary', '']
    for k, v in summary.items():
        out.append(f'- {k}: {v}')
    out += ['', 'Priority: `run` a run saw a protected field change; `high` a receiver in a player\'s initialisation or '
            'frame, or reached only through pointers (class U), and protected fields with potential writes; `medium` '
            'such a receiver; `low` none. Confidence: `low` the address is stored, returned or passed through a '
            'pointer call (not followed); `medium` potential writes into protected or constant fields; `high` neither.',
            '']
    detail = [r for r in report if r['priority'] in ('run', 'high') or r['protected_with_potential_writes']]
    out += [f'## Objects to review ({len(detail)})', '']
    for r in detail:
        out.append(f'### {r["base"]} ({r["size"]} bytes): priority {r["priority"]}, confidence {r["confidence"]}')
        out.append('')
        out.append(f'- constructor {r["constructor"]} (sized by {r["how_sized"]}); destructor '
                   f'{", ".join(r["destructor"]) or "none registered"}')
        out.append(f'- receivers: {", ".join(r["receivers"][:12])}{" ..." if len(r["receivers"]) > 12 else ""}')
        if r['passed']:
            out.append(f'- passed: {"; ".join(r["passed"][:10])}')
        if r['stored']:
            out.append(f'- stored: {"; ".join(r["stored"])}')
        if r['returned']:
            out.append(f'- returned by: {", ".join(r["returned"])}')
        out.append(f'- known direct writes: {"; ".join(r["direct_writes"][:10]) or "none"}')
        out.append(f'- potential indirect writes: {"; ".join(r["potential_writes"][:12]) or "none"}')
        out.append('- fields: ' + '; '.join(
            f'0x{f["at"]:X}+{f["size"]} {f["ownership"]}{" PROTECTED" if f["protected"] else ""}'
            f'{" (evidence)" if f["evidence"] else ""}{" OBSERVED" if f["observed"] else ""}' for f in r['fields']))
        for q in r['questions'][:8]:
            out.append(f'- open: {q}')
        out.append('')
    out += [f'## Protected ranges other code may write through a pointer ({len(protected_hits)})', '',
            'Every protected range (not only a constructor\'s), with the functions passing a pointer whose callee '
            'writes into it (its own writers excluded). These are the review candidates the earlier heuristic (228 '
            'ranges near a taken address) stood for; none is reclassified here.', '',
            '| range | priority | writers (class) | run |', '|---|---|---|---|']
    for h in sorted(protected_hits, key=lambda h: (h['priority'] != 'high', h['range'])):
        out.append(f'| {h["range"]} | {h["priority"]} | {"; ".join(h["writers"][:4])}'
                   f'{" ..." if len(h["writers"]) > 4 else ""} | {"; ".join(h["observed"])} |')
    out.append('')
    rest = [r for r in report if r not in detail]
    out += [f'## The other flagged objects ({len(rest)})', '', '| object | size | priority | confidence | receivers |',
            '|---|---|---|---|---|']
    for r in rest:
        out.append(f'| {r["base"]} | {r["size"]} | {r["priority"]} | {r["confidence"]} | '
                   f'{", ".join(r["receivers"][:4])}{" ..." if len(r["receivers"]) > 4 else ""} |')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    args = sys.argv[1:]
    jp = args[args.index('--json') + 1] if '--json' in args else None
    mp = args[args.index('--md') + 1] if '--md' in args else None
    s, _, _ = review(True, jp, mp)
    for k, v in s.items():
        print(f'{k}: {v}')
