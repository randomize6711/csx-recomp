#!/usr/bin/env python3
"""Checks 3D sound distance attenuation from a run's log (FINDINGS.md "Audio").

Usage: python3 -I tools/check_audio3d.py LOG

The run needs CSX_AUDIO_LOG3D=1 (src/game/audio3d_probe.cpp: each 3D sound's distance and the distance level XACT
computed, "audio3d: voice V distance D min M max X level L mB") and XBE_LOG_3D=1 (the mixer's gain for each routed 3D
voice and the 3D level it applied, "dsound: voice V routed gain L g R g, 3D level L mB"). Checks:
  - XACT's level falls as the distance grows beyond the minimum distance (rank correlation) and is 0 inside it
  - the mixer applies XACT's latest level for the voice (the level reaches the mixer)
  - far voices (level -800 mB or lower) play at least 6 dB below voices at 0 mB in the mixer
Exit status 0 when all hold.
"""
import math
import re
import sys

PROBE = re.compile(r'audio3d: voice (\w+) distance ([\d.]+) min (\d+) max (\d+) level (-?\d+) mB')
GAIN = re.compile(r'dsound: voice (\w+) routed gain L ([\d.]+) R ([\d.]+), 3D level (-?\d+) mB')


def ranks(xs):
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    r = [0.0] * len(xs)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and xs[order[j + 1]] == xs[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2
        i = j + 1
    return r


def spearman(a, b):
    ra, rb = ranks(a), ranks(b)
    ma, mb = sum(ra) / len(ra), sum(rb) / len(rb)
    cov = sum((x - ma) * (y - mb) for x, y in zip(ra, rb))
    va = math.sqrt(sum((x - ma) ** 2 for x in ra))
    vb = math.sqrt(sum((y - mb) ** 2 for y in rb))
    return cov / (va * vb) if va and vb else 0.0


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    latest = {}  # voice -> XACT's last level
    outside, inside = [], []
    matched = total = 0
    near, far = [], []
    for line in open(sys.argv[1], errors='replace'):
        m = PROBE.search(line)
        if m:
            v, d, lo, level = m.group(1), float(m.group(2)), float(m.group(3)), int(m.group(5))
            latest[v] = level
            hi = float(m.group(4))
            (outside if d > lo else inside).append((d, level, lo, hi))
            continue
        m = GAIN.search(line)
        if m:
            v, peak, level = m.group(1), max(float(m.group(2)), float(m.group(3))), int(m.group(4))
            if v in latest and level != 0:
                total += 1
                matched += abs(latest[v] - level) <= 1
            if level == 0:
                near.append(peak)
            elif level <= -800:
                far.append(peak)
    ok = True

    def report(good, text):
        nonlocal ok
        print(('ok   ' if good else 'FAIL ') + text)
        ok = ok and good

    # per sound class (minimum and maximum distance), within the range where the level can still change
    classes = {}
    for d, l, lo, hi in outside:
        if d < hi and l > -10000:
            classes.setdefault((lo, hi), []).append((d, l))
    tested = 0
    for (lo, hi), xs in sorted(classes.items()):
        if len(xs) < 20:
            continue
        tested += 1
        rho = spearman([d for d, _ in xs], [l for _, l in xs])
        report(rho < -0.95, f'XACT level falls with distance, min {lo:.0f} max {hi:.0f}: {len(xs)} samples, rank '
                            f'correlation {rho:.3f}')
    report(tested > 0, f'{tested} sound classes with enough samples')
    report(all(l == 0 for _, l, _, _ in inside), f'XACT level 0 inside the minimum distance ({len(inside)} samples)')
    report(total >= 10 and matched >= 0.95 * total, f'mixer applies XACT\'s level: {matched} of {total} gain changes')
    if near and far:
        db = 20 * math.log10((sum(near) / len(near)) / (sum(far) / len(far)))
        report(db >= 6, f'far voices {db:.1f} dB below voices at 0 mB ({len(far)} far, {len(near)} near gain changes)')
    else:
        report(False, f'near and far voices both heard ({len(near)} near, {len(far)} far gain changes)')
    for (lo, hi), xs in sorted(classes.items()):
        for d, l in sorted(xs)[:: max(1, len(xs) // 6)]:
            print(f'     min {lo:.0f} max {hi:.0f}: distance {d:7.1f}  level {l:6d} mB  gain {10 ** (l / 2000):.3f}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
