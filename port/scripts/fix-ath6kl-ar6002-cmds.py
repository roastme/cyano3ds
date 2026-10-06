#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-cmds.py - keep the DSi/3DS AR6002 (hw2) firmware's WMI task
                            alive: never send it an AR6003-only command, send
                            the first heartbeat immediately, and fill in the
                            WMI header's alignment byte.

Background
----------
After the NWM firmware is uploaded and HTC/WMI come up, the AR6014 (AR6002/hw2
family) reports WMI_READY and WMI_REGDOMAIN -- and then *stops processing every
host WMI command*: no event, no error, no assert.  The HTC layer still answers
credit reports (so the chip is running and is consuming the packets) but the
WMI task never replies, not even to GBATEK's WMIX_HB_CHALLENGE_RESP_CMD
(0x2E:0x2008), which the firmware is supposed to answer immediately.  The
driver's fw-recovery then resets the firmware every ~7 s and nothing ever
scans.

Two things mainline ath6kl does that the DSi/3DS firmware cannot cope with:

1. ``ath6kl_target_config_wlan_params()`` sends the AR6003/AR6004 WLAN init
   (``WMI_SET_KEEPALIVE_CMD`` 0x3D, ``WMI_SET_DISC_TIMEOUT_CMD`` 0x0D, ...)
   and ``WMI_RX_FRAME_FORMAT_CMD`` (0xF024).  GBATEK lists the developer range
   **F007h..F05Eh as "not implemented in DSi"** (Type4 only renumbers
   F00Ah..F013h to 004Ah..0052h), and an unimplemented id can fault the
   firmware -- GBATEK for cmd 0026h: *"that handler does just trigger a
   misalign exception"*.  The 0xF024 command is sent *first*, before the
   heartbeat params, so if it (or 0x3D/0x0D) kills the WMI task there is no
   recovery.

