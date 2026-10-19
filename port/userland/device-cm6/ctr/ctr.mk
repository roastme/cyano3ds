# ctr.mk - AOSP-style product for the old Nintendo 3DS (Froyo)
#
# 128 MB of RAM, a 320x240 touch panel and no phone hardware, so this is a
# trimmed core.mk product: framework, Launcher2, Settings, the Music, Gallery,
# Camera, Calendar and Email apps and a few small ones,
# not generic.mk's full application set.

$(call inherit-product, build/target/product/core.mk)
$(call inherit-product, build/target/product/languages_small.mk)

PRODUCT_PACKAGES += \
    Launcher2 \
    Settings \
    Provision \
    LatinIME \
    Calculator \
    DeskClock \
    AlarmProvider \
    Music \
    Gallery \
    Camera \
    Calendar \
    CalendarProvider \
    Email \
    libEGL \
    libGLESv1_CM \
    libGLESv2 \
    libGLES_android \
    gralloc.default \
    wpa_supplicant \
    wpa_cli \
    dhcpcd \
    dhcpcd-run-hooks


PRODUCT_COPY_FILES += \
    frameworks/base/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
    frameworks/base/data/etc/android.hardware.touchscreen.xml:system/etc/permissions/android.hardware.touchscreen.xml \
    frameworks/base/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
    device/nintendo/ctr/wifi/wpa_supplicant.conf:system/etc/wifi/wpa_supplicant.conf \
    device/nintendo/ctr/wifi/dhcpcd.conf:system/etc/dhcpcd/dhcpcd.conf

PRODUCT_PROPERTY_OVERRIDES += \
    ro.sf.lcd_density=120 \
    qemu.sf.lcd_density=120 \
    ro.opengles.version=65536 \
    ro.hardware=ctr \
    wifi.interface=wlan0 \
    ro.board.platform=ctr \
    ro.telephony.default_network=0 \
    ro.kernel.android.checkjni=0 \
    ro.setupwizard.mode=DISABLED \
    keyguard.no_require_sim=true \
    dalvik.vm.heapsize=32m \
    dalvik.vm.stack-trace-file=/data/anr/traces.txt

PRODUCT_NAME         := ctr
PRODUCT_DEVICE       := ctr
PRODUCT_BRAND        := nintendo
PRODUCT_MODEL        := Nintendo 3DS
PRODUCT_MANUFACTURER := Nintendo
PRODUCT_LOCALES      := en_US
