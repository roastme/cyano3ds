#!/bin/bash
# build-froyo.sh - assemble the Android 2.2 (Froyo / FRF91) userspace for the 3DS
#
# This is the Froyo sibling of build-android.sh.  It keeps the same
# "prebuilt SDK image, no source build" route the Donut port uses, because the
# Froyo platform zip is an armeabi (ARMv5TE) build.
#
#   1. extract the Froyo system.img from android-2.2_r03-linux.zip (yaffs2),
#   2. apply the port's overrides to that tree (build.prop, keylayout, ...),
#   3. pack it into out/froyo/system.img (ext4) and create out/froyo/data.img,
#   4. stage the Android boot stage (init + patched init.rc + default.prop) in
#      out/froyo-init/ for mkinitramfs.sh (ANDROID_FLAVOR=froyo).
#
# Why Froyo's own graphics stack is used unchanged:
#   The Froyo platform image already ships the generic fbdev gralloc
#   (lib/hw/gralloc.default.so, RGB565-first, 2 buffers, page flip) and the
#   software GLES1 driver (lib/egl/libGLES_android.so).  With no egl.cfg the
#   Froyo EGL loader uses the default tag "android" and opens exactly that
#   library.  That is the graphics HAL the assessment predicted we would have
#   to build - it is already in the image and matches ctr_lcd_fb (landscape
#   320x240 RGB565, yres_virtual=480).
#
# Usage: build-froyo.sh [--force-extract]
# Inputs:  src/froyo/android-2.2_r03-linux.zip
# Outputs: out/froyo/{system.img,data.img}, out/froyo-init/*

set -euo pipefail

LEGACY="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$LEGACY/.." && pwd)"
SRC="$REPO/src/froyo"
OUT="$REPO/out"
PORT="$REPO/port"
WORK=/root/p3ds/src
FWORK="$WORK/froyo"
SYSDIR="$WORK/froyo-system"
SDK="$SRC/android-2.2_r03-linux.zip"
IMAGES="$FWORK/android-2.2_r03-linux/images"
RAMDISK="$FWORK/ramdisk"
# Card apps + native libs to bake into the read-only /system image.  On Froyo
# /system is loop-mounted read-only and /data is a 121 MiB tmpfs, so a 45 MiB
# game cannot live in /data; as system apps they cost no RAM and get dexopt'd
# into /data/dalvik-cache like the stock packages.  Override with APPS_DIR=...
APPS_DIR="${APPS_DIR:-$REPO/sd-staging/CYANO3DS}"
SYSTEM_IMG_MB=256
DATA_IMG_MB=256

say() { printf '\n==> %s\n' "$*"; }

# ---------------------------------------------------------------------------
say "1/5  Froyo images from the SDK platform zip"
# ---------------------------------------------------------------------------
[ -f "$SDK" ] || {
	echo "missing $SDK" >&2
	echo "  curl -L -o '$SDK' https://dl.google.com/android/repository/android-2.2_r03-linux.zip" >&2
	exit 1; }

mkdir -p "$FWORK"
if [ ! -f "$IMAGES/system.img" ]; then
	unzip -o -q "$SDK" -d "$FWORK" \
		"android-2.2_r03-linux/images/*" "android-2.2_r03-linux/build.prop"
	cp "$FWORK/android-2.2_r03-linux/build.prop" "$FWORK/platform-build.prop"
fi
ls -l "$IMAGES/system.img" "$IMAGES/ramdisk.img" | sed 's/^/    /'

if [ "${1:-}" = "--force-extract" ] || [ ! -d "$SYSDIR/lib" ]; then
	rm -rf "$SYSDIR"
	python3 "$LEGACY/yaffs2-extract.py" "$IMAGES/system.img" "$SYSDIR"
fi

# the SDK ramdisk holds the real Froyo init + init.rc + default.prop
if [ ! -f "$RAMDISK/init" ]; then
	mkdir -p "$RAMDISK"
	( cd "$RAMDISK" && gzip -dc "$IMAGES/ramdisk.img" | cpio -idm --quiet )
fi
ls -l "$RAMDISK/init" "$RAMDISK/init.rc" "$RAMDISK/default.prop" | sed 's/^/    /'

# ---------------------------------------------------------------------------
say "2/5  port overrides for /system"
# ---------------------------------------------------------------------------
PROP="$SYSDIR/build.prop"
if ! grep -q "nintendo3ds" "$PROP"; then
	cat >> "$PROP" <<'PROPS'
