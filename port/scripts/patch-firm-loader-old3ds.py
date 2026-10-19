#!/usr/bin/env python3
"""
patch-firm-loader-old3ds.py - make firm_linux_loader behave like an OLD 3DS on
every console, for the CyanogenMod 6 (Froyo) build.

Upstream firm_linux_loader, on a New 3DS/2DS XL:
  * loads linux/nintendo3ds_ktr.dtb (256 MB FCRAM, four cores);
  * in enable_cores23() powers up cores 2/3 and switches the SoC to 804 MHz.

CM6 is built for the CTR family (128 MB, one 268 MHz core), so with this patch
the loader
  * always loads linux/nintendo3ds_ctr.dtb (128 MB, maxcpus=1);
  * leaves cores 2/3 powered off and the SoC in its 268 MHz boot mode.

Run it on a COPY of the loader tree (build-loader.sh does): it is the opposite
of patch-firm-loader-clock.py, which the CM7 build uses.  Idempotent.
"""

import pathlib
import sys

MARK = "Cyano3DS CM6"


def patch(path: pathlib.Path, old: str, new: str) -> None:
    s = path.read_text()
    if MARK in s and new in s:
        print(f"    {path.name}: already patched")
        return
    if old not in s:
        raise SystemExit(f"error: {path}: expected block not found")
    path.write_text(s.replace(old, new, 1))
    print(f"    {path.name}: patched")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: patch-firm-loader-old3ds.py <firm_linux_loader-dir>",
              file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])

    patch(root / "arm9/source/main.c",
          "dtb_filename = is_lgr() ? KTR_DTB_FILENAME : CTR_DTB_FILENAME;",
          "/* Cyano3DS CM6: always the old-3DS tree (128 MB, one 268 MHz core) */\n"
          "\tdtb_filename = CTR_DTB_FILENAME;")

    patch(root / "arm11/source/smp.c",
          "void enable_cores23(void)\n{\n\tif (!is_lgr())\n\t\treturn;\n",
          "void enable_cores23(void)\n{\n"
          "\t/* Cyano3DS CM6: no cores 2/3 and no 804 MHz, even on a New 3DS */\n"
          "\treturn;\n\n\tif (!is_lgr())\n\t\treturn;\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