2. ``ath6kl_wmi_cmd_send()`` never initialises ``struct wmi_cmd_hdr.reserved``
   (the AR6kSDK's "for alignment" third u16, which the AR6002 firmware parses).
   mainline leaves it as whatever was in the skb headroom.

What this does (all idempotent, AR6002 only where noted)
--------------------------------------------------------
* ``wmi.c``  - ``ath6kl_wmi_cmd_send()``:
  * drops every command in the GBATEK "not implemented in DSi" developer range
    0xF007..0xF05E for ``TARGET_TYPE_AR6002`` (logs it, returns success);
  * zeroes ``cmd_hdr->reserved``.
* ``init.c`` - ``ath6kl_target_config_wlan_params()`` returns immediately for
  ``TARGET_TYPE_AR6002`` (leave the firmware defaults; none of the AR6003 init
  is needed for scan/connect).
* ``recovery.c`` - ``ath6kl_recovery_init()`` arms the first heartbeat with a
  0 ms delay for ``TARGET_TYPE_AR6002``, so the challenge is on the wire right
  after the params command with no window for the firmware's default timeout.

Usage:
    python3 port/scripts/fix-ath6kl-ar6002-cmds.py [KERNEL_DIR]
        # default /root/p3ds/src/linux-3ds
"""

import pathlib
import sys

KD = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/root/p3ds/src/linux-3ds")
ATH = KD / "drivers" / "net" / "wireless" / "ath" / "ath6kl"
WMI_C = ATH / "wmi.c"
INIT_C = ATH / "init.c"
RECOVERY_C = ATH / "recovery.c"

MARK_DROP = "AR6002/AR6014: dropping DSi-unsupported WMI cmd"
MARK_RESV = "cmd_hdr->reserved = 0;"
MARK_CFG = "AR6002/AR6014: leaving the DSi/3DS firmware WLAN defaults alone"
MARK_HB = "AR6002/AR6014: first heartbeat challenge is sent immediately"
MARK_APPA = "AR6002/AR6014: no host_app_area"
MARK_HEX = "AR6002/AR6014: wmi tx %*ph"


def patch(path, old, new, marker, what):
    s = path.read_text()
    if marker in s:
        print("    %s: already patched (%s)" % (path.name, what))
        return
    if old not in s:
        print("cannot find %s in %s" % (what, path), file=sys.stderr)
        sys.exit(1)
    path.write_text(s.replace(old, new, 1))
    print("    %s: %s" % (path.name, what))


def main() -> int:
    for p in (WMI_C, INIT_C, RECOVERY_C):
        if not p.is_file():
            print("no %s" % p, file=sys.stderr)
            return 1

    # ------------------------------------------------------------------
    # wmi.c: drop DSi-unsupported developer commands for hw2
    # ------------------------------------------------------------------
    patch(
        WMI_C,
        '\tath6kl_dbg_dump(ATH6KL_DBG_WMI_DUMP, NULL, "wmi tx ",\n'
        '\t\t\tskb->data, skb->len);\n'
        '\n'
        '\tif (sync_flag >= END_WMIFLAG) {\n',
        '\tath6kl_dbg_dump(ATH6KL_DBG_WMI_DUMP, NULL, "wmi tx ",\n'
        '\t\t\tskb->data, skb->len);\n'
        '\n'
        '\t/*\n'
        '\t * [3ds] Nintendo 3DS AR6014 (AR6002/hw2, DSi/3DS firmware):\n'
        '\t * GBATEK lists the developer commands F007h..F05Eh as "not\n'
        '\t * implemented in DSi".  The 3DS Type4 firmware only renumbers\n'
        '\t * F00Ah..F013h (004Ah..0052h); the rest of the range is unknown\n'
        '\t * to it, and an unknown command can kill its WMI task -- after\n'
        '\t * which HTC keeps answering credit reports but no WMI command is\n'
        '\t * ever processed again (no event, no error, no recovery).  That\n'
        '\t * is what stopped every scan.  Never send them on hw2; the\n'
        '\t * affected commands (rx frame format, mcast filter, abort scan,\n'
        '\t * AR6003 aggregation/coexistence, ...) do not exist here.\n'
        '\t */\n'
        '\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002 &&\n'
        '\t    cmd_id >= 0xF007 && cmd_id <= 0xF05E) {\n'
        '\t\tath6kl_warn("' + MARK_DROP + ' 0x%x\\n", cmd_id);\n'
        '\t\tdev_kfree_skb(skb);\n'
        '\t\treturn 0;\n'
        '\t}\n'
        '\n'
        '\tif (sync_flag >= END_WMIFLAG) {\n',
        MARK_DROP,
        "drop DSi-unsupported developer commands (0xF007..0xF05E)",
    )

    # ------------------------------------------------------------------
    # wmi.c: fill the WMI header's "for alignment" byte
    # ------------------------------------------------------------------
    patch(
        WMI_C,
        '\tcmd_hdr->info1 = cpu_to_le16(info1);\n',
        '\tcmd_hdr->info1 = cpu_to_le16(info1);\n'
        '\t/*\n'
        '\t * [3ds] The AR6002/AR6kSDK WMI_CMD_HDR is\n'
        '\t * commandId + info1 + reserved (6 bytes); mainline never\n'
        '\t * initialises the "for alignment" u16, so the DSi/3DS\n'
        '\t * firmware sees whatever was in the skb headroom.  Make it\n'
        '\t * deterministic.\n'
        '\t */\n'
        '\t' + MARK_RESV + '\n',
        MARK_RESV,
        "zero the WMI header alignment byte",
    )

    # ------------------------------------------------------------------
    # init.c: skip mainline's AR6003-only WLAN init on hw2
    # ------------------------------------------------------------------
    patch(
        INIT_C,
        'static int ath6kl_target_config_wlan_params(struct ath6kl *ar, int idx)\n'
        '{\n'
        '\tint ret;\n'
        '\n',
        'static int ath6kl_target_config_wlan_params(struct ath6kl *ar, int idx)\n'
        '{\n'
        '\tint ret;\n'
        '\n'
        '\t/*\n'
        '\t * [3ds] ' + MARK_CFG + '.\n'
        '\t * The sequence below is mainline\'s AR6003/AR6004 WLAN init\n'
        '\t * (WMI_SET_KEEPALIVE 0x3D, WMI_SET_DISC_TIMEOUT 0x0D, ...).\n'
        '\t * On hw2 the DSi/3DS firmware stops replying to *every* WMI\n'
        '\t * command once it has seen one it does not handle -- including\n'
        '\t * the heartbeat challenge -- so leave the firmware defaults\n'
        '\t * and configure nothing here.  None of it is needed for a scan.\n'
        '\t */\n'
        '\tif (ar->target_type == TARGET_TYPE_AR6002)\n'
        '\t\treturn 0;\n'
        '\n',
        MARK_CFG,
        "skip the AR6003-only WLAN init on hw2",
    )

    # ------------------------------------------------------------------
    # recovery.c: send the first heartbeat immediately on hw2
    # ------------------------------------------------------------------
    patch(
        RECOVERY_C,
        '\t\tif (err)\n'
        '\t\t\tath6kl_warn("Failed to set hb params, err:%d\\n", err);\n'
        '\t}\n'
        '\n'
        '\tif (ar->fw_recovery.hb_poll)\n'
        '\t\tmod_timer(&ar->fw_recovery.hb_timer, jiffies +\n'
        '\t\t\t  msecs_to_jiffies(ar->fw_recovery.hb_poll));\n',
        '\t\tif (err)\n'
        '\t\t\tath6kl_warn("Failed to set hb params, err:%d\\n", err);\n'
        '\t}\n'
        '\n'
        '\tif (ar->fw_recovery.hb_poll) {\n'
        '\t\tunsigned long hb_delay = ar->fw_recovery.hb_poll;\n'
        '\n'
        '\t\t/* [3ds] ' + MARK_HB + ':\n'
        '\t\t * The DSi/3DS firmware is already running when the host gets\n'
        '\t\t * here, and its default timeout may be shorter than one poll. */\n'
        '\t\tif (ar->target_type == TARGET_TYPE_AR6002)\n'
        '\t\t\thb_delay = 0;\n'
        '\n'
        '\t\tmod_timer(&ar->fw_recovery.hb_timer, jiffies +\n'
        '\t\t\t  msecs_to_jiffies(hb_delay));\n'
        '\t}\n',
        MARK_HB,
        "first heartbeat challenge immediately on hw2",
    )

    # ------------------------------------------------------------------
    # init.c: the hw2 firmware does not publish a host_app_area pointer
    # ------------------------------------------------------------------
    patch(
        INIT_C,
        '\taddress = TARG_VTOP(ar->target_type, data);\n'
        '\thost_app_area.wmi_protocol_ver = cpu_to_le32(WMI_PROTOCOL_VERSION);\n',
        '\taddress = TARG_VTOP(ar->target_type, data);\n'
        '\n'
        '\t/*\n'
        '\t * [3ds] On the DSi/3DS AR6002/hw2 firmware the\n'
        '\t * host_app_area pointer is not published: the field still holds\n'
        '\t * the HTC_PROTOCOL_VERSION written during the BMI phase, so the\n'
        '\t * write below would land at target address 2.  Log it, and skip\n'
        '\t * the write when it is not a plausible RAM address.\n'
        '\t */\n'
        '\tif (ar->target_type == TARGET_TYPE_AR6002) {\n'
        '\t\tath6kl_info("AR6002/AR6014: host_app_area ptr=0x%x\\n", data);\n'
        '\t\tif (data < 0x1000) {\n'
        '\t\t\tath6kl_warn("' + MARK_APPA + ', skipping write (0x%x)\\n", data);\n'
        '\t\t\treturn 0;\n'
        '\t\t}\n'
        '\t}\n'
        '\n'
        '\thost_app_area.wmi_protocol_ver = cpu_to_le32(WMI_PROTOCOL_VERSION);\n',
        MARK_APPA,
        "skip the bogus hw2 host_app_area write",
    )

    # ------------------------------------------------------------------
    # wmi.c: hex-log the full hw2 WMI packet (header + payload)
    # ------------------------------------------------------------------
    patch(
        WMI_C,
        '\t' + MARK_RESV + '\n',
        '\t' + MARK_RESV + '\n'
        '\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002)\n'
        '\t\tath6kl_info("' + MARK_HEX + '\\n",\n'
        '\t\t\t    (int)skb->len, skb->data);\n',
        MARK_HEX,
        "hex-log the hw2 WMI packet",
    )

    print("    ath6kl AR6002 command fixes done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
