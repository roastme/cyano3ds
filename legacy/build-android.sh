#!/bin/bash
# build-android.sh - assemble the Android 1.6 (Donut) userspace for the 3DS
#
#   1. extract the Donut system.img from the Android 1.6 SDK (yaffs2) into a
#      directory tree (via yaffs2-extract.py - no unyaffs dependency),
#   2. apply the port's overrides to that tree (build.prop for a 320x240
#      screen, no GPU),
#   3. pack it into out/android/system.img (ext4) and create out/android/data.img,
#   4. stage the Android boot stage (init + patched init.rc + default.prop) in
#      out/android-init/ for mkinitramfs.sh to put into the initramfs.
#
# Why the SDK image and not a build from source: it is a real, complete AOSP
# Donut build (bionic + dalvik + framework + software GLES) for ARM, so no 2009
# toolchain needs to be revived.  This is the same route the iDroid project
# (Android on the iPhone 2G/3G) used: prebuilt Android images + a patched
# init.rc + a Linux kernel port.
#
# Usage: build-android.sh [--force-extract]
# Inputs:  src/android-sdk-linux_x86-1.6_r1.tgz   (SDK archive, 238 MB)
# Outputs: out/android/system.img, out/android/data.img, out/android-init/*

set -euo pipefail

LEGACY="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$LEGACY/.." && pwd)"
SRC="$REPO/src"
OUT="$REPO/out"
PORT="$REPO/port"
WORK=/root/p3ds/src
SYSDIR="$WORK/donut-system"
SDK="$SRC/android-sdk-linux_x86-1.6_r1.tgz"
IMAGES="$WORK/donut-sdk/android-sdk-linux_x86-1.6_r1/platforms/android-1.6/images"
SYSTEM_IMG_MB=128
DATA_IMG_MB=192

say() { printf '\n==> %s\n' "$*"; }

# ---------------------------------------------------------------------------
say "1/4  Donut images from the SDK"
# ---------------------------------------------------------------------------
if [ ! -f "$IMAGES/system.img" ]; then
	[ -f "$SDK" ] || { echo "missing $SDK" >&2; exit 1; }
	mkdir -p "$IMAGES"
	tar xzf "$SDK" -C "$WORK/donut-sdk" \
		android-sdk-linux_x86-1.6_r1/platforms/android-1.6/images
fi
ls -l "$IMAGES/system.img" "$IMAGES/ramdisk.img" | sed 's/^/    /'

if [ "${1:-}" = "--force-extract" ] || [ ! -d "$SYSDIR/lib" ]; then
	rm -rf "$SYSDIR"
	python3 "$LEGACY/yaffs2-extract.py" "$IMAGES/system.img" "$SYSDIR"
fi

# The SDK's libandroid_runtime.so hardcodes the VM heap as "-Xmx16m" instead
# of reading dalvik.vm.heapsize, so zygote always ran on a 16 MiB heap and
# GC'd during framework bootstrap - which is exactly where the flaky ashmem
# mark-stack allocation crashed it.  Same-length in-place string patch.
python3 - "$SYSDIR/lib/libandroid_runtime.so" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
if p.exists():
    d = p.read_bytes()
    if b"-Xmx16m" in d:
        p.write_bytes(d.replace(b"-Xmx16m", b"-Xmx96m"))
        print("    libandroid_runtime.so: VM heap -Xmx16m -> -Xmx96m")
    else:
        print("    libandroid_runtime.so: no -Xmx16m (already patched)")
PY

# The stock libsoundpool.so is prelinked for a fixed address and Dalvik refuses
# to load it, so SoundPool.<clinit> throws and AudioService fails.  Because
# AudioManager.mService is then null, StatusBarPolicy.installIcons() NPEs and
# the status bar layer never exists - which is why SurfaceFlinger never
# composites the bottom screen.  Audio is not needed for the port, so install a
# tiny freestanding no-op stub that satisfies every native the class declares.
if [ -f "$PORT/android-staging/libsoundpool-stub.c" ]; then
	arm-linux-gnueabi-gcc -shared -fPIC -nostdlib -marm -march=armv5te \
		-fno-asynchronous-unwind-tables -fno-unwind-tables \
		-Wl,-z,noexecstack -Wl,-soname,libsoundpool.so -Wl,--hash-style=both \
		-o "$SYSDIR/lib/libsoundpool.so" \
		"$PORT/android-staging/libsoundpool-stub.c"
	echo "    libsoundpool.so: no-op stub installed (audio not needed)"
