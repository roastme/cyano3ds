#!/bin/bash
# apply-arm9linuxfw-port.sh - give Linux a writable SD card
#
# Usage: apply-arm9linuxfw-port.sh [arm9linuxfw-tree]
#        (default: /root/p3ds/src/arm9linuxfw)
#
# Replaces source/vdev/sdcard.c with the port's version, which implements
# VIRTIO_BLK_T_OUT (writes) and stops advertising VIRTIO_BLK_F_RO, so Linux
# sees a read-write /dev/vda.  Upstream only implements reads, which is why
# Android's /data and SD-card logging were impossible until now.
#
# The SDMMC hardware layer already had a working sdmmc_sdcard_writesectors()
# (firm_linux_loader's FAT stack uses the same routine), so this only wires it
# up to virtio.
#
# Idempotent: running it twice changes nothing.

set -euo pipefail

FW="${1:-/root/p3ds/src/arm9linuxfw}"
PORT="$(cd "$(dirname "$0")/.." && pwd)"

die() { echo "error: $*" >&2; exit 1; }

[ -f "$FW/source/vdev/sdcard.c" ] || die "no arm9linuxfw tree at $FW"
[ -f "$PORT/arm9linuxfw/sdcard.c" ] || die "missing $PORT/arm9linuxfw/sdcard.c"

# NOTE: match the actual upstream *define*, not the bare string - the patched
# file mentions VIRTIO_BLK_F_RO in its comments, so a plain grep always matched
# and clobbered source/vdev/sdcard.c.upstream with the patched version.
if grep -q '^#define VIRTIO_BLK_F_RO' "$FW/source/vdev/sdcard.c"; then
	cp "$FW/source/vdev/sdcard.c" "$FW/source/vdev/sdcard.c.upstream"
	install -m 644 "$PORT/arm9linuxfw/sdcard.c" "$FW/source/vdev/sdcard.c"
	echo "    sdcard.c: replaced (writes enabled, VIRTIO_BLK_F_RO removed)"
elif grep -q "no VIRTIO_BLK_F_RO" "$FW/source/vdev/sdcard.c"; then
	echo "    sdcard.c: already patched"
else
	die "$FW/source/vdev/sdcard.c is neither upstream nor the patched version"
fi

grep -q "sdmmc_sdcard_writesectors" "$FW/source/vdev/sdcard.c" || \
	die "patched sdcard.c does not reference sdmmc_sdcard_writesectors"
echo "    verified: write path present"

# ARM9 black-box capture (option B): the ARM9 polls a page of ARM11 DRAM and
# mirrors it straight to the card, bypassing the Linux block layer.  See
# port/arm9linuxfw/capture.c and ctr_lcd_fb.c.
install -m 644 "$PORT/arm9linuxfw/capture.c" "$FW/source/capture.c"
install -m 644 "$PORT/arm9linuxfw/capture.h" "$FW/include/capture.h"
python3 "$PORT/scripts/fix-arm9-capture.py" "$FW"

# Bound the SD/MMC busy-waits: an unbounded wait in the ARM9 stops it draining
# the PXI FIFO, which stalls every Linux block request and (with the pxi
# transport's IRQ-off FIFO poll) freezes the whole machine.
python3 "$PORT/scripts/fix-arm9-sdmmc-timeout.py" "$FW"

# Bound the *PXI* FIFO waits in the RX interrupt handler too: the same class of
# bug, but it deadlocks the ARM9 inside its own IRQ handler (see the script).
python3 "$PORT/scripts/fix-arm9-pxi-timeout.py" "$FW"

# Do not hold the ARM9 critical section (interrupts disabled) across a whole
# run of SD transfers: while it transfers, the PXI RX interrupt cannot run, so
# an ARM11 register transaction (a virtio notify, or the IRQ-bank read in
# vpxi_irq_worker) hits its 1 s timeout and is flushed - losing the virtio
# completion and stalling every later block request behind it.  Service one
# request per critical section instead (see the script).
python3 "$PORT/scripts/fix-arm9-sdcard-jobbound.py" "$FW"
