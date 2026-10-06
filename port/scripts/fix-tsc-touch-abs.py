#!/usr/bin/env python3
"""
fix-tsc-touch-abs.py - report the 3DS touchscreen as a real evdev touchscreen

WHY THIS EXISTS
---------------
drivers/platform/nintendo3ds/tsc/touch.c reads the TSC FIFO, but it only uses
the touch coordinates to drive an on-screen virtual keyboard and reports the
circle pad as a relative mouse (REL_X/REL_Y).  It never exports ABS_X/ABS_Y or
BTN_TOUCH, so Android's EventHub sees a keyboard + mouse, not a touchscreen,
and the UI cannot be touched.

This patch adds the standard single-pointer touchscreen contract that Android's
EventHub looks for:

  * probe: EV_ABS + input_set_abs_params(ABS_X 0..319, ABS_Y 0..239,
    ABS_PRESSURE 0..255) and BTN_TOUCH;
  * poll: report ABS_X/ABS_Y/ABS_PRESSURE + BTN_TOUCH on every poll while the
    pen is down (so drags produce moves), and pressure 0 / BTN_TOUCH 0 on
    release.

The existing VKB/circle-pad behaviour is left in place, so nothing regresses.

Idempotent: running it twice changes nothing.  touch.c uses LF line endings.
"""

import sys
import pathlib

MARK = "input_report_abs(input, ABS_X"

OLD_PROBE = (
    "\t/* Enable VKB keys */\n"
    "\tset_bit(EV_KEY, input->evbit);\n"
    "\tinput_set_capability(input, EV_MSC, MSC_SCAN);\n"
)
NEW_PROBE = (
    "\t/* Touchscreen: Android's EventHub wants ABS_X/Y + BTN_TOUCH */\n"
    "\tset_bit(EV_ABS, input->evbit);\n"
    "\tinput_set_abs_params(input, ABS_X, 0, 319, 0, 0);\n"
    "\tinput_set_abs_params(input, ABS_Y, 0, 239, 0, 0);\n"
    "\tinput_set_abs_params(input, ABS_PRESSURE, 0, 255, 0, 0);\n"
    "\n"
    "\t/* Enable VKB keys */\n"
    "\tset_bit(EV_KEY, input->evbit);\n"
    "\tset_bit(BTN_TOUCH, input->keybit);\n"
    "\tinput_set_capability(input, EV_MSC, MSC_SCAN);\n"
)

OLD_POLL = (
    "\tpendown = !(raw_data[0] & BIT(4));\n"
    "\n"
    "\tif (pendown) {\n"
    "\t\tif(!touch_hid->pendown) {\n"
    "\t\t\traw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]);\n"
    "\t\t\traw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]);\n"
    "\n"
    "\t\t\tscreen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);\n"
    "\t\t\tscreen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);\n"
    "\n"
    "\t\t\tfor(j = 0; j < VKB_ROWS; j++) {\n"
)
NEW_POLL = (
    "\tpendown = !(raw_data[0] & BIT(4));\n"
    "\n"
    "\tif (pendown) {\n"
    "\t\traw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]);\n"
    "\t\traw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]);\n"
    "\n"
    "\t\tscreen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);\n"
    "\t\tscreen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);\n"
    "\n"
    "\t\t/*\n"
    "\t\t * Real touchscreen report: on every poll while down, so a drag\n"
    "\t\t * produces moves instead of only the initial press.\n"
    "\t\t */\n"
    "\t\tinput_report_abs(input, ABS_X, screen_touch_x);\n"
    "\t\tinput_report_abs(input, ABS_Y, screen_touch_y);\n"
    "\t\tinput_report_abs(input, ABS_PRESSURE, 255);\n"
    "\t\tinput_report_key(input, BTN_TOUCH, 1);\n"
    "\t\tsync = true;\n"
    "\n"
    "\t\tif(!touch_hid->pendown) {\n"
    "\t\t\tfor(j = 0; j < VKB_ROWS; j++) {\n"
)

OLD_REL = (
    "\t} else {\n"
    "\t\ttouch_hid->pendown = false;\n"
    "\n"
    "\t\tif(vkb->locked_key) {\n"
)
NEW_REL = (
    "\t} else {\n"
    "\t\ttouch_hid->pendown = false;\n"
    "\n"
    "\t\tinput_report_abs(input, ABS_PRESSURE, 0);\n"
    "\t\tinput_report_key(input, BTN_TOUCH, 0);\n"
    "\t\tsync = true;\n"
    "\n"
    "\t\tif(vkb->locked_key) {\n"
)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-tsc-touch-abs.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/tsc/touch.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    tsc/touch.c: ABS touchscreen reporting already present")
        return 0

    for old, new, what in ((OLD_PROBE, NEW_PROBE, "probe capability setup"),
                           (OLD_POLL, NEW_POLL, "poll pendown block"),
                           (OLD_REL, NEW_REL, "poll release block")):
        if old not in s:
            print(f"error: touch.c: {what} not found (unexpected version)",
                  file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    p.write_text(s, newline="")
    assert "input_set_abs_params(input, ABS_X" in s
    assert "input_report_abs(input, ABS_X" in s
    print("    tsc/touch.c: ABS_X/ABS_Y/ABS_PRESSURE + BTN_TOUCH added")
    return 0


if __name__ == "__main__":
    sys.exit(main())
