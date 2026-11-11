#!/usr/bin/env python3
"""External storage: fakevold answers Android's MountService.

The init mounts the card with vold's ownership/masks and binds it at
/mnt/sdcard, and fakevold stands in for vold to report it mounted (the card is
mounted by the init, not by vold).  This builds fakevold for the host and
speaks the vold socket protocol to it, so the checks are the replies a real
MountService would read.  When a patched kernel tree is present it also
confirms the VFAT_IOCTL_GET_VOLUME_ID patch actually landed there (that ioctl
is what keeps MediaProvider from crashing).
"""
import socket
import tempfile
import time
from t3ds import *

if not have("gcc"):
    skip("no host gcc for fakevold")

ft = KERNEL_TREE / "fs/fat/file.c"
if ft.exists():
    check("VFAT_IOCTL_GET_VOLUME_ID" in read(ft), f"{ft} lacks the Android volume-id ioctl")

with tempfile.TemporaryDirectory() as td:
    td = pathlib.Path(td)
    exe = td / "fakevold"
    r = run(["gcc", "-O2", "-Wall", "-Werror", "-o", str(exe), str(USERLAND / "storage/fakevold.c")])
    check(r.returncode == 0, "fakevold.c does not build: " + r.stderr[:400])
    if r.returncode == 0:
        path = str(td / "vold")
        ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ls.bind(path)
        p = subprocess.Popen([str(exe)], env={"ANDROID_SOCKET_vold": str(ls.fileno())},
                             pass_fds=[ls.fileno()])
        try:
            c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            for _ in range(50):
                try:
                    c.connect(path)
                    break
                except OSError:
                    time.sleep(0.05)
            c.settimeout(3)

            def ask(cmd, finals):
                c.sendall(cmd.encode() + b"\0")
                got, buf = [], b""
                while len([g for g in got if 200 <= int(g.split()[0]) < 600]) < finals:
                    buf += c.recv(4096)
                    *done_, buf = buf.split(b"\0")
                    got += [d.decode() for d in done_ if d]
                return got
            check(ask("volume list", 1) == ["110 sdcard /mnt/sdcard 4", "200 Volumes listed."],
                  "volume list does not report sdcard mounted at /mnt/sdcard")
            check(ask("share status ums", 0 + 1)[0].startswith("212 ums unavailable"),
                  "UMS must be reported unavailable")
            check(ask("volume unmount /mnt/sdcard force", 1)[0].startswith("405 "),
                  "unmounting the card must be refused")
            check(ask("asec list", 1) == ["200 asec operation succeeded"], "asec list reply changed")
        finally:
            p.kill()
            p.wait()
done()
