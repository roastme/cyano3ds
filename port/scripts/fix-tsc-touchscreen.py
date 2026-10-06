#!/usr/bin/env python3
"""
fix-tsc-touchscreen.py - expose the 3DS touchscreen as a real evdev touchscreen

WHY
---
drivers/platform/nintendo3ds/tsc/touch.c reads the TSC FIFO, but it only used
the coordinates to drive an on-screen virtual keyboard and reported the circle
pad as a relative mouse (REL_X/REL_Y).  It never exported ABS_X/ABS_Y or
BTN_TOUCH, so Android's EventHub saw a keyboard (+mouse), not a touchscreen,
and the UI could not be tapped.

Earlier an attempt added ABS_X/Y to the *same* input device; that is ambiguous
for Android (one device that is a keyboard + relative mouse + absolute
touchscreen all at once) and it also draws the VKB straight onto the LCD that
SurfaceFlinger now owns.

This patch instead:

  * adds a *second*, dedicated input device ("Nintendo 3DS touchscreen") with
    only EV_ABS (ABS_X 0..319, ABS_Y 0..239, ABS_PRESSURE 0..255) and
    BTN_TOUCH;
  * reports the position on *every* poll while the pen is down (so drags
    produce moves) and ABS_PRESSURE=0 / BTN_TOUCH=0 on release;
  * disables the bring-up VKB (it drew over Android's framebuffer and its key
    events would fight the touchscreen - Android has its own IME).

The circle-pad relative mouse stays on the original device.  Touch is reported
in the same landscape 320x240 coordinate system Android renders in, matching
the transposed fbdev the display driver already provides.

Idempotent: a marker makes a second run a no-op.  touch.c uses LF line
endings.
"""

import sys
import pathlib


MARK = "input_set_abs_params(touch_hid->ts_dev"

STRUCT_OLD = (
    "struct touch_hid {\n"
    "\tstruct device *dev;\n"
    "\tstruct regmap *map;\n"
    "\tstruct input_dev *input_dev;\n"
    "\n"
    "\tstruct vkb_ctx_t vkb;\n"
    "\tunsigned long touch_jiffies;\n"
    "\tbool pendown;\n"
    "};\n"
)
STRUCT_NEW = (
    "struct touch_hid {\n"
    "\tstruct device *dev;\n"
    "\tstruct regmap *map;\n"
    "\tstruct input_dev *input_dev;\n"
    "\tstruct input_dev *ts_dev;\t/* real touchscreen, separate device */\n"
    "\n"
    "\tstruct vkb_ctx_t vkb;\n"
    "\tunsigned long touch_jiffies;\n"
    "\tbool pendown;\n"
    "\tbool ts_pendown;\n"
    "};\n"
)

# poll(): drop the VKB coordinator variables (they are only used by the block
# we are removing below).
POLL_HEAD_OLD = (
    "\tstruct touch_hid *touch_hid = input_get_drvdata(input);\n"
    "\tstruct vkb_ctx_t *vkb = &touch_hid->vkb;\n"
    "\n"
    "\tu8 raw_data[0x40] __attribute__((aligned(sizeof(u32))));\n"
)
POLL_HEAD_NEW = (
    "\tstruct touch_hid *touch_hid = input_get_drvdata(input);\n"
    "\n"
    "\tu8 raw_data[0x40] __attribute__((aligned(sizeof(u32))));\n"
)

POLL_TAIL_OLD = (
    "\tint i, j, err;\n"
    "\n"
    "\terr = touch_request_data(touch_hid->map, raw_data);\n"
)
POLL_TAIL_NEW = (
    "\tint err;\n"
    "\n"
    "\terr = touch_request_data(touch_hid->map, raw_data);\n"
)

