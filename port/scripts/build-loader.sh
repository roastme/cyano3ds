#!/bin/bash
# build-loader.sh - build the FIRM payload + ARM9 firmware for the 3DS
#
# Usage: build-loader.sh [src-dir] [out-dir]
#
# Builds, from https://github.com/linux-3ds:
#   firm_linux_loader.firm   (ARM11 FIRM payload: loads zImage/dtb/initramfs,
#                             boots the ARM9 firmware, does the LCD setup)
#   arm9linuxfw.bin          (ARM9 TCM firmware: SD/MMC + PXI/virtio bridge)
#
# Requirements:
#   arm-none-eabi-gcc            (dpkg: gcc-arm-none-eabi)
#   firmtool                     (pip3 install --break-system-packages firmtool)

set -euo pipefail

SRC="${1:-$HOME/p3ds/src}"
OUT="${2:-$HOME/p3ds/dist}"
PORT="$(cd "$(dirname "$0")/.." && pwd)"
export CC=arm-none-eabi-gcc

mkdir -p "$OUT"

echo "==> cloning/updating sources in $SRC"
for r in firm_linux_loader arm9linuxfw; do
	if [ ! -d "$SRC/$r" ]; then
		git clone --depth=1 "https://github.com/linux-3ds/$r.git" "$SRC/$r"
	else
		echo "    $r: present"
	fi
done

echo "==> firmtool"
if ! command -v firmtool >/dev/null; then
	pip3 install --break-system-packages firmtool \
		|| pip3 install --break-system-packages --user firmtool
fi
command -v firmtool >/dev/null || \
	{ echo "firmtool not available - install it manually"; exit 1; }

echo "==> firm_linux_loader"
# Leave the New 3DS at 804 MHz instead of downclocking to 268 MHz.  The TWD
# timer clock (CPU/2) is described in the device tree, so the kernel knows the
# right rate; see patch-firm-loader-clock.py for why this is done here rather
# than with a runtime cpufreq switch.
python3 "$PORT/scripts/patch-firm-loader-clock.py" "$SRC/firm_linux_loader"
make -C "$SRC/firm_linux_loader" clean >/dev/null 2>&1 || true
make -C "$SRC/firm_linux_loader"
cp "$SRC/firm_linux_loader/firm_linux_loader.firm" "$OUT/firm_linux_loader.firm"
# fastboot3DS' file browser shows the file name; give it a recognisable one.
cp "$OUT/firm_linux_loader.firm" "$OUT/Cyano3DS.firm"

echo "==> arm9linuxfw"
# The upstream firmware exposes the SD card read-only (VIRTIO_BLK_F_RO and no
# VIRTIO_BLK_T_OUT handling).  Android needs a writable /data and this port
# wants SD-card boot logs, so patch the virtio block device first.
bash "$PORT/scripts/apply-arm9linuxfw-port.sh" "$SRC/arm9linuxfw"
make -C "$SRC/arm9linuxfw" clean >/dev/null 2>&1 || true
make -C "$SRC/arm9linuxfw"
find "$SRC/arm9linuxfw" -maxdepth 2 -name 'arm9linuxfw*.bin' -exec cp {} "$OUT/arm9linuxfw.bin" \;

echo
echo "==> artifacts in $OUT"
ls -l "$OUT"
