#!/usr/bin/env python3
"""trace.c: glibc's armel <sys/user.h> does not define every ARM_* macro, so
use the register array directly (uregs[11]=fp, 13=sp, 14=lr, 15=pc)."""
import pathlib

p = pathlib.Path(__file__).resolve().parents[1] / "userland/tests/trace.c"
s = p.read_text()
before = s
s = s.replace("(unsigned long)regs.ARM_pc", "(unsigned long)regs.uregs[15]")
s = s.replace("(unsigned long)regs.ARM_lr", "(unsigned long)regs.uregs[14]")
s = s.replace("(unsigned long)regs.ARM_sp", "(unsigned long)regs.uregs[13]")
s = s.replace("(unsigned long)regs.ARM_fp", "(unsigned long)regs.uregs[11]")
assert s != before, "nothing replaced"
assert "ARM_pc" not in s and "ARM_sp" not in s and "ARM_fp" not in s
p.write_bytes(s.encode())
print("trace.c fixed: uregs[] used for pc/lr/sp/fp")
