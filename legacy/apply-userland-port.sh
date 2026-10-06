#!/bin/bash
# apply-userland-port.sh - install the 3DS device directory into a Donut tree
#
# Usage: apply-userland-port.sh <donut-source-tree>
#
# The Donut tree can be reconstructed with:
#   repo init -u https://github.com/froyocomb/android.git -b donut-bakery -m <build>.xml
#   repo sync
# or use a prebuilt system image and only apply the HAL parts by hand.
#
# This script:
#   1. copies device/nintendo3ds/ (BoardConfig.mk, product mk, init.rc delta,
#      ueventd rules, copybit HAL, fbtest)
#   2. patches system/core/rootdir/init.rc for the 3DS:
#        - removes the yaffs2/mtd mounts (the initramfs already mounted
#          /system and /data from the SD card's ext4 images; a failing mount in
#          Donut's init is fatal)
#        - appends the device nodes, syslinks and properties the port needs
#   3. verifies that nothing obviously conflicted

set -euo pipefail

DONUT="${1:-}"
LEGACY="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$LEGACY/.." && pwd)"
PORT="$REPO/port"
DEV=device/nintendo3ds

die() { echo "error: $*" >&2; exit 1; }

[ -n "$DONUT" ] || die "usage: $(basename "$0") <donut-source-tree>"
[ -f "$DONUT/build/core/main.mk" ] || die "$DONUT does not look like an Android (Donut) tree"
[ -d "$PORT/userland/device/nintendo3ds" ] || die "missing $PORT/userland/device/nintendo3ds"

echo "==> copying $DEV"
mkdir -p "$DONUT/$DEV"
cp -a "$PORT/userland/device/nintendo3ds/." "$DONUT/$DEV/"
mkdir -p "$DONUT/$DEV/tests"
cp -a "$PORT/userland/tests/fbtest.c" "$DONUT/$DEV/tests/"

echo "==> patching system/core/rootdir/init.rc"
python3 - "$DONUT" "$PORT" <<'PY'
import pathlib, re, sys

donut = pathlib.Path(sys.argv[1])
port = pathlib.Path(sys.argv[2])

init_rc = donut / "system/core/rootdir/init.rc"
if not init_rc.exists():
    print("    WARNING: init.rc not found (different Donut revision?)")
    sys.exit(0)

s = init_rc.read_text()
orig = s

# 1. disable the storage mounts: the initramfs mounted them already.
s = re.sub(r'(?m)^(\s*)(mount\s+.*)$', r'\1# [3ds] \2', s)

# 2. append the port's own section once.
marker = "# --- Android-3DS port ---"
if marker not in s:
    delta = port / "userland/device/nintendo3ds/init.nintendo3ds.rc"
    block = []
    for line in delta.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        block.append(line)
    s += "\n" + marker + "\n" + "\n".join(block) + "\n"

if s != orig:
    init_rc.write_text(s)
    print("    init.rc patched")
else:
    print("    init.rc already patched")
PY

echo "==> sanity checks"
grep -q "nintendo3ds" "$DONUT/$DEV/BoardConfig.mk" || die "BoardConfig.mk wrong"
grep -q "copybit.nintendo3ds" "$DONUT/$DEV/copybit/Android.mk" || die "HAL Android.mk wrong"
echo "    device dir ok"

cat <<'TXT'

Next (M3):
  1. lunch nintendo3ds-eng && make      (in a Donut-era build environment)
  2. put out/target/product/nintendo3ds/system into an ext4 image:
       truncate -s 256M system.img && mkfs.ext4 -L system system.img
       mount -o loop system.img /mnt/sys && cp -a .../system/. /mnt/sys/ && umount
  3. copy system.img + data.img into /android/ on the SD card
  4. add /init + /init.rc to the initramfs:  port/scripts/mkinitramfs.sh --android
  5. boot and watch the top screen console for:
       servicemanager, zygote, surfaceflinger
TXT