#!/usr/bin/env python3
"""
fix-pxi-notify-always.py - always defer the PXI virtio kick to a workqueue

WHY THIS EXISTS
---------------
`vpxi_notify()` is reached from the block layer's `->queue_rq` path
(virtio_queue_rq -> virtqueue_notify) and it calls `vpxi_write_reg()`, which
goes through `pxi_txrx()` - and `pxi_txrx()` sleeps (might_sleep + mutex +
wait_event on the PXI FIFO).

`fix-pxi-notify-defer.py` made it defer *when it could tell* it was in atomic
context (`in_atomic() || irqs_disabled()`), but that test is not sufficient in
practice.  On the 23:07 boot with CONFIG_PREEMPT=y the kernel logged ~150 of
these, from the block layer's own workqueue:

    BUG: sleeping function called from invalid context at
        drivers/platform/nintendo3ds/ctr_pxi.c:94
    in_atomic(): 0, irqs_disabled(): 0, non_block: 0, pid: 138,
        name: kworker/0:2H
    Preemption disabled at: [<00000000>] 0x0
    Workqueue: kblockd blk_mq_run_work_fn
     pxi_txrx <- vpxi_multiwrite_reg <- vpxi_write_reg <- vpxi_notify
       <- virtqueue_notify <- virtio_queue_rq <- blk_mq_dispatch_rq_list

i.e. the `in_atomic()` probe said "not atomic" while `might_sleep()` disagreed.
That is a bug in the *probe*, not a licence to sleep there.

THE FIX
-------
Never call the sleeping path from `vpxi_notify()` at all: always hand the kick
to `notify_work` (a workqueue, process context - where sleeping is fine).  A
bit of latency on the kick is irrelevant (the ARM9 drains the virtqueue when
it sees the notify).

To avoid losing a kick when two notifies race with the worker, the work loops
on an atomic `notify_pending` bit and the notification always sets it before
scheduling.

Idempotent: running it twice changes nothing.
"""

import sys
import pathlib

MARK = "notify_pending"

OLD_WORK = """static void vpxi_notify_work(struct work_struct *work)
{
	struct pxi_host *pxi = container_of(work, struct pxi_host, notify_work);
	struct virtio_pxi_dev *vpd = pxi->notify_vpd;

	if (vpd)
		vpxi_write_reg(pxi, vpd->id,
			       VPXI_REG_QUEUE_NOTIFY(pxi->notify_index), 1);
}"""

NEW_WORK = """static void vpxi_notify_work(struct work_struct *work)
{
	struct pxi_host *pxi = container_of(work, struct pxi_host, notify_work);

	/*
	 * Loop until no notification arrived while we were kicking: a kick
	 * that raced with this worker must not be lost, or the block layer
	 * waits forever for a completion the ARM9 was never told to produce.
	 */
	do {
		struct virtio_pxi_dev *vpd;
		unsigned int index;

		clear_bit(0, &pxi->notify_pending);
		smp_mb__after_atomic();
		vpd = READ_ONCE(pxi->notify_vpd);
		index = READ_ONCE(pxi->notify_index);
		if (vpd)
			vpxi_write_reg(pxi, vpd->id,
				       VPXI_REG_QUEUE_NOTIFY(index), 1);
	} while (test_bit(0, &pxi->notify_pending));
}"""

OLD_NOTIFY = """static bool vpxi_notify(struct virtqueue *vq)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vq->vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	if (in_atomic() || irqs_disabled()) {
		pxi->notify_vpd = vpd;
		pxi->notify_index = vq->index;
		schedule_work(&pxi->notify_work);
		return true;
	}

	vpxi_write_reg(pxi, vpd->id, VPXI_REG_QUEUE_NOTIFY(vq->index), 1);
	return true;
}"""

NEW_NOTIFY = """static bool vpxi_notify(struct virtqueue *vq)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vq->vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);

	/*
	 * ALWAYS defer.  pxi_txrx() sleeps, this is reached from the block
	 * layer's ->queue_rq (sometimes under a spinlock), and the
	 * in_atomic()/irqs_disabled() probe used to be wrong about the
	 * context - the kernel logged "sleeping function called from invalid
	 * context" from kworker/0:2H in blk_mq_run_work_fn on real hardware.
	 * The workqueue runs in process context, where sleeping is legal.
	 */
	WRITE_ONCE(pxi->notify_vpd, vpd);
	WRITE_ONCE(pxi->notify_index, vq->index);
	set_bit(0, &pxi->notify_pending);
	schedule_work(&pxi->notify_work);
	return true;
}"""

OLD_HDR = """	struct virtio_pxi_dev *notify_vpd;
	unsigned notify_index;
"""
NEW_HDR = """	struct virtio_pxi_dev *notify_vpd;
	unsigned notify_index;
	/* set by vpxi_notify(), cleared by vpxi_notify_work() (see below) */
	unsigned long notify_pending;
"""


def main():
    if len(sys.argv) < 2:
        print("usage: fix-pxi-notify-always.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    c = kd / "drivers/platform/nintendo3ds/ctr_pxi.c"
    h = kd / "drivers/platform/nintendo3ds/ctr_pxi.h"
    for p in (c, h):
        if not p.exists():
            print(f"error: {p} not found", file=sys.stderr)
            return 1

    src = c.read_text()
    if "notify_pending" in src:
        print("    ctr_pxi.c: virtio kick is already always deferred")
        return 0

    for old, new, what in ((OLD_WORK, NEW_WORK, "vpxi_notify_work()"),
                           (OLD_NOTIFY, NEW_NOTIFY, "vpxi_notify()")):
        if old not in src:
            print(f"error: ctr_pxi.c: {what} is not the expected version",
                  file=sys.stderr)
            return 1
        src = src.replace(old, new, 1)

    c.write_text(src)

    hsrc = h.read_text()
    if OLD_HDR not in hsrc:
        print("error: ctr_pxi.h: notify_index field not found", file=sys.stderr)
        return 1
    hsrc = hsrc.replace(OLD_HDR, NEW_HDR, 1)
    h.write_text(hsrc)

    print("    ctr_pxi.c: vpxi_notify() always defers; work loops on "
          "notify_pending")
    print("    ctr_pxi.h: notify_pending added")
    return 0


if __name__ == "__main__":
    sys.exit(main())
