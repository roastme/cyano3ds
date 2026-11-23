#!/usr/bin/env python3
"""I2C: ctr_i2c.c must report a missing device (and recover the bus).

ctr_i2c.c looked at the ACK bit only after data bytes, so a read from an
address with no device returned FF and looked like a successful transfer, and a
NAK left the controller paused without a STOP.  That is why the gyroscope at
0x69 "probed" and then failed, and why i2cdetect showed devices that are not
there.  fix-i2c-ack.py patches the linux-3ds driver; the panel below runs it
against a tree built from its own pre-patch fragments.
"""
import importlib.util
import tempfile
from t3ds import *

fix = SCRIPTS / "fix-i2c-ack.py"
check(fix.is_file(), "scripts/fix-i2c-ack.py missing")
check("fix-i2c-ack.py" in read(SCRIPTS / "apply-kernel-port.sh"),
      "apply-kernel-port.sh does not call fix-i2c-ack.py")

if fix.is_file():
    spec = importlib.util.spec_from_file_location("fix_i2c_ack", fix)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    # Pre-patch file, assembled from the script's own OLD_* fragments.
    original = (mod.OLD_SELECT + mod.OLD_WRITE + mod.OLD_FUNC +
                "\nstatic int unused;\n" + mod.OLD_XFER)
    with tempfile.TemporaryDirectory() as td:
        kd = pathlib.Path(td)
        (kd / "drivers/platform/nintendo3ds").mkdir(parents=True)
        src = kd / "drivers/platform/nintendo3ds/ctr_i2c.c"
        src.write_text(original)
        for _ in range(2):
            r = run(["python3", str(fix), str(kd)])
            check(r.returncode == 0, "fix-i2c-ack.py failed: " + r.stderr.strip())
        patched = read(src)
        for probe in ("ctr_i2c_abort", "I2C_CNT_ERRACK", "I2C_CNT_PAUSE",
                      "I2C_CNT_LAST", "I2C_FUNC_SMBUS_EMUL", "-ENXIO"):
            check(probe in patched, f"patched ctr_i2c.c does not mention {probe}")
        check(patched.count("static int ctr_i2c_abort") == 1,
              "ctr_i2c_abort was duplicated (not idempotent)")
        check(mod.OLD_SELECT not in patched and mod.OLD_FUNC not in patched,
              "the pre-patch code is still in ctr_i2c.c")

# The on-device selftest must scan the IR/gyro bus, so a wrong address or a
# dead sensor is visible in CYANO3DS/selftest.txt instead of "no gyroscope".
st = norm(read(USERLAND / "tests/selftest.sh"))
check("motion sensors" in st, "selftest.sh has no motion-sensor section")
check("in_anglvel_x_raw" in st and "in_accel_x_raw" in st,
      "selftest.sh does not look for the IIO channels")
check("i2cdetect" in st and "i2cget" in st,
      "selftest.sh does not scan the I2C bus for the gyroscope")
check("sensors.nintendo3ds.so" in st,
      "selftest.sh does not check the sensors HAL")

# Runtime tools for that scan have to be in the initramfs, and the init dumps
# a scan of every i2c bus before Android starts, so one boot shows where a
# device answers even if the console is powered off before the self-test runs.
mkinit = read(SCRIPTS / "mkinitramfs.sh")
for applet in ("i2cget", "i2cdetect", "i2ctransfer"):
    check(applet in mkinit, f"mkinitramfs.sh does not install the {applet} applet")
check("/dev/i2c-*" in mkinit and "i2c-boot.txt" in mkinit,
      "mkinitramfs.sh does not dump the i2c buses at boot")

# Packing a system tree without the HAL must fail loudly (it used to produce an
# image with no sensors and no explanation).
bcm = norm(read(SCRIPTS / "build-cm7-source.sh"))
check("lib/hw/sensors.nintendo3ds.so" in bcm,
      "build-cm7-source.sh does not check for the sensors HAL")

done()
