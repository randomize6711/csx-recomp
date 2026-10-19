#!/usr/bin/env python3
"""Compares two captures of the same frame (any formats PIL reads, same size or resized to the first).

Usage: python3 -I tools/compare_images.py A B [DIFF.png]
Prints mean and maximum absolute difference per channel, PSNR, and the share of pixels differing by more than 8 and
32 levels; DIFF.png gets the absolute difference scaled x4.
"""
import math
import sys

from PIL import Image, ImageChops, ImageStat

a = Image.open(sys.argv[1]).convert('RGB')
b = Image.open(sys.argv[2]).convert('RGB')
if b.size != a.size:
    b = b.resize(a.size, Image.BILINEAR)
d = ImageChops.difference(a, b)
st = ImageStat.Stat(d)
mse = sum(v * v for v in st.rms) / 3
psnr = 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)
px = list(d.convert("L").get_flattened_data())
over8 = sum(1 for v in px if v > 8) / len(px)
over32 = sum(1 for v in px if v > 32) / len(px)
print(f'size {a.size}  mean {[round(v, 2) for v in st.mean]}  max {[e[1] for e in d.getextrema()]}  '
      f'PSNR {psnr:.1f} dB  >8: {over8 * 100:.2f}%  >32: {over32 * 100:.2f}%')
if len(sys.argv) > 3:
    d.point(lambda v: min(255, v * 4)).save(sys.argv[3])
