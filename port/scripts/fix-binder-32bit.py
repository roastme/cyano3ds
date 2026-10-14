#!/usr/bin/env python3
"""Make mainline binder expose the 32-bit (protocol 7) ABI Android expects.

include/uapi/linux/android/binder.h picks the binder ABI with:

        #ifdef BINDER_IPC_32BIT
        typedef __u32 binder_size_t;
        typedef __u32 binder_uintptr_t;
        ...
        #define BINDER_CURRENT_PROTOCOL_VERSION 7
        #else
        typedef __u64 binder_size_t;
        typedef __u64 binder_uintptr_t;
        ...
        #define BINDER_CURRENT_PROTOCOL_VERSION 8
        #endif

Mainline never defines BINDER_IPC_32BIT (the old
CONFIG_ANDROID_BINDER_IPC_32BIT was removed), so even on a 32-bit kernel the
driver presents the 64-bit ABI.  The ioctl command numbers encode
sizeof(struct binder_write_read/transaction_data/...), so they do not match the
ones Android's 32-bit libbinder computes: the first binder_loop()
BINDER_WRITE_READ returns ENOTTY, servicemanager exits, init restarts the
"critical" service four times and then says

        init: critical process 'servicemanager' exited 4 times in 4 minutes;
              rebooting into recovery mode

and the driver also reports protocol 8 where CM7 is from the v7 era.

Define BINDER_IPC_32BIT for ARCH_CTR so the kernel's struct sizes, ioctl
numbers and reported protocol version all match the 32-bit userspace.

Idempotent.
"""
import pathlib
import os
import sys

kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/src/linux-3ds"))
mk = kd / "drivers/android/Makefile"
s = mk.read_text()

if "BINDER_IPC_32BIT" in s:
    print("    drivers/android/Makefile: BINDER_IPC_32BIT already set")
    raise SystemExit(0)

anchor = "ccflags-y += -I$(src)\t\t\t# needed for trace events\n"
if anchor not in s:
    raise SystemExit("error: drivers/android/Makefile: anchor not found")

add = (
    anchor
    + "\n"
    + "# The 3DS userspace is 32-bit CM7.2 (API 10): use the 32-bit binder ABI\n"
    + "# (and protocol version 7) instead of mainline's 64-bit one.\n"
    + "ccflags-$(CONFIG_ARCH_CTR) += -DBINDER_IPC_32BIT\n"
)
mk.write_text(s.replace(anchor, add, 1))
print("    drivers/android/Makefile: -DBINDER_IPC_32BIT for ARCH_CTR")
