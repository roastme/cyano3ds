#!/usr/bin/env python3
"""Make a short power-button press switch the console off.

The 3DS MCU powers the console down when bit 0 of register 0x20 is set; the
device tree already models that as the "Power off" regulator
(poweroff-regulator@20, off = <0x01>), but nothing installs a power-off
handler, so reboot(RB_POWER_OFF) just halted the CPU (the user saw
"System halted") and the button still had to be held until the MCU cut power.

Two changes:
  * drivers/platform/nintendo3ds/mcu/regulator.c: a regulator node marked
    system-power-controller installs a reboot notifier (runs in process
    context with interrupts on, before device_shutdown(), so the I2C write
    works) plus a pm_power_off fallback; both write the node's "off" value -
    i.e. the MCU power-down bit.
  * nintendo3ds.dtsi: mark poweroff-regulator@20 system-power-controller.

The userspace side (powerkey, in the initramfs) calls reboot(RB_POWER_OFF) when
it sees KEY_POWER on the mcu_buttons input device.

Idempotent.  Applies to the linux-3ds tree.
"""
import pathlib
import os
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
reg = kd / "drivers/platform/nintendo3ds/mcu/regulator.c"
dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"

MARK = "ctr_poweroff_notify"

# --------------------------------------------------------------------------
# 1. regulator.c
# --------------------------------------------------------------------------
s = reg.read_text()
if MARK in s:
    print("    mcu/regulator.c: power-off handler already installed")
else:
    inc_old = "#include <linux/platform_device.h>\n"
    if inc_old not in s:
        raise SystemExit("error: regulator.c: platform_device.h include not found")
    s = s.replace(
        inc_old,
        inc_old + "#include <linux/pm.h>\n#include <linux/reboot.h>\n", 1)

    ops_old = "static struct regulator_ops ctr_regulator_ops = {\n"
    helpers = (
        "/*\n"
        " * Short-press power button: the MCU cuts power when the \"Power off\"\n"
        " * bit is written.  A system-power-controller regulator installs a\n"
        " * reboot notifier (process context, IRQs on, before device_shutdown)\n"
        " * and a pm_power_off fallback so reboot(RB_POWER_OFF) really switches\n"
        " * the console off.\n"
        " */\n"
        "static struct regmap *ctr_poweroff_map;\n"
        "static u32 ctr_poweroff_reg;\n"
        "static u32 ctr_poweroff_val;\n"
        "\n"
        "static void ctr_poweroff_cmd(void)\n"
        "{\n"
        "\tif (ctr_poweroff_map)\n"
        "\t\tregmap_write(ctr_poweroff_map, ctr_poweroff_reg,\n"
        "\t\t\t     ctr_poweroff_val);\n"
        "}\n"
        "\n"
        "static int ctr_poweroff_notify(struct notifier_block *nb,\n"
        "\t\t\t       unsigned long action, void *data)\n"
        "{\n"
        "\tif (action == SYS_POWER_OFF)\n"
        "\t\tctr_poweroff_cmd();\n"
        "\treturn NOTIFY_DONE;\n"
        "}\n"
        "\n"
        "static struct notifier_block ctr_poweroff_nb = {\n"
        "\t.notifier_call = ctr_poweroff_notify,\n"
        "};\n"
        "\n"
        "static void ctr_pm_power_off(void)\n"
        "{\n"
        "\tctr_poweroff_cmd();\n"
        "}\n"
        "\n")
    if ops_old not in s:
        raise SystemExit("error: regulator.c: ops anchor not found")
    s = s.replace(ops_old, helpers + ops_old, 1)

    decl_old = "\tu32 base, on, off, tdelay;\n"
    if decl_old not in s:
        raise SystemExit("error: regulator.c: probe declarations not found")
    s = s.replace(
        decl_old,
        "\tstruct regulator_dev *rdev;\n" + decl_old, 1)

    ret_old = "\treturn PTR_ERR_OR_ZERO(devm_regulator_register(dev, rdesc, &rcfg));\n"
    ret_new = (
        "\t/* devm_regulator_register() returns a struct regulator_dev *, not\n"
        "\t * an int: returning the truncated pointer as an error was what made\n"
        "\t * every MCU regulator probe fail (and left pm_power_off unset). */\n"
        "\trdev = devm_regulator_register(dev, rdesc, &rcfg);\n"
        "\tif (IS_ERR(rdev))\n"
        "\t\treturn PTR_ERR(rdev);\n"
        "\n"
        "\tif (of_property_read_bool(dev->of_node, \"system-power-controller\")) {\n"
        "\t\tctr_poweroff_map = map;\n"
        "\t\tctr_poweroff_reg = base;\n"
        "\t\tctr_poweroff_val = off;\n"
        "\t\tregister_reboot_notifier(&ctr_poweroff_nb);\n"
        "\t\tpm_power_off = ctr_pm_power_off;\n"
        "\t\tdev_info(dev, \"registered as system power-off controller\\n\");\n"
        "\t}\n"
        "\treturn 0;\n")
    if ret_old not in s:
        raise SystemExit("error: regulator.c: probe return not found")
    s = s.replace(ret_old, ret_new, 1)
    reg.write_text(s)
    print("    mcu/regulator.c: power-off handler installed (system-power-controller)")

# --------------------------------------------------------------------------
# 2. device tree
# --------------------------------------------------------------------------
d = dtsi.read_text()
node_old = ("\t\t\t\tpwroff_reg: poweroff-regulator@20 {\n"
            "\t\t\t\t\tcompatible = \"nintendo,3dsmcu-regulator\";\n"
            "\t\t\t\t\tregulator-name = \"Power off\";\n")
if "system-power-controller" in d:
    print("    nintendo3ds.dtsi: system-power-controller already set")
elif node_old in d:
    node_new = node_old + "\t\t\t\t\tsystem-power-controller;\n"
    dtsi.write_text(d.replace(node_old, node_new, 1))
    print("    nintendo3ds.dtsi: poweroff-regulator@20 is system-power-controller")
else:
    raise SystemExit("error: nintendo3ds.dtsi: poweroff-regulator node not found")
