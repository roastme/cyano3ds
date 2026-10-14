#!/usr/bin/env python3
"""External storage (2026-10-14): the card as Android's mounted /mnt/sdcard.

The init mounts the card with vold's ownership/masks and binds it at
/mnt/sdcard; fakevold stands in for vold and reports it mounted; the kernel
answers Gingerbread's VFAT_IOCTL_GET_VOLUME_ID so MediaProvider does not
crash.  fakevold is built for the host and driven over a real socket here.
"""
import socket
import tempfile
import time
from t3ds import *

mk = read(SCRIPTS / "mkinitramfs.sh")
check('o="-o uid=1000,gid=1015,fmask=0002,dmask=0002"' in mk, "card no longer mounted system:sdcard_rw with mask 0002")
# fmask=0702 removes every right from the owner, uid 1000 = system_server:
# PackageManager could not open the card APKs bound into /system/app.
check("fmask=07" not in mk.replace("vold's fmask=0702", ""), "a card mask locks the owner (system_server) out again")
check(order(mk, "mount -o bind /mnt/sd /mnt/sdcard", "/bin/android-init 2>&1"),
      "/mnt/sdcard must be bound before Android's init starts")
check("s|^service vold /system/bin/vold|service vold /bin/fakevold|" in mk, "vold not replaced by fakevold")
check('-o "$ROOT/bin/fakevold" "$PORT/userland/storage/fakevold.c"' in mk, "fakevold not built")
fat = read(SCRIPTS / "add-vfat-android-volume-id.py")
check("_IOR('r', 0x12, __u32)" in fat and "return MSDOS_SB(inode->i_sb)->vol_id;" in fat,
      "Android's VFAT_IOCTL_GET_VOLUME_ID patch changed")
check('python3 "$PORT/scripts/add-vfat-android-volume-id.py" "$KD"' in read(SCRIPTS / "apply-kernel-port.sh"),
      "apply-kernel-port.sh does not apply the VFAT volume-id ioctl")
ft = KERNEL_TREE / "fs/fat/file.c"
if ft.exists():
    check("VFAT_IOCTL_GET_VOLUME_ID" in read(ft), f"{ft} lacks the Android volume-id ioctl")

if not have("gcc"):
    skip("no host gcc for fakevold")
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
