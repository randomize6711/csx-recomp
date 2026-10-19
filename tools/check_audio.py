#!/usr/bin/env python3
"""Checks a capture of the mixer output (src/linux/sdl_audio.cpp, CSX_AUDIO_WAV): 48 kHz stereo float.

Usage: python3 -I tools/check_audio.py FILE [--from S] [--min-seconds S]
Fails (exit 1) unless, from second S on: the audio is not silent (mean RMS > 0.002), some second is placed clearly
to one side (|balance| >= 0.2, balance = (R - L) / (R + L) of the RMS: 3D sounds through their submix and azimuth),
under 0.5% of the samples are at full scale, and every sample is finite. Prints the per-second figures.
"""
import array
import math
import struct
import sys


def main():
    args = sys.argv[1:]
    path, start, min_seconds = args[0], 15, 30
    if '--from' in args:
        start = int(args[args.index('--from') + 1])
    if '--min-seconds' in args:
        min_seconds = int(args[args.index('--min-seconds') + 1])
    d = open(path, 'rb').read()
    fmt, ch, rate = struct.unpack_from('<HHI', d, 20)
    if (fmt, ch, rate) != (3, 2, 48000):
        sys.exit('not 48 kHz stereo float: %s' % ((fmt, ch, rate),))
    i = d.index(b'data') + 8
    s = array.array('f')
    s.frombytes(d[i:i + (len(d) - i) // 8 * 8])
    seconds = len(s) // 2 // rate
    ok = seconds >= min_seconds
    print('%d s captured%s' % (seconds, '' if ok else ' (too short)'))
    rms_sum, sided, clipped, total, bad = 0.0, 0, 0, 0, 0
    for t in range(start, seconds):
        seg = s[t * rate * 2:(t + 1) * rate * 2]
        bad += sum(1 for x in seg if not math.isfinite(x))
        l, r = seg[0::2], seg[1::2]
        rl = math.sqrt(sum(x * x for x in l) / rate)
        rr = math.sqrt(sum(x * x for x in r) / rate)
        bal = (rr - rl) / (rr + rl + 1e-9)
        c = sum(1 for x in seg if abs(x) >= 0.999)
        rms_sum += (rl + rr) / 2
        sided += abs(bal) >= 0.2
        clipped += c
        total += len(seg)
        print('%3ds  rms L %.4f R %.4f  balance %+.2f  full scale %d' % (t, rl, rr, bal, c))
    n = max(seconds - start, 1)
    checks = [('not silent', rms_sum / n > 0.002), ('placed to a side at least once', sided > 0),
              ('full scale under 0.5%', clipped < 0.005 * max(total, 1)), ('finite', bad == 0)]
    for name, good in checks:
        print('%s %s' % ('ok  ' if good else 'FAIL', name))
        ok = ok and good
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
