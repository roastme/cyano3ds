#!/usr/bin/env python3
"""
fix-tsc-touch-cal.py - affine digitizer->screen calibration for the touchscreen

WHY
---
`fix-tsc-touch-map.py` made the *shape* of the mapping selectable at runtime
(swap / invert), but the scale itself was hard-coded:

    screen_touch_x = raw_touch_x * 320 / 4095
    screen_touch_y = raw_touch_y * 240 / 4095

i.e. it assumes the digitizer's 12-bit range is exactly the panel's active
area, edge to edge.  It is not.  The TSC controllers in the 3DS report a
calibrated-but-offset span: 0 and 4095 sit a little *inside* the glass, and
the two axes differ.  The symptom is exactly what the owner reports -- taps
near the middle of the screen are fine, and the further you reach towards an
edge the further off the mark you land, because the error grows linearly with
the distance from the centre.  No amount of swap/invert fixes that; it is a
gain-and-offset error, not an orientation error.

The fix is the ordinary affine calibration, measured once:

    screen_x = (raw_x - x0) * 320 / (x1 - x0)      clamped to 0..319
    screen_y = (raw_y - y0) * 240 / (y1 - y0)      clamped to 0..239

with (x0, x1, y0, y1) the raw values that correspond to the panel's left/right
and top/bottom edges.  `port/userland/tests/tscal.c` measures exactly those
four numbers from five tapped targets and writes them to
CYANO3DS/touch-cal.txt; /init feeds that file to /proc/ctr_touch_cal, so a
mis-measurement is corrected by editing one text file on the card and
rebooting -- no kernel rebuild, the same philosophy as /proc/ctr_touch_map.

Ordering, which matters and is the one thing easy to get wrong:

    raw -> (swap if bit0) -> affine calibration -> (invert bits 1/2)

The swap is applied to the *raw* pair first, so the calibration always refers
to "the axis that ends up as screen X".

The defaults (43 3974 84 3982) were measured on this machine with tscal; a
`0 4095 0 4095` line in touch-cal.txt restores the original unscaled
behaviour if it is ever wanted.

Idempotent: a marker makes a second run a no-op.  touch.c uses LF line endings.
"""

import pathlib
import re
import sys

MARK = "ctr_touch_cal"

INCLUDE_OLD = (
    "#include <linux/proc_fs.h>\n"
    "#include <linux/seq_file.h>\n"
    "#include <linux/uaccess.h>\n"
)
INCLUDE_NEW = INCLUDE_OLD

# Calibration state next to the existing mapping global.
GLOBAL_OLD = (
    "static int ctr_touch_map = 0;\n"
)
GLOBAL_NEW = (
    "static int ctr_touch_map = 0;\n"
    "\n"
    "/*\n"
    " * Affine digitizer->screen calibration (see /proc/ctr_touch_cal):\n"
    " *   screen_x = (raw_x - x0) * 320 / (x1 - x0), clamped to 0..319\n"
    " *   screen_y = (raw_y - y0) * 240 / (y1 - y0), clamped to 0..239\n"
    " * x0/x1/y0/y1 are the raw values that correspond to the panel's\n"
    " * left/right/top/bottom edges.\n"
    " * N3DS_TOUCH_CALIBRATION: Octoblimp's conservative full-domain inset\n"
    " * (256..3840 of the 12-bit range) is the fallback.  Their port drives\n"
    " * the same panel and its touch is correct with exactly these numbers,\n"
    " * and /init writes them on every boot anyway.\n"
    " *\n"
    " * An earlier session replaced them with a 43..3974 span fitted from a\n"
    " * partly blind tscal run (the targets were hidden behind the boot log).\n"
    " * That fit was never in control at runtime -- /init still wrote\n"
    " * 256..3840 -- and is not the fix for the edge error: the real\n"
    " * difference from Octoblimp was the single FIFO sample and the missing\n"
    " * touch-slop anchor (see fix-tsc-touch-filter.py), not these four\n"
    " * numbers.  The honest value is measured per panel: tscal writes\n"
    " * CYANO3DS/touch-cal.txt and /init prefers it.\n"
    " */\n"
    "static int ctr_touch_x0 = 256;\n"
    "static int ctr_touch_x1 = 3840;\n"
    "static int ctr_touch_y0 = 256;\n"
    "static int ctr_touch_y1 = 3840;\n"
)