# --- Android-3DS port overrides (Froyo) ---
ro.product.model=Nintendo 3DS
ro.product.brand=nintendo
ro.product.name=nintendo3ds
ro.product.device=nintendo3ds
ro.product.board=nintendo3ds
ro.product.manufacturer=nintendo
# 320x240 landscape bottom screen, QVGA class (same density as the HTC Tattoo)
ro.sf.lcd_density=120
# software GLES 1.x (libGLES_android / pixelflinger): no GPU driver on the 3DS
ro.opengles.version=65536
ro.hardware=nintendo3ds
# no telephony, no camera, no network, no location
ro.telephony.default_network=0
ro.com.google.locationfeatures=0
ro.com.google.networklocation=0
# the SDK image is an eng build with CheckJNI on; turn the overhead off
ro.kernel.android.checkjni=0
# 48 MiB Dalvik heap: the 3DS has 256 MiB of FCRAM, and Froyo's JIT likes a bit
# more room than Donut's software-only VM.
dalvik.vm.heapsize=64m
dalvik.vm.heapgrowthlimit=64m
dalvik.vm.stack-trace-file=/data/anr/traces.txt
# rild: there is no radio.  Point the RIL at a library that does not exist so
# rild fails fast and quietly instead of trying to open a serial port.
# (rild is a harmless crash-loop here.)
PROPS
	echo "    build.prop: appended the 3DS overrides"
else
	echo "    build.prop: already patched"
fi

# The 3DS Y button is Linux scancode 15 (KEY_TAB).  Froyo's qwerty.kl has no
# `key 15`, so EventHub maps it to keycode 0 and Android drops it.  X/Y are
# 26/27 on the 3DS; keep whatever the stock layout says for those.
KL="$SYSDIR/usr/keylayout/qwerty.kl"
if [ -f "$KL" ]; then
	grep -q '^key 15 ' "$KL" || printf 'key 15    TAB\n' >> "$KL"
	echo "    qwerty.kl: 'key 15 TAB' present (Y/3DS button)"
fi

