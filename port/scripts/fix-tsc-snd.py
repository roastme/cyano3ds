#!/usr/bin/env python3
"""fix-tsc-snd.py - wire the 3DS audio codec bring-up into the TSC driver.

The TSC2117/AIC3010 has to be initialised for sound *before* the touchscreen
child device is populated (the OEM codec module initialises sound first and
touch second, and the audio init soft-resets the chip).  ctr_tsc_probe() is the
one place that runs before its children, so the audio half of ctr_snd.c is
called from there through ctr_snd_codec_init(map).

Idempotent.  Usage: fix-tsc-snd.py <kernel-tree>
"""
import pathlib
import sys


def main():
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/ctr_tsc.c"
    s = p.read_text()
    orig = s

    decl = ("#ifdef CONFIG_CTR_SND\n"
            "int ctr_snd_codec_init(struct regmap *map);\n"
            "#endif\n")
    if "ctr_snd_codec_init" not in s:
        anchor = "#include <linux/of_platform.h>\n"
        if anchor not in s:
            sys.exit("ctr_tsc.c: of_platform.h include not found")
        s = s.replace(anchor, anchor + "\n" + decl, 1)

    call = ("#ifdef CONFIG_CTR_SND\n"
            "\t/* Sound first: the audio init soft-resets the codec, so it\n"
            "\t * has to run before the touchscreen child is populated. */\n"
            "\terr = ctr_snd_codec_init(map);\n"
            "\tif (err)\n"
            "\t\tdev_warn(dev, \"audio codec init failed: %d\\n\", err);\n"
            "#endif\n\n"
            "\treturn devm_of_platform_populate(dev);\n")
    old_ret = "\treturn devm_of_platform_populate(dev);\n"
    if "ctr_snd_codec_init(map)" not in s:
        if old_ret not in s:
            sys.exit("ctr_tsc.c: probe return not found")
        s = s.replace(old_ret, call, 1)

    if s != orig:
        p.write_text(s)
        print("    ctr_tsc.c: ctr_snd_codec_init() hooked into probe")
    else:
        print("    ctr_tsc.c: already hooked")


if __name__ == "__main__":
    main()
