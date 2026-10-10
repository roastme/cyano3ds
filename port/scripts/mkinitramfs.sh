#!/bin/bash
# mkinitramfs.sh - build the bring-up initramfs for the 3DS
#
# Produces <out>/initramfs.cpio.gz containing:
#   /init               the bring-up script (a REAL file - see the assertion
#                       at the end of this script, which checks the packed
#                       artefact, not the build tree)
#   /bin/busybox        static armel busybox + applet symlinks
#   /bin/fbtest         fbdev/display self test (diagnostic boots)
#   /bin/fbsay          renders text on a framebuffer; used to page the boot
#                       log onto the BOTTOM screen on diagnostic boots
#                       (CYANO3DS/diag).  Normal boots keep the log on the
#                       top-screen console.
#   /etc/inittab        insurance in case something ever runs busybox init
#
# The firm_linux_loader only reserves an 8 MiB window (0x27800000-0x28000000),
# so this stays small: Android's /system lives on the SD card.
#
# Boot log: init writes it to the SD card when the card is writable
# (SD:/CYANO3DS/*.log) and always renders it on the bottom screen.
#
# Usage: mkinitramfs.sh [out-dir]        (default $HOME/p3ds/dist)

set -euo pipefail

OUT="${1:-$HOME/p3ds/dist}"
MINIMAL_DIAGNOSTICS="${MINIMAL_DIAGNOSTICS:-0}"
# ---------------------------------------------------------------------------
# Android flavor.  The kernel and the bring-up machinery are shared; only the
# userspace files (the boot-stage directory, the /data restore tar name and the
# user-visible log strings) differ.  Default is CM7.2, the current target; the
# archived prebuilt flavors are still selectable with ANDROID_FLAVOR=froyo or
# ANDROID_FLAVOR=gingerbread.
# ---------------------------------------------------------------------------
ANDROID_FLAVOR="${ANDROID_FLAVOR:-cm7}"
case "$ANDROID_FLAVOR" in
	cm7)
		ANDROID_NAME="Android 2.3.7 (CyanogenMod 7.2)"
		ADIR="cm7-init"
		DATATAR="data-cm7.tar"
		;;
	gingerbread)
		ANDROID_NAME="Android 2.3.3 (Gingerbread)"
		ADIR="gingerbread-init"
		DATATAR="data-gingerbread.tar"
		;;
	froyo)
		ANDROID_NAME="Android 2.2 (Froyo)"
		ADIR="froyo-init"
		DATATAR="data-froyo.tar"
		;;
	*)
		ANDROID_FLAVOR="cm7"
		ANDROID_NAME="Android 2.3.7 (CyanogenMod 7.2)"
		ADIR="cm7-init"
		DATATAR="data-cm7.tar"
		;;
esac
PORT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$(cd "$PORT/../src" && pwd)"
ROOT="$(mktemp -d)"
# mktemp -d gives 0700; the initramfs root entry inherits that and non-root
# processes (Android's zygote runs as AID_NOBODY/9999 while preloading) then
# cannot search "/" at all, so every absolute open fails with EACCES.
chmod 0755 "$ROOT"

say() { printf '\n==> %s\n' "$*"; }
trap 'rm -rf "$ROOT"' EXIT

mkdir -p "$OUT" "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" \
	 "$ROOT/tmp" "$ROOT/mnt" "$ROOT/etc"
if [ "$MINIMAL_DIAGNOSTICS" = "1" ]; then
	: > "$ROOT/etc/3ds-minimal-diagnostics"
fi

# ---------------------------------------------------------------------------
say "Wi-Fi firmware (Atheros AR6014G / Octoblimp staging ath6k)"
# ---------------------------------------------------------------------------
# The Octoblimp driver boots the AR6014 from Nintendo's own four NWM blobs
# (see port/scripts/fix-wifi-octoblimp.py) and asks for them at
# ath6k/AR6002/nwm/.  The driver is built into the kernel and probes before the
# card is mounted, so the firmware has to live in the initramfs rootfs.
REPO_DIR="$(cd "$PORT/.." && pwd)"
# NWM_DIR overrides.  Default: firmware/ath6k/AR6002/nwm in the checkout.
NWM="${NWM_DIR:-$REPO_DIR/firmware/ath6k/AR6002/nwm}"
[ -f "$NWM/stub_code.bin" ] || NWM="$REPO_DIR/work/wifi/carve_norm/nwm"
if [ -f "$NWM/stub_code.bin" ] && [ -f "$NWM/main_type4.bin" ]; then
	mkdir -p "$ROOT/lib/firmware/ath6k/AR6002/nwm"
	cp "$NWM/stub_data.bin" "$NWM/stub_code.bin" \
	   "$NWM/main_type4.bin" "$NWM/database.bin" \
	   "$ROOT/lib/firmware/ath6k/AR6002/nwm/"
	# Also keep the other Main image around for an A/B run.
	[ -f "$NWM/main_type1.bin" ] && \
		cp "$NWM/main_type1.bin" "$ROOT/lib/firmware/ath6k/AR6002/nwm/"
	ls -l "$ROOT/lib/firmware/ath6k/AR6002/nwm" | sed 's/^/    /'
else
	echo "    WARNING: no NWM Wi-Fi blobs in $NWM (set NWM_DIR or copy them to firmware/ath6k/AR6002/nwm/; see docs/BUILD.md step 7) — ath6k will fail its firmware load" >&2
fi
# Keep the AR6014 SDK-style set around too (was the old mainline ath6kl path).
WFW="${WFW_DIR:-$REPO_DIR/firmware/ath6k/AR6014}"
if [ -f "$WFW/athwlan.bin.z77" ]; then
	mkdir -p "$ROOT/lib/firmware/ath6k/AR6014"
	cp "$WFW"/* "$ROOT/lib/firmware/ath6k/AR6014/" 2>/dev/null || true
fi
# And the generic AR6002 set.
WREV="${WREV_DIR:-$REPO_DIR/firmware/ath6k/AR6002}"
if [ -f "$WREV/athwlan.bin.z77" ]; then
	mkdir -p "$ROOT/lib/firmware/ath6k/AR6002"
	cp "$WREV"/*.bin "$WREV"/*.data "$ROOT/lib/firmware/ath6k/AR6002/" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
say "static busybox (armel)"
# ---------------------------------------------------------------------------
command -v arm-linux-gnueabi-gcc >/dev/null || {
	echo "arm-linux-gnueabi-gcc not found" >&2; exit 1; }

# Build busybox into the repo's tools/ dir by default (writable by the
# current user); override with BBDIR.  This used to be hardcoded to
# /root/p3ds/tools, which only worked when run as root.
BBDIR="${BBDIR:-$PORT/../tools}"
BB="$BBDIR/busybox-armel"
if [ ! -x "$BB" ]; then
	mkdir -p "$BBDIR"
	cd "$BBDIR"
	[ -d busybox-1.36.1 ] || tar xjf "$SRC/busybox-1.36.1.tar.bz2"
	cd busybox-1.36.1
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- defconfig >/dev/null
	sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
	sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j"$(nproc)" >/dev/null
	cp busybox "$BB"
fi
file "$BB" | sed 's/^/    /'
cp "$BB" "$ROOT/bin/busybox"

# Applet symlinks, relative to busybox like a normal install.  Deliberately no
# "init" symlink: /init must be our script (the first version symlinked /init to
# busybox and then wrote the script through the symlink - with the result that
# busybox's own `init` applet ran instead, spawning shells on non-existent
# /dev/tty2..tty4 and spamming "can't open /dev/ttyN" forever).
# Single source of truth for the applets the init script uses; the same list is
# asserted against `busybox --list` after packing.  (Keeping two lists bit me:
# `ln` was in the check but not in the symlink loop, so a symlink silently
# failed at runtime.)
INIT_APPLETS="sh mount umount ln ls cat echo sleep dmesg tee sync mkdir mknod
             chmod chown rm mv cp df stat true false
             grep tail head cut tr hexdump dd wc ps free kill pidof poweroff
             reboot basename dirname readlink sed test awk date sort nice
             tar timeout unzip"
for a in $INIT_APPLETS; do
	ln -sf busybox "$ROOT/bin/$a"
done

# ---------------------------------------------------------------------------
# lateprobe - one snapshot of the running Android bring-up
# ---------------------------------------------------------------------------
# The framework reaches systemReady and then deadlocks a few seconds later, so
# a single probe (and the Android log, which simply stops) is not enough to tell
# "hung" from "slow", or to name the deadlock.  This script is called several
# times from /init and from powerkey and appends everything relevant to
# late-diag.txt:
#
#   * ps + the service list                  - what is still alive/registered
#   * /proc/ctr_lcd_flips                    - is the compositor presenting?
#   * every system_server thread's state and kernel wait channel, plus the
#     first line of each kernel stack
#   * bounded dumpsys activity / window / SurfaceFlinger
#   * a Dalvik thread dump (SIGQUIT -> /data/anr/traces.txt)
#
# The Dalvik dump is the decisive one: hung-task detection only reports
# TASK_UNINTERRUPTIBLE (D) waits, and a userspace deadlock is an S-state futex
# wait, so CONFIG_DETECT_HUNG_TASK stays silent by design.
#
# Usage: lateprobe <label> [am]
#   the optional "am" also tries to start the HOME activity, which tells
#   "the home app was never started" apart from "ActivityManager is wedged".
cat > "$ROOT/bin/lateprobe" <<'LATEPROBE'
#!/bin/sh
# lateprobe - one snapshot of the running Android bring-up.
#
# The output goes to a tmpfs file (/tmp/late-diag.txt); a separate flusher in
# /init copies it to the SD card every 10 s.  That is deliberate.  The system
# has now frozen twice right when the app processes start doing heavy SD I/O
# (loop-mounted /system reads + log writes), and a probe that wrote to the SD
# on every sample would add to that load and would itself stop mid-write on a
# hang.  /bin/heartbeat (top screen) tells us whether the kernel is alive.
#
# Modes:
#   quick   ps / services / flips / dmesg tail
#   stack   quick + per-thread state/wchan/kernel stack + binder debugfs
#   full    stack + bounded dumpsys activity/window/SurfaceFlinger
#   java    full + a Dalvik thread dump (SIGQUIT -> /data/anr/traces.txt)
#   am      java + try to start the HOME activity
LABEL="${1:-probe}"
MODE="${2:-quick}"
# /init mounts a private tmpfs at /mnt/probe *before* Android's init remounts
# the rootfs read-only ("mount rootfs rootfs / ro remount" in Android's init.rc),
# which is exactly what made the previous build's /tmp writes fail with EROFS.
L=/mnt/probe/late-diag.txt

SYS=$(ps 2>/dev/null | awk '$NF == "system_server" { print $1; exit }')

{
	echo ""
	echo "===== probe $LABEL/$MODE  uptime=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)s  $(date 2>/dev/null) ====="
	echo "--- ps ---"
	ps
	echo "--- top CPU (ticks pid comm state) ---"
	# One awk over all of /proc/<pid>/stat: the field after the comm is state,
	# and utime/stime are fields 12/13 of the remainder.  This is what tells a
	# busy-looping app process from a blocked one, which ps alone does not.
	awk '{
		i = index($0, ")");
		pid = FILENAME; sub("/proc/", "", pid); sub("/stat", "", pid);
		comm = substr($0, index($0, "(") + 1, i - index($0, "(") - 1);
		rest = substr($0, i + 2);
		split(rest, f, " ");
		printf "%d %s %s %s\n", f[12] + f[13], pid, comm, f[1];
	}' /proc/[0-9]*/stat 2>/dev/null | sort -rn | head -15
	echo "--- app processes (comm contains a dot) ---"
	awk '{
		i = index($0, ")");
		pid = FILENAME; sub("/proc/", "", pid); sub("/stat", "", pid);
		comm = substr($0, index($0, "(") + 1, i - index($0, "(") - 1);
		rest = substr($0, i + 2);
		split(rest, f, " ");
		if (comm ~ /\./)
			printf "pid %s comm=%s state=%s cpu=%d\n", pid, comm, f[1], f[12] + f[13];
	}' /proc/[0-9]*/stat 2>/dev/null
	echo "--- service list ---"
	# /system/bin/service is a *Dalvik* command in Android: it starts a whole new
	# VM and reads am.jar off the SD card.  Doing that on every routine probe
	# added seconds of CPU and a burst of SD reads to the exact window we are
	# debugging (and tripled the probe cost).  Only the heavyweight stack/full
	# snapshots need it; the quick path uses the kernel binder state instead.
	if [ "$MODE" != "quick" ]; then
		/system/bin/service list 2>&1 | head -60
	else
		mount -t debugfs none /sys/kernel/debug 2>/dev/null
		grep -a "^proc\|^node" /sys/kernel/debug/binder/state 2>/dev/null | head -20 \
			|| echo "(binder debugfs unavailable; run a stack probe for the service list)"
	fi
	echo "--- ctr-lcd flips ---"
	cat /proc/ctr_lcd_flips 2>/dev/null || echo "(no /proc/ctr_lcd_flips)"
	echo "--- memory ---"
	free 2>/dev/null
	grep -E "^(MemTotal|MemFree|MemAvailable|Buffers|Cached|Dirty|Writeback|Slab|Shmem|SwapCached|Committed_AS|CommitLimit):" /proc/meminfo 2>/dev/null
	grep -E "(allocstall|pgscan_|pgsteal_|pgmajfault|pswpin|pswpout|nr_dirty|nr_writeback|nr_unstable)" /proc/vmstat 2>/dev/null | head -20
	echo "--- block/loop state ---"
	cat /sys/block/loop0/loop/backing_file 2>/dev/null
	for L in loop0 loop1; do
		[ -r "/sys/block/$L/stat" ] && echo "$L stat: $(cat /sys/block/$L/stat 2>/dev/null)"
	done
	echo "--- inspectable states (D = blocked) ---"
	for d in /proc/[0-9]*; do
		[ -d "$d" ] || continue
		st=$(sed 's/^.*) //' "$d/stat" 2>/dev/null | cut -d' ' -f1)
		[ "$st" = "D" ] || continue
		p=${d#/proc/}
		cm=$(cat "$d/comm" 2>/dev/null)
		echo "D pid $p comm=$cm wchan=$(cat $d/wchan 2>/dev/null)"
	done
	echo "--- dmesg tail ---"
	dmesg 2>/dev/null | tail -12
} >> "$L" 2>&1

[ "$MODE" = "quick" ] && exit 0

{
	mount -t debugfs none /sys/kernel/debug 2>/dev/null
	echo "--- binder state ---"
	head -80 /sys/kernel/debug/binder/state 2>/dev/null
	echo "--- binder transactions ---"
	head -300 /sys/kernel/debug/binder/transactions 2>/dev/null
	echo "--- sysrq-t (all task kernel stacks) ---"
	# MAGIC_SYSRQ is on: this prints every task's kernel stack to the kernel
	# log, which is the only reliable way to see a spin that /proc/<tid>/stack
	# returns empty for.  kmsgdump also captures it.
	echo t > /proc/sysrq-trigger 2>/dev/null
	sleep 1
	dmesg 2>/dev/null | tail -220
	for WHO in system_server zygote; do
		P=$(ps 2>/dev/null | awk -v n="$WHO" '$NF == n { print $1; exit }')
		[ -n "$P" ] || continue
		echo "--- $WHO ($P) threads: tid comm state wchan ---"
		for d in /proc/$P/task/*; do
			[ -d "$d" ] || continue
			t=${d##*/}
			cm=$(sed 's/^.*(//; s/).*$//' "$d/stat" 2>/dev/null)
			st=$(sed 's/^.*) //' "$d/stat" 2>/dev/null | cut -d' ' -f1)
			echo "$t $cm $st $(cat "$d/wchan" 2>/dev/null)"
		done
		echo "--- $WHO ($P) kernel stacks (top frame per thread) ---"
		for d in /proc/$P/task/*; do
			[ -d "$d" ] || continue
			t=${d##*/}
			echo "tid $t: $(head -1 "$d/stack" 2>/dev/null)"
		done
	done
	echo "--- user processes (state + wchan) ---"
	ps 2>/dev/null | awk 'NR > 1 && $NF !~ /^\[/ { print $1, $NF }' | while read p n; do
		[ -d "/proc/$p" ] || continue
		st=$(sed 's/^.*) //' "/proc/$p/stat" 2>/dev/null | cut -d' ' -f1)
		echo "pid $p $n state=$st wchan=$(cat /proc/$p/wchan 2>/dev/null)"
	done
} >> "$L" 2>&1

if { [ "$MODE" = "java" ] || [ "$MODE" = "am" ]; } && [ -n "$SYS" ]; then
	# SIGQUIT makes Dalvik write every Java thread stack of the target to
	# dalvik.vm.stack-trace-file (/data/anr/traces.txt).  Delete the old file
	# so Dalvik creates it with the right owner.  This suspends all VM
	# threads of the target for a few seconds, so it is only done by the
	# `java` mode, never by the routine `full`/`quick` snapshots.
	dump_java() {
		who="$1"; pid="$2"
		[ -n "$pid" ] || return 0
		[ -d "/proc/$pid" ] || return 0
		rm -f /data/anr/traces.txt 2>/dev/null
		kill -3 "$pid" 2>/dev/null
		sleep 5
		{
			echo "--- Dalvik dump of $who (pid $pid) ---"
			head -500 /data/anr/traces.txt 2>/dev/null || echo "(no traces file for $who)"
		} >> "$L" 2>&1
	}
	dump_java system_server "$SYS"
	# and every app process (comm contains a dot), one at a time
	for d in /proc/[0-9]*; do
		[ -d "$d" ] || continue
		p=${d#/proc/}
		# App processes have comm=app_process and the package name only in
		# cmdline, so match both (the old comm-only test found nothing and
		# the app's Java stacks were never captured).
		cm=$(cat "$d/comm" 2>/dev/null)
		cl=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)
		case "$cm $cl" in
		*.*) dump_java "$cm" "$p" ;;
		esac
	done
fi

if [ "$MODE" = "full" ] || [ "$MODE" = "am" ]; then
	# dumpsys goes through binder; a wedged service blocks the call, so bound
	# all three and never let one stall the snapshot.
	/system/bin/dumpsys SurfaceFlinger > /mnt/probe/d-sf.txt  2>&1 &
	p1=$!
	/system/bin/dumpsys window         > /mnt/probe/d-win.txt 2>&1 &
	p2=$!
	/system/bin/dumpsys activity       > /mnt/probe/d-act.txt 2>&1 &
	p3=$!
	sleep 12
	kill $p1 $p2 $p3 2>/dev/null
	{
		echo "--- dumpsys SurfaceFlinger ---"
		head -100 /mnt/probe/d-sf.txt 2>/dev/null
		echo "--- dumpsys window ---"
		head -100 /mnt/probe/d-win.txt 2>/dev/null
		echo "--- dumpsys activity ---"
		head -150 /mnt/probe/d-act.txt 2>/dev/null
	} >> "$L" 2>&1
fi

if [ "$MODE" = "am" ]; then
	# If this brings the UI up, ActivityManager is alive and only the
	# systemReady -> startHomeActivity step never ran.
	( /system/bin/am start -a android.intent.action.MAIN \
		-c android.intent.category.HOME > /mnt/probe/am.txt 2>&1 ) &
	p=$!
	sleep 20
	kill $p 2>/dev/null
	{
		echo "--- am start HOME ---"
		cat /mnt/probe/am.txt 2>/dev/null
	} >> "$L" 2>&1
fi
exit 0
LATEPROBE
chmod 755 "$ROOT/bin/lateprobe"

