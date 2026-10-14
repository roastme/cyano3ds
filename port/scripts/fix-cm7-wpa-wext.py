#!/usr/bin/env python3
# fix-cm7-wpa-wext.py - make CyanogenMod 7.2's wpa_supplicant_6 "wext" driver
#                       work with a *plain cfg80211* driver (mainline ath6kl on
#                       the Nintendo 3DS AR6014G).
#
# Symptom this fixes (observed 2026-10-01, card boot with kernel v10):
#
#     wpa_supplicant: ioctl[SIOCSIWPRIV] (cscan): -1
#     wpa_supplicant: Failed to initiate AP scan.
#     wpa_supplicant: wpa_driver_priv_driver_cmd failed (-1): MACADDR
#     wpa_supplicant: wpa_driver_priv_driver_cmd failed (-1): RSSI
#     wpa_supplicant: wpa_driver_priv_driver_cmd failed (-1): LINKSPEED
#     wpa_supplicant: wpa_driver_priv_driver_cmd failed (-1): RXFILTER-ADD 0
#     wpa_supplicant: wpa_driver_priv_driver_cmd failed (-1): BTCOEXSCAN-STOP
#     wpa_supplicant: CTRL-EVENT-DRIVER-STATE HANGED
#     WifiStateTracker: Wifi Driver reports HUNG - reloading.
#     WifiStateTracker: Disabling interface          <-- and then off/on/off ...
#
# Two separate bugs, both in CM7's Android-flavoured driver_wext.c:
#
#   1. COMBO SCAN.  CM7's WEXT driver defaults to the TI/AR6k vendor "combo
#      scan" (SIOCSIWPRIV "cscan"), which a mainline cfg80211 driver does not
#      implement, so every scan fails.  The AOSP/CM build knob
#      BOARD_WEXT_NO_COMBO_SCAN=true makes it use the standard
#      SIOCSIWSCAN/SIOCGIWSCAN path, which cfg80211's wext-compat *does*
#      implement (CONFIG_CFG80211_WEXT=y in the port's kernel).  That half is
#      done in BoardConfig.mk; it is mentioned here so the pair stays together.
#
#   2. VENDOR DRIVER COMMANDS.  hardware/libhardware_legacy and
#      WifiStateTracker talk to the driver through the TI-style private
#      ioctls (DRIVER MACADDR / RSSI / LINKSPEED / RXFILTER-... / BTCOEXSCAN-...
#      / POWERMODE ...).  wpa_driver_priv_driver_cmd() sends them with
#      SIOCSIWPRIV, which cfg80211's wext-compat does not implement, so each
#      one fails and increments drv->errors.  After
#      WEXT_NUMBER_SEQUENTIAL_ERRORS (4) failures driver_wext emits
#      CTRL-EVENT-DRIVER-STATE HANGED and WifiStateTracker responds by
#      power-cycling the interface -- the endless Wi-Fi on/off loop.
#
# This patch:
#   * adds wpa_driver_wext_generic_cmd(), which answers the commands that have
#     a standard-WEXT equivalent (MACADDR from sysfs, RSSI from SIOCGIWSTATS,
#     LINKSPEED from SIOCGIWRATE) and quietly accepts the vendor-only
#     power/coexistence hints (SCAN-ACTIVE/PASSIVE, POWERMODE, BTCOEX*, RXFILTER*,
#     SETSUSPENDOPT, SCAN-CHANNELS) and START/STOP;
#   * stops a failed private ioctl from counting towards the HANGED threshold
#     (a missing SIOCSIWPRIV command set is "unsupported", not a hung driver).
#
# Usage:
#   python3 port/scripts/fix-cm7-wpa-wext.py [CM7_DIR]   # default /root/p3ds/cm7
#
# Idempotent: safe to run repeatedly.

import pathlib
import os
import sys

CM7_DIR = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/p3ds/cm7"))
DRIVER = (CM7_DIR / "external" / "wpa_supplicant_6" / "wpa_supplicant" /
          "src" / "drivers" / "driver_wext.c")

MARK = "[3ds]"

