#!/usr/bin/env python3
# The report of a CSX_PROFILE sample file (src/game/profiler.cpp): each function's own and inclusive share of the
# samples, per thread, and each thread's CPU use, over a window of the run's log time.
#   tools/profile_report.py EXE PROFILE RUN_OUT [--from S] [--to S] [--thread NAME] [--top N] [--callers FUNC]
#     EXE      the executable that wrote the profile (its symbol table names the functions)
#     RUN_OUT  the run's log: its "profiler: sample clock" line maps sample times to log times; --from / --to are log
#              times (default: the whole run); "--from setview" starts at the log's first setview line and lasts 55 s
#   Generated functions keep their guest address (sub_00XXXXXX; __imp__sub_ is folded into sub_). --callers FUNC
#   lists who calls FUNC, weighted by inclusive samples. --tree FUNC prints the calls under FUNC top-down (--depth,
#   --min percent of the thread's samples). Code in a shared library (the "M" mapping lines) is named lib:NAME+symbol.
import argparse, bisect, collections, os, re, subprocess, sys


def symbols(exe):
    out = subprocess.run(['nm', '-n', '-C', '--defined-only', exe], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split(' ', 2)
        if len(p) < 3 or p[1] not in 'tTwW':
            continue
        name = p[2]
        if name.startswith('__imp__sub_'):
            name = name[6:]
        addrs.append(int(p[0], 16))
        names.append(name)
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe'); ap.add_argument('profile'); ap.add_argument('run_out')
    ap.add_argument('--from', dest='t0', default=None); ap.add_argument('--to', dest='t1', type=float, default=None)
    ap.add_argument('--thread', default=None); ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--callers', default=None)
    ap.add_argument('--tree', default=None); ap.add_argument('--depth', type=int, default=8)
    ap.add_argument('--min', type=float, default=1.0)
    a = ap.parse_args()

    addrs, names = symbols(a.exe)
    anchor_sym = next(addrs[i] for i, n in enumerate(names) if 'OnProf' in n)
    log = open(a.run_out, errors='replace').read().splitlines()
    off = None
    t0 = t1 = None
    for l in log:
        m = re.match(r'\[\s*([\d.]+)\].*profiler: sample clock ([\d.]+)', l)
        if m and off is None:
            off = float(m.group(1)) - float(m.group(2))
        m = re.match(r'\[\s*([\d.]+)\].*setview', l)
        if m and a.t0 == 'setview' and t0 is None and 'off' not in l:
            t0 = float(m.group(1)) + 3
            t1 = t0 + 52
    if off is None:
        sys.exit('no profiler line in the log')
    if a.t0 not in (None, 'setview'):
        t0 = float(a.t0)
    if a.t1 is not None:
        t1 = a.t1

    load = None
    own = collections.defaultdict(lambda: collections.Counter())
    incl = collections.defaultdict(lambda: collections.Counter())
    callers = collections.Counter()
    total = collections.Counter()
    times = {}   # tid -> [(log time, cpu s, name)]
    cache = {}

    maps = []  # (start, end, offset, path)
    libsyms = {}

    def lib_symbol(path, off):
        if path not in libsyms:
            out = subprocess.run(['nm', '-D', '-n', '--defined-only', path], capture_output=True, text=True).stdout
            la, ln = [], []
            for l in out.splitlines():
                q = l.split()
                if len(q) >= 3 and q[1] in 'tTwWiI':
                    la.append(int(q[0], 16)); ln.append(q[2])
            libsyms[path] = (la, ln)
        la, ln = libsyms[path]
        i = bisect.bisect_right(la, off) - 1
        return ln[i] if i >= 0 else '?'

    def name_of(pc):
        n = cache.get(pc)
        if n is None:
            for st, en, fo, path in maps:
                if st <= pc < en:
                    n = 'lib:%s+%s' % (path.rsplit('/', 1)[-1], lib_symbol(path, pc - st + fo))
                    break
            else:
                i = bisect.bisect_right(addrs, pc - load) - 1
                n = names[i] if i >= 0 else '?'
            cache[pc] = n
        return n

    exe_real = os.path.realpath(a.exe)
    for line in open(a.profile):
        if line.startswith('M '):
            q = line.split()  # M start-end perms offset dev inode path
            if len(q) >= 7 and q[6].startswith('/') and os.path.realpath(q[6]) != exe_real:
                st, en = (int(x, 16) for x in q[1].split('-'))
                if not any(m[0] == st for m in maps):
                    maps.append((st, en, int(q[3], 16), q[6]))
    tree = {}

    for line in open(a.profile):
        if line[0] == 'A':
            load = int(line.split()[1], 16) - anchor_sym
            continue
        if line[0] == 'T':
            _, ns, tid, cpu, *comm = line.split()
            times.setdefault(tid, []).append((int(ns) / 1e9 + off, float(cpu), ' '.join(comm)))
            continue
        if line[0] != 'S':
            continue
        p = line.split()
        t = int(p[1]) / 1e9 + off
        if (t0 is not None and t < t0) or (t1 is not None and t > t1):
            continue
        tid = p[2]
        # the first pc is the handler's own frame, the second the trampoline: the interrupted code starts after
        pcs = [int(x, 16) for x in p[3:]]
        try:
            k = next(i for i, pc in enumerate(pcs) if 'restore_rt' in name_of(pc) or 'restore' in name_of(pc))
            pcs = pcs[k + 1:]
        except StopIteration:
            pcs = pcs[2:]
        if not pcs:
            continue
        # return addresses point after the call: the call is in the function holding pc - 1
        fr = [name_of(pcs[0])] + [name_of(pc - 1) for pc in pcs[1:]]
        total[tid] += 1
        own[tid][fr[0]] += 1
        if a.tree and a.tree in fr:
            k = len(fr) - 1 - fr[::-1].index(a.tree)  # the outermost call of it
            node = tree.setdefault(tid, {})
            for f in reversed(fr[:k + 1]):
                ent = node.setdefault(f, [0, {}])
                ent[0] += 1
                node = ent[1]
        seen = set()
        for i, f in enumerate(fr):
            if f in seen:
                continue
            seen.add(f)
            incl[tid][f] += 1
            if a.callers and f == a.callers and i + 1 < len(fr):
                callers[fr[i + 1]] += 1

    tname = {tid: v[-1][2] for tid, v in times.items()}
    print('window: %s .. %s (log time)' % (t0, t1))
    print('\nthreads (CPU use over the window, %% of one core; samples):')
    for tid, v in sorted(times.items(), key=lambda kv: -total[kv[0]]):
        w = [(t, c) for t, c, _ in v if (t0 is None or t >= t0) and (t1 is None or t <= t1)]
        use = (w[-1][1] - w[0][1]) / (w[-1][0] - w[0][0]) * 100 if len(w) > 1 and w[-1][0] > w[0][0] else 0
        if use < 0.5 and not total[tid]:
            continue
        print('  %-8s %-16s %6.1f%%  %7d' % (tid, tname.get(tid, '?'), use, total[tid]))
    for tid in sorted(total, key=lambda t: -total[t]):
        if a.thread and tname.get(tid) != a.thread and tid != a.thread:
            continue
        if total[tid] < 50:
            continue
        n = total[tid]
        print('\n== thread %s (%s): %d samples' % (tid, tname.get(tid, '?'), n))
        print('  own:')
        for f, c in own[tid].most_common(a.top):
            print('    %6.2f%%  %s' % (100 * c / n, f))
        print('  inclusive:')
        for f, c in incl[tid].most_common(a.top):
            print('    %6.2f%%  %s' % (100 * c / n, f))
    if a.tree:
        def show(node, depth, n):
            for f, (c, kids) in sorted(node.items(), key=lambda kv: -kv[1][0]):
                if 100 * c / n < a.min:
                    continue
                print('    %s%6.2f%%  %s' % ('  ' * depth, 100 * c / n, f))
                if depth < a.depth:
                    show(kids, depth + 1, n)
        for tid, node in tree.items():
            if total[tid] >= 50:
                print('\ntree of %s, thread %s (%% of its %d samples):' % (a.tree, tid, total[tid]))
                show(node, 0, total[tid])
    if a.callers:
        n = sum(callers.values()) or 1
        print('\ncallers of %s:' % a.callers)
        for f, c in callers.most_common(a.top):
            print('    %6.2f%%  %s' % (100 * c / n, f))


main()
