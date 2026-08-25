#!/usr/bin/env python3
"""Regression: brsl may use a non-r0 link register (SPURS uses r4/r5/r6)."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap


def w(value):
    return struct.pack(">I", value & 0xFFFFFFFF)


def ri16(op9, i16, rt):
    return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))


def rr(op11, rb, ra, rt):
    return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) |
             ((ra & 0x7F) << 7) | (rt & 0x7F))


def ch(op11, channel, rt):
    return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))


code = b""
code += ri16(0x81, 42, 3)                  # 0x00 il   r3, 42
code += ri16(0x66, (0x10 - 0x04) // 4, 4) # 0x04 brsl r4, 0x10
code += ch(0x10D, 28, 3)                   # 0x08 wrch SPU_WrOutMbox, r3
code += rr(0x000, 0, 0, 0)                 # 0x0C stop
code += ri16(0x81, 100, 5)                 # 0x10 il   r5, 100
code += rr(0x0C0, 5, 3, 3)                 # 0x14 a    r3, r3, r5
code += rr(0x1A8, 0, 4, 0)                 # 0x18 bi   r4

elf = wrap(code, base=0, entry=0,
           symbols=[{"name": "main", "addr": 0x00, "size": 0x08},
                    {"name": "continuation", "addr": 0x08, "size": 0x08},
                    {"name": "add100_r4", "addr": 0x10, "size": 0x0C}])
with open(os.path.join(HERE, "test_brsl_nonzero_return.elf"), "wb") as output:
    output.write(elf)
