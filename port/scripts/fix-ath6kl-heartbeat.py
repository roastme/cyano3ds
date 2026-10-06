#!/usr/bin/env python3
# fix-ath6kl-heartbeat.py - keep the Nintendo 3DS AR6014 (AR6002/hw2) firmware
#                           alive by servicing its WMI heartbeat.
#
# Symptom this fixes (2026-10-01, card boot with the new wpa_supplicant):
# the SDIO card enumerates, the NWM firmware boots, WMI_READY and
# WMI_REGDOMAIN arrive, wlan0 is registered -- and then *nothing*: every WMI
# command the host sends afterwards (SET_BSS_FILTER, SET_PROBED_SSID,
# START_SCAN, ...) is accepted by the HTC/SDIO transport but the firmware never
# sends a single event or reply.  Scanning therefore never completes
# (cfg80211's SIOCGIWSCAN returns EAGAIN forever, then SIOCSIWSCAN returns
# -EBUSY), so Android finds no networks ("out of range" / empty list).
#
# Root cause: the DSi/3DS AR6002-family firmware runs a WMI heartbeat.  If the
# host does not answer it, the firmware simply stops processing WMI commands.
# GBATEK (DSi Atheros Wifi - WMI Power Functions, "Heartbeat"):
#
#   "If a timeout occurs because of not sending heartbeat commands in time:
#    Some firmwares do simply ignoring the timeout (eg. DSi/AR6013, or
#    3DS/AR6014 with basic Type1 firmware). However, other firmwares do hang
#    up (eg. 3DS/AR6014 with AP-mode Type4 firmware). There seems to be no
#    error message/event on timeout, and no known way to recover."
#
# The DSi/3DS host sets the timeout with WMI_SET_HB_CHALLENGE_RESP_PARAMS_CMD
# (0x0047 -- the DSi/3DS firmware inserted it here and moved SET_FRAMERATES to
# 0x0048) and then sends WMIX_HB_CHALLENGE_RESP_CMD (0x002E:0x2008) at least
# once per timeout.  Mainline ath6kl already implements the *response* half
# (`ath6kl_wmi_get_challenge_resp_cmd()` -> WMIX_HB_CHALLENGE_RESP_CMDID,
# handled by the fw-recovery subsystem), but:
#
#   * `recovery_enable`/`heart_beat_poll` are module parameters that default
#     to 0, and even when set, `hb_poll` is only armed when the firmware
#     advertises ATH6KL_FW_CAPABILITY_HEART_BEAT_POLL -- which the AR6002
#     firmware does not;
#   * nothing ever sends WMI_SET_HB_CHALLENGE_RESP_PARAMS_CMD.
#
# This patch, for TARGET_TYPE_AR6002 only:
#   1. forces the recovery/heartbeat polling on (hb_poll = 1000 ms) regardless
#      of the module parameters or the capability bit;
#   2. adds ath6kl_wmi_set_hb_challenge_resp_params_cmd() and sends it
#      (timeout = 2 s, the value the DSi uses) from ath6kl_recovery_init(),
#      before the first poll is armed.
#
# Everything else (the periodic WMIX ping and the response event) is the
# stock ath6kl fw-recovery code.
#
# Usage:
#   python3 port/scripts/fix-ath6kl-heartbeat.py [KERNEL_DIR]
#       # default /root/p3ds/src/linux-3ds
#
# Idempotent: safe to run repeatedly.

import pathlib
import sys

KD = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/root/p3ds/src/linux-3ds")
ATH = KD / "drivers" / "net" / "wireless" / "ath" / "ath6kl"
CORE_H = ATH / "core.h"
WMI_H = ATH / "wmi.h"
WMI_C = ATH / "wmi.c"
CORE_C = ATH / "core.c"
RECOVERY_C = ATH / "recovery.c"

MARK = "[3ds]"


def replace_once(path, old, new, what):
    s = path.read_text()
    if new.split("\n")[0] in s and old not in s:
        print("    %s: already patched" % path.name)
        return False
    if old not in s:
        print("cannot find %s in %s" % (what, path), file=sys.stderr)
        sys.exit(1)
    path.write_text(s.replace(old, new, 1))
    print("    %s: %s" % (path.name, what))
    return True


