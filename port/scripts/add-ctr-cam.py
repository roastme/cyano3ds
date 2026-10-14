#!/usr/bin/env python3
"""Cameras: Android3DS/Octoblimp's MT9V113 capture driver.

Installs the wiring for port/kernel/drivers/platform/nintendo3ds/ctr_cam.c
(copied unchanged from github.com/Octoblimp/Android-Eclair-3DS, kernel patch
patches/linux.diff, GPL-2.0; apply-kernel-port.sh copies the file):

  * Kconfig CTR_CAM + Makefile line;
  * the cam@10120000 node in nintendo3ds.dtsi: both CAM receive ports
    (0x10120000 / 0x10121000), their pixel FIFO windows (0x10320000 /
    0x10321000), the Y2R block, CFG11_CAMERA_CNT and the two i2c buses the
    three sensors sit on (i2c1: internal 0x3d + outer right 0x3c, i2c2: outer
    left 0x3c).

Userspace gets /dev/ctr_cam: ioctl START {camera, width, height}, read() one
YUYV frame per call, STOP/STATS.  The driver header documents the protocol.

Idempotent.  Applies to the linux-3ds tree.
"""
import os
import pathlib
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
plat = kd / "drivers/platform/nintendo3ds"


def edit(path, mark, fn, what):
    s = path.read_text()
    if mark in s:
        print(f"    {what}: already done")
        return
    s2 = fn(s)
    if s2 == s or mark not in s2:
        raise SystemExit(f"error: {path}: could not apply ({what})")
    path.write_text(s2)
    print(f"    {what}: done")


kc = plat / "Kconfig"
def kc_fn(s):
    entry = '''
config CTR_CAM
	bool "Nintendo 3DS camera capture"
	depends on CTR_I2C
	default y
	help
	  Drive the three MT9V113 cameras (outer right and inner on i2c
	  bus 0, outer left on bus 1) and the two CAM receive ports, using
	  the register map GBATEK documents. Frames are read from the
	  receive FIFO by PIO and handed to userspace as YUYV through
	  /dev/ctr_cam (one frame per read(), START/STOP/STATS ioctls).
	  From Android3DS/Octoblimp.
'''
    i = s.rfind("endif")
    if i < 0:
        i = len(s)
    return s[:i] + entry.lstrip("\n") + "\n" + s[i:]
edit(kc, "config CTR_CAM", kc_fn, "nintendo3ds/Kconfig: CTR_CAM")

mk = plat / "Makefile"
edit(mk, "ctr_cam.o", lambda s: s.rstrip("\n") + "\nobj-$(CONFIG_CTR_CAM) += ctr_cam.o\n",
     "nintendo3ds/Makefile: ctr_cam.o")

dtsi = kd / "arch/arm/boot/dts/nintendo3ds.dtsi"
def dt_fn(s):
    anchor = "\t\t/* I2C blocks */\n"
    if anchor not in s:
        raise SystemExit(f"error: {dtsi}: anchor not found: {anchor!r}")
    node = '''\t\t/*
\t\t * Cameras (ctr_cam.c, from Android3DS/Octoblimp).  CAM0 0x10120000
\t\t * (internal + outer right sensors), CAM1 0x10121000 (outer left),
\t\t * each port's pixel FIFO window at 0x10320000 / 0x10321000, the
\t\t * Y2R block (unused) and CFG11_CAMERA_CNT bit0 (16 MHz camera
\t\t * clock).  The pdn byte shares a page with other PDN registers, so
\t\t * the driver maps it without requesting the region.  IRQs are the
\t\t * 3dbrew ARM11 numbers 0x48/0x49/0x4B minus 0x20.
\t\t */
\t\tcam: cam@10120000 {
\t\t\tcompatible = "nintendo,3ds-cam";
\t\t\treg = <0x10120000 0x1000>,
\t\t\t      <0x10102000 0x1000>,
\t\t\t      <0x10141224 0x4>,
\t\t\t      <0x10121000 0x1000>,
\t\t\t      <0x10320000 0x1000>,
\t\t\t      <0x10321000 0x1000>;
\t\t\treg-names = "registers", "y2r", "pdn", "registers-mirror",
\t\t\t\t    "fifo0", "fifo1";
\t\t\tinterrupts = <GIC_SPI 0x28 IRQ_TYPE_EDGE_RISING>,
\t\t\t\t     <GIC_SPI 0x29 IRQ_TYPE_EDGE_RISING>,
\t\t\t\t     <GIC_SPI 0x2B IRQ_TYPE_EDGE_RISING>;
\t\t\tinterrupt-names = "bus0", "bus1", "y2r";
\t\t\tnintendo,i2c-bus = <&i2c1>, <&i2c2>;
\t\t\tstatus = "okay";
\t\t};

'''
    return s.replace(anchor, node + anchor, 1)
edit(dtsi, "nintendo,3ds-cam", dt_fn, "nintendo3ds.dtsi: cam@10120000")
