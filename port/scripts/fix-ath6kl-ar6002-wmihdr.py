#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-wmihdr.py - the AR6002/AR6014 WMI header is 2 bytes, not 6.

`fix-ath6kl.py` gave mainline ath6kl a 6-byte `struct wmi_cmd_hdr`
(`commandId + info1 + reserved`) for the AR6002, matching the AR6002 SDK's
`WMI_CMD_HDR`.  The raw WMI RX dump added by `fix-ath6kl-diag.py` (card boot
2026-10-01 19:15, Type1 firmware) proved that is wrong for this target:

    wmi rx raw 01 10 <module-mac> 02 00 ec 00 00 23 02 00 00 00  WMI_READY
    wmi rx raw 06 10 88 01 00 80                                    WMI_REGDOMAIN

WMI_REGDOMAIN is *exactly* 6 bytes and its payload is the documented 4-byte
`0x80000188` reg-domain value (`88 01 00 80`), so the on-wire WMI payload is:

    commandId(u16)  then  parameters

i.e. a **2-byte** header with no info1/reserved.  (WMI_READY then starts with
the real module MAC <module-mac> at byte 2.)  `mainline`'s 4-byte header
and the port's 6-byte one both shifted every parameter by 2/4 bytes, so the
firmware received garbage for every command -- including the heartbeat's
`002Eh:2008h` extension id, which is why the AR6014 WMI task never answered a
thing (and why Type1/Type4 made no difference).

