#!/usr/bin/env python3
"""
add-ctr-gyro.py - install the New 3DS ITG-1010 gyroscope driver.

The New 3DS gyroscope is an InvenSense ITG-1010 on I2C bus 2 (0x10148000),
7-bit address 0x69 (GBATEK "2:D2h") or 0x68 ("2:D0h") depending on the AD0
strap.  The driver probes the other address when the node's one does not
answer.  It is not register compatible with the ITG-3200/ITG-3270 the
mainline itg3200 driver supports, so this installs
drivers/platform/nintendo3ds/ctr_gyro.c, wires it into Kconfig/Makefile and
adds the DT node under &i2c3.

Idempotent: running it twice changes nothing.

Usage: add-ctr-gyro.py [kernel-tree]
"""

import pathlib
import sys

PORT = pathlib.Path(__file__).resolve().parent.parent      # <repo>/port
DRV = PORT / "kernel/drivers/platform/nintendo3ds/ctr_gyro.c"
KD = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                  pathlib.Path.home() / "p3ds/src/linux-3ds")

KCONFIG_ENTRY = """
config CTR_GYRO
	tristate "Nintendo 3DS gyroscope (InvenSense ITG-1010)"
	depends on CTR_I2C
	depends on IIO
	default y
	help
	  The New 3DS gyroscope is an InvenSense ITG-1010 on I2C bus 2
	  (0x10148000, the IR/gyro bus) at 7-bit address 0x69 (GBATEK
	  "2:D2h") or 0x68 ("2:D0h"), selected by the AD0 strap; the
	  driver probes the other one if the DT address does not answer.
	  It is not register compatible with the ITG-3200/ITG-3270
	  used by the Old 3DS.  Exposes the three axes as an IIO angular
	  velocity device (in_anglvel_{x,y,z}_raw, in_anglvel_scale,
	  in_anglvel_en), which the Android sensors HAL polls.

"""

DTS_NODE = """
/* New 3DS gyroscope: InvenSense ITG-1010 on I2C bus 2 (0x10148000).
 * 7-bit address 0x69 (GBATEK "2:D2h", AD0 high) or 0x68 ("2:D0h");
 * ctr_gyro.c probes the other one when this one does not answer.
 */
&i2c3 {
	gyro@69 {
		compatible = "nintendo,3ds-gyro";
		reg = <0x69>;
	};
};
"""


def main():
    if not (KD / "Makefile").is_file():
        sys.exit(f"add-ctr-gyro: no kernel tree at {KD}")
    if not DRV.is_file():
        sys.exit(f"add-ctr-gyro: {DRV} missing")

    dst = KD / "drivers/platform/nintendo3ds/ctr_gyro.c"
    dst.write_bytes(DRV.read_bytes())
    print("    drivers/platform/nintendo3ds/ctr_gyro.c installed")

    kc = KD / "drivers/platform/nintendo3ds/Kconfig"
    s = kc.read_text()
    if "CTR_GYRO" not in s:
        i = s.rindex("\nendif")
        kc.write_text(s[:i] + KCONFIG_ENTRY + s[i:])
        print("    drivers/platform/nintendo3ds/Kconfig: added CTR_GYRO")
    else:
        print("    drivers/platform/nintendo3ds/Kconfig: CTR_GYRO already present")
        old_help = ('\t  (0x10148000, the IR/gyro bus) at 7-bit address 0x69 (GBATEK\n'
                    '\t  "2:D2h").  It is not register compatible with the ITG-3200/ITG-3270\n')
        new_help = ('\t  (0x10148000, the IR/gyro bus) at 7-bit address 0x69 (GBATEK\n'
                    '\t  "2:D2h") or 0x68 ("2:D0h"), selected by the AD0 strap; the\n'
                    '\t  driver probes the other one if the DT address does not answer.\n'
                    '\t  It is not register compatible with the ITG-3200/ITG-3270\n')
        if old_help in s:
            kc.write_text(s.replace(old_help, new_help, 1))
            print("    drivers/platform/nintendo3ds/Kconfig: CTR_GYRO text updated")

    mk = KD / "drivers/platform/nintendo3ds/Makefile"
    s = mk.read_text()
    if "CTR_GYRO" not in s:
        s += "obj-$(CONFIG_CTR_GYRO)\t+= ctr_gyro.o\n"
        mk.write_text(s)
        print("    drivers/platform/nintendo3ds/Makefile: added ctr_gyro.o")
    else:
        print("    drivers/platform/nintendo3ds/Makefile: ctr_gyro.o already listed")

    dtsi = KD / "arch/arm/boot/dts/nintendo3ds.dtsi"
    s = dtsi.read_text()
    if "gyro@69" in s:
        print("    dtsi: gyro@69 already present")
    else:
        if "&i2c3" not in s:
            sys.exit("add-ctr-gyro: nintendo3ds.dtsi has no &i2c3 label")
        dtsi.write_text(s + DTS_NODE)
        print("    dtsi: &i2c3 gyro@69 (ITG-1010) added")


if __name__ == "__main__":
    main()
