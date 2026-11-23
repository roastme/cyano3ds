#!/usr/bin/env python3
"""Motion sensors: IIO gyro driver + Android sensors HAL.

Checks that the port installs the New 3DS ITG-1010 driver, the DT node and the
CTR_GYRO config, and that the device product ships the sensors HAL and the
accelerometer/gyroscope feature files.  The idempotence test runs
add-ctr-gyro.py twice against a throw-away kernel tree.
"""
import tempfile
from t3ds import *

driver = PORT / "kernel/drivers/platform/nintendo3ds/ctr_gyro.c"
check(driver.is_file(), "kernel/drivers/platform/nintendo3ds/ctr_gyro.c missing")
if driver.is_file():
    s = read(driver)
    for probe in ("CTR_GYRO_REG_WHO_AM_I", "0x75", "0x43", "0x69",
                  "in_anglvel", "IIO_ANGL_VEL", "CTR_GYRO_SCALE_NANO"):
        check(probe in s, f"ctr_gyro.c does not mention {probe}")

add = read(SCRIPTS / "add-ctr-gyro.py")
check("KCONFIG_ENTRY" in add and "CTR_GYRO" in add,
      "add-ctr-gyro.py does not define the Kconfig entry")
check("gyro@69" in add and "&i2c3" in add,
      "add-ctr-gyro.py does not add the &i2c3 gyro node")
check("add-ctr-gyro.py" in read(SCRIPTS / "apply-kernel-port.sh"),
      "apply-kernel-port.sh does not call add-ctr-gyro.py")

bk = read(SCRIPTS / "build-kernel.sh")
check("--enable  CTR_GYRO" in bk, "build-kernel.sh does not enforce CONFIG_CTR_GYRO")
check("--enable  IIO" in bk, "build-kernel.sh does not enforce CONFIG_IIO")
defcfg = read(PORT / "kernel/configs/nintendo3ds_android_defconfig")
check("CONFIG_CTR_GYRO=y" in defcfg, "the committed defconfig lacks CONFIG_CTR_GYRO=y")
check("CONFIG_IIO=y" in defcfg, "the committed defconfig lacks CONFIG_IIO=y")

# --- HAL and device product ---
hal = USERLAND / "device/nintendo3ds/sensors"
halsrc = hal / "sensors_nintendo3ds.c"
check(halsrc.is_file(), "device/nintendo3ds/sensors/sensors_nintendo3ds.c missing")
if halsrc.is_file():
    s = read(halsrc)
    for probe in ("SENSORS_HARDWARE_MODULE_ID", "SENSOR_TYPE_ACCELEROMETER",
                  "SENSOR_TYPE_GYROSCOPE", "in_accel", "in_anglvel",
                  "HAL_MODULE_INFO_SYM", "map_accel", "map_gyro"):
        check(probe in s, f"the sensors HAL does not mention {probe}")
check((hal / "Android.mk").is_file(), "sensors/Android.mk missing")
check("LOCAL_MODULE := sensors.nintendo3ds" in read(hal / "Android.mk"),
      "sensors/Android.mk does not build sensors.nintendo3ds")

dev = read(USERLAND / "device/nintendo3ds/device_nintendo3ds.mk")
check("sensors.nintendo3ds" in dev, "the device product does not ship sensors.nintendo3ds")
for feature in ("android.hardware.sensor.accelerometer.xml",
                "android.hardware.sensor.gyroscope.xml"):
    check(feature in dev, f"the device product does not copy {feature}")

# The HAL runs in system_server (uid system), but IIO's *_en attributes are
# root-only; the boot init must open them up.
mkinit = read(SCRIPTS / "mkinitramfs.sh")
check("in_*_en" in mkinit and "chmod 0666" in mkinit,
      "mkinitramfs.sh does not make the IIO enable attributes writable by system_server")

# --- idempotence of the kernel-tree installer ---
with tempfile.TemporaryDirectory() as td:
    kd = pathlib.Path(td)
    (kd / "drivers/platform/nintendo3ds").mkdir(parents=True)
    (kd / "arch/arm/boot/dts").mkdir(parents=True)
    (kd / "Makefile").write_text("# fake kernel\n")
    (kd / "drivers/platform/nintendo3ds/Kconfig").write_text(
        "if NINTENDO3DS\n\nendif\n")
    (kd / "drivers/platform/nintendo3ds/Makefile").write_text("")
    (kd / "arch/arm/boot/dts/nintendo3ds.dtsi").write_text(
        "&i2c3 {\n\textrapad@2a { };\n};\n")
    for _ in range(2):
        r = run(["python3", str(SCRIPTS / "add-ctr-gyro.py"), str(kd)])
        check(r.returncode == 0, "add-ctr-gyro.py failed: " + r.stderr.strip())
    check((kd / "drivers/platform/nintendo3ds/ctr_gyro.c").is_file(),
          "add-ctr-gyro.py did not install ctr_gyro.c")
    kc = read(kd / "drivers/platform/nintendo3ds/Kconfig")
    check(kc.count("config CTR_GYRO") == 1, "CTR_GYRO Kconfig entry was duplicated")
    mk = read(kd / "drivers/platform/nintendo3ds/Makefile")
    check(mk.count("ctr_gyro.o") == 1, "ctr_gyro.o Makefile line was duplicated")
    dtsi = read(kd / "arch/arm/boot/dts/nintendo3ds.dtsi")
    check(dtsi.count("gyro@69") == 1, "gyro@69 DT node was duplicated")

done()
