#!/usr/bin/env python3
"""Make the high-vectors page kernel-writable on the Nintendo 3DS.

arch/arm/mm/mmu.c only adds L_PTE_MT_VECTORS - the "user r/o, kernel r/w"
memory type used for the vectors page on ARMv6, where translation domains are
not used - when it has autodetected an ARMv6 CPU:

        if (cpu_arch == CPU_ARCH_ARMv6)
                vecs_pgprot |= L_PTE_MT_VECTORS;

The 3DS ARM11 MPCore reports a *revised* CPUID (MIDR 0x410fb025, arch field
0xf), so __get_cpu_architecture() falls back to reading ID_MMFR0 and classifies
the core as ARMv7 - the boot log literally prints "(ARMv7)".  The vectors page
is then mapped privileged-read-only, and the kernel's store to the kuser
"software TLS" slot at 0xffff0ff0 (which Android's bionic reads) takes a
data abort on the very first context switch: no console output, no log, just a
hang right after the loader hands over.

The core really is ARMv6K and does not use domains, so the ARMv6 mapping is the
correct one.  Force it for ARCH_CTR.

Idempotent.
"""
import pathlib
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/root/p3ds/src/linux-3ds")
p = kd / "arch/arm/mm/mmu.c"
s = p.read_text()

if "3DS reports a revised CPUID" in s:
    print("    mmu.c: high-vectors page already forced kernel-r/w")
    raise SystemExit(0)

old = "\tif (cpu_arch == CPU_ARCH_ARMv6)\n\t\tvecs_pgprot |= L_PTE_MT_VECTORS;\n"
if old not in s:
    raise SystemExit("error: mmu.c: vectors pgprot condition not found")

new = (
    "\t/*\n"
    "\t * The 3DS reports a revised CPUID and is autodetected as ARMv7,\n"
    "\t * so also include ARCH_CTR: the ARM11 is really ARMv6K, does not\n"
    "\t * use domains, and needs this user r/o + kernel r/w memory type\n"
    "\t * (the kernel writes the kuser software TLS slot there).\n"
    "\t */\n"
    "\tif (cpu_arch == CPU_ARCH_ARMv6 || IS_ENABLED(CONFIG_ARCH_CTR))\n"
    "\t\tvecs_pgprot |= L_PTE_MT_VECTORS;\n"
)
p.write_text(s.replace(old, new, 1))
print("    mmu.c: high-vectors page forced kernel-r/w (user r/o)")
