#!/usr/bin/env python3
# Summarises a split-screen benchmark run (tests/scenarios/vk_perf_split_*.txt; docs/splitscreen.md "Performance"):
# each player's lines in the compositor's log ("pN: ...") through tools/perf_summary.py (frame rate, 1% and 0.1% lows,
# intervals, GPU and recording time, server frame, memory) over the held view; then the processes' CPU and memory
# (the compositor's "split: use" lines with CSX_SPLIT_PERF=1: the compositor and every player) and the network (net stats:
# the host's server and each joined player's client, KB/s).
#   tools/split_perf.py RUN_DIR [RUN_DIR ...]   --json: one JSON object a run
import json, os, re, statistics, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_summary  # noqa: E402


def players(run):
    lines = open(os.path.join(run, 'run.out'), errors='replace').read().splitlines()
    out = {}
    for l in lines:
        m = re.match(r'(\[\s*[\d.]+\] \[\w+\] \[\w+\] )p([1-4]): (.*)$', l)
        if m:
            out.setdefault(m.group(2), []).append(m.group(1) + m.group(3))
    if not out:  # the game alone in its window
        out['1'] = lines
    return lines, out


def summary(run):
    lines, per = players(run)
    r = {'run': os.path.basename(run.rstrip('/')), 'players': {}}
    w = None
    for n in sorted(per):
        with tempfile.TemporaryDirectory() as d:
            # the held view (setview) is player 1's: every player is measured over that window
            body = per[n] if n == '1' else [l for l in per['1'] if 'setview' in l][:1] + per[n]
            open(os.path.join(d, 'run.out'), 'w').write('\n'.join(body) + '\n')
            s = perf_summary.summary(d)
        s.pop('run', None)
        r['players'][n] = s
        if w is None:
            w = perf_summary.window(per['1'])
    if not w:
        return r
    t0, t1, _ = w
    # (lines from the held view on: earlier instances of the game, the menus, have times inside the window too)
    start = next(i for i, l in enumerate(lines) if 'setview' in l and 'off' not in l)
    kb = {}
    for l in lines[start:]:
        m = re.match(r'\[\s*([\d.]+)\].*?(?:p([1-4]): )?net stats: (server|client) \d+ connections, out ([\d.]+) KB/s.*'
                     r'in ([\d.]+) KB/s', l)
        if m and t0 <= float(m.group(1)) <= t1:
            kb.setdefault('p%s %s' % (m.group(2) or '1', m.group(3)), []).append(float(m.group(4)) + float(m.group(5)))
    r['network_kbs'] = {k: round(statistics.mean(v), 1) for k, v in sorted(kb.items())}
    use = []
    for l in lines[start:]:
        m = re.match(r'\[\s*([\d.]+)\].*split: use: (\d+) processes, cpu ([\d.]+)%, memory ([\d.]+) MB', l)
        if m and t0 <= float(m.group(1)) <= t1:
            use.append((int(m.group(2)), float(m.group(3)), float(m.group(4))))
    if use:
        r['cpu_percent'] = round(statistics.mean(u[1] for u in use), 1)
        r['rss_mb'] = round(max(u[2] for u in use), 0)
        r['processes'] = max(u[0] for u in use)
    return r


def show(r):
    print('== %s' % r['run'])
    for n, s in r['players'].items():
        if 'fps' in s:
            print('  player %s: %.1f frames/s, 1%% low %.1f, 0.1%% low %.1f; interval median %.2f ms, 99%% %.2f, worst %.1f;'
                  ' GPU %.2f ms, recording %.2f ms%s' % (n, s['fps'], s['low1'], s['low01'], s['p50'], s['p99'],
                                                       s['worst'], s.get('gpu_ms', 0), s.get('record_ms', 0),
                                                       ', server frame %.2f ms' % s['server_ms'] if 'server_ms' in s else ''))
        else:
            print('  player %s: %s' % (n, s.get('error', 'no frame intervals')))
    if 'cpu_percent' in r:
        print('  CPU %.0f%% of one core (all %d processes), memory %.0f MB resident' % (r['cpu_percent'], r['processes'],
                                                                                     r['rss_mb']))
    if r.get('network_kbs'):
        print('  network KB/s (out + in): ' + ', '.join('%s %.1f' % kv for kv in r['network_kbs'].items()))


if __name__ == '__main__':
    for a in [a for a in sys.argv[1:] if a != '--json']:
        r = summary(a)
        print(json.dumps(r)) if '--json' in sys.argv else show(r)
