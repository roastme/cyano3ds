#!/usr/bin/env python3
"""Wi-Fi (2026-10-14): firmware in the initramfs, key format pinned.

The 2026-10-13 card had no NWM firmware in its initramfs ("request firmware
failed ath6k/AR6002/nwm/stub_data.bin"), so the AR6014 never ran.  And the
driver swept four WMI_ADD_CIPHER_KEY formats one per association, starting
with two that lose the AP ~1.5 s after the keys go in; only row 2,
"dsi-45-u0" (the 45-byte form, PTK usage 0 / GTK usage 1, as the DSi library
dsiwifi sends it), kept the link up and delivered traffic on hardware
(2026-10-04 log).  Row 2 is now the default.

DHCP (2026-10-14 hardware boot): the link came up and dhcpcd leased
192.168.0.38/24, but /bin/dhcpcd_wlan0.sh published mask 0.0.0.0 and no
DNS (it took the netmask from the default route, a second before the lease
existed), so Android had no usable network.  The publisher now reads
dhcpcd's DHCPACK file; this runs it on the host against a stub dhcpcd.
"""
import struct
import tempfile
import time
from t3ds import *

cfg = read(KERNEL / "drivers/staging/ath6k_legacy/os/linux/cfg80211.c")
check("#define N3DS_KEYVAR_DEFAULT 2" in cfg, "default key variant is not 2")
check("static int n3ds_keyvar = N3DS_KEYVAR_DEFAULT;" in cfg, "n3ds_keyvar does not default to the pin")
check("static int n3ds_keyvar_current = N3DS_KEYVAR_DEFAULT;" in cfg,
      "the first association does not use the pinned variant")
check('{ "dsi-45-u0",  PAIRWISE_USAGE, 45 },' in cfg, "variant row 2 is no longer dsi-45-u0")
rows = cfg[cfg.index("n3ds_key_variants[] = {"):cfg.index("};", cfg.index("n3ds_key_variants[] = {"))]
check(rows.count("{ \"") == 4 and rows.index('"dsi-45-u0"') > rows.index('"dsi-51-u0"'),
      "variant table reordered: the pin (index 2) would point elsewhere")
check(order(cfg, "ar6k_cfg80211_add_key", "if (n3ds_keyvar >= 0 && n3ds_keyvar < N3DS_KEY_VARIANTS)",
            "n3ds_keyvar_current = n3ds_keyvar;", "wmi_addKey_cmd_ex"),
      "add_key() does not apply a runtime pin before sending the key")

init = read(SCRIPTS / "mkinitramfs.sh")
check('NWM="${NWM_DIR:-$REPO_DIR/firmware/ath6k/AR6002/nwm}"' in init, "initramfs firmware source moved")
nwm = REPO / "firmware/ath6k/AR6002/nwm"
sizes = {"stub_data.bin": 0x38, "stub_code.bin": 0x316, "database.bin": 0x1E8}
if nwm.is_dir() and any(nwm.iterdir()):
    for f, n in sizes.items():
        p = nwm / f
        check(p.exists() and p.stat().st_size == n,
              f"firmware/ath6k/AR6002/nwm/{f} missing or not {n} bytes (GBATEK block size)")
    check((nwm / "main_type4.bin").exists(), "firmware/ath6k/AR6002/nwm/main_type4.bin missing")
else:
    print("test_wifi: note: firmware/ath6k/AR6002/nwm is empty here (console-specific, never committed)")

# --- DHCP property publisher ---
marker = 'cat > "$ROOT/bin/dhcpcd_wlan0.sh" <<\'PUBLISHER\'\n'
pub = init[init.index(marker) + len(marker):init.index("\nPUBLISHER\n", init.index(marker)) + 1]
check('$2=="00000000" {print $3" "$8' not in pub, "netmask taken from the default route again (always 0.0.0.0)")
check('setprop dhcp.$IFACE.dns1' in pub, "dhcp.wlan0.dns1 is not published (Android configured dns1 0.0.0.0)")


