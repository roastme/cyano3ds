#!/bin/sh
# selftest - on-device checks of the port's subsystems (after Android3DS/
# Octoblimp's habit of testing every fix), one PASS/FAIL/INFO line each.
#
# The bring-up init runs it a few minutes after Android starts and copies the
# result to SD:/CYANO3DS/selftest.txt, so a real-hardware boot reports on lid
# sleep, Wi-Fi, audio, cameras, motion sensors, external storage, /data
# persistence and the console log without anyone typing a command.  It only
# reads state, except for a one-second microphone recording (pcmtest), which it
# skips while something else holds the capture stream.
#
# Usage: selftest            (exit status = number of FAILs)

PASS=0
FAIL=0
pass() { echo "PASS  $*"; PASS=$((PASS + 1)); }
fail() { echo "FAIL  $*"; FAIL=$((FAIL + 1)); }
info() { echo "INFO  $*"; }
strip() { sed 's/^\[[^]]*\] //; s/^[0-9]*,[0-9]*,[0-9]*,[^;]*;//'; }
check() {	# check "what" command...
	what=$1
	shift
	if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi
}
# The kernel ring buffer has long rotated out the boot messages by now; the
# init's kmsgdump keeps all of them in /mnt/probe/kmsg.log.
kmsg() {
	if [ -s /mnt/probe/kmsg.log ]; then
		grep -a -E "$1" /mnt/probe/kmsg.log
	else
		dmesg 2>/dev/null | grep -E "$1"
	fi
}

echo "selftest: $(date) (uptime $(cut -d' ' -f1 /proc/uptime) s)"
echo "kernel: $(cat /proc/version 2>/dev/null)"

echo "--- lid sleep (ctr_lid.c, mach-ctr/pm.c) ---"
LID=$(ls -d /sys/bus/platform/devices/*mcu-lid* 2>/dev/null | head -n 1)
if [ -n "$LID" ]; then
	pass "lid driver bound ($LID)"
	check "sleep on close enabled" grep -qx 1 "$LID/sleep_on_close"
	check "sleep hook is /bin/lid-sleep-hook" grep -qx /bin/lid-sleep-hook "$LID/sleep_hook"
	info "lid closed now: $(cat "$LID/closed" 2>/dev/null)"
else
	fail "lid driver bound (no *mcu-lid* platform device)"
fi
check "suspend to RAM offered" grep -qw mem /mnt/sysfs-real/power/state
info "sleep mode: $(kmsg 'ctr-pm: sleep mode ready' | tail -n 1 | strip)"
n=$(kmsg "ctr-pm: woke up" | wc -l)
light=$(kmsg "ctr-pm: woke up \(screen-off" | wc -l)
mcu=$(kmsg "ctr-pm: woke up \(PDN wake reason .*MCU" | wc -l)
info "sleeps this boot: $n (screen-off: $light; deep, woken by MCU events: $mcu)"
info "$(kmsg 'ctr-pm: woke up' | tail -n 1 | strip)"
if [ "$mcu" -ge 3 ] && [ "$mcu" -eq "$((n - light))" ]; then
	fail "every deep sleep was cut short by an MCU event (the old lid bug)"
fi
r=$(kmsg "sleep refused" | wc -l)
[ "$r" -gt 0 ] && info "refused suspends: $r ($(kmsg 'PM: .*failed to suspend|suspend.*error' | tail -n 1 | strip))"

echo "--- Wi-Fi (staging ath6k, AR6014 + NWM firmware) ---"
for f in stub_data stub_code main_type4 database; do
	check "firmware ath6k/AR6002/nwm/$f.bin" test -s "/lib/firmware/ath6k/AR6002/nwm/$f.bin"
done
if kmsg "AR6002: request firmware failed" >/dev/null; then
	fail "firmware loaded (driver: request firmware failed)"
fi
if kmsg "WMI is ready|WMI NWM READY" >/dev/null; then
	pass "AR6014 firmware running (WMI ready)"
else
	fail "AR6014 firmware running (no WMI ready in dmesg)"
fi
check "wlan0 exists" test -e /sys/class/net/wlan0
KV=$(cat /sys/module/*/parameters/n3ds_keyvar 2>/dev/null | head -n 1)
if [ "$KV" = 2 ]; then
	pass "cipher key format pinned to dsi-45-u0 (n3ds_keyvar=2)"
