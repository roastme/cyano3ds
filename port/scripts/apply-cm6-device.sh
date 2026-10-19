#!/bin/bash
# apply-cm6-device.sh - install the old-3DS (CTR) device tree + CM6 product into
#                       the CyanogenMod 6 (froyo-stable) source tree.
#
# Usage:
#   bash port/scripts/apply-cm6-device.sh [CM6_DIR]

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CM6_DIR="${1:-${CM6_DIR:-$REPO/../cm6/src}}"

say() { printf '\n==> %s\n' "$*"; }

[ -d "$CM6_DIR/build" ] || {
	echo "no CyanogenMod tree at $CM6_DIR" >&2
	exit 1; }

say "1/8  device/nintendo/ctr"
mkdir -p "$CM6_DIR/device/nintendo/ctr"
cp -a "$REPO/port/userland/device-cm6/ctr/." "$CM6_DIR/device/nintendo/ctr/"
ls "$CM6_DIR/device/nintendo/ctr/" | sed 's/^/    /'

say "2/8  vendor/cyanogen/products/cyanogen_ctr.mk"
P="$CM6_DIR/vendor/cyanogen/products"
cp "$REPO/port/vendor/cm6/products/cyanogen_ctr.mk" "$P/"
# The manifest drops every other CM6 device tree (device/htc/..., samsung, ...),
# so their cyanogen_<device>.mk products would fail the product scan
# ("device/htc/ace/ace.mk does not exist").  Only list ours.
printf 'PRODUCT_MAKEFILES := \\\n    $(LOCAL_DIR)/cyanogen_ctr.mk\n' > "$P/AndroidProducts.mk"

# common.mk references things a no-blobs tree does not have: the proprietary
# RomManager.apk and a GoogleSearch module.  Idempotent.
python3 - "$P/common.mk" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
s2 = s.replace("PRODUCT_COPY_FILES +=  \\\n    vendor/cyanogen/proprietary/RomManager.apk:system/app/RomManager.apk \\\n",
               "# [3ds] RomManager.apk not present (no proprietary blobs)\n")
s2 = s2.replace("        Provision \\\n        GoogleSearch \\\n        LatinIME", "        Provision \\\n        LatinIME")
if s2 != s:
    p.write_text(s2)
    print("    common.mk: dropped RomManager / GoogleSearch")
else:
    print("    common.mk: already patched (or nothing to do)")
PY

say "3/8  register the lunch combo"
VS="$CM6_DIR/vendor/cyanogen/vendorsetup.sh"
if [ -f "$VS" ] && ! grep -q "cyanogen_ctr" "$VS"; then
	sed -i '1i add_lunch_combo cyanogen_ctr-eng' "$VS"
	echo "    prepended to vendor/cyanogen/vendorsetup.sh"
else
	echo "    already registered"
fi

say "4/8  toolchain name"
# froyo-stable's manifest tracks platform/prebuilt at master, which today only
# ships arm-eabi-4.4.3; Froyo's combo/TARGET_linux-arm.mk wants arm-eabi-4.4.0.
# 4.4.3 is a drop-in point release.
T="$CM6_DIR/prebuilt/linux-x86/toolchain"
if [ ! -e "$T/arm-eabi-4.4.0" ] && [ -d "$T/arm-eabi-4.4.3" ]; then
	ln -s arm-eabi-4.4.3 "$T/arm-eabi-4.4.0"
	echo "    arm-eabi-4.4.0 -> arm-eabi-4.4.3"
else
	echo "    ok"
fi

say "5/8  modules that need projects the manifest drops"
# Host-side modules that need jars (ddmlib, kxml2, layoutlib_api, ninepatch)
# from the dropped sdk project: framework host tests, Eclipse layoutlib.  And
# external/openvpn, which needs the external/lzo project (not in the manifest).
for m in frameworks/base/core/tests/hosttests frameworks/base/tools/layoutlib/bridge external/openvpn; do
	if [ -f "$CM6_DIR/$m/Android.mk" ]; then
		mv "$CM6_DIR/$m/Android.mk" "$CM6_DIR/$m/Android.mk.disabled"
		echo "    disabled $m/Android.mk"
	fi
done

say "6/8  kernel notice file"
# The notice-file rules want the prebuilt kernel's licence even on a
# TARGET_NO_KERNEL board.  The kernel here is linux-3ds (GPL-2.0).
K="$CM6_DIR/prebuilt/android-arm/kernel"
mkdir -p "$K"
if [ ! -f "$K/LINUX_KERNEL_COPYING" ]; then
	if [ -f "$REPO/../linux-3ds/linux/COPYING" ]; then
		cp "$REPO/../linux-3ds/linux/COPYING" "$K/LINUX_KERNEL_COPYING"
	else
		echo "The Linux kernel is licensed under the GNU GPL version 2." > "$K/LINUX_KERNEL_COPYING"
	fi
	echo "    created LINUX_KERNEL_COPYING"
fi

say "7/8  dx heap"
# The build's dx call asks for -Xmx1536M.  A 32-bit JVM (the i386 chroot) cannot
# reserve that much contiguous address space next to proot's mappings
# ("Could not reserve enough space for object heap").
sed -i 's/-JXms16M -JXmx1536M/-JXms16M -JXmx768M/' "$CM6_DIR/build/core/definitions.mk"
grep -n 'JXmx' "$CM6_DIR/build/core/definitions.mk" | sed 's/^/    /'

say "8/8  sound, Wi-Fi and low-memory tuning for the 3DS hardware"
# Froyo has no ALSA HAL and its Wi-Fi HAL insmods a module; the 3DS kernel has
# both drivers built in.  Each script is idempotent.
python3 "$REPO/port/scripts/fix-cm6-audio.py"    "$CM6_DIR"
python3 "$REPO/port/scripts/fix-cm7-wifi.py"     "$CM6_DIR"
python3 "$REPO/port/scripts/fix-cm7-wpa-wext.py" "$CM6_DIR"
# system_server cannot write other uids' /proc/<pid>/oom_adj; relay to /bin/memguard.
python3 "$REPO/port/scripts/fix-cm6-oomadj.py"   "$CM6_DIR"
# keep 3 hidden apps instead of 15
python3 "$REPO/port/scripts/fix-cm6-lowmem.py"   "$CM6_DIR"
