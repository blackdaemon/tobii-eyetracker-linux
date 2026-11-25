#!/usr/bin/env python3
"""Clear the executable-stack flag (PT_GNU_STACK PF_X) of a 64-bit ELF file in place.

libtobii_stream_engine.so was built with an RWE GNU_STACK header. glibc >= 2.41
refuses to dlopen() such libraries into a running process ("cannot enable
executable stack"), which breaks Python/ctypes, Rust dlopen-based crates, etc.
The library does not need an executable stack; this is equivalent to
`patchelf --clear-execstack` / `execstack -c`.

    python3 -I clear-execstack.py FILE...
"""
import struct
import sys

PT_GNU_STACK = 0x6474E551
PF_X = 0x1


def clear(path):
    with open(path, "r+b") as f:
        hdr = f.read(64)
        if hdr[:4] != b"\x7fELF" or hdr[4] != 2 or hdr[5] != 1:
            raise SystemExit(f"{path}: not a little-endian ELF64 file")
        phoff, = struct.unpack_from("<Q", hdr, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", hdr, 0x36)
        for i in range(phnum):
            off = phoff + i * phentsize
            f.seek(off)
            p_type, p_flags = struct.unpack("<II", f.read(8))
            if p_type == PT_GNU_STACK:
                if p_flags & PF_X:
                    f.seek(off + 4)
                    f.write(struct.pack("<I", p_flags & ~PF_X))
                    print(f"{path}: cleared executable stack flag")
                else:
                    print(f"{path}: stack already non-executable")
                return
        print(f"{path}: no GNU_STACK header (nothing to do)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for p in sys.argv[1:]:
        clear(p)
