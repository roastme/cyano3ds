#!/usr/bin/env python3
"""Sleep mode: close the lid to sleep, open it to wake.

Follows open_agb_firm's lid sleep (Dominik Leicht's libn3ds "deep sleep"
support, profi200/open_agb_firm PR 295 + profi200/libn3ds PR 9): secondary
cores park DORMANT in the SCU, the graphics hardware is quiesced, and the boot
core enters PDN sleep with "shell opened" and "MCU interrupt" as wake sources.

Wires up (files installed by apply-kernel-port.sh):
  * arch/arm/mach-ctr/pm.c: CPU hotplug (cpu_die/cpu_kill: park in WFI) and
    platform_suspend_ops for "mem" (PDN sleep, display state and VRAM kept);
    platsmp.c hands it the SCU base, uses the hotplug hooks and releases a
    parked core instead of going through the boot ROM again;
  * drivers/platform/nintendo3ds/ctr_lid.c (CONFIG_CTR_LID): MCU shell
    interrupts -> pm_suspend(), plus the MCU half of the sleep sequence
    (LCD power, power LED, "entering sleep");
  * nintendo3ds.dtsi: the mcu-lid@f node under the MCU.

Idempotent.  Applies to the linux-3ds tree.
"""
import os
import pathlib
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
mach = kd / "arch/arm/mach-ctr"
plat = kd / "drivers/platform/nintendo3ds"


def edit(path, mark, fn, what):
    s = path.read_text()
    if mark in s:
        print(f"    {what}: already done")
        return
    s2 = fn(s)
    if s2 == s or mark not in s2:
        raise SystemExit(f"error: {path}: could not apply ({what})")
    path.write_text(s2)
    print(f"    {what}: done")


def need(s, old, path):
    if old not in s:
        raise SystemExit(f"error: {path}: anchor not found: {old!r}")


# --------------------------------------------------------------------------
# 1. mach-ctr: pm.o, platsmp.h prototypes, platsmp.c hooks
# --------------------------------------------------------------------------
mk = mach / "Makefile"
def mk_fn(s):
    old = "obj-$(CONFIG_ARCH_CTR) += bottom_lcd.o\n"
    need(s, old, mk)
    return s.replace(old, old + "obj-$(CONFIG_ARCH_CTR) += pm.o\n", 1)
edit(mk, "pm.o", mk_fn, "mach-ctr/Makefile: pm.o")

hdr = mach / "include/mach/platsmp.h"
def hdr_fn(s):
    old = "extern void ctr_secondary_startup(void);\n"
    need(s, old, hdr)
    return s.replace(old, old + (
        "\n"
        "/* pm.c: sleep mode and CPU hotplug */\n"
        "void ctr_pm_set_scu_base(void __iomem *scu_base);\n"
        "bool ctr_cpu_unpark(unsigned int cpu);\n"
        "void ctr_cpu_die(unsigned int cpu);\n"
        "int ctr_cpu_kill(unsigned int cpu);\n"), 1)
edit(hdr, "ctr_cpu_unpark", hdr_fn, "mach/platsmp.h: pm.c prototypes")

smp = mach / "platsmp.c"
def smp_fn(s):
    old = ("\tvoid __iomem *boot_addr;\n"
           "\tboot_addr = ioremap(")
    need(s, old, smp)
    s = s.replace(old, (
        "\tvoid __iomem *boot_addr;\n"
        "\n"
        "#ifdef CONFIG_HOTPLUG_CPU\n"
        "\t/* back online after CPU hotplug (e.g. sleep mode): it is parked in\n"
        "\t * ctr_cpu_die(), not in the boot ROM's wait loop */\n"
        "\tif (ctr_cpu_unpark(cpu))\n"
        "\t\treturn 0;\n"
        "#endif\n"
        "\n"
        "\tboot_addr = ioremap("), 1)

    old = "\t\tscu_enable(scu_base);\n"
    need(s, old, smp)
    s = s.replace(old, old + "\t\tctr_pm_set_scu_base(scu_base);\n", 1)

    old = "\t.smp_boot_secondary\t= ctr_smp_boot_secondary,\n"
    need(s, old, smp)
    s = s.replace(old, old + (
        "#ifdef CONFIG_HOTPLUG_CPU\n"
        "\t.cpu_die\t\t= ctr_cpu_die,\n"
        "\t.cpu_kill\t\t= ctr_cpu_kill,\n"
        "#endif\n"), 1)
    return s
edit(smp, "ctr_cpu_unpark", smp_fn, "mach-ctr/platsmp.c: hotplug hooks + SCU base")

# --------------------------------------------------------------------------
# 2. drivers/platform/nintendo3ds: CTR_LID
# --------------------------------------------------------------------------
kc = plat / "Kconfig"
def kc_fn(s):
    entry = '''
config CTR_LID
	bool "Nintendo 3DS lid (shell) sleep mode"
	depends on CTR_MCU
	depends on SUSPEND
	default y
	help
	  Puts the console to sleep (PDN sleep, arch/arm/mach-ctr/pm.c) when
	  the shell is closed and wakes it when it is opened, and does the MCU
	  part of the sleep sequence (LCD power, power LED) for every system
	  suspend.  sysfs: closed, sleep_on_close.
'''
    i = s.rfind("endif")
    if i < 0:
        i = len(s)
    return s[:i] + entry.lstrip("\n") + "\n" + s[i:]
edit(kc, "CTR_LID", kc_fn, "nintendo3ds/Kconfig: CTR_LID")

pm = plat / "Makefile"
def pm_fn(s):
    return s.rstrip("\n") + "\nobj-$(CONFIG_CTR_LID) += ctr_lid.o\n"
edit(pm, "ctr_lid.o", pm_fn, "nintendo3ds/Makefile: ctr_lid.o")

# --------------------------------------------------------------------------
# 3. device tree: mcu-lid@f
# --------------------------------------------------------------------------
dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
def dt_fn(s):
    old = ("\t\t\t\tmcu-charger@a {\n")
    need(s, old, dtsi)
    node = (
        "\t\t\t\t/* Shell (lid): MCU IRQ 5 closed, 6 opened; state in reg 0x0F bit 1.\n"
        "\t\t\t\t * Closing it puts the console to sleep (ctr_lid.c, mach-ctr/pm.c). */\n"
        "\t\t\t\tmcu-lid@f {\n"
        "\t\t\t\t\tcompatible = \"nintendo,3dsmcu-lid\";\n"
        "\t\t\t\t\treg = <0x0F>;\n"
        "\t\t\t\t\tinterrupt-parent = <&mcuintc>;\n"
        "\t\t\t\t\tinterrupts = <5 IRQ_TYPE_LEVEL_HIGH>, <6 IRQ_TYPE_LEVEL_HIGH>;\n"
        "\t\t\t\t\tinterrupt-names = \"close\", \"open\";\n"
        "\t\t\t\t\twakeup-source;\n"
        "\t\t\t\t};\n"
        "\n")
    return s.replace(old, node + old, 1)
edit(dtsi, "nintendo,3dsmcu-lid", dt_fn, "nintendo3ds.dtsi: mcu-lid@f")
