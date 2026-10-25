#!/usr/bin/env python3
"""Checks the scoreboard's rows in a run's log (CSX_LOG_SCOREBOARD=1, src/game/scoreboard.cpp; FINDINGS.md
"Scoreboard"): in every logged snapshot each section's players are in the game's order (score down, then deaths up),
a team header's count is its players, every bot's Ping is BOT and no human's is, a human's Ping is "-" or a number,
no player number appears twice, the window's numbers agree with the rows marked on screen; and over the run every
player of the largest list was on screen at some point (scrolling reached them all). A network client's run is read
up to its disconnect. The header counts are checked only while the Scoreboard screen is open: the hidden panel
refreshes its rows on a score change, but the game recounts its teams only when the board opens (vk_scoreboard_169:
a kill after the last bot joined logged a hidden header one behind its rows, right again the moment it opened).

Usage: python3 -I tools/check_scoreboard.py RUN_DIR [RUN_DIR ...]   (build/wruns/vk_scoreboard_169 ...)
Exit status 1 with the problems listed when a check fails.
"""
import re
import sys

ROW = re.compile(r'([|~]) (?:(team) (\d+) \((-?\d+)\)|(spectators)|more (\d+)|(\d+) "(.*?)" t(-?\d+) (-?\d+) (-?\d+) '
                 r'ping=(\S+) (bot|human))(?= [|~] |$)')


def check(run):
    problems = []
    seen_on, biggest = set(), 0
    snapshots = 0
    shown = False  # the Scoreboard screen is open
    for line in open(run + '/run.out', errors='replace'):
        # a network client's rows end with its connection: the player list goes first, the team headers' counts a
        # moment later (a snapshot in between is the disconnect's, not the scoreboard's)
        if re.search(r'net: server \S+ closed', line):
            break
        if m := re.search(r'pc mouse: screen (\S+)|pc keys: playing', line):
            shown = m.group(1) == 'Scoreboard'
        if 'scoreboard: ' not in line or ' players in ' not in line:
            continue
        head, _, rest = line.rstrip('\n').partition(';')
        m = re.search(r'scoreboard: (\d+) players in (\d+) rows, (\d+) lines, offset (\d+): players (\d+)-(\d+) of (\d+), '
                      r'(\d+) below', head)
        if not m:
            problems.append('unreadable: ' + head[:120])
            continue
        snapshots += 1
        players, lines, first, last, below = (int(m.group(i)) for i in (1, 3, 5, 6, 8))
        where = head[:12]
        sections, numbers, on_lines, on_players = [], [], 0, []
        for r in ROW.finditer(rest):
            on = r.group(1) == '|'
            if r.group(2):
                sections.append([int(r.group(4)), []])
                on_lines += on
            elif r.group(5):
                sections.append([None, []])
                on_lines += on
            elif r.group(6):
                on_lines += 1
                if int(r.group(6)) != below:
                    problems.append('%s the indicator says %s, the head %d below' % (where, r.group(6), below))
            else:
                who, name, score, deaths, ping, kind = (r.group(7), r.group(8), int(r.group(10)), int(r.group(11)),
                                                        r.group(12), r.group(13))
                numbers.append(who)
                if not sections:
                    sections.append([None, []])
                sections[-1][1].append((score, deaths, who))
                on_lines += on
                if on:
                    on_players.append(who)
                if kind == 'bot' and ping != 'BOT':
                    problems.append('%s bot %s "%s" shows %s' % (where, who, name, ping))
                if kind == 'human' and not (ping == '-' or ping.isdigit()):
                    problems.append('%s human %s "%s" shows %s' % (where, who, name, ping))
        if len(numbers) != len(set(numbers)):
            problems.append('%s a player number twice' % where)
        if len(numbers) != players:
            problems.append('%s %d player rows, the head says %d' % (where, len(numbers), players))
        if on_lines > lines:
            problems.append('%s %d lines on screen, the table has %d' % (where, on_lines, lines))
        if players and len(on_players) != (last - first + 1 if first else 0):
            problems.append('%s %d players on screen, the head says %d-%d' % (where, len(on_players), first, last))
        for count, rows in sections:
            if shown and count is not None and count != len(rows):
                problems.append('%s a team header counts %d, it has %d players' % (where, count, len(rows)))
            for a, b in zip(rows, rows[1:]):
                if (a[0], -a[1]) < (b[0], -b[1]):
                    problems.append('%s out of order: %s (%d/%d) before %s (%d/%d)' % (where, a[2], a[0], a[1], b[2],
                                                                                    b[0], b[1]))
        if players >= biggest:
            if players > biggest:
                seen_on = set()
            biggest = players
            seen_on |= set(on_players)
    if not snapshots:
        problems.append('no scoreboard rows logged (CSX_LOG_SCOREBOARD=1?)')
    elif len(seen_on) < biggest:
        problems.append('only %d of %d players were ever on screen' % (len(seen_on), biggest))
    return snapshots, biggest, problems


def main():
    bad = False
    for run in sys.argv[1:]:
        snapshots, biggest, problems = check(run.rstrip('/'))
        print('%s: %d snapshots, %d players at most, %s' % (run, snapshots, biggest,
                                                             'all checks pass' if not problems else
                                                             '%d problems' % len(problems)))
        for p in problems[:20]:
            print('  ' + p)
        bad |= bool(problems)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
