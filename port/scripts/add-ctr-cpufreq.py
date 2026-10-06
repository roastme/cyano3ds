#!/usr/bin/env python3
"""
add-ctr-cpufreq.py - install the New 3DS ARM11 CPU clock + cpufreq driver.

Adds drivers/platform/nintendo3ds/ctr_cpufreq.c (a common-clock provider for
PDN_LGR_SOCMODE plus a cpufreq driver on top of it) and wires it into the
device tree:

  * the old fixed 268 MHz "cpuclk" node becomes a real, settable clock backed
    by PDN_LGR_SOCMODE (0x10141300) / CFG11_SOCINFO (0x10140FFC);
  * a "twdclk" fixed-factor child (/2, the ARM11 MPCore PERIPHCLK) is added so
    that a CPU rate change reaches the TWD timer's clock notifier and the
    clockevent is re-programmed;
  * the TWD timer node is re-parented from refclk to twdclk.

The old 3DS is left alone (the clock provider refuses to register when
CFG11_SOCINFO says it is not a New 3DS).

Idempotent: running it twice changes nothing.
"""

import pathlib
import re
import shutil
import sys

PORT = pathlib.Path(__file__).resolve().parent.parent      # <repo>/port
DRV_SRC = PORT / "kernel/drivers/platform/nintendo3ds/ctr_cpufreq.c"

KCONFIG_ENTRY = """
config CTR_CPUFREQ
	tristate "Nintendo 3DS ARM11 CPU clock and cpufreq driver"
	depends on CPU_FREQ && COMMON_CLK
	select CPU_FREQ_GOV_PERFORMANCE
	default y
	help
	  Controls the New 3DS ARM11 clock through PDN_LGR_SOCMODE (268 MHz /
	  804 MHz on LGR2, 268 MHz / 536 MHz on LGR1) and exposes it to the
	  cpufreq core.  The clock is also a common-clock provider so that the
	  ARM11 MPCore TWD timer clock (CPU/2) is re-programmed on a rate
	  change and time does not run at the wrong speed.

"""

DTS_CPUCLK = """	/*
	 * ARM11 CPU clock.  On the New 3DS this is a runtime-selectable
	 * 268 MHz / 804 MHz (LGR2) or 268 MHz / 536 MHz (LGR1) clock driven
	 * through PDN_LGR_SOCMODE (0x10141300, a.k.a. CFG11_MPCORE_CLKCNT);
	 * ctr_cpufreq.c implements it.  The second register is CFG11_SOCINFO
	 * (0x10140FFC), which tells LGR1 from LGR2.  The interrupt is the PDN
	 * clock-change wake-up (IRQ 88); the upclock itself is done by
	 * firm_linux_loader, because the bootloader parks cores 1-3 in wfe and
	 * the SoC mode switch needs every powered-on core in wfi.
	 */
	cpuclk: cpu-clock@10141300 {
		compatible = "nintendo,3ds-cpuclk";
		reg = <0x10141300 0x4>, <0x10140FFC 0x4>;
		reg-names = "socmode", "socinfo";
		interrupts = <GIC_SPI 88 IRQ_TYPE_EDGE_RISING>;
		#clock-cells = <0>;
		clock-output-names = "3ds:cpu";
	};

	/*
	 * The ARM11 MPCore TWD timer is clocked from PERIPHCLK, which is the
	 * CPU clock / 2.  Modelled as a child of cpuclk so that a CPU rate
	 * change propagates here and the TWD driver's clock notifier
	 * re-programs the clockevent.
	 */
	twdclk: twd-clock {
		compatible = "fixed-factor-clock";
		clocks = <&cpuclk>;
		#clock-cells = <0>;
		clock-div = <2>;
		clock-mult = <1>;
		clock-output-names = "3ds:twd";
	};
"""


def patch_kconfig(kd: pathlib.Path) -> None:
    p = kd / "drivers/platform/nintendo3ds/Kconfig"
    s = p.read_text()
    if "config CTR_CPUFREQ" in s:
        print("    Kconfig: CTR_CPUFREQ already present")
        return
    anchor = "\nendif # NINTENDO3DS_PLATFORM_DEVICES"
    if anchor not in s:
        raise SystemExit("error: Kconfig: NINTENDO3DS_PLATFORM_DEVICES "
                         "endif anchor not found")
    s = s.replace(anchor, KCONFIG_ENTRY + anchor, 1)
    p.write_text(s)
    print("    Kconfig: added CTR_CPUFREQ")


def patch_makefile(kd: pathlib.Path) -> None:
    p = kd / "drivers/platform/nintendo3ds/Makefile"
    s = p.read_text()
    if "CTR_CPUFREQ" in s:
        print("    Makefile: ctr_cpufreq.o already present")
        return
    s = s.rstrip("\n") + "\n\nobj-$(CONFIG_CTR_CPUFREQ)\t+= ctr_cpufreq.o\n"
    p.write_text(s)
    print("    Makefile: added ctr_cpufreq.o")


def patch_dts(kd: pathlib.Path) -> None:
    p = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
    s = p.read_text()

    # Always normalise both nodes: remove any existing twdclk, then replace
    # the cpuclk node (old fixed-factor or an earlier provider version) with
    # the current pair.  This keeps repeated applies from stacking nodes.
    s = re.sub(r"\ttwdclk: twd-clock \{.*?\n\t\};\n", "", s, flags=re.S)
    pat = re.compile(
        r"\tcpuclk: (?:refclk268mhz|cpu-clock@10141300) \{.*?\n\t\};\n",
        re.S)
    if not pat.search(s):
        raise SystemExit("error: nintendo3ds.dtsi: cpuclk node not found")
    s = pat.sub(DTS_CPUCLK, s, count=1)
    print("    dtsi: cpuclk -> nintendo,3ds-cpuclk + twdclk")

    # Re-parent the TWD timer onto the (CPU/2) twdclk.
    m = re.search(r"\ttimer: twd-timer@17e00600 \{.*?\n\t\t\};\n", s, re.S)
    if not m:
        raise SystemExit("error: nintendo3ds.dtsi: twd-timer node not found")
    block = m.group(0)
    if "clocks = <&refclk>;" in block:
        s = s.replace(block, block.replace("clocks = <&refclk>;",
                                           "clocks = <&twdclk>;"), 1)
        print("    dtsi: twd-timer clocks -> twdclk")
    else:
        print("    dtsi: twd-timer already on twdclk")

    p.write_text(s)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: add-ctr-cpufreq.py <kernel-tree>", file=sys.stderr)
        return 2
    kd = pathlib.Path(sys.argv[1])
    if not (kd / "Makefile").exists():
        print(f"error: no kernel tree at {kd}", file=sys.stderr)
        return 1

    dst = kd / "drivers/platform/nintendo3ds/ctr_cpufreq.c"
    shutil.copyfile(DRV_SRC, dst)
    print(f"    installed {dst.relative_to(kd)}")

    patch_kconfig(kd)
    patch_makefile(kd)
    patch_dts(kd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
