#!/usr/bin/env python3
"""
fix-pxi-flush-on-timeout.py - resynchronise the PXI FIFOs after a timeout

WHY THIS EXISTS
---------------
`pxi_txrx()` (drivers/platform/nintendo3ds/ctr_pxi.c) is a two-phase
transaction: it pushes `nw` words into the TX FIFO, then reads `nr` words from
the RX FIFO, waiting up to PXI_FIFO_TIMEOUT (1 s) on the ARM9.

If that timeout fires, the ARM9 may still be mid-transaction and may push its
response into the TX FIFO *after* we gave up.  That stale word is then read as
the first word of the *next* transaction, desynchronising the protocol: the
driver reads a queue-notify value where it expected a config byte, etc.  The
block layer then sees nonsense completions - or, worse, a request that never
completes.

The ARM9 side is now bounded too (fix-arm9-pxi-timeout.py), so a timeout is an
expected, recoverable event and must leave both FIFOs clean.  This patch writes
the FIFO-flush bit (the same sequence `pxi_check_err()` already uses on an
error) before returning -ETIMEDOUT.

Idempotent: running it twice changes nothing.
"""

import sys
import pathlib

MARK = "pxi_fifo_flush"

OLD_FUNC_HEAD = """static int pxi_txrx(struct pxi_host *pxi, const u32 *ww, int nw, u32 *wr,
		    int nr)
{
	long err;

	might_sleep();
	mutex_lock(&pxi->fifo_lock);
"""

NEW_FUNC_HEAD = """/*
 * Drop whatever is in the PXI FIFOs and re-enable them.  Called after a
 * transaction times out: the ARM9 may still push its late response, which
 * would otherwise be read as the first word of the next transaction and
 * desynchronise the protocol (see fix-pxi-flush-on-timeout.py).
 */
static void pxi_fifo_flush(struct pxi_host *pxi)
{
	iowrite32(PXI_CNT_FIFO_FLUSH | PXI_CNT_ERRACK | PXI_CNT_ENABLE,
		  pxi->regs + REG_PXI_CNT);
}

""" + OLD_FUNC_HEAD

OLD_TIMEOUT = """		if (unlikely(err <= 0)) {
			err = -ETIMEDOUT;
			goto fifo_err;
		}
"""
NEW_TIMEOUT = """		if (unlikely(err <= 0)) {
			err = -ETIMEDOUT;
			pxi_fifo_flush(pxi);
			goto fifo_err;
		}
"""


def main():
    if len(sys.argv) < 2:
        print("usage: fix-pxi-flush-on-timeout.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/ctr_pxi.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    s = p.read_text()
    if MARK in s:
        print("    ctr_pxi.c: PXI FIFO flush on timeout already present")
        return 0

    if OLD_FUNC_HEAD not in s:
        print("error: ctr_pxi.c: pxi_txrx() head is not the expected version",
              file=sys.stderr)
        return 1
    s = s.replace(OLD_FUNC_HEAD, NEW_FUNC_HEAD, 1)

    n = s.count(OLD_TIMEOUT)
    if n != 2:
        print(f"error: ctr_pxi.c: expected 2 timeout blocks, found {n}",
              file=sys.stderr)
        return 1
    s = s.replace(OLD_TIMEOUT, NEW_TIMEOUT)

    p.write_text(s)
    assert s.count("pxi_fifo_flush(pxi);") == 2, "flush helper not wired up"
    print("    ctr_pxi.c: FIFO flushed on both PXI timeout paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
