# cyanogen_nintendo3ds.mk - CyanogenMod 7.2 product for the Nintendo 3DS
#
# This file is installed by port/scripts/apply-cm7-device.sh into
# vendor/cyanogen/products/cyanogen_nintendo3ds.mk.  `brunch nintendo3ds`
# turns into `lunch cyanogen_nintendo3ds-eng`, which makes the build import
# exactly this file (see build/core/product_config.mk, CM_BUILD != "").
#
# It deliberately does NOT inherit vendor/cyanogen/products/gsm.mk: the 3DS has
# no modem/telephony, no SIM, and no RIL.

# The 3DS device hooks (AOSP-style product + 3DS graphics/HAL choices).
$(call inherit-product, device/nintendo/nintendo3ds/device_nintendo3ds.mk)

# Common CyanogenMod extras: ADWLauncher, CMParts, theme engine, etc.
$(call inherit-product, vendor/cyanogen/products/common_full.mk)

#
# Device-specific product configuration.
#
PRODUCT_NAME       := cyanogen_nintendo3ds
PRODUCT_BRAND      := nintendo
PRODUCT_DEVICE     := nintendo3ds
PRODUCT_MODEL      := Nintendo 3DS
PRODUCT_MANUFACTURER := Nintendo

# Release name and versioning
PRODUCT_RELEASE_NAME := nintendo3ds
PRODUCT_VERSION_DEVICE_SPECIFIC :=
-include vendor/cyanogen/products/common_versions.mk
