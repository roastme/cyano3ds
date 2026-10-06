#!/bin/bash
# build-gingerbread.sh - assemble the Android 2.3.3 (Gingerbread / GRI34) userspace
#                        for the 3DS
#
# This is the Gingerbread sibling of build-froyo.sh.  It keeps the same
# "prebuilt SDK image, no source build" route, because the API-10 SDK platform
# zip is an armeabi (ARMv5TE) build.
#
#   1. extract the Gingerbread system.img from android-2.3.3_r02-linux.zip
#      (yaffs2),
#   2. apply the port's overrides to that tree (build.prop, keylayout, ...),
#   3. pack it into out/android/system.img (ext4) and create out/android/data.img,
#   4. stage the Android boot stage (init + patched init.rc + default.prop) in
#      out/gingerbread-init/ for mkinitramfs.sh (ANDROID_FLAVOR=gingerbread).
#
# Why Gingerbread's own graphics stack is used unchanged:
#   The API-10 SDK image ships the generic fbdev gralloc
#   (lib/hw/gralloc.default.so, RGB565-first, 2 buffers, page flip) and the
#   software GLES1 driver (lib/egl/libGLES_android.so) - exactly the two files
#   the Froyo port already drives with ctr_lcd_fb.  With no egl.cfg the EGL
#   loader uses the default tag "android" and opens that library.
#
# Kernel: unchanged.  Gingerbread 32-bit binder is protocol 7, the same as
# Froyo/Donut, so the port's kernel is already correct.
#
# Usage: build-gingerbread.sh [--force-extract]
# Inputs:  src/android-2.3.3_r02-linux.zip
# Outputs: out/android/{system.img,data.img}, out/gingerbread-init/*

set -euo pipefail

LEGACY="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$LEGACY/.." && pwd)"
SRC="$REPO/src"
OUT="$REPO/out"
PORT="$REPO/port"
WORK=/root/p3ds/src
FWORK="$WORK/gingerbread"
SYSDIR="$WORK/gingerbread-system"
SDK="$SRC/android-2.3.3_r02-linux.zip"
IMAGES="$FWORK/android-2.3.3_r02-linux/images"
RAMDISK="$FWORK/ramdisk"
# Card apps + native libs to bake into the read-only /system image (see
# docs/INSTALL.md for why they are system apps).  Override with APPS_DIR=...
APPS_DIR="${APPS_DIR:-$REPO/sd-staging/CYANO3DS}"
SYSTEM_IMG_MB=256
DATA_IMG_MB=256

say() { printf '\n==> %s\n' "$*"; }

# replace-or-append a property in a build.prop style file.  For "ro." keys a
# duplicate later in the file does NOT win (the property service refuses to
# change an already-set read-only property), so a plain append is wrong.
setprop_bp() {
	local f="$1" k="$2" v="$3"
	if grep -q "^${k}=" "$f"; then
		sed -i "s|^${k}=.*|${k}=${v}|" "$f"
	else
		printf '%s=%s\n' "$k" "$v" >> "$f"
	fi
}

# ---------------------------------------------------------------------------
say "1/5  Gingerbread images from the SDK platform zip"
# ---------------------------------------------------------------------------
[ -f "$SDK" ] || {
	echo "missing $SDK" >&2
	echo "  curl -L -o '$SDK' https://dl.google.com/android/repository/android-2.3.3_r02-linux.zip" >&2
	exit 1; }

mkdir -p "$FWORK"
if [ ! -f "$IMAGES/system.img" ]; then
	unzip -o -q "$SDK" -d "$FWORK" \
		"android-2.3.3_r02-linux/images/*" "android-2.3.3_r02-linux/build.prop" \
		|| unzip -o -q "$SDK" -d "$FWORK"
fi
ls -l "$IMAGES/system.img" "$IMAGES/ramdisk.img" | sed 's/^/    /'

if [ "${1:-}" = "--force-extract" ] || [ ! -d "$SYSDIR/lib" ]; then
	rm -rf "$SYSDIR"
	python3 "$LEGACY/yaffs2-extract.py" "$IMAGES/system.img" "$SYSDIR"
fi

