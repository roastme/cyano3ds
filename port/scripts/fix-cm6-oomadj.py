#!/usr/bin/env python3
# fix-cm6-oomadj.py - relay ActivityManager's per-process oom_adj to a root helper.
#
# Froyo's ActivityManagerService ranks every process (foreground 0, visible 1,
# ... hidden 7-15) and tells the kernel through Process.setOomAdj(), which
# writes /proc/<pid>/oom_adj.  system_server runs as uid 1000 and the target
# files belong to the app's own uid, so on this kernel (no CAP_DAC_OVERRIDE)
# every write fails silently and every process stays at 0.  With no
# lowmemorykiller in the 5.x kernel either, the OOM killer then picks
# system_server (the biggest process) on the 128 MB old 3DS.
#
# This patch keeps the /proc write and additionally appends "<pid> <adj>\n" to
# /dev/oomadj, a FIFO that the initramfs /bin/memguard (root) drains and
# applies as oom_score_adj.  If the FIFO is missing the call behaves as before.
#
# Usage: fix-cm6-oomadj.py [CM6_DIR]      Idempotent.

import os
import pathlib
import sys

CM6_DIR = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/cm6"))
SRC = CM6_DIR / "frameworks/base/core/jni/android_util_Process.cpp"

OLD = """        int fd = open(text, O_WRONLY);
        if (fd >= 0) {
            sprintf(text, "%d", adj);
            write(fd, text, strlen(text));
            close(fd);
        }
        return true;
"""
NEW = """        int fd = open(text, O_WRONLY);
        if (fd >= 0) {
            sprintf(text, "%d", adj);
            write(fd, text, strlen(text));
            close(fd);
        }
        /* [3ds] the write above is refused for other uids' processes: tell the
         * root memguard helper through its FIFO instead. */
        fd = open("/dev/oomadj", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            sprintf(text, "%d %d\\n", pid, adj);
            write(fd, text, strlen(text));
            close(fd);
        }
        return true;
"""


def main() -> int:
    s = SRC.read_text()
    if "/dev/oomadj" in s:
        print("    android_util_Process.cpp: already patched")
        return 0
    if OLD not in s:
        print("cannot find setOomAdj body in %s" % SRC, file=sys.stderr)
        return 1
    SRC.write_text(s.replace(OLD, NEW, 1))
    print("    android_util_Process.cpp: setOomAdj also feeds /dev/oomadj")
    return 0


if __name__ == "__main__":
    sys.exit(main())
