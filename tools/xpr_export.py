#!/usr/bin/env python3
"""The menus' and HUD's textures as PNG files (N10, docs/n10_hud.md "HD artwork"): the starting point for redrawn or
upscaled artwork (an image editor, or Krita with its AI Diffusion plugin: an optional production tool, never needed
to run the game). Reads an XPR1 file (tools/sxwad.py xpr: an archive's entry extracted, or Z:\\media's) and writes
OUT/NAME.png for each texture it can decode: DXT1 (0x0C), DXT2/3 (0x0E), DXT4/5 (0x0F), swizzled A8R8G8B8 (0x06)
and X8R8G8B8 (0x07); others are listed and left out. The game's files are read only.

  python3 -I tools/xpr_export.py FILE.xpr OUT_DIR
"""
import os
import struct
import sys
import zlib


def png(path, w, h, rgba):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def c565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def dxt(data, w, h, kind):
    out = bytearray(w * h * 4)
    block = 8 if kind == 1 else 16
    at = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            b = data[at:at + block]
            at += block
            if len(b) < block:
                return bytes(out)
            alpha = [255] * 16
            if kind == 3:
                bits = int.from_bytes(b[:8], 'little')
                alpha = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
            elif kind == 5:
                a0, a1 = b[0], b[1]
                table = [a0, a1] + ([((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else
                                    [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
                bits = int.from_bytes(b[2:8], 'little')
                alpha = [table[(bits >> (3 * i)) & 7] for i in range(16)]
            cb = b[block - 8:]
            c0, c1, idx = struct.unpack('<HHI', cb)
            p0, p1 = c565(c0), c565(c1)
            if kind == 1 and c0 <= c1:
                cols = [p0, p1, tuple((x + y) // 2 for x, y in zip(p0, p1)), (0, 0, 0)]
                trans = 3
            else:
                cols = [p0, p1, tuple((2 * x + y) // 3 for x, y in zip(p0, p1)), tuple((x + 2 * y) // 3 for x, y in zip(p0, p1))]
                trans = -1
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x >= w or y >= h:
                    continue
                k = (idx >> (2 * i)) & 3
                r, g, bl = cols[k]
                a = 0 if k == trans else alpha[i]
                o = (y * w + x) * 4
                out[o:o + 4] = bytes((r, g, bl, a))
    return bytes(out)


def unswizzle(data, w, h, alpha):
    out = bytearray(w * h * 4)
    n = min(w, h)
    for y in range(h):
        for x in range(w):
            # interleaved bits (x first) within the square part, then the longer side's rest
            i, bit, xx, yy = 0, 0, x, y
            sx, sy = w, h
            while sx > 1 or sy > 1:
                if sx > 1:
                    i |= (xx & 1) << bit
                    bit += 1
                    xx >>= 1
                    sx >>= 1
                if sy > 1:
                    i |= (yy & 1) << bit
                    bit += 1
                    yy >>= 1
                    sy >>= 1
            b, g, r, a = data[4 * i:4 * i + 4]
            o = (y * w + x) * 4
            out[o:o + 4] = bytes((r, g, b, a if alpha else 255))
    del n
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    b = open(sys.argv[1], 'rb').read()
    magic, total, header, count = struct.unpack_from('<4sIII', b, 0)
    if magic != b'XPR1':
        sys.exit('not an XPR1 file')
    os.makedirs(sys.argv[2], exist_ok=True)
    done = skipped = 0
    for i in range(count):
        name_off, res_off = struct.unpack_from('<II', b, 16 + 8 * i)
        name = b[12 + name_off:].split(b'\0')[0].decode('latin1')
        common, dataofs, lock, fmt, sz = struct.unpack_from('<IIIII', b, 12 + res_off)
        d3d = (fmt >> 8) & 0xFF
        w, h = 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF)
        if sz:
            w, h = (sz & 0xFFF) + 1, ((sz >> 12) & 0xFFF) + 1
        data = b[header + dataofs:]
        if d3d in (0x0C, 0x0E, 0x0F):
            rgba = dxt(data, w, h, {0x0C: 1, 0x0E: 3, 0x0F: 5}[d3d])
        elif d3d in (0x06, 0x07):
            rgba = unswizzle(data, w, h, d3d == 0x06)
        else:
            print(f'  {name}: format {d3d:#04x} left out')
            skipped += 1
            continue
        safe = name.replace('/', '_').replace('\\', '_')
        png(os.path.join(sys.argv[2], safe + '.png'), w, h, rgba)
        done += 1
    print(f'{done} textures written to {sys.argv[2]}, {skipped} left out')


if __name__ == '__main__':
    main()
