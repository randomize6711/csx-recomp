#!/usr/bin/env python3
"""Checks that menu hover sounds are audible in a capture of the mixer output (src/linux/sdl_audio.cpp CSX_AUDIO_WAV,
48 kHz stereo float) of a run with CSX_LOG_MENU_SOUNDS=1 (src/game/diag_hooks.cpp logs every menu sound the game
plays: "menu sound NAME"; src/game/pc_mouse.cpp logs "pc mouse: highlight NAME" when the mouse made it play).

Usage: python3 -I tools/check_menu_audio.py WAV RUN_OUT [--sound menu/buy_hover.wav] [--min-ratio R]

The sound's waveform is learnt from the plays the game made by itself (its d-pad or stick: no pc mouse line at that
moment), averaged after aligning them (a game play is measured against the others' average); each play is then found in the capture with that template (high-passed, so the
menu music matters less): the template's best match within 60 ms after the logged time, against the best match at
control moments halfway between plays. Passes when every mouse play matches at least R times (default 3) the median
control match, and the game's own plays do too (the capture really holds the sound). Prints one line per play.
"""
import re
import struct
import sys

import numpy as np

RATE = 48000


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    wav, log = args[0], args[1]
    sound = args[args.index('--sound') + 1] if '--sound' in args else 'menu/buy_hover.wav'
    min_ratio = float(args[args.index('--min-ratio') + 1]) if '--min-ratio' in args else 3.0
    d = open(wav, 'rb').read()
    fmt, ch, rate = struct.unpack_from('<HHI', d, 20)
    if (fmt, ch, rate) != (3, 2, RATE):
        sys.exit('not 48 kHz stereo float: %s' % ((fmt, ch, rate),))
    i = d.index(b'data') + 8
    pcm = np.frombuffer(d[i:i + (len(d) - i) // 8 * 8], dtype='<f4').reshape(-1, 2).mean(axis=1)
    hp = np.diff(pcm, prepend=0.0)  # first difference: a high-pass that takes most of the music's weight away

    start, plays = None, []  # plays: (log time, made by the mouse)
    lines = open(log, encoding='utf-8', errors='replace').read().splitlines()
    stamp = re.compile(r'^\[\s*([0-9.]+)\]')
    for n, line in enumerate(lines):
        m = stamp.match(line)
        if not m:
            continue
        t = float(m.group(1))
        if start is None and 'audio: capturing the output' in line:
            start = t
        if line.endswith('menu sound ' + sound):
            # made by the PC side: a pc mouse line at the same moment (a hover's highlight, a key of the virtual
            # keyboard selected, a typed character)
            near = [x for x in lines[max(0, n - 40):n + 40]
                    if stamp.match(x) and abs(float(stamp.match(x).group(1)) - t) < 0.002]
            mouse = any('pc mouse: highlight' in x or 'pc mouse: key row' in x or 'pc mouse: typed' in x for x in near)
            plays.append((t, mouse))
    if start is None:
        sys.exit('no "audio: capturing the output" line in %s' % log)
    # only plays the capture holds whole (it may stop before the log does)
    plays = [(t, m) for t, m in plays if (t - start + 0.5) * RATE < len(hp) and t - start > 0.1]
    game = [t for t, mouse in plays if not mouse]
    mouse = [t for t, m in plays if m]
    if len(game) < 2 or not mouse:
        sys.exit('need at least 2 plays by the game and 1 by the mouse (have %d and %d)' % (len(game), len(mouse)))

    def at(t):
        return int(round((t - start) * RATE))

    length = int(0.2 * RATE)
    # the game's plays aligned on the first (best correlation within 40 ms)
    ref = hp[at(game[0]):at(game[0]) + length][:int(0.1 * RATE)].copy()
    aligned = {}
    for t in game:
        seg = hp[at(t) - int(0.04 * RATE):at(t) + int(0.04 * RATE) + length]
        k = int(np.argmax(np.correlate(seg[:int(0.08 * RATE) + len(ref)], ref, mode='valid')))
        aligned[t] = seg[k:k + length]

    # the template: their average; a game play is measured against the others' average (it would favour itself)
    def template(leave=None):
        tpl = np.mean([v for t, v in aligned.items() if t != leave], axis=0)
        return tpl / (np.linalg.norm(tpl) or 1.0)

    full = template()

    def match(t, tpl=full):
        seg = hp[at(t) - int(0.01 * RATE):at(t) + int(0.07 * RATE) + length]
        if len(seg) < length:
            return 0.0
        return float(np.max(np.abs(np.correlate(seg, tpl, mode='valid'))))

    times = sorted(t for t, _ in plays)
    controls = [(a + b) / 2 for a, b in zip(times, times[1:]) if b - a > 0.6]
    base = float(np.median([match(t) for t in controls])) if controls else 0.0
    if base <= 0:
        sys.exit('no control moments between the plays (they need 0.6 s between them)')
    ok = True
    print('%d plays by the game, %d by the mouse; control match median %.5f (%d moments)' %
          (len(game), len(mouse), base, len(controls)))
    for t, m in plays:
        r = match(t, full if m else template(t)) / base
        good = r >= min_ratio
        ok = ok and good
        print('%s %8.3f s  %-5s  match %6.1f x control' % ('ok  ' if good else 'FAIL', t, 'mouse' if m else 'game', r))
    for t in controls:
        print('     %8.3f s  control match %6.1f x' % (t, match(t) / base))
    print('audible' if ok else 'NOT audible everywhere')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
