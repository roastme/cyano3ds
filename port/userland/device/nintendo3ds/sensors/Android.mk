# Android.mk - sensors HAL for the Nintendo 3DS
#
# hw_get_module() loads /system/lib/hw/sensors.<ro.product.board>.so, and
# BoardConfig.mk sets ro.product.board=nintendo3ds.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := sensors.nintendo3ds
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := sensors_nintendo3ds.c
LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_PRELINK_MODULE := false
include $(BUILD_SHARED_LIBRARY)
