#!/usr/bin/env python3
"""
fix-ath6kl-ar6002-hb-type1.py - stop forcing the WMI heartbeat on AR6002.

The heartbeat was forced on (fix-ath6kl-heartbeat.py) because the AR6014
**Type4** Main image hangs if the host does not answer its WMI heartbeat.
The port now ships the **Type1** Main image (the "basic internet" firmware,
nocash's known-working wifiboot uses it too), and GBATEK is explicit that
Type1 "does simply ignore the timeout":

    If a timeout occurs because of not sending heartbeat commands in time:
    Some firmwares do simply ignoring the timeout (eg. DSi/AR6013, or
    3DS/AR6014 with basic Type1 firmware).  However, other firmwares do
    hang up (eg. 3DS/AR6014 with AP-mode Type4 firmware).

So on Type1 the heartbeat challenge/response is not needed, and if the older
Type1 image happens not to answer the challenge, ath6kl's forced fw-recovery
would restart the firmware every ~6 s and break every scan.  This script:

  * makes AR6002 use a 0 ms heartbeat poll (no challenges, no recovery
    restarts);
  * sets ATH6KL_AR6002_HB_TIMEOUT_SEC to 0, which is what nocash's wifiboot
    sends for Type1 (`heartbeat_timeout equ 0 ;... better: 0=disable`).

ath6kl_recovery_init() still sends WMI_SET_HB_CHALLENGE_RESP_PARAMS (0x47),
now with timeout 0, matching the working reference.  Idempotent.

If the port ever goes back to the Type4 Main image, this script must be
dropped (or the value put back to 2 and the poll to 1000).
"""

import sys
import pathlib


def die(msg):
    sys.exit("fix-ath6kl-ar6002-hb-type1.py: " + msg)


CORE_OLD = (
    "\t\tar->fw_recovery.enable = true;\n"
    "\t\tar->fw_recovery.hb_poll = heart_beat_poll ?\n"
    "\t\t\theart_beat_poll : ATH6KL_AR6002_HB_POLL_MS;\n"
)
CORE_NEW = (
    "\t\t/* [3ds] Type1 firmware ignores its heartbeat timeout, so do\n"
    "\t\t * not arm the challenge poll: an unanswered challenge would\n"
    "\t\t * make fw-recovery restart the firmware every ~6 s (and the\n"
    "\t\t * old Type1 image need not answer).  See\n"
    "\t\t * fix-ath6kl-ar6002-hb-type1.py. */\n"
    "\t\tar->fw_recovery.enable = true;\n"
    "\t\tar->fw_recovery.hb_poll = 0;\n"
)

H_OLD = "#define ATH6KL_AR6002_HB_TIMEOUT_SEC\t2\n"
H_NEW = ("/* 0 = disabled; the Type1 Main image ignores the timeout and the\n"
         " * challenge poll is off, see fix-ath6kl-ar6002-hb-type1.py */\n"
         "#define ATH6KL_AR6002_HB_TIMEOUT_SEC\t0\n")


def main():
    kd = pathlib.Path(sys.argv[1] if len(sys.argv) > 1
                      else "/root/p3ds/src/linux-3ds")
    ath = kd / "drivers/net/wireless/ath/ath6kl"
    corec = ath / "core.c"
    coreh = ath / "core.h"
    for p in (corec, coreh):
        if not p.is_file():
            die("no %s" % p)

    changed = []
    s = corec.read_text()
    if "hb_poll = 0;" in s:
        pass
    elif CORE_OLD in s:
        corec.write_text(s.replace(CORE_OLD, CORE_NEW, 1))
        changed.append("core.c: hb_poll = 0 for AR6002")
    else:
        die("core.c: heartbeat anchor not found")

    s = coreh.read_text()
    if "ATH6KL_AR6002_HB_TIMEOUT_SEC\t0" in s:
        pass
    elif H_OLD in s:
        coreh.write_text(s.replace(H_OLD, H_NEW, 1))
        changed.append("core.h: HB timeout 0")
    else:
        die("core.h: HB timeout anchor not found")

    if changed:
        print("fix-ath6kl-ar6002-hb-type1.py: " + "; ".join(changed))
    else:
        print("fix-ath6kl-ar6002-hb-type1.py: already applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
