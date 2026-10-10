#!/usr/bin/env python3
"""
fix-wifi-octoblimp.py - install the Octoblimp/Android3DS (AOSP Eclair) WiFi
stack into the Cyano3DS kernel tree.

WHY THIS REPLACES fix-wifi.py + fix-ath6kl*.py
----------------------------------------------
The Cyano3DS port drove the 3DS Atheros AR6014G with mainline cfg80211 ath6kl,
teaching it the "hw2" AR6002 family and Nintendo's NWM firmware piece by
piece.  That path reached a registered card and an uploaded firmware but never
a working station.

The Octoblimp port instead resurrected the pre-mainline **staging ath6k**
driver (the Atheros "AR6K" codebase, removed from the kernel around 3.2),
ported it to cfg80211 + WEXT, and taught it to boot the AR6014 directly from
Nintendo's own NWM blobs (`ath6k/AR6002/nwm/{stub_data,stub_code,main_type4,
database}.bin`) instead of the SDK `.z77` images.  That is the driver this
script installs, together with Octoblimp's SDIO host changes in ctr_sdhc.c
(AR6002 PIO edge fix, card-IRQ timer fallback, wifi_recover sysfs).

The driver is built in and probes at boot with the defaults Octoblimp used:
`fwmode = 1` (STA) and `wlaninitmode = WLAN_INIT_MODE_DRV`.

Everything it needs lives in this repository, copied from the Octoblimp tree
(the base kernel patch plus the AR6014/NWM patch series already applied):

    port/kernel/drivers/staging/ath6k_legacy/       the driver (new)
    port/kernel/drivers/platform/nintendo3ds/ctr_sdhc.{c,h}
    arch/arm/boot/dts/nintendo3ds.dtsi              the `nwm` node

It is idempotent: running it twice changes nothing.
"""

import pathlib
import os
import re
import shutil
import sys

PORT = pathlib.Path(__file__).resolve().parent.parent   # <repo>/port
PORT_KERNEL = PORT / "kernel"
DRIVER_SRC = PORT_KERNEL / "drivers/staging/ath6k_legacy"
SDHC_SRC = PORT_KERNEL / "drivers/platform/nintendo3ds"


def die(msg):
    sys.exit("fix-wifi-octoblimp.py: " + msg)


NWM_NODE = (
    "\t\tnwm: sdhc-controller@10122000 {\n"
    "\t\t\tcompatible = \"nintendo,3ds-sdhc\";\n"
    "\t\t\treg = <0x10122000 0x200>;\n"
    "\n"
    "\t\t\tinterrupts =\n"
    "\t\t\t\t<GIC_SPI 0x20 IRQ_TYPE_EDGE_RISING>,\n"
    "\t\t\t\t<GIC_SPI 0x21 IRQ_TYPE_EDGE_RISING>;\n"
    "\n"
    "\t\t\tclocks = <&sdclk>;\n"
    "\n"
    "\t\t\tcap-sdio-irq;\n"
    "\t\t\tcap-sd-highspeed;\n"
    "\t\t\t/* The AR6014G WiFi chip is permanently attached on this\n"
    "\t\t\t * controller, not a hot-pluggable card, so tell the MMC\n"
    "\t\t\t * core to probe it at boot instead of waiting for a\n"
    "\t\t\t * card-detect event that will never come. */\n"
    "\t\t\tnon-removable;\n"
    "\t\t\t/* GPIO4 bit0 (0x10147028) is the WiFi chip's power\n"
    "\t\t\t * enable line. */\n"
    "\t\t\twifi-enable-gpios = <&gpio4 0 GPIO_ACTIVE_HIGH>;\n"
    "\t\t};\n"
)


def patch_dtsi(kd):
    dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
    s = dtsi.read_text()
    m = re.search(r'\t\tnwm: sdhc-controller@10122000 \{\n(?:.*?\n)*?\t\t\};\n',
                  s)
    if not m:
        die("nintendo3ds.dtsi: nwm node not found")
    if m.group(0) == NWM_NODE:
        print("    dtsi: nwm node already canonical")
        return
    s = s[:m.start()] + NWM_NODE + s[m.end():]
    dtsi.write_text(s)
    print("    dtsi: nwm node replaced (interrupts/clocks/caps/wifi-enable)")


def install_tree(kd):
    dst = kd / "drivers/staging/ath6k_legacy"
    if not DRIVER_SRC.is_dir():
        die("missing ported driver source at %s" % DRIVER_SRC)
    shutil.copytree(DRIVER_SRC, dst, dirs_exist_ok=True)
    print("    ath6k_legacy: %d files installed" %
          sum(1 for _ in dst.rglob("*") if _.is_file()))

    for f in ("ctr_sdhc.c", "ctr_sdhc.h"):
        src = SDHC_SRC / f
        if not src.exists():
            die("missing ported source %s" % src)
        shutil.copy2(src, kd / "drivers/platform/nintendo3ds" / f)
    print("    ctr_sdhc.c/.h: Octoblimp SDIO host installed")


def wire_staging(kd):
    kc = kd / "drivers/staging/Kconfig"
    s = kc.read_text()
    if "staging/ath6k_legacy/Kconfig" not in s:
        s = s.replace('\nendif # STAGING\n',
                      '\nsource "drivers/staging/ath6k_legacy/Kconfig"\n\nendif # STAGING\n', 1)
        kc.write_text(s)
        print("    drivers/staging/Kconfig: added ath6k_legacy/Kconfig")
    else:
        print("    drivers/staging/Kconfig: already wired")

    mk = kd / "drivers/staging/Makefile"
    s = mk.read_text()
    if "ath6k_legacy/" not in s:
        if not s.endswith("\n"):
            s += "\n"
        s += "obj-$(CONFIG_ATH6K_LEGACY)\t\t+= ath6k_legacy/\n"
        mk.write_text(s)
        print("    drivers/staging/Makefile: added ath6k_legacy/")
    else:
        print("    drivers/staging/Makefile: already wired")


def add_sdio_id(kd):
    p = kd / "include/linux/mmc/sdio_ids.h"
    s = p.read_text()
    if "SDIO_DEVICE_ID_ATHEROS_AR6014" in s:
        return
    anchor = "#define SDIO_DEVICE_ID_ATHEROS_AR6005\t\t0x050A\n"
    if anchor not in s:
        die("sdio_ids.h: AR6005 anchor not found")
    s = s.replace(
        anchor,
        anchor +
        "#define SDIO_DEVICE_ID_ATHEROS_AR6014\t\t0x0506\t/* Nintendo 3DS AR6014G */\n",
        1)
    p.write_text(s)
    print("    sdio_ids.h: added SDIO_DEVICE_ID_ATHEROS_AR6014")


def remove_mainline_ath6kl_conflict(kd):
    """The legacy driver registers the same 'ath6kl' name; the mainline
    module is disabled in build-kernel.sh, so nothing to do here.  Kept as a
    hook so the intent is documented."""
    return


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.path.expanduser("~/p3ds/src/linux-3ds"))
    if not (kd / "Makefile").exists():
        die("no kernel tree at %s" % kd)

    patch_dtsi(kd)
    install_tree(kd)
    wire_staging(kd)
    add_sdio_id(kd)
    remove_mainline_ath6kl_conflict(kd)
    print("fix-wifi-octoblimp.py: done")


if __name__ == "__main__":
    main()
