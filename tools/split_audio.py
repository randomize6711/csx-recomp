#!/usr/bin/env python3
"""Split-screen audio check (docs/splitscreen.md 8): every player's own stream as its process sent it to the device
(CSX_AUDIO_WAV=audio.wav: 48 kHz stereo float, after Master Volume and the split-screen gain), read with the game
sounds each process started (CSX_LOG_SOUNDS=1: "pN: sound NAME ent E chan C vol V at X Y Z") and the menu sounds
(CSX_LOG_MENU_SOUNDS=1). The streams are lined up in time through each capture's start ("audio: capturing the output
to audio.wav from frame F" at a log time).

Per player: length, level (RMS of the second-long windows with sound), peak, clipped samples, late device callbacks;
each kind of sound (gunfire, footsteps, reloads, damage, radio, menu, impacts, other) as started and as heard: an onset in that
player's stream within 0.3 s of the start (a 25 ms window's energy at least twice that of the 0.3 s before, or over
-40 dBFS) and duplicates (the same sound on the same entity twice within 20 ms). The streams added
together (what the system mixer plays: PipeWire sums them): peak and clipping. Positional: a sound of another entity
heard while this player's position and view are known (a "state" line of that player within 5 s before it): the side
it should come from (left or right of the view, from the positions) against the stream's balance in the 0.25 s after
it (louder channel), for sounds at least 30 degrees off the view's axis.

Usage: python3 -I tools/split_audio.py RUN [--players N]   (RUN: build/wruns/<scenario>)
Exit 1 when a stream is missing, a stream or the mix clips, a stream has late callbacks, or less than 80% of the
player's own gunfire (on its own entity) is heard in its own stream."""
import math
import os
import re
import struct
import sys

RATE = 48000


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return None
    # the data chunk: the header may lag the file (written every two seconds), so take the file's bytes
    at = data.find(b'data', 12)
    body = data[at + 8:]
    n = len(body) // 8
    vals = struct.unpack('<%df' % (n * 2), body[:n * 8])
    return vals[0::2], vals[1::2]


def classify(name):
    n = name.lower()
    if 'step' in n or 'ladder' in n:
        return 'footsteps'
    if n.startswith('radio/') or '/radio' in n or n.startswith('!'):  # radio: the sentences (!MRAD_...)
        return 'radio'
    if any(k in n for k in ('clipout', 'clipin', 'slide', 'boltpull', 'boltdown', 'reload', 'magout', 'magin', 'draw',
                             'coverup', 'cliprelease', 'deploy')):
        return 'reload'
    if any(k in n for k in ('pain', 'bhit', 'die', 'death', 'headshot', 'kevlar', 'damage')):
        return 'damage'
    if 'ric' in n.split('/')[-1][:4] or 'shell' in n or 'bullet' in n:
        return 'impacts'  # ricochets, shells: quiet at a distance, not counted as heard or not
    if 'explode' in n or 'debris' in n:
        return 'other'
    if n.startswith('weapons/') or n.startswith('*weapons/'):
        return 'gunfire'
    return 'other'


def window_energy(ch, a, b):
    a, b = max(0, a), min(len(ch), b)
    if b <= a:
        return 0.0
    return sum(x * x for x in ch[a:b]) / (b - a)


