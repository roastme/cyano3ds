#!/usr/bin/env python3
"""
fix-tsc-touchscreen-diag.py - always sample the TSC FIFO and log raw samples

WHY
---
After fix-tsc-touchscreen.py the kernel exports a clean evdev touchscreen
(`ABS_X 0..319`, `ABS_Y 0..239`, `ABS_PRESSURE`, `BTN_TOUCH`) and Android's
EventHub picks it up as CLASS_TOUCHSCREEN, but tapping the panel still does
nothing.  Two things are worth pinning down in one boot:

  1. `touch_request_data()` gates the 52-byte FIFO read on
     `TSC[67h:26h].bit1 == 0`.  fastboot3DS' own `CODEC_getRawAdcData()` uses
     the same bit, but its unconditional `CODEC_getRawData()` comments that
     this status register "seems useless and doesn't affect functionality".
     A gate that latches shut would stop *all* sampling forever, which is
     exactly "nothing happens when I touch the screen", so read every poll.

  2. Nothing logs the raw ADC values, so we cannot tell a dead controller from
     a coordinate-mapping problem.  Log the first few polls and the first few
     pen-down edges (raw 12-bit + mapped screen coordinates), then go quiet.

The 12-bit mask on the raw values matches 3dbrew Codec_Services ("12 bits
number") and fastboot3DS (`x * 320 / 4096`), and protects the mapping from the
0xFFFF "not touched" pattern (where bit 12 set would otherwise shift Y).

Idempotent: its own marker makes a second run a no-op.  touch.c uses LF.
"""

import sys
import pathlib


MARK = "tsc: poll="

STRUCT_OLD = (
    "\tstruct vkb_ctx_t vkb;\n"
    "\tunsigned long touch_jiffies;\n"
    "\tbool pendown;\n"
    "\tbool ts_pendown;\n"
    "};\n"
)
STRUCT_NEW = (
    "\tstruct vkb_ctx_t vkb;\n"
    "\tunsigned long touch_jiffies;\n"
    "\tbool pendown;\n"
    "\tbool ts_pendown;\n"
    "\tunsigned int dbg_polls;\t/* bring-up sample counter */\n"
    "\tunsigned int dbg_edges;\t/* bring-up pen-down counter */\n"
    "};\n"
)

REQ_OLD = (
    "static int touch_request_data(struct regmap *map, u8 *buffer)\n"
    "{\n"
    "\tint err;\n"
    "\tunsigned int reg;\n"
    "\n"
    "\t/* acknowledge touch? */\n"
    "\terr = regmap_read(map, TOUCH_REG(0x26), &reg);\n"
    "\tif (err) return err;\n"
    "\n"
    "\t/* no new data available */\n"
    "\tif (reg & BIT(1))\n"
    "\t\treturn -ENODATA;\n"
    "\n"
    "\treturn regmap_bulk_read(map, TOUCH_FIFO_REG, buffer, 0x34);\n"
    "}\n"
)
REQ_NEW = (
    "static int touch_request_data(struct regmap *map, u8 *buffer)\n"
    "{\n"
    "\tint err;\n"
    "\tunsigned int reg;\n"
    "\n"
    "\t/* status read: informational only (bank 67h register 26h) */\n"
    "\terr = regmap_read(map, TOUCH_REG(0x26), &reg);\n"
    "\tif (err) return err;\n"
    "\n"
    "\t/*\n"
    "\t * Do not gate on bit 1.  fastboot3DS' CODEC_getRawData() reads the\n"
    "\t * FIFO unconditionally and notes that this status register \"seems\n"
    "\t * useless and doesn't affect funtionality\".  A stuck gate here would\n"
    "\t * stop all sampling, so always read the 52-byte (26 x 16-bit) FIFO.\n"
    "\t */\n"
    "\treturn regmap_bulk_read(map, TOUCH_FIFO_REG, buffer, 0x34);\n"
    "}\n"
)

POLL_OLD = (
    "\terr = touch_request_data(touch_hid->map, raw_data);\n"
    "\tif (err == -ENODATA)\n"
    "\t\treturn;\n"
    "\n"
    "\tif (err) {\n"
)
POLL_NEW = (
    "\terr = touch_request_data(touch_hid->map, raw_data);\n"
    "\tif (err) {\n"
    "\t\t/* bring-up: tell \"poller never ran\" from \"SPI read failed\" */\n"
    "\t\tif (touch_hid->dbg_polls < 6)\n"
    "\t\t\tpr_err(\"tsc: FIFO read failed: %d\\n\", err);\n"
    "\t\ttouch_hid->dbg_polls++;\n"
)

COORD_OLD = (
    "\traw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]);\n"
    "\traw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]);\n"
    "\n"
    "\tscreen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);\n"
    "\tscreen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);\n"
    "\n"
)
COORD_NEW = (
    "\traw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]) & 0xfff;\n"
    "\traw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]) & 0xfff;\n"
    "\n"
    "\tscreen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);\n"
    "\tscreen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);\n"
    "\n"
    "\t/*\n"
    "\t * Bring-up diagnostic: the first few polls prove the controller is\n"
    "\t * being sampled; every pen-down edge logs the raw 12-bit values so a\n"
    "\t * single boot can pin down the axis mapping (the panel is portrait,\n"
    "\t * so a swap/invert may be needed).  Rate limited so the SD log cannot\n"
    "\t * flood.\n"
    "\t */\n"
    "\tif (touch_hid->dbg_polls < 6) {\n"
    "\t\tpr_info(\"tsc: poll=%u raw=(0x%03x,0x%03x) down=%d\\n\",\n"
    "\t\t\ttouch_hid->dbg_polls, raw_touch_x, raw_touch_y, pendown);\n"
    "\t\ttouch_hid->dbg_polls++;\n"
    "\t}\n"
    "\tif (pendown && !touch_hid->ts_pendown && touch_hid->dbg_edges < 300) {\n"
    "\t\tpr_info(\"tsc: tap raw=(0x%03x,0x%03x) legacy_screen=(%u,%u)\\n\",\n"
    "\t\t\traw_touch_x, raw_touch_y, screen_touch_x, screen_touch_y);\n"
    "\t\ttouch_hid->dbg_edges++;\n"
    "\t}\n"
    "\n"
)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-tsc-touchscreen-diag.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/tsc/touch.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    tsc/touch.c: TSC diagnostics already present")
        return 0

    for old, new, what in (
        (STRUCT_OLD, STRUCT_NEW, "struct touch_hid"),
        (REQ_OLD, REQ_NEW, "touch_request_data"),
        (POLL_OLD, POLL_NEW, "poll -ENODATA gate"),
        (COORD_OLD, COORD_NEW, "coordinate mapping"),
    ):
        if old not in s:
            print(f"error: touch.c: {what} not found (is fix-tsc-touchscreen.py applied?)",
                  file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    p.write_text(s, newline="")
    assert MARK in s
    print("    tsc/touch.c: FIFO always sampled + raw sample logging added")
    return 0


if __name__ == "__main__":
    sys.exit(main())
