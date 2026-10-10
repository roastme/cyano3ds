#!/usr/bin/env python3
"""
fix-ath6kl-boarddata-diag.py - dump the AR6002 board data the Stub fills.

The AR6014 firmware reports a bogus MAC in WMI_READY: the first byte has the
multicast bit set, so it is not a usable source address and association would
fail.  The real 3DS MAC lives in the NAND config; the ARM11
NWM downloader normally hands it to the firmware, but we bypass that.

The AR6002 Stub.code (Nintendo's NWM `stub_code`) reads the module I2C EEPROM
and writes the board data to the address in `hi_board_data` (observed
0x520e00).  GBATEK's *DSi Atheros Wifi I2C EEPROM* layout puts the MAC at
offset **0x0A** of that image, so a dump of the first 16 bytes tells us:

  * board data has a valid unicast MAC at +0x0A -> our WMI_READY parse is at
    the wrong offset; and
  * board data is zeros/0xFF there -> the module EEPROM has no MAC and we have
    to inject one (NAND config, or a locally-administered MAC + checksum).

This script adds an idempotent `ath6kl_info()` dump right after the Stub
executes.  Idempotent.
"""

import sys
import os
import pathlib

def die(msg):
    sys.exit("fix-ath6kl-boarddata-diag.py: " + msg)

def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else os.path.expanduser("~/p3ds/src/linux-3ds"))
    p = kd / "drivers/net/wireless/ath/ath6kl/init.c"
    if not p.exists():
        die("no ath6kl init.c at %s" % p)

    s = p.read_text()
    if "board data @0x%x" in s:
        print("    init.c: already patched")
        return

    old = ("\t\tath6kl_info(\"AR6002/AR6014: stub returned %d (param=0x%x)\\n\",\n"
           "\t\t\t    ret, param);\n"
           "\t\treturn ret;\n"
           "\t}\n")
    new = ("\t\tath6kl_info(\"AR6002/AR6014: stub returned %d (param=0x%x)\\n\",\n"
           "\t\t\t    ret, param);\n"
           "\n"
           "\t\t/* The Stub has just read the module I2C EEPROM into the\n"
           "\t\t * board data buffer.  GBATEK's EEPROM layout puts the MAC\n"
           "\t\t * at +0x0A, so dump the head of the image: it tells whether\n"
           "\t\t * the firmware has a real MAC (and hence whether the\n"
           "\t\t * WMI_READY parse is wrong) or none at all. */\n"
           "\t\tif (ret == 0) {\n"
           "\t\t\tu32 bd_addr = 0, bd[4] = { 0 };\n"
           "\n"
           "\t\t\tif (ath6kl_bmi_read_hi32(ar, hi_board_data, &bd_addr) == 0 &&\n"
           "\t\t\t    bd_addr != 0 &&\n"
           "\t\t\t    ath6kl_bmi_read(ar, bd_addr, (u8 *) bd,\n"
           "\t\t\t\t\t    sizeof(bd)) == 0)\n"
           "\t\t\t\tath6kl_info(\"AR6002/AR6014: board data @0x%x: %08x %08x %08x %08x (mac @+0x0a)\\n\",\n"
           "\t\t\t\t\t    bd_addr, bd[0], bd[1], bd[2], bd[3]);\n"
           "\t\t}\n"
           "\t\treturn ret;\n"
           "\t}\n")
    if old not in s:
        die("init.c: AR6002 stub-return anchor not found")
    p.write_text(s.replace(old, new, 1))
    print("    init.c: AR6002 board data dump added")

if __name__ == "__main__":
    main()
