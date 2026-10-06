#!/usr/bin/env python3
"""
fix-tsc-touch-filter.py - median-filter the TSC FIFO and anchor taps to DOWN

WHY
---
`fix-tsc-touchscreen.py` makes the TSC a real evdev touchscreen, and
`fix-tsc-touch-cal.py` fixes the *scale*.  Neither touches the two things the
TSC FIFO itself makes possible, and both are what the Octoblimp port does and
this one did not:

  1. The FIFO holds FIVE conversions per axis (see `struct touch_fifo_data`:
     `u16 touch[2][5]` at FIFO bytes 0..9 for X and 10..19 for Y).  This driver
     used only the first of each pair.  One noisy ADC conversion therefore
     reached Android as position jitter -- a tap near an edge can land several
     pixels away from the last one at the same spot.  Median-of-5 drops a
     single wild conversion, exactly like the hardware window implies.

  2. A resistive panel's reading ramps with contact pressure: the same physical
     tap reports a DOWN and an UP that can be tens of pixels apart (Octoblimp
     logged 20-70 px).  Android decides a tap's location from the DOWN sample,
     so a drifting UP turns a tap into a drag or lands it off the icon.  Anchor
     the contact to its DOWN position and only start reporting movement once the
     finger has genuinely travelled (12 px box, the value Octoblimp ships).

Both are what makes Octoblimp's touchscreen feel correct at the edges while
this port's did not; the calibration constants are a separate, smaller error.

Applied *after* fix-tsc-touchscreen.py / -map.py / -cal.py (it edits the code
they produce).  Idempotent: a marker makes a second run a no-op.
"""

import sys
import pathlib

MARK = "N3DS_TOUCH_MEDIAN_SLOP"

STRUCT_OLD = "\tbool ts_pendown;\n"
STRUCT_NEW = (
    "\tbool ts_pendown;\n"
    "\t/* N3DS_TOUCH_MEDIAN_SLOP: the position this contact was anchored to\n"
    "\t * when the pen went down (see touch_input_poll). */\n"
    "\tu16 ts_down_x;\n"
    "\tu16 ts_down_y;\n"
)

POLL_ANCHOR = "static void touch_input_poll(struct input_dev *input)\n{\n"
POLL_NEW = (
    "/*\n"
    " * The TSC FIFO holds five conversions per axis.  The inherited driver read\n"
    " * only the first, so a single noisy conversion reached Android as position\n"
    " * jitter.  Sorting to the middle discards it; 5 elements is a fixed size,\n"
    " * so this is a plain insertion sort on the stack.\n"
    " */\n"
    "static u16 touch_median5(u16 *v)\n"
    "{\n"
    "\tint i, j;\n"
    "\n"
    "\tfor (i = 1; i < 5; i++) {\n"
    "\t\tu16 n = v[i];\n"
    "\n"
    "\t\tfor (j = i; j && v[j - 1] > n; j--)\n"
    "\t\t\tv[j] = v[j - 1];\n"
    "\t\tv[j] = n;\n"
    "\t}\n"
    "\treturn v[2];\n"
    "}\n"
    "\n"
    "static void touch_input_poll(struct input_dev *input)\n{\n"
)

RAW_OLD = (
    "\traw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]) & 0xfff;\n"
    "\traw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]) & 0xfff;\n"
)
RAW_NEW = (
    "\t/*\n"
    "\t * Average out the FIFO's five conversions per axis with a median\n"
    "\t * instead of trusting the first sample (N3DS_TOUCH_MEDIAN_SLOP).\n"
    "\t */\n"
    "\t{\n"
    "\t\tu16 xs[5], ys[5];\n"
    "\t\tint i;\n"
    "\n"
    "\t\tfor (i = 0; i < 5; i++) {\n"
    "\t\t\txs[i] = (((u16)raw_data[i * 2] << 8) |\n"
    "\t\t\t\t raw_data[i * 2 + 1]) & 0xfff;\n"
    "\t\t\tys[i] = (((u16)raw_data[10 + i * 2] << 8) |\n"
    "\t\t\t\t raw_data[10 + i * 2 + 1]) & 0xfff;\n"
    "\t\t}\n"
    "\t\traw_touch_x = touch_median5(xs);\n"
    "\t\traw_touch_y = touch_median5(ys);\n"
    "\t}\n"
)

