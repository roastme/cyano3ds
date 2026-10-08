#!/bin/bash
# apply-cm7-device.sh - install the Nintendo 3DS device tree + CM product into
#                       the CyanogenMod 7.2 (gb-release-7.2) source tree.
#
# Build CM7.2 from source for a `device/nintendo/nintendo3ds` tree.  The tree
# lives under
# port/userland/device/nintendo3ds/ and the CM product under
# port/vendor/cyanogen/products/; this script copies them into $CM7_DIR.
#
# Usage:
#   bash port/scripts/apply-cm7-device.sh [CM7_DIR]
#   CM7_DIR=/root/p3ds/cm7 bash port/scripts/apply-cm7-device.sh

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CM7_DIR="${1:-${CM7_DIR:-$HOME/p3ds/cm7}}"

say() { printf '\n==> %s\n' "$*"; }

[ -d "$CM7_DIR/build" ] || {
	echo "no CyanogenMod tree at $CM7_DIR" >&2
	exit 1; }

say "1/4  device/nintendo/nintendo3ds"
# AOSP/CM7 resolves TARGET_DEVICE with the glob `device/*/$(TARGET_DEVICE)/
# BoardConfig.mk`, i.e. it needs device/<vendor>/<device>/.  The port's tree
# lives one level flatter, so install it at device/nintendo/nintendo3ds/.
mkdir -p "$CM7_DIR/device/nintendo/nintendo3ds"
cp -a "$REPO/port/userland/device/nintendo3ds/." "$CM7_DIR/device/nintendo/nintendo3ds/"
# remove any older flat install so AndroidProducts.mk is not found twice
[ -d "$CM7_DIR/device/nintendo3ds" ] && rm -rf "$CM7_DIR/device/nintendo3ds"
ls "$CM7_DIR/device/nintendo/nintendo3ds/" | sed 's/^/    /'

say "2/4  vendor/cyanogen/products/cyanogen_nintendo3ds.mk"
mkdir -p "$CM7_DIR/vendor/cyanogen/products"
cp "$REPO/port/vendor/cyanogen/products/cyanogen_nintendo3ds.mk" \
   "$CM7_DIR/vendor/cyanogen/products/"
ls -l "$CM7_DIR/vendor/cyanogen/products/cyanogen_nintendo3ds.mk" | sed 's/^/    /'

# common.mk ships with CM7.2 and references two things this tree does not have:
#   * a `GoogleSearch` module (CM7 expects a vendor/google tree, which we do not
#     sync) -> drop it from the no-Google branch (Provision stays);
#   * vendor/cyanogen/proprietary/RomManager.apk (proprietary, not in git) ->
#     drop the PRODUCT_COPY_FILES line.
# Both make the build fail at "no rule to make target" otherwise.  Idempotent.
python3 - "$CM7_DIR/vendor/cyanogen/products/common.mk" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
s2 = s.replace("        Provision \\\n        GoogleSearch\n", "        Provision\n")
s2 = s2.replace(
    "PRODUCT_COPY_FILES +=  \\\n"
    "    vendor/cyanogen/proprietary/RomManager.apk:system/app/RomManager.apk \\\n",
    "# [3ds] RomManager.apk not present (no proprietary blobs)\n")
if s2 != s:
    p.write_text(s2)
    print("    common.mk: dropped GoogleSearch / RomManager")
else:
    print("    common.mk: already patched")
PY

say "3/4  libhardware_legacy Wi-Fi HAL (built-in ath6kl)"
# The 3DS driver is built into zImage, so the stock HAL's insmod() always fails
# and WifiService reports "Failed to load Wi-Fi driver" (Settings -> "Error").
python3 "$REPO/port/scripts/fix-cm7-wifi.py" "$CM7_DIR"

# ...and the WEXT wpa_supplicant must not use the TI combo scan nor treat the
# missing vendor SIOCSIWPRIV command set as a hung driver (that made
# WifiStateTracker power-cycle Wi-Fi in an on/off loop).
python3 "$REPO/port/scripts/fix-cm7-wpa-wext.py" "$CM7_DIR"

say "4/4  register the lunch combo"
VS="$CM7_DIR/vendor/cyanogen/vendorsetup.sh"
if [ -f "$VS" ] && ! grep -q "cyanogen_nintendo3ds" "$VS"; then
	printf 'add_lunch_combo cyanogen_nintendo3ds-eng\n' >> "$VS"
	echo "    appended to vendor/cyanogen/vendorsetup.sh"
else
	echo "    already registered (or vendorsetup.sh absent)"
fi

echo
echo "==> done.  Build with:"
echo "      cd $CM7_DIR"
echo "      . build/envsetup.sh"
echo "      make droidcore -j2"
echo "      (NOT brunch nintendo3ds — TARGET_NO_KERNEL=true; see docs/CM7.md)"
