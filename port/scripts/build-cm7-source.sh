#!/bin/bash
# build-cm7-source.sh - assemble the CyanogenMod 7.2 (Android 2.3.7, API 10)
#                       Nintendo 3DS userspace from a CM7.2 *source build*.
#
# This is the source-build counterpart of legacy/build-cm7.sh (which repackages a stock
# cm-7.2.0-cooper.zip): this script builds CM7.2 from source for
# `device/nintendo3ds` (see port/scripts/apply-cm7-device.sh and docs/CM7.md),
# and this script takes the build's `out/target/product/nintendo3ds/` tree and
# turns it into the SD-card artifacts:
#
#   1. drop the generic-only graphics leftovers (there should be no Adreno
#      blobs in a source build, but delete egl.cfg so libEGL uses the default
#      "android" tag == libGLES_android.so),
#   2. apply the 3DS build.prop / keylayout overrides and bake the card APKs,
#   3. pack out/android/{system,data}.img (256 MiB ext4 each),
#   4. stage out/cm7-init/ (source-built init + default.prop + the port's
#      patched init.rc) for mkinitramfs.sh ANDROID_FLAVOR=cm7.
#
# Usage:
#   CM7_DIR=/root/p3ds/cm7 bash port/scripts/build-cm7-source.sh
#   CM7_DIR=/root/p3ds/cm7 bash port/scripts/build-cm7-source.sh --from-image
#
# --from-image uses out/android/system.img (already built), otherwise the
# tree out/target/product/nintendo3ds/system/ is used.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/out"
WORK=/root/p3ds
CM7_DIR="${CM7_DIR:-$WORK/cm7}"
TARGET_OUT="$CM7_DIR/out/target/product/nintendo3ds"
SYSDIR="$TARGET_OUT/system"
RAMDISK="$TARGET_OUT/root"
APPS_DIR="${APPS_DIR:-$REPO/sd-staging/CYANO3DS}"
SYSTEM_IMG_MB=256
DATA_IMG_MB=256

say() { printf '\n==> %s\n' "$*"; }

[ -d "$SYSDIR/framework" ] || {
	echo "missing source-build tree: $SYSDIR" >&2
	echo "  build CM7.2 first:  cd $CM7_DIR && . build/envsetup.sh && brunch nintendo3ds" >&2
	exit 1; }
[ -f "$RAMDISK/init" ] && [ -f "$RAMDISK/init.rc" ] || {
	echo "missing build ramdisk: $RAMDISK/{init,init.rc}" >&2
	exit 1; }

# ---------------------------------------------------------------------------
say "1/6  graphics: force the software GLES1 + generic gralloc path"
# ---------------------------------------------------------------------------
# A source build has no vendor GPU blobs, but a stray egl.cfg would point
# libEGL at a driver that does not exist.  Remove it (and any adreno leftovers)
# so libEGL falls back to tag "android" == libGLES_android.so.
removed=0
for f in \
	lib/egl/egl.cfg \
	lib/egl/libEGL_adreno200.so \
	lib/egl/libGLESv1_CM_adreno200.so \
	lib/egl/libGLESv2_adreno200.so \
	lib/egl/libq3dtools_adreno200.so \
	lib/hw/gralloc.cooper.so \
	lib/hw/copybit.cooper.so \
	lib/hw/lights.cooper.so ; do
	if [ -e "$SYSDIR/$f" ]; then rm -f "$SYSDIR/$f"; removed=$((removed+1)); echo "    removed $f"; fi
done
echo "    kept lib/egl/libGLES_android.so + lib/hw/gralloc.default.so"
[ "$removed" -gt 0 ] || echo "    (already clean)"

# ---------------------------------------------------------------------------
say "2/6  port overrides for /system (build.prop, keylayout)"
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
    # CyanogenMod 7.2 (unlike upstream AOSP) sends the optimized dex of everything
    # under /system -- the framework jars and the system apps -- to
    # $ANDROID_CACHE/dalvik-cache, i.e. /cache/dalvik-cache (see
    # dalvik/libdex/OptInvocation.c and frameworks/base/cmds/installd/commands.c,
    # both gated on this property).  The port keeps a single /data/dalvik-cache
    # (seeded from data.img and pre-dexopt'd by the initramfs) and mounts /cache
    # as a throwaway tmpfs, so without this the boot-classpath chaches the VM
    # needs are never where it looks: installd's dexopt forks abort with
    # SIGSEGV (status=0x000b) and system_server dies loading SettingsProvider.
    # CM7's own escape hatch forces the upstream-AOSP layout we already support.
    "dalvik.vm.dexopt-data-only": "1",
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
	grep -q '^key 217 ' "$KL" || printf 'key 217   SEARCH\n' >> "$KL"
	echo "    qwerty.kl: ensured 'key 15 TAB' + 'key 217 SEARCH'"
fi

