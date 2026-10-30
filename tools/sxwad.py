#!/usr/bin/env python3
"""List and extract the game's .SXWad archives (read only; extracted files go where you say).

The format (FrontEnd.SXWad and the maps'): u32 version (1), u32 count, u32 the table's end, u32 0, then the table,
one entry each:
u32 original size, u32 stored size, u32 offset, u32 name length, the name (a Windows path) and its zero, padded to
4 bytes. Each stored file is zlib (the stored size equal to the original: stored as is).

  tools/sxwad.py list ARCHIVE [PATTERN]
  tools/sxwad.py extract ARCHIVE PATTERN OUTDIR     (every entry whose name holds PATTERN, case-insensitive)
  tools/sxwad.py xpr FILE                           (an XPR1's named resources: name, type, format, size)
"""
import os
import struct
import sys
import zlib


def entries(path):
    with open(path, 'rb') as f:
        b = f.read()
    version, count = struct.unpack_from('<II', b, 0)
    at = 16
    out = []
    for _ in range(count):
        size, stored, offset, n = struct.unpack_from('<IIII', b, at)
        name = b[at + 16:at + 16 + n].split(b'\0')[0].decode('latin1')
        at = (at + 16 + n + 1 + 3) & ~3  # the name, its zero, padded to 4
        out.append((name, size, stored, offset))
    return b, out


def data(b, e):
    """an entry's bytes; None for a placeholder (original size 0xFFFFFFFF, nothing stored: server.cfg's, a file that
    is not there)"""
    name, size, stored, offset = e
    if size == 0xFFFFFFFF:
        return None
    raw = b[offset:offset + stored]
    return raw if stored == size else zlib.decompress(raw)


def xpr(path):
    """An XPR1: 'XPR1', u32 total size, u32 header size, u32 count; then count pairs (u32 name offset, u32 resource
    offset), both from byte 12, ended by a 0; the names; the D3D resource headers (common, data, lock, format, size);
    the data after the header."""
    with open(path, 'rb') as f:
        b = f.read()
    magic, total, header, count = struct.unpack_from('<4sIII', b, 0)
    print(f'{magic.decode()} total {total} header {header:#x} {count} resources')
    for i in range(count):
        name_off, res_off = struct.unpack_from('<II', b, 16 + 8 * i)
        name = b[12 + name_off:].split(b'\0')[0].decode('latin1')
        common, dataofs, lock, fmt, sz = struct.unpack_from('<IIIII', b, 12 + res_off)
        # the texture format's D3DFORMAT byte and, for a swizzled one, its log2 width / height (format bits 20.., 24..)
        d3dfmt = (fmt >> 8) & 0xFF
        w, h = 1 << ((fmt >> 20) & 0xF), 1 << ((fmt >> 24) & 0xF)
        if sz:
            w, h = (sz & 0xFFF) + 1, ((sz >> 12) & 0xFFF) + 1
        print(f'  {name:40s} common {common:#010x} format {d3dfmt:#04x} {w}x{h} data {dataofs:#x}')


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == 'xpr':
        return xpr(a[1])
    b, es = entries(a[1])
    pat = a[2].lower() if len(a) > 2 else ''
    for e in es:
        if pat not in e[0].lower():
            continue
        if a[0] == 'list':
            print(f'{e[1]:10d} {e[2]:10d} {e[0]}')
        elif a[0] == 'extract':
            if e[1] == 0xFFFFFFFF:
                continue
            rel = e[0].replace('\\', '/').split(':', 1)[-1].lstrip('/')
            dst = os.path.join(a[3], rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f:
                f.write(data(b, e))
            print(dst)


if __name__ == '__main__':
    main()
