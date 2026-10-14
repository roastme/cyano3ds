#!/bin/bash
# mksd.sh - assemble the SD card contents for Android on the Nintendo 3DS
#
# This never touches a real SD card: it builds a directory tree that you copy
# to the root of the SD card's FAT32 partition.  Keeping the existing FAT32
# partition means no repartitioning and no data loss - the Android /system and
# /data are ext4 *image files* that Linux mounts through a loop device
# (CONFIG_BLK_DEV_LOOP is enabled in the port's defconfig).
#
# Usage: mksd.sh [out-dir] [--with-android]
#   <out-dir>        default: <repo>/out/sd
#   --with-android   also create android/system.img and android/data.img
#                    (empty ext4 images for now; M3 fills /system)
#
# Resulting layout on the SD card:
#
#   /linux/zImage                   kernel
#   /linux/nintendo3ds_ktr.dtb      New 3DS / New 3DS XL / New 2DS XL
#   /linux/arm9linuxfw.bin          ARM9 firmware (SD + PXI/virtio bridge)
#   /linux/initramfs.cpio.gz        bring-up userspace (max 8 MiB!)
#   /Cyano3DS.firm                pick this in fastboot3DS
#   /android/system.img             Android /system (ext4, loop-mounted)
#   /android/data.img               Android /data
#   /ANDROID-3DS-README.txt         these instructions, on the card
#
# The FIRM deliberately keeps the file name `Cyano3DS.firm` so it stands out
# in fastboot3DS' file browser (Boot setup... -> Setup [slot N] -> Select firm).

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DIST="$REPO/out"
OUT=""
WITH_ANDROID=0

for a in "$@"; do
	case "$a" in
	--with-android) WITH_ANDROID=1 ;;
	*) OUT="$a" ;;
	esac
done
OUT="${OUT:-$REPO/out/sd}"
# The first argument is the destination, never the build dir (C-18).
if [ "$(realpath -m "$OUT")" = "$(realpath -m "$DIST")" ]; then
	echo "mksd.sh: destination '$OUT' is the build output dir ($DIST)." >&2
	echo "         Pass another directory, or omit it to use $REPO/out/sd." >&2
	exit 2
fi

ANDROID_FLAVOR="${ANDROID_FLAVOR:-cm7}"
case "$ANDROID_FLAVOR" in
	cm7)         ANDROID_NAME="Android 2.3.7 (CyanogenMod 7.2)" ;;
	gingerbread) ANDROID_NAME="Android 2.3.3 (Gingerbread)" ;;
	froyo)       ANDROID_NAME="Android 2.2 (Froyo)" ;;
	*)           ANDROID_FLAVOR="cm7"; ANDROID_NAME="Android 2.3.7 (CyanogenMod 7.2)" ;;
esac

say() { printf '\n==> %s\n' "$*"; }
need() { [ -f "$1" ] || { echo "missing $1 (run the build scripts first)" >&2; exit 1; }; }

need "$DIST/zImage"
need "$DIST/nintendo3ds_ktr.dtb"
need "$DIST/arm9linuxfw.bin"
need "$DIST/initramfs.cpio.gz"
need "$DIST/Cyano3DS.firm"

say "creating $OUT"
mkdir -p "$OUT/linux"

cp "$DIST/zImage"                "$OUT/linux/zImage"
cp "$DIST/nintendo3ds_ktr.dtb"   "$OUT/linux/nintendo3ds_ktr.dtb"
cp "$DIST/arm9linuxfw.bin"       "$OUT/linux/arm9linuxfw.bin"
cp "$DIST/initramfs.cpio.gz"     "$OUT/linux/initramfs.cpio.gz"
cp "$DIST/Cyano3DS.firm"       "$OUT/Cyano3DS.firm"

if [ "$WITH_ANDROID" = 1 ] || [ -f "$REPO/out/android/system.img" ]; then
	say "Android disk images"
	mkdir -p "$OUT/android"
	for img in system data; do
		src="$REPO/out/android/$img.img"
		if [ -f "$src" ]; then
			cp "$src" "$OUT/android/$img.img"
		elif [ "${ALLOW_EMPTY_ANDROID:-0}" = 1 ]; then
			# Empty images boot to a blank /system.  Only for testing the card layout.
			echo "    WARNING: $src missing, writing an EMPTY $img.img (ALLOW_EMPTY_ANDROID=1)" >&2
			[ -f "$OUT/android/$img.img" ] || {
				truncate -s 192M "$OUT/android/$img.img"
				mkfs.ext4 -q -L "$img" -F "$OUT/android/$img.img"
			}
		else
			echo "mksd.sh: $src is missing: no Android image to put on the card." >&2
			echo "         Build it with: bash port/scripts/cm7-rootless.sh package" >&2
			echo "         (or set ALLOW_EMPTY_ANDROID=1 to stage an empty placeholder)." >&2
			exit 1
		fi
	done
	ls -lh "$OUT/android" | sed 's/^/  /'
fi

cat > "$OUT/ANDROID-3DS-README.txt" <<TXT
$ANDROID_NAME on the Nintendo 3DS - SD card contents
=========================================================

Files here are loaded by firm_linux_loader (Cyano3DS.firm) from this card.

To boot it from fastboot3DS:
  1. Power on holding HOME to enter the fastboot3DS menu.
  2. Either
       "Boot setup..." -> "Setup [slot N]..." -> "Select [slot N] firm"
         and pick /Cyano3DS.firm, then optionally
         "Set [slot N] keycombo" (hold the keys for 3 seconds), or
     simply
       "Boot from file..." -> /Cyano3DS.firm
  3. Press B twice and choose "Continue boot".
  4. Hold HOME at power-on if you want the menu back; select any other slot
     (for example your Luma3DS boot.firm) to return to the normal system.

What you should see
  * both screens go glitchy for a few seconds - this is normal;
  * the TOP screen becomes the Linux console;
  * the BOTTOM screen runs the display test, then the bring-up log, and
    counts the dexopt progress ("[ 12/79] ...") during the first boot;
  * the TOP screen then counts the packages as system_server scans them;
  * finally the Android launcher (Launcher2/ADW) appears on the BOTTOM screen.

FIRST BOOT
  * The first boot takes ~2-3 minutes on a 3DS (the framework dexopt + the
    PackageManager scan).  The screen is mostly idle/black during that time.
  * Do NOT power off while the TOP screen keeps counting.  When the count
    stops and "PowerManagerService: system ready!" appears, the launcher is
    about to show.  A card file CYANO3DS/BOOT-OK.txt is written then.
  * The dexopt result is now cached on the card (android/dalvik.tar), so
    later boots skip it and are much faster.

The boot log is written to CYANO3DS/ on this card (init.log, kmsg.log,
android-log.txt, late-diag.txt, ...).

Safety
  * This does not modify the NAND. It only boots a payload from the SD card.
  * Make a NAND backup first anyway (fastboot3DS: "Miscellaneous..." ->
    "Backup NAND").
  * Keep a slot pointing at your normal boot.firm so you can always get back.
TXT

say "done"
find "$OUT" -type f | sort | while read -r f; do
	printf '  %-45s %8s bytes\n' "${f#$OUT/}" "$(stat -c%s "$f")"
done
