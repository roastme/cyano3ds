# device_nintendo3ds.mk - product for the Nintendo 3DS (CyanogenMod 7.2)
#
# This is the AOSP-style half of the product.  The CM half lives in
# vendor/cyanogen/products/cyanogen_nintendo3ds.mk (which inherits this file),
# and is what `brunch nintendo3ds` builds.
#
# Notes:
#  * There is no boot.img/ramdisk: the kernel + initramfs are on the SD card
#    and loaded by firm_linux_loader, so PRODUCT_COPY_FILES must not put
#    init.rc into "root/...": port/scripts/mkinitramfs.sh installs the boot
#    stage into the initramfs instead, and the build script patches the
#    source-built root/init.rc for the 3DS (same patch set as the port's
#    established init.rc patch set).
#  * /system and /data live in ext4 image files on the SD card, loop mounted by
#    the initramfs; the build script packs them from the build's system/ tree.

$(call inherit-product, build/target/product/full_base.mk)
$(call inherit-product, build/target/product/languages_full.mk)

# [3ds] no device/nintendo3ds/overlay directory: we ship no resource
# overlays, and naming a missing overlay dir makes aapt fail.

# Software GLES1 + generic fbdev gralloc (no GPU driver on the 3DS).
PRODUCT_PACKAGES += \
    libEGL \
    libGLESv1_CM \
    libGLESv2 \
    libGLES_android \
    gralloc.default

# ALSA audio HAL hardware modules.  libaudio/libaudiopolicy are pulled in as
# dependencies of the rebuilt libaudioflinger, but the hw modules
# (hw/alsa.default.so, hw/acoustics.default.so) are not, so list them here.
PRODUCT_PACKAGES += \
    alsa.default \
    acoustics.default

# Permission files: the 3DS has a touchscreen and normal buttons.
PRODUCT_COPY_FILES += \
    frameworks/base/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
    frameworks/base/data/etc/android.hardware.touchscreen.xml:system/etc/permissions/android.hardware.touchscreen.xml

# Wi-Fi: Atheros AR6014G via the in-kernel ath6kl (cfg80211 + CFG80211_WEXT).
# wpa_supplicant (0.6.x, WEXT) is only built when WPA_SUPPLICANT_VERSION and
# BOARD_WPA_SUPPLICANT_DRIVER are set (see BoardConfig.mk); it must be listed
# explicitly or it is not installed into /system/bin.
PRODUCT_PACKAGES += \
    wpa_supplicant \
    wpa_cli
PRODUCT_COPY_FILES += \
    device/nintendo/nintendo3ds/wifi/wpa_supplicant.conf:system/etc/wifi/wpa_supplicant.conf \
    device/nintendo/nintendo3ds/wifi/dhcpcd.conf:system/etc/dhcpcd/dhcpcd.conf

# Audio: map the Android ALSA PCM names (AndroidPlayback*) to the port's
# single CSND/TSC2117 card (hw:0,0).  See asound.conf for the details.
PRODUCT_COPY_FILES += \
    device/nintendo/nintendo3ds/asound.conf:system/etc/asound.conf

PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=120 \
    qemu.sf.lcd_density=120 \
    ro.opengles.version=65536 \
    ro.hardware=nintendo3ds \
    ro.board.platform=nintendo3ds \
    ro.product.device=nintendo3ds \
    ro.product.board=nintendo3ds \
    ro.telephony.default_network=0 \
    ro.com.google.locationfeatures=0 \
    ro.com.google.networklocation=0 \
    ro.kernel.android.checkjni=0 \
    ro.setupwizard.mode=DISABLED \
    dalvik.vm.heapsize=64m \
    dalvik.vm.heapgrowthlimit=64m \
    dalvik.vm.stack-trace-file=/data/anr/traces.txt

PRODUCT_NAME       := nintendo3ds
PRODUCT_DEVICE     := nintendo3ds
PRODUCT_BRAND      := nintendo
PRODUCT_MODEL      := Nintendo 3DS
PRODUCT_MANUFACTURER := Nintendo
PRODUCT_LOCALES    += en_US
