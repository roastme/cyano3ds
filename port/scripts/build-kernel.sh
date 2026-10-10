#!/bin/bash
# build-kernel.sh - configure and build the Cyano3DS (CM7.2) capable 3DS kernel
#
# Usage: build-kernel.sh [kernel-tree] [dist-dir]
#        defaults: $HOME/p3ds/src/linux-3ds  $HOME/p3ds/dist
#
# Produces (in dist/):
#   zImage                    - Linux kernel, loaded by firm_linux_loader
#   nintendo3ds_ktr.dtb       - device tree for New 3DS / New 3DS XL / New 2DS XL
#
# Requires: arm-linux-gnueabi-gcc (armel, matches linux-3ds' own CI toolchain)

set -euo pipefail

KD="${1:-$HOME/p3ds/src/linux-3ds}"
OUT="${2:-$HOME/p3ds/dist}"
PORT="$(cd "$(dirname "$0")/.." && pwd)"
DEF=nintendo3ds_android_defconfig

export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabi-

# Keep the compiled-in kernel identity neutral.  Settings > About phone >
# Kernel version shows /proc/version, which otherwise embeds the build host's
# user@hostname.  Override with KBUILD_BUILD_USER / KBUILD_BUILD_HOST.
export KBUILD_BUILD_USER="${KBUILD_BUILD_USER:-builder}"
export KBUILD_BUILD_HOST="${KBUILD_BUILD_HOST:-3ds}"

cd "$KD"

# ---------------------------------------------------------------------------
# The committed defconfig is the source of truth.  Copy it into the kernel
# tree; never copy a generated one back over the tracked file (C-8).
# ---------------------------------------------------------------------------
PORT_DEF="$PORT/kernel/configs/$DEF"
if [ -f "$PORT_DEF" ]; then
	cp "$PORT_DEF" "arch/arm/configs/$DEF"
	echo "==> using committed $PORT_DEF"
elif [ ! -f "arch/arm/configs/$DEF" ]; then
	# No committed defconfig yet: derive one from linux-3ds' nintendo3ds_defconfig.
	# The result goes to $OUT; review it and commit it under port/kernel/configs/.
	echo "==> generating arch/arm/configs/$DEF (no committed defconfig found)"
	make nintendo3ds_defconfig >/dev/null

	# Android userspace interfaces
	scripts/config --enable  STAGING
	scripts/config --enable  ANDROID_LEGACY_DRIVERS
	scripts/config --enable  ANDROID_ASHMEM
	scripts/config --enable  ANDROID
	scripts/config --enable  ANDROID_BINDER_IPC
	scripts/config --disable ANDROID_BINDERFS
	scripts/config --set-str ANDROID_BINDER_DEVICES binder
	scripts/config --enable  ANDROID_LOGGER
	scripts/config --enable  KUSER_HELPERS

	# display
	scripts/config --enable  FB
	scripts/config --enable  FRAMEBUFFER_CONSOLE
	scripts/config --enable  FB_CFB_FILLRECT
	scripts/config --enable  FB_CFB_COPYAREA
	scripts/config --enable  FB_CFB_IMAGEBLIT
	scripts/config --enable  CTR_LCD_FB

	# New 3DS ARM11 clock + cpufreq (the TWD timer is clocked from CPU/2)
	scripts/config --enable  COMMON_CLK
	scripts/config --enable  CTR_CPUFREQ

	make olddefconfig >/dev/null
	make savedefconfig >/dev/null
	cp defconfig "arch/arm/configs/$DEF"
	mkdir -p "$OUT"
	cp defconfig "$OUT/$DEF.generated"
	echo "==> generated $OUT/$DEF.generated"
	echo "    review it, then: cp $OUT/$DEF.generated $PORT_DEF"
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
echo "==> make $DEF"
make "$DEF" >/dev/null

# Keep the port's own options enabled even when an older generated defconfig is
# already on disk (the defconfig is only generated once, above).
# Display and clock options the generator used to add.  CTR_LCD_FB depends on
# FB, and a committed defconfig without these lines silently drops the LCD
# driver (C-8 change), so they are enforced here too.
scripts/config --enable  FB                   \
	       --enable  FRAMEBUFFER_CONSOLE  \
	       --enable  FB_CFB_FILLRECT      \
	       --enable  FB_CFB_COPYAREA      \
	       --enable  FB_CFB_IMAGEBLIT     \
	       --enable  COMMON_CLK           \
	       --enable  ANDROID_LEGACY_DRIVERS \
	       --enable  STAGING
