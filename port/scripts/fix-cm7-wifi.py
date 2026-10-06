#!/usr/bin/env python3
# fix-cm7-wifi.py - make CyanogenMod 7.2's libhardware_legacy Wi-Fi HAL work
#                   with the *in-kernel* ath6kl (Nintendo 3DS AR6014G).
#
# The stock hardware/libhardware_legacy/wifi/wifi.c assumes the WLAN driver is
# a loadable module and does  insmod("/system/lib/modules/wlan.ko")  in
# wifi_load_driver();  on the 3DS the driver is built into zImage (the firmware
# is loaded from the initramfs before /system exists), so that insmod always
# fails and WifiService logs the notorious
#
#     WifiService: Failed to load Wi-Fi driver.
#
# and Settings shows "Error" the moment Wi-Fi is toggled.
#
# This patch makes the HAL treat an existing wlan0 netdev as "driver loaded":
#
#   * check_driver_loaded()  returns 1 when /sys/class/net/<wifi.interface>
#     exists (so wifi_load_driver() succeeds without an insmod);
#   * wifi_load_driver()     explicitly reports success (and sets the
#     wlan.driver.status property) for that case;
#   * wifi_unload_driver()   reports success without rmmod (there is nothing to
#     unload; the netdev stays and is just rfkill/idle).
#
# The kernel side provides CFG80211_WEXT so the CM7 wpa_supplicant_6 "wext"
# backend can drive ath6kl.  See BoardConfig.mk and device_nintendo3ds.mk.
#
# Usage:
#   python3 port/scripts/fix-cm7-wifi.py [CM7_DIR]     # default /root/p3ds/cm7
#
# Idempotent: safe to run repeatedly.

import pathlib
import re
import sys

CM7_DIR = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "/root/p3ds/cm7")
WIFI_C = CM7_DIR / "hardware" / "libhardware_legacy" / "wifi" / "wifi.c"

MARK = "[3ds]"

HELPER = '''/* %(mark)s The 3DS Wi-Fi (Atheros AR6014G, mainline ath6kl) is built into
 * zImage; there is no .ko for wifi_load_driver() to insmod.  Treat the driver
 * as loaded as soon as its netdev exists in /sys/class/net. */
static int wifi_builtin_driver_present(void)
{
    char ifname[PROPERTY_VALUE_MAX];
    char path[128];

    property_get("wifi.interface", ifname, "wlan0");
    snprintf(path, sizeof(path), "/sys/class/net/%%s", ifname);
    return (access(path, F_OK) == 0);
}

''' % {"mark": MARK}

CHECK_ANCHOR = "static int check_driver_loaded() {"
CHECK_INSERT = '''static int check_driver_loaded() {

    /* %(mark)s built-in ath6kl: no module to insmod, the netdev is enough */
    if (wifi_builtin_driver_present())
        return 1;
''' % {"mark": MARK}

UNLOAD_ANCHOR = """int wifi_unload_driver()
{
    int count = 20; /* wait at most 10 seconds for completion */
"""
UNLOAD_INSERT = """int wifi_unload_driver()
{
    int count = 20; /* wait at most 10 seconds for completion */

    /* %(mark)s built-in ath6kl: there is no module to remove; the netdev
     * stays and Wi-Fi is simply left idle. */
    if (wifi_builtin_driver_present()) {
        property_set(DRIVER_PROP_NAME, "unloaded");
        return 0;
    }
""" % {"mark": MARK}

LOAD_ANCHOR = """    if (check_driver_loaded()) {
        return 0;
    }
"""
LOAD_INSERT = """    if (check_driver_loaded()) {
        return 0;
    }

    /* %(mark)s built-in ath6kl: nothing to insmod, report success. */
    if (wifi_builtin_driver_present()) {
        property_set(DRIVER_PROP_NAME, "ok");
        return 0;
    }
""" % {"mark": MARK}


def main() -> int:
    if not WIFI_C.is_file():
        print("no %s" % WIFI_C, file=sys.stderr)
        return 1

    s = WIFI_C.read_text()
    orig = s
    applied = []

    # 1. helper function, immediately before check_driver_loaded()
    if "wifi_builtin_driver_present" not in s:
        if CHECK_ANCHOR not in s:
            print("cannot find check_driver_loaded() in %s" % WIFI_C, file=sys.stderr)
            return 1
        s = s.replace(CHECK_ANCHOR, HELPER + CHECK_ANCHOR, 1)
        applied.append("added wifi_builtin_driver_present()")

    # 2. check_driver_loaded() returns 1 for the built-in driver
    if "built-in ath6kl: no module to insmod" not in s:
        if CHECK_ANCHOR not in s:
            print("cannot find check_driver_loaded() in %s" % WIFI_C, file=sys.stderr)
            return 1
        # remove the old body head (HELPER may already be in front of it)
        s = s.replace(CHECK_ANCHOR + "\n", CHECK_INSERT, 1)
        applied.append("check_driver_loaded(): built-in driver present -> 1")

    # 3. wifi_unload_driver() succeeds without rmmod
    if "built-in ath6kl: there is no module to remove" not in s:
        if UNLOAD_ANCHOR not in s:
            print("cannot find wifi_unload_driver() in %s" % WIFI_C, file=sys.stderr)
            return 1
        s = s.replace(UNLOAD_ANCHOR, UNLOAD_INSERT, 1)
        applied.append("wifi_unload_driver(): built-in driver -> success")

    # 4. wifi_load_driver() sets wlan.driver.status for the built-in driver
    if "built-in ath6kl: nothing to insmod" not in s:
        if LOAD_ANCHOR not in s:
            print("cannot find wifi_load_driver() body in %s" % WIFI_C, file=sys.stderr)
            return 1
        s = s.replace(LOAD_ANCHOR, LOAD_INSERT, 1)
        applied.append("wifi_load_driver(): built-in driver -> status ok")

    if s != orig:
        WIFI_C.write_text(s)

    if applied:
        for a in applied:
            print("    wifi.c: %s" % a)
    else:
        print("    wifi.c: already patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
