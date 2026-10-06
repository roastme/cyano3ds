#!/bin/bash
# apply-kernel-port.sh - apply the Cyano3DS (CM7.2-on-Nintendo-3DS) kernel port
#
# Usage: apply-kernel-port.sh [kernel-tree]
#        (default kernel tree: /root/p3ds/src/linux-3ds)
#
# Everything this script touches lives in this repository under port/ and
# src/android-staging/ so that the port stays reviewable and re-appliable on a
# pristine linux-3ds checkout.
#
# What it does
#   1. vendors the Android ashmem driver into drivers/staging/android/ and
#      adapts it to the modern (6.x) shrinker API
#   2. installs the 3DS bottom-screen LCD driver (ctr_lcd_fb.c) and wires it
#      into drivers/platform/nintendo3ds/{Kconfig,Makefile}
#   3. replaces the bottom-screen display setup in the device tree and moves the
#      kernel console to the top screen
#
# It is idempotent: running it twice changes nothing.

set -euo pipefail

KD="${1:-/root/p3ds/src/linux-3ds}"
PORT="$(cd "$(dirname "$0")/.." && pwd)"   # <repo>/port
SRC="$(cd "$PORT/../src" && pwd)"           # <repo>/src

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ -f "$KD/Makefile" ] || die "no kernel tree at $KD"
[ -d "$PORT/kernel" ] || die "unexpected layout, $PORT/kernel missing"

# ---------------------------------------------------------------------------
say "1/3  Android legacy drivers (ashmem)"
# ---------------------------------------------------------------------------
mkdir -p "$KD/drivers/staging/android"

install -m 644 "$SRC/android-staging/ashmem-v5.4.c" "$KD/drivers/staging/android/ashmem.c"
install -m 644 "$SRC/android-staging/ashmem.h"     "$KD/drivers/staging/android/ashmem.h"
install -m 644 "$SRC/android-staging/uapi-ashmem.h" "$KD/include/uapi/linux/ashmem.h"
install -m 644 "$PORT/kernel/drivers/staging/android/logger.c" "$KD/drivers/staging/android/logger.c"
install -m 644 "$PORT/kernel/drivers/staging/android/Kconfig"  "$KD/drivers/staging/android/Kconfig"
install -m 644 "$PORT/kernel/drivers/staging/android/Makefile" "$KD/drivers/staging/android/Makefile"

# Wire drivers/staging/android into the build (inside the "if STAGING" block).
python3 - "$KD" <<'PY'
import re, sys, pathlib
kd = pathlib.Path(sys.argv[1])

kc = kd / "drivers/staging/Kconfig"
s = kc.read_text()
if "staging/android/Kconfig" not in s:
    s = s.replace('if STAGING\n',
                  'if STAGING\n\nsource "drivers/staging/android/Kconfig"\n', 1)
    kc.write_text(s)
    print("    drivers/staging/Kconfig: added android/Kconfig")

mk = kd / "drivers/staging/Makefile"
s = mk.read_text()
if "android/" not in s:
    s += "\nobj-$(CONFIG_ANDROID_LEGACY_DRIVERS)\t+= android/\n"
    mk.write_text(s)
    print("    drivers/staging/Makefile: added android/")

