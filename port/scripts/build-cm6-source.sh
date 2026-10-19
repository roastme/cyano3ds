#!/bin/bash
# build-cm6-source.sh - assemble the CyanogenMod 6 (Android 2.2 Froyo, API 8)
#                       userspace for the OLD Nintendo 3DS / 2DS from a CM6
#                       *source build* (see docs/CM6.md).
#
# Takes out/target/product/ctr/ of the froyo-stable tree built by
# port/scripts/cm6-rootless.sh and turns it into the SD-card artifacts:
#   1. software GLES1 + generic gralloc (no egl.cfg),
#   2. the 3DS build.prop / keylayout overrides,
#   3. out/cm6/android/{system,data}.img (ext4),
#   4. out/cm6-init/ (source-built init + default.prop + patched init.rc) for
#      mkinitramfs.sh ANDROID_FLAVOR=cm6.
# It never touches out/android or out/cm7-init (the CM7 release artifacts).
#
# Usage: CM6_DIR=/path/to/cm6/src bash port/scripts/build-cm6-source.sh

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/out"
CM6_DIR="${CM6_DIR:-$REPO/../cm6/src}"
TARGET_OUT="$CM6_DIR/out/target/product/ctr"
SYSDIR="$TARGET_OUT/system"
RAMDISK="$TARGET_OUT/root"
SYSTEM_IMG_MB="${SYSTEM_IMG_MB:-192}"
DATA_IMG_MB="${DATA_IMG_MB:-64}"
OUTA="$OUT/cm6"

say() { printf '\n==> %s\n' "$*"; }

[ -d "$SYSDIR/framework" ] || {
	echo "missing source-build tree: $SYSDIR (run cm6-rootless.sh build)" >&2
	exit 1; }
[ -f "$RAMDISK/init" ] && [ -f "$RAMDISK/init.rc" ] || {
	echo "missing build ramdisk: $RAMDISK/{init,init.rc}" >&2
	exit 1; }

say "1/4  graphics: software GLES1 + generic gralloc"
rm -f "$SYSDIR/lib/egl/egl.cfg"
ls "$SYSDIR/lib/egl" "$SYSDIR/lib/hw" 2>/dev/null | sed 's/^/    /'

say "2/4  port overrides for /system (build.prop, keylayout)"
cat >> "$SYSDIR/build.prop" <<'PROPS'
# --- Android-3DS port overrides (CyanogenMod 6, old 3DS) ---
ro.sf.lcd_density=120
qemu.sf.lcd_density=120
ro.opengles.version=65536
ro.hardware=ctr
ro.telephony.default_network=0
ro.kernel.android.checkjni=0
ro.setupwizard.mode=DISABLED
dalvik.vm.heapsize=32m
dalvik.vm.stack-trace-file=/data/anr/traces.txt
PROPS
KL="$SYSDIR/usr/keylayout/qwerty.kl"
if [ -f "$KL" ]; then
	grep -q '^key 15 ' "$KL" || printf 'key 15    TAB\n' >> "$KL"
	grep -q '^key 217 ' "$KL" || printf 'key 217   SEARCH\n' >> "$KL"
	echo "    qwerty.kl: ensured 'key 15 TAB' + 'key 217 SEARCH'"
fi

say "3/4  packing /system and /data"
mkdir -p "$OUTA/android"
rm -f "$OUTA/android/system.img" "$OUTA/android/data.img"
mke2fs -q -t ext4 -O ^has_journal -b 4096 -L system -d "$SYSDIR" \
	"$OUTA/android/system.img" "${SYSTEM_IMG_MB}M"
printf '%-40s %s\n' "system.img" "$(du -h "$OUTA/android/system.img" | cut -f1)"
mke2fs -q -t ext4 -O ^has_journal -b 4096 -L data -F \
	"$OUTA/android/data.img" "${DATA_IMG_MB}M"
printf '%-40s %s\n' "data.img" "$(du -h "$OUTA/android/data.img" | cut -f1)"

say "4/4  Android boot stage (init + patched init.rc + default.prop)"
# ---------------------------------------------------------------------------
STAGE="$OUT/cm6-init"
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
ro.hardware=ctr
dalvik.vm.heapsize=32m
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
    setprop dalvik.vm.heapsize 32m
    setprop ro.sf.lcd_density 120
    setprop ro.hardware ctr

on boot
    setprop ro.sf.lcd_density 120
    setprop ro.hardware ctr
    setprop wifi.interface wlan0
    mkdir /data/misc/dhcp 0777 dhcp dhcp
    chmod 0777 /data/misc/dhcp
    # wpa_ctrl_open() binds its client socket here; without the directory
    # every connect fails with ENOENT and Wi-Fi never gets past "Turning on"
    mkdir /data/misc/wifi/sockets 0770 wifi wifi
    chmod 0770 /data/misc/wifi/sockets

# ---------------------------------------------------------------------------
# Wi-Fi: Atheros AR6014G on SDIO, driven by the in-kernel ath6kl (cfg80211 +
# CFG80211_WEXT), so Froyo's wpa_supplicant_6 runs with the "wext" driver.
# The control socket must be wpa_<ctrl_interface> (wpa_ctrl.c / wifi.c).
# dhcpcd is started by libnetutils as "dhcpcd:wlan0" (ctl.start with dynamic
# args, which Froyo's init only accepts for oneshot services); mkinitramfs.sh
# swaps /system/bin/dhcpcd for /bin/dhcpcd_wlan0.sh, the root-side property
# publisher that also starts the real dhcpcd (see N3DS_CM7_DHCP_PROPERTY_PUBLISHER).
# ---------------------------------------------------------------------------
service wpa_supplicant /system/bin/wpa_supplicant -Dwext -dd -K -t -iwlan0 -c/data/misc/wifi/wpa_supplicant.conf
    socket wpa_wlan0 dgram 660 wifi wifi
    disabled
    oneshot

service dhcpcd /system/bin/dhcpcd -dd -f /system/etc/dhcpcd/dhcpcd.conf
    disabled
    oneshot
'''
dst.write_text(s)
print("    init.rc: mounts disabled, servicemanager fixed, 3DS section added")
for d in dropped:
    print("    init.rc: %s" % d)
PY

ls -l "$STAGE" | sed 's/^/    /'
echo
echo "==> done: $OUTA/android/{system.img,data.img} and $OUT/cm6-init/"
