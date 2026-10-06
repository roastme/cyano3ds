#!/usr/bin/env python3
"""
fix-pxi-capture.py - export a manager-register write for the ARM9 capture

WHY THIS EXISTS
---------------
Option B (see port/arm9linuxfw/capture.c and ctr_lcd_fb.c) needs to tell the
ARM9 the physical address of a shared capture page exactly once.  The PXI
transport already has a manager register space (VPXI_RTYPE_MANAGER) that the
virtio device/queue/config paths never use, so a manager write is a free,
out-of-band channel for that single message.

This patch adds a tiny exported helper, ctr_pxi_mgr_write(), and remembers the
one pxi_host in a global so a driver that is not the virtio transport (the LCD
driver's ctr-diag kthread) can call it.

After the address arrives the ARM9 polls the page on its own timer; no further
PXI traffic is involved, so a later PXI/block-layer wedge cannot stop the
capture.

Idempotent: running it twice changes nothing.
"""

import sys
import pathlib

MARK = "ctr_cap_pxi"

ANCHOR = "static int ctr_pxi_probe(struct platform_device *pdev)\n"

FUNC = '''/*
 * ARM9 black-box capture (option B): hand the ARM9 the physical address of a
 * shared capture page through a manager register.  This is the only PXI
 * message in the capture path; after it the ARM9 polls the page itself (see
 * ctr_lcd_fb.c and port/arm9linuxfw/capture.c).
 */
static struct pxi_host *ctr_cap_pxi;

int ctr_pxi_mgr_write(u32 reg, u32 val)
{
	struct pxi_host *pxi = READ_ONCE(ctr_cap_pxi);

	if (!pxi)
		return -ENODEV;
	return vpxi_write_reg(pxi, 0, VPXI_REG_MANAGER(reg), val);
}
EXPORT_SYMBOL_GPL(ctr_pxi_mgr_write);

'''

OLD_DRVDATA = "\tplatform_set_drvdata(pdev, pxi);\n"
NEW_DRVDATA = ("\tplatform_set_drvdata(pdev, pxi);\n"
               "\tWRITE_ONCE(ctr_cap_pxi, pxi);\n")


def main():
    if len(sys.argv) < 2:
        print("usage: fix-pxi-capture.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    p = kd / "drivers/platform/nintendo3ds/ctr_pxi.c"
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        return 1

    with p.open("r", newline="") as f:
        s = f.read()

    if MARK in s:
        print("    ctr_pxi.c: manager-write capture hook already present")
        return 0

    if ANCHOR not in s:
        print("error: ctr_pxi.c: ctr_pxi_probe() not found", file=sys.stderr)
        return 1
    s = s.replace(ANCHOR, FUNC + ANCHOR, 1)

    if OLD_DRVDATA not in s:
        print("error: ctr_pxi.c: platform_set_drvdata() not found",
              file=sys.stderr)
        return 1
    s = s.replace(OLD_DRVDATA, NEW_DRVDATA, 1)

    p.write_text(s, newline="")
    assert "EXPORT_SYMBOL_GPL(ctr_pxi_mgr_write);" in s
    assert "WRITE_ONCE(ctr_cap_pxi, pxi);" in s
    print("    ctr_pxi.c: ctr_pxi_mgr_write() exported, host remembered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
