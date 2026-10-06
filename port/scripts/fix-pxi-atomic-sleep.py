#!/usr/bin/env python3
"""
fix-pxi-atomic-sleep.py - make the 3DS PXI FIFO transaction atomic-safe

WHY THIS EXISTS
---------------
`drivers/platform/nintendo3ds/ctr_pxi.c` implements the PXI FIFO transport that
the virtio-blk device (`/dev/vda`, the SD card) rides on.  Its `pxi_txrx()`
originally slept:

    might_sleep();
    mutex_lock(&pxi->fifo_lock);
    wait_event_interruptible_timeout(pxi->fifo_wq, ..., PXI_FIFO_TIMEOUT);

That is fine from process context, but the block layer calls
`->queue_rq` under `rcu_read_lock()`:

    blk_mq_dispatch_rq_list
      -> virtio_queue_rq
        -> virtqueue_notify
          -> vpxi_notify
            -> vpxi_write_reg -> vpxi_multiwrite_reg
              -> pxi_txrx            <-- sleeps while atomic!

On hardware with lockdep enabled this is exactly what was reported, over and
over, right before the whole machine froze ~3 s after the framework started its
first app:

    BUG: sleeping function called from invalid context at
        drivers/platform/nintendo3ds/ctr_pxi.c:82
    in_atomic(): 1, irqs_disabled(): 0, non_block: 0, pid: 92,
        name: kworker/0:1H
    3 locks held by kworker/0:1H/92:
     #0: ((wq_completion)kblockd)
     #1: ((work_completion)(&(&hctx->run_work)->work))
     #2: (rcu_read_lock)
    Workqueue: kblockd blk_mq_run_work_fn
     pxi_txrx from vpxi_multiwrite_reg from vpxi_write_reg from vpxi_notify
     from virtqueue_notify from virtio_queue_rq from blk_mq_dispatch_rq_list
    ...
    [ BUG: Invalid wait context ]  (&pxi->fifo_lock) at pxi_txrx+0x3c/0x264
    BUG: scheduling while atomic: kworker/0:1H/92/0x00000002
    BUG: workqueue leaked lock or atomic: kworker/0:1H ... blk_mq_run_work_fn

Scheduling away with `rcu_read_lock` held / an atomic preempt count corrupts
the scheduler and RCU state, and the machine eventually stops scheduling
entirely (no hung-task report, no softlockup report - which is why it looked
like an IRQ-off spin for so long).

THE FIX
-------
`pxi_txrx()` must never sleep.  A spinlock plus a bounded busy-poll of the FIFO
status flags is enough: the PXI FIFO is 16 words deep and the ARM9 drains it
immediately, so the poll normally succeeds on the first read.  `PXI_POLL_MAX`
only bounds the pathological case.

Idempotent: running it twice changes nothing.
"""

import sys
import pathlib
import re

MARK = "PXI_POLL_MAX"

OLD_FUNC = '''static int pxi_txrx(struct pxi_host *pxi, const u32 *ww, int nw, u32 *wr,
		    int nr)
{
	long err;

	might_sleep();
	mutex_lock(&pxi->fifo_lock);

	while (nw > 0) { // send
		err = wait_event_interruptible_timeout(
			pxi->fifo_wq, !(pxi_tx_full(pxi)), PXI_FIFO_TIMEOUT);
		if (unlikely(err <= 0)) {
			err = -ETIMEDOUT;
			goto fifo_err;
		}

		iowrite32(*(ww++), pxi->regs + REG_PXI_TX);

		err = pxi_check_err(pxi);
		if (err)
			goto fifo_err;
		nw--;
	}

	while (nr > 0) { // recv
		err = wait_event_interruptible_timeout(
			pxi->fifo_wq, !(pxi_rx_empty(pxi)), PXI_FIFO_TIMEOUT);
		if (unlikely(err <= 0)) {
			err = -ETIMEDOUT;
			goto fifo_err;
		}

		*(wr++) = ioread32(pxi->regs + REG_PXI_RX);

		err = pxi_check_err(pxi);
		if (err)
			goto fifo_err;
		nr--;
	}

fifo_err:
	mutex_unlock(&pxi->fifo_lock);
	return err;
}'''

