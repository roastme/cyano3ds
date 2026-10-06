#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-bssinfo-v1.py - parse the AR6002/AR6014 16-byte BSSINFO header.

mainline ath6kl only knows the version>=2 `struct wmi_bss_info_hdr2`
(12 bytes: ch(2) frameType(1) snr(1) bssid(6) ieMask(2)).  The DSi/3DS
AR6002/hw2 firmware sends the *version<2* 16-byte header

    ch(2) frameType(1) snr(1) rssi(2) bssid(6) ieMask(4)

even though the WMI protocol version is 2 (so the header size cannot be derived
from it).  Two independent sources agree:

* **devkitPro/calico**, the open-source DSi system library whose AR6K driver
  works on real hardware: it writes `AR6K_WMI_PROTOCOL_VER 2` into
  hi_app_host_interest and yet defines

      typedef struct Ar6kWmiBssInfoHdr {
          u16 channel_mhz; u8 frame_type; s8 snr; s16 rssi;
          u8 bssid[6]; u32 ie_mask;
      } Ar6kWmiBssInfoHdr;          // 16 bytes

* **GBATEK** *DSi Atheros Wifi - WMI Misc Events*: "I observed the version<2
  variant on my DSi" for WMIevent(1004h) - WMI_BSSINFO_EVENT.

Without this, even a scan that finally reports BSSINFO would be parsed on a
4-byte shift: wrong BSSID and garbage IEs.

This script adds `struct wmi_bss_info_hdr_v1` to wmi.h and a per-target header
selection in `ath6kl_wmi_bssinfo_event_rx()` (wmi.c): AR6002 uses the 16-byte
v1 header, everything else keeps the existing 12-byte v2 header.

