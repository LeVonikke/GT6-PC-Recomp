#!/usr/bin/env python3
"""Branch-dispatch emission checks for the PPU lifter.

Complements test_ppu_lift.py: that suite compiles emitted statements and runs
them against register inputs, which can't cover branch dispatch (it needs the
runtime's ps3_indirect_call + trampoline). These assert the emission SHAPE --
call vs return -- which is where the class of bug below lives.

The bug this pins: blrl (branch to LR *with link*) is an indirect CALL, the
LR-based twin of `mtctr;bctrl`, emitted for function-descriptor calls as
`lwz r12,0(rN); mtlr r12; lwz r2,4(rN); blrl`. It was lumped with blr (return)
and emitted a bare `return;`, which dropped the call AND skipped the frame
epilogue -> r1 leaked the frame size on every such call.

Usage:  py -3 tools\\test_ppu_branches.py
"""

import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from ppu_disasm import Instruction                  # noqa: E402
from ppu_lifter import (PPULifter, LiftedFunction, discover_jump_tables,
                        _last_line_is_terminator)  # noqa: E402


def emit(mnemonic: str, operands: str = "") -> str:
    lifter = PPULifter(prefix="")
    func = LiftedFunction(name="f", start_addr=0x1000, end_addr=0x2000)
    return lifter._translate_op(Instruction(0x1000, 0, mnemonic, operands), func)


def main() -> int:
    blr, blrl = emit("blr"), emit("blrl")
    bctrl, bnelrl = emit("bctrl"), emit("bnelrl", "cr7")

    # blr is the only real return of the four.
    assert blr == "return;", blr

    # A configured guest context-restore routine has longjmp semantics: its blr
    # must escape the stale native call chain instead of returning to its caller.
    nl = PPULifter(prefix="")
    nl.nonlocal_restore_addrs.add(0x1000)
    nlf = LiftedFunction(name="f", start_addr=0x1000, end_addr=0x2000)
    nl_blr = nl._translate_op(Instruction(0x1010, 0, "blr", ""), nlf)
    assert nl_blr == "ppu_nonlocal_jump(ctx); return;", nl_blr
    assert _last_line_is_terminator([nl_blr]), nl_blr

    owner = LiftedFunction(name="func_00001000", start_addr=0x1000,
                           end_addr=0x1100, body_lines=["    return;"],
                           nonlocal_continuations=[0x1044])
    emitted = "\n".join(nl._function_def_lines(
        owner, {0x1000: owner}, [0x1000], {0x1000: 0}))
    assert "try {" in emitted and "catch (...)" in emitted, emitted
    assert "case 0x00001044u: func_00001044(ctx);" in emitted, emitted
    assert "ppu_nonlocal_pending(ctx)" in emitted, emitted

    # blrl: dispatch through LR, then CONTINUE (link = call, so no return).
    assert "ps3_indirect_call" in blrl, blrl
    assert "ctx->lr" in blrl, blrl
    assert "return;" not in blrl, f"blrl must not return -- link = call: {blrl}"

    # b<cond>lrl: the conditional LR twin of b<cond>ctrl.
    assert "ps3_indirect_call" in bnelrl, bnelrl
    assert "ctx->lr" in bnelrl, bnelrl
    assert bnelrl.startswith("if ("), bnelrl

    # The CTR-based call it mirrors, unchanged.
    assert bctrl == "ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);", bctrl

    # GCC's in-text switch idiom keeps the table base outside the CTR source:
    # lis/addi r9; lwzx r0,...,r9; add r0,r0,r9; mtctr r0; bctr.  The cases
    # begin immediately after the dispatcher and must be emitted as local labels.
    inline = [
        Instruction(0x1000, 0, "lis", "r0, 0x0"),
        Instruction(0x1004, 0, "addi", "r0, r0, 0x1020"),
        Instruction(0x1008, 0, "mtctr", "r0"),
        Instruction(0x100C, 0, "bctr", ""),
    ]
    cases = PPULifter._inline_bctr_cases(inline, 3)
    assert cases == list(range(0x1010, 0x1024, 4)), cases

    # The common data-table form must resolve an offset table based in r9,
    # even though the final CTR value is assembled in r0.
    switch = [
        Instruction(0x1100, 0, "cmplwi", "cr7, r22, 0x1"),
        Instruction(0x1104, 0, "lis", "r9, 0x0"),
        Instruction(0x1108, 0, "addi", "r9, r9, 0x1200"),
        Instruction(0x110C, 0, "lwzx", "r0, r11, r9"),
        Instruction(0x1110, 0, "extsw", "r0, r0"),
        Instruction(0x1114, 0, "add", "r0, r0, r9"),
        Instruction(0x1118, 0, "mtctr", "r0"),
        Instruction(0x111C, 0, "bctr", ""),
    ]
    words = {0x1200: 0x100, 0x1204: 0x140}
    tables = discover_jump_tables(switch, words.get, 0, 0x1000, 0x2000)
    assert tables[0x111C] == [0x1300, 0x1340], tables

    print("ok: branch dispatch and inline bctr table recovery")
    return 0


if __name__ == "__main__":
    sys.exit(main())