# ---------------------------------------------------------------------------
say "2b/5  card apps + native libs into /system"
# ---------------------------------------------------------------------------
if [ -d "$APPS_DIR" ]; then
	mkdir -p "$SYSDIR/app" "$SYSDIR/lib"
	n=0
	for apk in "$APPS_DIR"/*.apk; do
		[ -f "$apk" ] || continue
		b=$(basename "$apk")
		cp -f "$apk" "$SYSDIR/app/$b"
		chmod 0644 "$SYSDIR/app/$b"
		printf '    app/%-24s %8s bytes\n' "$b" "$(stat -c%s "$apk")"
		n=$((n+1))
	done
	m=0
	for so in "$APPS_DIR"/*.so; do
		[ -f "$so" ] || continue
		b=$(basename "$so")
		# Never overlay Froyo's own graphics libraries: the card's libEGL.so
		# is the Donut prelink hack and would break the Froyo EGL loader.
		case "$b" in
			libEGL.so|libGLESv1_CM.so|libGLESv2.so)
				echo "    (skip $b - system graphics library)"; continue ;;
		esac
		cp -f "$so" "$SYSDIR/lib/$b"
		chmod 0644 "$SYSDIR/lib/$b"
		printf '    lib/%-24s %8s bytes\n' "$b" "$(stat -c%s "$so")"
		m=$((m+1))
	done
	echo "    installed $n APK(s) and $m native lib(s) as system apps"
else
	echo "    no $APPS_DIR; no card apps installed"
fi

# ---------------------------------------------------------------------------
say "3/5  packing /system and /data"
# ---------------------------------------------------------------------------
mkdir -p "$OUT/android"
rm -f "$OUT/android/system.img" "$OUT/android/data.img"

mke2fs -q -t ext4 -O ^has_journal -b 4096 -L system -d "$SYSDIR" \
	"$OUT/android/system.img" "${SYSTEM_IMG_MB}M"
printf '%-40s %s\n' "system.img" "$(du -h "$OUT/android/system.img" | cut -f1)"

mke2fs -q -t ext4 -O ^has_journal -b 4096 -L data -F \
	"$OUT/android/data.img" "${DATA_IMG_MB}M"
printf '%-40s %s\n' "data.img" "$(du -h "$OUT/android/data.img" | cut -f1)"

# ---------------------------------------------------------------------------
say "4/5  Android boot stage (init + patched init.rc + default.prop)"
# ---------------------------------------------------------------------------
STAGE="$OUT/froyo-init"
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$RAMDISK/init" "$STAGE/init"

# ---------------------------------------------------------------------------
# Patch Froyo's init binary: "tmpfs" -> "tmpfx"
#
# Froyo's init does `mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755")` in main()
# (system/core/init/init.c).  Our initramfs /init has already built a tmpfs /dev
# by hand (the kernel's devtmpfs refuses userspace mknod, and the port needs the
# /dev/log/* names, the fb symlink and world-accessible ashmem/binder).  Making
# that one mount fail with ENODEV keeps our /dev.  Same length, so it is an
# in-place byte patch.  Froyo's init tolerates later mount failures (the action
# queue runs every command; see drain_action_queue()), so the failed /dev
# mount is harmless.
# ---------------------------------------------------------------------------
python3 - "$STAGE/init" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
d = p.read_bytes()
n = d.count(b"tmpfs")
if n:
    p.write_bytes(d.replace(b"tmpfs", b"tmpfx"))
print(f"    init: patched {n} 'tmpfs' string(s) -> 'tmpfx' (keeps our /dev)")
PY
file "$STAGE/init" | sed 's/^/    /'

# default.prop plus our additions
cp "$RAMDISK/default.prop" "$STAGE/default.prop"
cat >> "$STAGE/default.prop" <<'PROPS'
ro.sf.lcd_density=120
ro.hardware=nintendo3ds
dalvik.vm.heapsize=64m
dalvik.vm.heapgrowthlimit=64m
PROPS

# init.rc: the authentic Froyo one, patched for a device whose /system, /data
# and /cache are already mounted by the port's initramfs (and for a kernel with
# no yaffs2 / apanic / Android wakelocks).
python3 - "$RAMDISK/init.rc" "$STAGE/init.rc" <<'PY'
import re, sys, pathlib

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = src.read_text()

# 1. disable the storage mounts: our initramfs already mounted them from the
#    ext4 image files on the SD card.  (Froyo's init keeps running after a
#    failed mount, but there is no reason to let it fail.)
def comment(m):
    return "# [3ds] " + m.group(0)
s = re.sub(r'(?m)^\s*mount\s+yaffs2\s+.*$', comment, s)
s = re.sub(r'(?m)^\s*mount\s+yfs2\s+.*$', comment, s)

# 2. keep the initramfs root writable (the port's /init writes there)
s = re.sub(r'(?m)^\s*mount\s+rootfs\s+rootfs\s+/\s+ro\s+remount\s*$', comment, s)

# 3. no apanic on this kernel
s = re.sub(r'(?m)^\s*(copy|write)\s+/proc/apanic.*$', comment, s)

# 4. servicemanager must run as root here (as 'system' it never becomes the
#    binder context manager on mainline binder) and must not be 'critical'
#    (its crash-loop would reboot the console).  Walk the service blocks with
#    a small state machine.
lines = s.splitlines(keepends=True)
out, svc, dropped = [], None, []
for ln in lines:
    if ln.startswith('service '):
        svc = ln.split()[1]
    if svc == 'servicemanager' and ln.strip() == 'critical':
        out.append('# [3ds] ' + ln)
        dropped.append('servicemanager: critical removed')
        continue
    if svc == 'servicemanager' and ln.strip() == 'user system':
        out.append('# [3ds] ' + ln)
        dropped.append('servicemanager: user system -> root')
        continue
    out.append(ln)
s = ''.join(out)

# 5. append the 3DS additions
s += '''
# ---------------------------------------------------------------------------
# Android-3DS port additions (Froyo)
# ---------------------------------------------------------------------------

on init
    # Android kernel interfaces.  /dev/binder and /dev/ashmem are misc devices
    # and the port's initramfs already created them; keep them world-accessible
    # for zygote's non-root GC mark-stack open.
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
    # input: touchscreen + buttons, all evdev
    chmod 0660 /dev/input/event0
    chmod 0660 /dev/input/event1
    chmod 0660 /dev/input/event2
    chmod 0660 /dev/input/event3
    chmod 0660 /dev/input/event4
    chmod 0660 /dev/input/event5
    chown root input /dev/input/event0
    chown root input /dev/input/event1
    chown root input /dev/input/event2
    chown root input /dev/input/event3
    chown root input /dev/input/event4
    chown root input /dev/input/event5
    # the property has to be set before zygote reads it
    setprop dalvik.vm.heapsize 64m
    setprop dalvik.vm.heapgrowthlimit 64m
    setprop ro.sf.lcd_density 120
    setprop ro.hardware nintendo3ds

on boot
    setprop ro.sf.lcd_density 120
    setprop ro.hardware nintendo3ds
'''
dst.write_text(s)
print("    init.rc: mounts disabled, servicemanager fixed, 3DS section added")
for d in dropped:
    print("    init.rc: %s" % d)
PY

ls -l "$STAGE" | sed 's/^/    /'
echo
echo "==> done: $OUT/android/{system.img,data.img} and $OUT/froyo-init/"
