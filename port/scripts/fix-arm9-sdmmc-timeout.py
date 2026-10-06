#!/usr/bin/env python3
"""
fix-arm9-sdmmc-timeout.py - bound the ARM9 SD/MMC busy-waits

WHY THIS EXISTS
---------------
`arm9linuxfw` runs the SD card on the ARM9.  Linux reaches it through the PXI
FIFO: the ARM11 writes a request into the FIFO and the ARM9 services it in its
main loop.  If the ARM9's SD/MMC code spins forever, it stops draining the PXI
FIFO, and then *every* block request on the ARM11 (the loop-mounted /system
image, the SD logs) stalls - which, with the pxi transport's IRQ-off FIFO poll,
freezes the whole machine.

`source/hw/sdmmc.c` had two unbounded waits:

    while((sdmmc_read16(REG_SDSTATUS1) & TMIO_STAT1_CMD_BUSY));   // no bound
    ...
    while(1)                                                      // no bound
    {
        ... break on TMIO_MASK_GW (error) or on (status0 & flags) == flags ...
    }

If the card stops responding in a way that sets neither the error bit nor the
completion bit, the ARM9 spins forever.

This patch bounds both.  A stuck card now yields an I/O error (ctx->error |= 4)
instead of hanging the ARM9 (and therefore the ARM11).

NOTE on the counter idiom: `while (cond && spin--)` / `while (timeout--)` are
wrong here - when the counter reaches 0 the post-decrement wraps it to
0xffffffff, so the `if (!spin)` check after the loop never fires.  The tests
are therefore written as `while (cond) { if (!spin--) { error; break; } ... }`.

Idempotent: running it twice changes nothing.
"""

import re
import sys
import pathlib

MARK = "SDMMC_CMD_TIMEOUT"

# The file uses CRLF line endings; read/write with newline='' so the patterns
# (which contain \r\n) match and the file keeps its original line endings.

OLD_ANCHOR = "static void sdmmc_send_command(struct mmcdevice *ctx, u32 cmd, u32 args)\r\n{\r\n"

NEW_ANCHOR = (
    "/*\r\n"
    " * Upper bound (loop iterations) for the SD/MMC busy-waits.  The ARM9 runs\r\n"
    " * at ~134 MHz and a 512-byte transfer takes ~150 us at 33 MHz, so this is\r\n"
    " * enormously generous - it only has to turn a hung card into an I/O error\r\n"
    " * instead of a dead ARM9 (and a frozen ARM11).\r\n"
    " */\r\n"
    "#define SDMMC_CMD_TIMEOUT\t0x200000u\r\n"
    "\r\n"
) + OLD_ANCHOR

OLD_BUSY = (
    "\tctx->error = 0;\r\n"
    "\twhile((sdmmc_read16(REG_SDSTATUS1) & TMIO_STAT1_CMD_BUSY)); //mmc working?\r\n"
)

NEW_BUSY = (
    "\tctx->error = 0;\r\n"
    "\t/* Bounded: an unbounded wait here hangs the ARM9, which stops\r\n"
    "\t * draining the PXI FIFO and therefore stalls every Linux block\r\n"
    "\t * request (see fix-arm9-sdmmc-timeout.py). */\r\n"
    "\t{\r\n"
    "\t\tu32 spin = SDMMC_CMD_TIMEOUT;\r\n"
    "\t\twhile((sdmmc_read16(REG_SDSTATUS1) & TMIO_STAT1_CMD_BUSY)) {\r\n"
    "\t\t\tif(!spin--) { ctx->error |= 4; return; }\r\n"
    "\t\t}\r\n"
    "\t}\r\n"
)

OLD_LOOP_HEAD = "\tu16 status0 = 0;\r\n\twhile(1)\r\n\t{\r\n"
NEW_LOOP_HEAD = (
    "\tu16 status0 = 0;\r\n"
    "\tu32 timeout = SDMMC_CMD_TIMEOUT;\r\n"
    "\twhile(1)\r\n"
    "\t{\r\n"
    "\t\tif(!timeout--) { ctx->error |= 4; break; }\r\n"
)

OLD_LOOP_END = "\t}\r\n\tctx->stat0 = sdmmc_read16(REG_SDSTATUS0);\r\n"
NEW_LOOP_END = "\t}\r\n\tctx->stat0 = sdmmc_read16(REG_SDSTATUS0);\r\n"


def main():
    if len(sys.argv) < 2:
        print("usage: fix-arm9-sdmmc-timeout.py <arm9linuxfw-tree>", file=sys.stderr)
        return 2
    fw = pathlib.Path(sys.argv[1])
    p = fw / "source/hw/sdmmc.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        # A previous (buggy) version of this patch may be present: verify the
        # counter idiom and repair it if needed.
        if "while(timeout--)" in s or "spin--);" in s:
            print("error: sdmmc.c already patched with the broken counter idiom; "
                  "restore the file (git checkout source/hw/sdmmc.c) and re-run",
                  file=sys.stderr)
            return 1
        print("    sdmmc.c: busy-waits already bounded")
        return 0

    if OLD_ANCHOR not in s:
        print("error: sdmmc.c: sdmmc_send_command() not found", file=sys.stderr)
        return 1
    s = s.replace(OLD_ANCHOR, NEW_ANCHOR, 1)

    for old, new, what in ((OLD_BUSY, NEW_BUSY, "CMD_BUSY wait"),
                           (OLD_LOOP_HEAD, NEW_LOOP_HEAD, "transfer loop head")):
        if old not in s:
            print(f"error: sdmmc.c: {what} not found", file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    with p.open("w", newline="") as f:
        f.write(s)

    body = s.split("sdmmc_send_command")[1].split("sdmmc_sdcard_writesectors")[0]
    assert "while(1)" in body, "sdmmc.c: transfer loop not found"
    assert "if(!timeout--)" in body, "sdmmc.c: timeout check missing"
    assert "if(!spin--)" in body, "sdmmc.c: CMD_BUSY timeout check missing"

    print("    sdmmc.c: CMD_BUSY wait and transfer loop are now bounded "
          "(SDMMC_CMD_TIMEOUT = 0x200000)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