# ---------------------------------------------------------------------------
# firstframe-probe - three low-frequency Java snapshots during startup
# ---------------------------------------------------------------------------
cat > "$ROOT/bin/firstframe-probe" <<'FIRSTFRAME'
#!/bin/sh
# Capture SystemServer just before/after its systemReady handoff, without the
# recurring ps/service/dumpsys workload of lateprobe.  The delays are measured
# from init's SD mount (early in boot).  The boot now reaches
# ActivityManager "Start running!" / the home activity around t=90-120 s, so
# sample well past that to see whether the launcher ever draws and flips.
D="${1:-/mnt/sd/CYANO3DS}"
[ -d "$D" ] || exit 0
n=0
for delay in 68 12 8 8 10 20 40 60; do
	sleep "$delay"
	n=$((n + 1))
	TMP="/mnt/probe/firstframe-$n.txt"
	TRACE="/mnt/probe/firstframe-traces-$n.txt"
	P=$(ps 2>/dev/null | awk '$NF == "system_server" { print $1; exit }')
	: > "$TMP"
	{
		echo "===== first-frame sample $n uptime=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)s $(date 2>/dev/null) ====="
		echo "--- flips ---"
		cat /proc/ctr_lcd_flips 2>/dev/null
		echo "--- processes ---"
		ps
		if [ -n "$P" ] && [ -d "/proc/$P" ]; then
			echo "--- system_server pid=$P stat/wchan/syscall ---"
			cat "/proc/$P/stat" "/proc/$P/wchan" "/proc/$P/syscall" 2>/dev/null
			rm -f /data/anr/traces.txt "$TRACE" 2>/dev/null
			kill -3 "$P" 2>/dev/null
			sleep 3
			if [ -s /data/anr/traces.txt ]; then
				cp /data/anr/traces.txt "$TRACE" 2>/dev/null
				echo "Dalvik thread dump captured."
			else
				echo "Dalvik thread dump not available."
			fi
		else
			echo "system_server not found."
		fi
		# The home app runs in android.process.acore / com.android.launcher
		# on this build.  Capture it too: the panel only updates once it
		# draws its first frame, and that is what "stuck at flinger" is.
		A=$(ps 2>/dev/null | awk '$1 ~ /^[0-9]+$/ && /acore|launcher/ { print $1; exit }')
		if [ -n "$A" ] && [ -d "/proc/$A" ]; then
			echo "--- app pid=$A stat/wchan/syscall ---"
			cat "/proc/$A/stat" "/proc/$A/wchan" "/proc/$A/syscall" 2>/dev/null
		else
			echo "--- no home-app process yet ---"
		fi
	} >> "$TMP" 2>&1
	cp "$TMP" "$D/firstframe-$n.txt" 2>/dev/null
	[ -s "$TRACE" ] && cp "$TRACE" "$D/firstframe-traces-$n.txt" 2>/dev/null
	sync
 done
FIRSTFRAME
chmod 755 "$ROOT/bin/firstframe-probe"

# ---------------------------------------------------------------------------
# heartbeat - prove on the TOP screen that the kernel is still scheduling
# ---------------------------------------------------------------------------
# The system freezes hard (all logs stop at once, no hung-task report, no
# softlockup report).  A spin with IRQs off is invisible to every in-kernel
# detector, so this prints one short line to the console (fb0 = the top screen)
# every few seconds.  If the number keeps climbing after "the hang", the kernel
# is alive and only the SD write path is stuck; if it stops at N, the kernel
# itself stopped scheduling at that moment.  It deliberately touches nothing on
# the SD card.
cat > "$ROOT/bin/heartbeat" <<'HEARTBEAT'
#!/bin/sh
# heartbeat - prove the kernel is still scheduling, two ways that never touch
# the SD card:
#   * /dev/kmsg  -> printk -> console + kmsg.log, and
#   * fbsay on /dev/fb1 -> the BOTTOM screen, bypassing printk/console_lock.
#
# The machine freezes hard at ~80 s: every SD writer stops at once, no
# hung-task report, no softlockup report.  If the bottom screen keeps counting
# after everything else stops, the kernel is alive and only the console or
# storage path is stuck; if the number stops changing, the kernel itself
# stopped scheduling.  The bottom-screen copy exists because printk can block
# on console_lock, which would otherwise look identical to a frozen kernel.
# It also reports the kernel wait channel of the two processes that write the
# SD logs, so a stuck storage path shows up directly.
SYS=""; KM=""; LD=""
n=0
while true; do
	up=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)
	fl=$(awk '{print $2}' /proc/ctr_lcd_flips 2>/dev/null)
	{ [ -n "$SYS" ] && [ -d "/proc/$SYS" ]; } || SYS=$(ps 2>/dev/null | awk '$NF == "system_server" { print $1; exit }')
	{ [ -n "$KM" ] && [ -d "/proc/$KM" ]; } || KM=$(ps 2>/dev/null | awk '$4 == "kmsgdump" { print $1; exit }')
	{ [ -n "$LD" ] && [ -d "/proc/$LD" ]; } || LD=$(ps 2>/dev/null | awk '$4 == "logdump" { print $1; exit }')
	st="-"; wc="-"
	if [ -n "$SYS" ] && [ -d "/proc/$SYS" ]; then
		st=$(sed 's/^.*) //' "/proc/$SYS/stat" 2>/dev/null | cut -d' ' -f1)
		wc=$(cat "/proc/$SYS/wchan" 2>/dev/null)
	fi
	km=$(cat "/proc/$KM/wchan" 2>/dev/null)
	ld=$(cat "/proc/$LD/wchan" 2>/dev/null)
	# Top CPU-consuming thread of system_server.  If the freeze is a spinning
	# realtime userspace thread, this names it (cumulative utime+stime ticks).
	top=""; topn=0
	if [ -n "$SYS" ] && [ -d "/proc/$SYS/task" ]; then
		for d in /proc/$SYS/task/*; do
			[ -d "$d" ] || continue
			c=$(awk '{print $14+$15}' "$d/stat" 2>/dev/null)
			[ -n "$c" ] || continue
			if [ "$c" -gt "$topn" ]; then
				topn=$c
				top=${d##*/}
			fi
		done
	fi
	tcm="-"
	[ -n "$top" ] && tcm=$(sed 's/^.*(//; s/).*$//' "/proc/$top/stat" 2>/dev/null)
	# What is that thread actually doing?  If it is in a syscall,
	# /proc/<tid>/syscall gives the syscall number, args and the PC; the kernel
	# PC is field 30 (kstkeip) of /proc/<tid>/stat.  The last heartbeat before
	# the freeze therefore names the kernel path that is spinning.
	tst="-"; twc="-"; tsy="-"
	if [ -n "$top" ]; then
		tst=$(sed 's/^.*) //' "/proc/$top/stat" 2>/dev/null | cut -d' ' -f1)
		twc=$(cat "/proc/$top/wchan" 2>/dev/null)
		tsy=$(cat "/proc/$top/syscall" 2>/dev/null | tr '\t' ',')
	fi
	echo "<4>[hb $n] t=$up flips=$fl sys=$st/$wc top=$tcm($top)=$topn $tst $twc syscall=$tsy kmsgdump=$km logdump=$ld" > /dev/kmsg 2>/dev/null
	# NOTE: the top-screen liveness bar is drawn by a *separate* /bin/hbfb
	# --loop process started from /init, not from here: this line's printk can
	# block on console_lock, and the indicator must survive that.
	# Log-only heartbeat.  It is deliberately NOT drawn on the bottom screen:
	# that panel belongs to Android now, and clearing/repainting it every 6 s
	# wiped whatever the framework had composited (and raced SurfaceFlinger
	# for the single panel).  The kernel log is the record; the top screen is
	# the console.
	n=$((n+1))
	sleep 6
done
HEARTBEAT
chmod 755 "$ROOT/bin/heartbeat"

# ---------------------------------------------------------------------------
# appwatch - snapshot the first app process the instant it appears
# ---------------------------------------------------------------------------
# The machine may freeze within a second of the first app process forking, so
# the 3-second probe loop can miss it.  This polls /proc with shell builtins
# (no forks) once a second and fires a full stack + Java dump the moment a
# process whose comm contains a dot shows up.
cat > "$ROOT/bin/appwatch" <<'APPWATCH'
#!/bin/sh
# appwatch - snapshot the first app process the instant it appears.
#
# The machine stops within ~1 s of the first app process forking, so the
# snapshot MUST go straight to the SD card and be synced immediately; the
# buffered tmpfs+flusher design loses it.  The heavy (slow) probes run
# afterwards, after the fast one is already on the card.
D=/mnt/sd/CYANO3DS
[ -d "$D" ] || D=/mnt/probe
seen=""
lastpid=""
pending=""
n=0
while true; do
	cur=""; apppids=""
	# Cheap detection: /proc/loadavg's 5th field is the last PID the kernel
	# allocated, so only examine PIDs created since the previous poll.  The
	# old version scanned all of /proc four times a second at nice -20, used
	# 21 % CPU and tripled the boot time (it starved system_server).
	if [ -z "$lastpid" ]; then
		read _ _ _ _ lastpid _ < /proc/loadavg 2>/dev/null
		[ -n "$lastpid" ] || lastpid=0
	fi
	curpid=""
	read _ _ _ _ curpid _ < /proc/loadavg 2>/dev/null
	[ -n "$curpid" ] || curpid=0
	# re-check app_process of the previous polls: a freshly forked app still
	# carries zygote's argv[0] for a few ms before ActivityThread.setArgV0().
	left=""
	for p in $pending; do
		[ -d "/proc/$p" ] || continue
		a0=""
		[ -r "/proc/$p/cmdline" ] && a0=$(tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | head -1)
		case "$a0" in
		*.*) cur="$cur $a0"; apppids="$apppids $p" ;;
		*) left="$left $p" ;;
		esac
	done
	pending="$left"
	# app argv[0] is set within milliseconds; a 1 s window is plenty, and this
	# keeps a non-app app_process (system_server) from being re-read forever.
	[ $((n % 10)) = 0 ] && pending=""
	if [ "$curpid" != "$lastpid" ]; then
		p=$((lastpid + 1))
		[ "$curpid" -lt "$lastpid" ] 2>/dev/null && p=1
		while [ "$p" -le "$curpid" ]; do
			# PIDs allocated in the interval may already be gone (many
			# short-lived children), so never read /proc/<p>/... unguarded:
			# busybox ash reports the failed redirection on the console.
			if [ -d "/proc/$p" ]; then
				cm=""
				read cm < "/proc/$p/comm" 2>/dev/null
				case "$cm" in
				app_process|zygote)
					# A freshly forked app still carries the zygote's comm
					# ("app_process" / "zygote") until
					# ActivityThread.setArgV0() runs; watch its cmdline for
					# the dotted package name.
					a0=""
					[ -r "/proc/$p/cmdline" ] && a0=$(tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | head -1)
					case "$a0" in
					*.*) cur="$cur $a0"; apppids="$apppids $p" ;;
					*) pending="$pending $p" ;;
					esac ;;
				*.*)
					# A dotted comm is either an app whose name was set, or a
					# system_server thread (er.ServerThread).  Only a
					# thread-group leader is a process.
					tgid=""
					if [ -r "/proc/$p/status" ]; then
						while read -r k v _; do
							case "$k" in Tgid:) tgid="$v"; break;; esac
						done < "/proc/$p/status" 2>/dev/null
					fi
					if [ "$tgid" = "$p" ]; then
						cur="$cur $cm"; apppids="$apppids $p"
					fi ;;
				esac
			fi
			p=$((p + 1))
		done
		lastpid=$curpid
	fi
	if [ -n "$cur" ] && [ "$cur" != "$seen" ]; then
		seen="$cur"
		echo "<4>appwatch: app pids=$apppids up=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)s" > /dev/kmsg 2>/dev/null
		# RAPID TRACE: the machine stops ~1 s after the fork, so sample the
		# app and system_server every ~0.3 s, syncing each time, so the last
		# sample before the freeze is safely on the card.
		SYS=""
		for q in /proc/[0-9]*; do
			[ -d "$q" ] || continue
			read cm < "$q/comm" 2>/dev/null
			[ "$cm" = "app_process" ] || continue
			a0=""
			[ -r "$q/cmdline" ] && a0=$(tr '\0' '\n' < "$q/cmdline" 2>/dev/null | head -1)
			[ "$a0" = "system_server" ] && SYS=${q#/proc/}
		done
		r=0
		# One-time per-thread dump of the app(s) and system_server, taken the
		# instant the app appears: the freeze is ~2 s later, and this names the
		# thread and the syscall that is spinning or blocked.
		{
			echo "===== thread dump at app detection: up=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)s pids=$apppids sys=$SYS ====="
			for p in $apppids $SYS; do
				[ -d "/proc/$p" ] || continue
				for t in /proc/$p/task/*; do
					[ -d "$t" ] || continue
					ti=${t##*/}
					tst=$(sed 's/^.*) //' "$t/stat" 2>/dev/null | cut -d' ' -f1)
					tcm=$(sed 's/^.*(//; s/).*$//' "$t/stat" 2>/dev/null)
					tcp=$(sed 's/^.*) //' "$t/stat" 2>/dev/null | awk '{print $12+$13}')
					echo " pid $p tid $ti comm=$tcm state=$tst cpu=$tcp wchan=$(cat "$t/wchan" 2>/dev/null) syscall=$(cat "$t/syscall" 2>/dev/null | tr '\t' ',')"
				done
			done
		} >> "$D/rapid.txt" 2>&1
		sync
		while [ $r -lt 300 ]; do
			{
				echo "=t $(cut -d' ' -f1 /proc/uptime 2>/dev/null) flips $(awk '{print $2}' /proc/ctr_lcd_flips 2>/dev/null)"
				for p in $apppids; do
					if [ -d "/proc/$p" ]; then
						rst=$(sed 's/^.*) //' "/proc/$p/stat" 2>/dev/null)
						echo " app $p $(echo "$rst" | cut -d' ' -f1) cpu$(echo "$rst" | awk '{print $12+$13}') wchan=$(cat /proc/$p/wchan 2>/dev/null) syscall=$(cat /proc/$p/syscall 2>/dev/null | tr '\t' ',')"
					else
						echo " app $p GONE"
					fi
				done
				if [ -n "$SYS" ] && [ -d "/proc/$SYS" ]; then
					rst=$(sed 's/^.*) //' "/proc/$SYS/stat" 2>/dev/null)
					echo " sys $SYS $(echo "$rst" | cut -d' ' -f1) cpu$(echo "$rst" | awk '{print $12+$13}') wchan=$(cat /proc/$SYS/wchan 2>/dev/null) syscall=$(cat /proc/$SYS/syscall 2>/dev/null | tr '\t' ',')"
				fi
			} >> "$D/rapid.txt" 2>&1
			# Sync rarely: a sync() every 0.3 s hammered the FAT, and a
			# blocked sync stops the trace exactly when it is needed.
			[ $((r % 15)) = 0 ] && sync
			r=$((r+1))
			sleep 0.2
		done
		# FASTEST first: the app's own /proc state, straight to the SD card
		# and synced, before the slow whole-system view.
		{
			echo ""
			echo "===== appwatch: app at uptime=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)s: $cur pids=$apppids ====="
			echo "--- ctr-lcd flips ---"; cat /proc/ctr_lcd_flips 2>/dev/null
			for p in $apppids; do
				echo "--- pid $p ---"
				echo "stat:  $(cat /proc/$p/stat 2>/dev/null)"
				echo "wchan: $(cat /proc/$p/wchan 2>/dev/null)"
				echo "stack: $(head -5 /proc/$p/stack 2>/dev/null | tr '\n' '|')"
				for t in /proc/$p/task/*; do
					[ -d "$t" ] || continue
					echo "  tid ${t##*/} state=$(sed 's/^.*) //' "$t/stat" 2>/dev/null | cut -d' ' -f1) wchan=$(cat "$t/wchan" 2>/dev/null)"
				done
			done
		} >> "$D/appwatch.txt" 2>&1
		sync
		echo "<4>appwatch: app pids=$apppids up=$(cut -d' ' -f1 /proc/uptime 2>/dev/null)s flips=$(awk '{print $2}' /proc/ctr_lcd_flips 2>/dev/null)" > /dev/kmsg 2>/dev/null
		# slower: whole-system view
		{
			echo "--- ps ---"; ps
			echo "--- top CPU (ticks pid comm state) ---"
			awk '{ i=index($0,")"); pid=FILENAME; sub("/proc/","",pid); sub("/stat","",pid);
			       comm=substr($0,index($0,"(")+1,i-index($0,"(")-1); rest=substr($0,i+2);
			       split(rest,f," "); printf "%d %s %s %s\n", f[12]+f[13], pid, comm, f[1]; }' \
			    /proc/[0-9]*/stat 2>/dev/null | sort -rn | head -20
		} >> "$D/appwatch.txt" 2>&1
		sync
		# heavy dumps now that the fast snapshot is safe on the card
		/bin/lateprobe "watch" stack
		/bin/lateprobe "watchj" java
		cp /mnt/probe/late-diag.txt "$D/late-diag.txt" 2>/dev/null
		[ -s /data/anr/traces.txt ] && cp /data/anr/traces.txt "$D/traces.txt" 2>/dev/null
		sync
	fi
	n=$((n+1))
	# userspace SD heartbeat: if this stops while the kernel's own /dev/kmsg
	# heartbeat (kmsg.log) keeps going, only the SD writers are stuck; if both
	# stop, the kernel stopped too.  Written every ~2 s to keep the sync load
	# low.
	if [ $((n % 100)) = 0 ]; then
		echo "watchbeat $n up=$(cut -d' ' -f1 /proc/uptime 2>/dev/null) flips=$(awk '{print $2}' /proc/ctr_lcd_flips 2>/dev/null)" >> /mnt/probe/watchbeat.txt 2>/dev/null
	fi
	sleep 0.1
done
APPWATCH
chmod 755 "$ROOT/bin/appwatch"

# busybox-init insurance: if anything ever runs busybox init again, give it an
# inittab that only opens the console instead of tty2/tty3/tty4.
cat > "$ROOT/etc/inittab" <<'INITTAB'
::askfirst:-/bin/sh
INITTAB

# ---------------------------------------------------------------------------
say "fbtest + fbsay"
# ---------------------------------------------------------------------------
arm-linux-gnueabi-gcc -static -O2 -Wall -o "$ROOT/bin/fbtest" \
	"$PORT/userland/tests/fbtest.c"
arm-linux-gnueabi-gcc -static -O2 -Wall -I"$PORT/userland/tests" \
	-o "$ROOT/bin/fbsay" "$PORT/userland/tests/fbsay.c"
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/logdump" "$PORT/userland/tests/logdump.c"
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/kmsgdump" "$PORT/userland/tests/kmsgdump.c"
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/androidtest" "$PORT/userland/tests/androidtest.c"
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/trace" "$PORT/userland/tests/trace.c"
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/powerkey" "$PORT/userland/tests/powerkey.c"
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/logwrap" "$PORT/userland/tests/logwrap.c"
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/rt" "$PORT/userland/tests/rt.c"
# hbfb: paint a liveness progress bar straight into a framebuffer (used by
# /bin/heartbeat so the top screen proves the kernel/scheduler is still alive
# even when the SD write path and printk are wedged).
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/hbfb" "$PORT/userland/tests/hbfb.c"
# tsdump: raw evdev dump of the 3DS touchscreen, so an M4 boot answers whether
# the input core delivers ABS_X/ABS_Y + BTN_TOUCH to userspace at all.
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/tsdump" "$PORT/userland/tests/tsdump.c"
# tsmark: draw the touch position on the TOP screen, independent of Android, so
# a tap can be seen to land where the finger is (or not).
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/tsmark" "$PORT/userland/tests/tsmark.c"
# tscal: one-shot touchscreen calibration.  Draws five targets on the bottom
# screen, records the raw taps, scores the eight candidate mappings, fits the
# affine calibration and writes both to /proc and the SD card.  -lm for the
# least-squares residual (static link).
arm-linux-gnueabi-gcc -static -O2 -Wall \
	-o "$ROOT/bin/tscal" "$PORT/userland/tests/tscal.c" -lm
# inputtest: simple on-screen touchscreen + button test.  /init runs it before
# Android when CYANO3DS/inputtest is on the card (see the handover below),
# so the user can verify evdev independently of Android's input pipeline.
arm-linux-gnueabi-gcc -static -O2 -Wall -I"$PORT/userland/tests" \
	-o "$ROOT/bin/inputtest" "$PORT/userland/tests/inputtest.c"
# alsaprobe: optional bionic ALSA PCM probe, built separately (arm-eabi,
# links libasound).  /init runs it
# just before Android (see the handover below) so a kernel PCM hang can be told
# apart from a CM7 hardware/alsa_sound hang.  Skipped if it was not built.
if [ -f "$OUT/alsaprobe" ]; then
	install -m 755 "$OUT/alsaprobe" "$ROOT/bin/alsaprobe"
fi
ls -l "$ROOT/bin/fbtest" "$ROOT/bin/fbsay" "$ROOT/bin/logdump" \
      "$ROOT/bin/kmsgdump" "$ROOT/bin/androidtest" "$ROOT/bin/powerkey" \
      "$ROOT/bin/hbfb" "$ROOT/bin/tsdump" "$ROOT/bin/tsmark" "$ROOT/bin/tscal" \
      "$ROOT/bin/inputtest" \
      | awk '{print "   ", $5, $9}'