This script makes the WMI header 2 bytes for `TARGET_TYPE_AR6002` only, in both
directions, and fixes the two `txrx.c` if-index lookups that assume an `info1`
field.  AR6003/AR6004 keep the 6-byte header.  Idempotent.
"""

import sys
import os
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-wmihdr.py: " + msg)


def replace_once(s, old, new, what):
    if new in s:
        return s, False
    if old not in s:
        die("anchor not found: %s" % what)
    return s.replace(old, new, 1), True


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.path.expanduser("~/p3ds/src/linux-3ds"))
    wmi = kd / "drivers/net/wireless/ath/ath6kl/wmi.c"
    txrx = kd / "drivers/net/wireless/ath/ath6kl/txrx.c"
    for p in (wmi, txrx):
        if not p.exists():
            die("no %s" % p)

    changed = []

    # ------------------------------------------------------------------ TX --
    s = wmi.read_text()
    TX_OLD = (
        "\tskb_push(skb, sizeof(struct wmi_cmd_hdr));\n"
        "\n"
        "\tcmd_hdr = (struct wmi_cmd_hdr *) skb->data;\n"
        "\tcmd_hdr->cmd_id = cpu_to_le16(cmd_id);\n"
        "\tinfo1 = if_idx & WMI_CMD_HDR_IF_ID_MASK;\n"
        "\tcmd_hdr->info1 = cpu_to_le16(info1);\n"
        "\t/*\n"
        "\t * [3ds] The AR6002/AR6kSDK WMI_CMD_HDR is\n"
        "\t * commandId + info1 + reserved (6 bytes); mainline never\n"
        "\t * initialises the \"for alignment\" u16, so the DSi/3DS\n"
        "\t * firmware sees whatever was in the skb headroom.  Make it\n"
        "\t * deterministic.\n"
        "\t */\n"
        "\tcmd_hdr->reserved = 0;\n"
    )
    TX_NEW = (
        "\t/*\n"
        "\t * [3ds] AR6002/hw2 (AR6014G): the WMI payload is\n"
        "\t * {commandId(u16), parameters...}; there is NO info1 and no\n"
        "\t * reserved field.  See fix-ath6kl-ar6002-wmihdr.py: the raw\n"
        "\t * WMI RX dump shows WMI_REGDOMAIN is 2+4 bytes and WMI_READY\n"
        "\t * carries the module MAC at byte 2.  Both mainline's 4-byte\n"
        "\t * header and the earlier 6-byte one shifted every parameter,\n"
        "\t * so the firmware got garbage for every command.  AR6003/6004\n"
        "\t * keep the 6-byte {cmd_id, info1, reserved} header.\n"
        "\t */\n"
        "\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\t__le16 *hdr2 = (__le16 *) skb_push(skb, sizeof(__le16));\n"
        "\t\t*hdr2 = cpu_to_le16(cmd_id);\n"
        "\t} else {\n"
        "\t\tskb_push(skb, sizeof(struct wmi_cmd_hdr));\n"
        "\t\tcmd_hdr = (struct wmi_cmd_hdr *) skb->data;\n"
        "\t\tcmd_hdr->cmd_id = cpu_to_le16(cmd_id);\n"
        "\t\tinfo1 = if_idx & WMI_CMD_HDR_IF_ID_MASK;\n"
        "\t\tcmd_hdr->info1 = cpu_to_le16(info1);\n"
        "\t\tcmd_hdr->reserved = 0;\n"
        "\t}\n"
    )
    s, c = replace_once(s, TX_OLD, TX_NEW, "wmi.c TX header")
    changed.append(c)

    # ------------------------------------------------------------------ RX --
    RX_OLD = (
        "\tcmd = (struct wmi_cmd_hdr *) skb->data;\n"
        "\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002)\n"
        "\t\tath6kl_info(\"AR6002/AR6014: wmi rx raw %*ph\\n\",\n"
        "\t\t\t    (int)skb->len, skb->data);\n"
        "\tid = le16_to_cpu(cmd->cmd_id);\n"
        "\tif_idx = le16_to_cpu(cmd->info1) & WMI_CMD_HDR_IF_ID_MASK;\n"
        "\n"
        "\tskb_pull(skb, sizeof(struct wmi_cmd_hdr));\n"
    )
    RX_NEW = (
        "\tcmd = (struct wmi_cmd_hdr *) skb->data;\n"
        "\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\tath6kl_info(\"AR6002/AR6014: wmi rx raw %*ph\\n\",\n"
        "\t\t\t    (int)skb->len, skb->data);\n"
        "\t\tid = le16_to_cpu(*(__le16 *) skb->data);\n"
        "\t\tif_idx = 0;\n"
        "\t\tskb_pull(skb, sizeof(__le16));\n"
        "\t} else {\n"
        "\t\tid = le16_to_cpu(cmd->cmd_id);\n"
        "\t\tif_idx = le16_to_cpu(cmd->info1) & WMI_CMD_HDR_IF_ID_MASK;\n"
        "\t\tskb_pull(skb, sizeof(struct wmi_cmd_hdr));\n"
        "\t}\n"
    )
    s, c = replace_once(s, RX_OLD, RX_NEW, "wmi.c RX header")
    changed.append(c)

    # ------------------------------------------------- control_rx length --
    RX_LEN_OLD = (
        "\tif (skb->len < sizeof(struct wmi_cmd_hdr)) {\n"
        "\t\tath6kl_err(\"bad packet 1\\n\");\n"
    )
    RX_LEN_NEW = (
        "\tif (skb->len < (wmi->parent_dev->target_type == TARGET_TYPE_AR6002 ?\n"
        "\t\t\tsizeof(__le16) : sizeof(struct wmi_cmd_hdr))) {\n"
        "\t\tath6kl_err(\"bad packet 1\\n\");\n"
    )
    s, c = replace_once(s, RX_LEN_OLD, RX_LEN_NEW, "wmi.c control_rx length")
    changed.append(c)

    if any(changed):
        wmi.write_text(s)
        print("fix-ath6kl-ar6002-wmihdr.py: wmi.c patched (%d sites)" % sum(changed))
    else:
        print("fix-ath6kl-ar6002-wmihdr.py: wmi.c already applied")

    # ------------------------------------------------------------ txrx.c --
    t = txrx.read_text()
    tchanged = []

    # TX completion: the control endpoint packet's if-index is info1 on
    # AR6003 but does not exist on AR6002 (always interface 0).
    T1_OLD = (
        "\t\tif (eid == ar->ctrl_ep) {\n"
        "\t\t\tif_idx = wmi_cmd_hdr_get_if_idx(\n"
        "\t\t\t\t(struct wmi_cmd_hdr *) packet->buf);\n"
        "\t\t} else {\n"
    )
    T1_NEW = (
        "\t\tif (eid == ar->ctrl_ep) {\n"
        "\t\t\tif (ar->target_type == TARGET_TYPE_AR6002)\n"
        "\t\t\t\tif_idx = 0;   /* hw2 WMI header is just commandId */\n"
        "\t\t\telse\n"
        "\t\t\t\tif_idx = wmi_cmd_hdr_get_if_idx(\n"
        "\t\t\t\t\t(struct wmi_cmd_hdr *) packet->buf);\n"
        "\t\t} else {\n"
    )
    t, c = replace_once(t, T1_OLD, T1_NEW, "txrx.c tx-complete if_idx")
    tchanged.append(c)

    T2_OLD = (
        "\t\tif_idx =\n"
        "\t\twmi_cmd_hdr_get_if_idx((struct wmi_cmd_hdr *) skb->data);\n"
    )
    T2_NEW = (
        "\t\tif_idx = (ar->target_type == TARGET_TYPE_AR6002) ? 0 :\n"
        "\t\t\twmi_cmd_hdr_get_if_idx(\n"
        "\t\t\t\t(struct wmi_cmd_hdr *) skb->data);\n"
    )
    t, c = replace_once(t, T2_OLD, T2_NEW, "txrx.c rx if_idx")
    tchanged.append(c)

    if any(tchanged):
        txrx.write_text(t)
        print("fix-ath6kl-ar6002-wmihdr.py: txrx.c patched (%d sites)" % sum(tchanged))
    else:
        print("fix-ath6kl-ar6002-wmihdr.py: txrx.c already applied")


if __name__ == "__main__":
    main()
