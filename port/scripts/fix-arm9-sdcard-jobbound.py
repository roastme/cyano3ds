#!/usr/bin/env python3
"""
fix-arm9-sdcard-jobbound.py - stop the ARM9 holding IRQs off across a whole
                              run of SD transfers

WHY THIS EXISTS
---------------
`vman_process_pending()` (source/virt/manager.c) de-queues a virtqueue and calls
`vdev_process_vqueue()` *inside* a `CRITICAL_BLOCK()`, which on the ARM9 disables
interrupts.  The SD card device's handler, `sdmc_process_vqueue()`, then drains
the ENTIRE virtqueue - one `sdmmc_sdcard_readsectors()`/`writesectors()` transfer
per request - before returning and letting the main loop re-enable interrupts.

While that runs, the ARM9 cannot service the PXI RX interrupt (`on_pxi_recv()`),
so any ARM11 register transaction waits in `pxi_txrx()` (config read, queue
notify, or the IRQ-bank read in `vpxi_irq_worker`).  With a deep queue the window
exceeds the transport's 1 s timeout, the transaction is abandoned and the FIFOs
are flushed - losing the virtio completion.  Every later block request queued
behind it never completes, every SD writer blocks in `D`, and the machine looks
hard-frozen while the ARM11 timer square keeps blinking.  (Nothing is logged
because the only log sink is that same wedged card.)

THE FIX
-------
Service at most SDMC_JOBS_PER_CALL requests per call.  If more are pending,
re-add the virtqueue to the manager's pending list (`vman_add_pending()`; the
list helpers reset the node on remove, so re-adding is correct) and return.  The
main loop then runs again with interrupts re-enabled, so the PXI FIFO is serviced
between requests.  No work is lost: the queue is simply processed over several
passes.

Idempotent: running it twice changes nothing.  The file uses LF line endings.
"""

import sys
import pathlib

MARK = "SDMC_JOBS_PER_CALL"

OLD_DEFS = """#define VIRTIO_BLK_S_OK\t\t0
#define VIRTIO_BLK_S_IOERR\t1
"""

NEW_DEFS = """#define VIRTIO_BLK_S_OK\t\t0
#define VIRTIO_BLK_S_IOERR\t1

/*
 * Upper bound on how many virtio requests this device services per call.
 *
 * vman_process_pending() calls sdmc_process_vqueue() from inside a
 * CRITICAL_BLOCK, i.e. with ARM9 interrupts disabled, and only re-enables
 * them between calls.  A long run of SD transfers therefore blocks the PXI
 * RX interrupt (see the comment in fix-arm9-sdcard-jobbound.py): an ARM11
 * register transaction waits, times out after 1 s and is flushed, losing the
 * virtio completion and hanging every later block request behind it.
 *
 * Re-adding the queue and returning after one request lets the main loop
 * re-enable interrupts between requests; the queue stays in the pending
 * list, so no work is lost.
 */
#define SDMC_JOBS_PER_CALL\t1
"""

OLD_HEAD = """static void sdmc_process_vqueue(vdev_s *vdev, vqueue_s *vq) {
\tvjob_s vjob;

\twhile(vqueue_fetch_job_new(vq, &vjob) >= 0) {
"""

NEW_HEAD = """static void sdmc_process_vqueue(vdev_s *vdev, vqueue_s *vq) {
\tvjob_s vjob;
\tunsigned jobs = 0;

\twhile(vqueue_fetch_job_new(vq, &vjob) >= 0) {
"""

OLD_TAIL = """\t\tvqueue_push_job(vq, &vjob);
\t}

\tvman_notify_host(vdev, VIRQ_VQUEUE);
}
"""

NEW_TAIL = """\t\tvqueue_push_job(vq, &vjob);

\t\t/*
\t\t * Bound the IRQs-off window (see SDMC_JOBS_PER_CALL): re-queue
\t\t * the vq and let the ARM9 main loop run us again after it has
\t\t * re-enabled interrupts.  No work is lost - the queue is in the
\t\t * pending list again and vman_process_pending() will dequeue it.
\t\t */
\t\tif (++jobs >= SDMC_JOBS_PER_CALL) {
\t\t\tvman_add_pending(vq);
\t\t\tbreak;
\t\t}
\t}

\tvman_notify_host(vdev, VIRQ_VQUEUE);
}
"""


def main():
    if len(sys.argv) < 2:
        print("usage: fix-arm9-sdcard-jobbound.py <arm9linuxfw-tree>",
              file=sys.stderr)
        return 2
    fw = pathlib.Path(sys.argv[1])
    p = fw / "source/vdev/sdcard.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    s = p.read_text()
    if MARK in s:
        print("    sdcard.c: SD jobs per call already bounded")
        return 0

    for old, new, what in ((OLD_DEFS, NEW_DEFS, "virtio-blk defines"),
                           (OLD_HEAD, NEW_HEAD, "sdmc_process_vqueue() head"),
                           (OLD_TAIL, NEW_TAIL, "sdmc_process_vqueue() tail")):
        if old not in s:
            print(f"error: sdcard.c: {what} not found (unexpected version)",
                  file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    p.write_text(s)

    assert s.count("SDMC_JOBS_PER_CALL") >= 2, "sdcard.c: marker/define missing"
    assert "vman_add_pending(vq);" in s, "sdcard.c: re-queue missing"
    print("    sdcard.c: at most SDMC_JOBS_PER_CALL request(s) per ARM9 "
          "critical section")
    return 0


if __name__ == "__main__":
    sys.exit(main())