# ashmem.c was written for 5.x.  The only interfaces that changed for us are
# the shrinker registration API (6.7+) and the fact that shrinker_alloc() needs
# <linux/shrinker.h>.  BSN is the two characters backslash-n, built without
# escape sequences on purpose.
p = kd / "drivers/staging/android/ashmem.c"
BSN = chr(92) + "n"
s = p.read_text()
mod_sh = (kd / "include/linux/shrinker.h").read_text()
if "shrinker_alloc" in mod_sh and "shrinker_alloc" not in s:
    s = re.sub(r"static struct shrinker ashmem_shrinker = \{.*?\};",
               "static struct shrinker *ashmem_shrinker;", s, flags=re.S)
    old = ("	ret = register_shrinker(&ashmem_shrinker);" + chr(10) +
           "	if (ret) {" + chr(10) +
           "		pr_err(\"failed to register shrinker!" + BSN + "\");" + chr(10) +
           "		goto out_demisc;" + chr(10) +
           "	}" + chr(10))
    new = ("	ashmem_shrinker = shrinker_alloc(0, \"ashmem\");" + chr(10) +
           "	if (!ashmem_shrinker) {" + chr(10) +
           "		pr_err(\"failed to allocate shrinker!" + BSN + "\");" + chr(10) +
           "		goto out_demisc;" + chr(10) +
           "	}" + chr(10) +
           "	ashmem_shrinker->scan_objects = ashmem_shrink_scan;" + chr(10) +
           "	ashmem_shrinker->count_objects = ashmem_shrink_count;" + chr(10) +
           "	ashmem_shrinker->seeks = DEFAULT_SEEKS * 4;" + chr(10) +
           "	shrinker_register(ashmem_shrinker);" + chr(10))
    assert old in s, "ashmem.c: shrinker registration block not found"
    s = s.replace(old, new)
    s = s.replace("&ashmem_shrinker", "ashmem_shrinker")
    p.write_text(s)
    print("    ashmem.c: adapted to the modern (6.7+) shrinker API")
else:
    print("    ashmem.c: kernel uses the classic shrinker API, unmodified")

if "#include <linux/shrinker.h>" not in s:
    s = s.replace("#include <linux/mutex.h>",
                  "#include <linux/mutex.h>" + chr(10) +
                  "#include <linux/shrinker.h>", 1)
    p.write_text(s)
PY

# ---------------------------------------------------------------------------
say "2/3  3DS bottom-screen LCD framebuffer driver"
# ---------------------------------------------------------------------------
install -m 644 "$PORT/kernel/drivers/platform/nintendo3ds/ctr_lcd_fb.c" \
	"$KD/drivers/platform/nintendo3ds/ctr_lcd_fb.c"

python3 - "$KD" <<'PY'
import sys, pathlib
kd = pathlib.Path(sys.argv[1])

kc = kd / "drivers/platform/nintendo3ds/Kconfig"
s = kc.read_text()
if "CTR_LCD_FB" not in s:
    entry = '''
config CTR_LCD_FB
	tristate "Nintendo 3DS bottom-screen LCD framebuffer"
	depends on FB
	select FB_CFB_FILLRECT
	select FB_CFB_COPYAREA
	select FB_CFB_IMAGEBLIT
	default y
	help
	  Framebuffer driver for the Nintendo 3DS bottom screen, used as the
	  Android display.  The 3DS LCD panel is wired portrait, so this driver
	  advertises a normal landscape 320x240 RGB565 framebuffer with two
	  buffers and performs the 90 degree transposition into VRAM on flip.

'''
    # must go inside "if NINTENDO3DS_PLATFORM_DEVICES"
    i = s.rindex("\nendif")
    s = s[:i] + entry + s[i:]
    kc.write_text(s)
    print("    drivers/platform/nintendo3ds/Kconfig: added CTR_LCD_FB")

mk = kd / "drivers/platform/nintendo3ds/Makefile"
s = mk.read_text()
if "CTR_LCD_FB" not in s:
    s += "\nobj-$(CONFIG_CTR_LCD_FB)\t+= ctr_lcd_fb.o\n"
    mk.write_text(s)
    print("    drivers/platform/nintendo3ds/Makefile: added ctr_lcd_fb.o")
PY

# ---------------------------------------------------------------------------
say "3/3  device tree"
# ---------------------------------------------------------------------------
python3 - "$KD" <<'PY'
import sys, pathlib, re
kd = pathlib.Path(sys.argv[1])

dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
s = dtsi.read_text()
node = '''	/*
	 * Bottom screen LCD.  Ordered *after* the top-screen framebuffer node
	 * below on purpose: the first framebuffer to register becomes fb0, and
	 * fbcon binds it as the kernel console.  The console must be the top
	 * screen (the same one firm_linux_loader prints on), so this device
	 * becomes fb1 and Android's /dev/graphics/fb0 is pointed at it from
	 * ueventd/init.  Keeping the console independent of this driver also
	 * means kernel messages stay visible even if this driver fails.
	 *
	 * "vram" is a private 512 KiB window of the 6 MiB VRAM region
	 * (0x18000000-0x18600000) holding the driver's two portrait scanout
	 * buffers (240x320; 230400 bytes each with the bootloader's 24bpp BGR).
	 * The firmware's own buffers end at 0x18192000, so 0x18500000 is out of
	 * the way.
	 */
	lcd: lcd@10400500 {
		compatible = "nintendo,3ds-lcd";
		reg = <0x10400500 0x100>,
		      <0x18500000 0x80000>;
		reg-names = "pdc", "vram";
	};

'''
# remove any older version of our node (the port is applied repeatedly)
node_marker = "\tlcd: lcd@10400500 "
while node_marker in s:
    i = s.index(node_marker)
    c = s.rindex("\t/*", 0, i)          # the comment block above it
    j = s.index("\n\t};\n", i) + len("\n\t};\n")
    while c > 0 and s[c - 1] == '\n':
        c -= 1
    s = s[:c] + s[j:]
assert "\n\tsoc {\n" in s, "nintendo3ds.dtsi: soc anchor not found"
s = s.replace("\n\tsoc {\n", "\n" + node + "\tsoc {\n", 1)
dtsi.write_text(s)
print("    nintendo3ds.dtsi: bottom-screen LCD node in place (after the top-screen fb)")

# Console: keep exactly what linux-3ds uses - top screen, rotate:1 because that
# panel is scanned portrait as well.  No fbcon=map is needed now that the
# console framebuffer is the first one registered; fbcon=map:1 with a missing
# fb1 would leave the kernel with no console at all.
for name in ("nintendo3ds_ctr.dts", "nintendo3ds_ktr.dts"):
    p = kd / "arch/arm/boot/dts" / name
    s = p.read_text()
    # fpcon=rotate:1 because the top panel is scanned portrait as well.
    # keep_bootcon is deliberately NOT used: it leaves a second, stale boot
    # console registered next to the real one, which is a known source of
    # confusing/duplicated console output ("tty %d not allocated" style
    # warnings) during bring-up.
    #
    # maxcpus=1: Android's bionic (API 10) was built for the single-core ARMv5
    # "goldfish" target and reads its thread pointer from the kuser software
    # TLS slot at 0xffff0ff0.  That is ONE fixed physical address shared by
    # every CPU, so on this 4-core ARM11 any other core's context switch (to
    # idle or a kernel thread, whose TLS value is 0) overwrites it while a
    # user thread is running, and the next TLS access takes a data abort
    # (seen on hardware as init dying at pc 0x13028 with r0=8).  Running a
    # single CPU makes the slot unambiguous again; the 3DS's extra cores can
    # come back once the userspace is rebuilt to use the hardware TLS
    # register (TPIDRURO) instead of the legacy slot.
    # The audio bring-up is done; ctr_lcd_fb.debug=1 (the top-screen liveness
    # marker + the softirq stall watchdog that dumps stacks via the slow ARM9
    # capture path) is removed again.  It was firing on busy native games and
    # could stall the UI.
    want = "maxcpus=1 fbcon=rotate:1 fbcon=font:VGA8x8 consoleblank=0"
    known = [
        "maxcpus=1 fbcon=rotate:1 fbcon=font:VGA8x8 consoleblank=0 ctr_lcd_fb.debug=1",
        "maxcpus=1 fbcon=rotate:1 fbcon=font:VGA8x8",
        "consoleblank=0 maxcpus=1 fbcon=rotate:1 fbcon=font:VGA8x8",
        "fbcon=rotate:1 fbcon=font:VGA8x8",
        "keep_bootcon fbcon=rotate:1 fbcon=font:VGA8x8",
        "fbcon=map:1,rotate:1,font:VGA8x8",
        "fbcon=rotate:1,font:VGA8x8",
    ]
    if f'bootargs = "{want}"' in s:
        print(f"    {name}: bootargs already '{want}'")
    else:
        for k in known:
            if f'bootargs = "{k}"' in s:
                p.write_text(s.replace(f'bootargs = "{k}"', f'bootargs = "{want}"'))
                print(f"    {name}: bootargs -> '{want}'")
                break
        else:
            print(f"    {name}: WARNING bootargs not recognised, left alone")
