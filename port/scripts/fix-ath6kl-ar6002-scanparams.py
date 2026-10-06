#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-scanparams.py - the AR6002/AR6014 needs explicit scan params.

Symptom (card boot 2026-10-01 19:47, the first boot with the single-block SDIO
fix): Wi-Fi stays up, scans are issued and answered, but the scan finds nothing:

    wmi tx id 7 len 46                WMI_START_SCAN (14 channels, ch1..ch14)
    wmi rx raw 0a 10 00 00 00 00      WMI_SCAN_COMPLETE (status 0)
    ... and *no* WMI_BSSINFO_EVENT at all, so wpa_supplicant completes every
    scan with zero results and Settings never populates.

The scan ran from 196.112 s to 196.264 s -- 152 ms for 14 channels, ~11 ms per
channel.  That is far too short to hear a beacon (100 ms interval), i.e. the
firmware's per-channel dwell time is ~0, because mainline ath6kl never sends
`WMI_SET_SCAN_PARAMS_CMD` for a plain scan (it only sends it on connect, and
the AR6003/AR6004 firmware has usable defaults).

The DSi host does send it -- GBATEK *DSi Atheros Wifi - WMI Scan Functions* and
the nesdev DSi capture show the sequence

    WMI_SET_PROBED_SSID_CMD ...
    WMI_SET_SCAN_PARAMS_CMD FF FF FF FF FF FF 6A 00 6A 00 00 01 6A 00 00 00 00 00 00 00
    WMI_START_SCAN_CMD      00 00 00 00 00 00 00 00 14 00 00 00 00 00 00 00 00 00 00 00

i.e. 106 ms (`0x6A`) min/max active and passive dwell, `scan_ctrl_flags = 0x01`
(`CONNECT_SCAN_CTRL_FLAGS`).  mainline's `struct wmi_scan_params_cmd` has
exactly the DSi/AR6kSDK byte layout, so the values map 1:1.

This script adds, for `TARGET_TYPE_AR6002` only, an
`ath6kl_wmi_scanparams_cmd()` call at the head of `ath6kl_cfg80211_scan()`.
`ACTIVE_SCAN_CTRL_FLAGS` is enabled as well so the firmware sends probe
requests and does not depend solely on a 106 ms passive listen.

Idempotent.
"""

import sys
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-scanparams.py: " + msg)


def replace_once_or_applied(s, old, new, what):
    if new in s:
        return s, False
    if old not in s:
        die("anchor not found: %s" % what)
    return s.replace(old, new, 1), True


MARK = "failed to set AR6002 scan params"


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    cfg = kd / "drivers/net/wireless/ath/ath6kl/cfg80211.c"
    if not cfg.exists():
        die("no %s" % cfg)

    s = cfg.read_text()

    OLD = (
        "\tif (ret) {\n"
        "\t\tath6kl_err(\"failed to set Probe Request appie for scan\\n\");\n"
        "\t\treturn ret;\n"
        "\t}\n"
        "\n"
        "\t/*\n"
        "\t * Scan only the requested channels if the request specifies a set of\n"
    )
    NEW = (
        "\tif (ret) {\n"
        "\t\tath6kl_err(\"failed to set Probe Request appie for scan\\n\");\n"
        "\t\treturn ret;\n"
        "\t}\n"
        "\n"
        "\t/*\n"
        "\t * [3ds] AR6002/AR6014: the DSi/3DS firmware needs explicit\n"
        "\t * WMI_SET_SCAN_PARAMS before WMI_START_SCAN.  Unlike AR6003/6004\n"
        "\t * its defaults are a zero channel dwell, so the scan completes\n"
        "\t * ~11 ms per channel with no WMI_BSSINFO results at all.  These\n"
        "\t * values are the DSi host's own (GBATEK/nesdev capture): 106 ms\n"
        "\t * min/max active and passive dwell; active scan enabled so the\n"
        "\t * firmware sends probe requests.  See\n"
        "\t * fix-ath6kl-ar6002-scanparams.py.\n"
        "\t */\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\tret = ath6kl_wmi_scanparams_cmd(ar->wmi, vif->fw_vif_idx,\n"
        "\t\t\t\t\t\t0xFFFF, 0xFFFF, 0xFFFF,\n"
        "\t\t\t\t\t\t106, 106, 106, 0,\n"
        "\t\t\t\t\t\tCONNECT_SCAN_CTRL_FLAGS |\n"
        "\t\t\t\t\t\tACTIVE_SCAN_CTRL_FLAGS,\n"
        "\t\t\t\t\t\t0, 0);\n"
        "\t\tif (ret) {\n"
        "\t\t\tath6kl_err(\"failed to set AR6002 scan params: %d\\n\",\n"
        "\t\t\t\t   ret);\n"
        "\t\t\treturn ret;\n"
        "\t\t}\n"
        "\t}\n"
        "\n"
        "\t/*\n"
        "\t * Scan only the requested channels if the request specifies a set of\n"
    )

    s, c = replace_once_or_applied(s, OLD, NEW, "cfg80211.c scan params")
    if not c:
        print("fix-ath6kl-ar6002-scanparams.py: already applied")
        return 0

    cfg.write_text(s)
    print("fix-ath6kl-ar6002-scanparams.py: patched ath6kl_cfg80211_scan()")
    return 0


if __name__ == "__main__":
    sys.exit(main())
