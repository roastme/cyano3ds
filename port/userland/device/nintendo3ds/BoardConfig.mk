# BoardConfig.mk - CyanogenMod 7.2 (Android 2.3.7, API 10) on the Nintendo 3DS
#
# Built from source.  The result must match the existing working
# userspace: API 10, 32-bit binder (protocol 7), software GLES1
# (libGLES_android) on the generic fbdev gralloc (gralloc.default), driving the
# kernel's ctr_lcd_fb, which exposes the bottom panel as a normal landscape
# 320x240 RGB565 fbdev (fb1).  The kernel and FIRM do not change.
#
# Things that make this board unlike a 2011 phone:
#   * the kernel is NOT built by the Android build system (no boot.img): the
#     linux-3ds zImage + DTB + initramfs are loaded by firm_linux_loader from
#     the SD card, and the port's own initramfs mounts /system and /data from
#     ext4 image files and then execs Android's init;
#   * no GPU driver  -> software GLES1 only, no Adreno/Qcom blobs;
#   * no modem, Wi-Fi, Bluetooth, GPS, camera, sensors, audio codec.
#
# Cross-checked against the 3DS CPU: ARM11 MPCore = ARMv6K + VFPv2, which is
# CM7's `armv6-vfp` build variant (and the ABI CM7's ARMv6 devices such as
# cooper use: armeabi-v6l / armeabi).

LOCAL_PATH := $(call my-dir)

TARGET_ARCH            := arm
TARGET_ARCH_VARIANT    := armv6-vfp
TARGET_CPU_ABI         := armeabi-v6l
TARGET_CPU_ABI2        := armeabi
TARGET_BOARD_PLATFORM  := nintendo3ds
TARGET_BOOTLOADER_BOARD_NAME := nintendo3ds

TARGET_NO_BOOTLOADER   := true
TARGET_NO_KERNEL       := true
TARGET_NO_RADIOIMAGE   := true

# --- graphics ---------------------------------------------------------------
# No GPU.  Keep Android's generic fbdev gralloc and the software GLES1
# library; no egl.cfg means libEGL's loader uses the default tag "android"
# (== libGLES_android.so).  No copybit HAL.
BOARD_NO_RGBX_8888          := true
BOARD_NO_NATIVE_COPYBIT     := true
BOARD_NO_ALLOW_DEQUEUE_CURRENT_BUFFER := true

# --- HALs ------------------------------------------------------------------
USE_CAMERA_STUB             := true
# Audio: the port's kernel driver (ctr_snd.c) initialises the TSC2117/AIC3010
# codec and exposes a normal ALSA PCM playback card fed by the CSND hardware
# (I2S2), with no XpertTeak DSP firmware needed.  Use CyanogenMod's ALSA HAL
# (hardware/alsa_sound) instead of the generic/stub one.  The device tree
# ships /system/etc/asound.conf so the Android* PCM names resolve to hw:0,0.
BOARD_USES_GENERIC_AUDIO    := false
BOARD_USES_ALSA_AUDIO       := true
BOARD_HAVE_BLUETOOTH        := false
BOARD_HAVE_GPS              := false

# --- Wi-Fi ------------------------------------------------------------------
# Atheros AR6014G (AR6002/hw2 family) on the second SDIO controller, driven by
# the *in-kernel* mainline ath6kl.  The Nintendo NWM firmware is uploaded by
# port/scripts/fix-ath6kl.py's AR6002 path and installed in the initramfs at
# /lib/firmware/ath6k/AR6014/, so there is no loadable module: libhardware_
# legacy's wifi.c would insmod /system/lib/modules/... and fail.  The port
# patches it (port/scripts/fix-cm7-wifi.py) to treat an existing wlan0 netdev
# as "driver loaded".
#
# ath6kl is a cfg80211 driver and the kernel has CONFIG_CFG80211_WEXT=y, so
# CM7.2's own wpa_supplicant_6 WEXT driver can drive it.  (The nl80211 backend
# needs external/libnl, which this CM7 tree does not ship.)  The "ar6000"
# module name is deliberate: wifi.c special-cases it to bring wlan0 up before
# the supplicant starts, which the AR6k firmware needs in order to scan.
BOARD_HAVE_WIFI             := true
WPA_SUPPLICANT_VERSION      := VER_0_6_X
BOARD_WPA_SUPPLICANT_DRIVER := WEXT
WIFI_DRIVER_MODULE_NAME     := "ar6000"
WIFI_DRIVER_MODULE_PATH     := "/system/lib/modules/ar6000.ko"
WIFI_DRIVER_MODULE_ARG      := ""

# CM7's WEXT wpa_supplicant defaults to the TI/AR6k vendor "combo scan"
# (SIOCSIWPRIV "cscan"), which a mainline cfg80211 driver does not implement,
# so every scan failed and the stack never reached "connected".  AOSP/CM's
# BOARD_WEXT_NO_COMBO_SCAN=true selects the standard SIOCSIWSCAN/SIOCGIWSCAN
# path, which cfg80211's wext-compat (CONFIG_CFG80211_WEXT=y) does implement.
# The matching CM7 source patch (fix-cm7-wpa-wext.py) additionally removes the
# vendor-command failures that made WifiStateTracker power-cycle the interface
# in an endless on/off loop.
BOARD_WEXT_NO_COMBO_SCAN    := true

# --- Dalvik ----------------------------------------------------------------
WITH_JIT                    := true
ENABLE_JSC_JIT              := true
JS_ENGINE                   := v8

# --- images ----------------------------------------------------------------
# The port repacks /system and /data itself with mke2fs (the images are loop
# mounted from the SD card), but keep the sizes sane so `make systemimage`
# works if it is ever invoked.
TARGET_USERIMAGES_USE_EXT4  := true
BOARD_SYSTEMIMAGE_PARTITION_SIZE   := 268435456
BOARD_USERDATAIMAGE_PARTITION_SIZE := 268435456
BOARD_FLASH_BLOCK_SIZE      := 4096

TARGET_OTA_ASSERT_DEVICE    := nintendo3ds

# No kernel cmdline of our own: firm_linux_loader supplies it.
BOARD_KERNEL_CMDLINE        :=
