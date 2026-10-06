#!/bin/bash
# build-cm7.sh - assemble the CyanogenMod 7.2 (Android 2.3.7, API 10) userspace
#                for the Nintendo 3DS.
#
# Route: "kang" an existing ARMv6 CM7.2 ROM and replace its device-specific
# parts with the 3DS's.  This is the fast, hardware-proven route.  The kernel
# does not change: CM7 is API 10
# and its graphics path is the same Gingerbread software stack
# (libGLES_android + generic gralloc.default) the working 2.3.3 build already
# drives through ctr_lcd_fb.
#
# The upstream ROM used is cm-7.2.0-cooper.zip (Samsung Galaxy Ace, msm7227 /
# ARMv6+VFP).  Its CPU is the same class as the 3DS ARM11 (ARMv6K + VFPv2), so
# the armeabi-v6l userspace binaries run natively.
#
#   1. extract system/ from the ROM zip,
#   2. reconstruct the exec bits and the busybox/toolbox symlinks that the
#      ROM's updater-script normally creates (the zip stores no unix metadata),
#   3. drop the cooper/adreno graphics blobs and fall back to gralloc.default
#      + libGLES_android (delete egl.cfg -> EGL default tag "android"),
#   4. apply the 3DS build.prop / keylayout overrides and bake the card APKs,
#   5. pack out/android/{system,data}.img and stage out/cm7-init/ (init +
#      patched init.rc + default.prop) for mkinitramfs.sh ANDROID_FLAVOR=cm7.
#
# Usage: build-cm7.sh [--force-extract]
# Inputs:  src/cm7-roms/cm-7.2.0-cooper.zip
# Outputs: out/android/{system.img,data.img}, out/cm7-init/*

set -euo pipefail

LEGACY="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$LEGACY/.." && pwd)"
SRC="$REPO/src"
OUT="$REPO/out"
PORT="$REPO/port"
WORK=/root/p3ds
ROM="${ROM:-$SRC/cm7-roms/cm-7.2.0-cooper.zip}"
CMWORK="$WORK/cm7-port"
SYSDIR="$CMWORK/system"
RAMDISK="$WORK/cm7-boot/ramdisk"
APPS_DIR="${APPS_DIR:-$REPO/sd-staging/CYANO3DS}"
SYSTEM_IMG_MB=256
DATA_IMG_MB=256

say() { printf '\n==> %s\n' "$*"; }

[ -f "$ROM" ] || {
	echo "missing $ROM" >&2
	echo "  download an ARMv6 CM7.2 ROM (e.g. cm-7.2.0-cooper.zip) into src/cm7-roms/" >&2
	exit 1; }

# ---------------------------------------------------------------------------
say "1/6  extract the CM7.2 system tree + boot ramdisk"
# ---------------------------------------------------------------------------
mkdir -p "$CMWORK" "$RAMDISK"
if [ "${1:-}" = "--force-extract" ] || [ ! -d "$SYSDIR/framework" ]; then
	rm -rf "$SYSDIR"
	( cd "$CMWORK" && unzip -oq "$ROM" "system/*" )
fi
du -sh "$SYSDIR" | sed 's/^/    /'
ls "$SYSDIR/framework" | sed 's/^/    framework\//' | head

# The ROM's boot.img carries the real CM7 init + init.rc + default.prop.
if [ ! -f "$RAMDISK/init" ] || [ "${1:-}" = "--force-extract" ]; then
	rm -rf "$RAMDISK"; mkdir -p "$RAMDISK"
	python3 - "$ROM" "$RAMDISK" <<'PY'
import io, struct, sys, tarfile, zipfile, gzip, os, subprocess
rom, dst = sys.argv[1], sys.argv[2]
z = zipfile.ZipFile(rom)
b = z.read("boot.img")
assert b[:8] == b"ANDROID!", "not an Android boot image"
ksize, _ = struct.unpack("<II", b[8:16])
rsize, _ = struct.unpack("<II", b[16:24])
page,   = struct.unpack("<I", b[36:40])
def pages(n): return (n + page - 1) // page
off = page + pages(ksize) * page
ram = b[off:off + rsize]
assert ram[:2] == b"\x1f\x8b", "ramdisk is not gzip"
raw = gzip.decompress(ram)
# cpio newc parse
p = 0
files = {}
while True:
    if raw[p:p+6] != b"070701":
        break
    def field(i): return int(raw[p+6+i*8:p+14+i*8], 16)
    namesize = field(11); filesize = field(6)
    name = raw[p+110:p+110+namesize-1].decode()
    data = raw[p+110+namesize : p+110+namesize+filesize]
    # 110 hdr + namesize, padded to 4 (absolute offset rounding)
    p = p + 110 + namesize + filesize
    p = (p + 3) & ~3
    files[name] = data
    if name == "TRAILER!!!":
        break
