#!/usr/bin/env python3
"""
fix-arm9-capture.py - wire the ARM9 black-box capture into the firmware

WHY THIS EXISTS
---------------
The 15 s tmpfs->SD flusher (and every Linux SD writer) wedges on the
ARM11/PXI/MMC write path under framework load.  Once it stalls, all card logs
stop at once and everything after that is trapped in tmpfs and lost when the
console is powered off - so we cannot tell whether Android ever reaches the
launcher or composites a frame.

The ARM9 is an independent CPU and owns the SD controller.  This patch makes it
poll a 4 KiB page in ARM11 DRAM (whose physical address it is told once, via a
PXI manager register write) and mirror that page to a rotating set of sectors in
a preallocated card file - entirely bypassing the Linux block layer.  See
port/arm9linuxfw/capture.{c,h} and ctr_lcd_fb.c.

The patch:
  * source/virt/manager.c: add the capture include and route the capture-page
    address manager register to a9cap_set_addr()
  * source/main.c: include capture.h, enable the timer0 IRQ (the wakeup source
    for the main loop), call a9cap_init() and a9cap_poll()

Idempotent: running it twice changes nothing.  Both files use CRLF line endings.
"""

import sys
import pathlib

MARK = "a9cap_"

# ---------------------------------------------------------------------------
# manager.c
# ---------------------------------------------------------------------------
MGR_INC_ANCHOR = '#include "virt/manager.h"\r\n'
MGR_INC_NEW = ('#include "virt/manager.h"\r\n'
               '\r\n'
               '#include "capture.h"\r\n')

MGR_OLD = (
    "static void vman_internal_reg_write(uint reg, u32 val) {\r\n"
    "\t// ignore all register writes for now\r\n"
    "}\r\n"
)
MGR_NEW = (
    "static void vman_internal_reg_write(uint reg, u32 val) {\r\n"
    "\t/*\r\n"
    "\t * ARM9 black-box capture: the ARM11 hands us the physical address of\r\n"
    "\t * its capture page once, then polls no PXI at all (see capture.c).\r\n"
    "\t */\r\n"
    "\tif (reg == ARM9CAP_MGR_REG) {\r\n"
    "\t\ta9cap_set_addr(val);\r\n"
    "\t\treturn;\r\n"
    "\t}\r\n"
    "\t// ignore all other register writes for now\r\n"
    "}\r\n"
)

# ---------------------------------------------------------------------------
# main.c
# ---------------------------------------------------------------------------
MAIN_INC_ANCHOR = '#include "virt/manager.h"\r\n'
MAIN_INC_NEW = ('#include "virt/manager.h"\r\n'
                '\r\n'
                '#include "capture.h"\r\n')

MAIN_TIMER_OLD = "\ttimer_reset(false);\r\n"
MAIN_TIMER_NEW = ("\t/* timer0 IRQ wakes the main loop so a9cap_poll() runs even\r\n"
                  "\t * when no PXI transaction arrives (see fix-arm9-capture.py). */\r\n"
                  "\ttimer_reset(true);\r\n")

MAIN_INIT_OLD = ("\tvman_init_all();\r\n"
                 "\r\n"
                 "\t// Enable interrupts, wait for new commands and process buffers\r\n")
MAIN_INIT_NEW = ("\tvman_init_all();\r\n"
                 "\r\n"
                 "\ta9cap_init();\r\n"
                 "\r\n"
                 "\t// Enable interrupts, wait for new commands and process buffers\r\n")

MAIN_LOOP_OLD = ("\twhile(1) {\r\n"
                 "\t\tneed_sleep();\r\n"
                 "\r\n"
                 "\t\tif (!vman_process_pending())\r\n"
                 "\t\t\tarm_wait_for_interrupt();\r\n"
                 "\t}\r\n")
MAIN_LOOP_NEW = ("\twhile(1) {\r\n"
                 "\t\tneed_sleep();\r\n"
                 "\r\n"
                 "\t\ta9cap_poll();\r\n"
                 "\r\n"
                 "\t\tif (!vman_process_pending())\r\n"
                 "\t\t\tarm_wait_for_interrupt();\r\n"
                 "\t}\r\n")


def patch(path, edits, mark=MARK):
    with path.open("r", newline="") as f:
        s = f.read()
    if mark in s:
        return False
    for old, new, what in edits:
        if old not in s:
            print(f"error: {path.name}: {what} not found (unexpected version)",
                  file=sys.stderr)
            raise SystemExit(1)
        s = s.replace(old, new, 1)
    path.write_text(s, newline="")
    return True


def main():
    if len(sys.argv) < 2:
        print("usage: fix-arm9-capture.py <arm9linuxfw-tree>", file=sys.stderr)
        return 2
    fw = pathlib.Path(sys.argv[1])

    mgr = fw / "source/virt/manager.c"
    main_c = fw / "source/main.c"
    for p in (mgr, main_c):
        if not p.exists():
            print(f"error: {p} not found", file=sys.stderr)
            return 1

    if patch(mgr, [(MGR_INC_ANCHOR, MGR_INC_NEW, "manager.c include"),
                   (MGR_OLD, MGR_NEW, "vman_internal_reg_write()")]):
        print("    manager.c: capture-page manager register routed to ARM9")
    else:
        print("    manager.c: capture already wired")

    if patch(main_c, [(MAIN_INC_ANCHOR, MAIN_INC_NEW, "main.c include"),
                      (MAIN_TIMER_OLD, MAIN_TIMER_NEW, "timer_reset"),
                      (MAIN_INIT_OLD, MAIN_INIT_NEW, "a9cap_init call"),
                      (MAIN_LOOP_OLD, MAIN_LOOP_NEW, "a9cap_poll call")]):
        print("    main.c: timer IRQ + a9cap_init()/a9cap_poll() wired in")
    else:
        print("    main.c: capture already wired")

    return 0


if __name__ == "__main__":
    sys.exit(main())