# the SDK ramdisk holds the real Gingerbread init + init.rc + default.prop
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
	# ro.* values: replace in place when the generic build already defines them.
	setprop_bp "$PROP" ro.product.model "Nintendo 3DS"
	setprop_bp "$PROP" ro.product.brand nintendo
	setprop_bp "$PROP" ro.product.name nintendo3ds
	setprop_bp "$PROP" ro.product.device nintendo3ds
	setprop_bp "$PROP" ro.product.board nintendo3ds
	setprop_bp "$PROP" ro.product.manufacturer nintendo
	# the generic SDK build ships an invalid "ldpi" for the locale language
	setprop_bp "$PROP" ro.product.locale.language en
	setprop_bp "$PROP" ro.product.locale.region US
	# 320x240 landscape bottom screen, QVGA class (same density as the HTC Tattoo)
	setprop_bp "$PROP" ro.sf.lcd_density 120
	# software GLES 1.x (libGLES_android / pixelflinger): no GPU driver on the 3DS.
	# Launcher2 falls back to its 2D drawer without GLES2.
	setprop_bp "$PROP" ro.opengles.version 65536
	setprop_bp "$PROP" ro.hardware nintendo3ds
	# no telephony, no camera, no network, no location
	setprop_bp "$PROP" ro.telephony.default_network 0
	setprop_bp "$PROP" ro.com.google.locationfeatures 0
	setprop_bp "$PROP" ro.com.google.networklocation 0
	# the SDK image is an eng build with CheckJNI on; turn the overhead off
	setprop_bp "$PROP" ro.kernel.android.checkjni 0
	# no network to run a setup wizard against
	setprop_bp "$PROP" ro.setupwizard.mode DISABLED
	# 64 MiB Dalvik heap: the 3DS has 256 MiB of FCRAM, and the Gingerbread JIT
	# likes the room.
	setprop_bp "$PROP" dalvik.vm.heapsize 64m
	setprop_bp "$PROP" dalvik.vm.heapgrowthlimit 64m
	setprop_bp "$PROP" dalvik.vm.stack-trace-file /data/anr/traces.txt
	echo "    build.prop: applied the 3DS overrides (in place for existing keys)"
else
	echo "    build.prop: already patched"
fi

# The 3DS Y button is Linux scancode 15 (KEY_TAB).  Gingerbread's qwerty.kl
# already has it, but keep the guard so a future layout change cannot break it.
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
		# Never overlay Gingerbread's own graphics libraries: the card's
		# libEGL.so is a Donut prelink hack and would break the EGL loader.
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
STAGE="$OUT/gingerbread-init"
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$RAMDISK/init" "$STAGE/init"

# ---------------------------------------------------------------------------
# Patch Gingerbread's init binary: "tmpfs" -> "tmpfx"
#
# Gingerbread's init does `mount("tmpfs", "/dev", "tmpfs", 0, "mode=0755")` in
# main().  Our initramfs /init has already built a tmpfs /dev by hand (the
# kernel's devtmpfs refuses userspace mknod, and the port needs the /dev/log/*
# names, the fb symlink and world-accessible ashmem/binder).  Making that one
# mount fail with ENODEV keeps our /dev.  Same length, so it is an in-place byte
# patch and Gingerbread's init tolerates the failed mount.
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

# ---------------------------------------------------------------------------
# init.rc: the authentic Gingerbread one, patched for a device whose /system,
# /data and /cache are already mounted by the port's initramfs (and for a kernel
# with no yaffs2 / apanic / Android wakelocks / Bluetooth).
# ---------------------------------------------------------------------------
python3 - "$RAMDISK/init.rc" "$STAGE/init.rc" <<'PY'
import re, sys, pathlib

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = src.read_text()

# 1. disable the storage mounts: our initramfs already mounted them from the
#    ext4 image files on the SD card.
def comment(m):
    return "# [3ds] " + m.group(0)
# [ \t] not \s: \s would match the newline + indentation of a preceding
# blank line, so the "# [3ds] " prefix would land on the blank line and leave
# the command itself uncommented.
s = re.sub(r'(?m)^[ \t]*mount[ \t]+yaffs2[ \t]+.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+yfs2[ \t]+.*$', comment, s)

# 2. keep the initramfs root writable (the port's /init writes there)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+rootfs[ \t]+rootfs[ \t]+/[ \t]+ro[ \t]+remount[ \t]*$', comment, s)

# 3. no apanic on this kernel
s = re.sub(r'(?m)^[ \t]*(copy|write)[ \t]+/proc/apanic.*$', comment, s)

