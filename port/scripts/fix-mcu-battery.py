#!/usr/bin/env python3
"""
fix-mcu-battery.py - make the MCU battery a complete, *live* Android battery

WHY
---
The 3DS MCU charger driver (drivers/platform/nintendo3ds/mcu/charger.c)
registers BAT0 (Battery) and ADP0 (Mains) with the kernel power_supply class
from MCU registers 0x0A/0x0B/0x0D/0x0F.  Two things are missing for Android:

1. Android's BatteryService (Gingerbread/CM7) scans /sys/class/power_supply/*/
   type and reads present/online/status/capacity/voltage_now/temp, but it also
   *looks* for `health` and `technology` and logs "batteryHealthPath not found" /
   "batteryTechnologyPath not found" without them.  Add both.

2. BatteryService only refreshes when the kernel emits a power_supply uevent
   (power_supply_changed()).  This driver has no charger interrupt wired up, so
   it never emitted one: the battery was read exactly once at boot (the
   status-bar icon showed the level at boot) and then never changed - plugging
   the charger in lit the console's orange LED but Android kept showing
   "Discharging".  Poll the MCU every 2 s and call power_supply_changed() when
   anything moves.

Idempotent.
"""

import pathlib
import sys

# --- 1. HEALTH / TECHNOLOGY properties -------------------------------------
PROPS_ANCHOR = """static enum power_supply_property bat_props[] = {
\tPOWER_SUPPLY_PROP_PRESENT,
\tPOWER_SUPPLY_PROP_ONLINE,
\tPOWER_SUPPLY_PROP_STATUS,
\tPOWER_SUPPLY_PROP_VOLTAGE_NOW,
\tPOWER_SUPPLY_PROP_CAPACITY,
\tPOWER_SUPPLY_PROP_TEMP,
}"""

PROPS_NEW = """static enum power_supply_property bat_props[] = {
\tPOWER_SUPPLY_PROP_PRESENT,
\tPOWER_SUPPLY_PROP_ONLINE,
\tPOWER_SUPPLY_PROP_STATUS,
\tPOWER_SUPPLY_PROP_HEALTH,
\tPOWER_SUPPLY_PROP_TECHNOLOGY,
\tPOWER_SUPPLY_PROP_VOLTAGE_NOW,
\tPOWER_SUPPLY_PROP_CAPACITY,
\tPOWER_SUPPLY_PROP_TEMP,
}"""

CASE_ANCHOR = """\tcase POWER_SUPPLY_PROP_CAPACITY:
\t\tval->intval = data[REG_CAPACITY];
\t\tbreak;
"""

CASE_NEW = """\tcase POWER_SUPPLY_PROP_CAPACITY:
\t\tval->intval = data[REG_CAPACITY];
\t\tbreak;

\tcase POWER_SUPPLY_PROP_HEALTH:
\t\t/*
\t\t * The MCU does not report a health code; treat a normal pack as
\t\t * "Good" (Android's getBatteryHealth() maps 'G' -> GOOD) and flag
\t\t * an implausibly hot reading as Overheat.  Temp reg is signed
\t\t * degrees C; Android receives the same sign-extended * 10 the
\t\t * TEMP property uses.
\t\t */
\t\tval->intval = sign_extend32(data[REG_TEMPERATURE], 7) >= 65 ?
\t\t\tPOWER_SUPPLY_HEALTH_OVERHEAT : POWER_SUPPLY_HEALTH_GOOD;
\t\tbreak;

\tcase POWER_SUPPLY_PROP_TECHNOLOGY:
\t\tval->intval = POWER_SUPPLY_TECHNOLOGY_LION;
\t\tbreak;
"""