fi

# the SDK ramdisk holds the real Donut init + init.rc + default.prop
RAMDISK="$WORK/donut-sdk/ramdisk"
if [ ! -f "$RAMDISK/init" ]; then
	mkdir -p "$RAMDISK"
	( cd "$RAMDISK" && gzip -dc "$IMAGES/ramdisk.img" | cpio -idm --quiet )
fi
ls -l "$RAMDISK/init" "$RAMDISK/init.rc" "$RAMDISK/default.prop" | sed 's/^/    /'

# ---------------------------------------------------------------------------
say "2/4  port overrides for /system"
# ---------------------------------------------------------------------------
PROP="$SYSDIR/build.prop"
if ! grep -q "nintendo3ds" "$PROP"; then
	cat >> "$PROP" <<'PROPS'
# --- Android-3DS port overrides ---
ro.product.model=Nintendo 3DS
ro.product.brand=nintendo
ro.product.name=nintendo3ds
ro.product.device=nintendo3ds
ro.product.board=nintendo3ds
# 320x240 landscape bottom screen, QVGA class (same density as the HTC Tattoo)
ro.sf.lcd_density=120
# software GLES 1.x (libagl / pixelflinger): no GPU driver exists for the 3DS
ro.opengles.version=65536
ro.hardware=nintendo3ds
# no telephony, no camera, no sensors we can drive yet
ro.telephony.default_network=0
ro.com.google.locationfeatures=0
ro.com.google.networklocation=0
dalvik.vm.heapsize=192m
dalvik.vm.stack-trace-file=/data/anr/traces.txt
PROPS
	echo "    build.prop: appended the 3DS overrides"
else
	echo "    build.prop: already patched"
fi

# ---------------------------------------------------------------------------
say "3/4  packing /system and /data"
# ---------------------------------------------------------------------------
mkdir -p "$OUT/android"
rm -f "$OUT/android/system.img" "$OUT/android/data.img"

# mkfs writes the ext4 image and populates it from the tree in one go, so no
# loop mount and no root-owned mountpoint churn.
mke2fs -q -t ext4 -O ^has_journal -b 4096 -L system -d "$SYSDIR" \
	"$OUT/android/system.img" "${SYSTEM_IMG_MB}M"
printf '%-40s %s\n' "system.img" "$(du -h "$OUT/android/system.img" | cut -f1)"

mke2fs -q -t ext4 -O ^has_journal -b 4096 -L data -F \
	"$OUT/android/data.img" "${DATA_IMG_MB}M"
printf '%-40s %s\n' "data.img" "$(du -h "$OUT/android/data.img" | cut -f1)"

# ---------------------------------------------------------------------------
say "4/4  Android boot stage (init + patched init.rc)"
# ---------------------------------------------------------------------------
STAGE="$OUT/android-init"
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$RAMDISK/init" "$STAGE/init"

# ---------------------------------------------------------------------------
# Patch Donut's init binary: "tmpfs" -> "tmpfx"
#
# init does `mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755")` before calling
# log_init().  That hides devtmpfs, so /dev/kmsg no longer exists and *every*
# message init logs (INFO/ERROR -> klog_write -> /dev/kmsg) is silently thrown
# away - which is why the first Android boots showed nothing at all between
# "mounted /system" and the kernel panic.
#
# Patching the filesystem-type string makes that one mount fail with ENODEV, so
# /dev stays as devtmpfs: init's logging works, and /dev/{kmsg,log/*,fb0,fb1,
# null,zero} stay where Android's userspace expects them.  Same length, so it is
# a plain in-place byte patch; devtmpfs is a ramfs and still allows the mknod/
# symlink/chmod commands Android's init.rc issues afterwards.
# ---------------------------------------------------------------------------
python3 - "$STAGE/init" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
d = bytearray(p.read_bytes())
n = d.count(b"tmpfs")
if n:
    d = bytearray(bytes(d).replace(b"tmpfs", b"tmpfx"))
    p.write_bytes(bytes(d))