# ---------------------------------------------------------------------------
say "2b/6  card apps + native libs into /system"
# ---------------------------------------------------------------------------
if [ -d "$APPS_DIR" ]; then
	mkdir -p "$SYSDIR/app" "$SYSDIR/lib"
	n=0
	for apk in "$APPS_DIR"/*.apk; do
		[ -f "$apk" ] || continue
		b=$(basename "$apk")
		case "$b" in
			GBCoid.apk|AngryBirds.apk|Bejeweled2.apk|Chuzzle.apk) ;;
		esac
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
say "3/6  packing /system and /data"
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
say "4/6  CM7 boot stage (source-built init + default.prop + patched init.rc)"
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
# CM7 sends every /system dex to $ANDROID_CACHE/dalvik-cache unless this is 1
# (libdex/OptInvocation.c).  /default.prop is loaded before build.prop, so the
# early helper VMs (svc/am/service) and the VM's own classpath lookup agree
# with the port's single /data/dalvik-cache from the very first process.
dalvik.vm.dexopt-data-only=1
PROPS

# ---------------------------------------------------------------------------
# init.rc: the source-built CM7 one, patched for a device whose /system, /data
# and /cache are already mounted by the port's initramfs, with no yaffs2 /
# squashfs / apanic / lowmemorykiller / Bluetooth.
# ---------------------------------------------------------------------------
python3 - "$RAMDISK/init.rc" "$STAGE/init.rc" <<'PY'
import re, sys, pathlib

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
s = src.read_text()

# CM7's init.rc exports ANDROID_CACHE=/cache and its VM/installd then look in
# /cache/dalvik-cache for everything under /system.  The port keeps a single
# /data/dalvik-cache; dalvik.vm.dexopt-data-only=1 already forces it, but point
# the environment at /data too so there is exactly one cache directory even if
# some path only reads the env (and the early helper VMs stop logging
# "Can't open dex cache '/cache/dalvik-cache/...'").
s = s.replace('export ANDROID_CACHE /cache', 'export ANDROID_CACHE /data')

def comment(m):
    return "# [3ds] " + m.group(0)

s = re.sub(r'(?m)^[ \t]*mount[ \t]+yaffs2[ \t]+.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+yfs2[ \t]+.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+ext4[ \t]+.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+squashfs[ \t]+.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*mount[ \t]+rootfs[ \t]+rootfs[ \t]+/[ \t]+ro[ \t]+remount[ \t]*$', comment, s)
s = re.sub(r'(?m)^[ \t]*(copy|write)[ \t]+/proc/apanic.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*write[ \t]+/proc/sys/kernel/hung_task_timeout_secs.*$', comment, s)
s = re.sub(r'(?m)^[ \t]*write[ \t]+/sys/module/lowmemorykiller/.*$', comment, s)

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

s += '''
# ---------------------------------------------------------------------------
# Android-3DS port additions (CyanogenMod 7.2, source build)
# ---------------------------------------------------------------------------

on init
    chmod 0666 /dev/binder
    chmod 0666 /dev/ashmem
    chown system log /dev/log/main
    chown system log /dev/log/events
    chmod 0662 /dev/log/main
    chmod 0662 /dev/log/events
    mkdir /dev/graphics 0755 root root
    symlink /dev/fb1 /dev/graphics/fb0
    symlink /dev/fb0 /dev/graphics/fb1
    chmod 0666 /dev/fb0
    chmod 0666 /dev/fb1
    chmod 0660 /dev/input/event0
    chmod 0660 /dev/input/event1
    chmod 0660 /dev/input/event2
    chmod 0660 /dev/input/event3
    chmod 0660 /dev/input/event4
    chmod 0660 /dev/input/event5
    # ALSA nodes for the CSND/TSC2117 card (created by the port's /init).
    chmod 0666 /dev/snd/controlC0
    chmod 0666 /dev/snd/pcmC0D0p
    chmod 0666 /dev/snd/timer
    chown root input /dev/input/event0
    chown root input /dev/input/event1
    chown root input /dev/input/event2
    chown root input /dev/input/event3
    chown root input /dev/input/event4
    chown root input /dev/input/event5
    setprop dalvik.vm.heapsize 64m
    setprop dalvik.vm.heapgrowthlimit 64m
    setprop dalvik.vm.dexopt-data-only 1
    setprop ro.sf.lcd_density 120
    setprop ro.hardware nintendo3ds

on boot
    setprop ro.sf.lcd_density 120
    setprop ro.hardware nintendo3ds

# on post-fs runs after every on init block.  CM7's do_mkdir() is a plain
# mkdir() that does not create parents, so creating the supplicant socket
# directory from on init failed with ENOENT (the stock on post-fs block above
# is what creates /data/misc/wifi) and wpa_ctrl_open() could not bind its
# client socket -- the framework then logged "Unable to open connection to
# supplicant on \"wlan0\"".  Create it here, after /data/misc/wifi exists.
on post-fs
    # wpa_supplicant/wpa_ctrl keep their private AF_UNIX sockets here
    mkdir /data/misc/wifi/sockets 0770 wifi wifi

# ---------------------------------------------------------------------------
# Wi-Fi: Atheros AR6014G on SDIO, driven by the in-kernel ath6kl (cfg80211 +
# CFG80211_WEXT), so wpa_supplicant_6 runs with the "wext" driver.  The
# control socket name must be wpa_<ctrl_interface> (see
# external/wpa_supplicant_6/.../ctrl_iface_unix.c and wifi.c).
# ---------------------------------------------------------------------------
service wpa_supplicant /system/bin/wpa_supplicant -Dwext -d -t -iwlan0 -c/data/misc/wifi/wpa_supplicant.conf
    socket wpa_wlan0 dgram 660 wifi wifi
    disabled
    oneshot

service dhcpcd_wlan0 /system/bin/dhcpcd -dd -f /system/etc/dhcpcd/dhcpcd.conf wlan0
    user root
    group dhcp inet
    disabled
    oneshot

service iprenew_wlan0 /system/bin/dhcpcd -n
    disabled
    oneshot
'''
dst.write_text(s)
print("    init.rc: mounts disabled, services fixed, 3DS section added")
for d in dropped:
    print("    init.rc: %s" % d)
PY

ls -l "$STAGE" | sed 's/^/    /'
echo
echo "==> done: $OUT/android/{system.img,data.img} and $OUT/cm7-init/"
