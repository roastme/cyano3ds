#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-preinit.py - do the DSi/3DS pre-scan init for AR6002.

The AR6014 (hw2) firmware is Nintendo's DSi/AR6002 firmware.  The one
known-working host is nocash's wifiboot (`WIFICORE.A22`, `sdio_prepare_scanning`
+ `sdio_scan_channel`), which first does a small "prepare scanning" step before
any scan:

    WMI_TARGET_ERROR_REPORT_BITMASK (0x22) = 0x7F   enable error reporting
    WMIX_DBGLOG_CFG_MODULE (0x2E:0x2009)  = clear  stop the bulky DBGLOG feed
    WMI_SET_HB_CHALLENGE_RESP_PARAMS (0x47)        already sent at init
    WMI_GET_CHANNEL_LIST (0x0E)                    ask the reg-domain channels

and then builds WMI_SET_CHANNEL_PARAMS from the *reply* of the channel-list
query, not from a hard-coded list.  GBATEK is explicit that a channel the
firmware does not have enabled ("Some (or all?) DSi's have only channel 1-11
enabled ... throw WMI_CMDERROR_EVENT when trying to set channel 12,13,14") can
make the firmware tune nothing at all -- which is exactly the observed symptom:
the scan runs for the full 105 ms/channel dwell and completes with status 0 but
never emits a single WMI_BSSINFO.

This script:

  * adds `reg_ch_list[32]`, `reg_num_ch`, `reg_ch_valid` to `struct ath6kl`;
  * makes `ath6kl_wmi_ch_list_reply_rx()` store the reply (with a 2.4 GHz
    sanity check; a malformed reply keeps the 1..13 fallback);
  * adds `ath6kl_wmi_get_channel_list_cmd()` and
    `ath6kl_wmi_set_target_error_report_bitmask_cmd()`;
  * runs the pre-scan init in the AR6002 branch of `ath6kl_cfg80211_scan()` and
    uses the firmware's own channel list for `WMI_SET_CHANNEL_PARAMS`.

AR6003/AR6004 are untouched.  Idempotent.
"""

import sys
import os
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-preinit.py: " + msg)


COREH_ANCHOR = "\tu16 last_ch;\n"
COREH_ADD = (
    "\n"
    "\t/* [3ds] AR6002/AR6014: WMI_GET_CHANNEL_LIST reply cache */\n"
    "\tu16 reg_ch_list[32];\n"
    "\tu8 reg_num_ch;\n"
    "\tbool reg_ch_valid;\n"
)

WMIH_ANCHOR = (
    "int ath6kl_wmi_set_channel_params_cmd(struct wmi *wmi, u8 if_idx,\n"
    "\t\t\t\t      u8 scan_param, u8 phy_mode,\n"
    "\t\t\t\t      u8 num_chan, const u16 *ch_list);\n"
)
WMIH_ADD = (
    "int ath6kl_wmi_get_channel_list_cmd(struct wmi *wmi, u8 if_idx);\n"
    "int ath6kl_wmi_set_target_error_report_bitmask_cmd(struct wmi *wmi,\n"
    "\t\t\t\t\t\t   u32 bitmask);\n"
)

REPLY_OLD = (
    "static int ath6kl_wmi_ch_list_reply_rx(struct wmi *wmi, u8 *datap, int len)\n"
    "{\n"
    "\tif (len < sizeof(struct wmi_channel_list_reply))\n"
    "\t\treturn -EINVAL;\n"
    "\n"
    "\tath6kl_wakeup_event(wmi->parent_dev);\n"
    "\n"
    "\treturn 0;\n"
    "}\n"
)
REPLY_NEW = (
    "static int ath6kl_wmi_ch_list_reply_rx(struct wmi *wmi, u8 *datap, int len)\n"
    "{\n"
    "\t/* [3ds] AR6002 pre-scan init (fix-ath6kl-ar6002-preinit.py): keep\n"
    "\t * the reg-domain channel list so the AR6002 scan can configure the\n"
    "\t * firmware with the channels it really enables. */\n"
    "\tstruct wmi_channel_list_reply *reply =\n"
    "\t\t(struct wmi_channel_list_reply *) datap;\n"
    "\tstruct ath6kl *ar = wmi->parent_dev;\n"
    "\tu8 i, n;\n"
    "\tbool valid;\n"
    "\n"
    "\tif (len < sizeof(struct wmi_channel_list_reply))\n"
    "\t\treturn -EINVAL;\n"
    "\n"
    "\tn = reply->num_ch;\n"
    "\tif (n > ARRAY_SIZE(ar->reg_ch_list))\n"
    "\t\tn = ARRAY_SIZE(ar->reg_ch_list);\n"
    "\tif (len < 2 + (int) n * 2)\n"
    "\t\tn = (len - 2) / 2;\n"
    "\tfor (i = 0; i < n; i++)\n"
    "\t\tar->reg_ch_list[i] = le16_to_cpu(reply->ch_list[i]);\n"
    "\tar->reg_num_ch = n;\n"
    "\n"
    "\t/* Only accept a plausible 2.4 GHz list; a malformed reply keeps\n"
    "\t * the port's 1..13 fallback instead of configuring the firmware\n"
    "\t * with nonsense channels. */\n"
    "\tvalid = (n > 0);\n"
    "\tfor (i = 0; i < n; i++) {\n"
    "\t\tif (ar->reg_ch_list[i] < 2400 ||\n"
    "\t\t    ar->reg_ch_list[i] > 2500)\n"
    "\t\t\tvalid = false;\n"
    "\t}\n"
    "\tar->reg_ch_valid = valid;\n"
    "\n"
    "\tath6kl_info(\"AR6002/AR6014: channel list reply n=%u (%u..%u) valid=%d\\n\",\n"
    "\t\t    ar->reg_num_ch, ar->reg_num_ch ? ar->reg_ch_list[0] : 0,\n"
    "\t\t    ar->reg_num_ch ? ar->reg_ch_list[n - 1] : 0, valid);\n"
    "\n"
    "\tath6kl_wakeup_event(wmi->parent_dev);\n"
    "\n"
    "\treturn 0;\n"
    "}\n"
)

WMI_ANCHOR = "int ath6kl_wmi_get_stats_cmd(struct wmi *wmi, u8 if_idx)\n"
WMI_ADD = (
    "/* [3ds] AR6002 pre-scan init (fix-ath6kl-ar6002-preinit.py): ask the\n"
    " * firmware for its reg-domain channel list (reply arrives as the\n"
    " * WMI_GET_CHANNEL_LIST_CMDID id 0x0E event). */\n"
    "int ath6kl_wmi_get_channel_list_cmd(struct wmi *wmi, u8 if_idx)\n"
    "{\n"
    "\tstruct sk_buff *skb;\n"
    "\tint ret;\n"
    "\n"
    "\tskb = ath6kl_wmi_get_new_buf(0);\n"
    "\tif (!skb)\n"
    "\t\treturn -ENOMEM;\n"
    "\n"
    "\tret = ath6kl_wmi_cmd_send(wmi, if_idx, skb,\n"
    "\t\t\t\t  WMI_GET_CHANNEL_LIST_CMDID, NO_SYNC_WMIFLAG);\n"
    "\treturn ret;\n"
    "}\n"
    "\n"
    "/* [3ds] AR6002 pre-scan init (fix-ath6kl-ar6002-preinit.py): enable\n"
    " * firmware error reporting (nocash's wifiboot sends 0x7F). */\n"
    "int ath6kl_wmi_set_target_error_report_bitmask_cmd(struct wmi *wmi,\n"
    "\t\t\t\t\t\t   u32 bitmask)\n"
    "{\n"
    "\tstruct sk_buff *skb;\n"
    "\t__le32 *cmd;\n"
    "\tint ret;\n"
    "\n"
    "\tskb = ath6kl_wmi_get_new_buf(sizeof(*cmd));\n"
    "\tif (!skb)\n"
    "\t\treturn -ENOMEM;\n"
    "\n"
    "\tcmd = (__le32 *) skb->data;\n"
    "\t*cmd = cpu_to_le32(bitmask);\n"
    "\n"
    "\tret = ath6kl_wmi_cmd_send(wmi, 0, skb,\n"
    "\t\t\t\t  WMI_TARGET_ERROR_REPORT_BITMASK_CMDID,\n"
    "\t\t\t\t  NO_SYNC_WMIFLAG);\n"
    "\treturn ret;\n"
    "}\n"
    "\n"
)

CFG_ANCHOR = (
    "\t\tconst u8 *probed_ssid = NULL;\n"
    "\n"
    "\t\tath6kl_cfg80211_sscan_disable(vif);\n"
)
CFG_ADD = (
    "\t\tconst u8 *probed_ssid = NULL;\n"
    "\n"
    "\t\tath6kl_cfg80211_sscan_disable(vif);\n"
    "\n"
    "\t\t/* [3ds] AR6002 pre-scan init (fix-ath6kl-ar6002-preinit.py):\n"
    "\t\t * nocash's wifiboot runs this before any scan.  Ask the\n"
    "\t\t * firmware which channels its reg-domain enables and use\n"
    "\t\t * that list below; a channel the firmware has not enabled\n"
    "\t\t * can make it tune nothing and report no BSSINFO. */\n"
    "\t\tath6kl_wmi_set_target_error_report_bitmask_cmd(ar->wmi, 0x7f);\n"
    "\t\tath6kl_wmi_config_debug_module_cmd(ar->wmi, 0xffffffff, 0);\n"
    "\t\tar->reg_ch_valid = false;\n"
    "\t\tath6kl_wmi_get_channel_list_cmd(ar->wmi, vif->fw_vif_idx);\n"
    "\t\twait_event_interruptible_timeout(ar->event_wq,\n"
    "\t\t\t\t\t\t ar->reg_ch_valid,\n"
    "\t\t\t\t\t\t msecs_to_jiffies(200));\n"
    "\t\tath6kl_info(\"AR6002/AR6014: preinit ch valid=%d n=%u\\n\",\n"
    "\t\t\t    ar->reg_ch_valid, ar->reg_num_ch);\n"
)

CFG_CHAN_OLD = (
    "\t\tret = ath6kl_wmi_set_channel_params_cmd(ar->wmi,\n"
    "\t\t\t\t\t\t\tvif->fw_vif_idx,\n"
    "\t\t\t\t\t\t\t0, WMI_11G_MODE,\n"
    "\t\t\t\t\t\t\tARRAY_SIZE(dsi_chan_2ghz),\n"
    "\t\t\t\t\t\t\tdsi_chan_2ghz);\n"
)
CFG_CHAN_NEW = (
    "\t\tret = ath6kl_wmi_set_channel_params_cmd(ar->wmi,\n"
    "\t\t\t\t\t\t\tvif->fw_vif_idx,\n"
    "\t\t\t\t\t\t\t0, WMI_11G_MODE,\n"
    "\t\t\t\t\t\t\tar->reg_ch_valid ?\n"
    "\t\t\t\t\t\t\t  ar->reg_num_ch :\n"
    "\t\t\t\t\t\t\t  ARRAY_SIZE(dsi_chan_2ghz),\n"
    "\t\t\t\t\t\t\tar->reg_ch_valid ?\n"
    "\t\t\t\t\t\t\t  ar->reg_ch_list :\n"
    "\t\t\t\t\t\t\t  dsi_chan_2ghz);\n"
)


def patch_text(path, subs, already_marker=None):
    s = path.read_text()
    if already_marker and already_marker in s:
        return False, "already applied"
    for old, new in subs:
        if new in s and old not in s:
            continue
        if old not in s:
            die("%s: anchor not found:\n%s" % (path.name, old[:200]))
        s = s.replace(old, new, 1)
    path.write_text(s)
    return True, "patched"


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.path.expanduser("~/p3ds/src/linux-3ds"))
    ath = kd / "drivers/net/wireless/ath/ath6kl"
    if not ath.is_dir():
        die("no %s" % ath)

    coreh = ath / "core.h"
    wmih = ath / "wmi.h"
    wmic = ath / "wmi.c"
    cfg = ath / "cfg80211.c"
    for p in (coreh, wmih, wmic, cfg):
        if not p.is_file():
            die("no %s" % p)

    changed = []
    ok, why = patch_text(coreh, [(COREH_ANCHOR, COREH_ANCHOR + COREH_ADD)],
                         already_marker="reg_ch_valid;")
    if ok:
        changed.append("core.h: reg_ch_list")

    ok, why = patch_text(wmih, [(WMIH_ANCHOR, WMIH_ANCHOR + WMIH_ADD)],
                         already_marker="ath6kl_wmi_get_channel_list_cmd")
    if ok:
        changed.append("wmi.h: prototypes")

    ok, why = patch_text(wmic, [(REPLY_OLD, REPLY_NEW),
                                (WMI_ANCHOR, WMI_ADD + WMI_ANCHOR)],
                         already_marker="channel list reply n=%u")
    if ok:
        changed.append("wmi.c: reply parser + senders")

    ok, why = patch_text(cfg, [(CFG_ANCHOR, CFG_ADD),
                               (CFG_CHAN_OLD, CFG_CHAN_NEW)],
                         already_marker="preinit ch valid")
    if ok:
        changed.append("cfg80211.c: AR6002 pre-scan init")

    if changed:
        print("fix-ath6kl-ar6002-preinit.py: " + "; ".join(changed))
    else:
        print("fix-ath6kl-ar6002-preinit.py: already applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
