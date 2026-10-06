#!/usr/bin/env python3
"""Replace the sysfs-walking node creation with explicit device enumeration.

busybox' `find` needs CONFIG_FEATURE_FIND_NAME for `-name`, which the static
busybox built for this port does not have - so `$(find /sys/class/misc -name
dev)` silently produced nothing and none of the sysfs-derived /dev nodes
(binder, ashmem, log/*, fb*, input/event*) were created.  Only the mem devices
were, which is exactly what the 15-entry /dev showed.

Explicit loops need only cat/test/mkdir/mknod, all of which are verified below.
"""
import pathlib

p = pathlib.Path(__file__).resolve().with_name("mkinitramfs.sh")
s = p.read_text()

start = s.index("\t\t# everything with a uevent: take the numbers from sysfs.")
end = s.index('\t\tsay "  /dev now has', start)

block = '''\t\t# Everything else comes from sysfs.  Explicit enumeration, no find:
\t\t# busybox' find needs CONFIG_FEATURE_FIND_NAME for -name, which this
\t\t# static busybox does not have (a silent zero-match that cost a boot).
\t\tfor m in binder ashmem log/main log/events log/radio; do
\t\t\ts="/sys/class/misc/$m/dev"
\t\t\t[ -e "$s" ] || continue
\t\t\tcase "$m" in
\t\t\t\t*/*) mkdir -p "/dev/${m%%/*}" 2>/dev/null ;;
\t\t\tesac
\t\t\tmm=$(cat "$s")
\t\t\t[ -c "/dev/$m" ] || mknod "/dev/$m" c "${mm%%:*}" "${mm##*:}" 2>/dev/null
\t\tdone
\t\tfor i in 0 1 2 3; do
\t\t\ts="/sys/class/graphics/fb$i/dev"
\t\t\t[ -e "$s" ] || continue
\t\t\tmm=$(cat "$s")
\t\t\t[ -c "/dev/fb$i" ] || mknod "/dev/fb$i" c "${mm%%:*}" "${mm##*:}" 2>/dev/null
\t\tdone
\t\tfor i in 0 1 2 3 4 5; do
\t\t\ts="/sys/class/input/event$i/dev"
\t\t\t[ -e "$s" ] || continue
\t\t\tmm=$(cat "$s")
\t\t\t[ -c "/dev/input/event$i" ] || mknod "/dev/input/event$i" c "${mm%%:*}" "${mm##*:}" 2>/dev/null
\t\tdone
\t\tfor i in 0 1 2 3 4 5 6 7; do
\t\t\ts="/sys/class/block/loop$i/dev"
\t\t\t[ -e "$s" ] || continue
\t\t\tmm=$(cat "$s")
\t\t\t[ -b "/dev/loop$i" ] || mknod "/dev/loop$i" b "${mm%%:*}" "${mm##*:}" 2>/dev/null
\t\tdone
\t\tfor b in vda vda1 vdb vdb1 mmcblk0 mmcblk0p1 mmcblk1 mmcblk1p1; do
\t\t\ts="/sys/class/block/$b/dev"
\t\t\t[ -e "$s" ] || continue
\t\t\tmm=$(cat "$s")
\t\t\t[ -b "/dev/$b" ] || mknod "/dev/$b" b "${mm%%:*}" "${mm##*:}" 2>/dev/null
\t\tdone
\t\t# Android's display is fb1 (the kernel console owns fb0)
\t\tln -sf /dev/fb1 /dev/graphics/fb0 2>/dev/null
\t\tln -sf /dev/fb0 /dev/graphics/fb1 2>/dev/null
'''
s = s[:start] + block + s[end:]

# the log line no longer uses readlink
s = s.replace(
    '\t\tsay "  /dev now has $(ls /dev | wc -l) entries; fb0 -> $(readlink /dev/graphics/fb0)"',
    '\t\tsay "  /dev now has $(ls /dev | wc -l) entries"\n'
    '\t\tsay "  graphics/fb0: $([ -e /dev/graphics/fb0 ] && echo ok || echo MISSING)"')

# ---------------------------------------------------------------- assertions
anchor = '# ---------------------------------------------------------------------------\nsay "verifying the packed artefact"'
assert anchor in s
s = s.replace(anchor, '''# ---------------------------------------------------------------------------
say "checking the busybox applets the init script uses"
# Only a handful of applets are compiled into the static busybox; using one that
# is not there fails *silently* at runtime on the console (see the find/-name
# episode).  Fail the build instead.
NEEDED="sh mount umount ls cat echo sleep dmesg tee sync mkdir mknod grep tail
        head cut tr hexdump dd wc ps free kill poweroff reboot basename dirname
        readlink sed test ln"
for a in $NEEDED; do
\t"$BB" --list 2>/dev/null | grep -qx "$a" || {
\t\techo "FATAL: busybox has no '$a' applet (init uses it)" >&2; exit 1; }
done
echo "    all $(echo $NEEDED | wc -w) applets present"

# ---------------------------------------------------------------------------
say "verifying the packed artefact"''')
print("patched: explicit device enumeration + applet assertions")
p.write_bytes(s.encode())
print("written", len(s), "bytes")