# The whole pen-down / VKB block becomes a clean touchscreen report.
BLOCK_OLD = (
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
    "\t\t\t\tfor(i = 0; i < VKB_COLS; i++) {\n"
    "\t\t\t\t\tif(vkb->x_sizes[j][i] > 0 &&\n"
    "\t\t\t\t\t   screen_touch_x >= vkb->x_offsets[j][i] &&\n"
    "\t\t\t\t\t   screen_touch_x < vkb->x_offsets[j][i] + vkb->x_sizes[j][i] &&\n"
    "\t\t\t\t\t   screen_touch_y >= j * vkb->font->height * 2 &&\n"
    "\t\t\t\t\t   screen_touch_y < (j + 1) * vkb->font->height * 2) {\n"
    "\t\t\t\t\t\ttouch_hid->pendown = true;\n"
    "\n"
    "\t\t\t\t\t\ttouch_hid->touch_jiffies = jiffies;\n"
    "\n"
    "\t\t\t\t\t\tvkb->last_key = vkb_map_keys[j][i];\n"
    "\t\t\t\t\t\tif(vkb->key_locked[j][i / sizeof(int)] & BIT(i % sizeof(int))) {\n"
    "\t\t\t\t\t\t\tvkb->key_locked[j][i / sizeof(int)] &= ~BIT(i % sizeof(int));\n"
    "\t\t\t\t\t\t\tinput_report_key(input, vkb->last_key, 0);\n"
    "\t\t\t\t\t\t\tif(vkb->last_key == KEY_LEFTSHIFT)\n"
    "\t\t\t\t\t\t\t\tvkb->shifted &= ~LEFT_SHIFTED;\n"
    "\t\t\t\t\t\t\telse if(vkb->last_key == KEY_RIGHTSHIFT)\n"
    "\t\t\t\t\t\t\t\tvkb->shifted &= ~RIGHT_SHIFTED;\n"
    "\n"
    "\t\t\t\t\t\t\tif(vkb->shifted == 0)\n"
    "\t\t\t\t\t\t\t\tvkb_draw_bottom_lcd(vkb);\n"
    "\n"
    "\t\t\t\t\t\t\tvkb->locked_key = true;\n"
    "\n"
    "\t\t\t\t\t\t\tvkb_draw_key(vkb, j, i);\n"
    "\t\t\t\t\t\t} else {\n"
    "\t\t\t\t\t\t\tinput_report_key(input, vkb->last_key, 1);\n"
    "\t\t\t\t\t\t}\n"
    "\n"
    "\t\t\t\t\t\tvkb->held_row = j;\n"
    "\t\t\t\t\t\tvkb->held_col = i;\n"
    "\n"
    "\t\t\t\t\t\tsync = true;\n"
    "\t\t\t\t\t\ti = VKB_COLS;\n"
    "\t\t\t\t\t\tj = VKB_ROWS;\n"
    "\t\t\t\t\t}\n"
    "\t\t\t\t}\n"
    "\t\t\t}\n"
    "\t\t} else {\n"
    "\t\t\tif(!vkb->locked_key && time_is_before_jiffies(touch_hid->touch_jiffies + msecs_to_jiffies(500))) {\n"
    "\t\t\t\tvkb->key_locked[vkb->held_row][vkb->held_col / sizeof(int)] |= BIT(vkb->held_col % sizeof(int));\n"
    "\t\t\t\tvkb->locked_key = true;\n"
    "\n"
    "\t\t\t\tif(vkb_map_keys[vkb->held_row][vkb->held_col] == KEY_LEFTSHIFT)\n"
    "\t\t\t\t\tvkb->shifted |= LEFT_SHIFTED;\n"
    "\t\t\t\telse if(vkb_map_keys[vkb->held_row][vkb->held_col] == KEY_RIGHTSHIFT)\n"
    "\t\t\t\t\tvkb->shifted |= RIGHT_SHIFTED;\n"
    "\n"
    "\t\t\t\tif(vkb->shifted != 0)\n"
    "\t\t\t\t\tvkb_draw_bottom_lcd(vkb);\n"
    "\n"
    "\t\t\t\tvkb_draw_key(vkb, vkb->held_row, vkb->held_col);\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t} else {\n"
    "\t\ttouch_hid->pendown = false;\n"
    "\n"
    "\t\tif(vkb->locked_key) {\n"
    "\t\t\tvkb->locked_key = false;\n"
    "\t\t} else {\n"
    "\t\t\tif(vkb->last_key) {\n"
    "\t\t\t\tinput_report_key(input, vkb->last_key, 0);\n"
    "\t\t\t\tsync = true;\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\n"
    "\t\tvkb->last_key = 0;\n"
    "\t}\n"
)
BLOCK_NEW = (
    "\tpendown = !(raw_data[0] & BIT(4));\n"
    "\n"
    "\traw_touch_x = le16_to_cpu((raw_data[0]  << 8) | raw_data[1]);\n"
    "\traw_touch_y = le16_to_cpu((raw_data[10] << 8) | raw_data[11]);\n"
    "\n"
    "\tscreen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);\n"
    "\tscreen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);\n"
    "\n"
    "\t/*\n"
    "\t * Real touchscreen on its own input device.  Report the position on\n"
    "\t * every poll while the pen is down so a drag produces moves, and clear\n"
    "\t * pressure + BTN_TOUCH on release.  Coordinates are already the\n"
    "\t * landscape 320x240 that Android renders in.\n"
    "\t */\n"
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
    "\n"
    "\t/*\n"
    "\t * The bring-up on-screen keyboard is gone: it drew straight onto the\n"
    "\t * LCD that SurfaceFlinger now owns, and its key events would fight the\n"
    "\t * touchscreen.  Android supplies its own IME.\n"
    "\t */\n"
    "\ttouch_hid->pendown = pendown;\n"
)