# ---------------------------------------------------------------------------
# PortHelper (porthelper.jar) - keep the screen awake + dismiss the keyguard.
#
# The port has no display power management, so PowerManagerService runs its
# normal userspace screen-off timer.  Once the "screen" turns off (mPowerState
# loses SCREEN_ON_BIT/SCREEN_BRIGHT_BIT) WindowManagerService drops every touch
# and consumes every WAKE_DROPPED key, so the UI looks frozen and the panel
# goes dark -- exactly the "audio plays, then the screen goes off and nothing
# responds" report.  `svc power stayon true` only holds a SCREEN_DIM lock while
# mIsPowered, and the 3DS reports a *discharging* battery, so it is not enough.
#
# This smali helper polls IPowerManager.userActivity() every 5 s (which resets
# the timeout and keeps the bright bit) and calls
# IWindowManager.disableKeyguard() once.  It only uses interface methods whose
# signatures are stable across the Android flavors --
# userActivity(JZ)V, disableKeyguard(IBinder,String),
# inKeyguardRestrictedInputMode()Z -- so the same jar works on every flavor
# (an old note claiming "the archived flavor's binder interface is different" was
# wrong: the difference is the *jar that was deployed*, not the interface).
# It runs as root, so the DEVICE_POWER/DISABLE_KEYGUARD checks pass.
# ---------------------------------------------------------------------------
SMALI_JAR="$PORT/../tools/smali.jar"
if [ -f "$SMALI_JAR" ] && command -v java >/dev/null 2>&1 && \
   command -v zip >/dev/null 2>&1; then
	SB="$(mktemp -d)"
	cp "$PORT/initramfs/PortHelper.smali" "$SB/"
	if ( cd "$SB" && java -jar "$SMALI_JAR" a . -o classes.dex >/dev/null 2>&1 ) && \
	   ( cd "$SB" && zip -q -j porthelper.jar classes.dex ); then
		install -m 644 "$SB/porthelper.jar" "$ROOT/bin/porthelper.jar"
	fi
	rm -rf "$SB"
fi
if [ -f "$ROOT/bin/porthelper.jar" ]; then
	echo "    /bin/porthelper.jar installed (keep-awake + keyguard helper)"
else
	echo "    WARNING: could not build porthelper.jar (java/smali/zip missing); the screen will time out" >&2
fi

# ---------------------------------------------------------------------------
# Legacy prebuilt boot stage: the archived flavor's init binary plus the init.rc that
# legacy/build-android.sh patched for the 3DS (mounts removed, fb symlink, props).
# If they are not there the script fails (C-19); set ALLOW_NO_ANDROID=1 to
# build a bring-up-only initramfs anyway.
# ---------------------------------------------------------------------------
AD="$PORT/../out/$ADIR"
if [ -x "$AD/init" ] && [ -f "$AD/init.rc" ]; then
	install -m 755 "$AD/init" "$ROOT/bin/android-init"
	install -m 644 "$AD/init.rc" "$ROOT/init.rc"
	# Bring-up service tweaks:
	#   * servicemanager: drop 'critical' (its crash-loop made init "reboot
	#     into recovery") and run it as root (as 'system' it was not becoming
	#     the binder context manager);
	#   * media: run as root too, so logwrap can log and to rule out a uid
	#     problem (AudioFlinger never got published, stalling system_server).
	python3 - "$ROOT/init.rc" <<'PY'
import sys
p = sys.argv[1]
lines = open(p).read().splitlines(keepends=True)
out, svc, dropped = [], None, []
for ln in lines:
    if ln.startswith("service "):
        svc = ln.split()[1]
    if svc == "servicemanager" and ln.strip() == "critical":
        continue
    if svc == "servicemanager" and ln.strip() == "user system":
        dropped.append("servicemanager->root")
        continue
    if svc == "media" and ln.strip() == "user media":
        dropped.append("media->root")
        continue
    out.append(ln)
open(p, "w").write("".join(out))
print("    init.rc: servicemanager no longer 'critical' (no reboot loop)")
for d in dropped:
    print("    init.rc: %s (bring-up)" % d)
PY
	# The 3DS CM7 boot stage.
	#
	# The screen is kept on by `svc power stayon true` further down (the
	# battery shim reports AC online); the notification shade is left as the
	# normal, working CM7 behaviour.
	# The Dalvik heap is already set in init.rc/default.prop by the flavor build.
	# Bring-up: wrap zygote/servicemanager/media in /bin/logwrap so their exit
	# status, signal and stderr are not swallowed by init.
	sed -i 's|^service zygote /system/bin/app_process |service zygote /bin/logwrap /system/bin/app_process |' "$ROOT/init.rc"
	grep -q "^service zygote /bin/logwrap " "$ROOT/init.rc" && echo "    init.rc: zygote wrapped in logwrap"
	sed -i 's|^service servicemanager /system/bin/servicemanager|service servicemanager /bin/logwrap /system/bin/servicemanager|' "$ROOT/init.rc"
	grep -q "^service servicemanager /bin/logwrap " "$ROOT/init.rc" && echo "    init.rc: servicemanager wrapped in logwrap"
	sed -i 's|^service media /system/bin/mediaserver|service media /bin/logwrap /system/bin/mediaserver|' "$ROOT/init.rc"
	grep -q "^service media /bin/logwrap " "$ROOT/init.rc" && echo "    init.rc: media wrapped in logwrap"
	# ---------------------------------------------------------------------
	# N3DS_CM7_DHCP_PROPERTY_PUBLISHER
	#
	# CM7 does not run dhcpcd the way it is documented to be run.  The
	# framework does not fork it: WifiStateTracker calls
	# NetworkUtils.runDhcp() -> system/core/libnetutils/dhcp_utils.c
	# dhcp_do_request(), which does
	#
	#   property_set("ctl.start", "dhcpcd_wlan0:wlan0");
	#   wait_for_property("dhcpcd_wlan0", "running", 10);   <-- step 1
	#   wait_for_property("dhcp.wlan0.result", NULL, 30);   <-- step 2
	#   ... then reads dhcp.wlan0.{ipaddress,mask,gateway,dns1,dns2,server,
	#   leasetime} and requires result == "ok".
	#
	# Nothing in the system publishes those properties here.  The stock
	# publisher is the dhcpcd hook /system/etc/dhcpcd/dhcpcd-hooks/
	# 95-configured ("setprop dhcp.${interface}.result ok"), and dhcpcd does
	# run it -- the card log shows "executing `...dhcpcd-run-hooks', reason
	# BOUND" right after it leases.  It cannot work, because
	# external/dhcpcd/dhcpcd.c:649 calls switchUser() as the *first* statement
	# of main():
	#
	#   setgroups({AID_INET=3003, AID_SHELL=2000});
	#   setgid(AID_DHCP); setuid(AID_DHCP);            /* uid 1014 */
	#
	# and init's property ACL (system/core/init/property_service.c:77) allows
	# the "dhcp." prefix only to AID_SYSTEM (1000) or AID_ROOT -- the check is
	# on the peer's uid *and* its single primary gid, and dhcpcd's primary gid
	# is 1014 while its supplementary groups are 3003/2000.  So every setprop
	# in the hook is refused.
	#
	# The observable result on the card is the whole failure mode of this port:
	#
	#   dhcpcd: offered 192.168.1.100 from 192.168.1.1
	#   dhcpcd: acknowledged 192.168.1.100 from 192.168.1.1
	#   dhcpcd: leased 192.168.1.100 for 86400 seconds
	#   dhcpcd: adding IP address 192.168.1.100/24
	#   WifiStateTracker: DHCP request failed: Timed out waiting for DHCP to finish
	#
	# -- DHCP works, the interface is configured, and the framework never
	# hears about it, so it disconnects the Wi-Fi it just authenticated.
	#
	# Fix: run dhcpcd as before (it keeps CAP_NET_ADMIN/NET_RAW after
	# switchUser() and configures the interface itself), but publish the
	# properties ourselves from a *root* context, which init's ACL always
	# allows (check_perms() returns 1 immediately for uid 0).  The hook we
	# install (-c) runs as dhcpcd's uid and only records the lease into a
	# file; the service wrapper waits for that file -- or, if the file never
	# arrives, for dhcpcd to have configured the interface -- and does the
	# setprops.  The wrapper also sets "dhcpcd_wlan0=running" so step 1 does
	# not depend on anything setting a property of that bare name, and it
	# skips the stock 95-configured hook so a *stopped* client cannot set
	# result=failed underneath a request that is still waiting for "ok".
	#
	# (Correcting the ACL note: init's property_perms table does also list
	# {"dhcp.", AID_DHCP, 0}, so the hook's setprops are not all refused --
	# the 2026-10-04 18:36 boot caught the framework reading a result of
	# "failed" that only the hook could have written.  The reason to publish
	# from root anyway is ordering and ownership, not the ACL: one publisher,
	# one writer, no race with a dying client.)
	#
	# (Octoblimp's port solves the same problem the same way for the same
	# framework generation: scripts/android_udhcpc.sh + a "service dhcpcd" in
	# its init.rc that translate a busybox udhcpc lease into dhcp.<iface>.*
	# with setprop.)
	cat > "$ROOT/bin/dhcpcd-hook.sh" <<'HOOK'
#!/system/bin/sh
# N3DS_CM7_DHCP_PROPERTY_PUBLISHER, hook half.  Installed with dhcpcd -c, so it
# runs *instead of* the stock /system/etc/dhcpcd/dhcpcd-run-hooks, as dhcpcd's
# uid 1014 (switchUser()).
#
# 2026-10-04 22:1x: the 21:5x boot ran this hook for real -- logcat has
# "dhcpcd: executing `/bin/dhcpcd-hook.sh', reason BOUND" at 22:14:08, right
# after "leased 192.168.1.100" -- and produced *nothing*: no trace line, no
# lease file, no dhcp.wlan0.* properties (the framework timed out again), and
# no "sys_prop: permission denied" in kmsg, i.e. setprop was never even
# attempted.  Everything the previous hook did before its first visible action
# was `skip_hooks=configured` and `. /system/etc/dhcpcd/dhcpcd-run-hooks`, so
# the stock chain is gone now and the publication happens first, in the first
# statements, with a file marker that survives to the SD card (logcat is not
# available here: /dev/socket/logdriv is root:log 0771 and dhcpcd's gid is
# 1014, so `log` from this hook cannot connect -- that is why a hook trace has
# never been seen).
#
# What the stock chain was worth: 20-dns.conf setpropped dhcp.<iface>.dns1..4,
# which this script does itself, and 95-configured is exactly the stock
# publisher whose "result=failed" on a dying client raced the framework (the
# 18:36 boot read that six seconds into a fresh request).  Nothing else in it
# matters: resolv.conf is not read by anything on this ROM (bionic reads
# net.dns*, which only uid 0 can set, and the root wrapper sets it from the
# lease file written below).
skip_hooks=configured
export skip_hooks

MARK=/data/misc/dhcp/hook.ran

# Two traces, both working as uid 1014:
#   * /system/bin/log -> logcat, when the log socket lets us in
#   * a marker file -> archived by /bin/dataflush into android/data-cm7.tar on
#     the SD card, which is always readable from the PC
log() {
	/system/bin/log -t N3DS-DHCP "$1" 2>/dev/null
	echo "$1" >> "$MARK" 2>/dev/null
}

mkdir -p /data/misc/dhcp 2>/dev/null
log "hook entered reason=$reason iface=$interface"

case "$reason" in
BOUND|RENEW|REBIND|REBOOT)
	log "lease ip=${new_ip_address} mask=${new_subnet_mask} gw=${new_routers} dns=${new_domain_name_servers} lease=${new_dhcp_lease_time} server=${new_dhcp_server_identifier}"

	# Publish first, record second: the properties are what the framework
	# waits for, the file is only the root half's source for net.dns*.
	# uid 1014 may set dhcp.* (init's property_perms has {"dhcp.", AID_DHCP}).
	setprop dhcp.${interface}.ipaddress "${new_ip_address}"
	setprop dhcp.${interface}.mask "${new_subnet_mask}"
	setprop dhcp.${interface}.gateway "${new_routers%% *}"
	setprop dhcp.${interface}.server "${new_dhcp_server_identifier}"
	setprop dhcp.${interface}.leasetime "${new_dhcp_lease_time}"
	setprop dhcp.${interface}.dns1 "${new_domain_name_servers%% *}"
	rest="${new_domain_name_servers#* }"
	case "$rest" in
	"${new_domain_name_servers}") setprop dhcp.${interface}.dns2 "" ;;
	*) setprop dhcp.${interface}.dns2 "${rest%% *}" ;;
	esac
	log "set lease properties, result now [$(getprop dhcp.${interface}.result)]"

	# Values are single-quoted: new_routers and new_domain_name_servers are
	# space-separated lists and the file is sourced by the root half, where an
	# unquoted "var=a b" line is a command invocation, not an assignment.
	if (
		echo "new_ip_address='${new_ip_address}'"
		echo "new_subnet_mask='${new_subnet_mask}'"
		echo "new_routers='${new_routers}'"
		echo "new_domain_name_servers='${new_domain_name_servers}'"
		echo "new_dhcp_server_identifier='${new_dhcp_server_identifier}'"
		echo "new_dhcp_lease_time='${new_dhcp_lease_time}'"
	) > /data/misc/dhcp/wlan0.lease.env 2>/dev/null; then
		log "lease file written"
	else
		log "lease file write FAILED"
	fi

	setprop dhcp.${interface}.result ok
	log "set dhcp.${interface}.result ok, readback [$(getprop dhcp.${interface}.result)]"
	;;
*)
	log "reason=$reason (publishing nothing)"
	;;
esac
exit 0
HOOK
	chmod 0755 "$ROOT/bin/dhcpcd-hook.sh"

	cat > "$ROOT/bin/dhcpcd_wlan0.sh" <<'PUBLISHER'
#!/system/bin/sh
# N3DS_CM7_DHCP_PROPERTY_PUBLISHER, root half.  Runs as the init service
# "dhcpcd_wlan0" (user root): it starts the real dhcpcd, then publishes the
# lease twice over -- from the hook's lease file, or, failing that, from the
# interface state -- and is the only thing that can set net.dns*, which bionic
# and the framework read for name resolution and which uid 1014 is refused.
#
# The interface fallback is what makes the publication independent of dhcpcd's
# hook mechanism: the 21:5x boot proved the hook can be announced by dhcpcd
# and still produce nothing at all, and nothing in the framework's contract
# requires that file.  dhcpcd configuring wlan0 is the one thing this system
# has always done reliably ("leased 192.168.1.100", "adding route 0.0.0.0/0 via
# 192.168.1.1" in every capture).
#
# Deliberately flat: no shell functions, no command substitution in the path
# that publishes.  A previous version with busybox helper bindings and a
# /proc/net/route parser vanished silently on two boots in a row.
DHCP_CONF=/system/etc/dhcpcd/dhcpcd.conf
HOOK=/bin/dhcpcd-hook.sh
LEASE=/data/misc/dhcp/wlan0.lease.env
IFACE=wlan0
BB=/bin/busybox

L() {
	echo "N3DS-DHCP: $*" > /dev/kmsg 2>/dev/null
	/system/bin/log -t N3DS-DHCP "$*" 2>/dev/null
}

mkdir -p /data/misc/dhcp 2>/dev/null
chown 1014:1014 /data/misc/dhcp 2>/dev/null
chmod 0777 /data/misc/dhcp 2>/dev/null
L "wrapper up args=$* pid=$$"

# step 1 of dhcp_do_request(): property "dhcpcd_wlan0" == "running"
setprop dhcpcd_wlan0 running
L "step1 dhcpcd_wlan0=[$(getprop dhcpcd_wlan0)]"

# A dhcpcd orphaned by an earlier ctl.stop (init SIGTERMs the *service*, this
# script, not the dhcpcd it forked) keeps running and fights the next one for
# wlan0.
for p in $(/bin/pidof dhcpcd 2>/dev/null); do
	L "killing stale dhcpcd pid $p"
	kill "$p" 2>/dev/null
done

rm -f "$LEASE"
/system/bin/dhcpcd -dd -f "$DHCP_CONF" -c "$HOOK" "$IFACE" &
DHCPCD_PID=$!
L "dhcpcd started pid=$DHCPCD_PID"

# ctl.stop has to take the client down with us, not orphan it.
trap 'kill $DHCPCD_PID 2>/dev/null; L "SIGTERM, stopping dhcpcd $DHCPCD_PID"; exit 0' TERM INT