scripts/config --enable  ANDROID_ASHMEM       \
	       --enable  ANDROID_LOGGER       \
	       --enable  ANDROID             \
	       --enable  ANDROID_BINDER_IPC  \
	       --disable ANDROID_BINDERFS    \
	       --set-str ANDROID_BINDER_DEVICES binder \
	       --enable  CTR_LCD_FB          \
	       --enable  CTR_CPUFREQ         \
	       --enable  CTR_EXTRAPAD        \
	       --enable  CTR_SND             \
	       --enable  SOUND               \
	       --enable  SND                 \
	       --enable  SND_PCM             \
	       --enable  KUSER_HELPERS       \
	       --enable  NETDEVICES          \
	       --enable  WLAN                \
	       --enable  WLAN_VENDOR_ATH     \
	       --enable  CFG80211            \
	       --enable  CFG80211_WEXT       \
	       --enable  ATH6K_LEGACY        \
	       --disable FW_LOADER_USER_HELPER
# Bring-up: the 3DS WiFi (Atheros AR6014G) is an SDIO card on the second
# controller (ctr_sdhc).  The Octoblimp port drives it with the pre-mainline
# staging ath6k driver (CONFIG_ATH6K_LEGACY), which still speaks the AR6002
# "hw2" protocol that mainline ath6kl dropped.  It is built in so it probes at
# boot (fwmode defaults to 1 = STA) and boots the AR6014 from Nintendo's four
# NWM blobs in the initramfs.  Mainline ath6kl must stay OFF: both drivers
# claim the same SDIO ids and the `wlan%d` interface.  See
# port/scripts/fix-wifi-octoblimp.py.
scripts/config --disable ATH6KL            \
	       --disable ATH6KL_SDIO
# A genuinely preemptible kernel.  The "freeze" turned out to be a long
# udelay() busy-wait inside a syscall (the stall watchdog caught the CPU in
# __loop_delay with __irq_svc in the backtrace).  With PREEMPT_VOLUNTARY a
# kernel busy-wait is *never* preempted, so that one loop starves every other
# task: userspace stops, the SD loggers stop, but timers/softirqs keep firing
# - exactly the "blinking square, frozen bar" symptom.  With full preemption
# the timer interrupt preempts the loop, so the system stays schedulable, the
# loggers keep flushing and the stall degrades into a slowdown instead of a
# hang.
scripts/config --enable  PREEMPT              \
	       --disable PREEMPT_VOLUNTARY
# Bring-up: with maxcpus=1 the softlockup/RCU detectors cannot name a hung
# task, and the framework deadlocks ~12 s after systemReady.
scripts/config --enable  DETECT_HUNG_TASK          \
	       --set-val DEFAULT_HUNG_TASK_TIMEOUT 20
# The machine used to freeze hard at ~80 s with *no* report at all (no
# hung-task report, no softlockup report, every SD writer stopped at once).
# lockdep found the cause: pxi_txrx() in ctr_pxi.c slept (might_sleep/mutex/
# wait_event) while the block layer called it from ->queue_rq under
# rcu_read_lock.  That is fixed by fix-pxi-atomic-sleep.py (step 9/9).
#
# The options below are kept for future bring-up (they are very useful and
# found this bug immediately), but they make the already-slow 3DS boot ~3-4x
# slower - with them on, the framework needed ~10 minutes to reach the
# launcher.  Re-enable when hunting another kernel bug.
#scripts/config --enable  DEBUG_KERNEL          \
#	       --enable  LOCKDEP               \
#	       --enable  PROVE_LOCKING         \
#	       --enable  DEBUG_LOCK_ALLOC      \
#	       --enable  DEBUG_SPINLOCK        \
#	       --enable  DEBUG_MUTEXES         \
#	       --enable  DEBUG_ATOMIC_SLEEP
# make sure the heavy options are off (a previous diagnostic build left them
# on): full lockdep made the boot ~3-4x slower, and its slowdown *hid* the
# timing-dependent freeze we are chasing.  LOCKDEP=1 turns them on again for a
# dedicated deadlock-hunting build.
if [ -n "${LOCKDEP:-}" ]; then
	echo "==> LOCKDEP diagnostic build (deadlock detection) - expect a slow boot"
	scripts/config --enable  DEBUG_KERNEL          \
		       --enable  LOCKDEP               \
		       --enable  PROVE_LOCKING         \
		       --enable  DEBUG_LOCK_ALLOC      \
		       --enable  DEBUG_SPINLOCK        \
		       --enable  DEBUG_MUTEXES         \
		       --enable  DEBUG_ATOMIC_SLEEP
elif [ -n "${LOCKDEBUG:-}" ]; then
	echo "==> LOCKDEBUG build (cheap lock/preempt checks, NO lockdep graph)"
	scripts/config --enable  DEBUG_KERNEL         \
		       --enable  DEBUG_SPINLOCK        \
		       --enable  DEBUG_MUTEXES         \
		       --enable  DEBUG_ATOMIC_SLEEP    \
		       --enable  DEBUG_PREEMPT         \
		       --enable  DEBUG_LIST
	# BUG_ON_DATA_CORRUPTION makes the list debug checks call BUG() (panic).
	# Keep it off so corruption is a WARN with a backtrace that the running
	# kmsgdump can write to the SD instead of a panic that kills every writer.
	scripts/config --disable BUG_ON_DATA_CORRUPTION
	scripts/config --disable PROVE_LOCKING         \
		       --disable DEBUG_LOCK_ALLOC      \
		       --disable LOCKDEP               \
		       --disable PROVE_RCU             \
		       --disable TRACE_IRQFLAGS