# ---------------------------------------------------------------------------
# helper inserted immediately before wpa_driver_priv_driver_cmd()
# ---------------------------------------------------------------------------
HELPER = r'''/*
 * [3ds] Generic Android driver-command handling for a plain cfg80211 driver.
 *
 * The 3DS Wi-Fi (Atheros AR6014G, mainline ath6kl) is exposed through
 * CONFIG_CFG80211_WEXT: the standard SIOCSIW* ioctls work, the vendor
 * SIOCSIWPRIV command set (which CM7's Android HAL assumes) does not.  Answer
 * the commands that have a standard-WEXT equivalent, accept the vendor-only
 * power/coexistence hints, and report the rest as handled here so that
 * wpa_driver_priv_driver_cmd() never accumulates failures.
 *
 * Returns the reply length for commands whose reply the caller must keep
 * (MACADDR/RSSI/LINKSPEED), 0 for commands whose generic reply is "OK", and
 * -1 when the command is not known here (caller may still try SIOCSIWPRIV).
 */
static int wpa_driver_wext_generic_cmd(struct wpa_driver_wext_data *drv,
				       const char *cmd, char *buf,
				       size_t buf_len)
{
	struct iwreq iwr;
	int ret;

	if (os_strcasecmp(cmd, "MACADDR") == 0) {
		char path[128], mac[32];
		int fd, n;

		os_snprintf(path, sizeof(path), "/sys/class/net/%s/address",
			    drv->ifname);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			return -1;
		n = read(fd, mac, sizeof(mac) - 1);
		close(fd);
		if (n <= 0)
			return -1;
		mac[n] = '\0';
		while (n > 0 && (mac[n - 1] == '\n' || mac[n - 1] == '\r' ||
				 mac[n - 1] == ' '))
			mac[--n] = '\0';
		os_snprintf(buf, buf_len, "Macaddr = %s", mac);
		return os_strlen(buf);
	}

	if (os_strcasecmp(cmd, "RSSI") == 0 ||
	    os_strcasecmp(cmd, "RSSI-APPROX") == 0) {
		struct iw_statistics stats;
		int level;

		os_memset(&stats, 0, sizeof(stats));
		os_memset(&iwr, 0, sizeof(iwr));
		os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);
		iwr.u.data.pointer = (caddr_t) &stats;
		iwr.u.data.length = sizeof(stats);
		if (ioctl(drv->ioctl_sock, SIOCGIWSTATS, &iwr) < 0)
			return -1;
		level = stats.qual.level;
		if ((stats.qual.updated & IW_QUAL_DBM) && level > 127)
			level -= 256;
		os_snprintf(buf, buf_len, "%s rssi %d", drv->ifname, level);
		return os_strlen(buf);
	}

	if (os_strcasecmp(cmd, "LINKSPEED") == 0) {
		os_memset(&iwr, 0, sizeof(iwr));
		os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);
		if (ioctl(drv->ioctl_sock, SIOCGIWRATE, &iwr) < 0)
			return -1;
		os_snprintf(buf, buf_len, "LinkSpeed %u",
			    (unsigned int) (iwr.u.bitrate.value / 1000000));
		return os_strlen(buf);
	}

	if (os_strcasecmp(cmd, "START") == 0) {
		drv->driver_is_started = TRUE;
		wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "STARTED");
		return 0;
	}

	if (os_strcasecmp(cmd, "STOP") == 0) {
		int flags;

		if ((wpa_driver_wext_get_ifflags(drv, &flags) == 0) &&
		    (flags & IFF_UP)) {
			wpa_printf(MSG_DEBUG, "WEXT: %s when iface is UP",
				   cmd);
			wpa_driver_wext_set_ifflags(drv, flags & ~IFF_UP);
		}
		if (drv->driver_is_started) {
			drv->driver_is_started = FALSE;
			wpa_msg(drv->ctx, MSG_INFO,
				WPA_EVENT_DRIVER_STATE "STOPPED");
		}
		return 0;
	}

	/*
	 * Vendor power/coexistence/scan hints with no cfg80211 equivalent.
	 * Accepting them is harmless: they only tune TI firmware behaviour.
	 */
	if (os_strncasecmp(cmd, "SCAN-CHANNELS", 13) == 0 ||
	    os_strcasecmp(cmd, "SCAN-ACTIVE") == 0 ||
	    os_strcasecmp(cmd, "SCAN-PASSIVE") == 0 ||
	    os_strncasecmp(cmd, "POWERMODE", 9) == 0 ||
	    os_strncasecmp(cmd, "BTCOEXMODE", 10) == 0 ||
	    os_strncasecmp(cmd, "BTCOEXSCAN-", 11) == 0 ||
	    os_strncasecmp(cmd, "RXFILTER-", 9) == 0 ||
	    os_strncasecmp(cmd, "SETSUSPENDOPT", 13) == 0)
		return 0;

	return -1;
}


'''

