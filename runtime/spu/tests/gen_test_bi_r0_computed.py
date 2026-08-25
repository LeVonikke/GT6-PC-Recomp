#!/usr/bin/env python3
"""Regression: `bi r0` without a host-call marker is a computed jump."""
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
code += ri16(0x81, 0x10, 0)              # 0x00 il   r0, 0x10
code += rr(0x1A8, 0, 0, 0)               # 0x04 bi   r0 (computed jump)
code += rr(0x000, 0, 0, 0)               # 0x08 stop (unreachable padding)
code += rr(0x000, 0, 0, 0)               # 0x0C stop (unreachable padding)
code += ri16(0x81, 77, 3)                 # 0x10 il   r3, 77
code += ch(0x10D, 28, 3)                  # 0x14 wrch SPU_WrOutMbox, r3
code += rr(0x000, 0, 0, 0)               # 0x18 stop

elf = wrap(code, base=0, entry=0,
           symbols=[{"name": "main", "addr": 0x00, "size": 0x08},
                    {"name": "target", "addr": 0x10, "size": 0x0C}])
with open(os.path.join(HERE, "test_bi_r0_computed.elf"), "wb") as output:
    output.write(elf)

