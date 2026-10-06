#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-dsiseq.py - use the DSi host's scan sequence for AR6002.

The AR6014 (hw2) firmware on the 3DS is Nintendo's DSi/AR6002 firmware.  The
one open-source host driver that actually drives it is devkitPro/calico's AR6K
driver (`source/nds/arm7/wifi.twl.32.c`, `source/dev/ar6k/ar6k_wmi.c`).  Its
scan sequence is exactly five commands:

    WMI_SET_PROBED_SSID   (0x0A)  one entry: flags 0 (or the target SSID)
    WMI_SET_CHANNEL_PARAMS(0x11)  11G, the channels from WMI_GET_CHANNEL_LIST
    WMI_SET_SCAN_PARAMS   (0x08)  dwell 105 ms, scan_ctrl_flags CONNECT
    WMI_SET_BSS_FILTER    (0x09)  ALL_BSS_FILTER, ieMask 0
    WMI_START_SCAN        (0x07)  numChannels 0, homeDwellTime 20

mainline ath6kl instead fires ~21 commands for one scan: the 16-entry
"clear old probed SSIDs" loop in ath6kl_set_probed_ssids(), an APPIE command
(with ie_len 0), then an explicit 14-channel START_SCAN.  On hardware this
floods the small hw2 WMI/HTC command queue: the scan-params command is lost
and the firmware runs the scan with its default (near-zero) dwell, completing
in ~200 ms with no WMI_BSSINFO at all.  The 20:56 build (zImage 1cfbe1da)
that did send the scan params and dwelled 1.5 s still produced no BSSINFO, so
the exact DSi order is the next thing to match.