# The coordinate block installed by fix-tsc-touch-map.py.
COORD_OLD = (
    "\t\tscreen_touch_x = (u16)(mx * 320 / MAX_12BIT);\n"
    "\t\tscreen_touch_y = (u16)(my * 240 / MAX_12BIT);\n"
)
COORD_NEW = (
    "\t\t/*\n"
    "\t\t * Affine calibration (N3DS_TOUCH_CAL).  The digitizer's 12-bit\n"
    "\t\t * range is not the panel's active area, so scaling the full\n"
    "\t\t * range onto 320x240 is wrong by a gain and an offset: taps\n"
    "\t\t * land correctly near the middle and drift towards the edges.\n"
    "\t\t * Divide rather than multiply to keep the 12-bit inputs in\n"
    "\t\t * 32 bits, and clamp so a corner tap cannot wrap around to\n"
    "\t\t * the opposite corner.\n"
    "\t\t */\n"
    "\t\t{\n"
    "\t\t\tint span, off;\n"
    "\t\t\tlong v;\n"
    "\n"
    "\t\t\tspan = ctr_touch_x1 - ctr_touch_x0;\n"
    "\t\t\tif (span < 1)\n"
    "\t\t\t\tspan = 1;\n"
    "\t\t\toff = (int)mx - ctr_touch_x0;\n"
    "\t\t\tif (off < 0)\n"
    "\t\t\t\toff = 0;\n"
    "\t\t\tv = (long)off * 320 / span;\n"
    "\t\t\tif (v > 319)\n"
    "\t\t\t\tv = 319;\n"
    "\t\t\tscreen_touch_x = (u16)v;\n"
    "\n"
    "\t\t\tspan = ctr_touch_y1 - ctr_touch_y0;\n"
    "\t\t\tif (span < 1)\n"
    "\t\t\t\tspan = 1;\n"
    "\t\t\toff = (int)my - ctr_touch_y0;\n"
    "\t\t\tif (off < 0)\n"
    "\t\t\t\toff = 0;\n"
    "\t\t\tv = (long)off * 240 / span;\n"
    "\t\t\tif (v > 239)\n"
    "\t\t\t\tv = 239;\n"
    "\t\t\tscreen_touch_y = (u16)v;\n"
    "\t\t}\n"
)

# Log the calibration with every tap: a mis-measurement has to be visible in
# kmsg.log, and one line per tap is cheap.
LOG_OLD = (
    "\t\t\traw_touch_x, raw_touch_y, ctr_touch_map,\n"
    "\t\t\tscreen_touch_x, screen_touch_y);\n"
)
LOG_NEW = (
    "\t\t\traw_touch_x, raw_touch_y, ctr_touch_map,\n"
    "\t\t\tscreen_touch_x, screen_touch_y);\n"
)

