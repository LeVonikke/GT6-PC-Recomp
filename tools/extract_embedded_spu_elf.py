"""Extract one complete embedded 32-bit big-endian SPU ELF from a PRX."""
from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("prx", type=Path)
    parser.add_argument("offset", type=lambda value: int(value, 0))
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    blob = args.prx.read_bytes()
    header = blob[args.offset:args.offset + 52]
    if len(header) != 52 or header[:4] != b"\x7fELF":
        raise SystemExit("embedded ELF header not found")
    _, _, _, _, phoff, _, _, ehsize, phentsize, phnum, _, _, _ = struct.unpack(
        ">HHIIIIIHHHHHH", header[16:52]
    )
    end = max(ehsize, phoff + phentsize * phnum)
    for index in range(phnum):
        start = args.offset + phoff + index * phentsize
        fields = struct.unpack(">IIIIIIII", blob[start:start + phentsize])
        _, file_offset, _, _, file_size, _, _, _ = fields
        end = max(end, file_offset + file_size)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob[args.offset:args.offset + end])
    print(f"wrote {args.output} ({end:#x} bytes)")


if __name__ == "__main__":
    main()