# hex little-endian word -> dotted quad (a gateway or a netmask out of
# /proc/net/route).  $((0x..)) is POSIX arithmetic and cut is in the ramdisk's
# busybox; ${var:off:len} would be shorter but is not POSIX (dash rejects it,
# and the device shell being tested only ever *seemed* to accept it).
hexip() {
	h="$1"
	o1=${h#??????}
	_o=${h#????}; o2=${_o%??}
	_o=${h#??};   o3=${_o%????}
	o4=${h%??????}
	echo $((0x$o1)).$((0x$o2)).$((0x$o3)).$((0x$o4))
}

i=0
while [ "$i" -lt 40 ]; do
	ip=""
	mask=""
	gw=""
	dns1=""
	dns2=""
	lease=""
	server=""

	if [ -s "$LEASE" ]; then
		. "$LEASE"
		ip="$new_ip_address"
		mask="$new_subnet_mask"
		gw="${new_routers%% *}"
		dns1="${new_domain_name_servers%% *}"
		rest="${new_domain_name_servers#* }"
		case "$rest" in
		"${new_domain_name_servers}") dns2="" ;;
		*) dns2="${rest%% *}" ;;
		esac
		lease="$new_dhcp_lease_time"
		server="$new_dhcp_server_identifier"
		L "lease file: ip=$ip gw=$gw dns=$dns1"
	fi

	# Fallback: ask the kernel what address dhcpcd put on the interface.  The
	# Local table in /proc/net/fib_trie lists it on its own line followed by
	# "/32 host LOCAL" (this is the 5.x format, not the older "IP/32" one),
	# and /proc/net/route carries the default route's gateway and netmask.
	if [ -z "$ip" ]; then
		ip=$($BB awk '/ [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/ { ip = $NF }
			/\/32 host LOCAL/ { if (ip != "" && ip !~ /^127\./) { print ip; exit } }' \
			/proc/net/fib_trie 2>/dev/null)
		if [ -n "$ip" ]; then
			gwh=""
			mh=""
			route=$($BB awk -v i="$IFACE" '$1==i && $2=="00000000" {print $3" "$8; exit}' /proc/net/route 2>/dev/null)
			gwh=${route%% *}
			mh=${route##* }
			[ -n "$gwh" ] && gw=$(hexip "$gwh")
			[ -n "$mh" ] && mask=$(hexip "$mh")
			L "interface fallback: ip=$ip mask=$mask gw=$gw"
		fi
	fi

	if [ -n "$ip" ]; then
		setprop dhcp.$IFACE.ipaddress "$ip"
		[ -n "$mask" ] && setprop dhcp.$IFACE.mask "$mask"
		[ -n "$gw" ] && setprop dhcp.$IFACE.gateway "$gw"
		[ -n "$server" ] && setprop dhcp.$IFACE.server "$server"
		setprop dhcp.$IFACE.leasetime "${lease:-86400}"
		# net.dns*: uid 1014 is refused these ("net." is AID_SYSTEM only).
		if [ -n "$dns1" ]; then
			setprop net.dns1 "$dns1"
			[ -n "$dns2" ] && setprop net.dns2 "$dns2"
		elif [ -n "$gw" ]; then
			setprop net.dns1 "$gw"
		fi
		setprop dhcp.$IFACE.result ok
		L "published: result=[$(getprop dhcp.$IFACE.result)] ip=[$(getprop dhcp.$IFACE.ipaddress)] gw=[$(getprop dhcp.$IFACE.gateway)] mask=[$(getprop dhcp.$IFACE.mask)] dns1=[$(getprop net.dns1)]"
		break
	fi

	if [ $((i % 5)) -eq 0 ]; then
		[ -s "$LEASE" ] && lf=yes || lf=no
		L "waiting ${i}s: leasefile=$lf result=[$(getprop dhcp.$IFACE.result)]"
	fi
	i=$((i + 1))
	sleep 1
done

if [ "$i" -ge 40 ]; then
	L "no lease after ${i}s: result was [$(getprop dhcp.$IFACE.result)]"
	setprop dhcp.$IFACE.result failed
fi

# Stay alive as long as dhcpcd so init tracks the client (stock service
# semantics: ctl.stop stops dhcpcd, not an orphan).
wait "$DHCPCD_PID" 2>/dev/null
L "wrapper exit: dhcpcd $DHCPCD_PID gone"
PUBLISHER
	chmod 0755 "$ROOT/bin/dhcpcd_wlan0.sh"

	sed -i 's|^service dhcpcd_wlan0 /system/bin/dhcpcd |service dhcpcd_wlan0 /bin/dhcpcd_wlan0.sh # was: /system/bin/dhcpcd |' "$ROOT/init.rc"
	if grep -q "^service dhcpcd_wlan0 /bin/dhcpcd_wlan0.sh" "$ROOT/init.rc"; then
		echo "    init.rc: dhcpcd_wlan0 -> /bin/dhcpcd_wlan0.sh (property publisher)"
	fi
	install -m 644 "$AD/default.prop" "$ROOT/default.prop"
	# ro.secure=0 (the SDK/emulator default) makes every background ANR pop an
	# AppNotRespondingDialog.  That window carries FLAG_SYSTEM_ERROR, and Android's
	# WindowManagerService.findTargetWindow() drops *all* pointer events that do
	# not land on an error window while one exists ("No window to dispatch
	# pointer action").  On this slow single core the com.android.mms broadcast
	# receiver ANRs every boot, so the dialog was swallowing every touch.  A
	# secure build kills the background app instead of showing the dialog, which
	# keeps input alive.  "ro.*" can only be set once, so replace the line, do
	# not append a duplicate.
	sed -i 's/^ro\.secure=.*/ro.secure=1/' "$ROOT/default.prop"
	# Turn the SDK/emulator debug build flags off.  ro.debuggable=1 makes every
	# app process start a JDWP debugger thread and enables extra runtime
	# bookkeeping; on a single 804 MHz core that is pure overhead.  adb and the
	# mock-location provider are likewise useless here.  (ro.secure=1 above is
	# what keeps the background-ANR dialog away; do not touch it.)
	sed -i 's/^ro\.debuggable=.*/ro.debuggable=0/' "$ROOT/default.prop"
	sed -i 's/^ro\.allow\.mock\.location=.*/ro.allow.mock.location=0/' "$ROOT/default.prop"
	sed -i 's/^persist\.service\.adb\.enable=.*/persist.service.adb.enable=0/' "$ROOT/default.prop"
	# legacy/build-android.sh writes dalvik.vm.heapsize=16m into default.prop; a
	# later duplicate did NOT take effect, so replace the line outright.  A
	# large heap keeps zygote from GC-ing - and therefore allocating the
	# mark stack - during its framework bootstrap.
	sed -i 's/^dalvik\.vm\.heapsize=.*/dalvik.vm.heapsize=64m/' "$ROOT/default.prop"
	sed -i 's/^dalvik\.vm\.heapgrowthlimit=.*/dalvik.vm.heapgrowthlimit=64m/' "$ROOT/default.prop"
	grep -n 'heapsize\|heapgrowth\|heapstart' "$ROOT/default.prop" | sed 's/^/    default.prop: /'
	grep -n 'ro.secure\|ro.debuggable\|allow.mock\|adb.enable' "$ROOT/default.prop" | sed 's/^/    default.prop: /'
	: > "$ROOT/init.nintendo3ds.rc"
	echo "    Android boot stage: /init.rc, /default.prop, /bin/android-init"
elif [ "${ALLOW_NO_ANDROID:-0}" = 1 ]; then
	echo "    WARNING: no Android boot stage ($AD missing); ALLOW_NO_ANDROID=1 set, building a bring-up-only initramfs" >&2
else
	echo "mkinitramfs.sh: no Android boot stage at $AD." >&2
	echo "                Build CM7 first: bash port/scripts/cm7-rootless.sh package" >&2
	echo "                (or set ALLOW_NO_ANDROID=1 for a bring-up-only initramfs)." >&2
	exit 1
fi

# ---------------------------------------------------------------------------
say "/init"
# ---------------------------------------------------------------------------
cat > "$ROOT/init" <<'INIT'
#!/bin/sh
# @@ANDROID_NAME@@ on Nintendo 3DS - bring-up init (PID 1)
#
# The 3DS has no serial port, so this script has three jobs:
#   1. mount the SD card and log everything to it (when writable),
#   2. exercise the display/input paths and print a full inventory,
#   3. print the log on the TOP screen - PID 1's stdout is the fbcon console
#      there (diagnostic boots additionally page it onto the BOTTOM screen
#      with fbsay) - and then hand over an
#      interactive shell on the TOP screen.  The touchscreen acts as a
#      keyboard (the kernel's 3dstsc-touch driver has a virtual keyboard:
#      8 rows of keys, each row 36px tall, top-left of the bottom screen),
#      so commands *can* be typed - just blind, because the keyboard is drawn
#      by the old bottom-screen driver, not by the one this port uses.

PATH=/bin:/sbin
export PATH

# The initramfs root is unpacked with the build host's mktemp mode (0700).
# Non-root processes (zygote runs as AID_NOBODY) cannot search such a root and
# get EACCES on every absolute path, so make it searchable first thing.
chmod 0755 / 2>/dev/null

LOGDIR=/mnt/sd/CYANO3DS
LOGF=""
SDDEV=""
FBTXT=/tmp/boot.txt
: > "$FBTXT"

# MINDIAG=1: bring-up experiment.  Keep only the cheap always-on loggers
# (kmsgdump -> kmsg.log, logdump -> android-log.txt, heartbeat, hbfb) and the
# kernel-side ctr-diag sink.  The aggressive userspace probes (lateprobe loop
# + flusher, appwatch, and the /dev-mode loop) are heavy process forkers that
# add a burst of load exactly where the framework starts; the last-state log
# showed their little tools (awk/sed/cat/dmesg) running right up to the freeze.
# This boot tests whether they are part of what stops the machine.  Default to
# the full probes for diagnostics; set MINDIAG=1 in the environment to force
# the light set for a minimal-load boot.  (Note: it must be *empty* by default -
# the enable checks are [ -z "$MINDIAG" ], so a literal "0" would falsely
# select the light set.)
# The build host's environment is not inherited by the booted initramfs.  The
# build-time MINIMAL_DIAGNOSTICS=1 option leaves a marker for this runtime check.
MINDIAG="${MINDIAG:-}"
[ -f /etc/3ds-minimal-diagnostics ] && MINDIAG=1

# DIAG: the full bring-up diagnostics.  They are OFF by default now that the
# port is usable: tsdump flushes every touch event to the FAT card, tsmark
# repaints the top screen on every event, appwatch/lateprobe fork constantly and
# the firstframe probe SIGQUITs every app thread.  On one 804 MHz core that is
# exactly the "laggy" the user sees.  Enable per boot by dropping a file named
# "diag" into CYANO3DS/ on the SD card (or ship /etc/3ds-diag in a
# diagnostic initramfs build).  kmsgdump/logdump/the flusher stay on regardless.
DIAG=""
[ -f /etc/3ds-diag ] && DIAG=1

say() {
	echo "$*"
	[ -n "$LOGF" ] && echo "$*" >> "$LOGF"
	echo "$*" >> "$FBTXT"
	return 0
}

dump() {
	if [ -n "$LOGF" ]; then
		sh -c "$1" 2>&1 | tee -a "$LOGF" "$FBTXT"
	else
		sh -c "$1" 2>&1 | tee -a "$FBTXT"
	fi
	return 0
}

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null

# The top screen *is* the log for a normal boot: this script's whole stdout
# is the fbcon console there, and the default printk level lets warnings and
# errors through as well.  The console is silenced later, right before
# Android's init starts - see "console off" in the Android handover below.
# (The liveness square and the hbfb bar draw straight into the framebuffer
# and are unaffected in either case; KERN_EMERG, level 0, always prints, so a
# panic/oops remains visible on the top screen even after the mute.)
mount -t devtmpfs none /dev 2>/dev/null
# The kernel registers the Android misc devices on devtmpfs with mode 0600.
# Android later uses a private tmpfs /dev (below), but make the devtmpfs nodes
# world-accessible too: EACCES opening /dev/ashmem was killing zygote's GC, and
# root-only success would otherwise hide a node that non-root resolves.
chmod 0666 /dev/ashmem /dev/binder 2>/dev/null
chmod 0666 /dev/log_main /dev/log_events /dev/log_radio 2>/dev/null
chmod 0666 /dev/urandom /dev/random /dev/tty0 /dev/fb0 /dev/fb1 2>/dev/null
[ -e /dev/console ] || mknod /dev/console c 5 1
[ -e /dev/tty0 ]    || mknod /dev/tty0    c 4 0
[ -e /dev/fb0 ]     || mknod /dev/fb0     c 29 0
[ -e /dev/fb1 ]     || mknod /dev/fb1     c 29 1

# Prove on the TOP screen that the kernel is still scheduling.  The system
# freezes hard with no hung-task and no softlockup report (i.e. a spin with
# IRQs off, which no in-kernel detector can see), so /bin/heartbeat prints one
# short line to the console every few seconds.  It touches nothing on the SD.
# It used to run at SCHED_FIFO priority 99 via /bin/rt, but that made the
# kernel itself print "sched: RT throttling activated" as the very last log
# line before the freeze and may have perturbed the very thing we are
# measuring.  A normal-priority process with a -20 nice value is enough to
# survive a userspace busy-loop on the single CPU without ever throttling.
if [ -n "$DIAG" ] && [ -x /bin/heartbeat ]; then
	if [ -x /bin/nice ]; then
		nice -n -20 heartbeat &
	else
		heartbeat &
	fi
fi

# A liveness bar drawn straight into the top-screen framebuffer, as its OWN
# process.  It must not share a process with the heartbeat's printk: printk
# can block on console_lock (the top-screen fbcon is very slow), which would
# freeze the indicator exactly when the system is in trouble.  hbfb touches
# only the fb mapping, so if this bar keeps stepping after everything else
# stops, the kernel and scheduler are alive and the freeze is elsewhere
# (console_lock, the SD path, or a userspace deadlock).
if [ -n "$DIAG" ] && [ -x /bin/hbfb ]; then
	hbfb /dev/fb0 --loop 2000 >/dev/null 2>&1 &
fi

# --------------------------------------------------------------------------
# SD card (also how Linux will see /system and /data).  Device naming depends
# on how the PXI virtio bridge enumerates, so try everything: partitions
# before whole disks, vfat then auto.
# --------------------------------------------------------------------------
mkdir -p /mnt/sd
# Private tmpfs for the late diagnostics.  Android's init.rc does
# "mount rootfs rootfs / ro remount", so /tmp stops being writable the moment
# Android's init runs (that is why the previous build's probes failed with
# EROFS); a separate tmpfs mount is unaffected by that remount.
mkdir -p /mnt/probe
mount -t tmpfs none /mnt/probe 2>/dev/null
PARTDEVS=""
WHOLEDEVS=""
for d in /dev/vd[a-z]* /dev/mmcblk* /dev/sd[a-z]*; do
	[ -b "$d" ] || continue
	case "$d" in
		*p[0-9]|*[0-9]) PARTDEVS="$PARTDEVS $d" ;;
		*)             WHOLEDEVS="$WHOLEDEVS $d" ;;
	esac
done
for d in $PARTDEVS $WHOLEDEVS; do
	[ -b "$d" ] || continue
	for fs in vfat auto; do
		if mount -t "$fs" "$d" /mnt/sd 2>/dev/null; then
			SDDEV="$d"
			break 2
		fi
	done
done

# Android apps (emulators, file managers, ...) look for external storage at
# /sdcard; point it at the FAT card so their file browsers can see ROMs, BIOS
# files and the APKs that live in CYANO3DS/.
ln -sfn /mnt/sd /sdcard 2>/dev/null

say ""
say "========================================================="
say " @@ANDROID_NAME@@ on Nintendo 3DS - bring-up initramfs"
say "========================================================="
say "candidate block devices:$PARTDEVS$WHOLEDEVS"
say "SD device: ${SDDEV:-none}"

if [ -n "$SDDEV" ]; then
	mkdir -p "$LOGDIR" 2>/dev/null
	if ( : > "$LOGDIR/.write-test" ) 2>/dev/null; then
		rm -f "$LOGDIR/.write-test"
		# Each boot gets clean logs: move the previous boot's files into
		# lastboot/ so an analysis never mixes two boots (the timestamps of
		# the last two runs were impossible to line up when the files were
		# appended across boots).
		rm -rf "$LOGDIR/lastboot" 2>/dev/null
		mkdir -p "$LOGDIR/lastboot" 2>/dev/null
		for f in "$LOGDIR"/*.log "$LOGDIR"/*.txt "$LOGDIR"/*.bin; do
			[ -f "$f" ] && mv -f "$f" "$LOGDIR/lastboot/" 2>/dev/null
		done
		# The boot log is written to a tmpfs file and the separate flusher below
		# mirrors it to the card.  The ARM9->SD write path has been seen to
		# wedge under load and block *every* SD writer at once; if `say` wrote
		# straight to the card, that would also block this init just before it
		# starts Android.  With the log on tmpfs a wedged SD stalls only the
		# flusher (and the log on the card just stops early, which is fine).
		: > /mnt/probe/init.log
		LOGF=/mnt/probe/init.log
		# Full diagnostics are opt-in through a file on the card (see DIAG above).
		[ -e "$LOGDIR/diag" ] && DIAG=1
		say "SD card mounted from $SDDEV (writable), logging to $LOGDIR"
		say "diagnostics: $( [ -n "$DIAG" ] && echo 'ON (CYANO3DS/diag present)' || echo 'off (create CYANO3DS/diag to enable the bring-up probes)') "
		WT="android-3ds-write-test-$$"
		echo "$WT" > "$LOGDIR/.write-test"
		sync
		if [ "$(cat "$LOGDIR/.write-test" 2>/dev/null)" = "$WT" ]; then
			say "SD write test: OK (read back correctly)"
		else
			say "SD write test: FAILED - wrote data, read back something else"
		fi
		rm -f "$LOGDIR/.write-test"
		# Stream kernel messages as they arrive, with fsync after every drain:
		# an exception immediately before a hang must not stay in the page
		# cache (a plain `cat` lost exactly that on the previous boot).
		mkdir -p /mnt/probe 2>/dev/null
		# Loggers write to tmpfs, NOT the card: the ARM9->SD write path has been
		# seen to wedge under load and block *every* SD writer at once (the
		# machine stays alive - timer square flashing - but logs stop and the
		# probe cannot even save its samples).  A single slow flusher below
		# mirrors tmpfs -> SD, so a wedged write can only stall the flusher.
		if [ -x /bin/kmsgdump ]; then
			kmsgdump /mnt/probe/kmsg.log 2>/dev/null &
		else
			cat /dev/kmsg > /mnt/probe/kmsg.log 2>/dev/null &
		fi
		dmesg > "$LOGDIR/dmesg-at-init.log" 2>/dev/null
		# Minimal mode removes the recurring probes, but keep three isolated
		# Java thread dumps around the systemReady -> ActivityManager handoff.
		if [ -n "$DIAG" ] && [ -x /bin/firstframe-probe ]; then
			/bin/firstframe-probe /mnt/probe >/dev/null 2>&1 &
			# appwatch is cheap in steady state and is the only thing that
			# captures the home app's Dalvik stack the instant it forks -
			# exactly where the UI stops and SurfaceFlinger stops flipping.
			[ -x /bin/appwatch ] && /bin/appwatch >/dev/null 2>&1 &
			say "SystemServer snapshots + app-start probe scheduled"
		fi
	else
		say "SD card mounted from $SDDEV but it is READ-ONLY to Linux"
		say "(that means arm9linuxfw.bin is the upstream one; the port's"
		say "patched firmware adds write support)"
	fi
else
	say "no mountable SD card found - logging to the screens only"
fi

# Short power-button press => power off.  The kernel's mcu_buttons input device
# reports KEY_POWER; nothing else in this bring-up userspace listens for it, so
# without this the console only switches off after holding the button.
if [ -x /bin/powerkey ]; then
	if [ -n "$LOGF" ]; then
		powerkey > "$LOGDIR/powerkey.log" 2>&1 &
	else
		powerkey >/dev/null 2>&1 &
	fi
	say "powerkey running (short press = power off)"
fi

dump 'cat /proc/version'
say "--- kernel command line ---"
dump 'cat /proc/cmdline'
say "--- active consoles ---"
dump 'cat /proc/consoles'
say "--- block devices ---"
dump 'cat /proc/partitions'
say "--- framebuffers ---"
for f in /sys/class/graphics/fb*; do
	[ -e "$f" ] || continue
	say "$f: $(cat $f/name 2>/dev/null) $(cat $f/virtual_size 2>/dev/null) $(cat $f/bits_per_pixel 2>/dev/null)bpp stride=$(cat $f/stride 2>/dev/null 2>&1)"
done
say "--- display driver messages ---"
dump 'dmesg | grep -iE "ctr-lcd|bottom_lcd|framebuffer|fbcon|Console:" | tail -30'
say "--- input devices ---"
for f in /sys/class/input/event*; do
	[ -e "$f" ] || continue
	say "$f: $(cat $f/device/name 2>/dev/null)"
done
say "--- audio (ALSA / CSND) ---"
dump 'cat /proc/asound/cards 2>/dev/null'
dump 'cat /proc/asound/pcm 2>/dev/null'
dump 'dmesg | grep -iE "3ds-snd|snd-|ALSA|sound" | tail -20'
for f in /sys/class/sound/*; do
	[ -e "$f" ] && say "$f"
done
say "  /dev/snd (devtmpfs at this point): $(ls /dev/snd 2>/dev/null | tr '\n' ' ')"

say "--- wifi (SDIO Atheros AR6014G) ---"
for h in /sys/class/mmc_host/*; do
	[ -e "$h" ] || continue
	say "$h: $(cat $h/device/name 2>/dev/null)"
done
for d in /sys/bus/sdio/devices/*; do
	[ -e "$d" ] || continue
	say "$d: vendor=$(cat $d/vendor 2>/dev/null) device=$(cat $d/device 2>/dev/null) class=$(cat $d/class 2>/dev/null)"
done
say "--- network interfaces (wlan0 = Wi-Fi driver registered) ---"
for n in /sys/class/net/*; do
	[ -e "$n" ] || continue
	say "$n: type=$(cat $n/type 2>/dev/null) addr=$(cat $n/address 2>/dev/null) operstate=$(cat $n/operstate 2>/dev/null)"
done
say "fw ath6k/AR6002/nwm: $(ls /lib/firmware/ath6k/AR6002/nwm 2>/dev/null | tr '\n' ' ')"
dump 'dmesg | grep -iE "3ds-sdhc|mmc[0-9]:|sdio|ath6kl|ar6k|ar6000|AR6002|cfg80211|firmware|wlan0|wifi" | tail -50'

# --------------------------------------------------------------------------
# display tests (diagnostic boots only: drop a file named "diag" into
# CYANO3DS/ on the card)
# --------------------------------------------------------------------------
if [ -n "$DIAG" ]; then
	say ""
	say "--- fbtest on /dev/fb1 (bottom screen) ---"
	fbtest --once /dev/fb1 2>&1 | tee -a "$FBTXT"
	fbtest --flip   2>&1 | tee -a "$FBTXT"
	say "expect: 8 colour bars, white staircase top-left, blue arrow pointing"
	say "right, then a sweeping bar (double buffering)."
else
	say "(display self-test skipped; it runs on 'diag' boots)"
fi

say ""
say "--- anything complaining about tty/console ---"
dump 'dmesg | grep -iE "tty|console" | tail -20'

[ -n "$LOGF" ] && dmesg > "$LOGDIR/dmesg-at-end.log" 2>/dev/null
[ -n "$LOGF" ] && cp /proc/consoles "$LOGDIR/consoles.txt" 2>/dev/null
[ -n "$LOGF" ] && sync

# --------------------------------------------------------------------------
# On diagnostic boots, also page the log onto the bottom screen (it cycles by
# itself; nobody can press a key here).  On a normal boot the log stays where
# it already is: the top-screen console.
# --------------------------------------------------------------------------
if [ -n "$DIAG" ] && [ -x /bin/fbsay ]; then
	say ""
	say "--- rendering this log on the BOTTOM screen ---"
	fbsay -s 2 < "$FBTXT" >/dev/null 2>&1 &
	say "fbsay running: $(wc -l < "$FBTXT") lines, new page every 4s"
fi
if [ -n "$LOGF" ]; then
	say "log files: $LOGDIR/ (init.log, kmsg.log, dmesg-*.log)"
fi

# --------------------------------------------------------------------------
# Android handover.  If the userspace images are on the SD card and the boot
# stage is in this initramfs, mount them and give PID 1 to Android's init
# (which then reads /init.rc and starts servicemanager/zygote/surfaceflinger).
# Everything from here on is Android's business; the bring-up log stays on the
# SD card and on the top-screen console.
# --------------------------------------------------------------------------
if [ -x /bin/android-init ] && [ -f /init.rc ] && [ -n "$SDDEV" ]; then
	say ""
	say "--- @@ANDROID_NAME@@ handover ---"

	# One dalvik-cache, on /data, for every process this init spawns (the root
	# pre-dexopt, the svc/am helpers).  CM7's init.rc re-exports
	# ANDROID_CACHE=/cache for Android's own services, but its
	# dalvik.vm.dexopt-data-only=1 (set in build.prop and /default.prop) wins
	# in libdvm/installd.  Exporting it here also stops the early helper VMs
	# from logging "Can't open dex cache '/cache/dalvik-cache/...'".
	export ANDROID_CACHE=/data
	export ANDROID_DATA=/data

	# Power HAL shim.  Android's set_screen_state() (hardware/libhardware_legacy/
	# power/power.c) open()s /sys/power/wake_lock, /sys/power/wake_unlock and
	# /sys/power/state and returns errno when any of them is missing.  This
	# kernel has no Android wakelocks ("/sys/power/wake_lock missing"), so the
	# opens failed and PowerManagerService never set
	# SCREEN_ON_BIT in mPowerState.  screenIsOn() then stayed false, and
	# WindowManagerService.preprocessEvent() drops every EV_ABS sample when the
	# screen is off -- touch reached apps as x=0 y=0, with only BTN_TOUCH
	# (EV_KEY) surviving, which is exactly the pre-fix touch log.  Overlay
	# /sys/power with plain writable files so the opens succeed and
	# set_screen_state() returns 0.  The writes are no-ops (our panel is always
	# on) and the userActivity helper keeps the timeout from clearing the bit.
	if mount -t tmpfs none /sys/power 2>/dev/null; then
		for f in wake_lock wake_unlock state; do
			: > "/sys/power/$f"
			chmod 0666 "/sys/power/$f"
		done
		say "power HAL shim: /sys/power/{wake_lock,wake_unlock,state} present"
	else
		say "WARNING: /sys/power shim failed (touch would read x=0 y=0)"
	fi

	# Start capturing Android's own logs (/dev/log/main) and make the SD card
	# logs durable *before* handing over: after exec, this init is gone, and a
	# hang must not cost us the evidence.
	if [ -n "$LOGF" ]; then
		if [ -x /bin/logdump ]; then
			# /dev/log/main is created by the /dev prep further down, so
			# wait for it: starting logdump on the devtmpfs /dev (which
			# only has /dev/log_main) makes it exit immediately.
			(
				while [ ! -e /dev/log/main ]; do sleep 1; done
				exec logdump
			) > /mnt/probe/android-log.txt 2>&1 &
			say "logdump -> tmpfs (flushed to SD lazily)"
		fi
		# Flusher: a boot that dies (or is powered off) in the middle of the
		# PackageManager scan must not lose the last stretch of the story.  10 s
		# keeps the tmpfs->SD copy cheap (the log is a few hundred KB) without
		# syncing the FAT card every 3 s during the pre-dexopt/scan, which is
		# the same SD the read-only /system loop image lives on.
		(
			while true; do
				for f in /mnt/probe/*; do
					[ -f "$f" ] || continue
					cp "$f" "$LOGDIR/" 2>/dev/null
				done
				if [ -d /data/tombstones ]; then
					mkdir -p "$LOGDIR/tombstones" 2>/dev/null
					for t in /data/tombstones/*; do
						[ -f "$t" ] || continue
						cp "$t" "$LOGDIR/tombstones/" 2>/dev/null
					done
				fi
				[ -s /data/anr/traces.txt ] && \
					cp /data/anr/traces.txt "$LOGDIR/traces.txt" 2>/dev/null
				sync
				sleep 10
			done
		) &
		say "log flusher started (tmpfs -> SD every 10s, +tombstones/anr)"
	fi

	mkdir -p /system /data /cache /mnt/system-img 2>/dev/null
	ANDROID=0
	if [ -f /mnt/sd/android/system.img ]; then
		# @@ANDROID_NAME@@: loop-mount /system read-only from the SD card.
		#
		# Unlike the archived prebuilt flavors, /system is NOT copied into a tmpfs.
		# The system tree is tens of MiB, and a tmpfs /system plus a tmpfs /data
		# would fight for the same RAM; the stock image already carries the
		# graphics HAL (lib/hw/gralloc.default.so + lib/egl/libGLES_android.so),
		# so nothing here needs to rewrite /system at runtime.  Reading the ext4
		# image through the loop device also avoids the loop-on-FAT write
		# corruption seen during bring-up.
		if mount -o loop,ro -t ext4 /mnt/sd/android/system.img /system 2>/dev/null; then
			say "android/system.img -> /system (loop,ro)  ok"
			[ -x /system/bin/app_process ] && ANDROID=1
			[ "$ANDROID" = 1 ] || say "  but /system/bin/app_process is missing"
		elif false; then
			if mount -t tmpfs none /system 2>/dev/null; then
				say "copying android/system.img -> tmpfs /system (RAM) ..."
				if cp -a /mnt/system-img/. /system/ 2>/dev/null; then
					say "android/system.img -> tmpfs /system  ok"
				else
					say "  cp into tmpfs /system FAILED (system may be incomplete)"
				fi
				umount /mnt/system-img 2>/dev/null
				# tmpfs roots default to 1777; Android only needs to traverse it.
				chmod 0755 /system 2>/dev/null
				# The 3DS Y button is mapped to Linux scancode 15 (KEY_TAB), but
				# the SDK's qwerty.kl has no `key 15 TAB` line, so EventHub maps
				# the scancode to keycode 0 and Android drops the key.  Add it
				# to the tmpfs /system copy.
				if [ -f /system/usr/keylayout/qwerty.kl ]; then
					grep -q '^key 15 ' /system/usr/keylayout/qwerty.kl 2>/dev/null || \
						echo 'key 15 TAB' >> /system/usr/keylayout/qwerty.kl
					say "  qwerty.kl: 'key 15 TAB' present (Y button)"
				fi
				# NOTE: do NOT try to "un-prelink" SDK libraries by stripping the
				# trailing "PRE " tag.  The prelink tool pre-applies the relocations
				# and removes the relocation tables, so a prelinked library loaded
				# at a different address has no relocations and crashes the linker:
				# mediaserver SIGSEGV'd at 0xacf0b050 while the linker loaded
				# libpixelflinger.so at 0x80000000.  (The games' EGL failure needs
				# a different fix: load the prelinked libagl.so before the game's
				# own mappings take 0xacc00000.)
				# Optional APKs (Angry Birds, ...): copy every
				# CYANO3DS/*.apk into /system/app so PackageManager installs
				# it and the root pre-dexopt below optimizes it.  (Boot keeps the
				# card's .apk files; only *.log/*.txt/*.bin are rotated away.)
				for apk in /mnt/sd/CYANO3DS/*.apk; do
					[ -f "$apk" ] || continue
					bn=$(basename "$apk")
					sz=$(stat -c %s "$apk" 2>/dev/null || echo 0)
					# /system is a tmpfs whose default limit is half of RAM
					# (~120 MiB) and the base image already uses ~60 MiB, so
					# a 45 MiB game cannot be copied in.  Bind-mount the
					# card's file at /system/app/NAME.apk instead: the path
					# PackageManager and dexopt see is unchanged (so the
					# system@app@NAME.apk@classes.dex cache name matches),
					# but the APK data stays on the card and costs no RAM.
					if [ "$sz" -gt 4000000 ] 2>/dev/null; then
						: > "/system/app/$bn" 2>/dev/null
						if mount --bind "$apk" "/system/app/$bn" 2>/dev/null; then
							say "  $bn -> /system/app (bind mount, $((sz / 1048576)) MiB)"
						else
							say "  $bn bind mount FAILED; copying"
							cp -f "$apk" /system/app/ \
								&& say "  $bn -> /system/app"
						fi
					else
						cp -f "$apk" /system/app/ \
							&& say "  $bn -> /system/app"
					fi
				done
				# Native libs for those apps: Android does not extract
				# lib/<abi>/*.so from a system APK, so a game's .so has to live in
				# /system/lib or it dies with UnsatisfiedLinkError.
				#
				# This also carries an optional patched libEGL.so (see
				# port/scripts/make-egl-preload.sh) whose only change is a DT_NEEDED
				# on libagl.so.  Copying it here makes zygote map libagl (and
				# libpixelflinger) at their prelinked addresses before an app's own
				# library can take them - which is what lets the software-GLES games
				# (Bejeweled 2, Chuzzle, ...) initialise EGL.  Delete the card file to
				# go back to the stock library with no rebuild.
				for so in /mnt/sd/CYANO3DS/*.so; do
					[ -f "$so" ] || continue
					cp -f "$so" /system/lib/ \
						&& chmod 0644 "/system/lib/$(basename "$so")" \
						&& say "  $(basename "$so") -> /system/lib"
				done
				# Optional `services.jar` overlay (card opt-in).
				#
				# The Android notification shade is a touch-modal
				# TYPE_STATUS_BAR_PANEL window that swallows every pointer event
				# while it is visible, and it keeps key focus, so dialogs cannot
				# be tapped and HOME goes nowhere.  The card may carry a
				# smali-rebuilt services.jar whose StatusBarService
				# .makeExpandedVisible() is a no-op; copy it in when present.
				#
				# services.jar is on the Dalvik boot classpath, so replacing it
				# invalidates the dependency signature recorded in every
				# already-optimized cache.  The previous session's overlay
				# boot-looped system_server because installd's `dexopt` SIGSEGVs
				# (status=0x000b) on the stale app caches.  The root pre-dexopt
				# below does not SIGSEGV, so delete the app caches here and let
				# it rebuild them against the new signature.
				if [ -f /mnt/sd/CYANO3DS/services.jar ]; then
					if cp -f /mnt/sd/CYANO3DS/services.jar \
							/system/framework/services.jar; then
						say "services.jar overlay applied (shade disabled)"
						OVERLAY_SERVICES=1
					else
						say "  services.jar overlay copy FAILED"
					fi
				fi
			else
				say "tmpfs /system mount FAILED; falling back to loop image"
				umount /mnt/system-img 2>/dev/null
				mount -o loop,ro -t ext4 /mnt/sd/android/system.img /system 2>/dev/null \
					&& say "android/system.img -> /system (loop,ro)  ok" \
					|| say "  loop mount also FAILED"
			fi
			[ -x /system/bin/app_process ] && ANDROID=1
			[ "$ANDROID" = 1 ] || say "  but /system/bin/app_process is missing"
		else
			say "android/system.img mount FAILED"
		fi
	else
		say "no /android/system.img on the SD card"
	fi

	# ------------------------------------------------------------------
	# Runtime apps and native libraries from the card - no system.img rebuild.
	#
	# /system is loop-mounted read-only, so the baked-in tree cannot be
	# extended.  When the card carries apps or libs, put a tmpfs over
	# /system/app and /system/lib and re-expose the baked-in files with bind
	# mounts.  A bind mount keeps the /system/... path unchanged, so
	# PackageManager, dexopt and the odex cache names are unaffected (unlike
	# an overlayfs/symlink trick).  Then:
	#   * every CYANO3DS/*.apk appears as /system/app/NAME.apk and
	#     overrides a baked-in app of the same name,
	#   * the native libraries bundled in those APKs (lib/armeabi/*.so) are
	#     extracted into /system/lib, so a matching *.so on the card is no
	#     longer required,
	#   * a CYANO3DS/*.so still overrides everything (patched libs).
	# Only the extracted .so live in RAM; the APKs stay on the card.
	# ------------------------------------------------------------------
	install_card_apps() {
		mkdir -p /mnt/orig-app /mnt/orig-lib 2>/dev/null
		# ---- /system/app -------------------------------------------------
		if mount --bind /system/app /mnt/orig-app 2>/dev/null; then
			if mount -t tmpfs none /system/app 2>/dev/null; then
				for f in /mnt/orig-app/*; do
					[ -e "$f" ] || continue
					bn=$(basename "$f")
					if [ -d "$f" ]; then
						mkdir -p "/system/app/$bn" 2>/dev/null
						mount --bind "$f" "/system/app/$bn" 2>/dev/null || \
							cp -a "$f" "/system/app/$bn" 2>/dev/null
					else
						: > "/system/app/$bn" 2>/dev/null
						mount --bind "$f" "/system/app/$bn" 2>/dev/null || \
							cp -f "$f" "/system/app/$bn" 2>/dev/null
					fi
				done
				for apk in /mnt/sd/CYANO3DS/*.apk; do
					[ -f "$apk" ] || continue
					bn=$(basename "$apk")
					umount "/system/app/$bn" 2>/dev/null
					: > "/system/app/$bn" 2>/dev/null
					if mount --bind "$apk" "/system/app/$bn" 2>/dev/null; then
						say "  app  $bn -> /system/app (card)"
					elif cp -f "$apk" "/system/app/$bn" 2>/dev/null; then
						say "  app  $bn -> /system/app (copied)"
					else
						say "  app  $bn: INSTALL FAILED"
					fi
				done
			else
				say "  card apps: tmpfs on /system/app FAILED"
			fi
		else
			say "  card apps: bind /system/app FAILED"
		fi
		# ---- /system/lib -------------------------------------------------
		if mount --bind /system/lib /mnt/orig-lib 2>/dev/null; then
			if mount -t tmpfs none /system/lib 2>/dev/null; then
				for f in /mnt/orig-lib/*; do
					[ -e "$f" ] || continue
					bn=$(basename "$f")
					if [ -d "$f" ]; then
						mkdir -p "/system/lib/$bn" 2>/dev/null
						mount --bind "$f" "/system/lib/$bn" 2>/dev/null || \
							cp -a "$f" "/system/lib/$bn" 2>/dev/null
					else
						: > "/system/lib/$bn" 2>/dev/null
						mount --bind "$f" "/system/lib/$bn" 2>/dev/null || \
							cp -f "$f" "/system/lib/$bn" 2>/dev/null
					fi
				done
				# 1. native libs bundled in the card APKs (armeabi)
				for apk in /mnt/sd/CYANO3DS/*.apk; do
					[ -f "$apk" ] || continue
					rm -rf /tmp/apk-lib 2>/dev/null
					mkdir -p /tmp/apk-lib 2>/dev/null
					if unzip -o -j "$apk" 'lib/armeabi/*.so' \
							-d /tmp/apk-lib >/dev/null 2>&1; then
						for so in /tmp/apk-lib/*.so; do
							[ -f "$so" ] || continue
							son=$(basename "$so")
							case "$son" in
								libEGL.so|libGLESv1_CM.so|libGLESv2.so) continue ;;
							esac
							umount "/system/lib/$son" 2>/dev/null
							cp -f "$so" "/system/lib/$son" 2>/dev/null && \
								chmod 0644 "/system/lib/$son" 2>/dev/null
						done
						say "  libs from $(basename "$apk"): $(ls /tmp/apk-lib 2>/dev/null | tr '\n' ' ')"
					fi
				done
				rm -rf /tmp/apk-lib 2>/dev/null
				# 2. explicit card .so overrides (patched libraries win).
				#    The graphics/EGL libraries are deliberately NOT overridden:
				#    the libEGL preload shim is built from the archived prebuilt
				#    SDK and is incompatible with CM7's EGL, so the
				#    system libEGL.so/libGLESv1_CM.so/libGLESv2.so always win.
				for so in /mnt/sd/CYANO3DS/*.so; do
					[ -f "$so" ] || continue
					son=$(basename "$so")
					case "$son" in
						libEGL.so|libGLESv1_CM.so|libGLESv2.so)
							say "  lib  $son: skipped (system EGL library)"
							continue ;;
					esac
					umount "/system/lib/$son" 2>/dev/null
					cp -f "$so" "/system/lib/$son" 2>/dev/null && \
						chmod 0644 "/system/lib/$son" 2>/dev/null && \
						say "  lib  $son -> /system/lib (card override)"
				done
			else
				say "  card libs: tmpfs on /system/lib FAILED"
			fi
		else
			say "  card libs: bind /system/lib FAILED"
		fi
		return 0
	}
	if [ "$ANDROID" = 1 ] && \
	   { ls /mnt/sd/CYANO3DS/*.apk >/dev/null 2>&1 || \
	     ls /mnt/sd/CYANO3DS/*.so  >/dev/null 2>&1; }; then
		say "--- installing card apps/libs into /system (no image rebuild) ---"
		install_card_apps
	fi

	if [ "$ANDROID" = 1 ]; then
		# /data is RAM (tmpfs).  Writing the ext4 image through the loop
		# device kept corrupting it ("freeing already freed block") and the
		# system hard-hung right afterwards.  So /data is a tmpfs, but the
		# expensive-to-rebuild state (packages.xml, the media database, app
		# databases/prefs) is saved as a *plain tar* on the FAT card: FAT
		# file writes are reliable, only the loop-on-FAT ext4 image was not.
		# This turns the per-boot PackageManager scan / media-database upgrade
		# into a one-time cost.  The dalvik-cache is deliberately NOT in the
		# tar (it is ~16 MiB and is seeded read-only from data.img).
		mount -t tmpfs none /data 2>/dev/null \
			&& say "/data on tmpfs (RAM)" \
			|| say "tmpfs /data mount FAILED"
		DATA_TAR=/mnt/sd/android/@@DATATAR@@
		if [ -f "$DATA_TAR" ]; then
			say "restoring /data state from android/@@DATATAR@@ ..."
			if tar xf "$DATA_TAR" -C /data 2>/dev/null; then
				say "  /data state restored (persistent)"
			else
				say "  @@DATATAR@@ restore FAILED; falling back to the data.img seed"
			fi
		fi
		# N3DS_CM7_DHCP_DIR: dhcpcd writes its pid file to
		# /data/misc/dhcp/dhcpcd-wlan0.pid and its lease next to it, and it
		# *exits* when that directory is missing:
		#   dhcpcd: open `/data/misc/dhcp/dhcpcd-wlan0.pid': No such file or directory
		#   WifiStateTracker: DHCP request failed: Timed out waiting for dhcpcd to start
		#   WifiStateTracker: EVENT_INTERFACE_CONFIGURATION_FAILED -> disconnect()
		# so a Wi-Fi link that had associated, completed the 4-way handshake and
		# installed its keys was torn down eleven seconds later, every time.  The
		# saved data tar carries misc/{wifi,keystore,...} but no misc/dhcp, and
		# /data is a fresh tmpfs each boot, so create it here.
		mkdir -p /data/misc/dhcp 2>/dev/null \
			&& say "/data/misc/dhcp created for dhcpcd" \
			|| say "/data/misc/dhcp mkdir FAILED"
		# ...and owned by **dhcp**, not by system/1000: /system/bin/dhcpcd
		# calls switchUser() (external/dhcpcd/dhcpcd.c:609) which does
		#   setgroups({AID_INET=3003, AID_SHELL=2000}); setgid(AID_DHCP);
		#   setuid(AID_DHCP);
		# so it writes its pid/lease files as uid/gid **1014** and has no group
		# in common with 1000.  The framework launches it, but never as 1000:
		#   dhcpcd: open `/data/misc/dhcp/dhcpcd-wlan0.pid': Permission denied
		# AOSP's own mode here is 0770 dhcp:dhcp
		# (private/android_filesystem_config.h); this device's init.rc never
		# creates the directory (only /data/misc/{bluetooth,keystore,vpn,
		# systemkeys,wifi}), which is why it was missing at all.  0777 rather than
		# 0770: the supplementary groups are 3003/2000, not 1014, so a 0770
		# dhcp:dhcp directory is only usable if the gid happens to match.
		chown 1014:1014 /data/misc/dhcp 2>/dev/null
		chmod 0777 /data/misc/dhcp 2>/dev/null
		ls -ld /data/misc/dhcp 2>/dev/null | sed "s/^/  dhcp dir: /"
		# dalvik-cache is always seeded from the read-only data.img.  Do NOT
		# restore a saved dalvik.tar: if a broken boot ever saves its caches the
		# next boot inherits them and the framework fails to register its
		# natives (JNITest boot loop).  The root pre-dexopt below rebuilds the
		# app caches from data.img, so this is safe.
		if [ ! -d /data/dalvik-cache ] || [ -z "$(ls -A /data/dalvik-cache 2>/dev/null)" ]; then
			if [ -f /mnt/sd/android/data.img ]; then
				mkdir -p /mnt/data-img
				if mount -o loop,ro -t ext4 /mnt/sd/android/data.img /mnt/data-img 2>/dev/null; then
					cp -a /mnt/data-img/dalvik-cache /data/ 2>/dev/null \
						&& say "seeded /data/dalvik-cache from data.img (ro)"
					umount /mnt/data-img 2>/dev/null
				else
					say "data.img ro mount failed; dexopt will have to run"
				fi
			fi
		fi
		# The seed above restored the app caches that were built against the
		# *original* services.jar.  When the overlay is active they are stale;
		# remove them so the root pre-dexopt rebuilds them (installd's dexopt
		# SIGSEGVs on them and boot-loops system_server).
		if [ "${OVERLAY_SERVICES:-0}" = 1 ]; then
			rm -f /data/dalvik-cache/system@app@*.apk@classes.dex
			say "  stale app dexopt caches removed (services.jar changed)"
		fi
		# /bin/dataflush: save the persistent subset.  tmp+rename so a power
		# loss or a yanked card cannot leave a half-written archive.
		cat > /bin/dataflush <<'DF'
#!/bin/sh
cd /data 2>/dev/null || exit 0
set --
for d in system data misc property local tombstones anr; do
	[ -e "$d" ] && set -- "$@" "$d"
done
[ $# -gt 0 ] || exit 0
sync
tar cf /mnt/sd/android/@@DATATAR@@.tmp "$@" \
	&& mv -f /mnt/sd/android/@@DATATAR@@.tmp /mnt/sd/android/@@DATATAR@@
DF
		chmod 755 /bin/dataflush
		# /bin/shutdown: clean power-off for a short power-button press.
		#
		# The native `powerkey` used to call reboot(RB_POWER_OFF) directly, which
		# is a hard cut: apps were never told to save and no process was stopped.
		# This does the orderly version: broadcast ACTION_SHUTDOWN, stop the
		# Android processes, flush the persistent /data subset, sync, then power
		# off.  The 3DS still force-powers-off on a long hold, so a wedged
		# shutdown can never brick the console.
		cat > /bin/shutdown <<'SD'
#!/bin/sh
# clean shutdown triggered by the power button
echo "shutdown: requested" > /dev/kmsg 2>/dev/null
# 1. let Android save state (best effort; the system may already be down)
if [ -x /system/bin/am ]; then
	/bin/timeout 30 /system/bin/am broadcast \
		-a android.intent.action.ACTION_SHUTDOWN >/dev/null 2>&1
fi
sleep 2
# 2. stop Android: SIGTERM, then SIGKILL.  Never PID 1 (the bring-up init),
#    this script, or its parent (powerkey).
for sig in TERM KILL; do
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "1" ] && continue
		[ "$pid" = "$$" ] && continue
		[ "$pid" = "$PPID" ] && continue
		kill -$sig "$pid" 2>/dev/null
	done
	[ "$sig" = TERM ] && sleep 2
done
# 3. persist /data and flush the card
[ -x /bin/dataflush ] && /bin/dataflush
sync
sleep 1
# 4. power off (kernel pm_power_off -> MCU)
echo "shutdown: power off" > /dev/kmsg 2>/dev/null
poweroff -f
SD
		chmod 755 /bin/shutdown
		say "clean shutdown helper installed (/bin/shutdown)"
		# periodic flush: a hard power-off can still lose up to 5 min of state
		(
			while true; do
				sleep 300
				/bin/dataflush
			done
		) >> "$LOGDIR/dataflush.log" 2>&1 &
		say "data flusher armed (tar /data/{system,data,misc,...} -> android/@@DATATAR@@ every 5 min)"
		mount -t tmpfs none /cache 2>/dev/null
		# Keep /data/dalvik-cache on /data (persistent): dexopt then only runs
		# once and survives reboots.  Also lift the system limits dalvik can
		# hit (dsms of file-max, nr_open, max_map_count, overcommit).
		mkdir -p /data/dalvik-cache 2>/dev/null
		# CM7.2 routes /system dex through /cache/dalvik-cache (installd's
		# DALVIK_SYSTEM_CACHE_PREFIX) and its early helper VMs look in
		# $ANDROID_CACHE == /cache.  The port keeps one cache on /data, so make
		# /cache/dalvik-cache an alias of it.  That way every path - the VM's
		# libdex lookup, installd, and the port's own pre-dexopt - is the same
		# directory, with or without dalvik.vm.dexopt-data-only.  Android's init
		# later mkdir()s /cache/dalvik-cache; that EEXISTs on the symlink and
		# leaves it in place (its chown/chmod follow the link to /data).
		rm -rf /cache/dalvik-cache 2>/dev/null
		ln -s /data/dalvik-cache /cache/dalvik-cache 2>/dev/null
		# Persist the root pre-dexopt result across boots.  /data is a tmpfs, so
		# without this every boot re-runs the ~100 s framework/app pre-dexopt.
		# The tar is written only right after a *successful* pre-dexopt, before
		# Android's own installd can leave a 0xff DexOptHeader in the cache, so
		# it is exactly the cache the framework expects.  The pre-dexopt loop
		# re-checks every entry's magic anyway, so a stale tar self-heals.
		DALVIK_TAR=/mnt/sd/android/dalvik.tar
		DALVIK_RESTORED=0
		DALVIK_DIRTY=0
		if [ ! -s /data/dalvik-cache/system@framework@core.jar@classes.dex ] && \
		   [ -f "$DALVIK_TAR" ]; then
			if tar xf "$DALVIK_TAR" -C /data 2>/dev/null; then
				DALVIK_RESTORED=1
				say "restored /data/dalvik-cache from android/dalvik.tar (skip pre-dexopt)"
			else
				say "dalvik.tar restore FAILED; pre-dexopt will rebuild"
			fi
		fi
		# The card APKs override the baked-in ones, so a restored odex for such
		# an app can belong to a *different* APK version.  The pre-dexopt only
		# checks the cache's magic byte, so a stale-but-valid odex is skipped and
		# the framework then asks installd to re-dexopt - which SIGSEGVs on this
		# port (status=0x000b) and the app cannot load (Angry Birds: 1.5.3 card
		# APK vs 2.1.1 baked, ClassNotFoundException).  Keep a signature of the
		# card APK set in the cache dir; when it changes drop those odex files so
		# the root pre-dexopt rebuilds them (and the tar is re-saved).
		CARD_SIG_FILE=/data/dalvik-cache/.card-apps.sig
		CARD_SIG_NEW=/tmp/card-apps.sig
		: > "$CARD_SIG_NEW" 2>/dev/null
		for apk in /mnt/sd/CYANO3DS/*.apk; do
			[ -f "$apk" ] || continue
			echo "$(basename "$apk") $(stat -c %s "$apk" 2>/dev/null) $(stat -c %Y "$apk" 2>/dev/null)" >> "$CARD_SIG_NEW"
		done
		if [ -s "$CARD_SIG_NEW" ] && \
		   [ "$(cat "$CARD_SIG_NEW")" != "$(cat "$CARD_SIG_FILE" 2>/dev/null)" ]; then
			n=0
			while read -r bn _rest; do
				rm -f "/data/dalvik-cache/system@app@$bn@classes.dex" 2>/dev/null
				n=$((n+1))
			done < "$CARD_SIG_NEW"
			cp -f "$CARD_SIG_NEW" "$CARD_SIG_FILE" 2>/dev/null
			DALVIK_DIRTY=1
			say "  card APK set changed -> dropped $n card odex cache(s) for re-dexopt"
		fi
		# AMS's prepareTraceFile(false) does not create this directory, and
		# Android's init.rc has no `mkdir /data/anr` either - so create it here,
		# owned by system and writable by it.  Dalvik (uid 1000) opens
		# /data/anr/traces.txt itself on SIGQUIT; a root-owned 0644 file would
		# make that open() fail with EACCES and lateprobe's kill -3 would
		# produce nothing.
		mkdir -p /data/anr 2>/dev/null
		chown 1000:1000 /data/anr 2>/dev/null
		# world-writable: Android's Signal Catcher writes the SIGQUIT thread dump
		# *in the receiving process*, and app processes (uid 100NN) are not in
		# the system group - 0770 made every lateprobe java dump fail with
		# EACCES ("Unable to open stack trace file '/data/anr/traces.txt'").
		chmod 0777 /data/anr 2>/dev/null
		rm -f /data/anr/traces.txt 2>/dev/null
		# GBCoid's Game Boy gamepad bindings.  Its built-in defaults map Game Boy
		# A/B to Android SEARCH/BACK (or, when it thinks a QWERTY keyboard is
		# attached, to P/O), which do not match this port's codes; and BACK is
		# also intercepted by EmulatorActivity.onKeyDown() for the quit dialog.
		# So on an unmodified GBCoid only the D-pad works.  Seed the app's
		# SharedPreferences with the bindings that match port/scripts/fix-buttons.py;
		# an explicit pref overrides the built-in default regardless of the
		# qwerty/non-qwerty branch the app takes.
		GBC_DIR=/data/data/com.androidemu.gbc
		GBC_PREFS="$GBC_DIR/shared_prefs/com.androidemu.gbc_preferences.xml"
		seed_gbc_keys() {
			mkdir -p "$GBC_DIR/shared_prefs" 2>/dev/null
			cat > "$GBC_PREFS" <<'GBCXML'
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <int name="gamepad_A" value="23" />
    <int name="gamepad_B" value="62" />
    <int name="gamepad_select" value="61" />
    <int name="gamepad_start" value="66" />
    <int name="gamepad_A_turbo" value="71" />
    <int name="gamepad_B_turbo" value="72" />
</map>
GBCXML
			# keep the app's ownership so it can rewrite the file itself later
			ref=$(stat -c '%u:%g' "$GBC_DIR" 2>/dev/null)
			[ -n "$ref" ] && chown "$ref" "$GBC_PREFS" 2>/dev/null
			chmod 0666 "$GBC_PREFS" 2>/dev/null
		}
		if [ -d "$GBC_DIR" ]; then
			seed_gbc_keys
			say "GBCoid: seeded Game Boy gamepad key bindings"
		else
			say "GBCoid: no data dir yet; will seed keys when it appears"
		fi
		# GBCoid's Emulator.java hardcodes its engine path as
		# /data/data/<pkg>/lib/lib<engine>.so.  It is a *system* app here, so its
		# native libraries live in /system/lib and the private lib dir is empty ->
		# "Cannot load .../lib/libgbc.so" and the emulator dies with SIGSEGV when
		# it then dereferences the NULL engine.  Wait for the framework to create
		# the app's data dir (so its owner stays correct) and link the engine
		# libraries into it.
		if [ -f /system/lib/libgbc.so ]; then
			(
				i=0
				while [ $i -lt 180 ]; do
					d=/data/data/com.androidemu.gbc
					if [ -d "$d" ]; then
						mkdir -p "$d/lib" 2>/dev/null
						for l in libgbc.so libemumedia.so libemu.so; do
							[ -f "/system/lib/$l" ] && ln -sfn "/system/lib/$l" "$d/lib/$l"
						done
						chmod 0755 "$d/lib" 2>/dev/null
						ls -l "$d/lib" > /mnt/probe/gbc-lib.log 2>&1
						cp /mnt/probe/gbc-lib.log "$LOGDIR/gbc-lib.log" 2>/dev/null
						seed_gbc_keys
						say "GBCoid: engine libs linked and gamepad keys seeded"
						break
					fi
					i=$((i+1))
					sleep 5
				done
			) >/dev/null 2>&1 &
			say "GBCoid: engine libs will be linked into /data/data/com.androidemu.gbc/lib"
		fi
		say "data/dalvik-cache seeded into /data (tmpfs)"
		for kv in "fs/file-max 200000" "fs/nr_open 200000" \
			  "vm/max_map_count 1048576" "vm/overcommit_memory 1"; do
			f=/proc/sys/${kv% *}
			echo "${kv#* }" > "$f" 2>/dev/null && \
				say "  $f = $(cat $f)" || say "  $f: cannot set"
		done
		say "free space:"
		df -h /system /data /data/dalvik-cache /cache 2>/dev/null | tee -a "$LOGF"
		mkdir -p /dev/graphics /dev/input 2>/dev/null
		[ -e /dev/fb1 ] && ln -sf /dev/fb1 /dev/graphics/fb0
		sync
		say ""
		say "=== starting @@ANDROID_NAME@@ init (as a child, not PID 1) ==="
		# Console off from here on: the top-screen fbcon is a slow,
		# software, rotated 24bpp framebuffer and every printk that reaches
		# it holds console_lock for a long time; SurfaceFlinger's fb ioctls
		# (FBIOPUT_VSCREENINFO / FBIOPAN_DISPLAY) take the *same* lock, so
		# console output and the compositor would serialise against each
		# other on the single CPU.  Messages are still stored in the kernel
		# ring buffer, so kmsgdump keeps writing them all to kmsg.log on the
		# SD; only the slow console rendering is muted.  KERN_EMERG (level 0)
		# still prints, so a panic/oops remains visible on the top screen.
		echo 1 > /proc/sys/kernel/printk 2>/dev/null
		say "(console output muted now; the full log continues in CYANO3DS/)"
		# dalvik's first GC allocates the mark stack with ashmem; if the
		# process is short of file descriptors that fails.  Show the limit
		# and raise it for everything we spawn.
		say "fd limit before: $(ulimit -n 2>/dev/null)"
		ulimit -n 65535 2>/dev/null && say "fd limit now: $(ulimit -n 2>/dev/null)"
		cat /proc/sys/fs/file-nr 2>/dev/null | sed 's/^/  file-nr: /'
		# ------------------------------------------------------------------
		# Battery for BatteryService.
		#
		# Gingerbread/CM7's BatteryService walks /sys/class/power_supply/*,
		# reads each subdirectory's `type` file and picks the Battery/Mains/
		# USB entries.  The kernel's MCU charger driver already registers
		# BAT0 (type=Battery) and ADP0 (type=Mains) from the real MCU
		# registers, so the status-bar indicator shows the actual charge.
		#
		# The legacy BatteryService from older releases instead reads fixed legacy names
		# (/sys/class/power_supply/battery/...) and finds nothing when only
		# BAT0/ADP0 exist.  Only if no real battery device is present, fall
		# back to a fake tmpfs that carries *both* the legacy names and
		# their `type` files, so every BatteryService variant still gets a
		# (healthy, charged) battery instead of the "connect charger"
		# warning.  With the real MCU battery there is no shim.
		# ------------------------------------------------------------------
		if [ -e /sys/class/power_supply/BAT0 ] || [ -e /sys/class/power_supply/battery ]; then
			say "power_supply: real MCU battery in use: $(ls /sys/class/power_supply 2>/dev/null | tr '\n' ' ')"
			for d in /sys/class/power_supply/*; do
				[ -d "$d" ] || continue
				say "  $(basename "$d"): type=$(cat "$d/type" 2>/dev/null) capacity=$(cat "$d/capacity" 2>/dev/null) status=$(cat "$d/status" 2>/dev/null) online=$(cat "$d/online" 2>/dev/null)"
			done
		elif ! grep -q " /sys/class/power_supply " /proc/mounts 2>/dev/null; then
			if mount -t tmpfs none /sys/class/power_supply 2>/dev/null; then
				mkdir -p /sys/class/power_supply/battery \
					 /sys/class/power_supply/ac \
					 /sys/class/power_supply/usb
				echo "Battery" > /sys/class/power_supply/battery/type
				echo 1   > /sys/class/power_supply/battery/present
				echo 100 > /sys/class/power_supply/battery/capacity
				echo 4200 > /sys/class/power_supply/battery/batt_vol
				echo 4200000 > /sys/class/power_supply/battery/voltage_now
				echo 250 > /sys/class/power_supply/battery/batt_temp
				echo 250 > /sys/class/power_supply/battery/temp
				echo "Full"     > /sys/class/power_supply/battery/status
				echo "Good"     > /sys/class/power_supply/battery/health
				echo "Li-ion"   > /sys/class/power_supply/battery/technology
				echo "Mains" > /sys/class/power_supply/ac/type
				echo 1 > /sys/class/power_supply/ac/online
				echo "USB" > /sys/class/power_supply/usb/type
				echo 0 > /sys/class/power_supply/usb/online
				say "power_supply shim: no real battery found; faked (type=Battery/Mains/USB)"
			else
				say "power_supply shim: tmpfs mount FAILED"
			fi
		fi
		# ------------------------------------------------------------------
		# Give Android a real /dev: a tmpfs that we populate.
		#
		# Android's init mknod()s /dev/__kmsg__ and /dev/__null__ itself and logs
		# through the former; devtmpfs refuses userspace-created device nodes, so
		# that mknod failed, klog_fd stayed -1 and *every* message init produced
		# was thrown away - which is why no boot so far showed a single line of
		# Android's own logging.  A tmpfs allows mknod, so we mount one and
		# populate the nodes Android expects; init inherits it because its own
		# tmpfs mount is disabled by the tmpfx patch (legacy/build-android.sh).
		# ------------------------------------------------------------------
		say "--- preparing /dev (tmpfs, populated by hand) ---"
		mount -t tmpfs none /dev 2>/dev/null || say "  tmpfs mount on /dev FAILED"
		mkdir -p /dev/pts /dev/socket /dev/graphics /dev/input /dev/block 2>/dev/null
		# the static "mem" devices: they announce no uevent, so nothing else
		# would create them
		mknod /dev/null    c 1 3  2>/dev/null
		mknod /dev/zero    c 1 5  2>/dev/null
		mknod /dev/full    c 1 7  2>/dev/null
		mknod /dev/random  c 1 8  2>/dev/null
		mknod /dev/urandom c 1 9  2>/dev/null
		mknod /dev/kmsg    c 1 11 2>/dev/null
		mknod /dev/tty     c 5 0  2>/dev/null
		mknod /dev/console c 5 1  2>/dev/null
		mknod /dev/tty0    c 4 0  2>/dev/null
		mknod /dev/ptmx    c 5 2  2>/dev/null
		# Gingerbread's init blocks up to COMMAND_RETRY_TIMEOUT (5 s) waiting for
		# /dev/.coldboot_done, which stock ueventd creates.  The port disables
		# ueventd (it hand-builds /dev), so provide the file to skip the wait.
		# Do NOT pre-create /dev/__kmsg__ or /dev/__null__: GB init only enables
		# its kmsg log when its own mknod() succeeds, and open_devnull_stdio()
		# exit(1)s when it does not.
		: > /dev/.coldboot_done
		# Everything else comes from sysfs.  Explicit enumeration, no find:
		# busybox' find needs CONFIG_FEATURE_FIND_NAME for -name, which this
		# static busybox does not have (a silent zero-match that cost a boot).
		# Misc device numbers come from /proc/misc.  Do NOT try to guess sysfs
		# paths: the logger registers names containing '/' (log/main) and they do
		# not appear where one would expect - that is why /dev/log/* never got
		# created while /dev/binder and /dev/ashmem did.
		say "  /proc/misc: $(tr '\n' ' ' < /proc/misc)"
		say "  /sys/class/misc: $(ls /sys/class/misc 2>&1 | tr '\n' ' ')"
		mkdir -p /dev/log 2>/dev/null
		# Misc device names are kernel-side identifiers: binder, ashmem and
		# log_main/log_events/log_radio (the 2009 ABI).  The old driver used
		# "log/main", which no Android init special-case matches, so init
		# created /dev/main instead of /dev/log/main.
		for m in binder ashmem log_main log_events log_radio; do
			case "$m" in
				log_*) dev="/dev/log/${m#log_}" ;;
				*)     dev="/dev/$m" ;;
			esac
			mm=$(awk -v n="$m" '$2 == n { print $1; exit }' /proc/misc 2>/dev/null)
			[ -n "$mm" ] || continue
			[ -c "$dev" ] || mknod "$dev" c 10 "$mm" 2>/dev/null
		done
		# The Android kernel interfaces must be world-accessible: zygote opens
		# /dev/ashmem as root for the heap, then the GC (possibly after a
		# setuid to system/uid 1000) opens it again for the mark stack.
		# Keep re-applying the mode in the background - Android's own init can
		# re-create device nodes - and log it so an EACCES can be told apart
		# from a uid problem.
		chmod 0666 /dev/binder /dev/ashmem 2>/dev/null
		chmod 0666 /dev/log/main /dev/log/events /dev/log/radio 2>/dev/null
		# system_server (uid 1000) must be able to read/write /dev/urandom and
		# open /dev/tty0; SurfaceFlinger also opens it.
		chmod 0666 /dev/urandom /dev/random /dev/tty0 /dev/fb0 /dev/fb1 2>/dev/null
		chmod 0755 /dev 2>/dev/null
		mount -o remount,dev /dev 2>/dev/null
		DEVLOG="$LOGDIR/devmode.log"
		[ -n "$DIAG" ] && (
			while true; do
				# Hedge: if anything remounts /dev with nodev, opening *any*
				# device node returns EACCES even for root (may_open_dev()).
				mount -o remount,dev /dev 2>/dev/null
				chmod 0755 /dev 2>/dev/null
				chmod 0666 /dev/ashmem /dev/binder 2>/dev/null
				chmod 0666 /dev/log/main /dev/log/events /dev/log/radio 2>/dev/null
				chmod 0666 /dev/urandom /dev/random /dev/tty0 /dev/fb0 /dev/fb1 2>/dev/null
				chmod 0666 /dev/snd/* 2>/dev/null
				for f in /dev/input/event*; do chmod 0666 "$f" 2>/dev/null; done
				ls -ld /dev /dev/ashmem /dev/binder /dev/log/main 2>&1 \
					| sed 's/^/devmode: /' >> "$DEVLOG"
				grep -E " /dev " /proc/mounts 2>/dev/null \
					| sed 's/^/devmode-mount: /' >> "$DEVLOG"
				# Only log the /dev state occasionally: this loop used to write
				# to the SD every 2 s and the freeze looks storage-related.
				sleep 30
			done
		) &
		for i in 0 1 2 3; do
			s="/sys/class/graphics/fb$i/dev"
			[ -e "$s" ] || continue
			mm=$(cat "$s")
			[ -c "/dev/fb$i" ] || mknod "/dev/fb$i" c "${mm%%:*}" "${mm##*:}" 2>/dev/null
		done
		for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
			s="/sys/class/input/event$i/dev"
			[ -e "$s" ] || continue
			mm=$(cat "$s")
			[ -c "/dev/input/event$i" ] || mknod "/dev/input/event$i" c "${mm%%:*}" "${mm##*:}" 2>/dev/null
			# mknod creates these root-only (or the underlying devtmpfs did),
			# so system_server (uid 1000) cannot open e.g. the physical Home
			# button's event4 ("Permission denied" in the EventHub log).
			chmod 0666 "/dev/input/event$i" 2>/dev/null
		done
		# ALSA sound nodes.  ctr_snd.c registers an ALSA PCM card, but the
		# Android ueventd is disabled (the port hand-builds /dev), so nothing
		# would otherwise create /dev/snd/*.  Enumerate the sound class:
		# controlC0, pcmC0D0p and timer each expose a dynamic major:minor in
		# their sysfs `dev` attribute (ALSA minors are not fixed).
		mkdir -p /dev/snd 2>/dev/null
		for s in /sys/class/sound/*; do
			[ -e "$s/dev" ] || continue
			n=$(basename "$s")
			mm=$(cat "$s/dev")
			[ -c "/dev/snd/$n" ] || mknod "/dev/snd/$n" c "${mm%%:*}" "${mm##*:}" 2>/dev/null
		done
		# mediaserver (the ALSA HAL's home) runs as root here, but keep the
		# nodes world-accessible just like the framebuffers and inputs.
		chmod 0666 /dev/snd/* 2>/dev/null
		say "  /dev/snd: $(ls /dev/snd 2>/dev/null | tr '\n' ' ')"
		# M4 bring-up: dump raw events from the dedicated touchscreen node
		# ("Nintendo 3DS touchscreen", classes=0x4 in EventHub).  Flushed per
		# event so the log survives even if the system later freezes.  tsdump
		# scans /dev/input itself, so it does not depend on the evdev minor
		# that probe order produced.
		if [ -n "$DIAG" ] && [ -x /bin/tsdump ] && [ -d /dev/input ]; then
			tsdump >> "$LOGDIR/tsdump.log" 2>&1 &
			say "tsdump running on the touchscreen -> tsdump.log"
		fi
		# Visible touch marker on the TOP screen (fb0): lets us confirm the touch
		# lands where the finger is, independently of Android and of the bottom
		# screen (which SurfaceFlinger owns).
		if [ -n "$DIAG" ] && [ -x /bin/tsmark ] && [ -e /dev/fb0 ] && [ -d /dev/input ]; then
			tsmark /dev/fb0 >> "$LOGDIR/tsmark.log" 2>&1 &
			say "tsmark running (touch marker on the top screen)"
		fi
		# M4: select the touchscreen coordinate mapping.  The driver exposes
		# /proc/ctr_touch_map; bit0 = swap X/Y, bit1 = invert the result X,
		# bit2 = invert the result Y.  The correct map is the naive identity
		# (0): the digitizer's native axes already line up with the landscape
		# 320x240 Android renders in (the xerpi bottom_lcd driver that worked
		# used exactly this, and the corner taps in tsdump.log land on the
		# corners only with map 0).  The upstream linux-3ds DTS asks for
		# touchscreen-swapped-x-y + touchscreen-inverted-y (3), but that is
		# for the portrait panel and rotates/mirrors the touch here -- it was
		# what made taps open the notification shade.  An optional
		# /mnt/sd/CYANO3DS/touch-map.txt overrides it, so a wrong guess can
		# be corrected by editing one file on the card instead of rebuilding
		# the kernel.
		if [ -e /proc/ctr_touch_map ]; then
			TM=0
			if [ -f "$LOGDIR/touch-map.txt" ]; then
				TM=$(cat "$LOGDIR/touch-map.txt" 2>/dev/null)
				case "$TM" in
					''|*[!0-9]*) TM=0 ;;
					*) [ "$TM" -le 7 ] 2>/dev/null || TM=0 ;;
				esac
			fi
			echo "$TM" > /proc/ctr_touch_map 2>/dev/null
			say "touch map: $TM (bit0=swap, bit1=invert-x, bit2=invert-y) -> $(cat /proc/ctr_touch_map 2>/dev/null)"
		fi
		# M4: the touchscreen *calibration* -- the raw digitizer values that
		# correspond to the panel's edges.  The eight-way map above picks the
		# orientation, but scaling the digitizer's whole 0..4095 range onto
		# 320x240 is a gain-and-offset error: taps are right near the middle
		# of the screen and drift further out the further out you reach, and
		# no swap or invert can fix that.  The kernel default is Octoblimp's
		# measured full-domain inset (256..3840); an optional
		# /mnt/sd/CYANO3DS/touch-cal.txt ("x0 x1 y0 y1") overrides it,
		# which is what tscal writes when it is run from the "calibrate"
		# trigger further down.  Same "edit one file on the card" property as
		# touch-map.txt: a bad measurement never needs a kernel rebuild.
		if [ -e /proc/ctr_touch_cal ]; then
			CAL="256 3840 256 3840"
			if [ -f "$LOGDIR/touch-cal.txt" ]; then
				RC=$(tr -s ' \t\n' '   ' < "$LOGDIR/touch-cal.txt" 2>/dev/null)
				case "$RC" in
					[0-9]*" "[0-9]*" "[0-9]*" "[0-9]*) CAL="$RC" ;;
					*) CAL="256 3840 256 3840" ;;
				esac
			fi
			echo "$CAL" > /proc/ctr_touch_cal 2>/dev/null
			say "touch calibration: $CAL -> $(cat /proc/ctr_touch_cal 2>/dev/null)"
		fi
		# Cheap always-on flip-rate probe: the display driver exposes the flip
		# count plus the pan/wait/blit counters.  A few bytes every 15 s lets us
		# see whether the compositor is actually running at the panel rate.
		if [ -e /proc/ctr_lcd_flips ]; then
			(
				while true; do
					echo "$(date +%H:%M:%S) $(tr '\n' ' ' < /proc/ctr_lcd_flips)" \
						>> "$LOGDIR/flips.log"
					sleep 15
				done
			) >/dev/null 2>&1 &
			say "flip-rate logger armed -> flips.log (every 15s)"
		fi
		# M4 diagnostic: once the launcher should be up, dump the window and
		# activity state so we can see whether the launcher / battery dialog
		# windows are visible, touchable and focused, and why a dispatched
		# touch misses them.  Each call is a binder round trip to a possibly
		# wedged service, so they run in the background after the boot settles.
		if [ -n "$DIAG" ] && [ -x /system/bin/dumpsys ]; then
			(
				sleep 75
				timeout 25 /system/bin/dumpsys window         > "$LOGDIR/dumpsys-window.txt"   2>&1
				timeout 25 /system/bin/dumpsys activity       > "$LOGDIR/dumpsys-activity.txt" 2>&1
				timeout 25 /system/bin/dumpsys SurfaceFlinger > "$LOGDIR/dumpsys-sf.txt"       2>&1
				# Power state: mPowerState shows whether the screen timed out
				# (SCREEN_ON_BIT / SCREEN_BRIGHT_BIT), which gates wake-key and
				# touch delivery.
				timeout 25 /system/bin/dumpsys power          > "$LOGDIR/dumpsys-power.txt"   2>&1
				# Status bar disable flags: mDisabled should have bit 0
				# (DISABLE_EXPAND) set by the helper.
				timeout 25 /system/bin/dumpsys statusbar      > "$LOGDIR/dumpsys-statusbar.txt" 2>&1
			) &
			say "dumpsys window/activity/power probe armed (+75s)"
		fi
		# The old CLOSE_SYSTEM_DIALOGS broadcast (and the DISABLE_EXPAND helper
		# and the driver's top-strip touch drop) are gone: touch works now, so
		# the status bar / notification shade is allowed to work normally.
		# Only the keep-awake userActivity remains in the helper.
		# The Dev Tools app (com.android.development) is installed but
		# deliberately *not* auto-launched any more: with the input fixes in
		# place the normal launcher comes up on its own, and the user opens
		# whichever app they want.
		# Android's notification shade is a touch-modal, focusable
		# TYPE_STATUS_BAR_PANEL window: while it is visible it swallows every
		# pointer event and keeps key focus, so a stray status-bar touch made
		# PortHelper: keep the screen awake (IPowerManager.userActivity every 5 s)
		# and dismiss the keyguard.  Without it PowerManagerService's screen-off
		# timer fires during music playback / games and WindowManagerService then
		# drops every touch and consumes every WAKE_DROPPED key (the "screen went
		# off and it does not accept input" report).  It retries until the window
		# manager and power service are published, then runs forever; the
		# supervisor restarts it if the VM dies.
		if [ -f /bin/porthelper.jar ] && [ ! -f "$LOGDIR/nohelper" ]; then
			(
				BCP=/system/framework/core.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/android.policy.jar:/system/framework/services.jar
				# let zygote finish its boot-class-path preload first: a second
				# Dalvik VM starting on the single core at the same time just
				# makes the boot slower.
				sleep 20
				while true; do
					PATH=/system/bin:$PATH BOOTCLASSPATH="$BCP" \
						CLASSPATH=/bin/porthelper.jar \
						/system/bin/app_process /system/bin com.cyano3ds.PortHelper
					echo "porthelper: exited, restarting"
					sleep 5
				done
			) > "$LOGDIR/porthelper.log" 2>&1 &
			say "PortHelper armed (keep-awake + keyguard, supervised)"
		elif [ -f /bin/porthelper.jar ]; then
			say "PortHelper DISABLED ($LOGDIR/nohelper present)"
		fi
		# Backup for the case where the helper cannot run at all: "stay on while
		# plugged in" gives at least SCREEN_DIM (wake keys work; a bright lock is
		# still needed for touch).  On CM7 the real MCU battery reports a
		# *discharging* battery, so this alone does NOT keep the screen on -- the
		# PortHelper above is what does.
		if [ -x /system/bin/svc ]; then
			(
				BCP=/system/framework/core.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/android.policy.jar:/system/framework/services.jar
				# `svc power stayon true` dies with an NPE if it runs before
				# system_server has published the "power" service (it did on
				# every CM7 boot: the old fixed 25 s sleep was far too early,
				# system_server only appears ~2 min in).  Wait for the process
				# (cheap pidof, no Dalvik VM) then a few seconds for PowerManager.
				i=0
				while [ $i -lt 120 ]; do
					[ -n "$(pidof system_server 2>/dev/null)" ] && break
					i=$((i+1))
					sleep 5
				done
				sleep 10
				PATH=/system/bin:$PATH BOOTCLASSPATH="$BCP" \
					/system/bin/svc power stayon true
			) > "$LOGDIR/svc-stayon.log" 2>&1 &
			say "svc power stayon true armed (waits for system_server)"
		fi
		# The old `service call statusbar 2` watchdog is deliberately gone.
		# (For the record it was harmless anyway: in Android's `android.app.IStatusBar`
		# the transaction codes are 1=activate/expand, 2=deactivate/collapse,
		# 3=toggle, 4=disable -- verified by decompiling framework.jar -- so the
		# watchdog was *collapsing* the shade, not opening it.)  The real fix is
		# the DISABLE_EXPAND helper above; the status-bar touch drop in the TSC
		# driver is the belt-and-braces fallback.
		# Sample the focused window every 60 s: if the notification shade is
		# what eats the touches, mCurrentFocus stays TrackingView.
		if [ -n "$DIAG" ] && [ -x /system/bin/dumpsys ]; then
			(
				while true; do
					sleep 60
					/system/bin/dumpsys window 2>/dev/null | \
						grep -E 'mCurrentFocus|mLastFocus' 2>/dev/null
				done
			) >> "$LOGDIR/focus.log" 2>&1 &
			say "focus probe armed (every 60s)"
		fi
		for i in 0 1 2 3 4 5 6 7; do
			s="/sys/class/block/loop$i/dev"
			[ -e "$s" ] || continue
			mm=$(cat "$s")
			[ -b "/dev/loop$i" ] || mknod "/dev/loop$i" b "${mm%%:*}" "${mm##*:}" 2>/dev/null
		done
		for b in vda vda1 vdb vdb1 mmcblk0 mmcblk0p1 mmcblk1 mmcblk1p1; do
			s="/sys/class/block/$b/dev"
			[ -e "$s" ] || continue
			mm=$(cat "$s")
			[ -b "/dev/$b" ] || mknod "/dev/$b" b "${mm%%:*}" "${mm##*:}" 2>/dev/null
		done
		# Android's display is fb1 (the kernel console owns fb0)
		ln -sf /dev/fb1 /dev/graphics/fb0 2>/dev/null
		ln -sf /dev/fb0 /dev/graphics/fb1 2>/dev/null
		say "  /dev now has $(ls /dev | wc -l) entries"
		say "  graphics/fb0: $([ -e /dev/graphics/fb0 ] && echo ok || echo MISSING)"
		for m in binder ashmem log/main log/events log/radio null zero kmsg console; do
			[ -e "/dev/$m" ] && say "  /dev/$m present" \
					   || say "  /dev/$m STILL MISSING"
		done

		# Smoke-test every kernel interface Android needs *before* handing over, so a
		# crash in Android's init has an obvious suspect list.
		if [ -x /bin/androidtest ]; then
			# Keep a dedicated copy on the card: the tee pipeline can lose
			# the tail if a test wedges, and this is the first place a
			# kernel/ABI mismatch shows up.
			androidtest > "$LOGDIR/androidtest.log" 2>&1
			cat "$LOGDIR/androidtest.log" | tee -a "$FBTXT" >> "$LOGF"
		fi
		# servicemanager is the critical service that crash-loops.  init
		# zaps service stderr to /dev/null, so run it here directly with
		# stderr captured: a failure prints its reason, a success blocks in
		# binder_loop (which is itself the answer).
		# The standalone servicemanager diagnostic is off by default now that
		# Android's init starts the real one; it left a stale context manager
		# around.  Set SM_DIAG=1 in the init to re-enable it.
		if [ -n "${SM_DIAG:-}" ] && [ -x /system/bin/servicemanager ]; then
			say ""
			say "=== diagnostic: servicemanager (5 s, stderr captured) ==="
			/system/bin/servicemanager > "$LOGDIR/servicemanager.log" 2>&1 &
			sp=$!
			sleep 5
			if kill -0 $sp 2>/dev/null; then
				kill $sp 2>/dev/null
				wait $sp 2>/dev/null
				say "  still running after 5 s -> binder OK, blocked in binder_loop"
			else
				wait $sp
				say "  exited early with status $?"
			fi
			say "  --- output ---"
			cat "$LOGDIR/servicemanager.log" 2>/dev/null | tee -a "$LOGF"
		fi
		# /data lives in an ext4 image on the FAT SD (loop device).  Write a
		# file, flush, drop the caches and read it back from the medium, to
		# check the SD write path really persists what the kernel wrote.
		say ""
		say "=== /data write/verify test (loop image on the SD card) ==="
		: > /data/wtest
		dd if=/dev/zero of=/data/wtest bs=4096 count=8 2>/dev/null
		printf 'HEADER!!' | dd of=/data/wtest bs=1 seek=0 conv=notrunc 2>/dev/null
		sync
		echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
		say "  first 16 bytes read back from the device:"
		hexdump -C -n 16 /data/wtest 2>/dev/null | tee -a "$LOGF"
		# --- late diagnostics ------------------------------------------------
		# The system freezes hard right when the app processes start doing heavy
		# SD I/O (loop-mounted /system reads + log writes).  Probe output goes
		# to a tmpfs file and a flusher copies it to the SD every 10 s, so the
		# probes themselves add almost no SD write load.
		if [ -n "$DIAG" ] && [ -x /bin/lateprobe ]; then
			: > /mnt/probe/late-diag.txt
			: > /mnt/probe/late-probe.log
			( while true; do cp /mnt/probe/late-diag.txt "$LOGDIR/late-diag.txt" 2>/dev/null; cp /mnt/probe/late-probe.log "$LOGDIR/late-probe.log" 2>/dev/null; cp /mnt/probe/service-stderr.log "$LOGDIR/service-stderr.log" 2>/dev/null; cp /mnt/probe/android-init.log "$LOGDIR/android-init.log" 2>/dev/null; [ -s /data/anr/traces.txt ] && cp /data/anr/traces.txt "$LOGDIR/traces.txt" 2>/dev/null; sync; sleep 5; done ) &
			(
				n=0
				while [ $n -lt 300 ]; do
					/system/bin/service list 2>/dev/null | grep -q SurfaceFlinger && break
					n=$((n+1))
					sleep 2
				done
				# Keep the periodic snapshot cheap: the app-start race is handled
				# by /bin/appwatch, which writes straight to the SD card.  The
				# heavy sysrq-t/Java probes here were slowing the boot so much
				# that it never reached the app at all.
				/bin/lateprobe sf quick
				# The SIGQUIT Dalvik thread dumps that used to run here every
				# ~10 s are gone: they were for the system_server deadlock, and
				# each one stops *every* app thread (including the input test
				# app) for up to a second -- a large part of the "glitchy /
				# delayed touches" symptom.  The kernel-side detectors (hung
				# task, softlockup, sysrq-t) are still there if a hang ever
				# needs naming.
				i=0
				while [ $i -lt 600 ]; do
					/bin/lateprobe q$i quick
					i=$((i+1))
					sleep 30
				done
			) >> /mnt/probe/late-probe.log 2>&1 &
			if [ -x /bin/appwatch ]; then
				# Deliberately NOT at nice -20: on a single CPU the
				# /proc scan that fires the moment an app forks could starve
				# system_server/zygote, which looks exactly like the freeze
				# we are chasing.  Its steady state is cheap (one
				# /proc/loadavg read and a sleep).
				/bin/appwatch >/dev/null 2>&1 &
			fi
			say "late diagnostics armed (tmpfs-buffered, stack probe on app start)"
		fi
		# (The standalone app_process diagnostic was removed: it runs zygote
		# without init's socket setup, so it always failed with
		# "ANDROID_SOCKET_zygote unset" and drowned out the real zygote's log.
		# Read android-log.txt for zygote/system_server instead.)
		# --- bring-up: pre-dexopt system packages as root ----------------
		# installd's `dexopt --zip` dies with SIGSEGV (status=0x000b) and
		# leaves a 0xff DexOptHeader, so system_server cannot load the system
		# providers and the whole system process crashes (boot loop).  The
		# boot-classpath jars were optimized by the VM's --dex path and are
		# fine, so do the --zip optimization here, as root, writing exactly
		# the cache files system_server expects.
		if [ -x /system/bin/dexopt ] && [ -d /data/dalvik-cache ]; then
			say ""
			say "=== pre-dexopt of /system/framework + /system/app (as root) ==="
			export BOOTCLASSPATH=/system/framework/core.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/android.policy.jar:/system/framework/services.jar
			# CM7 (2.3.7) resolves the optimized dex of every /system file through
			# $ANDROID_CACHE/dalvik-cache (libdex/OptInvocation.c) and only falls
			# back to $ANDROID_DATA/dalvik-cache when dalvik.vm.dexopt-data-only=1.
			# That property is not set here because Android's init -- which loads
			# build.prop -- has not started yet.  Point ANDROID_CACHE at /data so
			# the pre-dexopt VM also looks in (and finds) /data/dalvik-cache, the
			# directory this loop writes to.  Without it only the bootstrap
			# core.jar succeeds and every other jar/app dexopt aborts.
			export ANDROID_CACHE=/data
			: > /mnt/probe/dexopt.log
			: > /mnt/probe/dexopt-results.txt
			ok=0; bad=0; skip=0
			total=$(ls /system/framework/*.jar /system/app/*.apk 2>/dev/null | wc -l)
			i=0
			# Optimize the boot-classpath jars first, in BOOTCLASSPATH order.  A
			# plain glob runs them alphabetically, so am.jar, bouncycastle.jar,
			# android.policy.jar ... ran before core.jar had a cache, and their
			# dexopt aborted (the NoClassDefFoundError path SIGSEGVs).  The
			# later glob entries then skip the BCP jars as "already valid".
			BCP_FIRST=""
			for b in core bouncycastle ext framework android.policy services; do
				BCP_FIRST="$BCP_FIRST /system/framework/$b.jar"
			done
			for src in $BCP_FIRST /system/framework/*.jar /system/app/*.apk; do
				[ -f "$src" ] || continue
				i=$((i+1))
				base=$(basename "$src")
				# Live progress on the BOTTOM screen: the pre-dexopt is the
				# longest single phase (~100 s) and used to look like a hang.
				say "  [$i/$total] $base"
				case "$src" in
					/system/framework/*) cache="/data/dalvik-cache/system@framework@$base@classes.dex" ;;
					*)                   cache="/data/dalvik-cache/system@app@$base@classes.dex" ;;
				esac
				if [ -f "$cache" ]; then
					fb=$(hexdump -C -n 1 "$cache" 2>/dev/null | awk 'NR==1{print $2}')
					if [ "$fb" = "64" ]; then	# 'd' = valid "dey\n" DexOptHeader
						skip=$((skip+1))
						continue
					fi
				fi
				rm -f "$cache"
				/system/bin/dexopt --zip 3 4 "$src" "" \
					3<"$src" 4<>"$cache" >> /mnt/probe/dexopt.log 2>&1
				st=$?
				echo "$st $src" >> /mnt/probe/dexopt-results.txt
				if [ "$st" = 0 ]; then ok=$((ok+1)); else bad=$((bad+1)); fi
			done
			say "  dexopt: $ok ok, $bad failed, $skip already valid"
			say "  results (status path):"
			cat /mnt/probe/dexopt-results.txt | tee -a "$LOGF"
			say "  /data after pre-dexopt (ENOSPC shows up here):"
			df -h /data /data/dalvik-cache 2>/dev/null | tee -a "$LOGF"
			[ -s /mnt/probe/dexopt.log ] && { say "  dexopt.log tail:"; tail -20 /mnt/probe/dexopt.log | tee -a "$LOGF"; }
			# Save this known-good cache for the next boot.  Also re-save when a
			# card APK changed and we deliberately dropped its stale odex above
			# (otherwise the next boot would restore the stale cache again).
			if { [ "$DALVIK_RESTORED" = 0 ] || [ "$DALVIK_DIRTY" = 1 ]; } && \
			   [ -n "$(ls -A /data/dalvik-cache 2>/dev/null)" ]; then
				tar cf "$DALVIK_TAR.tmp" -C /data dalvik-cache 2>/dev/null \
					&& mv -f "$DALVIK_TAR.tmp" "$DALVIK_TAR" \
					&& say "  saved /data/dalvik-cache -> android/dalvik.tar"
			fi
		fi
		# Kernel-side ALSA PCM probe: open the port's PCM card and push a few
		# periods through it *before* Android starts, so a hang can be
		# attributed to the kernel PCM path rather than to CM7's
		# hardware/alsa_sound HAL.  It is a normal bionic binary out of
		# /system (the same way the pre-dexopt runs /system/bin/dexopt); the
		# output goes to tmpfs and the kernel's ctr_lcd_fb watchdog captures a
		# stall.
		if [ -x /bin/alsaprobe ]; then
			say ""
			say "=== ALSA PCM probe (kernel path, before Android) ==="
			LD_LIBRARY_PATH=/system/lib /bin/alsaprobe hw:0,0 \
				> /mnt/probe/alsaprobe.log 2>&1
			say "alsaprobe rc=$?"
			tail -20 /mnt/probe/alsaprobe.log | tee -a "$LOGF"
		fi
		# Leave a clear message on the bottom screen: the renderer is about to be
		# stopped so Android can own the display, and the first boot then spends
		# minutes in Dalvik/zygote warm-up with a frozen screen.
		say ""
		say "Android is starting now - the first boot takes ~2-3 minutes."
		say "Do NOT power off: watch the TOP screen - it counts the packages"
		say "as system_server scans them; the BOTTOM screen shows ADW when ready."
		sleep 4
		# Stop the log renderer while Android owns the display.  Make sure it
		# is really gone (the bottom screen showing the boot log later means
		# fbsay was still drawing, or Android never drew anything).
		kill -9 $(pidof fbsay) 2>/dev/null
		sleep 1
		say "fbsay pids after kill: [$(pidof fbsay)]"
		# Simple input test (bottom screen): if the card carries
		# CYANO3DS/inputtest, let the user verify the touchscreen and every
		# button at the evdev level before Android starts.  Holding START for
		# ~1.5 s (or the 120 s timeout) continues the boot, so it does not need
		# a separate build; remove the flag to skip it.  Keeping it before
		# Android means the UI cannot be the cause of a missing event.
		if [ -x /bin/inputtest ] && [ -e "$LOGDIR/inputtest" ]; then
			say "=== input test: tap the box, press every button ==="
			say "    (hold START ~1.5s, or wait, to continue to Android)"
			inputtest /dev/fb1 120 > "$LOGDIR/inputtest.log" 2>&1
			say "input test done -> $LOGDIR/inputtest.log"
		fi
		# Touch calibration: if the card asks for it, take over the bottom
		# screen and measure the digitizer->screen mapping.  tscal scores all
		# eight candidate orientations, writes the winner to
		# /proc/ctr_touch_map and CYANO3DS/touch-map.txt, and fits the
		# same five taps for the affine calibration into /proc/ctr_touch_cal
		# and CYANO3DS/touch-cal.txt.  Both are used immediately by the
		# Android that starts next, and again on later boots.
		if [ -x /bin/tscal ] && [ -e "$LOGDIR/calibrate" ]; then
			# fbsay renders the boot log onto the SAME framebuffer the
			# calibration targets are drawn on and repaints a page every
			# few seconds, so a page landing on top of a target hides it
			# for the whole 30 s timeout.  It is already killed above, but
			# do it again here: the targets must be visible whatever path
			# we came in by, and this is the last thing standing between
			# the user and a successful measurement.  (tscal also kills it
			# by name itself, and repaints its target once a second.)
			kill -9 $(pidof fbsay) 2>/dev/null
			sleep 1
			say "tscal: tap the five targets on the BOTTOM screen, in order"
			say "tscal: (they are drawn one at a time; the black screen is tscal)"
			tscal /dev/fb1 "" "$LOGDIR/touch-map.txt" "$LOGDIR/touch-cal.txt" \
				> "$LOGDIR/tscal.log" 2>&1
			say "tscal: $(tail -1 "$LOGDIR/tscal.log" 2>/dev/null)"
			# One-shot: once the mapping has been measured, remove the
			# trigger so the next boot goes straight to Android (the chosen
			# map is remembered in touch-map.txt and /proc/ctr_touch_map,
			# and the calibration in touch-cal.txt /proc/ctr_touch_cal).
			# Only on success -- a boot where nobody could see or tap the
			# targets must be retryable -- and only if the calibration fit
			# was good enough to apply, so a half-tapped run cannot leave
			# a bad touch-cal.txt behind.
			if grep -q "applied map=" "$LOGDIR/tscal.log" 2>/dev/null &&
			   grep -q "applied cal " "$LOGDIR/tscal.log" 2>/dev/null; then
				rm -f "$LOGDIR/calibrate" 2>/dev/null
				say "tscal: done, removed calibrate (one-shot)"
			fi
		fi
		# ------------------------------------------------------------------
		# Dalvik-cache layout + optional CM7 JIT toggle.
		#
		# CM7.2 (unlike upstream AOSP) routes every /system dex through
		# $ANDROID_CACHE/dalvik-cache (== /cache/dalvik-cache) unless the
		# property dalvik.vm.dexopt-data-only=1 (libdex/OptInvocation.c and
		# frameworks/base/cmds/installd/commands.c).  The port keeps one
		# /data/dalvik-cache.  build.prop already carries the property, but
		# /default.prop is loaded by Android's init *before* build.prop, and the
		# pre-dexopt / helper VMs run even earlier, so set it here too.  Writing
		# it to /default.prop (rootfs, writable) also fixes the early
		# app_process helpers (svc/am/service) that otherwise look in /cache.
		#
		# Card toggles, because the CM7 JIT is built for armv6-vfp while the
		# archived prebuilt build's JIT is armv5te:
		#   CYANO3DS/nojit   -> dalvik.vm.execution-mode=int:fast
		#   CYANO3DS/usejit  -> dalvik.vm.execution-mode=int:jit
		for kv in "dalvik.vm.dexopt-data-only=1"; do
			k=${kv%%=*}
			sed -i "/^${k}=/d" /default.prop 2>/dev/null
			echo "$kv" >> /default.prop
		done
		if [ -e "$LOGDIR/nojit" ]; then
			sed -i '/^dalvik.vm.execution-mode=/d' /default.prop 2>/dev/null
			echo 'dalvik.vm.execution-mode=int:fast' >> /default.prop
			say "CM7 JIT DISABLED by $LOGDIR/nojit (int:fast)"
		elif [ -e "$LOGDIR/usejit" ]; then
			sed -i '/^dalvik.vm.execution-mode=/d' /default.prop 2>/dev/null
			echo 'dalvik.vm.execution-mode=int:jit' >> /default.prop
			say "CM7 JIT forced ON by $LOGDIR/usejit (int:jit)"
		fi
		export ANDROID_CACHE=/data
		export ANDROID_DATA=/data
		say "dalvik-cache: /data (dexopt-data-only=1); ANDROID_CACHE=/data"
		# Run Android's init as a CHILD instead of exec'ing it.  As PID 1 a
		# segfault in it panics the kernel ("Attempted to kill init!
		# exitcode=0x0000000b" - exactly what the first Android attempt did).
		# As a child we survive it, learn the signal, and keep the shell.
		say ""
		say "=== starting @@ANDROID_NAME@@ init (as a child, not PID 1) ==="
		# Mute the (slow) top-screen console before the compositor takes off:
		# see the matching note in the CM7 handover above.
		echo 1 > /proc/sys/kernel/printk 2>/dev/null
		# The syscall tracer was essential for the early bring-up crashes but
		# now only adds overhead; run init directly and let the framework
		# logs (android-log.txt) carry the story.
		# Stream the init's own stdout/stderr into *tmpfs*, not straight onto the
		# SD card: if the card wedges, a tee blocked on the SD fills this pipe
		# and stops the init dead (it is the parent of every service).  The
		# late-diag flusher mirrors the file to the SD every few seconds.
		/bin/android-init 2>&1 | tee -a /mnt/probe/android-init.log
		st=$?
		say ""
		say "Android's init returned: status $st"
		if [ "$st" -gt 128 ]; then
			sig=$((st - 128))
			say "  killed by signal $sig"
			[ "$sig" = 11 ] && say "  signal 11 = SIGSEGV"
			[ "$sig" = 6 ] && say "  signal 6 = SIGABRT"
			[ "$sig" = 4 ] && say "  signal 4 = SIGILL"
		fi
		say "the machine stays alive on purpose; logs are in $LOGDIR/"
		# open_devnull_stdio() makes Android's init close the pipe to `tee` very
		# early, so the pipeline returning does NOT mean init exited.  Settle it
		# (is the init process still there?) and keep a rolling `ps` on the SD
		# card, so even a short boot shows how far the framework got.
		sleep 3
		if pidof android-init >/dev/null 2>&1; then
			say "  android-init is ALIVE (pid $(pidof android-init))"
		else
			say "  android-init is NOT running"
		fi
		(
			while true; do
				{
					echo "=== ps $(date) ==="
					ps
				} > /mnt/probe/ps.txt 2>&1
				cp /mnt/probe/ps.txt "$LOGDIR/ps.txt" 2>/dev/null
				cp /mnt/probe/android-init.log "$LOGDIR/android-init.log" 2>/dev/null
				sync
				sleep 15
			done
		) >/dev/null 2>&1 &
		# ------------------------------------------------------------------
		# sysmon: the moment system_server dies, snapshot everything that still
		# exists (its tombstone on the tmpfs /data is lost at power-off
		# otherwise), and drop a BOOT-OK marker when the framework is up.  This
		# is deliberately cheap: two pidof calls and one grep every 2 s.
		# ------------------------------------------------------------------
		(
			seen_ss=0; deaths=0; tick=0; last_ss=""
			while true; do
				ss=$(pidof system_server 2>/dev/null | awk '{print $1}')
				if [ -n "$ss" ]; then
					seen_ss=1
				else
					if [ "$seen_ss" = 1 ]; then
						seen_ss=0
						deaths=$((deaths+1))
						D="$LOGDIR/crash-$deaths"
						mkdir -p "$D/tombstones" "$D/anr" 2>/dev/null
						{
							echo "=== system_server died (death #$deaths) at $(date) ==="
							echo "uptime: $(cat /proc/uptime 2>/dev/null)"
							echo "--- ps ---"; ps
							echo "--- zygote present? $(pidof zygote 2>/dev/null) ---"
							echo "--- dmesg tail ---"; dmesg | tail -200
						} > "$D/report.txt" 2>&1
						cp /mnt/probe/android-log.txt "$D/android-log.txt" 2>/dev/null
						cp /data/tombstones/* "$D/tombstones/" 2>/dev/null
						cp /data/anr/traces.txt "$D/anr/traces.txt" 2>/dev/null
						say "sysmon: system_server DEATH #$deaths -> $D/report.txt"
						sync
					fi
				fi
				if [ ! -e "$LOGDIR/BOOT-OK.txt" ] && \
				   grep -q 'PowerManagerService: system ready!' /mnt/probe/android-log.txt 2>/dev/null; then
					echo "system ready at $(date)" > "$LOGDIR/BOOT-OK.txt"
					say "sysmon: BOOT-OK - PowerManagerService system ready"
					sync
				fi
				# Visible progress: the bottom screen is black until the launcher
				# and the first boot is minutes long, so at least put a heartbeat
				# on the TOP (kernel console) screen.
				tick=$((tick+1))
				if [ -n "$ss" ] && [ "$ss" != "$last_ss" ]; then
					last_ss="$ss"
					say "sysmon: system_server pid $ss up, PackageManager scanning"
				fi
				if [ -n "$ss" ] && [ $((tick % 8)) = 0 ]; then
					n=$(grep -c 'collecting certs' /mnt/probe/android-log.txt 2>/dev/null)
					echo "<4>FIRST BOOT: system_server up, ~$n/61 packages scanned, $((tick*2))s - do not power off" > /dev/kmsg 2>/dev/null
				fi
				sleep 2
			done
		) >/dev/null 2>&1 &
		say "sysmon armed (system_server death capture + BOOT-OK marker)"
		# If Android's init is not running at all, run servicemanager once on its
		# own under the tracer, with output on the SD card, so we can see exactly
		# which binder ioctl fails.  (If it IS running, its own log is the story.)
		if ! pidof android-init >/dev/null 2>&1 && \
		   [ -x /system/bin/servicemanager ] && [ -x /bin/trace ]; then
			say ""
			say "=== diagnostic: servicemanager under the tracer (15 s) ==="
			trace /system/bin/servicemanager > "$LOGDIR/servicemanager.log" 2>&1 &
			tp=$!
			sleep 15
			kill $tp 2>/dev/null
			wait $tp 2>/dev/null
			say "  trace -> $LOGDIR/servicemanager.log (last lines below)"
			tail -20 "$LOGDIR/servicemanager.log" 2>/dev/null
		fi
		test -n "$LOGF" && sync
		if [ -n "$DIAG" ] && [ -x /bin/fbsay ]; then
			fbsay -s 2 < "$FBTXT" >/dev/null 2>&1 &
		fi
	fi
fi

# --------------------------------------------------------------------------
say ""
say "staying in bring-up mode: starting an interactive shell on this (top)"
say "screen.  the touchscreen is a keyboard (blind: the on-screen keyboard"
say "is drawn by the old bottom-screen driver).  try: dmesg | tail -30"

while true; do
	/bin/sh -i
	echo "[init] shell exited, restarting"
	sleep 1
done
INIT
chmod 755 "$ROOT/init"

# The /init text is a quoted heredoc, so the boot-time shell variables are kept
# literal at build time.  The userspace version and the /data restore archive
# name are baked in as @@ANDROID_NAME@@ / @@DATATAR@@ placeholders and always
# rewritten for the selected flavor (the froyo flavor still yields
# "Android 2.2 (Froyo)" / "data-froyo.tar").
sed -i \
	-e "s/@@ANDROID_NAME@@/$ANDROID_NAME/g" \
	-e "s/@@DATATAR@@/$DATATAR/g" \
	"$ROOT/init"
if [ "$ANDROID_FLAVOR" != "froyo" ]; then
	# Gingerbread's boot classpath also carries bouncycastle.jar and
	# core-junit.jar.  The generated /init hardcodes the Froyo boot classpath
	# pre-dexopt and for the `svc`/app_process helpers; app_process refuses to
	# start the VM if the BCP does not match the jars the framework was built
	# against, so rewrite it.
	sed -i 's|/system/framework/core.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/android.policy.jar:/system/framework/services.jar|/system/framework/core.jar:/system/framework/bouncycastle.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/android.policy.jar:/system/framework/services.jar:/system/framework/core-junit.jar|g' "$ROOT/init"
fi

# ---------------------------------------------------------------------------
say "packing"
# ---------------------------------------------------------------------------
( cd "$ROOT" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -9 ) \
	> "$OUT/initramfs.cpio.gz"
ls -l "$OUT/initramfs.cpio.gz" | awk '{print "   ", $5, "bytes", $9}'

# ---------------------------------------------------------------------------
say "checking the busybox applets the init script uses"
# Only a handful of applets are compiled into the static busybox; using one that
# is not there fails *silently* at runtime on the console (see the find/-name
# episode).  Fail the build instead.
NEEDED="$INIT_APPLETS"
APPLET_LIST=/tmp/busybox-applets-$$
# The busybox is armel: on an x86 host it only runs under qemu-user (the
# qemu-user-static package in BUILD.md step 0).  Without this, --list fails
# silently and every applet looks missing.
BB_RUN=("$BB")
if ! "$BB" --list >/dev/null 2>&1; then
	command -v qemu-arm-static >/dev/null || {
		echo "FATAL: cannot run $BB on this host and qemu-arm-static is not installed" >&2
		exit 1; }
	BB_RUN=(qemu-arm-static "$BB")
fi
"${BB_RUN[@]}" --list > "$APPLET_LIST" 2>/dev/null || true
for a in $NEEDED; do
	# NOTE: do *not* pipe --list into `grep -q`: with pipefail, grep's early
	# exit closes the pipe, busybox --list dies of SIGPIPE and the pipeline
	# reports failure (141) even though the applet is present.  Collect the
	# list first, then grep the file (this false "missing applet" cost a build).
	grep -qx "$a" "$APPLET_LIST" || {
		echo "FATAL: busybox has no '$a' applet (init uses it)" >&2
		rm -f "$APPLET_LIST"
		exit 1; }
done
rm -f "$APPLET_LIST"
echo "    all $(echo $NEEDED | wc -w) applets present"

# ---------------------------------------------------------------------------
say "verifying the packed artefact"
# ---------------------------------------------------------------------------
# This check exists because the first version of this script symlinked /init
# to busybox and then wrote the script *through* the symlink: the result
# booted busybox's init applet instead of the port's init, which took two
# bring-up cycles to find.
V="$(mktemp -d)"
trap 'rm -rf "$ROOT" "$V"' EXIT
( cd "$V" && gzip -dc "$OUT/initramfs.cpio.gz" | cpio -idm 2>/dev/null )

[ -f "$V/init" ] || { echo "FAIL: /init is missing from the image" >&2; exit 1; }
[ -L "$V/init" ] && { echo "FAIL: /init is a symlink, not our script" >&2; exit 1; }
[ -x "$V/init" ] || { echo "FAIL: /init is not executable" >&2; exit 1; }
head -1 "$V/init" | grep -q '^#!/bin/sh' || {
	echo "FAIL: /init has the wrong interpreter: $(head -1 "$V/init")" >&2; exit 1; }
grep -q "bring-up init (PID 1)" "$V/init" || {
	echo "FAIL: /init is not the port's script" >&2; exit 1; }
grep -q "fbsay running" "$V/init" || {
	echo "FAIL: /init is truncated" >&2; exit 1; }
file "$V/bin/busybox" | grep -q "ELF" || {
	echo "FAIL: /bin/busybox is not an ELF (did a redirect follow the symlink?)" >&2
	exit 1; }
[ -x "$V/bin/fbsay" ] && [ -x "$V/bin/fbtest" ] || {
	echo "FAIL: test tools missing" >&2; exit 1; }

echo "    /init is a real $(stat -c%s "$V/init") byte script, busybox is an ELF, tools present"
sz=$(stat -c%s "$OUT/initramfs.cpio.gz")
[ "$sz" -gt 8388608 ] && echo "WARNING: larger than the loader's 8 MiB window" >&2
echo "==> ok"
