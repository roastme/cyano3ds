#!/usr/bin/env python3
"""
fix-ath6kl.py - teach mainline ath6kl about the Nintendo 3DS AR6014G.

The 3DS WiFi is an Atheros AR6014G.  GBATEK (DSi Atheros Wifi) classifies it as
the "hw2" family together with the AR6002/AR6013:

    AR60xx chip   AR6002     AR6003     AR6004     AR6013     AR6014
    hw name       hw2        hw4        hw6        hw2        hw2
    ROM ID hex    20000188   ?          ?          23000024   2300006F
    RAM Base      100000     140000     -          120000     120000
    RAM Host Int  500400     540600     400600     520000     520000
    RAM Free      502400     -          -          524C00     524C00
    BMI_DONE      915000     -          -          927000     927000

The renaming is why the earlier linux-3ds attempt failed: the AR6014 reports
ROM ID **0x2300006f**, which mainline ath6kl's hw table does not know
("Unsupported hardware version: 0x2300006f"), and the driver only ever handled
AR6003 (type 3) / AR6004 (type 5), never the AR6002 (type 2) "hw2" family.

This script makes the driver *recognise* the chip and describe it, so the
bring-up log shows the real target info and the firmware it asks for.  It is
idempotent.  NOTE: a working driver still needs the AR6014 Xtensa firmware,
which Nintendo ships inside the console's NWM module - see WIFI.md.  The
generic AR6002 firmware is NOT a substitute (different ROM function table).
"""

import sys
import pathlib
import re

def die(msg):
    sys.exit("fix-ath6kl.py: " + msg)

def replace_once(s, old, new, what):
    if new in s:
        return s, False
    if old not in s:
        die("%s: anchor not found" % what)
    return s.replace(old, new, 1), True

def replace_once_or_applied(s, old, new, marker, what):
    """Like replace_once, but a stable `marker` means an earlier run already
    inserted the block (possibly rewritten by fix-ath6kl-diag.py), so it is a
    no-op instead of a fatal anchor error."""
    if marker in s:
        return s, False
    return replace_once(s, old, new, what)

