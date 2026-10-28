#!/usr/bin/env python3
# Summarises a benchmark run (tests/scenarios/vk_perf_*.txt; FINDINGS.md "High population performance") over its held
# view: from 3 s after the "setview" line for 52 s.
#   tools/perf_summary.py RUN_DIR [RUN_DIR ...]   (build/wruns/vk_perf_aztec_64 ...)   --json: one JSON object a run
# Frame rate from every frame interval (CSX_LOG_INTERVALS=1): mean frames/s, 1% and 0.1% low (the frame rate of the
# slowest 1% / 0.1% of frames: 1000 / their mean interval), interval percentiles; the renderer's GPU and CPU recording
# time a frame; the server frame (CSX_LOG_SERVERTIME); the DISPATCH_LEVEL lock (CSX_LOG_LOCKS); memory peaks
# (CSX_LOG_MEMORY); each thread's CPU use (CSX_PROFILE thread times).
import json, os, re, statistics, sys


def window(lines):
    # (t0, t1, i): from 3 s after the first setview line for 52 s, and that line's index. Only the lines after it
    # count: the log holds every instance of the game (each relaunch starts its clock again, so the menus before the
    # match have times inside the window too) and, with a local server, the server's own lines
    for i, l in enumerate(lines):
        m = re.match(r'\[\s*([\d.]+)\].*setview', l)
        if m and 'off' not in l:
            t0 = float(m.group(1)) + 3
            return t0, t0 + 52, i
    return None


def summary(run):
    lines = open(os.path.join(run, 'run.out'), errors='replace').read().splitlines()
    w = window(lines)
    if not w:
        return {'run': run, 'error': 'no setview line'}
    t0, t1, first = w
    iv, gpu, rec, srv, srvmax, moves, rest = [], [], [], [], [], [], []
    lock = [0, 0, 0.0, 0.0, 0.0, 0.0]  # raises, contended, wait ms, wait max, held ms, seconds
    mem = None
    clock_off = None
    players = None
    for n, l in enumerate(lines):
        m = re.match(r'\[\s*([\d.]+)\]', l)
        if not m:
            continue
        t = float(m.group(1))
        if 'profiler: sample clock' in l and clock_off is None:
            clock_off = t - float(l.rsplit(' ', 1)[1])
        if 'memory: user' in l:
            mem = l
        if n < first or not (t0 <= t <= t1):
            continue
        if 'frame intervals:' in l:
            iv += [float(x) for x in l.split('frame intervals:')[1].split()]
        elif 'renderer: per frame:' in l:
            gpu.append(float(re.search(r'GPU ([\d.]+)', l).group(1)))
            rec.append(float(re.search(r'recording ([\d.]+)', l).group(1)))
        elif 'server timing:' in l:
            srv.append(float(re.search(r'server frame ([\d.]+) ms mean', l).group(1)))
            srvmax.append(float(re.search(r'([\d.]+) max', l).group(1)))
            moves.append(float(re.search(r'player moves ([\d.]+) ms', l).group(1)))
            rest.append(float(re.search(r'the rest \(.*?\) ([\d.]+) ms', l).group(1)))
        elif 'dispatch level:' in l:
            m = re.search(r'(\d+) raises in ([\d.]+) s, (\d+) found it held \(waited ([\d.]+) ms in all, ([\d.]+) at most\), '
                          r'held ([\d.]+) ms', l)
            lock[0] += int(m.group(1)); lock[5] += float(m.group(2)); lock[1] += int(m.group(3))
            lock[2] += float(m.group(4)); lock[3] = max(lock[3], float(m.group(5))); lock[4] += float(m.group(6))
    r = {'run': os.path.basename(run.rstrip('/'))}
    if iv:
        s = sorted(iv)
        n = len(s)
        pct = lambda q: s[min(n - 1, int(n * q))]
        low = lambda q: 1000 / statistics.mean(s[n - max(1, int(n * q)):])
        r.update(fps=1000 * n / sum(iv), low1=low(0.01), low01=low(0.001), p50=pct(0.5), p95=pct(0.95),
                 p99=pct(0.99), p999=pct(0.999), worst=s[-1], frames=n)
    if gpu:
        r.update(gpu_ms=statistics.mean(gpu), record_ms=statistics.mean(rec))
    if srv:
        r.update(server_ms=statistics.mean(srv), server_worst_frame=max(srvmax), moves_ms=statistics.mean(moves),
                 rest_ms=statistics.mean(rest))
    if lock[5]:
        r.update(lock_raises_s=lock[0] / lock[5], lock_contended_s=lock[1] / lock[5], lock_wait_ms_s=lock[2] / lock[5],
                 lock_wait_max=lock[3], lock_held_ms_s=lock[4] / lock[5])
    if mem:
        r['memory'] = re.search(r'peaks: (.*)', mem).group(1) if 'peaks:' in mem else mem.split('memory:', 1)[1].strip()
    prof = os.path.join(run, 'threads.txt')
    if os.path.exists(prof) and clock_off is not None:
        first, last, names = {}, {}, {}
        for l in open(prof):
            if not l.startswith('T '):
                continue
            _, ns, tid, cpu, *comm = l.split()
            t = int(ns) / 1e9 + clock_off
            if not (t0 <= t <= t1):
                continue
            first.setdefault(tid, (t, float(cpu)))
            last[tid] = (t, float(cpu))
            names[tid] = ' '.join(comm)
        threads = {}
        for tid in last:
            (ta, ca), (tb, cb) = first[tid], last[tid]
            if tb > ta:
                use = 100 * (cb - ca) / (tb - ta)
                if use >= 0.5:
                    key = '%s/%s' % (names[tid], tid)
                    threads[key] = round(use, 1)
        r['threads'] = dict(sorted(threads.items(), key=lambda kv: -kv[1]))
        r['process_cpu'] = round(sum(threads.values()), 1)
    return r


def show(r):
    print('== %s' % r['run'])
    if 'error' in r:
        print('  ' + r['error'])
        return
    if 'fps' in r:
        print('  frames/s %.1f mean, 1%% low %.1f, 0.1%% low %.1f (%d frames); interval median %.2f ms, 95%% %.2f, '
              '99%% %.2f, 99.9%% %.2f, worst %.1f' % (r['fps'], r['low1'], r['low01'], r['frames'], r['p50'], r['p95'],
                                                     r['p99'], r['p999'], r['worst']))
    if 'gpu_ms' in r:
        print('  GPU %.3f ms a frame, CPU recording %.3f ms a frame' % (r['gpu_ms'], r['record_ms']))
    if 'server_ms' in r:
        print('  server frame %.3f ms mean (worst frame %.1f): moves %.3f, AI / navigation / rules / network %.3f' %
              (r['server_ms'], r['server_worst_frame'], r['moves_ms'], r['rest_ms']))
    if 'lock_raises_s' in r:
        print('  DISPATCH_LEVEL: %.0f raises/s, %.2f found it held/s, waited %.4f ms/s (at most %.3f), held %.3f ms/s' %
              (r['lock_raises_s'], r['lock_contended_s'], r['lock_wait_ms_s'], r['lock_wait_max'], r['lock_held_ms_s']))
    if 'memory' in r:
        print('  memory: ' + r['memory'])
    if 'threads' in r:
        print('  CPU %.1f%% of one core: %s' % (r['process_cpu'], ', '.join('%s %.1f%%' % kv for kv in
                                                                          list(r['threads'].items())[:8])))


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--json']
    for run in args:
        r = summary(run)
        print(json.dumps(r)) if '--json' in sys.argv else show(r)
