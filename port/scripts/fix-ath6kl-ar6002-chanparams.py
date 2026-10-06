#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-chanparams.py - send WMI_SET_CHANNEL_PARAMS before a scan.

The working
devkitPro/calico DSi AR6K driver sends `WMI_SET_CHANNEL_PARAMS` (0x11) with the
channel list and the 11G phy mode before every scan, and the AR6002/AR6014
firmware needs it to configure its radio (without it the scan dwells but never
reports a WMI_BSSINFO).

**This revision sends it at the TOP of `ath6kl_cfg80211_scan()`**, right after
`ath6kl_cfg80211_ready()`, instead of between WMI_SET_SCAN_PARAMS and
WMI_START_SCAN.  The first card boot with the original placement (zImage
`4649fab5`) showed the setup commands going out but **no WMI_START_SCAN at all**
(`wmi tx id 7` absent, no `htc tx ... len 44`, no WMI_SCAN_COMPLETE), so the
extra command queued at the tail of the HTC TX queue appears to have pushed the
start-scan packet out of the sequence.  Sending it first keeps the proven 20:56
scan sequence intact.

Also adds `ath6kl_info()` diagnostics at each step so the next boot's kmsg.log
shows exactly where the scan path goes (or stops).

For TARGET_TYPE_AR6002 only.  Idempotent, and repairs the old placement.
"""

import sys
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-chanparams.py: " + msg)


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    ath = kd / "drivers/net/wireless/ath/ath6kl"
    cfg = ath / "cfg80211.c"
    wmi_c = ath / "wmi.c"
    for p in (cfg, wmi_c):
        if not p.exists():
            die("no %s" % p)

    s = cfg.read_text()
    changed = []

    # ------------------------------------------------------------------
    # 1. remove the old placement (between scan params and start scan)
    # ------------------------------------------------------------------
    OLD_BLOCK = (
        "\t/*\n"
        "\t * [3ds] AR6002/AR6014: the DSi host sends\n"
        "\t * WMI_SET_CHANNEL_PARAMS (0x11) with the channel list and the\n"
        "\t * 11G phy mode before every scan, and starts the scan with\n"
        "\t * numChannels = 0.  The AR6002 firmware uses this to configure\n"
        "\t * its radio for the scan; without it the scan runs (and dwells)\n"
        "\t * but the radio never reports a single WMI_BSSINFO_EVENT.\n"
        "\t * Layout/sequence from devkitPro/calico's AR6K driver; see\n"
        "\t * fix-ath6kl-ar6002-chanparams.py.\n"
        "\t */\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\tstatic const u16 ar6002_chan_2ghz[] = {\n"
        "\t\t\t2412, 2417, 2422, 2427, 2432, 2437, 2442,\n"
        "\t\t\t2447, 2452, 2457, 2462, 2467, 2472,\n"
        "\t\t};\n"
        "\t\tconst u16 *cp = channels;\n"
        "\t\tu8 nc = n_channels;\n"
        "\n"
        "\t\tif (!nc) {\n"
        "\t\t\tcp = ar6002_chan_2ghz;\n"
        "\t\t\tnc = ARRAY_SIZE(ar6002_chan_2ghz);\n"
        "\t\t}\n"
        "\n"
        "\t\tret = ath6kl_wmi_set_channel_params_cmd(ar->wmi,\n"
        "\t\t\t\t\t\t\tvif->fw_vif_idx,\n"
        "\t\t\t\t\t\t\t0, WMI_11G_MODE,\n"
        "\t\t\t\t\t\t\tnc, cp);\n"
        "\t\tif (ret) {\n"
        "\t\t\tath6kl_err(\"failed to set AR6002 channel params: %d\\n\",\n"
        "\t\t\t\t   ret);\n"
        "\t\t\tkfree(channels);\n"
        "\t\t\treturn ret;\n"
        "\t\t}\n"
        "\t}\n"
        "\n"
    )
    if OLD_BLOCK in s:
        s = s.replace(OLD_BLOCK, "", 1)
        changed.append("removed old chanparams block")

    # ------------------------------------------------------------------
    # 2. insert the top-of-scan block + step diagnostics
    # ------------------------------------------------------------------
    if "AR6002/AR6014: scan enter" not in s:
        OLD = (
            "\tif (!ath6kl_cfg80211_ready(vif))\n"
            "\t\treturn -EIO;\n"
            "\n"
            "\tath6kl_cfg80211_sscan_disable(vif);\n"
        )
        NEW = (
            "\tif (!ath6kl_cfg80211_ready(vif))\n"
            "\t\treturn -EIO;\n"
            "\n"
            "\t/*\n"
            "\t * [3ds] AR6002/AR6014: the DSi host sends\n"
            "\t * WMI_SET_CHANNEL_PARAMS (0x11) with the channel list and the\n"
            "\t * 11G phy mode before a scan; the AR6002 firmware uses it to\n"
            "\t * configure its radio for the scan, and without it the scan\n"
            "\t * dwells but reports no WMI_BSSINFO.  Send it first, before\n"
            "\t * the rest of the scan setup, so the proven scan sequence is\n"
            "\t * unchanged.  Layout from devkitPro/calico's AR6K driver.\n"
            "\t */\n"
            "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
            "\t\tstatic const u16 ar6002_chan_2ghz[] = {\n"
            "\t\t\t2412, 2417, 2422, 2427, 2432, 2437, 2442,\n"
            "\t\t\t2447, 2452, 2457, 2462, 2467, 2472,\n"
            "\t\t};\n"
            "\t\tconst u16 *cp = ar6002_chan_2ghz;\n"
            "\t\tu8 nc = ARRAY_SIZE(ar6002_chan_2ghz);\n"
            "\t\tu16 req_ch[WMI_MAX_CHANNELS];\n"
            "\t\tint ci;\n"
            "\n"
            "\t\tif (request->n_channels > 0 &&\n"
            "\t\t    request->n_channels <= WMI_MAX_CHANNELS) {\n"
            "\t\t\tfor (ci = 0; ci < request->n_channels; ci++)\n"
            "\t\t\t\treq_ch[ci] = request->channels[ci]->center_freq;\n"
            "\t\t\tcp = req_ch;\n"
            "\t\t\tnc = request->n_channels;\n"
            "\t\t}\n"
            "\n"
            "\t\tath6kl_info(\"AR6002/AR6014: scan enter n_channels=%d nc=%u\\n\",\n"
            "\t\t\t    request->n_channels, nc);\n"
            "\t\tret = ath6kl_wmi_set_channel_params_cmd(ar->wmi,\n"
            "\t\t\t\t\t\t\tvif->fw_vif_idx,\n"
            "\t\t\t\t\t\t\t0, WMI_11G_MODE,\n"
            "\t\t\t\t\t\t\tnc, cp);\n"
            "\t\tath6kl_info(\"AR6002/AR6014: scan chanparams ret=%d\\n\", ret);\n"
            "\t\tif (ret)\n"
            "\t\t\treturn ret;\n"
            "\t}\n"
            "\n"
            "\tath6kl_cfg80211_sscan_disable(vif);\n"
        )
        if OLD not in s:
            die("cfg80211.c: ready-check anchor not found")
        s = s.replace(OLD, NEW, 1)
        changed.append("chanparams at top of scan + diag")

    if "AR6002/AR6014: scan beginscan" not in s:
        OLD = (
            "\tvif->scan_req = request;\n"
            "\n"
            "\tret = ath6kl_wmi_beginscan_cmd(ar->wmi, vif->fw_vif_idx,\n"
        )
        NEW = (
            "\tvif->scan_req = request;\n"
            "\n"
            "\tath6kl_info(\"AR6002/AR6014: scan beginscan n_channels=%d\\n\",\n"
            "\t\t    n_channels);\n"
            "\tret = ath6kl_wmi_beginscan_cmd(ar->wmi, vif->fw_vif_idx,\n"
        )
        if OLD not in s:
            die("cfg80211.c: beginscan anchor not found")
        s = s.replace(OLD, NEW, 1)
        s = s.replace(
            "\tif (ret) {\n"
            "\t\tath6kl_err(\"failed to start scan: %d\\n\", ret);\n"
            "\t\tvif->scan_req = NULL;\n"
            "\t}\n"
            "\n"
            "\tkfree(channels);\n",
            "\tath6kl_info(\"AR6002/AR6014: scan beginscan ret=%d\\n\", ret);\n"
            "\tif (ret) {\n"
            "\t\tath6kl_err(\"failed to start scan: %d\\n\", ret);\n"
            "\t\tvif->scan_req = NULL;\n"
            "\t}\n"
            "\n"
            "\tkfree(channels);\n",
            1,
        )
        changed.append("beginscan diagnostics")
    cfg.write_text(s)

    # ------------------------------------------------------------------
    # 3. wmi.c: log inside ath6kl_wmi_startscan_cmd()
    # ------------------------------------------------------------------
    s = wmi_c.read_text()
    if "AR6002/AR6014: startscan" not in s:
        OLD = (
            "{\n"
            "\tstruct sk_buff *skb;\n"
            "\tstruct wmi_start_scan_cmd *sc;\n"
            "\ts8 size;\n"
            "\tint i, ret;\n"
            "\n"
            "\tsize = sizeof(struct wmi_start_scan_cmd);\n"
        )
        NEW = (
            "{\n"
            "\tstruct sk_buff *skb;\n"
            "\tstruct wmi_start_scan_cmd *sc;\n"
            "\ts8 size;\n"
            "\tint i, ret;\n"
            "\n"
            "\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002)\n"
            "\t\tath6kl_info(\"AR6002/AR6014: startscan type=%d num_chan=%d\\n\",\n"
            "\t\t\t    scan_type, num_chan);\n"
            "\n"
            "\tsize = sizeof(struct wmi_start_scan_cmd);\n"
        )
        if OLD not in s:
            die("wmi.c: startscan anchor not found")
        s = s.replace(OLD, NEW, 1)
        changed.append("startscan diagnostic")
    wmi_c.write_text(s)

    if changed:
        print("fix-ath6kl-ar6002-chanparams.py: " + "; ".join(changed))
    else:
        print("fix-ath6kl-ar6002-chanparams.py: already applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
