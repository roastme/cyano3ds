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

The driver itself is not vendored in this repository.  install_tree() fetches
it from Octoblimp's Android-Eclair-3DS at a pinned commit (the kernel patch
series in patches/linux.diff, which adds drivers/staging/ath6k_legacy/), then
applies port/kernel/patches/ath6k_legacy-cyano3ds.patch on top: the Cyano3DS
changes (cfg80211 and HIF fixes, keep-power suspend, calibration).  The
Octoblimp base is pinned by OCTO_SHA, so the result is reproducible.

What still lives in this repository:

    port/kernel/patches/ath6k_legacy-cyano3ds.patch         Cyano3DS changes to the driver
    port/kernel/drivers/platform/nintendo3ds/ctr_sdhc.{c,h} Octoblimp's SDIO host

The `nwm` device-tree node is written by patch_dtsi() below.

It is idempotent: running it twice changes nothing.
"""

import pathlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

PORT = pathlib.Path(__file__).resolve().parent.parent   # <repo>/port
PORT_KERNEL = PORT / "kernel"
SDHC_SRC = PORT_KERNEL / "drivers/platform/nintendo3ds"
DRIVER_PATCH = PORT_KERNEL / "patches/ath6k_legacy-cyano3ds.patch"

OCTO_URL = "https://github.com/Octoblimp/Android-Eclair-3DS.git"
OCTO_SHA = "3227b806deb5bd1a3ba1a03fb4fc94a5bab23162"   # main, pinned
DRIVER_PATH = "drivers/staging/ath6k_legacy"
# Octoblimp's tree left a backup copy of ar6000_drv.c behind; it is not built.
DRIVER_SKIP = "os/linux/ar6000_drv.c.pre256"          # relative to DRIVER_PATH


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


def fetch_octo_driver(dst):
    """Extract drivers/staging/ath6k_legacy from Octoblimp's kernel patch
    series at OCTO_SHA into dst, then apply the Cyano3DS delta on top."""
    with tempfile.TemporaryDirectory(prefix="ath6k-") as tmp:
        tmp = pathlib.Path(tmp)
        octo = tmp / "octo"
        octo.mkdir()
        git = ["git", "-C", str(octo)]
        subprocess.run(git + ["init", "-q"], check=True)
        subprocess.run(git + ["fetch", "-q", "--depth=1", OCTO_URL, OCTO_SHA],
                       check=True)
        diff = subprocess.run(git + ["show", "FETCH_HEAD:patches/linux.diff"],
                              check=True, capture_output=True).stdout
        series = tmp / "linux.diff"
        series.write_bytes(diff)

        ext = tmp / "extract"
        ext.mkdir()
        subprocess.run(["git", "-C", str(ext), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(ext), "apply", "--whitespace=nowarn",
                        "--include=" + DRIVER_PATH + "/*", str(series)],
                       check=True)
        upstream = ext / DRIVER_PATH
        if not upstream.is_dir():
            die("Octoblimp patch series has no %s" % DRIVER_PATH)

        shutil.copytree(upstream, dst)
    (dst / DRIVER_SKIP).unlink(missing_ok=True)

    if not DRIVER_PATCH.exists():
        die("missing driver delta %s" % DRIVER_PATCH)
    subprocess.run(["patch", "-p1", "--batch", "-d", str(dst),
                    "-i", str(DRIVER_PATCH)], check=True,
                   stdout=subprocess.DEVNULL)


def install_tree(kd):
    dst = kd / DRIVER_PATH
    shutil.rmtree(dst, ignore_errors=True)
    fetch_octo_driver(dst)
    print("    ath6k_legacy: %d files installed (Octoblimp %s + delta)" %
          (sum(1 for _ in dst.rglob("*") if _.is_file()), OCTO_SHA[:12]))

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
