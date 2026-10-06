#!/usr/bin/env python3
"""
fix-tsc-touch-map.py - make the touchscreen coordinate mapping selectable at runtime

WHY
---
Android renders in a landscape 320x240 space, but the 3DS bottom panel is
scanned portrait (240 px/line, 320 lines) and the driver transposes a finished
landscape frame into it.  The touch digitizer has its own native axes, so the
raw 12-bit X/Y have to be transposed into that same landscape space.  The naive
map (X = raw_x, Y = raw_y) puts the finger up to 90 degrees away from where the
UI thinks it is -- a tap meant for an icon can land on the status bar, which
opens the notification shade (a touch-modal TYPE_STATUS_BAR_PANEL window) and
then swallows every following touch.

The upstream linux-3ds DTS says exactly which transpose it wants:

    touchscreen-size-x = <4096>;
    touchscreen-size-y = <4096>;
    touchscreen-inverted-y;
    touchscreen-swapped-x-y;

`touchscreen_apply_prop_to_x_y()` inverts y and *then* swaps, i.e.

    X = (max - raw_y) * 320 / 4096      (invert then swap)
    Y =  raw_x        * 240 / 4096

but touch.c never calls touchscreen_parse_properties(), so the properties are
inert and the mapping is the naive one.

This patch does two things:

  1. computes the mapping from a variable instead of hard-coding it, and
  2. exposes that variable as /proc/ctr_touch_map so /init can set it from a
     file on the SD card.  Mapping values (bit mask):

        bit0 (1) = swap X and Y
        bit1 (2) = invert the resulting X (319 - x)
        bit2 (4) = invert the resulting Y (239 - y)

     0 = naive            X=raw_x          Y=raw_y
     1 = swap             X=raw_y          Y=raw_x
     2 = invert-x         X=319-raw_x      Y=raw_y
     3 = swap + invert-x  X=319-raw_y      Y=raw_x   <- the DTS mapping
     4 = invert-y         X=raw_x          Y=239-raw_y
     5 = swap + invert-y  X=raw_y          Y=239-raw_x
     6 = invert both      X=319-raw_x      Y=239-raw_y
     7 = swap + both      X=319-raw_y      Y=239-raw_x

The default is 0 (identity, the mapping the working xerpi bottom_lcd driver
used): the 3DS digitizer's native axes already line up with the landscape
320x240 Android renders in, so a tap lands under the finger.  (The upstream
DTS asks for 3, but that is the portrait-panel mapping: on this landscape
framebuffer it rotates/mirrors the touch and sends taps into the status bar,
which opens the touch-modal notification shade.)  /init reads
/mnt/sd/CYANO3DS/touch-map.txt and writes it here, so a wrong guess can be
corrected by editing one file on the card instead of rebuilding the kernel.

Idempotent: a marker makes a second run a no-op.  touch.c uses LF line endings.
"""

import sys
import pathlib


MARK = "ctr_touch_map"

INCLUDE_OLD = (
    "#include <linux/font.h>\n"
    "#include <asm/io.h>\n"
)
INCLUDE_NEW = (
    "#include <linux/font.h>\n"
    "#include <asm/io.h>\n"
    "#include <linux/proc_fs.h>\n"
    "#include <linux/seq_file.h>\n"
    "#include <linux/uaccess.h>\n"
)

GLOBAL_OLD = (
    "#define TOUCH_FIFO_REG\t((0xFB << 7) | 0x01) /* bank FBh, register 01h */\n"
)
GLOBAL_NEW = (
    "#define TOUCH_FIFO_REG\t((0xFB << 7) | 0x01) /* bank FBh, register 01h */\n"
    "\n"
    "/*\n"
    " * Runtime touch coordinate mapping (see /proc/ctr_touch_map):\n"
    " *   bit0 = swap X/Y, bit1 = invert X, bit2 = invert Y.\n"
    " * Default 0 (identity): the digitizer already lines up with the\n"
    " * landscape 320x240 Android renders in.  (The upstream DTS asks for 3,\n"
    " * but that is the portrait-panel mapping and rotates the touch here.)\n"
    " */\n"
    "static int ctr_touch_map = 0;\n"
)

COORD_OLD = (
    "\tscreen_touch_x = (u16)((u32)raw_touch_x * 320 / MAX_12BIT);\n"
    "\tscreen_touch_y = (u16)((u32)raw_touch_y * 240 / MAX_12BIT);\n"
)
COORD_NEW = (
    "\t/*\n"
    "\t * Map the digitizer's native axes into the landscape 320x240 space\n"
    "\t * Android renders in.  The transpose (and any inversion) is selected\n"
    "\t * at runtime through /proc/ctr_touch_map; the default is the mapping\n"
    "\t * the upstream DTS asks for (swap + inverted-y).\n"
    "\t */\n"
    "\t{\n"
    "\t\tu32 mx = raw_touch_x, my = raw_touch_y;\n"
    "\n"
    "\t\tif (ctr_touch_map & 1) {\n"
    "\t\t\tu32 t = mx;\n"
    "\t\t\tmx = my;\n"
    "\t\t\tmy = t;\n"
    "\t\t}\n"
    "\t\tscreen_touch_x = (u16)(mx * 320 / MAX_12BIT);\n"
    "\t\tscreen_touch_y = (u16)(my * 240 / MAX_12BIT);\n"
    "\t\tif (ctr_touch_map & 2)\n"
    "\t\t\tscreen_touch_x = 319 - screen_touch_x;\n"
    "\t\tif (ctr_touch_map & 4)\n"
    "\t\t\tscreen_touch_y = 239 - screen_touch_y;\n"
    "\t}\n"
)

