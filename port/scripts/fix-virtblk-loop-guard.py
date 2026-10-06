#!/usr/bin/env python3
"""
fix-virtblk-loop-guard.py - bound the virtio-blk completion loop

WHY THIS EXISTS
---------------
`virtblk_done()` runs with interrupts disabled (it is called from
`vpxi_irq_worker` under `spin_lock_irqsave(&vpd->lock)` and then takes
`spin_lock_irqsave(&vblk->vqs[qid].lock)` itself).  Its drain loop is:

    do {
        virtqueue_disable_cb(vq);
        while ((vbr = virtqueue_get_buf(...)) != NULL) { ... }
        if (unlikely(virtqueue_is_broken(vq)))
            break;
    } while (!virtqueue_enable_cb(vq));

The loop only exits when `virtqueue_enable_cb()` sees no new used entries.  If
the used ring ever advances faster than it is drained (or its index wraps in a
way the driver mis-reads - the ARM9 side writes `used->last` itself), this loop
never terminates *with interrupts off*, which is a hard, silent freeze: the
timer square stops, no hung-task report, no softlockup, no oops - exactly what
the port sees under heavy SD load.

This patch adds a generous iteration bound and a rate-limited warning, so a
runaway drain turns into a log line plus (possibly) a stuck request instead of
a dead machine.

Idempotent: running it twice changes nothing.
"""

import sys
import pathlib

MARK = "completion loop guard"

OLD = """	spin_lock_irqsave(&vblk->vqs[qid].lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((vbr = virtqueue_get_buf(vblk->vqs[qid].vq, &len)) != NULL) {
			struct request *req = blk_mq_rq_from_pdu(vbr);

			if (likely(!blk_should_fake_timeout(req->q)))
				blk_mq_complete_request(req);
			req_done = true;
		}
		if (unlikely(virtqueue_is_broken(vq)))
			break;
	} while (!virtqueue_enable_cb(vq));
"""

NEW = """	spin_lock_irqsave(&vblk->vqs[qid].lock, flags);
	/*
	 * Bound the drain loop (fix-virtblk-loop-guard.py).  It runs with IRQs
	 * disabled; if the used ring ever advances faster than it is drained
	 * this loop spins forever and the machine hard-freezes with no report.
	 * A runaway drain now logs and gives up instead.
	 */
	{
	unsigned int guard = 0;
	do {
		virtqueue_disable_cb(vq);
		while ((vbr = virtqueue_get_buf(vblk->vqs[qid].vq, &len)) != NULL) {
			struct request *req = blk_mq_rq_from_pdu(vbr);

			if (likely(!blk_should_fake_timeout(req->q)))
				blk_mq_complete_request(req);
			req_done = true;
		}
		if (unlikely(virtqueue_is_broken(vq)))
			break;
		if (unlikely(++guard > 200000)) {
			pr_warn_ratelimited(
				"virtio_blk: completion loop guard hit (qid %d)\\n",
				qid);
			break;
		}
	} while (!virtqueue_enable_cb(vq));
	}
"""


def main():
    if len(sys.argv) < 2:
        print("usage: fix-virtblk-loop-guard.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/block/virtio_blk.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1
    s = p.read_text()
    if MARK in s:
        print("    virtio_blk.c: completion loop already bounded")
        return 0
    if OLD not in s:
        print("error: virtio_blk.c: virtblk_done() loop not in the expected form",
              file=sys.stderr)
        return 1
    p.write_text(s.replace(OLD, NEW, 1))
    print("    virtio_blk.c: virtblk_done() drain loop bounded")
    return 0


if __name__ == "__main__":
    sys.exit(main())
