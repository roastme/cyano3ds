# cyanogen_ctr.mk - CyanogenMod 6 (Froyo) product for the old Nintendo 3DS
#
# Installed by port/scripts/apply-cm6-device.sh into
# vendor/cyanogen/products/.  No gsm.mk: no modem, SIM or RIL.

$(call inherit-product, device/nintendo/ctr/ctr.mk)
$(call inherit-product, vendor/cyanogen/products/common.mk)

PRODUCT_NAME         := cyanogen_ctr
PRODUCT_BRAND        := nintendo
PRODUCT_DEVICE       := ctr
PRODUCT_MODEL        := Nintendo 3DS
PRODUCT_MANUFACTURER := Nintendo

PRODUCT_BUILD_PROP_OVERRIDES += BUILD_ID=FRG83 BUILD_DISPLAY_ID=FRG83

PRODUCT_PROPERTY_OVERRIDES += \
    ro.modversion=CyanogenMod-6.1.2-ctr