CALL_ANCHOR = """	if (!drv->driver_is_started && (os_strcasecmp(cmd, "START") != 0)) {
		wpa_printf(MSG_ERROR,"WEXT: Driver not initialized yet");
		return -1;
	}
"""

CALL_INSERT = CALL_ANCHOR + """
	/* [3ds] cfg80211 wext-compat has no SIOCSIWPRIV command set: answer
	 * the Android driver commands generically instead. */
	ret = wpa_driver_wext_generic_cmd(drv, cmd, buf, buf_len);
	if (ret >= 0)
		return ret;
"""

ERR_ANCHOR = """	if (ret < 0) {
		wpa_printf(MSG_ERROR, "%s failed (%d): %s", __func__, ret, cmd);
		drv->errors++;
		if (drv->errors > WEXT_NUMBER_SEQUENTIAL_ERRORS) {
			drv->errors = 0;
			wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
		}
	} else {"""

ERR_REPLACE = """	if (ret < 0) {
		/* [3ds] ath6kl (cfg80211 + CONFIG_CFG80211_WEXT) has no
		 * vendor SIOCSIWPRIV command set.  A missing vendor command is
		 * "unsupported", not a hung driver: do NOT count it towards
		 * WEXT_NUMBER_SEQUENTIAL_ERRORS, or driver_wext emits
		 * CTRL-EVENT-DRIVER-STATE HANGED and WifiStateTracker
		 * power-cycles Wi-Fi forever (the on/off loop). */
		wpa_printf(MSG_DEBUG, "%s unsupported (%d): %s",
			   __func__, ret, cmd);
	} else {"""

FUNC_ANCHOR = "static int wpa_driver_priv_driver_cmd( void *priv, char *cmd, char *buf, size_t buf_len )"

INCLUDE_ANCHOR = "#include <net/if.h>\n"
INCLUDE_INSERT = ("#include <net/if.h>\n"
                  "#include <fcntl.h>   /* [3ds] open()/O_RDONLY for the "
                  "generic driver commands */\n"
                  "#include <unistd.h>  /* [3ds] read()/close() */\n")


def main() -> int:
    if not DRIVER.is_file():
        print("no %s" % DRIVER, file=sys.stderr)
        return 1

    s = DRIVER.read_text()
    orig = s
    applied = []

    # 0. the generic handler opens a sysfs file; this file predates os_open()
    if "[3ds] open()/O_RDONLY" not in s:
        if INCLUDE_ANCHOR not in s:
            print("cannot find #include <net/if.h> in %s" % DRIVER,
                  file=sys.stderr)
            return 1
        s = s.replace(INCLUDE_ANCHOR, INCLUDE_INSERT, 1)
        applied.append("added <fcntl.h>/<unistd.h>")

    # 1. helper function before wpa_driver_priv_driver_cmd()
    if "wpa_driver_wext_generic_cmd" not in s:
        if FUNC_ANCHOR not in s:
            print("cannot find wpa_driver_priv_driver_cmd() in %s" % DRIVER,
                  file=sys.stderr)
            return 1
        s = s.replace(FUNC_ANCHOR, HELPER + FUNC_ANCHOR, 1)
        applied.append("added wpa_driver_wext_generic_cmd()")

    # 2. call the helper from wpa_driver_priv_driver_cmd()
    if "cfg80211 wext-compat has no SIOCSIWPRIV" not in s:
        if CALL_ANCHOR not in s:
            print("cannot find driver_is_started guard in %s" % DRIVER,
                  file=sys.stderr)
            return 1
        s = s.replace(CALL_ANCHOR, CALL_INSERT, 1)
        applied.append("wpa_driver_priv_driver_cmd(): generic command path")

    # 3. never escalate a failed private ioctl to CTRL-EVENT-DRIVER-STATE HANGED
    if "not a hung driver" not in s:
        if ERR_ANCHOR not in s:
            print("cannot find SIOCSIWPRIV error branch in %s" % DRIVER,
                  file=sys.stderr)
            return 1
        s = s.replace(ERR_ANCHOR, ERR_REPLACE, 1)
        applied.append("SIOCSIWPRIV failure no longer counts towards HANGED")

    if s != orig:
        DRIVER.write_text(s)

    if applied:
        for a in applied:
            print("    driver_wext.c: %s" % a)
    else:
        print("    driver_wext.c: already patched")
    return 0


if __name__ == "__main__":
    sys.exit(main())
