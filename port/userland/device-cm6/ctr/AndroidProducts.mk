# AndroidProducts.mk - products defined by the old-3DS (CTR) Froyo device dir
#
#   . build/envsetup.sh
#   lunch cyanogen_ctr-eng        (CyanogenMod 6 product)
#   lunch ctr-eng                 (AOSP-style product)

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/ctr.mk