else
	fail "cipher key format pinned to dsi-45-u0 (n3ds_keyvar=${KV:-missing})"
fi
info "associations: $(kmsg 'ASSOCIATED channel' | wc -l), drops: $(kmsg 'NWM disconnect' | wc -l), EAPOL rx: $(kmsg 'EAPOL rx' | wc -l)"
info "kept powered through sleep: $(kmsg 'AR6002 suspend: keeping the chip powered' | wc -l)x; Wi-Fi dead after a sleep: $(kmsg 'Fail to setup WLAN state|Wmi not ready' | wc -l) lines"
info "wlan0: $(cat /sys/class/net/wlan0/operstate 2>/dev/null) $(ip -4 addr show wlan0 2>/dev/null | grep -o 'inet [0-9./]*')"
GP=/system/bin/getprop
if [ -x $GP ] && [ "$($GP dhcp.wlan0.result)" = ok ]; then
	info "DHCP published: ip=$($GP dhcp.wlan0.ipaddress) mask=$($GP dhcp.wlan0.mask) gw=$($GP dhcp.wlan0.gateway) dns1=$($GP dhcp.wlan0.dns1) net.dns1=$($GP net.dns1)"
	m=$($GP dhcp.wlan0.mask)
	if [ -n "$m" ] && [ "$m" != 0.0.0.0 ] && [ -n "$($GP dhcp.wlan0.dns1)" ]; then
		pass "DHCP lease handed to Android with a netmask and DNS"
	else
		fail "DHCP lease handed to Android with a netmask and DNS (mask=[$m])"
	fi
	info "$(kmsg 'N3DS-DHCP: traffic check' | tail -n 1 | strip)"
	gw=$($GP dhcp.wlan0.gateway)
	if [ -n "$gw" ] && [ -x /system/bin/ping ]; then
		if /system/bin/ping -c 2 -w 5 "$gw" >/dev/null 2>&1; then pass "gateway $gw answers ping"
		else fail "gateway $gw answers ping"; fi
	fi
else
	info "DHCP: no lease published to Android (Wi-Fi off or not connected)"
fi

echo "--- audio (ctr_snd.c: Octoblimp codec sequence, re-arm, MIC capture) ---"
check "ALSA playback node" test -c /dev/snd/pcmC0D0p
check "ALSA capture node" test -c /dev/snd/pcmC0D0c
if kmsg "3ds-snd: codec ready" >/dev/null; then pass "codec initialised"; else fail "codec initialised"; fi
info "$(kmsg '3ds-snd: codec vendor' | tail -n 1 | strip)"
info "output-stage re-arms this boot: $(kmsg 'output stage dropped' | wc -l)"
if [ -x /bin/pcmtest ]; then
	out=$(/bin/timeout 10 /bin/pcmtest capture 1 8000 1 2>&1)
	case "$out" in
	*"capture OK"*) pass "microphone stream delivers frames"
		info "$(echo "$out" | grep 'captured')"
		info "$(echo "$out" | grep '+1.0s\|+0.5s' | tail -n 1)" ;;
	*"Device or resource busy"*) info "microphone busy (an app is recording): not tested" ;;
	*) fail "microphone stream delivers frames"
		echo "$out" | sed 's/^/      /' ;;
	esac
fi
info "$(kmsg 'mic: ' | tail -n 2 | strip | tr '\n' ' ')"
# playback pointer: the CSND's own position once it is seen running at the
# stream rate (the clock estimate makes audio glitchy)
live=$(kmsg "CSND position register advances at the stream rate" | wc -l)
clk=$(kmsg "CSND position moved .*using the clock pointer|CSND position register stuck" | wc -l)
info "playback pointer: hardware on $live stream(s), clock fallback $clk time(s)"
[ "$clk" -gt 0 ] && info "$(kmsg 'CSND position (moved|register stuck)' | tail -n 1 | strip)"
if [ -x /bin/pcmtest ]; then
	out=$(/bin/timeout 15 /bin/pcmtest play 2 44100 2>&1)
	case "$out" in
	*"play OK"*) pass "speaker stream plays at the right speed"
		info "$(echo "$out" | grep 'played')"
		x=$(echo "$out" | grep -c 'Broken pipe')
		[ "$x" -gt 0 ] && info "underruns during the 2 s tone: $x" ;;
	*"Device or resource busy"*) info "speaker busy (Android holds it): playback not tested" ;;
	*) fail "speaker stream plays at the right speed"
		echo "$out" | tail -n 4 | sed 's/^/      /' ;;
	esac