PY

# ---------------------------------------------------------------------------
say "3b/9  kernel: New 3DS ARM11 CPU clock + cpufreq driver"
# ---------------------------------------------------------------------------
# The New 3DS can run its ARM11 at 804 MHz (LGR2) / 536 MHz (LGR1), but
# firm_linux_loader leaves it at 268 MHz because the TWD timer clock scales
# with the CPU clock (PERIPHCLK = CPU/2) and the kernel has to be told.  This
# installs a common-clock provider for PDN_LGR_SOCMODE plus a cpufreq driver,
# and re-parents the TWD timer onto a CPU/2 child so its clock notifier keeps
# the clockevent correct.  The old 3DS is untouched (the provider bails out
# when CFG11_SOCINFO is not LGR1/LGR2).
python3 "$PORT/scripts/add-ctr-cpufreq.py" "$KD"

# ---------------------------------------------------------------------------
say "4/7  kernel: CM7 TLS (kuser slot + writable vectors page)"
# ---------------------------------------------------------------------------
# Android's bionic reads its thread pointer from the kuser helper page
# slot at 0xffff0ff0, but on CONFIG_CPU_32v6K the kernel (asm/tls.h:
# switch_tls_v6k) only ever touches the hardware TLS register.  The syscall
# tracer showed init dying one instruction after __ARM_NR_set_tls with a
# store through address 0x8 because of exactly that.
#
# Two things are needed:
#   1. keep 0xffff0ff0 in sync with the thread pointer (fix-tls-slot.py), and
#   2. make the high-vectors page kernel-writable at all (fix-vectors-rw.py):
#      the 3DS ARM11 is misdetected as ARMv7, so mmu.c otherwise maps that
#      page privileged-read-only and step 1's store faults on the first
#      context switch.
python3 "$PORT/scripts/fix-tls-slot.py" "$KD"
python3 "$PORT/scripts/fix-vectors-rw.py" "$KD"

# The CPU ID makes the generic detector conclude ARMv7 from MMFR0; the ARM11
# is ARMv6K.  Force the correct answer (fault hooks, alignment decoding,
# /proc/cpuinfo and the HWCAP userspace sees).
python3 "$PORT/scripts/fix-cpu-arch.py" "$KD"

# CONFIG_CPU_SW_DOMAIN_PAN makes user addresses inaccessible in kernel mode,
# so the cacheflush syscall's cache-maintenance instructions on a user range
# domain-fault (oops) or silently fail to flush.  That breaks libpixelflinger's
# JIT self-modifying code - SurfaceFlinger hangs.  Upstream "ARM: fix
# cacheflush with PAN" enables user access around the flush.
python3 "$PORT/scripts/fix-cacheflush-pan.py" "$KD"

# 16 KiB kernel stacks: a silent hard freeze with no detector firing is what a
# kernel stack overflow in the interrupt path looks like.
python3 "$PORT/scripts/fix-thread-size.py" "$KD"

# ---------------------------------------------------------------------------
say "5/7  kernel: 32-bit binder ABI (protocol 7) for the 32-bit Android userspace"
# ---------------------------------------------------------------------------
python3 "$PORT/scripts/fix-binder-32bit.py" "$KD"

# cgroup v1 permission: CM7's ActivityManagerService calls setProcessGroup()
# for every process, which writes the target tid into /dev/cpuctl/*/tasks.
# The writer is system_server (uid 1000, CAP_SYS_NICE) and the target belongs
# to an app uid, so upstream's uid-only check rejects it.  Let a CAP_SYS_NICE
# holder migrate tasks, as every Android common kernel does.
python3 "$PORT/scripts/fix-cgroup-sched-perm.py" "$KD"

