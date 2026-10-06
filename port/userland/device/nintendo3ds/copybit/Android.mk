# Android.mk for the Nintendo 3DS device dir.
#
# Intentionally builds NOTHING.
#
# The CM7.2 port uses the stock generic fbdev `gralloc.default` plus the
# software `libGLES_android` (libpixelflinger) renderer, exactly like the
# existing working build, so no `copybit` HAL is needed (BoardConfig.mk sets
# BOARD_NO_NATIVE_COPYBIT := true).  The legacy pre-gralloc copybit skeleton and its
# `fbtest` executable lived here; the executable referenced
# `../tests/fbtest.c`, which this device tree does not carry, and the `eng`
# build pulls optional modules in, so it broke the build.  fbtest is already
# built into the port's initramfs by port/scripts/mkinitramfs.sh and does not
# need to be in /system.
#
# Keeping the file present (and empty) documents the decision; the source of
# the old blitter, copybit_nintendo3ds.c, is kept next to it for reference.

LOCAL_PATH := $(call my-dir)
