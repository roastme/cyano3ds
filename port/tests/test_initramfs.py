#!/usr/bin/env python3
"""Inspect a built initramfs (INITRAMFS=path): what the fixes need is in it,
and it fits the loader's 8 MiB window."""
import gzip
import io
import os
from t3ds import *

path = os.environ.get("INITRAMFS")
if not path or not os.path.exists(path):
    skip("set INITRAMFS=path/to/initramfs.cpio.gz")
blob = pathlib.Path(path).read_bytes()
check(len(blob) < 8 * 1024 * 1024, f"initramfs is {len(blob)} bytes, over the 8 MiB loader window")
raw = gzip.decompress(blob)
files, i = {}, 0
while i + 110 <= len(raw):
    hdr = raw[i:i + 110]
    if hdr[:6] != b"070701":
        break
    fsize, nsize = int(hdr[54:62], 16), int(hdr[94:102], 16)
    mode = int(hdr[14:22], 16)
    name = raw[i + 110:i + 110 + nsize - 1].decode()
    off = (i + 110 + nsize + 3) & ~3
    files[name] = (mode, raw[off:off + fsize])
    i = (off + fsize + 3) & ~3
    if name == "TRAILER!!!":
        break
for f in ["init", "bin/fakevold", "bin/pcmtest", "bin/selftest", "init.rc",
          "lib/firmware/ath6k/AR6002/nwm/stub_data.bin", "lib/firmware/ath6k/AR6002/nwm/stub_code.bin",
          "lib/firmware/ath6k/AR6002/nwm/main_type4.bin", "lib/firmware/ath6k/AR6002/nwm/database.bin"]:
    check(f in files, f"initramfs lacks /{f}")
init = files.get("init", (0, b""))[1].decode(errors="replace")
for needle in ["/bin/lid-sleep-hook", "/bin/dataflush periodic", "logcat -v time", "mount -o bind /mnt/sd /mnt/sdcard",
               "ctr_cam", "/bin/selftest"]:
    check(needle in init, f"initramfs /init lacks {needle!r}")
check(b"service vold /bin/fakevold" in files.get("init.rc", (0, b""))[1], "initramfs init.rc still starts vold")
for name, (mode, _) in files.items():
    if name == "init" or name.startswith("bin/") and name in ("bin/fakevold", "bin/pcmtest", "bin/selftest"):
        check(mode & 0o111, f"/{name} is not executable")
done()