# ---------------------------------------------------------------------------
say "6/8  kernel: bring-up debug instrumentation"
# ---------------------------------------------------------------------------
# The ashmem / open()-failure / may_open() instrumentation was essential while
# tracking down the "/ is 0700" and device-permission bugs, but it logs on
# every ashmem open and floods the (very slow) fbcon console.  Off for now;
# the scripts are idempotent, so re-enable by uncommenting.
# python3 "$PORT/scripts/add-ashmem-debug.py" "$KD"
# python3 "$PORT/scripts/add-open-debug.py" "$KD"
# python3 "$PORT/scripts/add-mayopen-debug.py" "$KD"

# ---------------------------------------------------------------------------
say "7/8  kernel: MCU power-off (short power-button press)"
# ---------------------------------------------------------------------------
python3 "$PORT/scripts/add-mcu-poweroff.py" "$KD"

# ---------------------------------------------------------------------------
say "8/9  kernel: pre-console ARM11 heartbeat (top-screen green band)"
# ---------------------------------------------------------------------------
python3 "$PORT/scripts/add-heartbeat.py" "$KD"

# ---------------------------------------------------------------------------
say "9/9  kernel: PXI virtio kick must not sleep in atomic context (defer it)"
# ---------------------------------------------------------------------------
# The block layer calls ->queue_rq under rcu_read_lock() and that reaches
# vpxi_notify -> pxi_txrx, which sleeps.  Defer the kick to a workqueue in
# atomic context instead (busy-polling with IRQs off starves the whole system
# under heavy SD I/O, which looks exactly like a frozen kernel).
python3 "$PORT/scripts/fix-pxi-notify-defer.py" "$KD"

# ... and in fact the in_atomic() probe used there is unreliable: on real
# hardware the block layer's own workqueue still reached the sleeping path and
# the kernel logged ~150 "sleeping function called from invalid context"
# traces.  Always defer instead.
python3 "$PORT/scripts/fix-pxi-notify-always.py" "$KD"

# A PXI transaction that times out leaves a late ARM9 response in the FIFO,
# which the next transaction would read as its own first word (protocol
# desync).  Flush both FIFOs on timeout; the ARM9 side is bounded too now.
python3 "$PORT/scripts/fix-pxi-flush-on-timeout.py" "$KD"

# ARM9 black-box capture (option B): export a manager-register write so the
# ctr-diag kthread can hand the ARM9 the shared capture page's physical
# address once; after that the ARM9 polls it with no PXI at all.
python3 "$PORT/scripts/fix-pxi-capture.py" "$KD"

# The virtio-blk completion loop runs with IRQs disabled; a runaway used ring
# there is a silent hard freeze with no detector firing.  Bound it.
python3 "$PORT/scripts/fix-virtblk-loop-guard.py" "$KD"

# ---------------------------------------------------------------------------
# 10/10 3DS touchscreen -- the Octoblimp/Android3DS driver, verbatim
#
# The codec chip's clock (CFG11_SPEAKER_CNT, 10141220h, bit1) is required for
# the touchscreen and is never set on this boot chain (fastboot3DS skips its
# CODEC_init, firm_linux_loader and the linux-3ds TSC driver never touch
# CFG11).  Without it the TSC ADC never runs and the FIFO reads 0xFFFF.
python3 "$PORT/scripts/fix-tsc-speaker-clock.py" "$KD"

# The whole touchscreen driver is now Octoblimp's, vendored verbatim in
# port/kernel/.  This port used to patch the inherited linux-3ds touch.c into a
# dedicated evdev touchscreen (fix-tsc-touchscreen.py) and then layer an
# eight-way mapping (fix-tsc-touch-map.py), an affine calibration
# (fix-tsc-touch-cal.py) and a FIFO median + touch-slop filter
# (fix-tsc-touch-filter.py) on top.  It still felt wrong at the edges, while
# Octoblimp's driver on the identical panel feels right.  That driver medians
# the FIFO's five samples, anchors a contact to its DOWN position, reports the
# newest sample at 60 Hz, registers the panel as INPUT_PROP_DIRECT, and moves
# the circle pad to its own trackball input device.  Keep the vendored file
# byte-identical to Octoblimp's drivers/platform/nintendo3ds/tsc/touch.c.
mkdir -p "$KD/drivers/platform/nintendo3ds/tsc"
install -m 644 "$PORT/kernel/drivers/platform/nintendo3ds/tsc/touch.c" \
	"$KD/drivers/platform/nintendo3ds/tsc/touch.c"