LOG_OLD = 'pr_info("tsc: tap raw=(0x%03x,0x%03x) legacy_screen=(%u,%u)\\n",\n'
LOG_NEW = 'pr_info("tsc: tap raw=(0x%03x,0x%03x) map=%d screen=(%u,%u)\\n",\n'
LOG_ARGS_OLD = "\t\t\traw_touch_x, raw_touch_y, screen_touch_x, screen_touch_y);\n"
LOG_ARGS_NEW = "\t\t\traw_touch_x, raw_touch_y, ctr_touch_map,\n" \
               "\t\t\tscreen_touch_x, screen_touch_y);\n"

PROBE_INSERT_OLD = (
    "static int touch_hid_probe(struct platform_device *pdev)\n"
)
PROC_BLOCK = (
    "/*\n"
    " * /proc/ctr_touch_map - read or write the touchscreen coordinate mapping.\n"
    " * Written by /init from /mnt/sd/CYANO3DS/touch-map.txt so the mapping\n"
    " * can be corrected without rebuilding the kernel.\n"
    " */\n"
    "static int ctr_touch_map_show(struct seq_file *m, void *v)\n"
    "{\n"
    "\tseq_printf(m, \"%d\\n\", ctr_touch_map);\n"
    "\treturn 0;\n"
    "}\n"
    "\n"
    "static int ctr_touch_map_open(struct inode *inode, struct file *file)\n"
    "{\n"
    "\treturn single_open(file, ctr_touch_map_show, NULL);\n"
    "}\n"
    "\n"
    "static ssize_t ctr_touch_map_write(struct file *file,\n"
    "\t\tconst char __user *buf, size_t count, loff_t *ppos)\n"
    "{\n"
    "\tchar kbuf[8];\n"
    "\tint val;\n"
    "\n"
    "\tif (count > sizeof(kbuf) - 1)\n"
    "\t\tcount = sizeof(kbuf) - 1;\n"
    "\tif (copy_from_user(kbuf, buf, count))\n"
    "\t\treturn -EFAULT;\n"
    "\tkbuf[count] = '\\0';\n"
    "\tif (kstrtoint(kbuf, 0, &val))\n"
    "\t\treturn -EINVAL;\n"
    "\tctr_touch_map = val & 7;\n"
    "\tpr_info(\"touch map set to %d\\n\", ctr_touch_map);\n"
    "\treturn count;\n"
    "}\n"
    "\n"
    "static const struct proc_ops ctr_touch_map_fops = {\n"
    "\t.proc_open\t= ctr_touch_map_open,\n"
    "\t.proc_read\t= seq_read,\n"
    "\t.proc_lseek\t= seq_lseek,\n"
    "\t.proc_release\t= single_release,\n"
    "\t.proc_write\t= ctr_touch_map_write,\n"
    "};\n"
    "\n"
)
PROC_BLOCK_NEW = PROC_BLOCK + PROBE_INSERT_OLD

PROBE_OLD = (
    "\tinput_set_abs_params(touch_hid->ts_dev, ABS_PRESSURE, 0, 255, 0, 0);\n"
    "\n"
    "\terr = input_register_device(touch_hid->ts_dev);\n"
)

PROBE_OLD = (
    "\tinput_set_abs_params(touch_hid->ts_dev, ABS_PRESSURE, 0, 255, 0, 0);\n"
    "\n"
    "\terr = input_register_device(touch_hid->ts_dev);\n"
)
PROBE_NEW = (
    "\tinput_set_abs_params(touch_hid->ts_dev, ABS_PRESSURE, 0, 255, 0, 0);\n"
    "\n"
    "\tproc_create(\"ctr_touch_map\", 0644, NULL, &ctr_touch_map_fops);\n"
    "\n"
    "\terr = input_register_device(touch_hid->ts_dev);\n"
)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-tsc-touch-map.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/tsc/touch.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    tsc/touch.c: runtime touch mapping already present")
        return 0

    for old, new, what in (
        (INCLUDE_OLD, INCLUDE_NEW, "includes"),
        (GLOBAL_OLD, GLOBAL_NEW, "mapping global"),
        (COORD_OLD, COORD_NEW, "coordinate computation"),
        (PROBE_INSERT_OLD, PROC_BLOCK_NEW, "proc file before probe"),
        (LOG_OLD, LOG_NEW, "tap log tag"),
        (LOG_ARGS_OLD, LOG_ARGS_NEW, "tap log args"),
        (PROBE_OLD, PROBE_NEW, "probe proc_create"),
    ):
        if old not in s:
            print(f"error: touch.c: {what} not found "
                  "(is fix-tsc-touchscreen-diag.py applied?)", file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    p.write_text(s, newline="")
    assert MARK in s
    print("    tsc/touch.c: runtime touch mapping (/proc/ctr_touch_map, default 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
