#!/bin/bash
# build-sensors-hal.sh - build the device/nintendo3ds sensors HAL on its own.
#
# `make droidcore` builds device/nintendo3ds/sensors into
# $TARGET_OUT/system/lib/hw/sensors.nintendo3ds.so as part of the full CM7
# build.  When the tree's out/ is gone (or was never built here) the card image
# still needs that one library, so this compiles it directly against the CM7
# headers with the tree's own prebuilt toolchain, and links it against the
# bionic libraries taken from an existing system.img.  The result is ABI
# compatible with the platform build: same compiler, same headers,
# -fno-short-enums, and every undefined symbol resolved by the image's libs.
#
# Usage:
#   bash port/scripts/build-sensors-hal.sh                 # -> out/sensors-hal/
#   CM7_DIR=~/p3ds/cm7 SYSTEM_IMG=/path/system.img bash port/scripts/build-sensors-hal.sh
#   bash port/scripts/build-sensors-hal.sh -o /tmp/hal
#
# Then, to install it into an image (debugfs writes to the image in place, so
# work on a copy):
#   cp system.img system.work.img
#   debugfs -w -R "write $OUT/sensors.nintendo3ds.so /lib/hw/sensors.nintendo3ds.so" system.work.img
#   debugfs -w -R "sif /lib/hw/sensors.nintendo3ds.so mode 0100775" system.work.img
#   e2fsck -fn system.work.img

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$REPO/port/userland/device/nintendo3ds/sensors/sensors_nintendo3ds.c"
OUT="$REPO/out/sensors-hal"
SYSTEM_IMG="${SYSTEM_IMG:-/mnt/sdcard/android/system.img}"

# The CM7.2 source tree.  /mnt/Movies/cc/cm7 is the rootless build's fuse2fs
# mount of cm7.img (see /mnt/Movies/cc/build-cm7.sh); ~/p3ds/cm7 is the older
# plain-tree location.
for d in "${CM7_DIR:-}" /mnt/Movies/cc/cm7/cm7-src "$HOME/p3ds/cm7"; do
	[ -n "$d" ] || continue
	[ -d "$d/hardware/libhardware/include/hardware" ] && CM7_DIR="$d" && break
done
[ -n "${CM7_DIR:-}" ] || {
	echo "error: no CM7 source tree found" >&2
	echo "  set CM7_DIR= (see AGENTS.md: /mnt/Movies/cc)" >&2
	exit 1; }

TC="$CM7_DIR/prebuilt/linux-x86/toolchain/arm-eabi-4.4.3"
CC="$TC/bin/arm-eabi-gcc"
[ -x "$CC" ] || { echo "error: missing $CC" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
	-o) OUT="$2"; shift 2 ;;
	*)  echo "usage: $0 [-o outdir]" >&2; exit 1 ;;
	esac
done

mkdir -p "$OUT"

# ---------------------------------------------------------------------------
# bionic libraries to link against.  They are only used as link inputs (SONAMEs
# go into DT_NEEDED, the code stays in the image), so taking them from the
# image the HAL will run on is the exact target ABI.
# ---------------------------------------------------------------------------
LIBS="$OUT/libs"
if [ ! -f "$LIBS/libc.so" ]; then
	echo "==> extracting bionic libs from $SYSTEM_IMG"
	[ -f "$SYSTEM_IMG" ] || { echo "error: no $SYSTEM_IMG" >&2; exit 1; }
	mkdir -p "$LIBS"
	for l in libc.so libm.so liblog.so libcutils.so libstdc++.so; do
		debugfs -R "dump /lib/$l $LIBS/$l" "$SYSTEM_IMG" >/dev/null 2>&1 ||
			{ echo "error: /lib/$l not in the image" >&2; exit 1; }
	done
fi

# ---------------------------------------------------------------------------
# libgcc's soft-float compare helpers.  bionic exports __aeabi_fcmple?  No:
# libc.so has __aeabi_fcmpun only, the compare helpers normally come from
# libgcc.a linked into the module.  `ld -shared` does not pull archive members
# for undefined references, so the one member that is needed is linked in
# explicitly (it has no undefined symbols of its own).
# ---------------------------------------------------------------------------
GCCLIB="$TC/lib/gcc/arm-eabi/4.4.3/libgcc.a"
OBJ="$OUT/obj"
mkdir -p "$OBJ"
( cd "$OBJ" && ar x "$GCCLIB" _arm_cmpsf2.o )

CFLAGS=(
	-c -fPIC -O2 -Wall -Wno-unused-parameter
	-fno-short-enums		# matches the platform build
	-nostdlib
	-I"$CM7_DIR/hardware/libhardware/include"
	-I"$CM7_DIR/system/core/include"
	-I"$CM7_DIR/bionic/libc/include"
	-I"$CM7_DIR/bionic/libc/arch-arm/include"
	-I"$CM7_DIR/bionic/libc/kernel/common"
	-I"$CM7_DIR/bionic/libc/kernel/arch-arm"
	-I"$CM7_DIR/bionic/libm/include"
)

echo "==> compiling sensors_nintendo3ds.c"
"$CC" "${CFLAGS[@]}" -o "$OBJ/hal.o" "$SRC"

echo "==> linking"
"$CC" -shared -nostdlib -Wl,--hash-style=sysv \
	-o "$OUT/sensors.nintendo3ds.so" \
	"$OBJ/hal.o" "$OBJ/_arm_cmpsf2.o" \
	-L"$LIBS" -llog -lcutils -lm -lc

# ---------------------------------------------------------------------------
# Checks.  hw_get_module() dlsyms "HMI" (hardware.c: HAL_MODULE_INFO_SYM_AS_STR),
# and an undefined symbol bionic cannot resolve is a crash on first call, so
# both are verified here rather than on the console.
# ---------------------------------------------------------------------------
fail=0
nm -D "$OUT/sensors.nintendo3ds.so" | grep -q " D HMI$" ||
	{ echo "error: no HMI symbol exported" >&2; fail=1; }
for l in libc.so libm.so liblog.so libcutils.so; do
	readelf -d "$OUT/sensors.nintendo3ds.so" | grep -q "\[$l\]" ||
		{ echo "error: DT_NEEDED is missing $l" >&2; fail=1; }
done
und=$(readelf --dyn-syms "$OUT/sensors.nintendo3ds.so" |
	awk '$7=="UND"{print $8}' | sort -u)
# One list of everything the image's libraries export, then a plain file grep:
# `nm | grep -q` would exit the pipe early and trip pipefail.
: > "$OUT/syms.txt"
for l in "$LIBS"/*.so; do
	nm -D "$l" 2>/dev/null | awk '{print $NF}' >> "$OUT/syms.txt" || true
done
sort -u -o "$OUT/syms.txt" "$OUT/syms.txt"
for s in $und; do
	grep -qxF "$s" "$OUT/syms.txt" ||
		{ echo "error: unresolved symbol $s" >&2; fail=1; }
done
[ "$fail" = 0 ] || exit 1

printf '%-28s %8s bytes  md5 %s\n' sensors.nintendo3ds.so \
	"$(stat -c%s "$OUT/sensors.nintendo3ds.so")" \
	"$(md5sum "$OUT/sensors.nintendo3ds.so" | cut -d' ' -f1)"
echo "    $(printf '%s ' $und | wc -w) undefined symbols, all resolved by the image's libs"