for name in ("init", "init.rc", "default.prop", "ueventd.rc"):
    key = name
    if key in files:
        with open(os.path.join(dst, name), "wb") as f:
            f.write(files[key])
        print("    ramdisk:", name, len(files[key]), "bytes")
PY
fi
ls -l "$RAMDISK/init" "$RAMDISK/init.rc" "$RAMDISK/default.prop" | sed 's/^/    /'

# ---------------------------------------------------------------------------
say "2/6  reconstruct permissions + busybox/toolbox symlinks"
# ---------------------------------------------------------------------------
# The CM7 ROM zip stores neither unix modes nor symlinks: the recovery
# updater-script creates them on install.  Reproduce the parts that matter.
python3 - "$ROM" "$SYSDIR" <<'PY'
import os, re, sys, zipfile

rom, sysdir = sys.argv[1], sys.argv[2]

# 1. sane base modes: dirs 0755, files 0644, then binaries executable.
for root, dirs, files in os.walk(sysdir):
    os.chmod(root, 0o755)
    for f in files:
        os.chmod(os.path.join(root, f), 0o644)
for sub in ("bin", "xbin", "vendor/bin", "etc/init.d", "etc/ppp"):
    d = os.path.join(sysdir, sub)
    if not os.path.isdir(d):
        continue
    for f in os.listdir(d):
        p = os.path.join(d, f)
        if os.path.isfile(p) and not os.path.islink(p):
            os.chmod(p, 0o755)
for special in ("xbin/su", "bin/run-as", "bin/netcfg", "bin/ping", "bin/screenshot"):
    p = os.path.join(sysdir, special)
    if os.path.exists(p):
        os.chmod(p, 0o755)

# 2. the updater-script's symlink() calls (busybox -> xbin, toolbox -> bin).
z = zipfile.ZipFile(rom)
script = z.read("META-INF/com/google/android/updater-script").decode()
made = 0
for m in re.finditer(r'symlink\("([^"]+)"\s*,(.*?)\);', script, re.S):
    target = m.group(1)
    paths = re.findall(r'"([^"]+)"', m.group(2))
    for path in paths:
        path = path.replace("/system", sysdir, 1)
        if not os.path.isdir(os.path.dirname(path)):
            continue
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        os.symlink(target, path)
        made += 1
print("    base modes set; %d symlinks created (busybox/toolbox)" % made)
PY

# ---------------------------------------------------------------------------
say "3/6  replace the cooper GPU stack with the generic software one"
# ---------------------------------------------------------------------------
# The 3DS has no Linux GPU driver.  Keep Gingerbread's generic fbdev gralloc
# and software GLES1; drop the Adreno blobs and egl.cfg so libEGL's loader
# falls back to the default tag "android" (== libGLES_android.so).
removed=0
for f in \
	lib/egl/egl.cfg \
	lib/egl/libEGL_adreno200.so \
	lib/egl/libGLESv1_CM_adreno200.so \
	lib/egl/libGLESv2_adreno200.so \
	lib/egl/libq3dtools_adreno200.so \
	lib/hw/gralloc.cooper.so \
	lib/hw/copybit.cooper.so \
	lib/hw/lights.cooper.so \
	lib/hw/sensors.goldfish.so \
	lib/hw/gps.goldfish.so \
	vendor/lib/hw/gps.cooper.so ; do
	if [ -e "$SYSDIR/$f" ]; then rm -f "$SYSDIR/$f"; removed=$((removed+1)); echo "    removed $f"; fi
done
echo "    kept lib/egl/libGLES_android.so + lib/hw/gralloc.default.so + sensors.default.so"
[ "$removed" -gt 0 ] || echo "    (already clean)"