# The Octoblimp driver binds to its own compatible string and reports final
# 320x240 coordinates, so point the DT node at it.  (The old node advertised
# touchscreen-size 4096 plus swap/invert properties, none of which either
# driver ever parsed.)
python3 - "$KD" <<'PY'
import pathlib, sys
kd = pathlib.Path(sys.argv[1])
dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
s = dtsi.read_text()
old = ('compatible = "nintendo,3dstsc-touch";\n'
       '\t\t\t\t\ttouchscreen-size-x = <4096>;\n'
       '\t\t\t\t\ttouchscreen-size-y = <4096>;\n'
       '\t\t\t\t\ttouchscreen-inverted-y;\n'
       '\t\t\t\t\ttouchscreen-swapped-x-y;')
new = ('compatible = "nintendo,android3ds-touchscreen";\n'
       '\t\t\t\t\t/* Driver reports final Android 320x240 coordinates. */\n'
       '\t\t\t\t\ttouchscreen-size-x = <320>;\n'
       '\t\t\t\t\ttouchscreen-size-y = <240>;')
if "nintendo,android3ds-touchscreen" in s:
    print("    dtsi: touch node already binds android3ds-touchscreen")
elif old in s:
    dtsi.write_text(s.replace(old, new, 1))
    print("    dtsi: touch node -> nintendo,android3ds-touchscreen (320x240)")
else:
    sys.exit("dtsi: touch node properties not recognised")
PY

# Map the 3DS face buttons to Android navigation keys (the D-pad already uses
# KEY_UP/DOWN/LEFT/RIGHT which qwerty.kl maps; the rest were BTN_* gamepad
# codes that Android dropped).
python3 "$PORT/scripts/fix-buttons.py" "$KD"

# The physical Home button shares one gpio-keys input device with the power
# button, and powerkey EVIOCGRABs that device (so it reliably sees a short
# power press).  That hides the Home button from Android entirely.  Split the
# node so home + wireless get their own evdev.
python3 "$PORT/scripts/fix-mcu-home-button.py" "$KD"

# The legacy status bar TrackingView is touch-modal over the top ~19 px, so a
# touch there expands the shade and the shade then swallows input.  This port
# used to drop touches that start in that strip (fix-tsc-statusbar-drop.py,
# now removed); that is DISABLED now because
# touch works and the status bar should be usable (the DISABLE_EXPAND helper
# and the CLOSE_SYSTEM_DIALOGS broadcast are gone too).

# New 3DS ZL/ZR buttons (not in HID_PAD; on I2C 2:0x54)
install -m 644 "$PORT/kernel/drivers/platform/nintendo3ds/ctr_extrapad.c" \
	"$KD/drivers/platform/nintendo3ds/ctr_extrapad.c"
python3 - "$KD" <<'PY'
import re, sys, pathlib
kd = pathlib.Path(sys.argv[1])

kc = kd / "drivers/platform/nintendo3ds/Kconfig"
s = kc.read_text()
if "CTR_EXTRAPAD" not in s:
    entry = '''
config CTR_EXTRAPAD
	tristate "Nintendo 3DS New-3DS ZL/ZR buttons (I2C)"
	depends on CTR_I2C
	depends on INPUT
	select INPUT_EVDEV
	default y
	help
	  On the New 3DS, ZL/ZR are not in the HID_PAD register (bits 14/15
	  there are the IRQ-enable/condition bits); the chip that reports them
	  is on I2C bus 2 = 0x10148000 (GBATEK "2:54h", the IR/gyro bus),
	  7-bit address 0x2A.  Read it and report ZL as KEY_MENU and ZR as
	  KEY_SEARCH input events.

'''
    i = s.rindex("\nendif")
    s = s[:i] + entry + s[i:]
    kc.write_text(s)
    print("    drivers/platform/nintendo3ds/Kconfig: added CTR_EXTRAPAD")
