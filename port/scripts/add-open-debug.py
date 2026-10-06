#!/usr/bin/env python3
"""Temporary: log why open("/dev/ashmem") fails, with the credentials.

dalvik's GC logs "Could not create 700416-byte ashmem mark stack" because
ashmem_create_region() (= open("/dev/ashmem") + two ioctls) returns -1, and the
driver's own ashmem_open() is never reached, so the failure is in the VFS
(permission, EMFILE, ...).  V1 of this instrumentation printed only the path and
the errno; V2 also prints the pid/tgid and the real/effective/fs uid plus the
inode's mode and owner, which is what tells a *mode* problem apart from a *uid*
problem.

Idempotent.  Remove once bring-up is done.
"""
import pathlib
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/root/p3ds/src/linux-3ds")
p = kd / "fs/open.c"
s = p.read_text()

V2_MARK = "ASHMEM-OPENDEBUG-V2"
V1_MARK = "ashmem open[dbg]"

# pristine do_sys_openat2() body
ORIG = ("\tfd = get_unused_fd_flags(how->flags);\n"
        "\tif (fd >= 0) {\n"
        "\t\tstruct file *f = do_filp_open(dfd, tmp, &op);\n"
        "\t\tif (IS_ERR(f)) {\n"
        "\t\t\tput_unused_fd(fd);\n"
        "\t\t\tfd = PTR_ERR(f);\n"
        "\t\t} else {\n"
        "\t\t\tfsnotify_open(f);\n"
        "\t\t\tfd_install(fd, f);\n"
        "\t\t}\n"
        "\t}\n"
        "\tputname(tmp);\n"
        "\treturn fd;\n")

# V1 (previous version of this script)
V1 = ("\tfd = get_unused_fd_flags(how->flags);\n"
      "\tif (fd < 0 && strstr(tmp->name, \"ashmem\"))\n"
      "\t\tpr_err(\"ashmem open[dbg]: %s get_unused_fd -> %d\\n\",\n"
      "\t\t       tmp->name, fd);\n"
      "\tif (fd >= 0) {\n"
      "\t\tstruct file *f = do_filp_open(dfd, tmp, &op);\n"
      "\t\tif (IS_ERR(f)) {\n"
      "\t\t\tput_unused_fd(fd);\n"
      "\t\t\tfd = PTR_ERR(f);\n"
      "\t\t\tif (strstr(tmp->name, \"ashmem\"))\n"
      "\t\t\t\tpr_err(\"ashmem open[dbg]: %s do_filp_open -> %d\\n\",\n"
      "\t\t\t\t       tmp->name, fd);\n"
      "\t\t} else {\n"
      "\t\t\tfsnotify_open(f);\n"
      "\t\t\tfd_install(fd, f);\n"
      "\t\t}\n"
      "\t}\n"
      "\tputname(tmp);\n"
      "\treturn fd;\n")

NEW = ("\tfd = get_unused_fd_flags(how->flags);\n"
       "\tif (fd < 0)\n"
       "\t\tashmem_dbg_open(tmp->name, fd, how->flags);\n"
       "\tif (fd >= 0) {\n"
       "\t\tstruct file *f = do_filp_open(dfd, tmp, &op);\n"
       "\t\tif (IS_ERR(f)) {\n"
       "\t\t\tput_unused_fd(fd);\n"
       "\t\t\tfd = PTR_ERR(f);\n"
       "\t\t\tashmem_dbg_open(tmp->name, fd, how->flags);\n"
       "\t\t} else {\n"
       "\t\t\tfsnotify_open(f);\n"
       "\t\t\tfd_install(fd, f);\n"
       "\t\t}\n"
       "\t}\n"
       "\tputname(tmp);\n"
       "\treturn fd;\n")

