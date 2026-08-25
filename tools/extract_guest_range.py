#!/usr/bin/env python3
"""Extract a byte range addressed in a loaded PPU ELF PT_LOAD segment.

This is deliberately small and deterministic: it is useful for PS3 modules
that ship raw SPU policy code rather than a standalone embedded SPU ELF.
"""
import argparse
from pathlib import Path
import struct


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("address", type=lambda s: int(s, 0))
    ap.add_argument("length", type=lambda s: int(s, 0))
    ap.add_argument("output")
    a = ap.parse_args()
    blob = Path(a.elf).read_bytes()
    hdr = struct.unpack_from(">16sHHIQQQIHHHHHH", blob, 0)
    phoff, phentsz, phnum = hdr[5], hdr[9], hdr[10]
    for i in range(phnum):
        typ, _flags, offset, vaddr, _paddr, filesz, _memsz, _align = \
            struct.unpack_from(">IIQQQQQQ", blob, phoff + i * phentsz)
        if typ == 1 and vaddr <= a.address and a.address + a.length <= vaddr + filesz:
            start = offset + a.address - vaddr
            Path(a.output).parent.mkdir(parents=True, exist_ok=True)
            Path(a.output).write_bytes(blob[start:start + a.length])
            print(f"extracted guest 0x{a.address:X}+0x{a.length:X} "
                  f"from PT_LOAD[{i}] file 0x{start:X} -> {a.output}")
            return
    raise SystemExit("requested range is not fully backed by a PT_LOAD file segment")


if __name__ == "__main__":
    main()
