#!/usr/bin/env python3
"""The shared bytes a new player's initialisation changed (docs/native_splitscreen.md 15.12), classified by the
region model (tools/region_model.py): each changed run's object, its ownership as the model decides it, the functions
that write it and the evidence. The game reports the runs ("native split:   shared change at 0x... (n byte(s))",
the first 16, and with CSX_NATIVE_SPLIT_CHANGES=1 every run); this reads them from a log or the command line.

    tools/shared_changes.py LOG            the runs in a game log (build/run/logs/cs_001.log)
    tools/shared_changes.py 0x4251A9:2 ... runs given as address:bytes
    --json                                 one JSON object a run

Nothing here decides a run is harmless: the ownership is the model's, 'unresolved' where it has none. A run inside a
'player' object would be a region the model has (an error of the list); 'process' / 'immutable' runs changed by a
player's steps are the ones to review first (written once by the process, changed again).

Each run is reported as one of five kinds (docs 15.14):
    protected   'process' / 'immutable': start-up state no player's steps may change (a change is a violation)
    player      a per-player object: it is swapped, so a change in the shared copy is an error of the region list
    scratch     shared, and the evidence proves it rewritten before every use (region_evidence.SCRATCH)
    process     process-wide mutable state, one copy for every player (shared-mutable, server, registry, connection)
    unresolved  in no object, 'unresolved', or classified only by a fallback rule (its writers were not traced)
The 'review' lines list every protected, player and unresolved run; nothing is left out of them."""
import json, os, re, sys
from bisect import bisect_right

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

KINDS = ['protected', 'player', 'unresolved', 'process', 'scratch']
REVIEW = ('protected', 'player', 'unresolved')
# the subsystem a run's object belongs to, named from its evidence (diagnostics only: the kind decides nothing here)
SUBSYSTEMS = [('string-intern pool', 'string-intern pool'), ('the console (docs 15.14)', 'console'),
              ('particle', 'particle manager'), ('pad-to-key bridge', 'pad bridge'), ("health HUD", 'health HUD')]


def kind_of(ownership, why, lo, scratch, fallbacks):
    if ownership in ('process', 'immutable'):
        return 'protected'
    if ownership == 'player':
        return 'player'
    if ownership in (None, '', 'unresolved') or any(f in (why or '') for f in fallbacks):
        return 'unresolved'
    if lo in scratch:
        return 'scratch'
    return 'process'


def subsystem(why):
    return next((name for key, name in SUBSYSTEMS if key in (why or '')), '')


RUN = re.compile(r'shared change at 0x([0-9A-Fa-f]+) \((\d+) byte')
# the order a report lists the classes (the ones to review first)
ORDER = ['process', 'immutable', 'player', 'connection', 'server', 'shared', 'shared-mutable', 'unresolved']


def runs_from(args):
    runs = []
    for a in args:
        if os.path.exists(a):
            for line in open(a, errors='replace'):
                m = RUN.search(line)
                if m:
                    runs.append((int(m[1], 16), int(m[2])))
        else:
            va, _, n = a.partition(':')
            runs.append((int(va, 16), int(n or '1')))
    return sorted(set(runs))


def classify(runs, objs, icls=None, scratch=None, fallbacks=None):
    if scratch is None or fallbacks is None:
        import region_evidence as EV
        scratch = EV.SCRATCH if scratch is None else scratch
        fallbacks = EV.FALLBACK_REASONS if fallbacks is None else fallbacks
    objs = sorted(objs, key=lambda o: o.lo)
    los = [o.lo for o in objs]
    out = []
    for va, n in runs:
        i = bisect_right(los, va) - 1
        o = objs[i] if i >= 0 and objs[i].lo <= va < objs[i].hi else None
        if o is None:
            out.append({'va': va, 'bytes': n, 'object': None, 'ownership': 'unresolved', 'kind': 'unresolved',
                        'subsystem': '', 'why': 'in no object of the model (no access the scan sees)', 'writers': []})
            continue
        out.append({'va': va, 'bytes': n, 'object': [o.lo, o.hi - o.lo], 'ownership': o.ownership or 'unresolved',
                    'kind': kind_of(o.ownership, o.why, o.lo, scratch, fallbacks), 'subsystem': subsystem(o.why),
                    'why': o.why or '', 'writers': sorted(getattr(o, 'writers', ()) or ())[:8],
                    'crosses': va + n > o.hi})
    return out


def main(argv):
    as_json = '--json' in argv
    args = [a for a in argv if a != '--json']
    if not args:
        print(__doc__)
        return 2
    runs = runs_from(args)
    import region_model as M
    import contextlib, io
    with contextlib.redirect_stdout(io.StringIO()):
        objs, errors, cls, icls = M.model()
    rows = classify(runs, objs, icls)
    if as_json:
        for r in rows:
            print(json.dumps(r))
        return 0
    total = {}
    for r in rows:
        t = total.setdefault(r['kind'], [0, 0])
        t[0] += 1
        t[1] += r['bytes']
    print(f'{len(rows)} runs, {sum(r["bytes"] for r in rows)} bytes')
    for k in KINDS:
        if k in total:
            print(f'  {k}: {total[k][0]} runs, {total[k][1]} bytes')
    subs = {}
    for r in rows:
        if r['subsystem']:
            t = subs.setdefault((r['kind'], r['subsystem']), [0, 0])
            t[0] += 1
            t[1] += r['bytes']
    for (k, name), t in sorted(subs.items()):
        print(f'    {name} ({k}): {t[0]} runs, {t[1]} bytes')
    for r in rows:
        obj = f'0x{r["object"][0]:X}+0x{r["object"][1]:X}' if r['object'] else '-'
        w = ', '.join(f'0x{f:X}' for f in r['writers']) or 'none seen'
        print(f'0x{r["va"]:08X} {r["bytes"]:5d}  {r["kind"]:10s} {r["ownership"]:15s} {obj:20s} writers {w}'
              f'{" (crosses its end)" if r.get("crosses") else ""}')
        if r['why']:
            print(f'    {r["why"][:200]}')
    review = [r for r in rows if r['kind'] in REVIEW]
    print(f'review: {len(review)} runs ({sum(r["bytes"] for r in review)} bytes) protected, player or unresolved')
    for r in review:
        print(f'review: 0x{r["va"]:08X} {r["bytes"]} byte(s) {r["kind"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
