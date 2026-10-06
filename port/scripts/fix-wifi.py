#!/usr/bin/env python3
"""
fix-wifi.py - bring up the Nintendo 3DS WiFi SDIO card (Atheros AR6014G).

The 3DS Wi-Fi is an Atheros AR6014G on a *second* SD host controller at
0x10122000 (the `nwm:` node; upstream ctr_sdhc.c drives it).

Two things stop Linux from even seeing the card:

  1. The chip is held in reset until **GPIO4 bit 0 (0x10147028)** is set to 1
     (GBATEK: "0 = Reset, need re-upload wifi firmware, 1 = On").  The
     bootloader leaves it at 0, so the SDIO bus enumerates nothing.
     GPIO4 is a direction-less output port; bgpio's bgpio_simple_dir_out
     handles it, so the ordinary gpiolib consumer API works.

  2. ctr_sdhc.c uses the controller's CARD_PRESENT bit for card detect and
     does not mark the (soldered, always-present) card non-removable, so the
     MMC core can skip probing entirely.

This script is idempotent and repairs the two earlier buggy states (the
"second reg" spelling, and the version that accidentally dropped `reg`).
It edits, in place:

  arch/arm/boot/dts/nintendo3ds.dtsi
      ensure the `nwm` node keeps reg = <0x10122000 0x200>, and add
      wifi-enable-gpios = <&gpio4 0 GPIO_ACTIVE_HIGH> and non-removable.

  drivers/platform/nintendo3ds/ctr_sdhc.c
      request + assert the WiFi enable GPIO (propagating -EPROBE_DEFER), mark
      the card non-removable, gate the CARD_PRESENT early-return on
      removability, print the controller state at probe, and report the real
      ioremap error instead of a hardcoded -ENOMEM.

  drivers/platform/nintendo3ds/ctr_sdhc.h
      add the gpio_desc `wifi_enable` field to struct ctr_sdhc.
"""

import re
import sys
import pathlib

def die(msg):
    sys.exit("fix-wifi.py: " + msg)