fi

echo "--- cameras (ctr_cam.c + CameraHardwareN3ds) ---"
check "/dev/ctr_cam exists" test -c /dev/ctr_cam
check "/dev/ctr_cam usable by mediaserver (0666)" sh -c '[ "$(stat -c %a /dev/ctr_cam)" = 666 ]'
if kmsg "3ds-cam .*capture driver ready" >/dev/null; then
	pass "camera driver probed"
	info "$(kmsg '3ds-cam .*capture driver ready' | tail -n 1 | strip)"
else
	fail "camera driver probed"
fi
for s in outer-right inner outer-left; do
	l=$(kmsg "3ds-cam .*$s: chip id" | tail -n 1 | strip)
	[ -n "$l" ] && info "$l"
done
if [ -f /system/lib/libcameraservice.so ] && grep -q CameraHardwareN3ds /system/lib/libcameraservice.so; then
	pass "camera HAL in /system/lib/libcameraservice.so"
else
	fail "camera HAL in /system/lib/libcameraservice.so (stub camera; is CYANO3DS/libcameraservice.so on the card?)"
fi

echo "--- motion sensors (MCU accelerometer + ITG-1010/L3GD20H gyroscope) ---"
# The zero-rate level moves as the part warms up (the datasheet allows
# 0.03-0.05 dps per degree, a few LSB over the first minutes), so measure it
# again now that the console has been on for a while.  gyrocal adds to the
# calibration in force instead of replacing it, and its stillness gate keeps
# the previous one if the console is being moved.
if [ -x /bin/gyrocal ]; then
	/bin/gyrocal 2>&1 | while IFS= read -r line; do info "$line"; done
fi
ACC=""
GYRO=""
for d in /sys/bus/iio/devices/iio:device*; do
	[ -e "$d/in_accel_x_raw" ] && ACC=$d
	[ -e "$d/in_anglvel_x_raw" ] && GYRO=$d
done
# One "x y z" line of raw values, so a series is one pipe into awk.
raw_xyz() {
	printf '%s %s %s\n' "$(cat "$1/$2_x_raw" 2>/dev/null)" \
		"$(cat "$1/$2_y_raw" 2>/dev/null)" \
		"$(cat "$1/$2_z_raw" 2>/dev/null)"
}
# What a user sees as "the numbers jump around" is either a real noise floor or
# a broken read, and the two look nothing alike in a min/max spread.
jitter() { # $1 dir, $2 prefix, $3 label
	j=0
	while [ $j -lt 25 ]; do
		raw_xyz "$1" "$2"
		j=$((j + 1))
		sleep 0.02
	done | awk -v l="$3" '
		{ for (k = 1; k <= 3; k++) {
			if (NR == 1 || $k < mn[k]) mn[k] = $k
			if (NR == 1 || $k > mx[k]) mx[k] = $k
			sm[k] += $k } }
		END { printf "  %s jitter %d samples @20ms: x %d..%d y %d..%d z %d..%d (mean %.0f %.0f %.0f)\n",
			l, NR, mn[1], mx[1], mn[2], mx[2], mn[3], mx[3],
			sm[1]/NR, sm[2]/NR, sm[3]/NR }'
}
# Five samples a second apart, in the units the HAL reports.  At rest the
# accelerometer must read one g (9.81 m/s^2, on Android's +z when the console
# lies flat with the screen up) and the gyroscope ~0 rad/s: a wrong scale, a
# swapped axis or a noisy MCU/IMU read all show up in one line.
if [ -n "$ACC" ]; then
	pass "accelerometer IIO device ($ACC, MCU LIS331DLH)"
	ASCALE=$(cat "$ACC/in_accel_scale" 2>/dev/null)
	info "accel scale $ASCALE m/s^2/LSB, enabled=$(cat "$ACC/in_accel_en" 2>/dev/null)"
	i=0
	while [ $i -lt 5 ]; do
		raw_xyz "$ACC" in_accel | awk -v s="$ASCALE" -v i="$i" '
			{ printf "  a t+%ds raw %6d %6d %6d |a|=%5.2f m/s^2  HAL %+.2f %+.2f %+.2f\n",
				i, $1, $2, $3, s * sqrt($1*$1 + $2*$2 + $3*$3),
				-$1*s, $2*s, -$3*s }'
		i=$((i + 1))
		sleep 1
	done
	# Name the pose, so the report says whether the map is right without
	# anyone having to interpret the numbers: flat with the screen up must
	# put one g on HAL +z.
	raw_xyz "$ACC" in_accel | awk -v s="$ASCALE" '
		{ x = -$1*s; y = $2*s; z = -$3*s
		  m = sqrt(x*x + y*y + z*z)
		  ax = (x < 0 ? -x : x); ay = (y < 0 ? -y : y); az = (z < 0 ? -z : z)
		  if (m < 1.5) { printf "  pose: HAL %+.2f %+.2f %+.2f, |a| too small to tell\n", x, y, z }
		  else if (az > 0.9*m && z > 0) printf "  pose: flat, screen up - landscape is correct\n"
		  else if (az > 0.9*m) printf "  pose: flat, screen DOWN (or the z sign is inverted)\n"
		  else if (ay > 0.9*m && y > 0) printf "  pose: upright, hinge up\n"
		  else if (ay > ax) printf "  pose: mostly upright, %s end down\n", (y > 0 ? "hinge" : "D-pad")
		  else printf "  pose: sideways, %s side up\n", (x > 0 ? "right" : "left") }'
	jitter "$ACC" in_accel a