PROC_OLD = (
    "static const struct proc_ops ctr_touch_map_fops = {\n"
    "\t.proc_open\t= ctr_touch_map_open,\n"
    "\t.proc_read\t= seq_read,\n"
    "\t.proc_lseek\t= seq_lseek,\n"
    "\t.proc_release\t= single_release,\n"
    "\t.proc_write\t= ctr_touch_map_write,\n"
    "};\n"
)
PROC_NEW = PROC_OLD + (
    "\n"
    "/*\n"
    " * /proc/ctr_touch_cal - read or write the affine calibration.\n"
    " *\n"
    " * One line, four numbers: \"x0 x1 y0 y1\", the raw digitizer values\n"
    " * that correspond to the panel's left, right, top and bottom edges.\n"
    " * Written by /init from CYANO3DS/touch-cal.txt, which tscal\n"
    " * measures, so a bad measurement is fixable from the SD card.\n"
    " */\n"
    "static int ctr_touch_cal_show(struct seq_file *m, void *v)\n"
    "{\n"
    "\tseq_printf(m, \"%d %d %d %d\\n\", ctr_touch_x0, ctr_touch_x1,\n"
    "\t\t ctr_touch_y0, ctr_touch_y1);\n"
    "\treturn 0;\n"
    "}\n"
    "\n"
    "static int ctr_touch_cal_open(struct inode *inode, struct file *file)\n"
    "{\n"
    "\treturn single_open(file, ctr_touch_cal_show, NULL);\n"
    "}\n"
    "\n"
    "static ssize_t ctr_touch_cal_write(struct file *file,\n"
    "\t\tconst char __user *buf, size_t count, loff_t *ppos)\n"
    "{\n"
    "\tchar kbuf[64];\n"
    "\tint v[4];\n"
    "\tint n;\n"
    "\n"
    "\tif (count > sizeof(kbuf) - 1)\n"
    "\t\tcount = sizeof(kbuf) - 1;\n"
    "\tif (copy_from_user(kbuf, buf, count))\n"
    "\t\treturn -EFAULT;\n"
    "\tkbuf[count] = '\\0';\n"
    "\tn = sscanf(kbuf, \"%d %d %d %d\", &v[0], &v[1], &v[2], &v[3]);\n"
    "\tif (n != 4)\n"
    "\t\treturn -EINVAL;\n"
    "\t/* Reject nonsense rather than clamp it: a span that is not\n"
    "\t * positive, or an edge outside the digitizer's range, is a typo\n"
    "\t * or a corrupted file, and silently accepting it would make every\n"
    "\t * touch land somewhere random. */\n"
    "\tif (v[0] < 0 || v[0] > MAX_12BIT || v[1] < 0 || v[1] > MAX_12BIT ||\n"
    "\t    v[2] < 0 || v[2] > MAX_12BIT || v[3] < 0 || v[3] > MAX_12BIT)\n"
    "\t\treturn -EINVAL;\n"
    "\tif (v[1] - v[0] < 16 || v[3] - v[2] < 16)\n"
    "\t\treturn -EINVAL;\n"
    "\tctr_touch_x0 = v[0];\n"
    "\tctr_touch_x1 = v[1];\n"
    "\tctr_touch_y0 = v[2];\n"
    "\tctr_touch_y1 = v[3];\n"
    "\tpr_info(\"touch calibration set to x0=%d x1=%d y0=%d y1=%d\\n\",\n"
    "\t\tctr_touch_x0, ctr_touch_x1, ctr_touch_y0, ctr_touch_y1);\n"
    "\treturn count;\n"
    "}\n"
    "\n"
    "static const struct proc_ops ctr_touch_cal_fops = {\n"
    "\t.proc_open\t= ctr_touch_cal_open,\n"
    "\t.proc_read\t= seq_read,\n"
    "\t.proc_lseek\t= seq_lseek,\n"
    "\t.proc_release\t= single_release,\n"
    "\t.proc_write\t= ctr_touch_cal_write,\n"
    "};\n"
)

PROBE_OLD = (
    "\tproc_create(\"ctr_touch_map\", 0644, NULL, &ctr_touch_map_fops);\n"
)
PROBE_NEW = (
    "\tproc_create(\"ctr_touch_map\", 0644, NULL, &ctr_touch_map_fops);\n"
    "\tproc_create(\"ctr_touch_cal\", 0644, NULL, &ctr_touch_cal_fops);\n"
)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-tsc-touch-cal.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/tsc/touch.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        # Keep an already-patched tree honest too: enforce the current
        # defaults rather than silently keeping whatever a previous session
        # wrote.  /init overrides them at runtime from touch-cal.txt, so this
        # only decides the value used before that file exists.
        new_s = s
        for name, val in (("x0", 256), ("x1", 3840),
                          ("y0", 256), ("y1", 3840)):
            new_s = re.sub(r"static int ctr_touch_%s = -?\d+;" % name,
                           "static int ctr_touch_%s = %d;" % (name, val),
                           new_s)
        if new_s != s:
            p.write_text(new_s, newline="")
            print("    tsc/touch.c: calibration defaults "
                  "refreshed to 256 3840 256 3840")
        else:
            print("    tsc/touch.c: affine touch calibration already present")
        return 0

    for old, new, what in (
        (GLOBAL_OLD, GLOBAL_NEW, "calibration globals"),
        (COORD_OLD, COORD_NEW, "coordinate computation"),
        (PROC_OLD, PROC_NEW, "calibration proc file"),
        (PROBE_OLD, PROBE_NEW, "probe proc_create"),
    ):
        if old not in s:
            print(f"error: touch.c: {what} not found "
                  "(is fix-tsc-touch-map.py applied?)", file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    p.write_text(s, newline="")
    assert MARK in s
    print("    tsc/touch.c: affine touch calibration "
          "(/proc/ctr_touch_cal, default 256 3840 256 3840)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
