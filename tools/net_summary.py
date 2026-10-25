#!/usr/bin/env python3
"""Summarises a network run's statistics (CSX_LOG_NET=1 "net stats:" lines, CSX_LOG_SERVERTIME=1 "server time"
lines) from a game log (run.out): per endpoint the connections, the traffic out and in (KB/s: mean, peak), datagrams
and messages a second, fragmented messages, the largest message, loss, replays, failed authentications, malformed
datagrams, reassembly timeouts; the time to build and send the clients' messages; the server frame.

Usage: python3 -I tools/net_summary.py RUN.OUT [--from S] [--to S]   (seconds of the log's own clock)
"""
import re, statistics, sys


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    path = args[0]
    lo = float(args[args.index('--from') + 1]) if '--from' in args else -1
    hi = float(args[args.index('--to') + 1]) if '--to' in args else 1e18
    ep = {}
    send_mean, send_worst, server = [], [], []
    rx = re.compile(r'\[\s*([\d.]+)\].*net stats: (server|client) (\d+) connections, out ([\d.]+) KB/s in (\d+) datagrams '
                    r'\((\d+) messages, (\d+) fragmented, largest (\d+) bytes\), in ([\d.]+) KB/s in (\d+) datagrams; lost '
                    r'(\d+), replays (\d+), auth (\d+), malformed (\d+), reassembly timeouts (\d+)')
    rs = re.compile(r'\[\s*([\d.]+)\].*net stats: SV_SendClientMessages (\d+) calls, ([\d.]+) ms each on average, ([\d.]+) ms at most')
    rt = re.compile(r'\[\s*([\d.]+)\].*server timing: \d+ frames, server frame ([\d.]+) ms mean')
    for line in open(path, errors='replace'):
        m = rx.search(line)
        if m:
            t = float(m.group(1))
            if not lo <= t <= hi:
                continue
            e = ep.setdefault(m.group(2), {k: [] for k in ('conn', 'out', 'dout', 'msg', 'frag', 'largest', 'in', 'din', 'lost',
                                                         'replay', 'auth', 'malformed', 'reasm')})
            for k, g in zip(('conn', 'out', 'dout', 'msg', 'frag', 'largest', 'in', 'din', 'lost', 'replay', 'auth', 'malformed',
                             'reasm'), range(3, 16)):
                e[k].append(float(m.group(g)))
            continue
        m = rs.search(line)
        if m and lo <= float(m.group(1)) <= hi:
            send_mean.append(float(m.group(3)))
            send_worst.append(float(m.group(4)))
            continue
        m = rt.search(line)
        if m and lo <= float(m.group(1)) <= hi:
            server.append(float(m.group(2)))
    for name, e in ep.items():
        if not e['out']:
            continue
        n = len(e['out'])
        print(f'{name}: {n} s, {max(e["conn"]):.0f} connections; out {statistics.mean(e["out"]):.1f} KB/s '
              f'(peak {max(e["out"]):.1f}), {statistics.mean(e["dout"]):.0f} datagrams/s, {statistics.mean(e["msg"]):.0f} '
              f'messages/s, {sum(e["frag"]):.0f} fragmented, largest {max(e["largest"]):.0f} bytes; in '
              f'{statistics.mean(e["in"]):.1f} KB/s (peak {max(e["in"]):.1f}), {statistics.mean(e["din"]):.0f} datagrams/s; '
              f'lost {sum(e["lost"]):.0f}, replays {sum(e["replay"]):.0f}, auth {sum(e["auth"]):.0f}, malformed '
              f'{sum(e["malformed"]):.0f}, reassembly timeouts {sum(e["reasm"]):.0f}')
        if name == 'server' and max(e['conn']) > 0:
            print(f'  per client: out {statistics.mean(e["out"]) / max(e["conn"]):.1f} KB/s, '
                  f'in {statistics.mean(e["in"]) / max(e["conn"]):.1f} KB/s')
    if send_mean:
        print(f'SV_SendClientMessages: {statistics.mean(send_mean):.3f} ms on average, {max(send_worst):.3f} ms at most')
    if server:
        print(f'server frame: {statistics.mean(server):.2f} ms on average, {max(server):.2f} ms at most ({len(server)} s)')


main()
