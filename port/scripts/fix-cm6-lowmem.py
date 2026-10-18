#!/usr/bin/env python3
# fix-cm6-lowmem.py - keep fewer background processes on the 128 MB old 3DS.
#
# Froyo's ActivityManagerService keeps up to 15 hidden (cached) app processes.
# That suits a 192+ MB phone; on 128 MB each cached process pins 5-10 MB (or
# churns the compressed swap) while a game is loading.  Cap it at 3.
#
# Usage: fix-cm6-lowmem.py [CM6_DIR]      Idempotent.

import os
import pathlib
import sys

CM6_DIR = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/cm6"))
SRC = CM6_DIR / "frameworks/base/services/java/com/android/server/am/ActivityManagerService.java"

OLD = "    static final int MAX_HIDDEN_APPS = 15;\n"
NEW = "    static final int MAX_HIDDEN_APPS = 3; // [3ds] 128 MB device (stock: 15)\n"


def main() -> int:
    s = SRC.read_text()
    if NEW in s:
        print("    ActivityManagerService.java: already patched")
        return 0
    if OLD not in s:
        print("cannot find MAX_HIDDEN_APPS in %s" % SRC, file=sys.stderr)
        return 1
    SRC.write_text(s.replace(OLD, NEW, 1))
    print("    ActivityManagerService.java: MAX_HIDDEN_APPS 15 -> 3")
    return 0


if __name__ == "__main__":
    sys.exit(main())