# ---------------------------------------------------------------------------
say "4/6  port overrides for /system (build.prop, keylayout)"
# ---------------------------------------------------------------------------
python3 - "$SYSDIR/build.prop" <<'PY'
import re, sys
p = sys.argv[1]
over = {
    "ro.product.model": "Nintendo 3DS",
    "ro.product.brand": "nintendo",
    "ro.product.name": "nintendo3ds",
    "ro.product.device": "nintendo3ds",
    "ro.product.board": "nintendo3ds",
    "ro.product.manufacturer": "nintendo",
    "ro.product.locale.language": "en",
    "ro.product.locale.region": "US",
    "ro.sf.lcd_density": "120",
    "qemu.sf.lcd_density": "120",
    "ro.opengles.version": "65536",       # software GLES 1.x only
    "ro.hardware": "nintendo3ds",
    "ro.board.platform": "nintendo3ds",
    "ro.telephony.default_network": "0",
    "ro.com.google.locationfeatures": "0",
    "ro.com.google.networklocation": "0",
    "ro.kernel.android.checkjni": "0",
    "ro.setupwizard.mode": "DISABLED",
    "dalvik.vm.heapsize": "64m",
    "dalvik.vm.heapgrowthlimit": "64m",
    "dalvik.vm.stack-trace-file": "/data/anr/traces.txt",
    "wifi.interface": "wlan0",
}
lines = open(p).read().splitlines()
out, seen = [], set()
pat = lambda k: re.compile(r'^\s*' + re.escape(k) + r'\s*=')
for ln in lines:
    hit = None
    for k in over:
        if pat(k).match(ln):
            hit = k
            break
    if hit is None:
        out.append(ln)
    elif hit not in seen:
        out.append("%s=%s" % (hit, over[hit]))
        seen.add(hit)
    # duplicate key line: dropped
for k, v in over.items():
    if k not in seen:
        out.append("%s=%s" % (k, v))
open(p, "w").write("\n".join(out) + "\n")
print("    build.prop: applied %d 3DS overrides (duplicates collapsed)" % len(over))
PY
grep -E "^(ro.product.device|ro.sf.lcd_density|ro.opengles.version|ro.hardware|ro.board.platform|dalvik.vm.heapsize)=" "$SYSDIR/build.prop" | sed 's/^/    /'

KL="$SYSDIR/usr/keylayout/qwerty.kl"
if [ -f "$KL" ]; then
	grep -q '^key 15 ' "$KL" || printf 'key 15    TAB\n' >> "$KL"
	echo "    qwerty.kl: 'key 15 TAB' present (3DS X/Y bracket row)"
fi
# The 3DS L/R are volume keys already in qwerty.kl; make sure the search key
# scancode 217 is present so ZR reaches Android as SEARCH.
grep -q '^key 217 ' "$KL" || printf 'key 217   SEARCH\n' >> "$KL" 2>/dev/null || true

# ---------------------------------------------------------------------------
say "4b/6  card apps + native libs into /system"
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
say "5/6  packing /system and /data"
# ---------------------------------------------------------------------------
mkdir -p "$OUT/android"
rm -f "$OUT/android/system.img" "$OUT/android/data.img"

# CM7's system tree is ~120 MiB against the 256 MiB image.  If it ever grows
# past the image, mke2fs fails loudly rather than silently truncating.
mke2fs -q -t ext4 -O ^has_journal -b 4096 -L system -d "$SYSDIR" \
	"$OUT/android/system.img" "${SYSTEM_IMG_MB}M"
printf '%-40s %s\n' "system.img" "$(du -h "$OUT/android/system.img" | cut -f1)"

mke2fs -q -t ext4 -O ^has_journal -b 4096 -L data -F \
	"$OUT/android/data.img" "${DATA_IMG_MB}M"
printf '%-40s %s\n' "data.img" "$(du -h "$OUT/android/data.img" | cut -f1)"

# ---------------------------------------------------------------------------
say "6/6  CM7 boot stage (init + patched init.rc + default.prop)"
# ---------------------------------------------------------------------------
STAGE="$OUT/cm7-init"
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$RAMDISK/init" "$STAGE/init"
python3 - "$STAGE/init" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
d = p.read_bytes()
n = d.count(b"tmpfs")
if n:
    p.write_bytes(d.replace(b"tmpfs", b"tmpfx"))
print("    init: patched %d 'tmpfs' string(s) -> 'tmpfx' (keeps our /dev)" % n)
PY
file "$STAGE/init" | sed 's/^/    /'

cp "$RAMDISK/default.prop" "$STAGE/default.prop"
cat >> "$STAGE/default.prop" <<'PROPS'
ro.sf.lcd_density=120
ro.hardware=nintendo3ds
ro.board.platform=nintendo3ds
dalvik.vm.heapsize=64m
dalvik.vm.heapgrowthlimit=64m
PROPS