REPORT_OLD = (
    "\tif (touch_hid->ts_dev) {\n"
    "\t\tif (pendown) {\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_X, screen_touch_x);\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_Y, screen_touch_y);\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_PRESSURE, 255);\n"
    "\t\t\tinput_report_key(touch_hid->ts_dev, BTN_TOUCH, 1);\n"
    "\t\t\tinput_sync(touch_hid->ts_dev);\n"
    "\t\t\ttouch_hid->ts_pendown = true;\n"
    "\t\t} else if (touch_hid->ts_pendown) {\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_PRESSURE, 0);\n"
    "\t\t\tinput_report_key(touch_hid->ts_dev, BTN_TOUCH, 0);\n"
    "\t\t\tinput_sync(touch_hid->ts_dev);\n"
    "\t\t\ttouch_hid->ts_pendown = false;\n"
    "\t\t}\n"
    "\t}\n"
)
REPORT_NEW = (
    "\tif (touch_hid->ts_dev) {\n"
    "\t\tif (pendown) {\n"
    "\t\t\t/*\n"
    "\t\t\t * N3DS_TOUCH_SLOP: a resistive contact's reading ramps\n"
    "\t\t\t * with pressure, so the same physical tap can report a\n"
    "\t\t\t * DOWN and an UP tens of pixels apart.  Android takes a\n"
    "\t\t\t * tap's position from DOWN, so anchor the whole contact\n"
    "\t\t\t * to it and only report movement once the finger has\n"
    "\t\t\t * genuinely travelled -- otherwise ADC drift becomes a\n"
    "\t\t\t * drag and the tap lands off the icon.\n"
    "\t\t\t */\n"
    "\t\t\tif (!touch_hid->ts_pendown) {\n"
    "\t\t\t\ttouch_hid->ts_down_x = screen_touch_x;\n"
    "\t\t\t\ttouch_hid->ts_down_y = screen_touch_y;\n"
    "\t\t\t} else if (abs((int)screen_touch_x -\n"
    "\t\t\t\t       (int)touch_hid->ts_down_x) <= 12 &&\n"
    "\t\t\t\t   abs((int)screen_touch_y -\n"
    "\t\t\t\t       (int)touch_hid->ts_down_y) <= 12) {\n"
    "\t\t\t\tscreen_touch_x = touch_hid->ts_down_x;\n"
    "\t\t\t\tscreen_touch_y = touch_hid->ts_down_y;\n"
    "\t\t\t}\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_X, screen_touch_x);\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_Y, screen_touch_y);\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_PRESSURE, 255);\n"
    "\t\t\tinput_report_key(touch_hid->ts_dev, BTN_TOUCH, 1);\n"
    "\t\t\tinput_sync(touch_hid->ts_dev);\n"
    "\t\t\ttouch_hid->ts_pendown = true;\n"
    "\t\t} else if (touch_hid->ts_pendown) {\n"
    "\t\t\tinput_report_abs(touch_hid->ts_dev, ABS_PRESSURE, 0);\n"
    "\t\t\tinput_report_key(touch_hid->ts_dev, BTN_TOUCH, 0);\n"
    "\t\t\tinput_sync(touch_hid->ts_dev);\n"
    "\t\t\ttouch_hid->ts_pendown = false;\n"
    "\t\t}\n"
    "\t}\n"
)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-tsc-touch-filter.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/tsc/touch.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    tsc/touch.c: FIFO median + touch-slop already present")
        return 0

    for old, new, what, required in (
        (STRUCT_OLD, STRUCT_NEW, "struct touch_hid", True),
        (POLL_ANCHOR, POLL_NEW, "touch_input_poll anchor", True),
        (RAW_OLD, RAW_NEW, "raw sample extraction", True),
        (REPORT_OLD, REPORT_NEW, "touchscreen report block", True),
    ):
        n = s.count(old)
        if n != 1:
            if required:
                print(f"error: touch.c: {what}: expected 1 match, found {n} "
                      "(unexpected version)", file=sys.stderr)
                return 1
            continue
        s = s.replace(old, new, 1)

    p.write_text(s, newline="")
    assert MARK in s
    print("    tsc/touch.c: FIFO median-of-5 + touch-slop anchoring")
    return 0


if __name__ == "__main__":
    sys.exit(main())
