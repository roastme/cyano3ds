#!/usr/bin/env python3
"""
fix-ath6kl-diag.py - make the AR6002/AR6014 upload sequence visible.

`fix-ath6kl.py` teaches mainline ath6kl about the Nintendo AR6014G, but its
AR6002 upload path logs every step at ATH6KL_DBG_BOOT level, i.e. it is silent
unless `ath6kl_core.debug_mask` is set on the kernel command line.  On the 3DS
the boot log is the only channel (there is no serial console), so this script
promotes those steps to `ath6kl_info()` and adds a one-shot dump of the host
interest items the ROM/firmware care about (board-data pointer, clock info,
external-clock detect).  That is what tells us whether the target even reached
the firmware and what it thinks the board data is.

It is idempotent and safe to run after `fix-ath6kl.py`.
"""

import sys
import os
import pathlib

def die(msg):
    sys.exit("fix-ath6kl-diag.py: " + msg)

def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.path.expanduser("~/p3ds/src/linux-3ds"))
    p = kd / "drivers/net/wireless/ath/ath6kl/init.c"
    if not p.exists():
        die("no ath6kl init.c at %s" % p)
    s = p.read_text()
    orig = s

    # ---- promote the AR6002 board/stub/execute steps to ath6kl_info() -----
    # Accept either the old ath6kl_dbg() form or the new ath6kl_info() form so
    # re-running this script (and running it after a fresh fix-ath6kl.py) is a
    # no-op.
    reps = [
        (
            "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
            "\t\t\t   \"writing AR6002 stub data to 0x%x (%zd B)\\n\",\n"
            "\t\t\t   board_address, ar->fw_board_len);\n",
            "\t\tath6kl_info(\"AR6002/AR6014: writing stub data to 0x%x (%zd B)\\n\",\n"
            "\t\t\t    board_address, ar->fw_board_len);\n",
        ),
        (
            "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
            "\t\t\t   \"writing AR6002 stub to 0x%x (%zd B)\\n\",\n"
            "\t\t\t   address, ar->fw_otp_len);\n",
            "\t\tath6kl_info(\"AR6002/AR6014: writing stub to 0x%x (%zd B)\\n\",\n"
            "\t\t\t    address, ar->fw_otp_len);\n",
        ),
        (
            "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
            "\t\t\t   \"executing AR6002 stub at 0x%x\\n\",\n"
            "\t\t\t   ar->hw.app_start_override_addr);\n",
            "\t\tath6kl_info(\"AR6002/AR6014: executing stub at 0x%x\\n\",\n"
            "\t\t\t    ar->hw.app_start_override_addr);\n",
        ),
    ]
    for old, new in reps:
        if new in s:
            continue
        if old not in s:
            die("AR6002 log anchor not found (run fix-ath6kl.py first?):\n" + old)
        s = s.replace(old, new, 1)

    # ---- capture the stub's BMI_EXECUTE return value ---------------------
    # GBATEK: the AR6002 boot stub is executed via BMI_CMD(04h) and RETURNS a
    # value; the AR.drone logs "Return Value from target: 0x0" on success.  A
    # nonzero value means the stub could not read the module I2C EEPROM (board
    # data / MAC / RF calibration), which is exactly the failure mode that would
    # make the firmware assert right after boot.  mainline ath6kl discards the
    # value, so log it.
    EXEC_OLD = (
        "\t\tparam = 0;\n"
        "\t\tath6kl_info(\"AR6002/AR6014: executing stub at 0x%x\\n\",\n"
        "\t\t\t    ar->hw.app_start_override_addr);\n"
        "\t\treturn ath6kl_bmi_execute(ar, ar->hw.app_start_override_addr,\n"
        "\t\t\t\t\t  &param);\n"
    )
    EXEC_NEW = (
        "\t\tparam = 0;\n"
        "\t\tath6kl_info(\"AR6002/AR6014: executing stub at 0x%x\\n\",\n"
        "\t\t\t    ar->hw.app_start_override_addr);\n"
        "\t\tret = ath6kl_bmi_execute(ar, ar->hw.app_start_override_addr,\n"
        "\t\t\t\t\t &param);\n"
        "\t\tath6kl_info(\"AR6002/AR6014: stub returned %d (param=0x%x)\\n\",\n"
        "\t\t\t    ret, param);\n"
        "\t\treturn ret;\n"
    )
    if "stub returned %d" not in s:
        if EXEC_OLD not in s:
            die("AR6002 execute anchor not found (run fix-ath6kl.py first?)")
        s = s.replace(EXEC_OLD, EXEC_NEW, 1)

    # ---- dump the host-interest items once, before the upload ------------
    OLD_IF = (
        "\tif (ar->target_type == TARGET_TYPE_AR6002)\n"
        "\t\tath6kl_info(\"AR6002/AR6014: hi=0x%x app_load=0x%x patch=0x%x exec=0x%x\\n\",\n"
        "\t\t\t    ATH6KL_AR6014_HI_START_ADDR, ar->hw.app_load_addr,\n"
        "\t\t\t    ar->hw.dataset_patch_addr,\n"
        "\t\t\t    ar->hw.app_start_override_addr);\n"
    )
    NEW_IF = (
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\tu32 hv;\n"
        "\n"
        "\t\tath6kl_info(\"AR6002/AR6014: hi=0x%x app_load=0x%x patch=0x%x exec=0x%x\\n\",\n"
        "\t\t\t    ATH6KL_AR6014_HI_START_ADDR, ar->hw.app_load_addr,\n"
        "\t\t\t    ar->hw.dataset_patch_addr,\n"
        "\t\t\t    ar->hw.app_start_override_addr);\n"
        "\n"
        "\t\t/* The firmware reads its board data (MAC / calibration) from\n"
        "\t\t * the module I2C EEPROM via the Stub.code it jumps to on\n"
        "\t\t * BMI_DONE; these reads show what the ROM set up so a failed\n"
        "\t\t * bring-up can tell \"no board data\" from \"bad upload\". */\n"
        "\t\tif (ath6kl_bmi_read_hi32(ar, hi_app_host_interest, &hv) == 0)\n"
        "\t\t\tath6kl_info(\"AR6002/AR6014: hi_app_host_interest=0x%x\\n\", hv);\n"
        "\t\tif (ath6kl_bmi_read_hi32(ar, hi_board_data, &hv) == 0)\n"
        "\t\t\tath6kl_info(\"AR6002/AR6014: hi_board_data=0x%x\\n\", hv);\n"
        "\t\tif (ath6kl_bmi_read_hi32(ar, hi_board_data_initialized, &hv) == 0)\n"
        "\t\t\tath6kl_info(\"AR6002/AR6014: hi_board_data_initialized=%u\\n\", hv);\n"
        "\t\tif (ath6kl_bmi_read_hi32(ar, hi_clock_info, &hv) == 0)\n"
        "\t\t\tath6kl_info(\"AR6002/AR6014: hi_clock_info=0x%x\\n\", hv);\n"
        "\t\tif (ath6kl_bmi_read_hi32(ar, hi_refclk_hz, &hv) == 0)\n"
        "\t\t\tath6kl_info(\"AR6002/AR6014: hi_refclk_hz=%u\\n\", hv);\n"
        "\t\tif (ath6kl_bmi_read_hi32(ar, hi_ext_clk_detected, &hv) == 0)\n"
        "\t\t\tath6kl_info(\"AR6002/AR6014: hi_ext_clk_detected=%u\\n\", hv);\n"
        "\t}\n"
    )
    if "hi_app_host_interest=0x%x" not in s:
        if OLD_IF not in s:
            die("AR6002 info anchor not found (run fix-ath6kl.py first?)")
        s = s.replace(OLD_IF, NEW_IF, 1)

    if s != orig:
        p.write_text(s)
        print("fix-ath6kl-diag.py: AR6002 steps promoted + host-interest dump added")
    else:
        print("fix-ath6kl-diag.py: init.c already applied")

    # ---- dump every hw2 WMI RX payload, header included ------------------
    # mainline parses `struct wmi_cmd_hdr` (6 bytes on hw2) before the event
    # payload.  The header layout is the one remaining question about the
    # AR6002 WMI protocol -- melonDS' DSi model uses a 6-byte MBOX header and
    # then a 2-byte command, mainline uses {cmd_id, info1, reserved} -- so an
    # unconditional dump of the raw skb settles it and shows exactly what the
    # firmware sends (WMI_READY, heartbeat replies, scan results, ...).
    pw = kd / "drivers/net/wireless/ath/ath6kl/wmi.c"
    if not pw.exists():
        die("no ath6kl wmi.c at %s" % pw)
    w = pw.read_text()
    orig_w = w
    RX_ANCHOR = (
        "\tcmd = (struct wmi_cmd_hdr *) skb->data;\n"
        "\tid = le16_to_cpu(cmd->cmd_id);\n"
    )
    RX_NEW = (
        "\tcmd = (struct wmi_cmd_hdr *) skb->data;\n"
        "\tif (wmi->parent_dev->target_type == TARGET_TYPE_AR6002)\n"
        "\t\tath6kl_info(\"AR6002/AR6014: wmi rx raw %*ph\\n\",\n"
        "\t\t\t    (int)skb->len, skb->data);\n"
        "\tid = le16_to_cpu(cmd->cmd_id);\n"
    )
    if "wmi rx raw" not in w:
        if RX_ANCHOR not in w:
            die("WMI RX anchor not found in wmi.c (run fix-ath6kl.py first?)")
        w = w.replace(RX_ANCHOR, RX_NEW, 1)
    if w != orig_w:
        pw.write_text(w)
        print("fix-ath6kl-diag.py: AR6002 WMI RX raw dump added")
    else:
        print("fix-ath6kl-diag.py: wmi.c already applied")


if __name__ == "__main__":
    main()
