#!/usr/bin/env python3
"""
fix-cpu-arch.py - report the 3DS ARM11 as ARMv6, not the ARMv7 the generic
detector infers from the "revised CPUID" / MMFR0 heuristic.

The 3DS CPU ID is 0x410fb025: implementer 0x41 (ARM), architecture field 0xf,
part 0xb02 (ARM11 MPCore), revision 5.  The architecture nibble makes the
kernel take the "revised CPUID format" branch in __get_cpu_architecture()
and read MMFR0, which on this part advertises VMSAv7/PMSAv7 - so the kernel
concludes CPU_ARCH_ARMv7 even though the core is only ARMv6K.

That single wrong answer changes, among others:

  * /proc/cpuinfo ("CPU architecture: 7") and the ELF HWCAP exposed to
    userspace,
  * arch/arm/mm/fault.c exception hooks (ARMv7 access-flag faults),
  * arch/arm/mm/alignment.c (Thumb-2 32-bit instruction decoding),
  * arch/arm/kernel/swp_emulate.c (whether $swp is emulated).

The 3DS is ARMv6K, so force the answer.  Idempotent.
"""

import sys
import pathlib


MARK = "3DS ARM11 is really ARMv6K"


def main():
    if len(sys.argv) < 2:
        print("usage: fix-cpu-arch.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "arch/arm/kernel/setup.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    s = p.read_text()
    if MARK in s:
        print("    setup.c: ARMv6 architecture override already present")
        return 0

    anchor = "static int __get_cpu_architecture(void)\n{\n\tint cpu_arch;\n"
    if anchor not in s:
        print("error: setup.c: __get_cpu_architecture anchor not found",
              file=sys.stderr)
        return 1

    override = anchor + (
        "\n#ifdef CONFIG_ARCH_CTR\n"
        "\t/*\n"
        "\t * The 3DS ARM11 MPCore reports a \"revised\" CPUID whose ARMv7\n"
        "\t * architecture bit makes the generic detector read MMFR0 and\n"
        "\t * wrongly conclude ARMv7.  The 3DS ARM11 is really ARMv6K.\n"
        "\t */\n"
        "\treturn CPU_ARCH_ARMv6;\n"
        "#endif\n")

    s = s.replace(anchor, override, 1)
    p.write_text(s)
    assert MARK in s or "return CPU_ARCH_ARMv6" in s
    print("    setup.c: forced CPU_ARCH_ARMv6 for CONFIG_ARCH_CTR")
    return 0


if __name__ == "__main__":
    sys.exit(main())