print(f"    init: patched {n} 'tmpfs' string(s) -> 'tmpfx' (keeps /dev a devtmpfs)")
PY
file "$STAGE/init" | sed 's/^/    /'
# default.prop plus our additions
cp "$RAMDISK/default.prop" "$STAGE/default.prop"
cat >> "$STAGE/default.prop" <<'PROPS'
ro.sf.lcd_density=120
ro.hardware=nintendo3ds
PROPS

# init.rc: take the authentic Donut one and patch it for a device that is not
# mtd/yaffs based:
#   * the yaffs2 mounts must go: our initramfs /init already mounted /system,
#     /data and /cache, and a failing mount in Donut's init is fatal;
#   * add the device nodes and symlinks the port needs (above all
#     /dev/graphics/fb0 -> /dev/fb1, because fb0 is the kernel console on the
#     top screen while Android's screen is fb1);
#   * set the 3DS properties.
python3 - "$RAMDISK/init.rc" "$STAGE/init.rc" <<'PY'
import re, sys, pathlib

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = src.read_text()

# 1. disable the storage mounts (the initramfs did them)
s = re.sub(r'(?m)^(\s*)mount\s+yaffs2\s+.*$', r'\1# [3ds] removed: \g<0>'.replace('# [3ds] removed: ', '# [3ds] '), s)
s = re.sub(r'(?m)^(\s*)mount\s+yfs2\s+.*$', r'\1# [3ds] \g<0>', s)

# 2. append the 3DS section
s += '''
# ---------------------------------------------------------------------------
# Android-3DS port additions
# ---------------------------------------------------------------------------

on init
    # Android 1.6 kernel interfaces.  /dev/binder and /dev/ashmem are misc
    # devices: init's device handling creates them from sysfs, but the
    # permissions are set here so the framework can open them as system.
    chmod 0666 /dev/binder
    chmod 0666 /dev/ashmem
    chown system log /dev/log/main
    chown system log /dev/log/events
    chmod 0662 /dev/log/main
    chmod 0662 /dev/log/events
    # Android's display is the bottom screen, which the kernel exposes as fb1
    # (fb0 is the kernel console on the top screen).
    mkdir /dev/graphics 0755 root root
    symlink /dev/fb1 /dev/graphics/fb0
    symlink /dev/fb0 /dev/graphics/fb1
    chmod 0666 /dev/fb0
    chmod 0666 /dev/fb1
    # input: the touchscreen is a keyboard today (3dstsc-touch virtual
    # keyboard) plus the buttons, both via evdev
    chmod 0660 /dev/input/event0
    chmod 0660 /dev/input/event1
    chmod 0660 /dev/input/event2
    chmod 0660 /dev/input/event3
    chown root input /dev/input/event0
    chown root input /dev/input/event1
    chown root input /dev/input/event2
    chown root input /dev/input/event3

on boot
    setprop ro.sf.lcd_density 120
    setprop ro.hardware nintendo3ds
    # 16 MiB dalvik heap: the 3DS has 256 MiB of FCRAM, and Donut's default is
    # generous for a QVGA device, so keep it modest and leave room for the
    # software renderer's buffers.
    setprop dalvik.vm.heapsize 16m
'''
dst.write_text(s)
print(f"    init.rc: wrote {dst} ({len(s)} bytes, mounts disabled, 3DS section added)")
PY

ls -l "$STAGE" | sed 's/^/    /'
echo
echo "==> done: $OUT/android/{system.img,data.img} and $OUT/android-init/"