This script replaces the AR6002 scan path with the DSi sequence above and
leaves AR6003/AR6004 on the untouched mainline path.  It removes the two
earlier AR6002 blocks (fix-ath6kl-ar6002-chanparams.py's top-of-scan block and
fix-ath6kl-ar6002-scanparams.py's pre-beginscan block) so the sequence is sent
once, in order.  The BSSINFO v1 parser (fix-ath6kl-ar6002-bssinfo-v1.py) and
the startscan homeDwellTime=20 safety net are left in place.

Idempotent.
"""

import re
import sys
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-dsiseq.py: " + msg)


NEW_BLOCK = (
    "\t/*\n"
    "\t * [3ds] AR6002/AR6014 (DSi/3DS firmware): use exactly the DSi\n"
    "\t * host's scan sequence.  mainline's sequence (a 16-entry\n"
    "\t * probed-SSID clear, an APPIE command, then an explicit\n"
    "\t * 14-channel START_SCAN) floods the small hw2 WMI/HTC command\n"
    "\t * queue, the scan-params command is lost, and the firmware\n"
    "\t * scans with its default ~zero dwell and reports no WMI_BSSINFO.\n"
    "\t * devkitPro/calico's working DSi AR6K driver does, in order:\n"
    "\t *   WMI_SET_PROBED_SSID    one entry (flags 0, or target SSID)\n"
    "\t *   WMI_SET_CHANNEL_PARAMS 11G, channels 1..13\n"
    "\t *   WMI_SET_SCAN_PARAMS    dwell 105 ms, CONNECT flag\n"
    "\t *   WMI_SET_BSS_FILTER     ALL_BSS_FILTER, ieMask 0\n"
    "\t *   WMI_START_SCAN         numChannels 0, homeDwellTime 20\n"
    "\t * See fix-ath6kl-ar6002-dsiseq.py.\n"
    "\t */\n"
    "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
    "\t\tstatic const u16 dsi_chan_2ghz[] = {\n"
    "\t\t\t2412, 2417, 2422, 2427, 2432, 2437, 2442,\n"
    "\t\t\t2447, 2452, 2457, 2462, 2467, 2472,\n"
    "\t\t};\n"
    "\t\tu8 probed_flag = DISABLE_SSID_FLAG;\n"
    "\t\tu8 probed_len = 0;\n"
    "\t\tconst u8 *probed_ssid = NULL;\n"
    "\n"
    "\t\tath6kl_cfg80211_sscan_disable(vif);\n"
    "\n"
    "\t\tif (request->n_ssids > 0 && request->ssids[0].ssid_len) {\n"
    "\t\t\tprobed_flag = SPECIFIC_SSID_FLAG;\n"
    "\t\t\tprobed_len = request->ssids[0].ssid_len;\n"
    "\t\t\tprobed_ssid = request->ssids[0].ssid;\n"
    "\t\t}\n"
    "\n"
    "\t\tret = ath6kl_wmi_probedssid_cmd(ar->wmi, vif->fw_vif_idx, 0,\n"
    "\t\t\t\t\t\tprobed_flag, probed_len,\n"
    "\t\t\t\t\t\t(u8 *)probed_ssid);\n"
    "\t\tath6kl_info(\"AR6002/AR6014: dsi ssid ret=%d\\n\", ret);\n"
    "\t\tif (ret)\n"
    "\t\t\tgoto ar6002_scan_fail;\n"
    "\n"
    "\t\tret = ath6kl_wmi_set_channel_params_cmd(ar->wmi,\n"
    "\t\t\t\t\t\t\tvif->fw_vif_idx,\n"
    "\t\t\t\t\t\t\t0, WMI_11G_MODE,\n"
    "\t\t\t\t\t\t\tARRAY_SIZE(dsi_chan_2ghz),\n"
    "\t\t\t\t\t\t\tdsi_chan_2ghz);\n"
    "\t\tath6kl_info(\"AR6002/AR6014: dsi chan ret=%d\\n\", ret);\n"
    "\t\tif (ret)\n"
    "\t\t\tgoto ar6002_scan_fail;\n"
    "\n"
    "\t\tret = ath6kl_wmi_scanparams_cmd(ar->wmi, vif->fw_vif_idx,\n"
    "\t\t\t\t\t\t0xFFFF, 0xFFFF, 0xFFFF,\n"
    "\t\t\t\t\t\t105, 105, 105, 0,\n"
    "\t\t\t\t\t\tCONNECT_SCAN_CTRL_FLAGS,\n"
    "\t\t\t\t\t\t0, 0);\n"
    "\t\tath6kl_info(\"AR6002/AR6014: dsi sparams ret=%d\\n\", ret);\n"
    "\t\tif (ret)\n"
    "\t\t\tgoto ar6002_scan_fail;\n"
    "\n"
    "\t\tret = ath6kl_wmi_bssfilter_cmd(ar->wmi, vif->fw_vif_idx,\n"
    "\t\t\t\t\t       ALL_BSS_FILTER, 0);\n"
    "\t\tath6kl_info(\"AR6002/AR6014: dsi bssfilter ret=%d\\n\", ret);\n"
    "\t\tif (ret)\n"
    "\t\t\tgoto ar6002_scan_fail;\n"
    "\n"
    "\t\tvif->scan_req = request;\n"
    "\n"
    "\t\tret = ath6kl_wmi_beginscan_cmd(ar->wmi, vif->fw_vif_idx,\n"
    "\t\t\t\t\t       WMI_LONG_SCAN, 0, false,\n"
    "\t\t\t\t\t       20, 0, 0, NULL,\n"
    "\t\t\t\t\t       request->no_cck, request->rates);\n"
    "\t\tif (ret)\n"
    "\t\t\tvif->scan_req = NULL;\n"
    "\n"
    "\t\tath6kl_info(\"AR6002/AR6014: dsi startscan ret=%d n_ssids=%d\\n\",\n"
    "\t\t\t    ret, request->n_ssids);\n"
    "\t\treturn ret;\n"
    "\n"
    "ar6002_scan_fail:\n"
    "\t\tath6kl_err(\"AR6002/AR6014: dsi scan seq failed: %d\\n\", ret);\n"
    "\t\treturn ret;\n"
    "\t}\n"
)


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    cfg = kd / "drivers/net/wireless/ath/ath6kl/cfg80211.c"
    if not cfg.exists():
        die("no %s" % cfg)

    s = cfg.read_text()
    changed = []

    old_a = "[3ds] AR6002/AR6014: the DSi host sends"
    old_b = "[3ds] AR6002/AR6014: the DSi/3DS firmware needs explicit"

    # Already applied *and* the provisional blocks are gone?  Then do nothing.
    # (apply-kernel-port.sh runs the provisional scripts before this one, so
    # on a re-apply their blocks reappear and must be stripped again.)
    if NEW_BLOCK in s and old_a not in s and old_b not in s:
        print("fix-ath6kl-ar6002-dsiseq.py: already applied")
        return 0

    # 0. remove any copy of the block this script inserts (a previous run of
    #    an earlier, buggy revision could have put it in the wrong function).
    if NEW_BLOCK in s:
        s = s.replace(NEW_BLOCK, "")
        changed.append("removed an existing dsiseq block")

    # Scope everything to ath6kl_cfg80211_scan() so the anchors cannot match
    # the many other functions that call ath6kl_cfg80211_ready().
    sig = "static int ath6kl_cfg80211_scan(struct wiphy *wiphy,"
    start = s.find(sig)
    if start < 0:
        die("cfg80211.c: ath6kl_cfg80211_scan() not found")
    end = s.find("\n}\n", start)
    if end < 0:
        die("cfg80211.c: ath6kl_cfg80211_scan() end not found")
    end += len("\n}\n")
    body = s[start:end]

    # 1. drop the top-of-scan chanparams block (fix-...-chanparams.py)
    body2 = re.sub(
        r"\n\t/\*.*?\[3ds\] AR6002/AR6014: the DSi host sends"
        r".*?\n\t\}\n",
        "\n", body, count=1, flags=re.S)
    if body2 != body:
        body = body2
        changed.append("removed old top-of-scan chanparams block")

    # 2. drop the pre-beginscan scanparams block (fix-...-scanparams.py)
    body2 = re.sub(
        r"\n\t/\*.*?\[3ds\] AR6002/AR6014: the DSi/3DS firmware needs"
        r" explicit.*?\n\t\}\n",
        "\n", body, count=1, flags=re.S)
    if body2 != body:
        body = body2
        changed.append("removed old pre-beginscan scanparams block")

    # 3. insert the DSi sequence right after the ready check
    anchor = ("\tif (!ath6kl_cfg80211_ready(vif))\n"
              "\t\treturn -EIO;\n"
              "\n")
    if anchor not in body:
        die("cfg80211.c: scan ready-check anchor not found")
    body = body.replace(anchor, anchor + NEW_BLOCK + "\n", 1)
    changed.append("inserted DSi scan sequence")

    s = s[:start] + body + s[end:]
    cfg.write_text(s)
    print("fix-ath6kl-ar6002-dsiseq.py: " + "; ".join(changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
