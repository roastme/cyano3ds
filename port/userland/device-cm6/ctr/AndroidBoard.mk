# AndroidBoard.mk - key layout + character map.  CM6's manifest drops
# development/emulator/keymaps, which is where Froyo installs qwerty.kl from;
# without it EventHub finds no layout and no button reaches the framework.
# qwerty.kl maps the 3DS buttons (L=MENU 139, R=SEARCH 217, Y=TAB 15, ...).
LOCAL_PATH := $(call my-dir)

file := $(TARGET_OUT_KEYLAYOUT)/qwerty.kl
ALL_PREBUILT += $(file)
$(file) : $(LOCAL_PATH)/qwerty.kl | $(ACP)
	$(transform-prebuilt-to-target)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := qwerty.kcm
include $(BUILD_KEY_CHAR_MAP)
