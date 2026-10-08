#!/bin/bash
# cleanup-cm7-tree.sh - remove stale device trees and vendor products that
#                      break the CM7.2 product config after a full `repo sync`.
#
# After a full `repo sync`, the working trees of every vendor device are on
# disk.  `build/core/config.mk` resolves `TARGET_DEVICE` with the glob
# `device/*/$(TARGET_DEVICE)/BoardConfig.mk`, and `product_config.mk` globs
# `vendor/*/products/*.mk` — so stale trees make the build pick up products
# and devices that are not ours.  This script removes the stale trees and
# trims the vendor product list to just `cyanogen_nintendo3ds` (+ generic).
#
# Usage:  bash port/scripts/cleanup-cm7-tree.sh /path/to/cm7
#
# Run this AFTER `repo sync` and BEFORE `apply-cm7-device.sh`.

set -euo pipefail

CM7_DIR="${1:-}"
if [ -z "$CM7_DIR" ]; then
	echo "usage: $0 /path/to/cm7" >&2
	exit 1
fi
if [ ! -d "$CM7_DIR/device" ]; then
	echo "error: $CM7_DIR/device not found — is this a CM7 tree?" >&2
	exit 1
fi

cd "$CM7_DIR"

echo "==> removing stale device trees"
# Keep only device/nintendo (ours).  Remove every other vendor tree.
for d in device/*/; do
	[ -d "$d" ] || continue
	case "$d" in
		device/nintendo/) echo "    keep $d" ;;
		*)
			echo "    rm -rf $d"
			rm -rf "$d"
			;;
	esac
done

echo "==> trimming vendor/cyanogen/products to nintendo3ds + generic"
PROD_DIR="vendor/cyanogen/products"
if [ -d "$PROD_DIR" ]; then
	for f in "$PROD_DIR"/*.mk; do
		[ -f "$f" ] || continue
		b=$(basename "$f")
		# Keep the port's product + the generic fallback, AND the common_*.mk /
		# themes*.mk files that cyanogen_nintendo3ds.mk inherits (and that
		# apply-cm7-device.sh patches).  Deleting them breaks the build.
		case "$b" in
			cyanogen_nintendo3ds.mk|generic.mk|common.mk|common_full.mk|common_full_no_themes.mk|common_versions.mk|themes.mk|themes_common.mk)
				echo "    keep $b"
				;;
			*)
				echo "    rm -f $b"
				rm -f "$f"
				;;
		esac
	done
fi

echo "==> trimming AndroidProducts.mk"
AP="vendor/cyanogen/AndroidProducts.mk"
if [ -f "$AP" ]; then
	# Keep only the nintendo3ds + generic product lines.
	grep -v 'products/.*\.mk' "$AP" > "$AP.tmp" || true
	printf 'PRODUCT_MAKEFILES := \\\n    $(LOCAL_DIR)/cyanogen_nintendo3ds.mk \\\n    $(LOCAL_DIR)/generic.mk\n' >> "$AP.tmp"
	mv "$AP.tmp" "$AP"
	echo "    trimmed $AP"
fi

echo "==> trimming vendorsetup.sh"
VS="vendor/cyanogen/vendorsetup.sh"
if [ -f "$VS" ]; then
	cat > "$VS" <<'EOF'
add_lunch_combo cyanogen_nintendo3ds-userdebug
add_lunch_combo generic-userdebug
EOF
	echo "    trimmed $VS"
fi

echo "==> done.  Now run: bash port/scripts/apply-cm7-device.sh $CM7_DIR"