def db(e):
    return 10 * math.log10(e) if e > 1e-20 else -200.0


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    run = args[0]
    players = int(args[args.index('--players') + 1]) if '--players' in args else 4
    log = open(os.path.join(run, 'run.out'), encoding='utf-8', errors='replace').read().splitlines()
    line_rx = re.compile(r'^\[\s*([\d.]+)\] \[\w+\] \[\w+\] p(\d): (.*)$')
    seg = {}       # player -> list of (log time, first frame)
    sounds = {}    # player -> list of (time, name, ent, origin)
    menus = {}
    states = {}    # player -> list of (time, x, y, z, yaw)
    late = {}
    last = {}      # player -> its last log line's time (its capture's end)
    for l in log:
        m = line_rx.match(l)
        if not m:
            continue
        t, p, text = float(m.group(1)), int(m.group(2)), m.group(3)
        last[p] = t
        c = re.match(r'audio: capturing the output to \S+ from frame (\d+)', text)
        if c:
            seg.setdefault(p, []).append((t, int(c.group(1))))
            continue
        s = re.match(r'sound (\S+) ent (-?\d+) chan (-?\d+) vol ([\d.]+) at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)', text)
        if s:
            sounds.setdefault(p, []).append((t, s.group(1), int(s.group(2)), tuple(float(s.group(i)) for i in (5, 6, 7))))
            continue
        ms = re.match(r'menu sound (\S+)', text)
        if ms:
            menus.setdefault(p, []).append((t, ms.group(1)))
            continue
        st = re.match(r'state \S+ (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)', text)
        if st:
            states.setdefault(p, []).append((t,) + tuple(float(st.group(i)) for i in range(1, 5)))
            continue
        lt = re.match(r'audio: (\d+) late device callbacks of (\d+)', text)
        if lt:
            late[p] = (int(lt.group(1)), int(lt.group(2)))
    ok = True
    streams = {}
    print('player  seconds  level dBFS  peak dBFS  clipped  late callbacks')
    for p in range(1, players + 1):
        path = os.path.join(run, 'audio.wav') if p == 1 else os.path.join(run, 'split%d' % p, 'audio.wav')
        w = read_wav(path) if os.path.exists(path) else None
        if not w or not seg.get(p):
            print('%d       no capture (%s)' % (p, path))
            ok = False
            continue
        L, R = w
        streams[p] = (L, R)
        peak = max(max(abs(x) for x in L), max(abs(x) for x in R)) if L else 0.0
        clipped = sum(1 for x in L if abs(x) >= 0.999) + sum(1 for x in R if abs(x) >= 0.999)
        secs = [window_energy(L, i, i + RATE) + window_energy(R, i, i + RATE) for i in range(0, len(L), RATE)]
        active = [e / 2 for e in secs if e / 2 > 1e-7]
        level = db(sum(active) / len(active)) if active else -200.0
        lc = late.get(p, (0, 0))
        print('%d       %7.1f  %10.1f  %9.1f  %7d  %d of %d' % (p, len(L) / RATE, level, db(peak * peak), clipped, lc[0], lc[1]))
        ok = ok and clipped == 0 and lc[0] == 0

    # each capture segment's frames a second as it ran: the device (SDL's dummy one in the tests) is not exactly the
    # log's clock, and over minutes the difference moves a sound by a large part of a second
    rates = {}
    for p in streams:
        rates[p] = []
        for i, (t0, f0) in enumerate(seg[p]):
            t1, f1 = (seg[p][i + 1][0], seg[p][i + 1][1]) if i + 1 < len(seg[p]) else (last[p], len(streams[p][0]))
            rates[p].append((f1 - f0) / (t1 - t0) if t1 - t0 > 5 and f1 > f0 else RATE)
        print('player %d: capture %s frames a second against the log' % (p, ', '.join('%.0f' % r for r in rates[p])))

    def frame_of(p, t):
        # the capture segment the time falls in (a relaunch continues the file from its frame count)
        i = len([s for s in seg[p] if s[0] <= t]) - 1
        if i < 0:
            return None
        t0, f0 = seg[p][i]
        return f0 + int((t - t0) * rates[p][i])

    # the mix (the streams added at their times)
    if len(streams) > 1:
        start = max(seg[p][0][0] for p in streams)
        ends = [seg[p][-1][0] + (len(streams[p][0]) - seg[p][-1][1]) / RATE for p in streams]
        end = min(ends)
        peak, clip, n = 0.0, 0, 0
        t = start
        while t < end:
            fr = {p: frame_of(p, t) for p in streams}
            for i in range(0, RATE // 10, 4):  # every 4th frame of each tenth of a second
                for ch in (0, 1):
                    v = 0.0
                    for p, f in fr.items():
                        if f is not None and 0 <= f + i < len(streams[p][ch]):
                            v += streams[p][ch][f + i]
                    peak = max(peak, abs(v))
                    clip += abs(v) >= 0.999
                    n += 1
            t += 0.1
        print('mix of %d streams over %.0f s: peak %.1f dBFS, %d of %d samples clipped' %
              (len(streams), end - start, db(peak * peak), clip, n))
        ok = ok and clip == 0

    kinds = ['gunfire', 'footsteps', 'reload', 'damage', 'radio', 'menu', 'impacts', 'other']
    print('\nsounds started / heard (an onset in the player\'s own stream within 0.3 s), duplicates')
    print('player  ' + '  '.join('%-14s' % k for k in kinds))
    own_ent, own_gun = {}, {}
    for p in sounds:  # a radio sentence a player's own client plays is on its own entity
        for _, name, ent, _ in sounds[p]:
            if name.startswith('!MRAD') and ent > 0:
                own_ent[p] = ent
                break
    for p in sorted(streams):
        L, R = streams[p]
        got = {k: [0, 0] for k in kinds}
        own_heard = [0, 0]
        dup = 0
        events = [(t, classify(n), n, e) for t, n, e, _ in sounds.get(p, [])] + [(t, 'menu', n, -1) for t, n in menus.get(p, [])]
        last = {}
        for t, kind, name, ent in events:
            key = (name, ent)
            if key in last and t - last[key] < 0.02 and kind != 'menu':
                dup += 1
            last[key] = t
            f = frame_of(p, t)
            if f is None or f >= len(L):
                continue
            got[kind][0] += 1
            before = window_energy(L, f - int(RATE * 0.3), f - RATE // 50) + window_energy(R, f - int(RATE * 0.3), f - RATE // 50)
            after = max(window_energy(L, f + k, f + k + RATE // 40) + window_energy(R, f + k, f + k + RATE // 40)
                        for k in range(0, int(RATE * 0.3), RATE // 80))
            heard = after >= 2 * before or after / 2 > 10 ** (-40 / 10)
            got[kind][1] += heard
            if kind == 'gunfire' and ent == own_ent.get(p) and ent >= 0:
                own_heard[0] += 1
                own_heard[1] += heard
        print('%d       ' % p + '  '.join('%-14s' % ('%d / %d' % tuple(got[k])) for k in kinds) + '  duplicates %d' % dup)
        own_gun[p] = own_heard
    print('\nown gunfire (the player\'s own entity: the one its own radio sentences play on), started / heard')
    for p in sorted(streams):
        n_all, n_heard = own_gun.get(p, (0, 0))
        print('%d       %d / %d%s' % (p, n_all, n_heard, '' if own_ent.get(p) is not None else '  (own entity not found)'))
        if n_all == 0 or n_heard < 0.8 * n_all:
            ok = False

    print('\npositional: sounds of other entities 30+ degrees off the listener\'s view, left or right as expected')
    for p in sorted(streams):
        L, R = streams[p]
        right = wrong = 0
        for t, name, ent, org in sounds.get(p, []):
            st = [s for s in states.get(p, []) if 0 <= t - s[0] <= 5]
            if not st or org == (0.0, 0.0, 0.0):
                continue
            _, x, y, z, yaw = st[-1]
            dx, dy = org[0] - x, org[1] - y
            if math.hypot(dx, dy) < 64:
                continue  # its own sounds (at its own position)
            ang = math.degrees(math.atan2(dy, dx)) - yaw
            ang = (ang + 180) % 360 - 180   # + to the left of the view
            if abs(ang) < 30 or abs(ang) > 150:
                continue
            f = frame_of(p, t)
            if f is None:
                continue
            el, er = window_energy(L, f, f + RATE // 4), window_energy(R, f, f + RATE // 4)
            if el + er < 1e-9:
                continue
            if (el > er) == (ang > 0):
                right += 1
            else:
                wrong += 1
        print('%d       %d from the expected side, %d from the other' % (p, right, wrong))
    print('\nRESULT %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
