#!/usr/bin/env python3
"""Cameras (2026-10-14): Octoblimp's driver + their HAL moved to Gingerbread.

  * kernel: ctr_cam.c is Android3DS/Octoblimp's file, unchanged; the DT node
    and Kconfig come from add-ctr-cam.py;
  * userspace: CM7's CameraService.cpp rebuilt with CameraHardwareN3ds (their
    HAL) instead of the stub: RGB565 preview (libagl shows NV21 grey), a
    takePicture() that does not join the preview thread under CameraService's
    lock (that deadlocked the shutter), Gingerbread camera ids 0 = back
    (outer right) and 1 = front (inner) with orientation 90/270;
  * /dev/ctr_cam is made 0666 by the init (mediaserver is not root).
"""
import hashlib
from t3ds import *

cam = KERNEL / "drivers/platform/nintendo3ds/ctr_cam.c"
OCTOBLIMP_SHA256 = "260d2803f504d9b365b1d19c24fcdd952fad7250c0fdaa7e9320da06bd0a3b1d"
check(hashlib.sha256(cam.read_bytes()).hexdigest() == OCTOBLIMP_SHA256,
      "ctr_cam.c differs from the Octoblimp file it was copied from")
s = read(SCRIPTS / "add-ctr-cam.py")
for needle in ['compatible = "nintendo,3ds-cam"', "nintendo,i2c-bus = <&i2c1>, <&i2c2>;",
               "<0x10320000 0x1000>", "<0x10321000 0x1000>", "obj-$(CONFIG_CTR_CAM) += ctr_cam.o"]:
    check(needle in s, f"add-ctr-cam.py lost {needle!r}")
check("install -m 644 \"$PORT/kernel/drivers/platform/nintendo3ds/ctr_cam.c\"" in
      read(SCRIPTS / "apply-kernel-port.sh"), "apply-kernel-port.sh does not install ctr_cam.c")
check("--enable  CTR_CAM" in read(SCRIPTS / "build-kernel.sh"), "build-kernel.sh does not enable CTR_CAM")

hal = read(USERLAND / "camera/CameraHardwareN3ds.cpp")
svc = read(USERLAND / "camera/CameraService.cpp")
check('extern "C" int HAL_getNumberOfCameras()' in hal and
      'extern "C" sp<CameraHardwareInterface> HAL_openCameraHardware(int cameraId)' in hal and
      'extern "C" void HAL_getCameraInfo(int cameraId, struct CameraInfo* cameraInfo)' in hal,
      "Gingerbread HAL entry points missing")
check("{ CAMERA_FACING_BACK,  90 }" in hal and "{ CAMERA_FACING_FRONT, 270 }" in hal,
      "camera orientations changed (the preview was sideways with 0)")
tp = hal[hal.index("status_t CameraHardwareN3ds::takePicture()"):]
tp = tp[:tp.index("\n}\n")]
check("stopPreviewThread" not in tp and "requestExitAndWait" not in tp,
      "takePicture() joins the preview thread again (deadlocks with CameraService's lock)")
check("mPreviewThread->requestExit();" in tp and "mPreviewStopping = true;" in tp,
      "takePicture() no longer stops the preview asynchronously")
check(order(hal, "int CameraHardwareN3ds::pictureThread()", "stopPreviewThread();"),
      "pictureThread() does not join the preview thread first")
check(order(hal, "void CameraHardwareN3ds::postPreview", "if (mPreviewStopping)", "mDataCb("),
      "frames are still posted after takePicture()")
check(order(svc, "N3DS_CAMERA_RGB565", "CameraParameters::PIXEL_FORMAT_RGB565",
            "mPixelFormat = HAL_PIXEL_FORMAT_RGB_565;"),
      "CameraService no longer honours the RGB565 preview format")
b = read(USERLAND / "camera/build-camera-hal.sh")
check("--hash-style=sysv" in b, "camera lib must use SysV hash (Gingerbread linker)")
check("-Bsymbolic" in b and "-fno-short-enums" in b and "-mfloat-abi=softfp" in b,
      "camera lib ABI flags changed")

mk = read(SCRIPTS / "mkinitramfs.sh")
check("for m in binder ashmem alarm ctr_cam log_main" in mk, "/dev/ctr_cam node not created")
check("chmod 0666 /dev/ctr_cam" in mk, "/dev/ctr_cam not accessible to mediaserver")
done()
