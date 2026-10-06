#!/usr/bin/env python3
"""
fix-ath6kl-phycap.py - accept the Nintendo AR6014's WMI_READY phy capability.

The AR6002/hw2 WMI_READY event on the 3DS carries phyCapability = 0x00 (the
0Ch DSi layout, GBATEK "WMI Misc Events"); GBATEK documents the value as 02h
(11G), and on the 3DS the ARM11 NWM downloader normally patches in a 10h
variant that does set it.  We upload the plain NWM Main image, so we get 0x00.

ath6kl_cfg80211_init() switches on ar->hw.cap and, for anything outside
WMI_11A_CAP..WMI_11AGN_CAP, does:

        default:
                ath6kl_err("invalid phy capability!\\n");
                return -EINVAL;

That aborts ath6kl_core_init() before ath6kl_interface_add(), so no wlan0 is
ever registered - and Android's WifiService then reports "Failed to load
Wi-Fi driver." / Settings shows "Error".

Sanitise the capability for the AR6002 target in ath6kl_ready_event(): the
AR6014 is a 2.4 GHz 11g part, so fall back to WMI_11G_CAP when the firmware
reports an invalid value.  Idempotent.
"""

import sys
import pathlib

def die(msg):
    sys.exit("fix-ath6kl-phycap.py: " + msg)

def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    p = kd / "drivers/net/wireless/ath/ath6kl/main.c"
    if not p.exists():
        die("no ath6kl main.c at %s" % p)

    s = p.read_text()
    if "WMI_READY phy cap" in s:
        print("    main.c: already patched")
        return

    old = ("\tstruct ath6kl *ar = devt;\n"
           "\n"
           "\tmemcpy(ar->mac_addr, datap, ETH_ALEN);\n")
    new = ("\tstruct ath6kl *ar = devt;\n"
           "\n"
           "\t/* The Nintendo AR6014 (hw2) firmware reports phyCapability 0\n"
           "\t * in its 0Ch WMI_READY event.  An invalid capability makes\n"
           "\t * ath6kl_cfg80211_init() fail and no wlan0 is registered, so\n"
           "\t * fall back to the AR6014's real 2.4 GHz capability. */\n"
           "\tif (ar->target_type == TARGET_TYPE_AR6002 &&\n"
           "\t    (cap < WMI_11A_CAP || cap > WMI_11AGN_CAP)) {\n"
           "\t\tath6kl_warn(\"AR6002/AR6014: WMI_READY phy cap %u invalid, assuming 11G\\n\",\n"
           "\t\t\t    cap);\n"
           "\t\tcap = WMI_11G_CAP;\n"
           "\t}\n"
           "\n"
           "\tmemcpy(ar->mac_addr, datap, ETH_ALEN);\n")
    if old not in s:
        die("main.c: ath6kl_ready_event anchor not found")
    p.write_text(s.replace(old, new, 1))
    print("    main.c: AR6002 invalid phy cap -> 11G")

if __name__ == "__main__":
    main()
