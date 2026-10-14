#!/usr/bin/env python3
"""Android's VFAT_IOCTL_GET_VOLUME_ID for Gingerbread's MediaProvider.

Gingerbread identifies the external card by its FAT volume serial:
MediaProvider.getDatabaseForUri() calls FileUtils.getFatVolumeId(), whose
JNI half (android_os_FileUtils_getFatVolumeId) does

    result = ioctl(fd, VFAT_IOCTL_GET_VOLUME_ID);   // _IOR('r', 0x12, __u32)

and expects the serial as the *return value*.  That ioctl only ever existed
in Android kernels.  Mainline has FAT_IOCTL_GET_VOLUME_ID instead (0x13, the
serial through a pointer), so on this kernel the call fails, MediaProvider
throws "Unable to obtain external storage volume ID for external volume" and
android.process.media crashes as soon as Android sees a mounted card.

This adds the Android ioctl next to the mainline one in fat_generic_ioctl()
(files and directories; MediaProvider opens the mount point).  0x12 is unused
in the 'r' range.  Idempotent.
"""
import os
import pathlib
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
f = kd / "fs/fat/file.c"
s = f.read_text()
MARK = "VFAT_IOCTL_GET_VOLUME_ID"
if MARK in s:
    print("    fs/fat/file.c: Android VFAT_IOCTL_GET_VOLUME_ID: already done")
    sys.exit(0)

old = "long fat_generic_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)\n"
if old not in s:
    raise SystemExit(f"error: {f}: anchor not found")
s = s.replace(old, '''/*
 * Android (Gingerbread) VFAT_IOCTL_GET_VOLUME_ID: the volume serial as the
 * return value, for MediaProvider (port/scripts/add-vfat-android-volume-id.py).
 */
#define VFAT_IOCTL_GET_VOLUME_ID	_IOR('r', 0x12, __u32)

''' + old, 1)

old = "\tcase FAT_IOCTL_GET_VOLUME_ID:\n\t\treturn fat_ioctl_get_volume_id(inode, user_attr);\n"
if old not in s:
    raise SystemExit(f"error: {f}: FAT_IOCTL_GET_VOLUME_ID case not found")
s = s.replace(old, old + "\tcase VFAT_IOCTL_GET_VOLUME_ID:\n\t\treturn MSDOS_SB(inode->i_sb)->vol_id;\n", 1)
f.write_text(s)
print("    fs/fat/file.c: Android VFAT_IOCTL_GET_VOLUME_ID: done")