def ack(ip, mask, gw, dns, lease, server):
    q = lambda a: bytes(int(x) for x in a.split("."))
    m = bytearray(236)
    m[0:4] = b"\x02\x01\x06\x00"
    m[16:20] = q(ip)
    m += b"\x63\x82\x53\x63"
    m += b"\x35\x01\x05" + b"\x36\x04" + q(server) + b"\x33\x04" + struct.pack(">I", lease)
    m += b"\x00"  # a pad option
    m += b"\x01\x04" + q(mask) + b"\x03\x04" + q(gw)
    m += bytes([6, 4 * len(dns)]) + b"".join(q(d) for d in dns)
    m += b"\x0f\x03lan\xff"
    return bytes(m)


if have("busybox"):
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        dh, props, bin_ = td / "dhcp", td / "props", td / "bin"
        for d in (dh, props, bin_):
            d.mkdir()
        (dh / "dhcpcd-wlan0.lease").write_bytes(ack("10.9.9.9", "255.0.0.0", "10.0.0.1", ["10.0.0.1"], 7, "10.0.0.1"))
        pkt = td / "ack.bin"
        pkt.write_bytes(ack("192.168.0.38", "255.255.255.0", "192.168.0.1",
                            ["192.168.0.1", "8.8.8.8"], 86400, "192.168.0.1"))
        (bin_ / "setprop").write_text(f'#!/bin/sh\nprintf %s "$2" > {props}/"$1"\n')
        (bin_ / "getprop").write_text(f'#!/bin/sh\ncat {props}/"$1" 2>/dev/null\n')
        # dhcpcd: leases 1 s in, then lives on a little (the wrapper waits for it)
        (bin_ / "dhcpcd").write_text(f'#!/bin/sh\nsleep 1\ncp {pkt} {dh}/dhcpcd-wlan0.lease\nsleep 2\n')
        for f in bin_.iterdir():
            f.chmod(0o755)
        s2 = (pub.replace("#!/system/bin/sh", "#!/bin/sh")
                 .replace("/data/misc/dhcp", str(dh))
                 .replace("/system/bin/dhcpcd ", f"{bin_}/dhcpcd ")
                 .replace("/system/bin/log", "true")
                 .replace("/dev/kmsg", "/dev/null")
                 .replace("/bin/pidof dhcpcd", "true")
                 .replace("BB=/bin/busybox", "BB=busybox")
                 .replace("chown 1014:1014", "true"))
        w = td / "dhcpcd_wlan0.sh"
        w.write_text(s2)
        t0 = time.time()
        r = run(["busybox", "sh", str(w), "wlan0"], timeout=30,
                env={"PATH": f"{bin_}:/usr/bin:/bin"})
        check(r.returncode == 0, f"publisher failed: {r.stdout}{r.stderr}")
        got = {p.name: p.read_text() for p in props.iterdir()}
        want = {"dhcp.wlan0.result": "ok", "dhcp.wlan0.ipaddress": "192.168.0.38",
                "dhcp.wlan0.mask": "255.255.255.0", "dhcp.wlan0.gateway": "192.168.0.1",
                "dhcp.wlan0.dns1": "192.168.0.1", "dhcp.wlan0.dns2": "8.8.8.8",
                "dhcp.wlan0.server": "192.168.0.1", "dhcp.wlan0.leasetime": "86400",
                "net.dns1": "192.168.0.1", "dhcpcd_wlan0": "running"}
        for k, v in want.items():
            check(got.get(k) == v, f"publisher set {k}={got.get(k)!r}, want {v!r} (stale lease not removed?)")
        check(time.time() - t0 < 9, "publisher waited for the 10 s interface fallback despite a lease packet")
else:
    print("test_wifi: note: no host busybox, DHCP publisher not run")
done()