Idempotent.
"""

import sys
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-bssinfo-v1.py: " + msg)


def replace_once_or_applied(s, old, new, what):
    if new in s:
        return s, False
    if old not in s:
        die("anchor not found: %s" % what)
    return s.replace(old, new, 1), True


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    ath = kd / "drivers/net/wireless/ath/ath6kl"
    wmi_h = ath / "wmi.h"
    wmi_c = ath / "wmi.c"
    for p in (wmi_h, wmi_c):
        if not p.exists():
            die("no %s" % p)

    applied = 0

    # ------------------------------------------------------------------
    # wmi.h: the version<2 header
    # ------------------------------------------------------------------
    s = wmi_h.read_text()
    if "struct wmi_bss_info_hdr_v1" not in s:
        OLD = (
            "struct wmi_bss_info_hdr2 {\n"
            "\t__le16 ch; /* frequency in MHz */\n"
            "\n"
            "\t/* see, enum wmi_bi_ftype */\n"
            "\tu8 frame_type;\n"
            "\n"
            "\tu8 snr; /* note: rssi = snr - 95 dBm */\n"
            "\tu8 bssid[ETH_ALEN];\n"
            "\t__le16 ie_mask;\n"
            "} __packed;\n"
        )
        NEW = (
            "struct wmi_bss_info_hdr2 {\n"
            "\t__le16 ch; /* frequency in MHz */\n"
            "\n"
            "\t/* see, enum wmi_bi_ftype */\n"
            "\tu8 frame_type;\n"
            "\n"
            "\tu8 snr; /* note: rssi = snr - 95 dBm */\n"
            "\tu8 bssid[ETH_ALEN];\n"
            "\t__le16 ie_mask;\n"
            "} __packed;\n"
            "\n"
            "/*\n"
            " * [3ds] WMI_BSSINFO_EVENT, version<2 (16-byte) header.\n"
            " *\n"
            " * The DSi/3DS AR6002/AR6013/AR6014 firmware sends this layout (GBATEK\n"
            " * *DSi Atheros Wifi - WMI Misc Events*, and devkitPro/calico's\n"
            " * working DSi AR6K driver Ar6kWmiBssInfoHdr) even though the WMI\n"
            " * protocol version is 2.  mainline only knows the 12-byte header\n"
            " * above, so hw2 events must be parsed with this one.\n"
            " */\n"
            "struct wmi_bss_info_hdr_v1 {\n"
            "\t__le16 ch; /* frequency in MHz */\n"
            "\tu8 frame_type;\n"
            "\ts8 snr;\n"
            "\ts16 rssi;\n"
            "\tu8 bssid[ETH_ALEN];\n"
            "\t__le32 ie_mask;\n"
            "} __packed;\n"
        )
        s, c = replace_once_or_applied(s, OLD, NEW, "wmi.h v1 bssinfo header")
        applied += c
    wmi_h.write_text(s)

    # ------------------------------------------------------------------
    # wmi.c: per-target header selection in ath6kl_wmi_bssinfo_event_rx()
    # ------------------------------------------------------------------
    s = wmi_c.read_text()
    if "struct wmi_bss_info_hdr_v1" not in s:
        OLD = (
            "\tstruct wmi_bss_info_hdr2 *bih;\n"
            "\tu8 *buf;\n"
            "\tstruct ieee80211_channel *channel;\n"
            "\tstruct ath6kl *ar = wmi->parent_dev;\n"
            "\tstruct cfg80211_bss *bss;\n"
            "\n"
            "\tif (len <= sizeof(struct wmi_bss_info_hdr2))\n"
            "\t\treturn -EINVAL;\n"
            "\n"
            "\tbih = (struct wmi_bss_info_hdr2 *) datap;\n"
            "\tbuf = datap + sizeof(struct wmi_bss_info_hdr2);\n"
            "\tlen -= sizeof(struct wmi_bss_info_hdr2);\n"
            "\n"
            "\tath6kl_dbg(ATH6KL_DBG_WMI,\n"
            "\t\t   \"bss info evt - ch %u, snr %d, rssi %d, bssid \\\"%pM\\\" \"\n"
            "\t\t   \"frame_type=%d\\n\",\n"
            "\t\t   bih->ch, bih->snr, bih->snr - 95, bih->bssid,\n"
            "\t\t   bih->frame_type);\n"
            "\n"
            "\tif (bih->frame_type != BEACON_FTYPE &&\n"
            "\t    bih->frame_type != PROBERESP_FTYPE)\n"
            "\t\treturn 0; /* Only update BSS table for now */\n"
            "\n"
            "\tif (bih->frame_type == BEACON_FTYPE &&\n"
        )
        NEW = (
            "\tstruct wmi_bss_info_hdr2 *bih;\n"
            "\tu8 *buf;\n"
            "\tstruct ieee80211_channel *channel;\n"
            "\tstruct ath6kl *ar = wmi->parent_dev;\n"
            "\tstruct cfg80211_bss *bss;\n"
            "\tu16 ch;\n"
            "\tu8 frame_type;\n"
            "\ts8 snr;\n"
            "\tconst u8 *bssid;\n"
            "\tu32 hdr_len;\n"
            "\n"
            "\t/*\n"
            "\t * [3ds] AR6002/hw2 sends the version<2 16-byte BSSINFO header;\n"
            "\t * everything else keeps mainline's 12-byte version>=2 header.\n"
            "\t * See fix-ath6kl-ar6002-bssinfo-v1.py.\n"
            "\t */\n"
            "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
            "\t\tstruct wmi_bss_info_hdr_v1 *bih1;\n"
            "\n"
            "\t\tif (len <= sizeof(*bih1))\n"
            "\t\t\treturn -EINVAL;\n"
            "\n"
            "\t\tbih1 = (struct wmi_bss_info_hdr_v1 *) datap;\n"
            "\t\tch = le16_to_cpu(bih1->ch);\n"
            "\t\tframe_type = bih1->frame_type;\n"
            "\t\tsnr = bih1->snr;\n"
            "\t\tbssid = bih1->bssid;\n"
            "\t\thdr_len = sizeof(*bih1);\n"
            "\t} else {\n"
            "\t\tif (len <= sizeof(*bih))\n"
            "\t\t\treturn -EINVAL;\n"
            "\n"
            "\t\tbih = (struct wmi_bss_info_hdr2 *) datap;\n"
            "\t\tch = le16_to_cpu(bih->ch);\n"
            "\t\tframe_type = bih->frame_type;\n"
            "\t\tsnr = bih->snr;\n"
            "\t\tbssid = bih->bssid;\n"
            "\t\thdr_len = sizeof(*bih);\n"
            "\t}\n"
            "\n"
            "\tbuf = datap + hdr_len;\n"
            "\tlen -= hdr_len;\n"
            "\n"
            "\tath6kl_dbg(ATH6KL_DBG_WMI,\n"
            "\t\t   \"bss info evt - ch %u, snr %d, rssi %d, bssid \\\"%pM\\\" \"\n"
            "\t\t   \"frame_type=%d\\n\",\n"
            "\t\t   ch, snr, snr - 95, bssid, frame_type);\n"
            "\n"
            "\tif (frame_type != BEACON_FTYPE &&\n"
            "\t    frame_type != PROBERESP_FTYPE)\n"
            "\t\treturn 0; /* Only update BSS table for now */\n"
            "\n"
            "\tif (frame_type == BEACON_FTYPE &&\n"
        )
        s, c = replace_once_or_applied(s, OLD, NEW, "wmi.c bssinfo header select")
        applied += c

    if "channel = ieee80211_get_channel(ar->wiphy, ch);" not in s:
        OLD = (
            "\tchannel = ieee80211_get_channel(ar->wiphy, le16_to_cpu(bih->ch));\n"
            "\tif (channel == NULL)\n"
            "\t\treturn -EINVAL;\n"
            "\n"
            "\tif (len < 8 + 2 + 2)\n"
            "\t\treturn -EINVAL;\n"
            "\n"
            "\tif (bih->frame_type == BEACON_FTYPE &&\n"
            "\t    test_bit(CONNECTED, &vif->flags) &&\n"
            "\t    memcmp(bih->bssid, vif->bssid, ETH_ALEN) == 0) {\n"
        )
        NEW = (
            "\tchannel = ieee80211_get_channel(ar->wiphy, ch);\n"
            "\tif (channel == NULL)\n"
            "\t\treturn -EINVAL;\n"
            "\n"
            "\tif (len < 8 + 2 + 2)\n"
            "\t\treturn -EINVAL;\n"
            "\n"
            "\tif (frame_type == BEACON_FTYPE &&\n"
            "\t    test_bit(CONNECTED, &vif->flags) &&\n"
            "\t    memcmp(bssid, vif->bssid, ETH_ALEN) == 0) {\n"
        )
        s, c = replace_once_or_applied(s, OLD, NEW, "wmi.c bssinfo channel/bssid")
        applied += c

    if "bih->bssid, get_unaligned_le64" in s:
        OLD = (
            "\tbss = cfg80211_inform_bss(ar->wiphy, channel,\n"
            "\t\t\t\t  bih->frame_type == BEACON_FTYPE ?\n"
            "\t\t\t\t\tCFG80211_BSS_FTYPE_BEACON :\n"
            "\t\t\t\t\tCFG80211_BSS_FTYPE_PRESP,\n"
            "\t\t\t\t  bih->bssid, get_unaligned_le64((__le64 *)buf),\n"
            "\t\t\t\t  get_unaligned_le16(((__le16 *)buf) + 5),\n"
            "\t\t\t\t  get_unaligned_le16(((__le16 *)buf) + 4),\n"
            "\t\t\t\t  buf + 8 + 2 + 2, len - 8 - 2 - 2,\n"
            "\t\t\t\t  (bih->snr - 95) * 100, GFP_ATOMIC);\n"
        )
        NEW = (
            "\tbss = cfg80211_inform_bss(ar->wiphy, channel,\n"
            "\t\t\t\t  frame_type == BEACON_FTYPE ?\n"
            "\t\t\t\t\tCFG80211_BSS_FTYPE_BEACON :\n"
            "\t\t\t\t\tCFG80211_BSS_FTYPE_PRESP,\n"
            "\t\t\t\t  bssid, get_unaligned_le64((__le64 *)buf),\n"
            "\t\t\t\t  get_unaligned_le16(((__le16 *)buf) + 5),\n"
            "\t\t\t\t  get_unaligned_le16(((__le16 *)buf) + 4),\n"
            "\t\t\t\t  buf + 8 + 2 + 2, len - 8 - 2 - 2,\n"
            "\t\t\t\t  (snr - 95) * 100, GFP_ATOMIC);\n"
        )
        s, c = replace_once_or_applied(s, OLD, NEW, "wmi.c bssinfo inform_bss")
        applied += c

    wmi_c.write_text(s)

    if applied:
        print("fix-ath6kl-ar6002-bssinfo-v1.py: patched %d location(s)" % applied)
    else:
        print("fix-ath6kl-ar6002-bssinfo-v1.py: already applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
