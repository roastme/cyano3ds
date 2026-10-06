#!/usr/bin/env python3
"""
patch-firm-loader-clock.py - restore the final 804 MHz upclock in
firm_linux_loader.

`online_cores23()` downclocks to 268 MHz to bring up cores 2/3 and then has the
upclock back to the fast mode commented out:

    downclock();
    setup_overlays();
    // upclock();
    gic_send_swi(2, 2);
    gic_send_swi(3, 3);

Upstream (firm_linux_loader PR #25) left it commented because the Linux side
was not aware of the faster clock (the ARM11 MPCore TWD timer is clocked from
CPU/2, so time would run 3x fast).  This port now tells the kernel the timer
rate through the device tree (cpuclk / twdclk), so the bootloader can leave the
SoC at 804 MHz.

Why here and not a runtime cpufreq switch: by the time Linux runs, the
bootloader has parked cores 1-3 in a `wfe` loop (arm11/source/start.S), and the
SoC mode switch requires every powered-on core to be in `wfi`.  At this point in
the bootloader cores 2/3 are still in the `core23_entry` wfi loop and core 1 is
still in the bootrom, so the switch works.

Idempotent.
"""

import pathlib
import sys

MARK = "upclock(); /* Cyano3DS port: leave the SoC at the fast clock */"
OLD_MARK = "upclock(); /* Android 1.6 port: leave the SoC at the fast clock */"


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: patch-firm-loader-clock.py <firm_linux_loader-dir>",
              file=sys.stderr)
        return 2
    p = pathlib.Path(sys.argv[1]) / "arm11/source/smp.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    s = p.read_text()
    if MARK in s:
        print("    smp.c: final upclock already restored")
        return 0
    if OLD_MARK in s:
        print("    smp.c: final upclock already restored (legacy mark)")
        return 0

    old = "\tdownclock();\n\tsetup_overlays();\n\t// upclock();\n"
    if old not in s:
        print("error: smp.c: commented-upclock block not found", file=sys.stderr)
        return 1

    new = ("\tdownclock();\n\tsetup_overlays();\n"
           "\tupclock(); /* Cyano3DS port: leave the SoC at the fast clock */\n")
    p.write_text(s.replace(old, new, 1))
    print("    smp.c: final upclock restored (New 3DS stays at 804 MHz)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
