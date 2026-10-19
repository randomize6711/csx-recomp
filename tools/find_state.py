#!/usr/bin/env python3
"""Searches guest memory dumps (src/common/memory_dump.cpp) for state that follows controller input.

Usage: python3 -I tools/find_state.py DUMPDIR

Expects dumps named idle0, idle1 (no input), move_mid, move_end (left stick forward held between idle1 and
move_end), idle2 (released), turned (right stick right after idle2). Reports:
  position candidates: float triples, finite, |v| < 8192, unchanged idle0 -> idle1, changed idle1 -> move_mid and
    move_mid -> move_end by a plausible distance (2..2000 units), unchanged move_end -> idle2
  angle candidates: floats in [-360, 360] unchanged idle1 -> idle2 that change idle2 -> turned by 2..180 degrees
"""
import struct
import sys

import numpy as np


def load(path):
    regions = {}
    with open(path, 'rb') as f:
        data = f.read()
    off = 0
    while off + 8 <= len(data):
        va, n = struct.unpack_from('<II', data, off)
        regions[va] = np.frombuffer(data, dtype='<f4', count=n // 4, offset=off + 8)
        off += 8 + n
    return regions


def common(dumps):
    keys = set(dumps[0])
    for d in dumps[1:]:
        keys &= set(d)
    out = {}
    for va in sorted(keys):
        n = min(len(d[va]) for d in dumps)
        out[va] = [d[va][:n] for d in dumps]
    return out


def main():
    d = sys.argv[1]
    names = ['idle0', 'idle1', 'move_mid', 'move_end', 'idle2', 'turned']
    dumps = [load(f'{d}/{n}.bin') for n in names]
    regs = common(dumps)
    pos, ang = [], []
    with np.errstate(all='ignore'):
        for va, (i0, i1, mm, me, i2, tu) in regs.items():
            n = len(i0) - 2
            if n <= 0:
                continue
            def tri(a):
                return np.stack([a[:n], a[1:n + 1], a[2:n + 2]], axis=1)
            I0, I1, MM, ME, I2 = tri(i0), tri(i1), tri(mm), tri(me), tri(i2)
            ok = np.all(np.isfinite(I1) & np.isfinite(MM) & np.isfinite(ME), axis=1)
            ok &= np.all(np.abs(I1) < 8192, axis=1) & np.all(np.abs(ME) < 8192, axis=1)
            ok &= np.all(I0 == I1, axis=1) & np.all(ME == I2, axis=1)
            d1 = np.linalg.norm(MM - I1, axis=1)
            d2 = np.linalg.norm(ME - MM, axis=1)
            ok &= (d1 > 2) & (d1 < 2000) & (d2 > 2) & (d2 < 2000)
            ok &= np.any(I1 != 0, axis=1)
            for k in np.nonzero(ok)[0]:
                pos.append((va + 4 * int(k), I1[k], MM[k], ME[k]))
            a1, a2, at = i1[:len(i1)], i2, tu
            okA = np.isfinite(a2) & np.isfinite(at) & (np.abs(a2) <= 360) & (np.abs(at) <= 360)
            okA &= (a1 == a2) & (np.abs(at - a2) > 2) & (np.abs(at - a2) < 180) & (a2 != 0)
            for k in np.nonzero(okA)[0]:
                ang.append((va + 4 * int(k), float(a2[k]), float(at[k])))
    print(f'position candidates: {len(pos)}')
    for va, a, b, c in pos[:60]:
        print(f'  {va:08X}  idle {np.round(a, 2)}  mid {np.round(b, 2)}  end {np.round(c, 2)}  moved {np.linalg.norm(c - a):.1f}')
    print(f'angle candidates: {len(ang)}')
    for va, a, b in ang[:60]:
        print(f'  {va:08X}  {a:9.3f} -> {b:9.3f}')


if __name__ == '__main__':
    main()
