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
player's steps are the ones to review first (written once by the process, changed again)."""
import json, os, re, sys
from bisect import bisect_right

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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


def classify(runs, objs, icls=None):
    objs = sorted(objs, key=lambda o: o.lo)
    los = [o.lo for o in objs]
    out = []
    for va, n in runs:
        i = bisect_right(los, va) - 1
        o = objs[i] if i >= 0 and objs[i].lo <= va < objs[i].hi else None
        if o is None:
            out.append({'va': va, 'bytes': n, 'object': None, 'ownership': 'unresolved',
                        'why': 'in no object of the model (no access the scan sees)', 'writers': []})
            continue
        out.append({'va': va, 'bytes': n, 'object': [o.lo, o.hi - o.lo], 'ownership': o.ownership or 'unresolved',
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
        t = total.setdefault(r['ownership'], [0, 0])
        t[0] += 1
        t[1] += r['bytes']
    print(f'{len(rows)} runs, {sum(r["bytes"] for r in rows)} bytes')
    for k in sorted(total, key=lambda k: ORDER.index(k) if k in ORDER else len(ORDER)):
        print(f'  {k}: {total[k][0]} runs, {total[k][1]} bytes')
    for r in rows:
        obj = f'0x{r["object"][0]:X}+0x{r["object"][1]:X}' if r['object'] else '-'
        w = ', '.join(f'0x{f:X}' for f in r['writers']) or 'none seen'
        print(f'0x{r["va"]:08X} {r["bytes"]:5d}  {r["ownership"]:15s} {obj:20s} writers {w}'
              f'{" (crosses its end)" if r.get("crosses") else ""}')
        if r['why']:
            print(f'    {r["why"][:200]}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