PROBE_OLD = "\tvkb_init(&touch_hid->vkb);\n\n\treturn 0;\n}\n"
PROBE_NEW = (
    "\t/*\n"
    "\t * Dedicated touchscreen device: ABS_X/ABS_Y/ABS_PRESSURE + BTN_TOUCH\n"
    "\t * only, so Android's EventHub classifies it as a touchscreen.\n"
    "\t */\n"
    "\ttouch_hid->ts_dev = devm_input_allocate_device(dev);\n"
    "\tif (!touch_hid->ts_dev) {\n"
    "\t\tpr_err(\"failed to allocate touchscreen input device\");\n"
    "\t\treturn -ENOMEM;\n"
    "\t}\n"
    "\n"
    "\ttouch_hid->ts_dev->name = \"Nintendo 3DS touchscreen\";\n"
    "\ttouch_hid->ts_dev->phys = DRIVER_NAME \"/input1\";\n"
    "\ttouch_hid->ts_dev->dev.parent = dev;\n"
    "\ttouch_hid->ts_dev->id.bustype = BUS_HOST;\n"
    "\n"
    "\tset_bit(EV_ABS, touch_hid->ts_dev->evbit);\n"
    "\tset_bit(EV_KEY, touch_hid->ts_dev->evbit);\n"
    "\tset_bit(BTN_TOUCH, touch_hid->ts_dev->keybit);\n"
    "\tinput_set_abs_params(touch_hid->ts_dev, ABS_X, 0, 319, 0, 0);\n"
    "\tinput_set_abs_params(touch_hid->ts_dev, ABS_Y, 0, 239, 0, 0);\n"
    "\tinput_set_abs_params(touch_hid->ts_dev, ABS_PRESSURE, 0, 255, 0, 0);\n"
    "\n"
    "\terr = input_register_device(touch_hid->ts_dev);\n"
    "\tif (err) {\n"
    "\t\tpr_err(\"failed to register touchscreen device (%d)\\n\", err);\n"
    "\t\treturn err;\n"
    "\t}\n"
    "\n"
    "\treturn 0;\n"
    "}\n"
)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-tsc-touchscreen.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/tsc/touch.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    tsc/touch.c: dedicated touchscreen input device already present")
        return 0

    for old, new, what in (
        (STRUCT_OLD, STRUCT_NEW, "struct touch_hid"),
        (POLL_HEAD_OLD, POLL_HEAD_NEW, "poll header"),
        (POLL_TAIL_OLD, POLL_TAIL_NEW, "poll locals"),
        (BLOCK_OLD, BLOCK_NEW, "poll pen/VKB block"),
        (PROBE_OLD, PROBE_NEW, "probe registration"),
    ):
        if old not in s:
            print(f"error: touch.c: {what} not found (unexpected version)",
                  file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    p.write_text(s, newline="")
    assert MARK in s
    print("    tsc/touch.c: touchscreen input device added, VKB disabled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