else:
    # fix the help text of a tree written before the bus was corrected
    s2 = s.replace("at 2:0x54, the same bus as the MCU.  Read them and report",
                   "at bus 2 = 0x10148000 (the IR/gyro bus), 7-bit address 0x2A.")
    s2 = s2.replace("Read it and report KEY_BACK / KEY_SEARCH input",
                    "Read it and report ZL as KEY_MENU and ZR as KEY_SEARCH input")
    if s2 != s:
        kc.write_text(s2)
        print("    drivers/platform/nintendo3ds/Kconfig: updated CTR_EXTRAPAD text")

mk = kd / "drivers/platform/nintendo3ds/Makefile"
s = mk.read_text()
if "CTR_EXTRAPAD" not in s:
    s += "obj-$(CONFIG_CTR_EXTRAPAD)\t+= ctr_extrapad.o\n"
    mk.write_text(s)
    print("    drivers/platform/nintendo3ds/Makefile: added ctr_extrapad.o")

dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
s = dtsi.read_text()
extrapad_bus = re.compile(r'&i2c\d+ \{\n\textrapad@2a \{')
if "extrapad@2a" in s:
    # The chip is on I2C bus 2 = 0x10148000 (the IR + gyro bus), NOT
    # 0x10144000 (the MCU bus).  An earlier version of this script attached
    # it to &i2c2; on that bus address 0x2A never ACKs and every read returns
    # FF FF (ctr_i2c does not surface a NACK), so ZL/ZR never worked.
    # Migrate such a tree in place.
    s2 = extrapad_bus.sub('&i2c3 {\n\textrapad@2a {', s)
    s2 = s2.replace(
        "/* New 3DS ZL/ZR (and C-stick): same I2C bus as the MCU.",
        "/* New 3DS ZL/ZR (and C-stick): I2C bus 2 = 0x10148000 (IR/gyro).")
    if s2 != s:
        dtsi.write_text(s2)
        print("    dtsi: extrapad moved to &i2c3 (0x10148000, the IR/gyro bus)")
    else:
        print("    dtsi: extrapad already on &i2c3 (0x10148000)")
else:
    s += """
/* New 3DS ZL/ZR (and C-stick): I2C bus 2 = 0x10148000 (IR/gyro).
 * ZL/ZR are not in HID_PAD; ctr_extrapad reads the I2C device and reports
 * ZL as KEY_MENU and ZR as KEY_SEARCH.
 */
&i2c3 {
	extrapad@2a {
		compatible = "nintendo,3ds-extrapad";
		reg = <0x2a>;
	};
};
"""
    dtsi.write_text(s)
    print("    dtsi: &i2c3 extrapad@2a (ZL/ZR) added")
PY

# The MCU charger already exposes a real BAT0/ADP0 through the kernel
# power_supply class.  Add the HEALTH/TECHNOLOGY properties Android's
# BatteryService looks for, and poll the MCU + power_supply_changed() so the
# charging state reaches Android when the charger is plugged in (the driver
# had no uevent source, so the indicator only ever showed the boot value).
python3 "$PORT/scripts/fix-mcu-battery.py" "$KD"

# ---------------------------------------------------------------------------
# 11/11 3DS audio: TSC2117/AIC3010 codec + CSND sound hardware
#
# The 3DS drives its speakers through a TSC2117/AIC3010 codec fed by two I2S
# lines.  I2S1 belongs to the undocumented XpertTeak DSP (needs Nintendo's
# signed dsp1 firmware); I2S2 belongs to the CSND block - the DSi "sound"
# engine with 32 DMA channels that read PCM straight out of main memory.  This
# uses CSND/I2S2, so no DSP firmware is required.
#
# The codec bring-up has to happen in ctr_tsc_probe() before its touchscreen
# child is populated (fix-tsc-snd.py); the CSND half is an ALSA card registered
# from a child "nintendo,3ds-snd" node.
install -m 644 "$PORT/kernel/drivers/platform/nintendo3ds/ctr_snd.c" \
	"$KD/drivers/platform/nintendo3ds/ctr_snd.c"
