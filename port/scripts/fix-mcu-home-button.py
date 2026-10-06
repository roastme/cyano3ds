#!/usr/bin/env python3
"""
fix-mcu-home-button.py - give the physical Home button its own evdev.

The 3DS MCU reports the power, home and wireless buttons on one `gpio-keys`
node, so the kernel creates a single input device for all three.  The bring-up
`powerkey` daemon finds the device that can emit KEY_POWER and `EVIOCGRAB`s it
(exclusive open) so it reliably sees a short power press -- which means
Android's EventHub can no longer read that device, and the **Home button never
reaches the framework**.

Split the node in two: the power button keeps its own device (grabbed by
powerkey), while home + wireless land on a second device Android can read.

The Home button is mapped to **KEY_HOME**: Android treats it as "go to the
launcher" (leave the running app and show the home screen), which is what the
user asked for.  The MENU key now lives on **ZL** (see ctr_extrapad.c), so the
physical Home button is free to be HOME again.

Idempotent, and it will convert an already-split tree from KEY_MENU to
KEY_HOME.
"""

import pathlib
import re
import sys

MARK = "mcu_home_buttons"

SPLIT = """\tmcu_buttons {
\t\tcompatible = "gpio-keys";
\t\tinterrupt-parent = <&mcuintc>;

\t\tpower {
\t\t\tlinux,code = <KEY_POWER>;
\t\t\tinterrupts = <0 IRQ_TYPE_LEVEL_HIGH>;
\t\t\tlabel = "Power";
\t\t};
\t};

\t/*
\t * Home + wireless are on a separate input device on purpose: powerkey
\t * EVIOCGRABs the KEY_POWER device above, and if they shared it Android
\t * would never see the physical Home button.
\t */
\tmcu_home_buttons {
\t\tcompatible = "gpio-keys";
\t\tinterrupt-parent = <&mcuintc>;

\t\thome {
\t\t\tlinux,code = <KEY_HOME>;\t/* Home = Android HOME (launcher) */
\t\t\tinterrupts = <2 IRQ_TYPE_LEVEL_HIGH>;
\t\t\tlabel = "Home";
\t\t};

\t\twifi {
\t\t\tlinux,code = <KEY_WWAN>;
\t\t\tinterrupts = <4 IRQ_TYPE_LEVEL_HIGH>;
\t\t\tlabel = "Wireless";
\t\t};
\t};
"""

# already-split tree that (from the previous revision) says KEY_MENU
OLD_HOME_ANNOTATED = "\t\t\tlinux,code = <KEY_MENU>;\t/* Home = Android MENU (options menu) */"
OLD_HOME_PLAIN = "\t\t\tlinux,code = <KEY_MENU>;"
NEW_HOME = "\t\t\tlinux,code = <KEY_HOME>;\t/* Home = Android HOME (launcher) */"


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: fix-mcu-home-button.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    s = p.read_text()

    if MARK in s:
        if OLD_HOME_ANNOTATED in s:
            p.write_text(s.replace(OLD_HOME_ANNOTATED, NEW_HOME, 1))
            print("    dtsi: home button KEY_MENU -> KEY_HOME (Android launcher)")
        elif OLD_HOME_PLAIN in s and NEW_HOME not in s:
            p.write_text(s.replace(OLD_HOME_PLAIN, NEW_HOME, 1))
            print("    dtsi: home button KEY_MENU -> KEY_HOME (Android launcher)")
        else:
            print("    dtsi: mcu home button already split (KEY_HOME)")
        return 0

    pat = re.compile(r"\tmcu_buttons \{.*?\n\t\};\n", re.S)
    if not pat.search(s):
        print("error: dtsi: mcu_buttons node not found", file=sys.stderr)
        return 1

    p.write_text(pat.sub(SPLIT, s, count=1))
    print("    dtsi: mcu_buttons split -> power | home(=HOME)+wireless (separate evdev)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