def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    if not (kd / "Makefile").exists():
        die("no kernel tree at %s" % kd)

    # ==================================================================
    # 1. device tree: keep `reg`, add wifi-enable-gpios + non-removable
    # ==================================================================
    dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
    s = dtsi.read_text()

    # repair the two earlier spellings
    if 'reg = <0x10122000 0x200>, <0x10147028 0x02>;' in s:
        s = s.replace('reg = <0x10122000 0x200>, <0x10147028 0x02>;',
                      'reg = <0x10122000 0x200>;', 1)
    if '\t\t\treg-names = "sdhc", "wifi-enable";\n' in s:
        s = s.replace('\t\t\treg-names = "sdhc", "wifi-enable";\n', '', 1)
    s = s.replace(
        "\t\t\t/* 0x10147028 = GPIO4, bit0 = WiFi enable (0=reset,\n"
        "\t\t\t * 1=on).  The chip is dead until this is set. */\n", '', 1)
    # old comment from the wifi-enable-gpios version (re-added below, once)
    s = s.replace(
        "\t\t\t/* GPIO4 bit0 (0x10147028) = WiFi enable: 0 = reset,\n"
        "\t\t\t * 1 = on.  The chip is dark until this is set. */\n", '', 1)

    m = re.search(r'(\t\tnwm: sdhc-controller@10122000 \{\n)(.*?)(\t\t\};)',
                  s, re.S)
    if not m:
        die("nintendo3ds.dtsi: nwm node not found")
    head, body, tail = m.group(1), m.group(2), m.group(3)
    changed = []

    # the reg property is mandatory or probe fails with "invalid resource"
    if 'reg = <0x10122000 0x200>;' not in body:
        body = re.sub(r'(\t\t\tcompatible = "nintendo,3ds-sdhc";\n)',
                      r'\1\t\t\treg = <0x10122000 0x200>;\n', body, count=1)
        changed.append("reg restored")

    if 'wifi-enable-gpios' not in body:
        body = body.replace(
            '\t\t\treg = <0x10122000 0x200>;\n',
            '\t\t\treg = <0x10122000 0x200>;\n'
            '\n\t\t\t/* GPIO4 bit0 (0x10147028) = WiFi enable:\n'
            '\t\t\t * 0 = reset, 1 = on. */\n'
            '\t\t\twifi-enable-gpios = <&gpio4 0 GPIO_ACTIVE_HIGH>;\n', 1)
        changed.append("wifi-enable-gpios")

    if 'non-removable' not in body:
        body = body.rstrip('\n') + '\n\n\t\t\tnon-removable;\n'
        changed.append("non-removable")

    if changed:
        s = s[:m.start()] + head + body + tail + s[m.end():]
        dtsi.write_text(s)
        print("    dtsi: " + ", ".join(changed))
    else:
        print("    dtsi: already patched")

    # ==================================================================
    # 2. ctr_sdhc.h: gpio_desc wifi_enable field
    # ==================================================================
    hdr = kd / "drivers/platform/nintendo3ds/ctr_sdhc.h"
    s = hdr.read_text()
    if "struct gpio_desc *wifi_enable;" in s:
        print("    ctr_sdhc.h: already patched")
    elif "void __iomem *wifi_enable;" in s:
        s = s.replace("void __iomem *wifi_enable;",
                      "struct gpio_desc *wifi_enable;", 1)
        hdr.write_text(s)
        print("    ctr_sdhc.h: migrated wifi_enable to gpio_desc")
    else:
        old = "\tstruct device *dev;\n\tvoid __iomem *regs;\n"
        if old not in s:
            die("ctr_sdhc.h: struct ctr_sdhc anchor not found")
        s = s.replace(old, old +
                      "\t/* GPIO4 bit0 (0x10147028): WiFi enable */\n"
                      "\tstruct gpio_desc *wifi_enable;\n", 1)
        hdr.write_text(s)
        print("    ctr_sdhc.h: wifi_enable field added")

    # ==================================================================
    # 3. ctr_sdhc.c
    # ==================================================================
    src = kd / "drivers/platform/nintendo3ds/ctr_sdhc.c"
    s = src.read_text()
    changed = []

    # 3a. include the gpio consumer API
    if "<linux/gpio/consumer.h>" not in s:
        s = s.replace("#include <linux/platform_device.h>\n",
                      "#include <linux/platform_device.h>\n"
                      "#include <linux/gpio/consumer.h>\n", 1)
        changed.append("gpio include")

    # 3b. report the real ioremap error (upstream hardcodes -ENOMEM, which
    #     hid the missing-`reg` failure as "error -12")
    old = ("\thost->regs = devm_platform_ioremap_resource(pdev, 0);\n"
           "\tif (IS_ERR(host->regs)) {\n"
           "\t\tret = -ENOMEM;\n"
           "\t\tgoto free_mmc;\n"
           "\t}\n")
    new = ("\thost->regs = devm_platform_ioremap_resource(pdev, 0);\n"
           "\tif (IS_ERR(host->regs)) {\n"
           "\t\tret = PTR_ERR(host->regs);\n"
           "\t\tgoto free_mmc;\n"
           "\t}\n")
    if new not in s:
        if old not in s:
            die("ctr_sdhc.c: ioremap error block not found")
        s = s.replace(old, new, 1)
        changed.append("real ioremap error")

    # 3c. card-detect early-return must not fire for a non-removable card
    new_chk = ("\tif (mmc_card_is_removable(host->mmc) &&\n"
               "\t    !(ioread16(host->regs + SDHC_IRQ_STAT) & "
               "SDHC_STAT_CARDPRESENT)) {\n")
    if new_chk not in s:
        old_chk = ("\tif (!(ioread16(host->regs + SDHC_IRQ_STAT) & "
                   "SDHC_STAT_CARDPRESENT)) {\n")
        if old_chk not in s:
            die("ctr_sdhc.c: CARDPRESENT check not found")
        s = s.replace(old_chk, new_chk, 1)
        changed.append("card-detect gated on removable")

    # 3d. mark the soldered card non-removable
    new_caps = ("\tmmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_SDIO_IRQ |\n"
                "\t\t    MMC_CAP_NONREMOVABLE;\n")
    if new_caps not in s:
        old_caps = "\tmmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_SDIO_IRQ;\n"
        if old_caps not in s:
            die("ctr_sdhc.c: mmc->caps assignment not found")
        s = s.replace(old_caps, new_caps, 1)
        changed.append("MMC_CAP_NONREMOVABLE")

    # 3e. request + assert the WiFi enable GPIO
    gpio_block = (
        "\t/* WiFi power: GPIO4 bit0 (0=reset, 1=on).  Without this the\n"
        "\t * AR6014G never answers and the SDIO bus stays empty.  gpio4 and\n"
        "\t * this controller are both device_initcall, so defer if the gpio\n"
        "\t * controller has not probed yet. */\n"
        "\thost->wifi_enable = devm_gpiod_get_optional(dev, \"wifi-enable\",\n"
        "\t\t\t\t\t\t    GPIOD_OUT_HIGH);\n"
        "\tif (IS_ERR(host->wifi_enable)) {\n"
        "\t\tret = PTR_ERR(host->wifi_enable);\n"
        "\t\tif (ret == -EPROBE_DEFER)\n"
        "\t\t\tgoto free_mmc;\n"
        "\t\tdev_warn(dev, \"wifi-enable gpio unavailable: %d\\n\", ret);\n"
        "\t\thost->wifi_enable = NULL;\n"
        "\t} else if (host->wifi_enable) {\n"
        "\t\tdev_info(dev, \"wifi-enable gpio asserted\\n\");\n"
        "\t} else {\n"
        "\t\tdev_warn(dev, \"no wifi-enable gpio; WiFi may stay off\\n\");\n"
        "\t}\n\n")
    if "devm_gpiod_get_optional" not in s:
        anchor = "\tmmc->ops = &ctr_sdhc_ops;\n"
        if anchor not in s:
            die("ctr_sdhc.c: mmc->ops anchor not found")
        s = s.replace(anchor, gpio_block + anchor, 1)
        changed.append("wifi-enable gpio")
    elif "EPROBE_DEFER" not in s:
        # old block without deferral
        old_gpio = re.search(
            r'\t/\* WiFi power: GPIO4 bit0.*?\n\t\}\n\n', s, re.S)
        if old_gpio:
            s = s[:old_gpio.start()] + gpio_block + s[old_gpio.end():]
            changed.append("propagate EPROBE_DEFER")

    # 3f. print the controller state after reset
    if "reset done, IRQ_STAT" not in s:
        old = ("\tctr_sdhc_reset(host);\n\n"
               "\tret = devm_request_threaded_irq(dev, platform_get_irq(pdev, 0),\n")
        new = ("\tctr_sdhc_reset(host);\n"
               "\tdev_info(dev, \"reset done, IRQ_STAT=%04x (CARDPRESENT=%d)\\n\",\n"
               "\t\t ioread16(host->regs + SDHC_IRQ_STAT),\n"
               "\t\t !!(ioread16(host->regs + SDHC_IRQ_STAT) & "
               "SDHC_STAT_CARDPRESENT));\n\n"
               "\tret = devm_request_threaded_irq(dev, platform_get_irq(pdev, 0),\n")
        if old not in s:
            die("ctr_sdhc.c: reset anchor not found")
        s = s.replace(old, new, 1)
        changed.append("post-reset controller state log")

    if changed:
        src.write_text(s)
        print("    ctr_sdhc.c: " + ", ".join(changed))
    else:
        print("    ctr_sdhc.c: already patched")

    print("fix-wifi.py: done")

if __name__ == "__main__":
    main()