# ---------------------------------------------------------------------------
# init.rc: the authentic CM7 one, patched for a device whose /system, /data and
# /cache are already mounted by the port's initramfs, with no yaffs2 / squashfs
# / apanic / lowmemorykiller / Bluetooth / Adreno.
# ---------------------------------------------------------------------------
python3 - "$RAMDISK/init.rc" "$STAGE/init.rc" <<'PY'
import re, sys, pathlib

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = src.read_text()

def comment(m):
    return "# [3ds] " + m.group(0)

# 1. disable the storage mounts: the initramfs already mounted /system, /data
#    and /cache from the ext4 image files on the SD card.
s = re.sub(r'(?m)^[ \t]*mount[ \t]+yaffs2[ \t]+.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+yfs2[ \t]+.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+ext4[ \t]+.*$', comment, s)
# CM7 cooper ships modules.sqf / xbin.sqf squashed; this kernel has no squashfs
# and the image files are not present, so the mounts would only log errors.
s = re.sub(r'(?m)^[ \t]*mount[ \t]+squashfs[ \t]+.*$', comment, s)

# 2. keep the initramfs root writable
s = re.sub(r'(?m)^[ \t]*mount[ \t]+rootfs[ \t]+rootfs[ \t]+/[ \t]+ro[ \t]+remount[ \t]*$', comment, s)

# 3. no apanic on this kernel
s = re.sub(r'(?m)^[ \t]*(copy|write)[ \t]+/proc/apanic.*$', comment, s)

# 4. keep the port's 20 s hung-task detector alive (CM7 disables it)
s = re.sub(r'(?m)^[ \t]*write[ \t]+/proc/sys/kernel/hung_task_timeout_secs.*$', comment, s)

# 5. no lowmemorykiller module in this kernel
s = re.sub(r'(?m)^[ \t]*write[ \t]+/sys/module/lowmemorykiller/.*$', comment, s)

# 6. services that are pointless or harmful on the 3DS:
#      ueventd        - the initramfs hand-builds /dev; no /sbin/ueventd exists,
#                       and as a 'critical' service it would reboot in a loop
#      dbus/bluetoothd/hfag/hsag/opush/pbap/map - no Bluetooth hardware
#      flash_recovery - no recovery partition
DROP_SERVICES = {"ueventd", "dbus", "bluetoothd", "hfag", "hsag", "opush",
                 "pbap", "map", "flash_recovery", "setup_fs"}
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
        out.append('# [3ds] ' + ln)
        continue
    out.append(ln)
s = ''.join(out)
s = re.sub(r'(?m)^[ \t]*start[ \t]+(ueventd|dbus|bluetoothd)[ \t]*$', comment, s)

# 7. Gingerbread's get_first_command() walks past the sentinel of an action
#    that ends up with no commands and jumps to address 0.  Commenting out the
#    only command of `on early-init` (start ueventd) and of `on fs` (the
#    yaffs2 mounts) empties them, so drop any empty action.
def _is_header(ln):
    return (ln.startswith('on ') or ln.startswith('service ') or
            ln.startswith('import '))
def _is_cmd(ln):
    t = ln.strip()
    return bool(t) and not t.startswith('#') and not _is_header(t)
raw = s.split('\n')
out, i, n = [], 0, len(raw)
while i < n:
    ln = raw[i]
    if ln.startswith('on '):
        j, ncmds = i + 1, 0
        while j < n and not _is_header(raw[j]):
            if _is_cmd(raw[j]):
                ncmds += 1
            j += 1
        if ncmds:
            out.extend(raw[i:j])
        else:
            out.append('# [3ds] dropped empty action: ' + ln)
            dropped.append('empty action dropped: ' + ln)
        i = j
    else:
        out.append(ln)
        i += 1
s = '\n'.join(out)

# 8. append the 3DS additions
s += '''
# ---------------------------------------------------------------------------
# Android-3DS port additions (CyanogenMod 7.2)
# ---------------------------------------------------------------------------

on init
    chmod 0666 /dev/binder
    chmod 0666 /dev/ashmem
    chown system log /dev/log/main
    chown system log /dev/log/events
    chmod 0662 /dev/log/main
    chmod 0662 /dev/log/events
    # Android's display is the bottom screen, exposed by ctr_lcd_fb as fb1
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
echo "==> done: $OUT/android/{system.img,data.img} and $OUT/cm7-init/"
