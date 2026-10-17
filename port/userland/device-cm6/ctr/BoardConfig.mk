# BoardConfig.mk - CyanogenMod 6 (Android 2.2 Froyo, API 8) on the OLD
# Nintendo 3DS / 2DS ("CTR": ARM11 MPCore at 268 MHz, 128 MB FCRAM)
#
# Like the CM7 tree this is a "no kernel" board: the linux-3ds zImage, DTB and
# initramfs are loaded from the SD card by firm_linux_loader, and the port's
# initramfs mounts /system and /data from ext4 image files and execs Android's
# init.  No GPU (software GLES1 on the generic fbdev gralloc), no modem, no
# camera, no Bluetooth, no GPS, generic audio stub.
#
# Froyo has no armeabi-v6l ABI: the ARM11 (ARMv6K + VFPv2) runs the armv6-vfp
# variant (arm1136jf-s, softfp) as plain "armeabi".

TARGET_ARCH                := arm
TARGET_ARCH_VARIANT        := armv6-vfp
TARGET_CPU_ABI             := armeabi
TARGET_BOARD_PLATFORM      := ctr
TARGET_BOOTLOADER_BOARD_NAME := ctr

TARGET_NO_BOOTLOADER       := true
TARGET_NO_KERNEL           := true
TARGET_NO_RADIOIMAGE       := true

# Software GLES1 + generic gralloc, no copybit.
BOARD_NO_RGBX_8888         := true
BOARD_NO_NATIVE_COPYBIT    := true

USE_CAMERA_STUB            := true
BOARD_USES_GENERIC_AUDIO   := true
BOARD_HAVE_BLUETOOTH       := false
BOARD_HAVE_GPS             := false

# Dalvik's JIT is ARMv7-only in Froyo, so none here.  ext4 images for
# `make systemimage`; the port repacks /system and /data itself.
TARGET_USERIMAGES_USE_EXT4 := true
BOARD_SYSTEMIMAGE_PARTITION_SIZE   := 201326592
BOARD_USERDATAIMAGE_PARTITION_SIZE := 134217728
BOARD_FLASH_BLOCK_SIZE     := 4096

BOARD_KERNEL_CMDLINE       :=
