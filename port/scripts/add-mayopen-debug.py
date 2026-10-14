#!/usr/bin/env python3
"""Temporary: log the *resolved* inode for /dev/ashmem in may_open().

Root can open /dev/ashmem but every non-root process (including zygote, which
Android runs with euid=fsuid=AID_NOBODY/9999 while preloading classes) gets
EACCES even though /dev/ashmem is cross-checked as 0666 with `ls -l`.  That is
only possible if the inode the kernel resolves is not the one the shell sees
(different superblock / stale node) or if an LSM is involved - may_open()
knows both, so log the superblock name, the mode/owner and the nodev flags for
every /dev/ashmem open (success and failure).

Idempotent.  Remove once bring-up is done.
"""
import pathlib
import os
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
p = kd / "fs/namei.c"
s = p.read_text()

MARK = "ASHMEM-MAYOPEN-DEBUG"
if MARK in s:
    print("    fs/namei.c: ashmem may_open instrumentation already present")
    raise SystemExit(0)

old = ("\tcase S_IFBLK:\n"
       "\tcase S_IFCHR:\n"
       "\t\tif (!may_open_dev(path))\n"
       "\t\t\treturn -EACCES;\n"
       "\t\tfallthrough;\n")
if old not in s:
    raise SystemExit("error: fs/namei.c: may_open() S_IFCHR block not found")

new = ("\tcase S_IFBLK:\n"
       "\tcase S_IFCHR:\n"
       "\t\tif (!strcmp(dentry->d_name.name, \"ashmem\")) /* " + MARK + " */\n"
       "\t\t\tpr_info(\"ashmem may_open[dbg]: sb=%s mode=0%o uid=%u gid=%u\"\n"
       "\t\t\t\t\" fsuid=%u euid=%u acc=0x%x flag=0%o nodev=%d sbnodev=%d\\n\",\n"
       "\t\t\t\tdentry->d_sb->s_id, inode->i_mode,\n"
       "\t\t\t\tfrom_kuid_munged(current_user_ns(), inode->i_uid),\n"
       "\t\t\t\tfrom_kgid_munged(current_user_ns(), inode->i_gid),\n"
       "\t\t\t\tfrom_kuid_munged(current_user_ns(), current_fsuid()),\n"
       "\t\t\t\tfrom_kuid_munged(current_user_ns(), current_euid()),\n"
       "\t\t\t\tacc_mode, flag,\n"
       "\t\t\t\t!!(path->mnt->mnt_flags & MNT_NODEV),\n"
       "\t\t\t\t!!(path->mnt->mnt_sb->s_iflags & SB_I_NODEV));\n"
       "\t\tif (!may_open_dev(path))\n"
       "\t\t\treturn -EACCES;\n"
       "\t\tfallthrough;\n")

p.write_text(s.replace(old, new, 1))
print("    fs/namei.c: ashmem may_open instrumentation added")
