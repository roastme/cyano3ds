#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-bssinfo.py - make the AR6002/AR6014 actually report scan BSS.

Symptom (card boot 2026-10-01 20:1x, zImage 9cdcfdab = note-r scan-params fix):
now the scan runs for the right duration (~1.5 s for 14 channels, 106 ms dwell
from WMI_SET_SCAN_PARAMS) but still **no WMI_BSSINFO_EVENT** is ever emitted, so
wpa_supplicant completes every scan with zero results.

Two mainline behaviours gate the DSi/3DS firmware's BSSINFO reporting:

1. **Probed-SSID flags.**  `ath6kl_set_probed_ssids()` sends a broadcast scan's
   wildcard entry with `ANY_SSID_FLAG` (0x02).  melonDS' reverse-engineered
   DSi/AR6014 firmware (`src/DSi_NWifi.cpp`, `case 0x000A`) does:

       SendBSSInfo = flags == 0 || strcmp(ssid, WifiAP::APName) == 0;

   and `SendWMIBSSInfo()` returns immediately when `SendBSSInfo` is false.  So
   a non-zero probed-SSID flag for an SSID that does not match the AP (an empty
   wildcard never matches) suppresses **all** BSSINFO events.  The DSi host
   sends `WMI_SET_PROBED_SSID_CMD 00 00 00 00 ...` -- entry 0, flags 0
   (`DISABLE_SSID_FLAG`), len 0 -- and gets every beacon.  The port's driver
   sends flags 2 and gets nothing.

2. **`WMI_START_SCAN.homeDwellTime`.**  GBATEK (*WMI Scan Functions*):

       08h  homeDwellTime   Max duration in the home channel (msec)
       ...the DSi Browser receives one or more WMI_BSSINFO_EVENT's ... and,
       once when the DwellTime (mul5?) has elapsed, finally receives
       WMI_SCAN_COMPLETE_EVENT.

   `ath6kl_cfg80211_scan()` passes `home_dwell_time = 0`; the DSi host sends
   20.  melonDS' model also keys on it (`ScanTimer = scantime*8`, and no
   BSSINFO at all while `ScanTimer == 0`).

Fix, for `TARGET_TYPE_AR6002` only:

* `cfg80211.c ath6kl_set_probed_ssids()`: a wildcard (ssid_len == 0) entry is
  sent with `DISABLE_SSID_FLAG` instead of `ANY_SSID_FLAG` (and without
  `MATCH_SSID_FLAG`).
* `wmi.c ath6kl_wmi_startscan_cmd()`: a zero `home_dwell_time` becomes 20
  (the DSi value); an explicitly requested value is left alone.

Idempotent.
"""

import sys
import os
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-bssinfo.py: " + msg)


def replace_once_or_applied(s, old, new, what):
    if new in s:
        return s, False
    if old not in s:
        die("anchor not found: %s" % what)
    return s.replace(old, new, 1), True


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.path.expanduser("~/p3ds/src/linux-3ds"))
    cfg = kd / "drivers/net/wireless/ath/ath6kl/cfg80211.c"
    wmi = kd / "drivers/net/wireless/ath/ath6kl/wmi.c"
    for p in (cfg, wmi):
        if not p.exists():
            die("no %s" % p)

    n = 0

    # ---- 1. probed-SSID flags (cfg80211.c) --------------------------------
    s = cfg.read_text()
    OLD = (
        "\t\tif (ar->wiphy->max_match_sets != 0 && n_match_ssid == 0)\n"
        "\t\t\tssid_list[i].flag |= MATCH_SSID_FLAG;\n"
        "\t}\n"
    )
    NEW = (
        "\t\tif (ar->wiphy->max_match_sets != 0 && n_match_ssid == 0)\n"
        "\t\t\tssid_list[i].flag |= MATCH_SSID_FLAG;\n"
        "\n"
        "\t\t/*\n"
        "\t\t * [3ds] AR6002/AR6014: the DSi/3DS firmware only emits\n"
        "\t\t * WMI_BSSINFO when the probed-SSID entry has flags==0\n"
        "\t\t * (DISABLE_SSID_FLAG) or matches the target SSID.  A\n"
        "\t\t * wildcard entry with ANY_SSID_FLAG makes it report\n"
        "\t\t * nothing at all, so a broadcast scan silently yields\n"
        "\t\t * zero results.  The DSi host sends entry 0 with\n"
        "\t\t * flags=0.  See fix-ath6kl-ar6002-bssinfo.py.\n"
        "\t\t */\n"
        "\t\tif (ar->target_type == TARGET_TYPE_AR6002 &&\n"
        "\t\t    ssids[i].ssid_len == 0)\n"
        "\t\t\tssid_list[i].flag = DISABLE_SSID_FLAG;\n"
        "\t}\n"
    )
    s, c = replace_once_or_applied(s, OLD, NEW, "cfg80211.c probed ssid")
    if c:
        cfg.write_text(s)
        n += 1

    # ---- 2. home_dwell_time (wmi.c) ---------------------------------------
    s = wmi.read_text()
    OLD = (
        "\tsc = (struct wmi_start_scan_cmd *) skb->data;\n"
        "\tsc->scan_type = scan_type;\n"
        "\tsc->force_fg_scan = cpu_to_le32(force_fgscan);\n"
        "\tsc->is_legacy = cpu_to_le32(is_legacy);\n"
        "\tsc->home_dwell_time = cpu_to_le32(home_dwell_time);\n"
    )
    NEW = (
        "\t/*\n"
        "\t * [3ds] AR6002/AR6014: GBATEK calls homeDwellTime the \"max\n"
        "\t * duration in the home channel\"; with 0 the DSi/3DS firmware\n"
        "\t * reports no WMI_BSSINFO at all.  The DSi host sends 20.\n"
        "\t * See fix-ath6kl-ar6002-bssinfo.py.\n"
        "\t */\n"
        "\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002 &&\n"
        "\t    home_dwell_time == 0)\n"
        "\t\thome_dwell_time = 20;\n"
        "\n"
        "\tsc = (struct wmi_start_scan_cmd *) skb->data;\n"
        "\tsc->scan_type = scan_type;\n"
        "\tsc->force_fg_scan = cpu_to_le32(force_fgscan);\n"
        "\tsc->is_legacy = cpu_to_le32(is_legacy);\n"
        "\tsc->home_dwell_time = cpu_to_le32(home_dwell_time);\n"
    )
    s, c = replace_once_or_applied(s, OLD, NEW, "wmi.c home_dwell_time")
    if c:
        wmi.write_text(s)
        n += 1

    if n == 0:
        print("fix-ath6kl-ar6002-bssinfo.py: already applied")
    else:
        print("fix-ath6kl-ar6002-bssinfo.py: patched %d site(s)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