python3 "$PORT/scripts/fix-tsc-snd.py" "$KD"

python3 - "$KD" <<'PY'
import sys, pathlib, re
kd = pathlib.Path(sys.argv[1])

kc = kd / "drivers/platform/nintendo3ds/Kconfig"
s = kc.read_text()
if "CTR_SND" not in s:
    entry = '''
config CTR_SND
	tristate "Nintendo 3DS audio (TSC2117/AIC3010 codec + CSND)"
	depends on CTR_TSC
	depends on SND
	select SND_PCM
	default y
	help
	  Sound on the Nintendo 3DS.  The codec is a TSC2117/AIC3010 driven
	  over the TSC SPI controller; the samples come from the CSND block
	  (the DSi-style 32-channel DMA sound engine) on the I2S2 line, so no
	  DSP firmware is needed.  Exposes an ALSA PCM playback device and
	  plays a short test tone at boot.  CTR_SND also hooks the codec
	  bring-up into the TSC driver via ctr_snd_codec_init().

'''
    i = s.rindex("\nendif")
    s = s[:i] + entry + s[i:]
    kc.write_text(s)
    print("    drivers/platform/nintendo3ds/Kconfig: added CTR_SND")
else:
    print("    drivers/platform/nintendo3ds/Kconfig: CTR_SND already present")

mk = kd / "drivers/platform/nintendo3ds/Makefile"
s = mk.read_text()
if "CTR_SND" not in s:
    s += "obj-$(CONFIG_CTR_SND)\t\t+= ctr_snd.o\n"
    mk.write_text(s)
    print("    drivers/platform/nintendo3ds/Makefile: added ctr_snd.o")
else:
    print("    drivers/platform/nintendo3ds/Makefile: CTR_SND already present")

# ALSA sound node as a child of the TSC SPI device, so it can reuse the
# parent's codec regmap.  ctr_snd_codec_init() runs from the TSC probe, so the
# codec is already up by the time this child probes.
dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
s = dtsi.read_text()
if "nintendo,3ds-snd" not in s:
    touch_node = re.compile(
        r'(\t+)(touch: touchscreen \{.*?\n\1\};\n)', re.S)
    m = touch_node.search(s)
    if not m:
        sys.exit("dtsi: touchscreen node not found, cannot add sound node")
    node = (m.group(1) +
            "\n" + m.group(1) + "/* Audio: TSC2117/AIC3010 codec + CSND (I2S2). */\n" +
            m.group(1) + "snd {\n" +
            m.group(1) + "\tcompatible = \"nintendo,3ds-snd\";\n" +
            m.group(1) + "};\n")
    s = s[:m.end()] + node + s[m.end():]
    dtsi.write_text(s)
    print("    dtsi: tsc@0/snd (nintendo,3ds-snd) added")
else:
    print("    dtsi: sound node already present")
PY

# ---------------------------------------------------------------------------
say "10/10  kernel: Nintendo 3DS WiFi (Octoblimp staging ath6k + AR6014/NWM)"
# ---------------------------------------------------------------------------
# The 3DS WiFi is an Atheros AR6014G on a *second* SD host controller
# (0x10122000, the "nwm" node).  The Cyano3DS port used to drive it with mainline
# cfg80211 ath6kl, patched piece by piece for the AR6002 "hw2" family and
# Nintendo's NWM firmware; that never reached a working station.  This port now
# installs the Octoblimp/Android3DS stack instead: the pre-mainline staging
# ath6k driver, ported to cfg80211 + WEXT and taught to boot the AR6014 from
# Nintendo's four NWM blobs, together with Octoblimp's ctr_sdhc.c SDIO host
# changes.  The driver source and the AR6014/NWM patch series are vendored in
# port/kernel/ (see fix-wifi-octoblimp.py).  See docs/WIFI.md.
python3 "$PORT/scripts/fix-wifi-octoblimp.py" "$KD"

say "done - now run build-kernel.sh"
