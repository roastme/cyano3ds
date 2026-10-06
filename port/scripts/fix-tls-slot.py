#!/usr/bin/env python3
"""Make the kernel maintain the kuser "software TLS" slot (ARMv6K path).

Android's bionic (API 10) reads its thread pointer from 0xffff0ff0 (the kuser helper
page's software TLS slot), not from the hardware TLS register.  With
CONFIG_CPU_32v6K the kernel selects switch_tls_v6k and set_tls()'s has_tls_reg
path, neither of which touches that slot - so it keeps stale contents and every
static Android binary faults on its first TLS access (the tracer caught init doing
a store through address 0x8 one instruction after __ARM_NR_set_tls).

Both insertions are anchored on unambiguous text and are idempotent.
"""
import pathlib
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/root/p3ds/src/linux-3ds")
p = kd / "arch/arm/include/asm/tls.h"
s = p.read_text()

if "bionic reads its thread pointer" in s:
    print("    tls.h: already patched")
    raise SystemExit(0)

# ---------------------------------------------------------------- 1. the switch
i = s.index(".macro switch_tls_v6k")
j = s.index(".endm", i)
s = (s[:j]
     + "#ifdef CONFIG_KUSER_HELPERS\n"
     + "\t/* Android's bionic reads its thread pointer from the kuser\n"
     + "\t * \"software TLS\" slot, so mirror the register into it as well;\n"
     + "\t * without this every static Android binary faults on its first TLS\n"
     + "\t * access (init died storing through address 0x8). */\n"
     + "\tmov\t\\tmp2, #0xffff0fff\n"
     + "\tstr\t\\tp, [\\tmp2, #-15]\n"
     + "#endif\n"
     + s[j:])

# ---------------------------------------------------------------- 2. set_tls()
anchor = ': : "r" (val));\n'
i = s.index(anchor) + len(anchor)
s = (s[:i]
     + "#ifdef CONFIG_KUSER_HELPERS\n"
     + "\t\t\t/* Android's bionic reads its thread pointer from the kuser\n"
     + "\t\t\t * \"software TLS\" slot at 0xffff0ff0; keep it in sync with the\n"
     + "\t\t\t * hardware register (which modern userspace uses). */\n"
     + "\t\t\t*((unsigned int *)0xffff0ff0) = val;\n"
     + "#endif\n"
     + s[i:])

p.write_text(s)
print("    tls.h: kuser software-TLS slot maintained alongside the register")
