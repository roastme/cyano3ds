# AndroidBoard.mk - board-specific Android.mk includes for the Nintendo 3DS
#
# The build system (build/core/main.mk) already runs findleaves.py over the
# whole tree and includes every Android.mk, *including* the ones under this
# device directory.  Do NOT also `include $(call all-makefiles-under,...)`
# here: that defines every module twice (e.g. copybit.nintendo3ds), which is a
# fatal "already defined" error in build/core/base_rules.mk.

LOCAL_PATH := $(call my-dir)
