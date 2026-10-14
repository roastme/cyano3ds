#!/usr/bin/env python3
"""The on-device self-test parses, covers every subsystem, and is scheduled."""
from t3ds import *

st = USERLAND / "tests/selftest.sh"
if have("busybox"):
    r = run(["busybox", "sh", "-n", str(st)])
    check(r.returncode == 0, "selftest.sh does not parse: " + r.stderr)
s = read(st)
for part in ["--- lid sleep", "--- Wi-Fi", "--- audio", "--- cameras", "--- external storage",
             "--- /data persistence", "--- top-screen console log"]:
    check(part in s, f"selftest lost its {part!r} section")
mk = read(SCRIPTS / "mkinitramfs.sh")
check('install -m 755 "$PORT/userland/tests/selftest.sh" "$ROOT/bin/selftest"' in mk, "selftest not installed")
check('cp /mnt/probe/selftest.txt "$LOGDIR/selftest.txt"' in mk, "selftest report not copied to the card")
done()