HELPER = (
"/* ASHMEM-OPENDEBUG-V2: log an open() failure on an ashmem path, with the\n"
" * caller's credentials, the open flags and the inode's mode/owner. */\n"
"static void ashmem_dbg_open(const char *name, int err, unsigned int flags)\n"
"{\n"
"\tstruct path path;\n"
"\n"
"\tif (!strstr(name, \"ashmem\"))\n"
"\t\treturn;\n"
"\tpr_err(\"ashmem open[dbg]: %s -> %d flags=0%o pid=%d tgid=%d uid=%u euid=%u fsuid=%u\\n\",\n"
"\t       name, err, flags, task_pid_nr(current), task_tgid_nr(current),\n"
"\t       from_kuid_munged(current_user_ns(), current_uid()),\n"
"\t       from_kuid_munged(current_user_ns(), current_euid()),\n"
"\t       from_kuid_munged(current_user_ns(), current_fsuid()));\n"
"\tif (kern_path(name, 0, &path) == 0) {\n"
"\t\tstruct inode *inode = d_backing_inode(path.dentry);\n"
"\n"
"\t\tif (inode)\n"
"\t\t\tpr_err(\"ashmem open[dbg]:   inode mode=0%o uid=%u gid=%u\"\n"
"\t\t\t       \" mnt_flags=0x%x\\n\",\n"
"\t\t\t       inode->i_mode,\n"
"\t\t\t       from_kuid_munged(current_user_ns(), inode->i_uid),\n"
"\t\t\t       from_kgid_munged(current_user_ns(), inode->i_gid),\n"
"\t\t\t       path.mnt->mnt_flags);\n"
"\t\tpath_put(&path);\n"
"\t}\n"
"}\n"
"\n")

if V2_MARK in s:
    # Upgrade an existing V2 (no-flags) build in place: V2_MARK is only a
    # comment in the helper, so replacing the two call sites + helper text
    # keeps this idempotent.
    s = s.replace("ashmem_dbg_open(tmp->name, fd);",
                  "ashmem_dbg_open(tmp->name, fd, how->flags);")
    if "mnt_flags=0x%x" not in s:
        s = s.replace(
            "static void ashmem_dbg_open(const char *name, int err)\n",
            "static void ashmem_dbg_open(const char *name, int err, unsigned int flags)\n")
        s = s.replace(
            "pr_err(\"ashmem open[dbg]: %s -> %d pid=%d tgid=%d uid=%u euid=%u fsuid=%u\\n\",\n"
            "\t       name, err, task_pid_nr(current), task_tgid_nr(current),\n",
            "pr_err(\"ashmem open[dbg]: %s -> %d flags=0%o pid=%d tgid=%d uid=%u euid=%u fsuid=%u\\n\",\n"
            "\t       name, err, flags, task_pid_nr(current), task_tgid_nr(current),\n")
        s = s.replace(
            "pr_err(\"ashmem open[dbg]:   inode mode=0%o uid=%u gid=%u\\n\",\n"
            "\t\t\t       inode->i_mode,\n"
            "\t\t\t       from_kuid_munged(current_user_ns(), inode->i_uid),\n"
            "\t\t\t       from_kgid_munged(current_user_ns(), inode->i_gid));\n",
            "pr_err(\"ashmem open[dbg]:   inode mode=0%o uid=%u gid=%u\"\n"
            "\t\t\t       \" mnt_flags=0x%x\\n\",\n"
            "\t\t\t       inode->i_mode,\n"
            "\t\t\t       from_kuid_munged(current_user_ns(), inode->i_uid),\n"
            "\t\t\t       from_kgid_munged(current_user_ns(), inode->i_gid),\n"
            "\t\t\t       path.mnt->mnt_flags);\n")
        p.write_text(s)
        print("    fs/open.c: upgraded open instrumentation to V2+flags")
    else:
        print("    fs/open.c: V2 open instrumentation already present")
    raise SystemExit(0)

if V1 in s:
    s = s.replace(V1, NEW, 1)
    print("    fs/open.c: upgraded open-failure instrumentation to V2")
elif ORIG in s:
    s = s.replace(ORIG, NEW, 1)
    print("    fs/open.c: added open-failure instrumentation (V2)")
else:
    raise SystemExit("error: fs/open.c: do_sys_openat2 body not found")

anchor = "static long do_sys_openat2("
if anchor not in s:
    raise SystemExit("error: fs/open.c: do_sys_openat2 anchor not found")
s = s.replace(anchor, HELPER + anchor, 1)

p.write_text(s)
print("    fs/open.c: ashmem open-failure instrumentation (V2) in place")
