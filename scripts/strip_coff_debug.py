#!/usr/bin/env python3
"""Remove COFF debug sections from PE files (in place).

The UB-Mannheim tesseract build ships libtesseract-5.dll at ~100 MB, of which
~97 MB is the COFF symbol table and long-name string table stored in `/nn`
sections and the trailing symbol records. The Windows loader never maps those,
so dropping them is safe for execution. A plain `strip` is unavailable on the
GitHub Windows runners, hence this ~90 line replacement.

Usage: python strip_coff_debug.py file1 [file2 ...]
Prints "<file>: 101.5 MB -> 4.3 MB" per file; files without debug data are
left untouched.
"""
import os
import struct
import sys


def strip_pe(path):
    with open(path, "rb") as fh:
        data = fh.read()

    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe : pe + 4] != b"PE\x00\x00":
        return None

    machine, nsec = struct.unpack_from("<HH", data, pe + 4)
    sym_ptr, nsyms = struct.unpack_from("<II", data, pe + 12)
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    if opt_size & 0xF:
        return None  # alignment looks wrong; bail out before touching anything
    sec_off = pe + 24 + opt_size
    if sec_off + nsec * 40 > len(data):
        return None

    debug_raw = 0
    keep_end = 0
    sections = []
    for i in range(nsec):
        off = sec_off + i * 40
        name = data[off : off + 8]
        raw_size, raw_ptr = struct.unpack_from("<II", data, off + 16)
        sections.append((off, name, raw_size, raw_ptr))
        if name.startswith(b"/"):
            debug_raw += raw_size
        elif raw_ptr and raw_size:
            keep_end = max(keep_end, raw_ptr + raw_size)

    if debug_raw == 0 and sym_ptr == 0:
        return None  # nothing to do

    out = bytearray(data)
    # 1. unlink the COFF symbol table from the header
    struct.pack_into("<II", out, pe + 12, 0, 0)
    # 2. zero the raw extent of every /nn debug section
    for off, name, raw_size, raw_ptr in sections:
        if not name.startswith(b"/"):
            continue
        struct.pack_into("<I", out, off + 16, 0)   # SizeOfRawData
        struct.pack_into("<I", out, off + 20, 0)   # PointerToRawData
    # 3. drop the trailing bytes the symbol table occupied
    if keep_end == 0:
        keep_end = len(out)
    keep_end = min(keep_end, len(out))
    del out[keep_end:]

    tmp = path + ".stripped"
    with open(tmp, "wb") as fh:
        fh.write(out)
    os.replace(tmp, path)
    return (len(data), len(out))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    changed = 0
    for path in sys.argv[1:]:
        if not os.path.isfile(path):
            print("skip (missing): %s" % path, file=sys.stderr)
            continue
        result = strip_pe(path)
        if result is None:
            continue
        before, after = result
        changed += 1
        print("%s: %.1f MB -> %.1f MB" % (path, before / 1e6, after / 1e6), flush=True)
    print("stripped %d file(s)" % changed, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