NEW_FUNC = '''/*
 * PXI FIFO transaction.  MUST NOT SLEEP.
 *
 * The block layer calls ->queue_rq under rcu_read_lock() and the virtio-blk
 * path reaches this through
 *   virtio_queue_rq -> virtqueue_notify -> vpxi_notify -> vpxi_write_reg
 * so sleeping here (the original might_sleep()/mutex_lock()/
 * wait_event_interruptible_timeout() version) is a bug: lockdep reported
 * "sleeping function called from invalid context at ctr_pxi.c:82" from
 * kworker/0:1H in blk_mq_run_work_fn, then "scheduling while atomic", and the
 * machine stopped scheduling a few seconds after the framework started its
 * first app.
 *
 * A spinlock plus a bounded busy-poll of the FIFO status flags is short in
 * practice (the FIFO is 16 words deep and the ARM9 drains it immediately);
 * PXI_POLL_MAX only bounds the pathological case.
 */
static int pxi_txrx(struct pxi_host *pxi, const u32 *ww, int nw, u32 *wr,
		    int nr)
{
	unsigned long flags;
	long err = 0;

	spin_lock_irqsave(&pxi->fifo_lock, flags);

	while (nw > 0) { // send
		unsigned int poll = PXI_POLL_MAX;

		while (pxi_tx_full(pxi)) {
			if (!--poll) {
				err = -ETIMEDOUT;
				goto fifo_err;
			}
			cpu_relax();
		}

		iowrite32(*(ww++), pxi->regs + REG_PXI_TX);

		err = pxi_check_err(pxi);
		if (err)
			goto fifo_err;
		nw--;
	}

	while (nr > 0) { // recv
		unsigned int poll = PXI_POLL_MAX;

		while (pxi_rx_empty(pxi)) {
			if (!--poll) {
				err = -ETIMEDOUT;
				goto fifo_err;
			}
			cpu_relax();
		}

		*(wr++) = ioread32(pxi->regs + REG_PXI_RX);

		err = pxi_check_err(pxi);
		if (err)
			goto fifo_err;
		nr--;
	}

fifo_err:
	spin_unlock_irqrestore(&pxi->fifo_lock, flags);
	return err;
}'''


def main():
    if len(sys.argv) < 2:
        print("usage: fix-pxi-atomic-sleep.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])

    c = kd / "drivers/platform/nintendo3ds/ctr_pxi.c"
    h = kd / "drivers/platform/nintendo3ds/ctr_pxi.h"
    for p in (c, h):
        if not p.exists():
            print(f"error: {p} not found", file=sys.stderr)
            return 1

    src = c.read_text()
    if MARK in src:
        # keep the poll bound in sync if the script's value changed
        new_src = re.sub(r"#define PXI_POLL_MAX\s+\d+",
                         "#define PXI_POLL_MAX\t\t100000", src)
        if new_src != src:
            c.write_text(new_src)
            print("    ctr_pxi.c: PXI_POLL_MAX updated to 100000")
        else:
            print("    ctr_pxi.c: pxi_txrx already atomic-safe")
        return 0

    if OLD_FUNC not in src:
        print("error: ctr_pxi.c: pxi_txrx is neither the original nor the "
              "patched version", file=sys.stderr)
        return 1

    # 1. bound the busy-poll
    anchor = "#define PXI_FIFO_TIMEOUT\tmsecs_to_jiffies(1000)\n"
    assert anchor in src, "ctr_pxi.c: PXI_FIFO_TIMEOUT define not found"
    src = src.replace(
        anchor,
        anchor +
        "/* upper bound for the atomic FIFO poll in pxi_txrx() (iterations) */\n"
        "#define PXI_POLL_MAX\t\t100000\n",
        1)

    # 2. the function itself
    src = src.replace(OLD_FUNC, NEW_FUNC, 1)

    # 3. the lock is now a spinlock
    assert "mutex_init(&pxi->fifo_lock);" in src, \
        "ctr_pxi.c: mutex_init(&pxi->fifo_lock) not found"
    src = src.replace("mutex_init(&pxi->fifo_lock);",
                      "spin_lock_init(&pxi->fifo_lock);", 1)

    c.write_text(src)

    hsrc = h.read_text()
    assert "\tstruct mutex fifo_lock;\n" in hsrc, \
        "ctr_pxi.h: 'struct mutex fifo_lock;' not found"
    hsrc = hsrc.replace("\tstruct mutex fifo_lock;\n",
                        "\tspinlock_t fifo_lock;\n", 1)
    h.write_text(hsrc)

    # sanity: no sleeping call left in the code (the comment above pxi_txrx
    # mentions the old calls by name, so skip comment lines)
    code = "\n".join(
        ln for ln in NEW_FUNC.splitlines()
        if not ln.lstrip().startswith(("*", "/*", "//")))
    for bad in ("might_sleep", "mutex_lock", "mutex_unlock", "wait_event"):
        assert bad not in code, f"ctr_pxi.c: {bad} still present in pxi_txrx"

    print("    ctr_pxi.c: pxi_txrx is now spinlock + bounded busy-poll "
          "(atomic-safe)")
    print("    ctr_pxi.h: fifo_lock is now a spinlock")
    return 0


if __name__ == "__main__":
    sys.exit(main())
