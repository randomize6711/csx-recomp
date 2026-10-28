#!/usr/bin/env python3
"""Each process of a window run, from its procs.csv (tools/drive.py: the game's process tree every half second): CPU
(% of one core) and resident memory over a window of the run, and the total. The window is the benchmarks' held view
(drive.out's setview step to its "setview off" step: the run's own clock, which procs.csv has; the game's log restarts
its clock at each relaunch), or --from / --to seconds of the run.

Usage: python3 -I tools/proc_usage.py RUN [--from S] [--to S]     (RUN: build/wruns/<scenario>)"""
import os
import re
import sys


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    run = args[0]
    lo = float(args[args.index('--from') + 1]) if '--from' in args else None
    hi = float(args[args.index('--to') + 1]) if '--to' in args else None
    if lo is None or hi is None:
        # the held view: drive.out's setview steps ("ok   setview ...  [ 133.0s, ...]", the run's seconds)
        times = []
        for l in open(os.path.join(run, 'drive.out'), errors='replace'):
            m = re.match(r'^ok\s+setview (off)?.*\[\s*([\d.]+)s', l)
            if m:
                times.append((float(m.group(2)), m.group(1) == 'off'))
        starts = [t for t, off in times if not off]
        ends = [t for t, off in times if off]
        if lo is None and starts:
            lo = starts[0]
        if hi is None and ends:
            hi = ends[-1]
    rows = {}  # pid -> [(t, name, rss, cpu, threads)]
    for line in open(os.path.join(run, 'procs.csv')):
        f = line.rstrip('\n').split(',')
        if f[0] == 'seconds' or f[4] == 'ended':
            continue
        rows.setdefault(int(f[1]), []).append((float(f[0]), f[3], float(f[5]), float(f[6]), int(f[7])))
    lo = lo if lo is not None else 0.0
    hi = hi if hi is not None else max(r[-1][0] for r in rows.values())
    print('window %.1f..%.1f s of the run' % (lo, hi))
    print('pid      name             cpu %   rss MB (mean / peak)  threads')
    total_cpu = total_rss = 0.0
    for pid, r in sorted(rows.items()):
        w = [x for x in r if lo <= x[0] <= hi]
        if len(w) < 2:
            continue
        cpu = 100 * (w[-1][3] - w[0][3]) / (w[-1][0] - w[0][0])
        mean = sum(x[2] for x in w) / len(w)
        total_cpu += cpu
        total_rss += mean
        print('%-8d %-16s %6.0f   %6.0f / %6.0f      %d' % (pid, w[-1][1], cpu, mean, max(x[2] for x in w), w[-1][4]))
    print('all      %-16s %6.0f   %6.0f' % ('', total_cpu, total_rss))


if __name__ == '__main__':
    main()