def main() -> int:
    for p in (CORE_H, WMI_H, WMI_C, CORE_C, RECOVERY_C):
        if not p.is_file():
            print("no %s" % p, file=sys.stderr)
            return 1

    # ------------------------------------------------------------------
    # 1. core.h: constants for the AR6002/AR6014 heartbeat
    # ------------------------------------------------------------------
    if "ATH6KL_AR6002_HB_POLL_MS" not in CORE_H.read_text():
        replace_once(
            CORE_H,
            "#define ATH6KL_HB_RESP_MISS_THRES\t5\n",
            "#define ATH6KL_HB_RESP_MISS_THRES\t5\n"
            "\n"
            "/* %s Nintendo 3DS AR6014 (AR6002/hw2) firmware heartbeat.  The\n"
            " * DSi/3DS firmware hangs if the host does not answer its WMI\n"
            " * heartbeat; GBATEK documents 2 s as the DSi timeout. */\n"
            "#define ATH6KL_AR6002_HB_TIMEOUT_SEC\t2\n"
            "#define ATH6KL_AR6002_HB_POLL_MS\t1000\n" % MARK,
            "AR6002 heartbeat constants")
    else:
        print("    core.h: already patched")

    # ------------------------------------------------------------------
    # 2. wmi.h: the AR6002 heartbeat-params command id + declaration
    # ------------------------------------------------------------------
    if "WMI_AR6002_SET_HB_CHALLENGE_RESP_PARAMS_CMDID" not in WMI_H.read_text():
        replace_once(
            WMI_H,
            "struct wmix_hb_challenge_resp_cmd {\n"
            "\t__le32 cookie;\n"
            "\t__le32 source;\n"
            "} __packed;\n",
            "/* %s DSi/3DS AR6002/AR6013/AR6014 (hw2) firmware inserts the\n"
            " * heartbeat-parameter command at 0x0047 (mainline ath6kl calls\n"
            " * that id WMI_SET_FRAMERATES_CMDID and moves SET_FRAMERATES to\n"
            " * 0x0048 on this family).  The host must set the heartbeat\n"
            " * timeout here, then keep answering with\n"
            " * WMIX_HB_CHALLENGE_RESP_CMD. */\n"
            "#define WMI_AR6002_SET_HB_CHALLENGE_RESP_PARAMS_CMDID\t0x0047\n"
            "\n"
            "struct wmix_hb_challenge_resp_cmd {\n"
            "\t__le32 cookie;\n"
            "\t__le32 source;\n"
            "} __packed;\n" % MARK,
            "AR6002 heartbeat-params cmd id")
        replace_once(
            WMI_H,
            "int ath6kl_wmi_get_challenge_resp_cmd(struct wmi *wmi, u32 cookie, u32 source);\n"
            "int ath6kl_wmi_config_debug_module_cmd(struct wmi *wmi, u32 valid, u32 config);\n",
            "int ath6kl_wmi_get_challenge_resp_cmd(struct wmi *wmi, u32 cookie, u32 source);\n"
            "int ath6kl_wmi_set_hb_challenge_resp_params_cmd(struct wmi *wmi,\n"
            "\t\t\t\t\t\t u32 timeout);\n"
            "int ath6kl_wmi_config_debug_module_cmd(struct wmi *wmi, u32 valid, u32 config);\n",
            "AR6002 heartbeat-params prototype")
    else:
        print("    wmi.h: already patched")

    # ------------------------------------------------------------------
    # 3. wmi.c: send WMI_AR6002_SET_HB_CHALLENGE_RESP_PARAMS_CMDID
    # ------------------------------------------------------------------
    if "ath6kl_wmi_set_hb_challenge_resp_params_cmd" not in WMI_C.read_text():
        replace_once(
            WMI_C,
            "\tret = ath6kl_wmi_cmd_send_xtnd(wmi, skb, WMIX_HB_CHALLENGE_RESP_CMDID,\n"
            "\t\t\t\t       NO_SYNC_WMIFLAG);\n"
            "\treturn ret;\n"
            "}\n"
            "\n"
            "int ath6kl_wmi_config_debug_module_cmd(struct wmi *wmi, u32 valid, u32 config)\n",
            "\tret = ath6kl_wmi_cmd_send_xtnd(wmi, skb, WMIX_HB_CHALLENGE_RESP_CMDID,\n"
            "\t\t\t\t       NO_SYNC_WMIFLAG);\n"
            "\treturn ret;\n"
            "}\n"
            "\n"
            "/* %s AR6002/AR6014 (hw2, DSi/3DS firmware): set the WMI heartbeat\n"
            " * timeout (seconds; 0 disables it).  Sent before the periodic\n"
            " * WMIX_HB_CHALLENGE_RESP_CMD pings in ath6kl_recovery_init(). */\n"
            "int ath6kl_wmi_set_hb_challenge_resp_params_cmd(struct wmi *wmi,\n"
            "\t\t\t\t\t\t u32 timeout)\n"
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
            "\t*cmd = cpu_to_le32(timeout);\n"
            "\n"
            "\tret = ath6kl_wmi_cmd_send(wmi, 0, skb,\n"
            "\t\t\t\t  WMI_AR6002_SET_HB_CHALLENGE_RESP_PARAMS_CMDID,\n"
            "\t\t\t\t  NO_SYNC_WMIFLAG);\n"
            "\treturn ret;\n"
            "}\n"
            "\n"
            "int ath6kl_wmi_config_debug_module_cmd(struct wmi *wmi, u32 valid, u32 config)\n" % MARK,
            "AR6002 heartbeat-params sender")
    else:
        print("    wmi.c: already patched")

    # ------------------------------------------------------------------
    # 4. core.c: force heartbeat polling on for AR6002
    # ------------------------------------------------------------------
    if "AR6002/hw2): the DSi/3DS Type4 firmware" not in CORE_C.read_text():
        replace_once(
            CORE_C,
            "\tar->fw_recovery.enable = !!recovery_enable;\n"
            "\tif (!ar->fw_recovery.enable)\n"
            "\t\treturn ret;\n"
            "\n"
            "\tif (heart_beat_poll &&\n"
            "\t    test_bit(ATH6KL_FW_CAPABILITY_HEART_BEAT_POLL,\n"
            "\t\t     ar->fw_capabilities))\n"
            "\t\tar->fw_recovery.hb_poll = heart_beat_poll;\n"
            "\n"
            "\tath6kl_recovery_init(ar);\n",
            "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
            "\t\t/* %s Nintendo 3DS AR6014 (AR6002/hw2): the DSi/3DS Type4\n"
            "\t\t * firmware simply stops processing WMI commands if the host\n"
            "\t\t * does not answer its heartbeat (GBATEK: \"other firmwares\n"
            "\t\t * do hang up (eg. 3DS/AR6014 with AP-mode Type4 firmware);\n"
            "\t\t * there seems to be no error message/event on timeout, and\n"
            "\t\t * no known way to recover\").  The firmware does not\n"
            "\t\t * advertise HEART_BEAT_POLL, so force polling on. */\n"
            "\t\tar->fw_recovery.enable = true;\n"
            "\t\tar->fw_recovery.hb_poll = heart_beat_poll ?\n"
            "\t\t\theart_beat_poll : ATH6KL_AR6002_HB_POLL_MS;\n"
            "\t} else {\n"
            "\t\tar->fw_recovery.enable = !!recovery_enable;\n"
            "\t\tif (!ar->fw_recovery.enable)\n"
            "\t\t\treturn ret;\n"
            "\n"
            "\t\tif (heart_beat_poll &&\n"
            "\t\t    test_bit(ATH6KL_FW_CAPABILITY_HEART_BEAT_POLL,\n"
            "\t\t\t     ar->fw_capabilities))\n"
            "\t\t\tar->fw_recovery.hb_poll = heart_beat_poll;\n"
            "\t}\n"
            "\n"
            "\tath6kl_recovery_init(ar);\n" % MARK,
            "AR6002 forced heartbeat polling")
    else:
        print("    core.c: already patched")

    # ------------------------------------------------------------------
    # 5. recovery.c: set the heartbeat timeout before the first ping
    # ------------------------------------------------------------------
    if "ath6kl_wmi_set_hb_challenge_resp_params_cmd" not in RECOVERY_C.read_text():
        replace_once(
            RECOVERY_C,
            "\ttimer_setup(&ar->fw_recovery.hb_timer, ath6kl_recovery_hb_timer,\n"
            "\t\t    TIMER_DEFERRABLE);\n"
            "\n"
            "\tif (ar->fw_recovery.hb_poll)\n",
            "\ttimer_setup(&ar->fw_recovery.hb_timer, ath6kl_recovery_hb_timer,\n"
            "\t\t    TIMER_DEFERRABLE);\n"
            "\n"
            "\t/* %s AR6002/AR6014 (hw2): tell the DSi/3DS firmware the\n"
            "\t * heartbeat timeout before the first challenge ping, or it will\n"
            "\t * hang and ignore every later WMI command. */\n"
            "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
            "\t\tint err;\n"
            "\n"
            "\t\terr = ath6kl_wmi_set_hb_challenge_resp_params_cmd(\n"
            "\t\t\t\tar->wmi, ATH6KL_AR6002_HB_TIMEOUT_SEC);\n"
            "\t\tif (err)\n"
            "\t\t\tath6kl_warn(\"Failed to set hb params, err:%%d\\n\", err);\n"
            "\t}\n"
            "\n"
            "\tif (ar->fw_recovery.hb_poll)\n" % MARK,
            "AR6002 heartbeat timeout")
    else:
        print("    recovery.c: already patched")

    print("    ath6kl heartbeat patch done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