else
	fail "accelerometer IIO device (no in_accel_* channel)"
fi
if [ -n "$GYRO" ]; then
	pass "gyroscope IIO device ($GYRO, $(cat "$GYRO/name" 2>/dev/null))"
	GSCALE=$(cat "$GYRO/in_anglvel_scale" 2>/dev/null)
	# The driver refuses reads while powered down, so turn it on for the
	# duration unless a running app already did - and leave it as we found it.
	GEN=$(cat "$GYRO/in_anglvel_en" 2>/dev/null)
	[ "$GEN" = "1" ] || echo 1 > "$GYRO/in_anglvel_en" 2>/dev/null
	info "gyro scale $GSCALE rad/s/LSB, enabled=$(cat "$GYRO/in_anglvel_en" 2>/dev/null)$( [ "$GEN" = "1" ] || echo ', powered up by the test')"
	i=0
	while [ $i -lt 5 ]; do
		raw_xyz "$GYRO" in_anglvel | awk -v s="$GSCALE" -v i="$i" '
			{ printf "  w t+%ds raw %6d %6d %6d |w|=%6.3f rad/s  HAL %+.3f %+.3f %+.3f\n",
				i, $1, $2, $3, s * sqrt($1*$1 + $2*$2 + $3*$3),
				-$1*s, $2*s, -$3*s }'
		i=$((i + 1))
		sleep 1
	done
	jitter "$GYRO" in_anglvel w
	# ctr_gyro.c subtracts in_anglvel_*_calibbias from every read; /bin/gyrocal
	# measures it at boot.  Without it a console lying still reads a couple of
	# deg/s, which Android integrates into a drifting pointer.
	cal=""
	for a in x y z; do
		v=$(cat "$GYRO/in_anglvel_${a}_calibbias" 2>/dev/null)
		cal="$cal ${v:-?}"
	done
	cal=${cal# }
	info "gyro calibbias ${cal:-none} LSB (measured at boot by /bin/gyrocal)"
	# What matters is the bias, not one noisy sample: average 20 reads.
	w=$({ i=0
	       while [ $i -lt 20 ]; do
		       raw_xyz "$GYRO" in_anglvel
		       i=$((i + 1))
		       sleep 0.02
	       done; } | awk '
		{ n++; for (j = 1; j <= 3; j++) s[j] += $j }
		END { for (j = 1; j <= 3; j++) { m = s[j] / n; if (m < 0) m = -m; if (m > w) w = m }
		      printf "%.0f", w + 0 }')
	if [ "${w:-99}" -le 10 ]; then
		pass "gyro at rest averages $w LSB (zero-rate offset removed)"
	else
		fail "gyro at rest averages $w LSB (zero-rate offset not removed: is /bin/gyrocal running?)"
	fi
	[ "$GEN" = "1" ] || echo 0 > "$GYRO/in_anglvel_en" 2>/dev/null
else
	fail "gyroscope IIO device (no in_anglvel_* channel)"
fi
info "gyro driver: $(kmsg '3ds-gyro' | tail -n 2 | strip | tr '\n' ' ')"
if [ -f /system/lib/hw/sensors.nintendo3ds.so ]; then
	pass "sensors HAL /system/lib/hw/sensors.nintendo3ds.so"
else
	fail "sensors HAL missing (the device product did not ship sensors.nintendo3ds)"
fi
for f in accelerometer gyroscope; do
	check "feature file android.hardware.sensor.$f.xml" \
		test -f "/system/etc/permissions/android.hardware.sensor.$f.xml"
done
# Both IMUs share I2C bus 2 (0x10148000) with the IR chip and ZL/ZR.  The
# ZL/ZR chip at 0x2a is the known-good reference.  The gyro answers at 0x69 or
# 0x68 (ITG-1010, AD0 strap) or at 0x6b/0x6a (L3GD20H, SA0) depending on the
# model, and 0x6b is also the other HID id in Nintendo's table.  i2cget reports
# a NAK as an error because ctr_i2c.c now checks the ACK of the address byte.
BUS=$(ls -d /sys/bus/i2c/devices/*-002a 2>/dev/null | sed 's#.*/##; s/-002a$//' | head -n 1)
if [ -n "$BUS" ] && [ -x /bin/busybox ]; then
	info "I2C bus $BUS ($(cat /sys/bus/i2c/devices/$BUS-002a/name 2>/dev/null) at 0x2a)"
	for a in 68 69 6b; do
		info "  i2c-$BUS 0x$a: reg 0x75 = $(/bin/busybox i2cget -y "$BUS" "0x$a" 0x75 2>&1 | tail -n 1), reg 0x00 = $(/bin/busybox i2cget -y "$BUS" "0x$a" 0x00 2>&1 | tail -n 1)"
	done
	info "i2c-$BUS map: $(/bin/busybox i2cdetect -y -r "$BUS" 2>&1 | sed -n '2,9p' | tr -s ' ' | tr '\n' '|')"
else
	info "I2C scan skipped (no *-002a client or no busybox)"
fi

echo "--- external storage (fakevold) ---"
if grep -q " /mnt/sdcard " /proc/mounts; then pass "card mounted at /mnt/sdcard"; else fail "card mounted at /mnt/sdcard"; fi
if pidof fakevold >/dev/null; then pass "fakevold running"; else fail "fakevold running"; fi
check "/mnt/sdcard is group sdcard_rw (1015)" sh -c '[ "$(stat -c %g /mnt/sdcard)" = 1015 ]'

echo "--- /data persistence ---"
check "settings/app data tar on the card" test -s /mnt/sd/android/data-cm7.tar
# (busybox runs `sleep` inside the shell, so the loop has no process to find:
# every save, and the boot restore, leaves a stamp instead)
check "saver has run (stamp present)" test -e /mnt/probe/dataflush-state.stamp
info "last saves: $(tail -n 3 /mnt/sd/CYANO3DS/dataflush.log 2>/dev/null | tr '\n' ' ')"
info "tars: $(ls -l /mnt/sd/android/*.tar 2>/dev/null | awk '{print $5, $NF}' | tr '\n' ' ')"

echo "--- screen timeout ---"
T=$(/system/xbin/sqlite3 /data/data/com.android.providers.settings/databases/settings.db \
	"SELECT value FROM system WHERE name='screen_off_timeout'" 2>/dev/null)
if [ "$T" = -1 ] || [ -s /mnt/sd/CYANO3DS/screen-timeout ]; then
	pass "screen_off_timeout=$T (never, or CYANO3DS/screen-timeout)"
else
	fail "screen_off_timeout=${T:-unreadable}: the screens will turn off and only a reboot brings them back"
fi

echo "--- top-screen console log ---"
if ps | grep -v grep | grep -q "logcat -v time"; then
	pass "Android log mirrored to the top screen"
else
	if [ -e /mnt/sd/CYANO3DS/noconsolelog ]; then info "console log disabled (CYANO3DS/noconsolelog)"
	else fail "Android log mirrored to the top screen"; fi
fi

echo "--- result: $PASS passed, $FAIL failed ---"
exit $FAIL
