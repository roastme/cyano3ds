#!/usr/bin/env python3
"""kernel/cgroup/cgroup-v1.c: let a CAP_SYS_NICE holder migrate tasks.

CyanogenMod 7.2's ActivityManagerService (unlike upstream AOSP) calls
Process.setProcessGroup() -> set_sched_policy() -> write() to
/dev/cpuctl/tasks and /dev/cpuctl/bg_non_interactive/tasks.  The writer is
system_server (uid 1000) and the target task belongs to an app uid (100xx).

cgroup v1's __cgroup1_procs_write() (kernel/cgroup/cgroup-v1.c) only permits
GLOBAL_ROOT_UID, or a matching tcred->uid / tcred->suid, so every call gets
EACCES.  The framework then logs, for every process it schedules:

    SchedPolicy: add_tid_to_cgroup failed to write 'NNN' (Permission denied)
    ActivityManager: Failed setting process group of NNN to 0/1
    System.err: java.lang.SecurityException: No permission to set to given group

Android common kernels have carried the escape since "cgroup: Add generic
cgroup subsystem permission checks" (android.googlesource.com 4a12178d):
a task may be migrated if the caller is root, owns the task, or holds
CAP_SYS_NICE.  system_server is started with it - Zygote's startSystemServer()
hard-codes --capabilities=130104352 (0x7c13c20) which includes bit 23
(CAP_SYS_NICE) in both the permitted and effective sets.

Only the legacy (v1) hierarchy is touched, which is what Android (and the
port's CM7 init.rc) uses.  Idempotent.
"""
import sys
import pathlib

kd = pathlib.Path(sys.argv[1])
p = kd / "kernel/cgroup/cgroup-v1.c"
s = p.read_text()

OLD = (
    "\tif (!uid_eq(cred->euid, GLOBAL_ROOT_UID) &&\n"
    "\t    !uid_eq(cred->euid, tcred->uid) &&\n"
    "\t    !uid_eq(cred->euid, tcred->suid))\n"
    "\t\tret = -EACCES;\n"
)
NEW = (
    "\tif (!uid_eq(cred->euid, GLOBAL_ROOT_UID) &&\n"
    "\t    !uid_eq(cred->euid, tcred->uid) &&\n"
    "\t    !uid_eq(cred->euid, tcred->suid) &&\n"
    "\t    !capable(CAP_SYS_NICE))\n"
    "\t\tret = -EACCES;\n"
)

if NEW in s:
    print("    cgroup-v1.c: CAP_SYS_NICE cgroup attach already allowed")
elif OLD in s:
    s = s.replace(OLD, NEW, 1)
    # capable() lives in <linux/capability.h>; pull it in explicitly rather
    # than relying on a transitive include that could disappear.
    if "linux/capability.h" not in s:
        s = s.replace("#include <linux/slab.h>",
                      "#include <linux/capability.h>\n#include <linux/slab.h>", 1)
    p.write_text(s)
    print("    cgroup-v1.c: allow cgroup v1 task migration with CAP_SYS_NICE")
else:
    print("    cgroup-v1.c: permission block not found (already patched, "
          "or the kernel changed) - leaving it alone")