#!/bin/bash
# build-loader.sh - build the FIRM payload + ARM9 firmware for the 3DS
#
# Usage: build-loader.sh [src-dir] [out-dir]
#        defaults: $HOME/p3ds/src  $HOME/p3ds/dist
#
# Builds, from https://github.com/linux-3ds:
#   firm_linux_loader.firm   (ARM11 FIRM payload: loads zImage/dtb/initramfs,
#                             boots the ARM9 firmware, does the LCD setup)
#   arm9linuxfw.bin          (ARM9 TCM firmware: SD/MMC + PXI/virtio bridge)
#
# Requirements (bash scripts/check-deps.sh loader lists what is missing):
#   arm-none-eabi-gcc            (apt: gcc-arm-none-eabi binutils-arm-none-eabi
#                                 libnewlib-arm-none-eabi)
#   python3 with venv support    (apt: python3-venv)
#   git, make
#
# firmtool is not on PyPI under that name.  The upstream loader's Dockerfile
# installs it from git, and this script does the same, into a private venv
# under $SRC so nothing is installed system-wide (no --break-system-packages).

set -euo pipefail

SRC="${1:-$HOME/p3ds/src}"
OUT="${2:-$HOME/p3ds/dist}"
PORT="$(cd "$(dirname "$0")/.." && pwd)"
FIRMTOOL_REPO="${FIRMTOOL_REPO:-git+https://github.com/TuxSH/firmtool.git}"
VENV="$SRC/.venv-firmtool"
export CC=arm-none-eabi-gcc

die() { echo "FATAL: $*" >&2; exit 1; }

bash "$PORT/scripts/check-deps.sh" loader

mkdir -p "$OUT"

echo "==> cloning/updating sources in $SRC"
for r in firm_linux_loader arm9linuxfw; do
	if [ ! -d "$SRC/$r" ]; then
		git clone --depth=1 "https://github.com/linux-3ds/$r.git" "$SRC/$r"
	else
		echo "    $r: present"
	fi
done

echo "==> firmtool (private venv: $VENV)"
if [ ! -x "$VENV/bin/firmtool" ]; then
	python3 -m venv "$VENV" || die "python3 -m venv failed (apt: python3-venv)"
	"$VENV/bin/pip" install --quiet "$FIRMTOOL_REPO" \
		|| die "firmtool install from $FIRMTOOL_REPO failed"
fi
export PATH="$VENV/bin:$PATH"
command -v firmtool >/dev/null || die "firmtool is not on PATH after install"

echo "==> firm_linux_loader"
# Leave the New 3DS at 804 MHz instead of downclocking to 268 MHz.  The TWD
# timer clock (CPU/2) is described in the device tree, so the kernel knows the
# right rate; see patch-firm-loader-clock.py for why this is done here rather
# than with a runtime cpufreq switch.
python3 "$PORT/scripts/patch-firm-loader-clock.py" "$SRC/firm_linux_loader"
make -C "$SRC/firm_linux_loader" clean >/dev/null 2>&1 \
	|| echo "    (make clean failed; continuing with the existing objects)"
make -C "$SRC/firm_linux_loader"
[ -f "$SRC/firm_linux_loader/firm_linux_loader.firm" ] \
	|| die "firm_linux_loader.firm was not produced"
cp "$SRC/firm_linux_loader/firm_linux_loader.firm" "$OUT/firm_linux_loader.firm"
# fastboot3DS' file browser shows the file name; give it a recognisable one.
cp "$OUT/firm_linux_loader.firm" "$OUT/Cyano3DS.firm"

echo "==> arm9linuxfw"
# The upstream firmware exposes the SD card read-only (VIRTIO_BLK_F_RO and no
# VIRTIO_BLK_T_OUT handling).  Android needs a writable /data and this port
# wants SD-card boot logs, so patch the virtio block device first.
bash "$PORT/scripts/apply-arm9linuxfw-port.sh" "$SRC/arm9linuxfw"
make -C "$SRC/arm9linuxfw" clean >/dev/null 2>&1 \
	|| echo "    (make clean failed; continuing with the existing objects)"
make -C "$SRC/arm9linuxfw"
# Fail loudly: a missing file here used to leave the script reporting success.
FW_BIN="$(find "$SRC/arm9linuxfw" -maxdepth 2 -name 'arm9linuxfw*.bin' | head -n1)"
[ -n "$FW_BIN" ] || die "no arm9linuxfw*.bin under $SRC/arm9linuxfw"
cp "$FW_BIN" "$OUT/arm9linuxfw.bin"

echo
echo "==> artifacts in $OUT"
ls -l "$OUT"