# --- 2. periodic power_supply_changed() ------------------------------------
POLL_EDITS = [
    # includes
    ("#include <linux/mod_devicetable.h>\n",
     "#include <linux/mod_devicetable.h>\n"
     "#include <linux/string.h>\n"
     "#include <linux/workqueue.h>\n"),
    # poll period
    ("#define STATUS_AC_PLUGGED\tBIT(3)\n"
     "#define STATUS_BAT_CHARGING\tBIT(4)\n",
     "#define STATUS_AC_PLUGGED\tBIT(3)\n"
     "#define STATUS_BAT_CHARGING\tBIT(4)\n"
     "\n"
     "/*\n"
     " * Android's BatteryService only re-reads the battery when the kernel emits\n"
     " * a power_supply uevent, and this driver has no charger interrupt to hang\n"
     " * one off, so poll the MCU and call power_supply_changed() when anything\n"
     " * moves (plug/unplug, charging, capacity, ...).  2 s keeps the charging\n"
     " * icon prompt without hammering the I2C bus.\n"
     " */\n"
     "#define CTR_CHARGER_POLL_MS\t2000\n"),
    # struct fields
    ("struct ctr_charger {\n"
     "\tunsigned io_addr;\n"
     "\tstruct regmap *map;\n"
     "\n"
     "\tstruct power_supply *ac;\n"
     "\tstruct power_supply *bat;\n"
     "};\n",
     "struct ctr_charger {\n"
     "\tunsigned io_addr;\n"
     "\tstruct regmap *map;\n"
     "\n"
     "\tstruct power_supply *ac;\n"
     "\tstruct power_supply *bat;\n"
     "\n"
     "\tstruct delayed_work work;\n"
     "\tu8 last[6];\n"
     "\tbool have_last;\n"
     "};\n"),
    # poll worker
    ("static int ctr_charger_probe(struct platform_device *pdev)\n",
     "static void ctr_charger_poll(struct work_struct *work)\n"
     "{\n"
     "\tstruct ctr_charger *charger =\n"
     "\t\tcontainer_of(to_delayed_work(work), struct ctr_charger, work);\n"
     "\tu8 data[6];\n"
     "\n"
     "\tif (!ctr_charger_read(charger, data)) {\n"
     "\t\tif (!charger->have_last ||\n"
     "\t\t    memcmp(data, charger->last, sizeof(data))) {\n"
     "\t\t\tmemcpy(charger->last, data, sizeof(data));\n"
     "\t\t\tcharger->have_last = true;\n"
     "\t\t\tpower_supply_changed(charger->bat);\n"
     "\t\t\tpower_supply_changed(charger->ac);\n"
     "\t\t}\n"
     "\t}\n"
     "\tschedule_delayed_work(&charger->work,\n"
     "\t\t\t      msecs_to_jiffies(CTR_CHARGER_POLL_MS));\n"
     "}\n"
     "\n"
     "static int ctr_charger_probe(struct platform_device *pdev)\n"),
    # arm the poll after both supplies exist
    ("\tcharger->bat = devm_power_supply_register(dev, &bat_desc, &psy_cfg);\n"
     "\tif (IS_ERR(charger->bat))\n"
     "\t\treturn PTR_ERR(charger->bat);\n"
     "\n"
     "\treturn 0;\n"
     "}\n",
     "\tcharger->bat = devm_power_supply_register(dev, &bat_desc, &psy_cfg);\n"
     "\tif (IS_ERR(charger->bat))\n"
     "\t\treturn PTR_ERR(charger->bat);\n"
     "\n"
     "\tplatform_set_drvdata(pdev, charger);\n"
     "\tINIT_DELAYED_WORK(&charger->work, ctr_charger_poll);\n"
     "\tschedule_delayed_work(&charger->work,\n"
     "\t\t\t      msecs_to_jiffies(CTR_CHARGER_POLL_MS));\n"
     "\n"
     "\treturn 0;\n"
     "}\n"),
    # stop the poll on remove
    ("static int ctr_charger_remove(struct platform_device *pdev)\n"
     "{\n"
     "\treturn 0;\n"
     "}\n",
     "static int ctr_charger_remove(struct platform_device *pdev)\n"
     "{\n"
     "\tstruct ctr_charger *charger = platform_get_drvdata(pdev);\n"
     "\n"
     "\tif (charger)\n"
     "\t\tcancel_delayed_work_sync(&charger->work);\n"
     "\treturn 0;\n"
     "}\n"),
]


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: fix-mcu-battery.py <kernel-tree>", file=sys.stderr)
        return 2
    p = pathlib.Path(sys.argv[1]) / "drivers/platform/nintendo3ds/mcu/charger.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    s = p.read_text()
    changed = []

    if "POWER_SUPPLY_PROP_TECHNOLOGY" not in s:
        if PROPS_ANCHOR not in s:
            print("error: charger.c: bat_props[] anchor not found", file=sys.stderr)
            return 1
        if CASE_ANCHOR not in s:
            print("error: charger.c: capacity switch case anchor not found", file=sys.stderr)
            return 1
        s = s.replace(PROPS_ANCHOR, PROPS_NEW, 1)
        s = s.replace(CASE_ANCHOR, CASE_NEW, 1)
        changed.append("HEALTH/TECHNOLOGY")

    if "ctr_charger_poll" not in s:
        for old, new in POLL_EDITS:
            if old not in s:
                print("error: charger.c: polling anchor not found:\n"
                      + old.splitlines()[0], file=sys.stderr)
                return 1
            s = s.replace(old, new, 1)
        changed.append("periodic power_supply_changed()")

    if changed:
        p.write_text(s)
        print("    mcu/charger.c: BAT0/ADP0 now report " + " + ".join(changed))
    else:
        print("    mcu/charger.c: battery properties + polling already present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
