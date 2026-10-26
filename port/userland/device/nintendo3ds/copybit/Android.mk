# Android.mk for the Nintendo 3DS device dir.
#
# Intentionally builds NOTHING.
#
# The CM7.2 port uses the stock generic fbdev `gralloc.default` plus the
# software `libGLES_android` (libpixelflinger) renderer, so no `copybit` HAL
# is needed (BoardConfig.mk sets BOARD_NO_NATIVE_COPYBIT := true).  fbtest is
# built into the port's initramfs by port/scripts/mkinitramfs.sh and does not
# need to be in /system.
#
# Keeping the file present (and empty) documents the decision;
# copybit_nintendo3ds.c, a pre-gralloc copybit blitter, is kept next to it for
# reference.

LOCAL_PATH := $(call my-dir)
