#!/usr/bin/env python3
"""
fix-arm9-pxi-timeout.py - bound the ARM9 PXI interrupt handler's FIFO waits

WHY THIS EXISTS
---------------
`arm9linuxfw`'s `on_pxi_recv()` (source/main.c) runs as the ARM9's PXI RX
interrupt handler and services one FIFO word at a time.  Two of its waits are
unbounded:

    case 0: // READ REGISTER
        data = vman_reg_read(dev, reg);
        while(pxi_is_tx_full());     // <-- unbounded, IRQs off
        pxi_send(data);
        break;

    case 1: // WRITE REGISTER
        while(pxi_is_rx_empty());    // <-- unbounded, IRQs off
        data = pxi_recv();

If the ARM11 stops draining the TX FIFO (or never supplies the data word of a
write), the ARM9 spins forever inside its own interrupt handler with interrupts
disabled.  It then stops draining the PXI FIFO *and* never returns to its main
loop, so the SD/MMC work (and the whole virtio bridge) stops - which on the
ARM11 looks like the block layer hanging and the system freezing.  This is the
same class of bug the SD/MMC busy-waits had (fix-arm9-sdmmc-timeout.py); these
two were simply missed.

This patch bounds both waits.  On timeout the handler abandons the transaction;
the ARM11 side turns its own PXI timeout into an I/O error and flushes the FIFO
so the two ends resynchronise.

Idempotent: running it twice changes nothing.  main.c uses CRLF line endings.
"""

import sys
import pathlib

MARK = "PXI_WAIT_TIMEOUT"

OLD_READ = (
    "\t\tcase 0: // READ REGISTER\r\n"
    "\t\t\tdata = vman_reg_read(dev, reg);\r\n"
    "\t\t\twhile(pxi_is_tx_full());\r\n"
    "\t\t\tpxi_send(data);\r\n"
    "\t\t\tbreak;\r\n"
)
NEW_READ = (
    "\t\tcase 0: // READ REGISTER\r\n"
    "\t\t\tdata = vman_reg_read(dev, reg);\r\n"
    "\t\t\t/* Bounded: an unbounded wait here spins the ARM9 forever in\r\n"
    "\t\t\t * its own IRQ handler with interrupts disabled, which stops\r\n"
    "\t\t\t * it draining the PXI FIFO and freezes the whole bridge (see\r\n"
    "\t\t\t * fix-arm9-pxi-timeout.py). */\r\n"
    "\t\t\t{\r\n"
    "\t\t\t\tu32 spin = PXI_WAIT_TIMEOUT;\r\n"
    "\t\t\t\twhile(pxi_is_tx_full()) {\r\n"
    "\t\t\t\t\tif(!spin--) return;\r\n"
    "\t\t\t\t}\r\n"
    "\t\t\t}\r\n"
    "\t\t\tpxi_send(data);\r\n"
    "\t\t\tbreak;\r\n"
)

OLD_WRITE = (
    "\t\tcase 1: // WRITE REGISTER\r\n"
    "\t\t\twhile(pxi_is_rx_empty());\r\n"
    "\t\t\tdata = pxi_recv();\r\n"
    "\t\t\tvman_reg_write(dev, reg, data);\r\n"
    "\t\t\tbreak;\r\n"
)
NEW_WRITE = (
    "\t\tcase 1: // WRITE REGISTER\r\n"
    "\t\t\t{\r\n"
    "\t\t\t\tu32 spin = PXI_WAIT_TIMEOUT;\r\n"
    "\t\t\t\twhile(pxi_is_rx_empty()) {\r\n"
    "\t\t\t\t\tif(!spin--) return;\r\n"
    "\t\t\t\t}\r\n"
    "\t\t\t}\r\n"
    "\t\t\tdata = pxi_recv();\r\n"
    "\t\t\tvman_reg_write(dev, reg, data);\r\n"
    "\t\t\tbreak;\r\n"
)

OLD_ANCHOR = "static void on_pxi_recv(u32 irqn) {\r\n"
NEW_ANCHOR = (
    "/*\r\n"
    " * Upper bound (loop iterations) for the PXI FIFO waits in the RX\r\n"
    " * interrupt handler.  The ARM9 runs at ~134 MHz and the FIFO is 16 words\r\n"
    " * deep, so this is enormously generous - it only has to turn a wedged\r\n"
    " * FIFO into an abandoned transaction instead of a dead ARM9.\r\n"
    " */\r\n"
    "#define PXI_WAIT_TIMEOUT\t0x400000u\r\n"
    "\r\n"
) + OLD_ANCHOR


def main():
    if len(sys.argv) < 2:
        print("usage: fix-arm9-pxi-timeout.py <arm9linuxfw-tree>", file=sys.stderr)
        return 2
    fw = pathlib.Path(sys.argv[1])
    p = fw / "source/main.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    main.c: PXI FIFO waits already bounded")
        return 0

    if OLD_ANCHOR not in s:
        print("error: main.c: on_pxi_recv() not found", file=sys.stderr)
        return 1
    s = s.replace(OLD_ANCHOR, NEW_ANCHOR, 1)

    for old, new, what in ((OLD_READ, NEW_READ, "READ tx-full wait"),
                           (OLD_WRITE, NEW_WRITE, "WRITE rx-empty wait")):
        if old not in s:
            print(f"error: main.c: {what} not found", file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    with p.open("w", newline="") as f:
        f.write(s)

    body = s.split("static void on_pxi_recv")[1].split("void NORETURN")[0]
    assert body.count("PXI_WAIT_TIMEOUT") >= 2, "main.c: bounds missing"
    assert "while(pxi_is_tx_full());" not in body, "main.c: unbounded tx wait left"
    assert "while(pxi_is_rx_empty());" not in body, "main.c: unbounded rx wait left"

    print("    main.c: PXI READ/WRITE FIFO waits bounded (PXI_WAIT_TIMEOUT)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
