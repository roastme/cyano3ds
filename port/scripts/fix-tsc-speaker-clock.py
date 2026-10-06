#!/usr/bin/env python3
"""
fix-tsc-speaker-clock.py - enable CFG11_SPEAKER_CNT.bit1 before the TSC probe

WHY
---
GBATEK, CONFIG11 registers:

    10141220h - CFG11_SPEAKER_CNT (R/W)
      0     Unknown (R/W)
      1     Audio Clock TP152 (Speaker PWM?) (0=Off/Muted Sound, 1=8MHz/16MHz On)
    "Should be set to 02h. Bit1=1 is required for Audio and Touchscreen."

NATIVE_FIRM's codec module sets this during normal boot, and libn3ds/
fastboot3DS' CODEC_init() does `*((vu8*)0x10141220) = 2;` (the "General codec
reset + init" step).  This port never runs NATIVE_FIRM: fastboot3DS only
comments out its `CODEC_init()` call and `firm_linux_loader` touches neither
CFG11 nor the codec, and the linux-3ds `ctr_tsc`/`touch` drivers never write
the register either.  If bit1 is left clear the codec chip's clock is off,
the TSC's ADC never runs and the 52-byte FIFO always reads 0xFFFF -- a dead
touchscreen (and circle pad) while display, buttons and everything else work.

Set bit1 (preserving bit0) from the TSC driver probe, before the chip is
reset/configured.  Harmless if the boot chain already set it (we only OR it).

Idempotent: marker-guarded.  Uses LF line endings (ctr_tsc.c is upstream).
"""

import sys
import pathlib


MARK = "CFG11_SPEAKER_CNT"

INCLUDE_OLD = "#include <linux/delay.h>\n#include <linux/kernel.h>\n"
INCLUDE_NEW = "#include <linux/delay.h>\n#include <linux/io.h>\n#include <linux/kernel.h>\n"

FUNC_ANCHOR = (
    "static int ctr_tsc_sw_reset(struct ctr_tsc *cdc)\n"
)

FUNC_NEW = (
    "#define CTR_CFG11_SPEAKER_CNT\t0x10141220\n"
    "\n"
    "/*\n"
    " * CFG11_SPEAKER_CNT.bit1 gates the codec chip clock; GBATEK: \"Bit1=1 is\n"
    " * required for Audio and Touchscreen\".  NATIVE_FIRM's codec module sets it,\n"
    " * but this port boots straight from fastboot3DS (which never runs\n"
    " * CODEC_init) through firm_linux_loader (which touches no CFG11/codec\n"
    " * register), so if the bit is clear the TSC ADC never runs and the FIFO\n"
    " * always reads 0xFFFF -> dead touchscreen.  Enable it here; OR-in is a no-op\n"
    " * when the boot chain already did it.\n"
    " */\n"
    "static void ctr_tsc_enable_codec_clock(struct device *dev)\n"
    "{\n"
    "\tvoid __iomem *reg;\n"
    "\tu8 val;\n"
    "\n"
    "\treg = devm_ioremap(dev, CTR_CFG11_SPEAKER_CNT, 1);\n"
    "\tif (!reg) {\n"
    "\t\tdev_warn(dev, \"cannot map CFG11_SPEAKER_CNT\\n\");\n"
    "\t\treturn;\n"
    "\t}\n"
    "\n"
    "\tval = readb(reg);\n"
    "\tif (val & BIT(1)) {\n"
    "\t\tdev_info(dev, \"CFG11_SPEAKER_CNT=%02x (codec clock already on)\\n\", val);\n"
    "\t} else {\n"
    "\t\twriteb(val | BIT(1), reg);\n"
    "\t\tdev_info(dev, \"CFG11_SPEAKER_CNT %02x -> %02x (codec/touchscreen clock on)\\n\",\n"
    "\t\t\t val, readb(reg));\n"
    "\t}\n"
    "}\n"
    "\n"
)

PROBE_OLD = (
    "\tcdc->dev = dev;\n"
    "\tcdc->spi = spi;\n"
    "\tcdc->banksel = -1; /* don't assume any selected bank by default */\n"
    "\n"
    "\t/* reset the chip into a known-good state */\n"
)
PROBE_NEW = (
    "\tcdc->dev = dev;\n"
    "\tcdc->spi = spi;\n"
    "\tcdc->banksel = -1; /* don't assume any selected bank by default */\n"
    "\n"
    "\tctr_tsc_enable_codec_clock(dev);\n"
    "\n"
    "\t/* reset the chip into a known-good state */\n"
)


def main():
    if len(sys.argv) < 2:
        print("usage: fix-tsc-speaker-clock.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/ctr_tsc.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    tsc/ctr_tsc.c: CFG11_SPEAKER_CNT codec clock already enabled")
        return 0

    for old, new, what in (
        (INCLUDE_OLD, INCLUDE_NEW, "includes"),
        (FUNC_ANCHOR, FUNC_NEW + FUNC_ANCHOR, "sw_reset anchor"),
        (PROBE_OLD, PROBE_NEW, "probe"),
    ):
        if old not in s:
            print(f"error: ctr_tsc.c: {what} not found (unexpected version)",
                  file=sys.stderr)
            return 1
        s = s.replace(old, new, 1)

    p.write_text(s, newline="")
    assert MARK in s
    print("    tsc/ctr_tsc.c: CFG11_SPEAKER_CNT.bit1 (codec/touchscreen clock) enabled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
