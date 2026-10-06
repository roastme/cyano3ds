#!/usr/bin/env python3
"""
fix-pxi-notify-defer.py - make the PXI virtio kick safe from atomic context

WHY THIS EXISTS
---------------
`ctr_pxi.c` implements the PXI FIFO transport that the SD card's virtio-blk
device rides on.  Its `pxi_txrx()` sleeps (mutex + wait_event on the FIFO) -
fine from process context, but the virtio *kick* path is reached from atomic
context:

    blk_mq_run_work_fn (kworker/0:1H, under rcu_read_lock)
      -> virtio_queue_rq
        -> virtqueue_notify
          -> vpxi_notify
            -> vpxi_write_reg -> vpxi_multiwrite_reg -> pxi_txrx   <-- sleeps!

Lockdep reported exactly this, over and over, right before the whole machine
froze a few seconds after the framework started its first app:

    BUG: sleeping function called from invalid context at
        drivers/platform/nintendo3ds/ctr_pxi.c:82
    in_atomic(): 1, irqs_disabled(): 0, pid: 92, name: kworker/0:1H
    Workqueue: kblockd blk_mq_run_work_fn
    ... pxi_txrx <- vpxi_multiwrite_reg <- vpxi_write_reg <- vpxi_notify
        <- virtqueue_notify <- virtio_queue_rq <- blk_mq_dispatch_rq_list

Scheduling away with `rcu_read_lock` held / an atomic preempt count corrupts
scheduler and RCU state and the machine eventually stops scheduling entirely
(no hung-task report, no softlockup report - which is why it looked like an
IRQ-off spin for so long).

Busy-polling the FIFO instead is *not* a fix: under heavy SD I/O the ARM9 keeps
the FIFO full, and a long IRQ-off poll starves the whole system - the storage-
free SCHED_FIFO heartbeat stops, which is indistinguishable from a frozen
kernel.

THE FIX
-------
In atomic context, defer the kick to a workqueue (process context, where
sleeping is allowed); from process context, do it directly.  `pxi_txrx()` keeps
its original sleeping implementation, so `might_sleep()` still catches any
remaining atomic caller (with CONFIG_DEBUG_ATOMIC_SLEEP).

Idempotent: running it twice changes nothing.
"""

import sys
import pathlib

MARK = "vpxi_notify_work"

OLD_NOTIFY = '''static bool vpxi_notify(struct virtqueue *vq)
{
	struct virtio_pxi_dev *vpd = to_vpxi_dev(vq->vdev);
	struct pxi_host *pxi = to_pxi_host(vpd);
	vpxi_write_reg(pxi, vpd->id, VPXI_REG_QUEUE_NOTIFY(vq->index), 1);
	return true;
}'''

NEW_NOTIFY = '''/*
 * The virtio kick path is reached from atomic context: the block layer calls
 * ->queue_rq under rcu_read_lock(), and virtio_queue_rq -> virtqueue_notify ->
 * vpxi_notify -> vpxi_write_reg -> pxi_txrx, which sleeps (mutex + wait_event
 * on the PXI FIFO).  Sleeping there is illegal and was the machine's hard
 * freeze (see fix-pxi-notify-defer.py).
 *
 * Busy-polling the FIFO instead is not a fix either: under heavy SD I/O the
 * ARM9 keeps the FIFO full and a long IRQ-off poll starves the whole system.
 *
 * So in atomic context the kick is deferred to a workqueue (process context,
 * where sleeping is allowed); otherwise it is done directly.
 */
static void vpxi_notify_work(struct work_struct *work)
{
	struct pxi_host *pxi = container_of(work, struct pxi_host, notify_work);
	struct virtio_pxi_dev *vpd = pxi->notify_vpd;

	if (vpd)
		vpxi_write_reg(pxi, vpd->id,
			       VPXI_REG_QUEUE_NOTIFY(pxi->notify_index), 1);
}

static bool vpxi_notify(struct virtqueue *vq)
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
}'''

OLD_STRUCT = "\tstruct work_struct irq_worker;\n"
NEW_STRUCT = ("\tstruct work_struct irq_worker;\n"
              "\t/* deferred virtio kick (see fix-pxi-notify-defer.py) */\n"
              "\tstruct work_struct notify_work;\n"
              "\tstruct virtio_pxi_dev *notify_vpd;\n"
              "\tunsigned notify_index;\n")

OLD_INIT = "\tmutex_init(&pxi->fifo_lock);\n"
NEW_INIT = ("\tmutex_init(&pxi->fifo_lock);\n"
            "\tINIT_WORK(&pxi->notify_work, vpxi_notify_work);\n")

# pxi_initialize_host() is defined *before* vpxi_notify_work(), so forward
# declare it (otherwise INIT_WORK() gets an implicit declaration).
OLD_FWD = "static void pxi_initialize_host(struct pxi_host *pxi)\n{\n"
NEW_FWD = ("static void vpxi_notify_work(struct work_struct *work);\n\n"
           + OLD_FWD)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-pxi-notify-defer.py <kernel-tree>", file=sys.stderr)
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
        print("    ctr_pxi.c: virtio kick already deferred from atomic context")
        return 0

    if OLD_NOTIFY not in src:
        print("error: ctr_pxi.c: vpxi_notify() is not the pristine version",
              file=sys.stderr)
        return 1
    src = src.replace(OLD_NOTIFY, NEW_NOTIFY, 1)

    if OLD_INIT not in src:
        print("error: ctr_pxi.c: mutex_init(&pxi->fifo_lock) not found",
              file=sys.stderr)
        return 1
    src = src.replace(OLD_INIT, NEW_INIT, 1)

    if OLD_FWD not in src:
        print("error: ctr_pxi.c: pxi_initialize_host() not found", file=sys.stderr)
        return 1
    src = src.replace(OLD_FWD, NEW_FWD, 1)

    c.write_text(src)

    hsrc = h.read_text()
    if OLD_STRUCT not in hsrc:
        print("error: ctr_pxi.h: 'struct work_struct irq_worker;' not found",
              file=sys.stderr)
        return 1
    hsrc = hsrc.replace(OLD_STRUCT, NEW_STRUCT, 1)
    h.write_text(hsrc)

    print("    ctr_pxi.c: vpxi_notify() defers the kick when in atomic context")
    print("    ctr_pxi.h: notify_work / notify_vpd / notify_index added")
    print("    ctr_pxi.c: pxi_txrx() keeps its (sleeping) original version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
