#!/usr/bin/env python3
"""
fix-buttons.py - map the 3DS buttons to Android keycodes for the port

WHY
---
drivers/platform/nintendo3ds/ctr_gpio.c exposes the HID PAD register as a
16-line gpiochip, and the device tree's gpio-keys-polled `hid_buttons` node
turns 12 of those lines into key events.  The stock codes (BTN_A, BTN_B,
BTN_START, ...) have no entry in Android's /system/usr/keylayout/qwerty.kl, so
EventHub maps them to keycode 0 and Android drops them.

The mapping is chosen so the **3DS face buttons are normal Android system
keys** (A = OK, B = back) and the New-3DS shoulder buttons keep the volume
keys:

    D-pad              -> KEY_UP/DOWN/LEFT/RIGHT -> DPAD_UP/DOWN/LEFT/RIGHT
    A (hid 0)          -> 232  DPAD_CENTER   -> Android OK / select
    B (hid 1)          -> 158  BACK          -> Android back
    Select (hid 2)     ->  15  TAB
    Start (hid 3)      ->  28  ENTER
    R / trig_right (8) -> 115  VOLUME_UP
    L / trig_left (9)  -> 114  VOLUME_DOWN
    X (hid 10)         ->  26  LEFT_BRACKET
    Y (hid 11)         ->  27  RIGHT_BRACKET

The physical Home button is handled by fix-mcu-home-button.py (KEY_HOME).

ZL/ZR are NOT here: they are not in the HID_PAD register at all (on the New
3DS bits 14/15 of that register are the IRQ-enable/condition bits).  They are
read from the I2C device 2:0x2A (0x10148000) by the ctr_extrapad driver
instead.  ZL reports KEY_MENU and ZR reports KEY_SEARCH there.

Why not BTN_A/BTN_B?  qwerty.kl is the only keylayout the SDK ships, and only
its names (DPAD_CENTER, SPACE, TAB, ENTER, SEARCH, BACK, ...) can be mapped.
The Android gamepad KEYCODE_BUTTON_* names do not exist in the API-10 keylayout
table, so they cannot be used.

Why these particular keys: they are the stock Android navigation set, so a
key press is handled by the framework/apps the normal way (no injection or
forced launch).  We avoid alphabetic keys so EventHub does not reclassify the
pad as an alphabetic keyboard (which would change Configuration.keyboard to
QWERTY and suppress the soft input method in every other app).

The 16-bit HID PAD register bits are (3dbrew / libctru KEY_*):
  0 A, 1 B, 2 Select, 3 Start, 4 Right, 5 Left, 6 Up, 7 Down,
  8 R, 9 L, 10 X, 11 Y, 14 ZL, 15 ZR  (ZL/ZR are New-3DS only).

Idempotent: the whole `hid_buttons` node is rebuilt every run.
"""

import sys
import pathlib


# (node name, linux,code, hid gpio line, label, comment)
BUTTONS = [
    ("a",           232, 0,  "A",             "Android key mapping: DPAD_CENTER (OK / select)"),
    ("b",           158, 1,  "B",             "Android key mapping: BACK"),
    ("select",      15,  2,  "Select",        "Android key mapping: TAB"),
    ("start",       28,  3,  "Start",         "Android key mapping: ENTER"),
    ("right",       "KEY_RIGHT", 4, "Right",  None),
    ("left",        "KEY_LEFT",  5, "Left",   None),
    ("up",          "KEY_UP",    6, "Up",     None),
    ("down",        "KEY_DOWN",  7, "Down",   None),
    ("trig_right",  115, 8,  "Right trigger", "Android key mapping: VOLUME_UP"),
    ("trig_left",   114, 9,  "Left trigger",  "Android key mapping: VOLUME_DOWN"),
    ("x",           26,  10, "X",             "Android key mapping: LEFT_BRACKET"),
    ("y",           27,  11, "Y",             "Android key mapping: RIGHT_BRACKET"),
]


def build_block():
    out = ["\thid_buttons {\n",
           '\t\tcompatible = "gpio-keys-polled";\n',
           "\t\tpoll-interval = <10>;\n",
           "\n"]
    for name, code, line, label, comment in BUTTONS:
        out.append("\t\t%s {\n" % name)
        if comment:
            out.append("\t\t\tlinux,code = <%s>;\t/* %s */\n" % (code, comment))
        else:
            out.append("\t\t\tlinux,code = <%s>;\n" % code)
        out.append("\t\t\tgpios = <&hid %d GPIO_ACTIVE_LOW>;\n" % line)
        out.append('\t\t\tlabel = "%s";\n' % label)
        out.append("\t\t};\n\n")
    out.append("\t};\n")
    return "".join(out)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-buttons.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    lines = p.read_text().splitlines(keepends=True)

    start = end = None
    for i, ln in enumerate(lines):
        if ln.strip() == "hid_buttons {":
            start = i
            for j in range(i + 1, len(lines)):
                if lines[j].rstrip("\n") == "\t};":
                    end = j
                    break
            break
    if start is None or end is None:
        print("error: dtsi: hid_buttons node not found", file=sys.stderr)
        return 1

    lines[start:end + 1] = [build_block()]
    p.write_text("".join(lines))
    print("    dtsi: hid_buttons -> A=DPAD_CENTER(OK) B=BACK L=VOLUME_DOWN R=VOLUME_UP; "
          "ZL unmapped, ZR=SEARCH (ctr_extrapad)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