else
	scripts/config --disable PROVE_LOCKING         \
		       --disable DEBUG_LOCK_ALLOC      \
		       --disable DEBUG_SPINLOCK        \
		       --disable DEBUG_MUTEXES         \
		       --disable LOCKDEP
fi
# DEBUG_ATOMIC_SLEEP is cheap and reports EVERY "sleeping function called
# from invalid context" with a stack trace, whereas full lockdep printed only
# the first site (ctr_pxi.c:82) and then turned itself off.  Keep it on so the
# *next* such site can be found without hiding the freeze behind lockdep's
# slowdown.
scripts/config --enable DEBUG_KERNEL --enable DEBUG_ATOMIC_SLEEP
# Android's init cannot even start without sockets: it opens the property
# service as a PF_UNIX socket ("Failed to open socket 'property_service':
# Function not implemented" = ENOSYS when CONFIG_UNIX is off) and listens for
# kernel uevents on a NETLINK socket.  linux-3ds' defconfig has CONFIG_NET off
# (the 3DS has no network hardware), which makes the socket syscalls return
# ENOSYS, so turn the protocol stacks on.  They are pure software; no driver is
# needed for init to get its sockets, and the framework needs INET anyway.
scripts/config --enable  NET                  \
	       --enable  UNIX                 \
	       --enable  INET                 \
	       --enable  PACKET
# KUSER_HELPERS is mandatory: Android's bionic implements its atomics by
# calling the kernel's kuser cmpxchg helper at 0xffff0fc0.
#
# Note: do NOT enable TLS_REG_EMUL.  It looks tempting ("TLS emulation") but it
# means "emulate the TLS *register* via the undefined-instruction trap" and it
# skips maintaining the kuser software TLS slot that bionic actually reads.  The
# port instead clears HWCAP_TLS in arch/arm/mach-ctr/main_ctr.c: that makes
# switch_tls_v6 maintain the slot at 0xffff0ff0 (see the patch in
# apply-kernel-port.sh).
# ANDROID_BINDERFS must be OFF: mainline binder only creates the /dev/binder
# misc device when binderfs is disabled
# ("if (!IS_ENABLED(CONFIG_ANDROID_BINDERFS) && ...)" in drivers/android/binder.c).
# With binderfs enabled the only way in is mounting binderfs, but Android
# (API 10) opens "/dev/binder" literally - which is why the smoke test found no binder
# device at all.
make olddefconfig >/dev/null

echo "==> building kernel + device trees"
make -j"$(nproc)" zImage dtbs

mkdir -p "$OUT"
cp arch/arm/boot/zImage "$OUT/zImage"
cp arch/arm/boot/dts/nintendo3ds_ktr.dtb "$OUT/" 2>/dev/null || true
cp ctr_lcd_fb_stamp 2>/dev/null || true

echo
echo "==> artifacts in $OUT"
ls -l "$OUT"
echo
echo "check the Android pieces made it in:"
"${CROSS_COMPILE}objdump" -h arch/arm/boot/zImage >/dev/null 2>&1 || true
grep -c "CONFIG_ANDROID_ASHMEM=y" .config | sed 's/^/  CONFIG_ANDROID_ASHMEM=y : /'
grep -c "CONFIG_ANDROID_BINDER_IPC=y" .config | sed 's/^/  CONFIG_ANDROID_BINDER_IPC=y : /'
grep -c "CONFIG_CTR_LCD_FB=y" .config | sed 's/^/  CONFIG_CTR_LCD_FB=y : /'
grep -c "CONFIG_CTR_EXTRAPAD=y" .config | sed 's/^/  CONFIG_CTR_EXTRAPAD=y : /'
grep -c "CONFIG_CTR_SND=y" .config | sed 's/^/  CONFIG_CTR_SND=y : /'
grep -c "CONFIG_SND_PCM=y" .config | sed 's/^/  CONFIG_SND_PCM=y : /'
grep -c "CONFIG_CFG80211=y" .config | sed 's/^/  CONFIG_CFG80211=y : /'
grep -c "CONFIG_CFG80211_WEXT=y" .config | sed 's/^/  CONFIG_CFG80211_WEXT=y : /'
grep -c "CONFIG_ATH6K_LEGACY=y" .config | sed 's/^/  CONFIG_ATH6K_LEGACY=y : /'
grep -c "CONFIG_CTR_SDHC=y" .config | sed 's/^/  CONFIG_CTR_SDHC=y : /'
