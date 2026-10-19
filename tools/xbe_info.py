#!/usr/bin/env python3
"""Read-only XBE inspector: header, certificate, sections, library versions, kernel imports.

Usage: python3 -I xbe_info.py path/to/default.xbe
Opens the file read-only; never writes to it.
"""
import struct
import sys
import datetime

XOR_ENTRY_RETAIL, XOR_ENTRY_DEBUG = 0xA8FC57AB, 0x94859D4B
XOR_THUNK_RETAIL, XOR_THUNK_DEBUG = 0x5B6D40B6, 0xEFB1F152


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def main(path):
    data = open(path, "rb").read()
    if data[:4] != b"XBEH":
        sys.exit("not an XBE")
    base = struct.unpack_from("<I", data, 0x104)[0]

    sections = []

    def off(va):
        # Header region maps 1:1; section contents map through the section table.
        for sva, svs, sra, srs in sections:
            if sva <= va < sva + srs:
                return sra + (va - sva)
        return va - base

    (size_headers, size_image, size_image_header, timestamp, cert_addr,
     n_sections, section_hdr_addr, init_flags, entry_enc, tls_addr,
     stack_commit, heap_reserve, heap_commit, pe_base, pe_size, pe_csum,
     pe_ts, debug_path_addr, debug_file_addr, debug_ufile_addr,
     thunk_enc, nonkern_import_dir, n_libs, lib_addr, kern_lib_addr,
     xapi_lib_addr, logo_addr, logo_size) = struct.unpack_from("<28I", data, 0x108)

    _n, _sh = struct.unpack_from("<II", data, 0x11C)
    for i in range(_n):
        _fl, _va, _vs, _ra, _rs = struct.unpack_from("<5I", data, _sh - base + i * 56)
        sections.append((_va, _vs, _ra, _rs))

    print(f"file          : {path} ({len(data)} bytes)")
    print(f"base          : {base:#010x}")
    print(f"image size    : {size_image:#x}  headers {size_headers:#x}")
    print(f"timestamp     : {datetime.datetime.fromtimestamp(timestamp, datetime.UTC)}")
    print(f"init flags    : {init_flags:#x}")
    print(f"PE base/size  : {pe_base:#x} / {pe_size:#x}  stack commit {stack_commit:#x}")
    print(f"debug path    : {cstr(data[off(debug_path_addr):off(debug_path_addr)+260])}")
    print(f"non-kernel import dir: {nonkern_import_dir:#x}")

    for name, xe, xt in (("retail", XOR_ENTRY_RETAIL, XOR_THUNK_RETAIL),
                         ("debug", XOR_ENTRY_DEBUG, XOR_THUNK_DEBUG)):
        e, t = entry_enc ^ xe, thunk_enc ^ xt
        if base <= e < base + size_image and base <= t < base + size_image:
            print(f"keys          : {name}  entry {e:#010x}  kernel thunk {t:#010x}")
            entry, thunk = e, t
            break
    else:
        sys.exit("could not decode entry point")

    c = off(cert_addr)
    title_id = struct.unpack_from("<I", data, c + 8)[0]
    title = data[c + 12:c + 12 + 80].decode("utf-16-le").split("\0")[0]
    regions, media = struct.unpack_from("<I", data, c + 0xA4)[0], struct.unpack_from("<I", data, c + 0xA0)[0]
    disc, version = struct.unpack_from("<II", data, c + 0xA8)
    print(f"title         : {title!r}  id {title_id:#010x} ({chr(title_id>>24)}{chr((title_id>>16)&0xff)}-{title_id&0xffff:03d})")
    print(f"regions {regions:#x} media {media:#x} disc {disc} version {version:#x}")

    print(f"\nTLS dir       : {tls_addr:#x}")
    if tls_addr:
        t = off(tls_addr)
        s, e, idx, cb, zf, ch = struct.unpack_from("<6I", data, t)
        print(f"  raw {s:#x}-{e:#x} index@{idx:#x} callbacks@{cb:#x} zerofill {zf:#x}")

    print(f"\nsections ({n_sections}):")
    print(f"  {'name':10} {'vaddr':>10} {'vsize':>9} {'raddr':>9} {'rsize':>9} flags")
    sh = off(section_hdr_addr)
    for i in range(n_sections):
        fl, va, vs, ra, rs, name_addr = struct.unpack_from("<6I", data, sh + i * 56)
        f = []
        if fl & 1: f.append("W")
        if fl & 2: f.append("PRELOAD")
        if fl & 4: f.append("X")
        if fl & 8: f.append("INSERTED")
        if fl & 0x10: f.append("HEAD_RO")
        if fl & 0x20: f.append("TAIL_RO")
        print(f"  {cstr(data[off(name_addr):off(name_addr)+16]):10} {va:#010x} {vs:#9x} {ra:#9x} {rs:#9x} {','.join(f)}")

    print(f"\nlibrary versions ({n_libs}):")
    lv = off(lib_addr)
    for i in range(n_libs):
        name = cstr(data[lv + i * 16: lv + i * 16 + 8])
        maj, mn, build, flags = struct.unpack_from("<HHHH", data, lv + i * 16 + 8)
        qfe, approved, dbg = flags & 0x1FFF, (flags >> 13) & 3, flags >> 15
        print(f"  {name:8} {maj}.{mn}.{build}.{qfe}  approved={approved} debug={dbg}")

    print("\nkernel imports:")
    th = off(thunk)
    ords = []
    while True:
        v = struct.unpack_from("<I", data, th + len(ords) * 4)[0]
        if v == 0:
            break
        ords.append(v & 0x7FFFFFFF)
    print(f"  count {len(ords)}")
    print("  " + " ".join(str(o) for o in ords))


if __name__ == "__main__":
    main(sys.argv[1])
