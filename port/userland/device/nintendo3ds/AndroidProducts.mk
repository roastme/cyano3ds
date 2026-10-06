# AndroidProducts.mk - products defined by the Nintendo 3DS device directory
#
# Used by a CyanogenMod-7.2 (gb-release-7.2) source tree:
#   . build/envsetup.sh
#   brunch nintendo3ds            (CM product, recommended)
# or
#   lunch nintendo3ds-eng         (AOSP-style product)
#   make -jN systemimage

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/nintendo3ds.mk
