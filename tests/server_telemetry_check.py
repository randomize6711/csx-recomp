#!/usr/bin/env python3
"""Checks the dedicated server's telemetry (docs/server_tool.md "Telemetry format") in a server's captured stdout.

  python3 -I tools/run_net_tests.py tests/net/net_ded_telemetry.txt
  python3 -I tests/server_telemetry_check.py build/nruns/net_ded_telemetry/server/run.out

Every "@csx status" line must be valid JSON with the version 1 fields and sane values; at least one line must show the
scenario's remote player as a human with a ping, an address and an identity key, and traffic in both directions.
"""
import json
import re
import sys

FIELDS = {'v': int, 'state': str, 'time': int, 'uptime': (int, float), 'map': str, 'frames': int,
          'tick_hz': (int, float), 'frame_ms': (int, float), 'frame_ms_max': (int, float), 'max_players': int,
          'humans': int, 'bots': int, 'port': int, 'key': str, 'password': bool, 'lan_only': bool, 'game_mode': str,
          'players': list}
PLAYER = {'slot': int, 'name': str, 'team': str, 'score': int, 'deaths': int, 'bot': bool, 'seconds': (int, float)}


def main(path):
    failures, lines = [], []
    with open(path, errors='replace') as f:
        for n, line in enumerate(f, 1):
            if line.startswith('@csx status '):
                try:
                    lines.append((n, json.loads(line[len('@csx status '):])))
                except ValueError as e:
                    failures.append(f'line {n}: not JSON: {e}')
    if len(lines) < 5:
        failures.append(f'only {len(lines)} status lines')
    human_seen = traffic_seen = False
    prev_frames = -1
    for n, d in lines:
        for k, t in FIELDS.items():
            if not isinstance(d.get(k), t):
                failures.append(f'line {n}: {k} missing or of the wrong type: {d.get(k)!r}')
        if d.get('v') != 1 or d.get('state') not in ('loading', 'running'):
            failures.append(f'line {n}: version or state: {d.get("v")} {d.get("state")}')
        if d.get('state') == 'running' and not (20 <= d.get('tick_hz', 0) <= 40):
            failures.append(f'line {n}: tick rate {d.get("tick_hz")} far from 30')
        if not re.fullmatch(r'[0-9a-f]{64}', d.get('key', '')):
            failures.append(f'line {n}: server key {d.get("key")!r}')
        if d.get('frames', 0) < prev_frames:
            failures.append(f'line {n}: frames went back')
        prev_frames = d.get('frames', 0)
        players = d.get('players', [])
        if len(players) != d.get('humans', -1) + d.get('bots', -1):
            failures.append(f'line {n}: {len(players)} players, {d.get("humans")} humans + {d.get("bots")} bots')
        for p in players:
            for k, t in PLAYER.items():
                if not isinstance(p.get(k), t):
                    failures.append(f'line {n}: player {k} wrong: {p.get(k)!r}')
            if p.get('team') not in ('T', 'CT', 'SPEC', '-'):
                failures.append(f'line {n}: team {p.get("team")!r}')
            if p.get('bot'):
                if p.get('ping') is not None or 'address' in p:
                    failures.append(f'line {n}: a bot with a ping or address')
            elif isinstance(p.get('ping'), int) and re.fullmatch(r'[0-9a-f]{64}', p.get('key') or '') and \
                    re.fullmatch(r'\d+\.\d+\.\d+\.\d+:\d+', p.get('address', '')):
                human_seen = True
        net = d.get('net') or {}
        if (net.get('in_bps') or 0) > 0 and (net.get('out_bps') or 0) > 0:
            traffic_seen = True
    if not human_seen:
        failures.append('no line shows a human player with a ping, an address and a key')
    if not traffic_seen:
        failures.append('no traffic in both directions')
    for f in failures[:20]:
        print('FAIL', f)
    print(f'{len(lines)} status lines, {len(failures)} problems')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'build/nruns/net_ded_telemetry/server/run.out'))