def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    a = kd / "drivers/net/wireless/ath/ath6kl"
    if not (a / "core.h").exists():
        die("no ath6kl tree at %s" % a)

    # ------------------------------------------------------------------
    # bmi.h: the AR6002 target type (hw2 family)
    # ------------------------------------------------------------------
    p = a / "bmi.h"
    s = p.read_text()
    s, ch = replace_once(
        s,
        "#define TARGET_VERSION_SENTINAL 0xffffffff\n"
        "#define TARGET_TYPE_AR6003      3\n",
        "#define TARGET_VERSION_SENTINAL 0xffffffff\n"
        "#define TARGET_TYPE_AR6002      2 /* Nintendo 3DS AR6014G (hw2) */\n"
        "#define TARGET_TYPE_AR6003      3\n", "bmi.h")
    if ch:
        p.write_text(s)
    print("    bmi.h: AR6002 target type %s" % ("added" if ch else "present"))

    # ------------------------------------------------------------------
    # core.h: AR6014 definitions
    # ------------------------------------------------------------------
    p = a / "core.h"
    s = p.read_text()
    orig = s
    defs = (
        "\n/* AR6014 2.3 (Nintendo 3DS / New 3DS, hw2 family) definitions */\n"
        "/* ROM ID 0x2300006f == \"2.3.0.111\" (GBATEK: DSi Atheros Wifi) */\n"
        "#define AR6014_HW_2_3_VERSION\t\t\t0x2300006f\n"
        "#define AR6014_HW_2_3_FW_DIR\t\t\t\"ath6k/AR6014\"\n"
        "#define AR6014_HW_2_3_FIRMWARE_FILE\t\t\"athwlan.bin.z77\"\n"
        "#define AR6014_HW_2_3_OTP_FILE\t\t\t\"eeprom.bin\"\n"
        "#define AR6014_HW_2_3_PATCH_FILE\t\t\"data.patch.hw2_0.bin\"\n"
        "#define AR6014_HW_2_3_BOARD_DATA_FILE\t\t\\\n"
        "\t\t\tAR6014_HW_2_3_FW_DIR \"/eeprom.data\"\n"
        "#define AR6014_HW_2_3_DEFAULT_BOARD_DATA_FILE\t\\\n"
        "\t\t\tAR6014_HW_2_3_FW_DIR \"/eeprom.data\"\n")
    # Upgrade an earlier run's AR6002-SDK file names to the Nintendo ones.
    s = re.sub(r'(#define AR6014_HW_2_3_OTP_FILE\s+)"[^"]+"',
               r'\g<1>"eeprom.bin"', s)
    s = re.sub(r'(AR6014_HW_2_3_FW_DIR\s+)"/bdata.bin"',
               r'\g<1>"/eeprom.data"', s)
    s, ch = replace_once(
        s,
        '#define AR6004_HW_3_0_DEFAULT_BOARD_DATA_FILE AR6004_HW_3_0_FW_DIR "/bdata.bin"\n',
        '#define AR6004_HW_3_0_DEFAULT_BOARD_DATA_FILE AR6004_HW_3_0_FW_DIR "/bdata.bin"\n'
        + defs, "core.h")
    if s != orig:
        p.write_text(s)
    print("    core.h: AR6014 definitions %s" % ("updated" if s != orig else "present"))

    # ------------------------------------------------------------------
    # target.h: AR6014 host-interest base + VTOP
    # ------------------------------------------------------------------
    p = a / "target.h"
    orig = p.read_text()
    s = orig
    # Upgrade an earlier run's address.  The AR6014 host-interest base is
    # 0x520000, NOT 0x520400: GBATEK's table above and the 3dbrew FIRM page
    # ("Atheros RAM Vars/Host Interest address (0x520000 on 3DS)") both give
    # 0x520000, and the DSi AR6013 firmware's own header carries
    # (00520000h, 00520000h, 00020000h) as RAM vars/base/size.  The host
    # interest area is only 0x100 bytes, so 0x520400 is 0x400 bytes past the
    # end of the struct - writing there corrupts firmware data and the target
    # reads zeros/garbage for every HI item.  Also collapse any accidental
    # duplicate lines from earlier script versions.
    s = re.sub(r"(#define ATH6KL_AR6014_HI_START_ADDR\s+)0x[0-9a-fA-F]+",
               r"\g<1>0x00520000", s)
    s = re.sub(r"(#define ATH6KL_AR6014_HI_START_ADDR[^\n]*\n)"
               r"(?:[ \t]*#define ATH6KL_AR6014_HI_START_ADDR[^\n]*\n)+",
               r"\1", s)
    s, ch = replace_once(
        s,
        "#define ATH6KL_AR6004_HI_START_ADDR           0x00400800\n",
        "#define ATH6KL_AR6004_HI_START_ADDR           0x00400800\n"
        "#define ATH6KL_AR6014_HI_START_ADDR           0x00520000 /* 3DS AR6014G */\n",
        "target.h HI")
    s, ch2 = replace_once(
        s,
        "#define AR6003_VTOP(vaddr) ((vaddr) & 0x001fffff)\n"
        "#define AR6004_VTOP(vaddr) (vaddr)\n"
        "\n"
        "#define TARG_VTOP(target_type, vaddr) \\\n"
        "\t(((target_type) == TARGET_TYPE_AR6003) ? AR6003_VTOP(vaddr) : \\\n"
        "\t(((target_type) == TARGET_TYPE_AR6004) ? AR6004_VTOP(vaddr) : 0))\n",
        "#define AR6003_VTOP(vaddr) ((vaddr) & 0x001fffff)\n"
        "#define AR6002_VTOP(vaddr) ((vaddr) & 0x001fffff)\n"
        "#define AR6004_VTOP(vaddr) (vaddr)\n"
        "\n"
        "#define TARG_VTOP(target_type, vaddr) \\\n"
        "\t(((target_type) == TARGET_TYPE_AR6003) ? AR6003_VTOP(vaddr) : \\\n"
        "\t(((target_type) == TARGET_TYPE_AR6004) ? AR6004_VTOP(vaddr) : \\\n"
        "\t(((target_type) == TARGET_TYPE_AR6002) ? AR6003_VTOP(vaddr) : 0)))\n",
        "target.h VTOP")
    if s != orig:
        p.write_text(s)
    print("    target.h: AR6014 HI/VTOP %s" % ("updated" if s != orig else "present"))

    # ------------------------------------------------------------------
    # core.h: host-interest lookup must know AR6014
    # ------------------------------------------------------------------
    p = a / "core.h"
    s = p.read_text()
    s, ch = replace_once(
        s,
        "\tif (ar->target_type == TARGET_TYPE_AR6003)\n"
        "\t\taddr = ATH6KL_AR6003_HI_START_ADDR + item_offset;\n"
        "\telse if (ar->target_type == TARGET_TYPE_AR6004)\n"
        "\t\taddr = ATH6KL_AR6004_HI_START_ADDR + item_offset;\n",
        "\tif (ar->target_type == TARGET_TYPE_AR6003)\n"
        "\t\taddr = ATH6KL_AR6003_HI_START_ADDR + item_offset;\n"
        "\telse if (ar->target_type == TARGET_TYPE_AR6004)\n"
        "\t\taddr = ATH6KL_AR6004_HI_START_ADDR + item_offset;\n"
        "\telse if (ar->target_type == TARGET_TYPE_AR6002)\n"
        "\t\taddr = ATH6KL_AR6014_HI_START_ADDR + item_offset;\n",
        "core.h HI lookup")
    if ch:
        p.write_text(s)
    print("    core.h: AR6014 host-interest lookup %s" % ("added" if ch else "present"))

    # ------------------------------------------------------------------
    # init.c: AR6014 hw table entry
    # ------------------------------------------------------------------
    p = a / "init.c"
    s = p.read_text()
    entry = (
        "\t{\n"
        "\t\t/* Nintendo 3DS / New 3DS AR6014G (AR6002/hw2 family).\n"
        "\t\t * These are not guesses: they are the NWM downloader's\n"
        "\t\t * literal-pool constants (main.dst 0x524c00,\n"
        "\t\t * stub.code.dst 0x527000, database.dst 0x53fe18) and the\n"
        "\t\t * stub execute address stub.code.dst + 0x400000 = 0x927000.\n"
        "\t\t * The host-interest base is 0x520000 (GBATEK's RAM Host Int\n"
        "\t\t * column for AR6013/AR6014, the 3dbrew FIRM Atheros struct,\n"
        "\t\t * and the AR6013 firmware header all agree).  See\n"
        "\t\t * nwm-to-ath6k.py. */\n"
        "\t\t.id\t\t\t\t= AR6014_HW_2_3_VERSION,\n"
        "\t\t.name\t\t\t\t= \"ar6014 hw2 (Nintendo 3DS)\",\n"
        "\t\t.dataset_patch_addr\t\t= 0x53fe18,\n"
        "\t\t.app_load_addr\t\t\t= 0x524c00,\n"
        "\t\t.app_start_override_addr\t= 0x927000,\n"
        "\t\t.board_addr\t\t\t= 0x524c00,\n"
        "\t\t.board_ext_data_addr\t\t= 0,\n"
        "\t\t.reserved_ram_size\t\t= 0,\n"
        "\t\t.refclk_hz\t\t\t= 40000000,\n"
        "\t\t.uarttx_pin\t\t\t= 8,\n"
        "\t\t.flags\t\t\t\t= 0,\n"
        "\t\t.fw = {\n"
        "\t\t\t.dir\t\t= AR6014_HW_2_3_FW_DIR,\n"
        "\t\t\t.otp\t\t= AR6014_HW_2_3_OTP_FILE,\n"
        "\t\t\t.fw\t\t= AR6014_HW_2_3_FIRMWARE_FILE,\n"
        "\t\t\t.patch\t\t= AR6014_HW_2_3_PATCH_FILE,\n"
        "\t\t},\n"
        "\t\t.fw_board\t\t\t= AR6014_HW_2_3_BOARD_DATA_FILE,\n"
        "\t\t.fw_default_board\t\t= AR6014_HW_2_3_DEFAULT_BOARD_DATA_FILE,\n"
        "\t},\n"
        "};\n")
    # Upgrade an earlier run: drop any existing AR6014 hw_list entry (it is
    # the last entry) so the corrected one below is added once.
    s = re.sub(r"\n\t\{\n\t\t/\* Nintendo 3DS / New 3DS AR6014G.*?\n\t\},\n(?=\};)",
               "\n", s, flags=re.S)
    # replace the closing of hw_list (the first "};" after the AR6004 3.0 entry)
    anchor = "\t\t.fw_default_board\t= AR6004_HW_3_0_DEFAULT_BOARD_DATA_FILE,\n\t},\n};\n"
    s, ch = replace_once(
        s, anchor,
        "\t\t.fw_default_board\t= AR6004_HW_3_0_DEFAULT_BOARD_DATA_FILE,\n\t},\n"
        + entry, "init.c hw_list")
    if ch:
        p.write_text(s)
    print("    init.c: AR6014 hw entry %s" % ("added" if ch else "present"))

    # ------------------------------------------------------------------
    # sdio.c: match the AR6014 (catch-all on the Atheros vendor)
    # ------------------------------------------------------------------
    p = a / "sdio.c"
    s = p.read_text()
    ids = kd / "include/linux/mmc/sdio_ids.h"
    t = ids.read_text()
    ids_changed = False
    sdio_changed = False

    # The real AR6014G SDIO device id, observed on hardware during the
    # 2026-10-01 bring-up:
    #     mmc0:0001:1: vendor=0x0271 device=0x0201 class=0x00
    # GBATEK: AR6013/AR6014 SDIO MANFID = 0x02010271 (AR6002 = 0x02000271),
    # so the id is 0x0201, not the 0x0200 the first cut guessed.  Normalise
    # away the old guess so re-running this script stays idempotent.
    if "SDIO_DEVICE_ID_ATHEROS_AR6002_00" in t:
        t = t.replace(
            "#define SDIO_DEVICE_ID_ATHEROS_AR6002_00\t0x0200 /* 3DS AR6014G */\n", "", 1)
        t = t.replace("#define SDIO_DEVICE_ID_ATHEROS_AR6002_00\t0x0200\n", "", 1)
        ids_changed = True
    if "SDIO_DEVICE_ID_ATHEROS_AR6014_00" not in t:
        anchor = "#define SDIO_DEVICE_ID_ATHEROS_AR6003_00\t0x0300\n"
        if anchor not in t:
            die("sdio_ids.h: AR6003_00 anchor not found")
        t = t.replace(
            anchor,
            "#define SDIO_DEVICE_ID_ATHEROS_AR6014_00\t0x0201 /* 3DS AR6014G (observed) */\n"
            + anchor, 1)
        ids_changed = True

    if "SDIO_DEVICE_ID_ATHEROS_AR6002_00" in s:
        s = s.replace(
            "\t{SDIO_DEVICE(SDIO_VENDOR_ID_ATHEROS, SDIO_DEVICE_ID_ATHEROS_AR6002_00)},\n",
            "", 1)
        sdio_changed = True
    if "SDIO_DEVICE_ID_ATHEROS_AR6014_00" not in s:
        anchor = "\t{SDIO_DEVICE(SDIO_VENDOR_ID_ATHEROS, SDIO_DEVICE_ID_ATHEROS_AR6003_00)},\n"
        if anchor not in s:
            die("sdio.c: AR6003_00 table anchor not found")
        s = s.replace(
            anchor, anchor +
            "\t{SDIO_DEVICE(SDIO_VENDOR_ID_ATHEROS, SDIO_DEVICE_ID_ATHEROS_AR6014_00)},\n",
            1)
        sdio_changed = True
    # Keep the vendor-wide fallback for bring-up (harmless; specific ids win).
    if "SDIO_ANY_ID" not in s:
        s = s.replace("\t{},\n};\n\nMODULE_DEVICE_TABLE(sdio, ath6kl_sdio_devices);",
                      "\t{SDIO_DEVICE(SDIO_VENDOR_ID_ATHEROS, SDIO_ANY_ID)},\n"
                      "\t{},\n};\n\nMODULE_DEVICE_TABLE(sdio, ath6kl_sdio_devices);", 1)
        sdio_changed = True

    if ids_changed:
        ids.write_text(t)
    if sdio_changed:
        p.write_text(s)
    print("    sdio.c: AR6014 id 0x0201 %s; vendor fallback %s" % (
        "added" if sdio_changed else "present",
        "present" if "SDIO_ANY_ID" in s else "missing"))

    # ------------------------------------------------------------------
    # core.c: print the real target info (the version alone is not enough)
    # ------------------------------------------------------------------
    p = a / "core.c"
    s = p.read_text()
    s, ch = replace_once(
        s,
        "\tar->wiphy->hw_version = le32_to_cpu(targ_info.version);\n"
        "\n"
        "\tret = ath6kl_init_hw_params(ar);\n",
        "\tar->wiphy->hw_version = le32_to_cpu(targ_info.version);\n"
        "\tath6kl_info(\"target: ver=0x%x type=0x%x (AR6002/hw2 family=2)\\n\",\n"
        "\t\t    ar->version.target_ver, ar->target_type);\n"
        "\n"
        "\tret = ath6kl_init_hw_params(ar);\n",
        "core.c target log")
    if ch:
        p.write_text(s)
    print("    core.c: target-info log %s" % ("added" if ch else "present"))

    # ------------------------------------------------------------------
    # init.c: the AR6002/hw2 (AR6014G) upload path.  The mainline driver
    # only ever handled AR6003/AR6004; the AR6002 SDK (and Nintendo's own
    # NWM downloader) use a different order:
    #
    #   stub data -> stub code -> execute stub -> Main (LZ) -> ROM patch
    #
    # with the "board file" slot carrying Nintendo's Stub.data and the
    # "OTP" slot carrying Stub.code.  Addresses are in the hw_list entry.
    # ------------------------------------------------------------------
    p = a / "init.c"
    s = p.read_text()

    # Upgrade: the first cut set hi_board_data/hi_board_data_initialized for
    # AR6002, but the AR6002 SDK leaves the board data to the eeprom stub --
    # claiming it made the firmware read the stub bytes as board data.
    before_board = s
    s = s.replace(
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\t/* AR6002/hw2 (AR6014G): the \"board file\" is Nintendo's\n"
        "\t\t * NWM Stub.data (AR6002 SDK eeprom.data).  It is written\n"
        "\t\t * raw to app_load_addr, which the stub reads before the\n"
        "\t\t * Main image overwrites it.  No extended board data. */\n"
        "\t\tboard_address = ar->hw.board_addr;\n"
        "\t\tath6kl_bmi_write_hi32(ar, hi_board_data, board_address);\n"
        "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
        "\t\t\t   \"writing AR6002 stub data to 0x%x (%zd B)\\n\",\n"
        "\t\t\t   board_address, ar->fw_board_len);\n"
        "\t\tret = ath6kl_bmi_write(ar, board_address, ar->fw_board,\n"
        "\t\t\t\t       ar->fw_board_len);\n"
        "\t\tif (ret) {\n"
        "\t\t\tath6kl_err(\"AR6002 stub data write failed: %d\\n\", ret);\n"
        "\t\t\treturn ret;\n"
        "\t\t}\n"
        "\t\tath6kl_bmi_write_hi32(ar, hi_board_data_initialized, 1);\n"
        "\t\treturn 0;\n"
        "\t}\n",
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\t/* AR6002/hw2 (AR6014G): the \"board file\" is Nintendo's\n"
        "\t\t * NWM Stub.data (AR6002 SDK eeprom.data), written raw to\n"
        "\t\t * app_load_addr.  The AR6002 SDK leaves hi_board_data to\n"
        "\t\t * the eeprom stub -- do NOT claim it here.  No extended\n"
        "\t\t * board data. */\n"
        "\t\tboard_address = ar->hw.board_addr;\n"
        "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
        "\t\t\t   \"writing AR6002 stub data to 0x%x (%zd B)\\n\",\n"
        "\t\t\t   board_address, ar->fw_board_len);\n"
        "\t\tret = ath6kl_bmi_write(ar, board_address, ar->fw_board,\n"
        "\t\t\t\t       ar->fw_board_len);\n"
        "\t\tif (ret) {\n"
        "\t\t\tath6kl_err(\"AR6002 stub data write failed: %d\\n\", ret);\n"
        "\t\t\treturn ret;\n"
        "\t\t}\n"
        "\t\treturn 0;\n"
        "\t}\n")
    board_dropped = (s != before_board)

    s, ch = replace_once_or_applied(
        s,
        "\tif (WARN_ON(ar->fw_board == NULL))\n"
        "\t\treturn -ENOENT;\n"
        "\n"
        "\t/*\n"
        "\t * Determine where in Target RAM to write Board Data.\n",
        "\tif (WARN_ON(ar->fw_board == NULL))\n"
        "\t\treturn -ENOENT;\n"
        "\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\t/* AR6002/hw2 (AR6014G): the \"board file\" is Nintendo's\n"
        "\t\t * NWM Stub.data (AR6002 SDK eeprom.data), written raw to\n"
        "\t\t * app_load_addr.  The AR6002 SDK leaves hi_board_data to\n"
        "\t\t * the eeprom stub -- do NOT claim it here.  No extended\n"
        "\t\t * board data. */\n"
        "\t\tboard_address = ar->hw.board_addr;\n"
        "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
        "\t\t\t   \"writing AR6002 stub data to 0x%x (%zd B)\\n\",\n"
        "\t\t\t   board_address, ar->fw_board_len);\n"
        "\t\tret = ath6kl_bmi_write(ar, board_address, ar->fw_board,\n"
        "\t\t\t\t       ar->fw_board_len);\n"
        "\t\tif (ret) {\n"
        "\t\t\tath6kl_err(\"AR6002 stub data write failed: %d\\n\", ret);\n"
        "\t\t\treturn ret;\n"
        "\t\t}\n"
        "\t\treturn 0;\n"
        "\t}\n"
        "\n"
        "\t/*\n"
        "\t * Determine where in Target RAM to write Board Data.\n",
        "AR6002 stub data write failed",
        "init.c AR6002 board")

    s, ch2 = replace_once_or_applied(
        s,
        "\tif (ar->fw_otp == NULL)\n"
        "\t\treturn 0;\n"
        "\n"
        "\taddress = ar->hw.app_load_addr;\n",
        "\tif (ar->fw_otp == NULL)\n"
        "\t\treturn 0;\n"
        "\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\t/* AR6002/hw2: the \"OTP\" file is Nintendo's NWM\n"
        "\t\t * Stub.code, uploaded raw to stub.code.dst and executed\n"
        "\t\t * through the Xtensa instruction window. */\n"
        "\t\taddress = ar->hw.app_start_override_addr - 0x400000;\n"
        "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
        "\t\t\t   \"writing AR6002 stub to 0x%x (%zd B)\\n\",\n"
        "\t\t\t   address, ar->fw_otp_len);\n"
        "\t\tret = ath6kl_bmi_write(ar, address, ar->fw_otp,\n"
        "\t\t\t\t       ar->fw_otp_len);\n"
        "\t\tif (ret) {\n"
        "\t\t\tath6kl_err(\"Failed to upload AR6002 stub: %d\\n\", ret);\n"
        "\t\t\treturn ret;\n"
        "\t\t}\n"
        "\t\tparam = 0;\n"
        "\t\tath6kl_dbg(ATH6KL_DBG_BOOT,\n"
        "\t\t\t   \"executing AR6002 stub at 0x%x\\n\",\n"
        "\t\t\t   ar->hw.app_start_override_addr);\n"
        "\t\treturn ath6kl_bmi_execute(ar, ar->hw.app_start_override_addr,\n"
        "\t\t\t\t\t  &param);\n"
        "\t}\n"
        "\n"
        "\taddress = ar->hw.app_load_addr;\n",
        "Failed to upload AR6002 stub",
        "init.c AR6002 otp")

    # An earlier run inserted a whole early-return AR6002 branch into
    # ath6kl_init_upload(); drop it so the setup below can run.
    OLD_BRANCH = (
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\tath6kl_info(\"AR6002/AR6014: hi=0x%x app_load=0x%x patch=0x%x exec=0x%x\\n\",\n"
        "\t\t\t    ATH6KL_AR6014_HI_START_ADDR, ar->hw.app_load_addr,\n"
        "\t\t\t    ar->hw.dataset_patch_addr,\n"
        "\t\t\t    ar->hw.app_start_override_addr);\n"
        "\t\t/* AR6002/hw2 (AR6014G): no AR6003 mailbox/PLL bring-up.\n"
        "\t\t * The AR6002 SDK boot order is stub data -> stub code ->\n"
        "\t\t * execute stub -> Main (LZ) -> ROM patch, then\n"
        "\t\t * hi_option_flag |= 1.  See nwm-to-ath6k.py. */\n"
        "\t\tstatus = ath6kl_upload_board_file(ar);\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t\tstatus = ath6kl_upload_otp(ar);\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t\tstatus = ath6kl_upload_firmware(ar);\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t\tstatus = ath6kl_upload_patch(ar);\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t\t{\n"
        "\t\t\tu32 opt = 0;\n"
        "\t\t\tath6kl_bmi_read_hi32(ar, hi_option_flag, &opt);\n"
        "\t\t\tstatus = ath6kl_bmi_write_hi32(ar, hi_option_flag,\n"
        "\t\t\t\t\t\t       opt | 1);\n"
        "\t\t}\n"
        "\t\treturn status;\n"
        "\t}\n"
        "\n")
    before = s
    s = s.replace(OLD_BRANCH, "")
    dropped = (s != before)

    # Allow AR6002 through, and log the addresses we are about to use.
    s, ch3 = replace_once_or_applied(
        s,
        "static int ath6kl_init_upload(struct ath6kl *ar)\n"
        "{\n"
        "\tu32 param, options, sleep, address;\n"
        "\tint status = 0;\n"
        "\n"
        "\tif (ar->target_type != TARGET_TYPE_AR6003 &&\n"
        "\t    ar->target_type != TARGET_TYPE_AR6004)\n"
        "\t\treturn -EINVAL;\n",
        "static int ath6kl_init_upload(struct ath6kl *ar)\n"
        "{\n"
        "\tu32 param, options, sleep, address;\n"
        "\tint status = 0;\n"
        "\n"
        "\tif (ar->target_type != TARGET_TYPE_AR6002 &&\n"
        "\t    ar->target_type != TARGET_TYPE_AR6003 &&\n"
        "\t    ar->target_type != TARGET_TYPE_AR6004)\n"
        "\t\treturn -EINVAL;\n"
        "\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002)\n"
        "\t\tath6kl_info(\"AR6002/AR6014: hi=0x%x app_load=0x%x patch=0x%x exec=0x%x\\n\",\n"
        "\t\t\t    ATH6KL_AR6014_HI_START_ADDR, ar->hw.app_load_addr,\n"
        "\t\t\t    ar->hw.dataset_patch_addr,\n"
        "\t\t\t    ar->hw.app_start_override_addr);\n",
        "AR6002/AR6014: hi=0x%x",
        "init.c AR6002 target check")

    # AR6002 runs its CPU at 40/44MHz (not the AR6003's 80/88MHz) and has
    # no analog PLL register write (staging ath6kl: ar6000_init_upload()).
    s, ch4 = replace_once(
        s,
        "\t/* program analog PLL register */\n"
        "\t/* no need to control 40/44MHz clock on AR6004 */\n"
        "\tif (ar->target_type != TARGET_TYPE_AR6004) {\n"
        "\t\tstatus = ath6kl_bmi_reg_write(ar, ATH6KL_ANALOG_PLL_REGISTER,\n"
        "\t\t\t\t\t      0xF9104001);\n"
        "\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\n"
        "\t\t/* Run at 80/88MHz by default */\n"
        "\t\tparam = SM(CPU_CLOCK_STANDARD, 1);\n"
        "\n"
        "\t\taddress = RTC_BASE_ADDRESS + CPU_CLOCK_ADDRESS;\n"
        "\t\tstatus = ath6kl_bmi_reg_write(ar, address, param);\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t}\n",
        "\t/* program analog PLL register */\n"
        "\t/* no need to control 40/44MHz clock on AR6004/AR6002 */\n"
        "\tif (ar->target_type != TARGET_TYPE_AR6004 &&\n"
        "\t    ar->target_type != TARGET_TYPE_AR6002) {\n"
        "\t\tstatus = ath6kl_bmi_reg_write(ar, ATH6KL_ANALOG_PLL_REGISTER,\n"
        "\t\t\t\t\t      0xF9104001);\n"
        "\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t}\n"
        "\n"
        "\t/* AR6003 runs at 80/88MHz, the AR6002/hw2 family at 40/44MHz */\n"
        "\tparam = SM(CPU_CLOCK_STANDARD,\n"
        "\t\t   (ar->target_type == TARGET_TYPE_AR6002) ? 0 : 1);\n"
        "\n"
        "\taddress = RTC_BASE_ADDRESS + CPU_CLOCK_ADDRESS;\n"
        "\tstatus = ath6kl_bmi_reg_write(ar, address, param);\n"
        "\tif (status)\n"
        "\t\treturn status;\n",
        "init.c AR6002 clock")

    # AR6002: only run the LPO calibration when no external clock is used.
    s, ch5 = replace_once(
        s,
        "\tparam = 0;\n"
        "\taddress = RTC_BASE_ADDRESS + LPO_CAL_ADDRESS;\n"
        "\tparam = SM(LPO_CAL_ENABLE, 1);\n"
        "\tstatus = ath6kl_bmi_reg_write(ar, address, param);\n"
        "\tif (status)\n"
        "\t\treturn status;\n",
        "\tparam = 0;\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\tstatus = ath6kl_bmi_read_hi32(ar, hi_ext_clk_detected,\n"
        "\t\t\t\t\t      &param);\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t}\n"
        "\n"
        "\tif (param != 1) {\n"
        "\t\taddress = RTC_BASE_ADDRESS + LPO_CAL_ADDRESS;\n"
        "\t\tparam = SM(LPO_CAL_ENABLE, 1);\n"
        "\t\tstatus = ath6kl_bmi_reg_write(ar, address, param);\n"
        "\t\tif (status)\n"
        "\t\t\treturn status;\n"
        "\t}\n",
        "init.c AR6002 lpo")

    # The AR6002 SDK ends with a read-modify-write OR 1 of hi_option_flag
    # (the AR.drone log's final "BMI bit-wise OR" at 0x500410).
    s, ch6 = replace_once(
        s,
        "\taddress = MBOX_BASE_ADDRESS + LOCAL_SCRATCH_ADDRESS;\n"
        "\tparam = options | 0x20;\n"
        "\tstatus = ath6kl_bmi_reg_write(ar, address, param);\n"
        "\tif (status)\n"
        "\t\treturn status;\n"
        "\n"
        "\treturn status;\n"
        "}\n",
        "\taddress = MBOX_BASE_ADDRESS + LOCAL_SCRATCH_ADDRESS;\n"
        "\tparam = options | 0x20;\n"
        "\tstatus = ath6kl_bmi_reg_write(ar, address, param);\n"
        "\tif (status)\n"
        "\t\treturn status;\n"
        "\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\tu32 opt = 0;\n"
        "\n"
        "\t\tath6kl_bmi_read_hi32(ar, hi_option_flag, &opt);\n"
        "\t\tstatus = ath6kl_bmi_write_hi32(ar, hi_option_flag,\n"
        "\t\t\t\t\t       opt | 1);\n"
        "\t}\n"
        "\n"
        "\treturn status;\n"
        "}\n",
        "init.c AR6002 option flag")

    if ch or ch2 or ch3 or ch4 or ch5 or ch6 or dropped or board_dropped:
        p.write_text(s)
    print("    init.c: AR6002 upload path %s"
          % ("updated" if (ch or ch2 or ch3 or ch4 or ch5 or ch6 or dropped
                           or board_dropped) else "present"))

    # ------------------------------------------------------------------
    # wmi.h / wmi.c: the AR6002-family WMI_READY event layout
    #
    # GBATEK ("DSi Atheros Wifi - WMI Misc Events", WMIevent(1001h)) lists
    # several mutually-incompatible WMI_READY layouts; the DSi/3DS
    # AR6002/AR6013/AR6014 firmware returns the 0Ch-byte one, which is
    # macaddr-first and has NO abi_version:
    #     00h macaddr[6], 06h phyCapability, 07h pad, 08h version (u32)
    # mainline ath6kl only knows the 0Fh AR6003 layout (sw_version, abi_version,
    # mac[6], phy_cap), so its wmi_ready_event_rx() rejects the 12-byte event
    # with -EINVAL and never sets WMI_READY -- the boot then stalls at
    # "wmi is not ready or wait was interrupted" even though the firmware IS
    # ready (the log shows "wmi rx id 4097 len 12" / WMI_READY_EVENTID).
    # ------------------------------------------------------------------
    p = a / "wmi.h"
    s = p.read_text()
    s, chw = replace_once_or_applied(
        s,
        "struct wmi_ready_event_2 {\n"
        "\t__le32 sw_version;\n"
        "\t__le32 abi_version;\n"
        "\tu8 mac_addr[ETH_ALEN];\n"
        "\tu8 phy_cap;\n"
        "} __packed;\n",
        "struct wmi_ready_event_2 {\n"
        "\t__le32 sw_version;\n"
        "\t__le32 abi_version;\n"
        "\tu8 mac_addr[ETH_ALEN];\n"
        "\tu8 phy_cap;\n"
        "} __packed;\n"
        "\n"
        "/* AR6002/AR6013/AR6014 (hw2) WMI_READY: the 0Ch-byte DSi/3DS layout.\n"
        " * macaddr first, no abi_version.  See GBATEK DSi Atheros Wifi -\n"
        " * WMI Misc Events, WMIevent(1001h). */\n"
        "struct wmi_ready_event_dsi {\n"
        "\tu8 mac_addr[ETH_ALEN];\n"
        "\tu8 phy_cap;\n"
        "\tu8 pad;\n"
        "\t__le32 version;\n"
        "} __packed;\n",
        "struct wmi_ready_event_dsi",
        "wmi.h DSi ready event")
    if chw:
        p.write_text(s)
    print("    wmi.h: DSi ready-event struct %s" % ("added" if chw else "present"))

    p = a / "wmi.c"
    s = p.read_text()
    s, chw2 = replace_once_or_applied(
        s,
        "static int ath6kl_wmi_ready_event_rx(struct wmi *wmi, u8 *datap, int len)\n"
        "{\n"
        "\tstruct wmi_ready_event_2 *ev = (struct wmi_ready_event_2 *) datap;\n"
        "\n"
        "\tif (len < sizeof(struct wmi_ready_event_2))\n"
        "\t\treturn -EINVAL;\n"
        "\n"
        "\tath6kl_ready_event(wmi->parent_dev, ev->mac_addr,\n"
        "\t\t\t   le32_to_cpu(ev->sw_version),\n"
        "\t\t\t   le32_to_cpu(ev->abi_version), ev->phy_cap);\n"
        "\n"
        "\treturn 0;\n"
        "}\n",
        "static int ath6kl_wmi_ready_event_rx(struct wmi *wmi, u8 *datap, int len)\n"
        "{\n"
        "\tstruct ath6kl *ar = wmi->parent_dev;\n"
        "\n"
        "\tif (ar->target_type == TARGET_TYPE_AR6002) {\n"
        "\t\t/* AR6002/hw2 (AR6014G): the 0Ch-byte DSi/3DS layout. */\n"
        "\t\tstruct wmi_ready_event_dsi *ev =\n"
        "\t\t\t(struct wmi_ready_event_dsi *) datap;\n"
        "\n"
        "\t\tif (len < sizeof(*ev)) {\n"
        "\t\t\tath6kl_err(\"AR6002 WMI_READY too short: %d\\n\", len);\n"
        "\t\t\treturn -EINVAL;\n"
        "\t\t}\n"
        "\n"
        "\t\tath6kl_info(\"AR6002/AR6014: WMI_READY len %d mac %pM ver 0x%x phy %u\\n\",\n"
        "\t\t\t    len, ev->mac_addr, le32_to_cpu(ev->version),\n"
        "\t\t\t    ev->phy_cap);\n"
        "\n"
        "\t\t/* This event carries no ABI version; assume the host ABI. */\n"
        "\t\tath6kl_ready_event(ar, ev->mac_addr,\n"
        "\t\t\t\t   le32_to_cpu(ev->version),\n"
        "\t\t\t\t   ATH6KL_ABI_VERSION, ev->phy_cap);\n"
        "\t\treturn 0;\n"
        "\t}\n"
        "\n"
        "\t{\n"
        "\t\tstruct wmi_ready_event_2 *ev =\n"
        "\t\t\t(struct wmi_ready_event_2 *) datap;\n"
        "\n"
        "\t\tif (len < sizeof(struct wmi_ready_event_2))\n"
        "\t\t\treturn -EINVAL;\n"
        "\n"
        "\t\tath6kl_ready_event(ar, ev->mac_addr,\n"
        "\t\t\t\t   le32_to_cpu(ev->sw_version),\n"
        "\t\t\t\t   le32_to_cpu(ev->abi_version), ev->phy_cap);\n"
        "\t}\n"
        "\n"
        "\treturn 0;\n"
        "}\n",
        "AR6002/AR6014: WMI_READY len",
        "wmi.c DSi ready event")
    if chw2:
        p.write_text(s)
    print("    wmi.c: AR6002 WMI_READY handling %s" % ("added" if chw2 else "present"))

    # A raw byte dump of the ready event.  GBATEK lists five mutually
    # incompatible WMI_READY layouts (07h/0Bh/0Ch/0Fh/10h bytes) and the AR6014
    # reports 0Ch, but the parsed mac looks wrong; dumping
    # the bytes makes the real layout unambiguous without a WMI_DUMP build.
    s, chraw = replace_once_or_applied(
        s,
        "\t\tath6kl_info(\"AR6002/AR6014: WMI_READY len %d mac %pM ver 0x%x phy %u\\n\",\n"
        "\t\t\t    len, ev->mac_addr, le32_to_cpu(ev->version),\n"
        "\t\t\t    ev->phy_cap);\n",
        "\t\tath6kl_info(\"AR6002/AR6014: WMI_READY len %d mac %pM ver 0x%x phy %u\\n\",\n"
        "\t\t\t    len, ev->mac_addr, le32_to_cpu(ev->version),\n"
        "\t\t\t    ev->phy_cap);\n"
        "\t\tif (len >= 12)\n"
        "\t\t\tath6kl_info(\"AR6002/AR6014: WMI_READY raw "
        "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\\n\",\n"
        "\t\t\t\t    datap[0], datap[1], datap[2], datap[3], datap[4], datap[5],\n"
        "\t\t\t\t    datap[6], datap[7], datap[8], datap[9], datap[10], datap[11]);\n",
        "WMI_READY raw %02x",
        "wmi.c raw ready dump")
    if chraw:
        p.write_text(s)
    print("    wmi.c: AR6002 WMI_READY raw dump %s" % ("added" if chraw else "present"))

    print("fix-ath6kl.py: done")

if __name__ == "__main__":
    main()