# 4. services that are pointless or harmful on the 3DS:
#      servicemanager - must run as root here (as 'system' it never becomes the
#                       binder context manager on mainline binder) and must not
#                       be 'critical' (its crash-loop would reboot the console)
#      media          - run as root during bring-up (logwrap can log; rules out
#                       a uid problem stalling AudioFlinger/system_server)
#      dbus/bluetoothd/sdptool helpers - there is no Bluetooth hardware
#      flash_recovery - no recovery partition to patch
#      ueventd        - the port's initramfs hand-builds /dev before Android
#                       starts (devtmpfs refuses userspace mknod, and the port
#                       needs specific names/perms).  Stock GB makes ueventd a
#                       'critical' service; leaving it enabled with no
#                       /sbin/ueventd would make init reboot in a loop.
#    Walk the service blocks with a small state machine.
DROP_SERVICES = {"ueventd", "dbus", "bluetoothd", "hfag", "hsag", "opush",
                 "pbap", "flash_recovery"}
lines = s.splitlines(keepends=True)
out, svc, dropped = [], None, []
for ln in lines:
    if ln.startswith('service '):
        svc = ln.split()[1]
        if svc in DROP_SERVICES:
            out.append('# [3ds] ' + ln)
            dropped.append('%s: disabled' % svc)
            continue
    elif ln and not ln[0].isspace() and not ln.startswith('#'):
        # a new top-level statement ends the current service block
        svc = None
    if svc == 'servicemanager' and ln.strip() == 'critical':
        out.append('# [3ds] ' + ln)
        dropped.append('servicemanager: critical removed')
        continue
    if svc == 'servicemanager' and ln.strip() == 'user system':
        out.append('# [3ds] ' + ln)
        dropped.append('servicemanager: user system -> root')
        continue
    if svc == 'media' and ln.strip() == 'user media':
        out.append('# [3ds] ' + ln)
        dropped.append('media: user media -> root')
        continue
    if svc in DROP_SERVICES:
        # an indented property line of a dropped service block; if the service
        # line itself was already dropped svc would be that name, so only reach
        # here for the *first* line (handled above) - keep as comment for safety
        out.append('# [3ds] ' + ln)
        continue
    out.append(ln)
s = ''.join(out)

# 5. don't try to start the services we just disabled (init logs an error but
#    this keeps the console clean)
s = re.sub(r'(?m)^[ \t]*start[ \t]+(ueventd|dbus|bluetoothd)[ \t]*$', comment, s)

# 5b. Gingerbread's get_first_command() does not stop at the list sentinel when
#     an action has no commands: it treats the sentinel as a command and calls
#     its NULL func pointer, so init jumps to address 0 and dies with SIGSEGV
#     (the port saw android-init: pc=0 lr=0xc923).  Commenting out the only
#     command of `on early-init` (start ueventd) and of `on fs` (the yaffs2
#     mounts) emptied those actions, so drop any action with no commands.
def _is_header(ln):
    return (ln.startswith('on ') or ln.startswith('service ') or
            ln.startswith('import '))
def _is_cmd(ln):
    t = ln.strip()
    return bool(t) and not t.startswith('#') and not _is_header(t)
_raw = s.split('\n')
_out, _i, _n = [], 0, len(_raw)
while _i < _n:
    _ln = _raw[_i]
    if _ln.startswith('on '):
        _j = _i + 1
        _ncmds = 0
        while _j < _n and not _is_header(_raw[_j]):
            if _is_cmd(_raw[_j]):
                _ncmds += 1
            _j += 1
        if _ncmds:
            _out.extend(_raw[_i:_j])
        else:
            _out.append('# [3ds] dropped empty action: ' + _ln)
            dropped.append('empty action dropped: ' + _ln)
        _i = _j
    else:
        _out.append(_ln)
        _i += 1
s = '\n'.join(_out)

# 6. append the 3DS additions
s += '''
# ---------------------------------------------------------------------------
# Android-3DS port additions (Gingerbread)
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
print("    init.rc: mounts disabled, services fixed, 3DS section added")
for d in dropped:
    print("    init.rc: %s" % d)
PY

ls -l "$STAGE" | sed 's/^/    /'
echo
echo "==> done: $OUT/android/{system.img,data.img} and $OUT/gingerbread-init/"
