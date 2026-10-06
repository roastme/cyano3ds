//------------------------------------------------------------------------------
// Copyright (c) 2004-2010 Atheros Communications Inc.
// All rights reserved.
//
// 
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
//
//
// Author(s): ="Atheros"
//------------------------------------------------------------------------------

#include <linux/wireless.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/netlink.h>

#include "ar6000_drv.h"


extern A_WAITQUEUE_HEAD arEvent;
extern unsigned int wmitimeout;
extern int reconnect_flag;

/* N3DS_AR6014_NWM_CONNECTION_SCAN:
 * N3DS_AR6014_DISCOVERY_CONNECT_SPLIT: #245 proved that applying NWM's
 * selected-profile scan verbatim to a cfg80211 discovery request can finish
 * in about 100 ms with zero BSS records.  Preserve Nintendo's slot-zero,
 * command-8, command-17, and filter order only at connect submission.
 * N3DS_AR6014_CONNECT_SCAN_DWELL_REGRESSION: the #245->#247 split
 * accidentally replaced the disassembly-verified 20 ms home/active/passive
 * dwell recovered at 0x0012ad70 (command 8 with periods 0xffff and dwell
 * 20 ms) with an invented 105 ms
 * value, on the mistaken theory that it needed to exceed a beacon interval.
 * That caused every real-hardware WPA2 connect to end in NWM disconnect
 * reason 1 (NO_NETWORK_AVAIL) about 4.5s later, even though the identical
 * BSS was already visible to host discovery. Restore the real firmware's
 * 20 ms dwell. */
#define N3DS_NWM_CONNECT_SCAN_DWELL_MS 20
#define N3DS_NWM_CONNECT_SCAN_FLAGS \
    (CONNECT_SCAN_CTRL_FLAGS | ACTIVE_SCAN_CTRL_FLAGS)

/* N3DS_AR6014_SCAN_NO_PROBES: WMI_SET_SCAN_PARAMS was being sent with
 * ACTIVE_SCAN_CTRL_FLAGS set but maxact_scan_per_ssid = 0 -- literally
 * "run an active scan, sending zero probe requests per SSID".  With the 20 ms
 * dwell, which is well under a 100 ms beacon interval, the target-side
 * CONNECT_SCAN had no mechanism left to find the BSS, so every association
 * ended in NWM disconnect reason 1 (NO_NETWORK_AVAIL) about 4.5 s later --
 * even for a -26 dBm AP that host discovery had listed moments earlier.  The
 * #245 note above records these same parameters measured on hardware
 * finishing a scan "in about 100 ms with zero BSS records", which is this
 * same defect observed from the discovery side.  Send real probe requests:
 * three gives margin against one lost probe response inside a short dwell and
 * costs nothing on the single channel a connect scan visits. */
#define N3DS_NWM_SCAN_PROBES_PER_SSID 3

/* N3DS_AR6014_ASSOCIATION_COMPAT:
 * N3DS_AR6014_NWM_CHANNEL_TABLE: NWM 0x0012ad70 calls 0x0011aa2c before
 * its discovery/connect scan.  The 0x00136830 serializer sends command 17
 * with scanParam=0, phyMode=2 (11G), and a MHz channel list.  cfg80211 had
 * supplied channels only to START_SCAN, leaving the persistent target table
 * used by WMI_CONNECT's own search unprogrammed.  Use the command ABI rather
 * than the retired direct target-RAM experiment. */
/* N3DS_AR6014_11G_CHANNEL_LIST: WMI_SET_CHANNEL_PARAMS carries one phyMode
 * for the whole list and 802.11g is not legal on channel 14, so a list that
 * contains 2484 MHz is rejected outright with WMI_CMDERROR errorCode=1.  A
 * rejected command changes nothing on the target, which means the persistent
 * channel table WMI_CONNECT's own profile search reads was never reprogrammed
 * by discovery at all -- it kept whatever the previous connect attempt left
 * behind.  Hardware logged 40 of those rejections in one boot, every one of
 * them right after a 14-entry discovery table and never after a single-channel
 * connect table.  Channel 14 is 11b-only and Japan-only; drop it instead of
 * running discovery a second time in 11b just to cover it. */
static bool n3ds_ar6014_channel_is_11g(u16 mhz)
{
    return mhz >= 2412 && mhz <= 2472 && !((mhz - 2412) % 5);
}

static int n3ds_ar6014_set_search_channel(struct ar6_softc *ar, u16 mhz)
{
    u16 channel_list[1] = { mhz };
    int status;

    if (!n3ds_ar6014_channel_is_11g(mhz))
        return -EINVAL;

    status = wmi_set_channelParams_cmd(ar->arWmi, 0, WMI_11G_MODE,
                                       ARRAY_SIZE(channel_list), channel_list);
    if (status != 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: NWM channel table failed channel=%u status=%d\n",
             mhz, status));
        return status;
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
        ("AR6002 connect: search channel %u; NWM channel table mode=11G count=1\n",
         mhz));
    return 0;
}



#define RATETAB_ENT(_rate, _rateid, _flags) {   \
    .bitrate    = (_rate),                  \
    .flags      = (_flags),                 \
    .hw_value   = (_rateid),                \
}

#define CHAN2G(_channel, _freq, _flags) {   \
    .band           = NL80211_BAND_2GHZ,  \
    .hw_value       = (_channel),           \
    .center_freq    = (_freq),              \
    .flags          = (_flags),             \
    .max_antenna_gain   = 0,                \
    .max_power      = 30,                   \
}

#define CHAN5G(_channel, _flags) {              \
    .band           = NL80211_BAND_5GHZ,      \
    .hw_value       = (_channel),               \
    .center_freq    = 5000 + (5 * (_channel)),  \
    .flags          = (_flags),                 \
    .max_antenna_gain   = 0,                    \
    .max_power      = 30,                       \
}

static struct
ieee80211_rate ar6k_rates[] = {
    RATETAB_ENT(10,  0x1,   0),
    RATETAB_ENT(20,  0x2,   0),
    RATETAB_ENT(55,  0x4,   0),
    RATETAB_ENT(110, 0x8,   0),
    RATETAB_ENT(60,  0x10,  0),
    RATETAB_ENT(90,  0x20,  0),
    RATETAB_ENT(120, 0x40,  0),
    RATETAB_ENT(180, 0x80,  0),
    RATETAB_ENT(240, 0x100, 0),
    RATETAB_ENT(360, 0x200, 0),
    RATETAB_ENT(480, 0x400, 0),
    RATETAB_ENT(540, 0x800, 0),
};

#define ar6k_a_rates     (ar6k_rates + 4)
#define ar6k_a_rates_size    8
#define ar6k_g_rates     (ar6k_rates + 0)
#define ar6k_g_rates_size    12

static struct
ieee80211_channel ar6k_2ghz_channels[] = {
    CHAN2G(1, 2412, 0),
    CHAN2G(2, 2417, 0),
    CHAN2G(3, 2422, 0),
    CHAN2G(4, 2427, 0),
    CHAN2G(5, 2432, 0),
    CHAN2G(6, 2437, 0),
    CHAN2G(7, 2442, 0),
    CHAN2G(8, 2447, 0),
    CHAN2G(9, 2452, 0),
    CHAN2G(10, 2457, 0),
    CHAN2G(11, 2462, 0),
    CHAN2G(12, 2467, 0),
    CHAN2G(13, 2472, 0),
    CHAN2G(14, 2484, 0),
};

static struct
ieee80211_channel ar6k_5ghz_a_channels[] = {
    CHAN5G(34, 0),      CHAN5G(36, 0),
    CHAN5G(38, 0),      CHAN5G(40, 0),
    CHAN5G(42, 0),      CHAN5G(44, 0),
    CHAN5G(46, 0),      CHAN5G(48, 0),
    CHAN5G(52, 0),      CHAN5G(56, 0),
    CHAN5G(60, 0),      CHAN5G(64, 0),
    CHAN5G(100, 0),     CHAN5G(104, 0),
    CHAN5G(108, 0),     CHAN5G(112, 0),
    CHAN5G(116, 0),     CHAN5G(120, 0),
    CHAN5G(124, 0),     CHAN5G(128, 0),
    CHAN5G(132, 0),     CHAN5G(136, 0),
    CHAN5G(140, 0),     CHAN5G(149, 0),
    CHAN5G(153, 0),     CHAN5G(157, 0),
    CHAN5G(161, 0),     CHAN5G(165, 0),
    CHAN5G(184, 0),     CHAN5G(188, 0),
    CHAN5G(192, 0),     CHAN5G(196, 0),
    CHAN5G(200, 0),     CHAN5G(204, 0),
    CHAN5G(208, 0),     CHAN5G(212, 0),
    CHAN5G(216, 0),
};

static struct
ieee80211_supported_band ar6k_band_2ghz = {
    .n_channels = ARRAY_SIZE(ar6k_2ghz_channels),
    .channels = ar6k_2ghz_channels,
    .n_bitrates = ar6k_g_rates_size,
    .bitrates = ar6k_g_rates,
};

static struct
ieee80211_supported_band ar6k_band_5ghz __attribute__((unused)) = {
    .n_channels = ARRAY_SIZE(ar6k_5ghz_a_channels),
    .channels = ar6k_5ghz_a_channels,
    .n_bitrates = ar6k_a_rates_size,
    .bitrates = ar6k_a_rates,
};

static int
ar6k_set_wpa_version(struct ar6_softc *ar, enum nl80211_wpa_versions wpa_version)
{

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: %u\n", __func__, wpa_version));

    if (!wpa_version) {
        ar->arAuthMode = NONE_AUTH;
    } else if (wpa_version & NL80211_WPA_VERSION_1) {
        ar->arAuthMode = WPA_AUTH;
    } else if (wpa_version & NL80211_WPA_VERSION_2) {
        ar->arAuthMode = WPA2_AUTH;
    } else {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: %u not spported\n", __func__, wpa_version));
        return -ENOTSUPP;
    }

    return 0;
}

static int
ar6k_set_auth_type(struct ar6_softc *ar, enum nl80211_auth_type auth_type)
{

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: 0x%x\n", __func__, auth_type));

    switch (auth_type) {
    case NL80211_AUTHTYPE_OPEN_SYSTEM:
        ar->arDot11AuthMode = OPEN_AUTH;
        break;
    case NL80211_AUTHTYPE_SHARED_KEY:
        ar->arDot11AuthMode = SHARED_AUTH;
        break;
    case NL80211_AUTHTYPE_NETWORK_EAP:
        ar->arDot11AuthMode = LEAP_AUTH;
        break;

    case NL80211_AUTHTYPE_AUTOMATIC:
        ar->arDot11AuthMode = OPEN_AUTH;
        ar->arAutoAuthStage = AUTH_OPEN_IN_PROGRESS;
        break;

    default:
        ar->arDot11AuthMode = OPEN_AUTH;
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                        ("%s: 0x%x not spported\n", __func__, auth_type));
        return -ENOTSUPP;
    }

    return 0;
}

static int
ar6k_set_cipher(struct ar6_softc *ar, u32 cipher, bool ucast)
{
    u8 *ar_cipher = ucast ? &ar->arPairwiseCrypto :
                                &ar->arGroupCrypto;
    u8 *ar_cipher_len = ucast ? &ar->arPairwiseCryptoLen :
                                    &ar->arGroupCryptoLen;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                    ("%s: cipher 0x%x, ucast %u\n", __func__, cipher, ucast));

    switch (cipher) {
    case 0:
    case IW_AUTH_CIPHER_NONE:
        *ar_cipher = NONE_CRYPT;
        *ar_cipher_len = 0;
        break;
    case WLAN_CIPHER_SUITE_WEP40:
        *ar_cipher = WEP_CRYPT;
        *ar_cipher_len = 5;
        break;
    case WLAN_CIPHER_SUITE_WEP104:
        *ar_cipher = WEP_CRYPT;
        *ar_cipher_len = 13;
        break;
    case WLAN_CIPHER_SUITE_TKIP:
        *ar_cipher = TKIP_CRYPT;
        *ar_cipher_len = 0;
        break;
    case WLAN_CIPHER_SUITE_CCMP:
        *ar_cipher = AES_CRYPT;
        *ar_cipher_len = 0;
        break;
    default:
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: cipher 0x%x not supported\n", __func__, cipher));
        return -ENOTSUPP;
    }

    return 0;
}

static void
ar6k_set_key_mgmt(struct ar6_softc *ar, u32 key_mgmt)
{
    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: 0x%x\n", __func__, key_mgmt));

    if (WLAN_AKM_SUITE_PSK == key_mgmt) {
        if (WPA_AUTH == ar->arAuthMode) {
            ar->arAuthMode = WPA_PSK_AUTH;
        } else if (WPA2_AUTH == ar->arAuthMode) {
            ar->arAuthMode = WPA2_PSK_AUTH;
        }
    } else if (WLAN_AKM_SUITE_8021X != key_mgmt) {
        ar->arAuthMode = NONE_AUTH;
    }
}

/* N3DS_AR6014_CONNECT_VARIANT_SWEEP: every WMI_CONNECT this driver has sent
 * to the NWM firmware comes back as DISCONNECT reason 1 (NO_NETWORK_AVAIL)
 * about 4.5 s later, while the host discovery scan lists that same BSS at
 * -34 dBm moments earlier.  Several single-variable guesses have each cost a
 * full flash/boot/photograph cycle and none of them moved the failure,
 * because the submission carries four genuinely independent unknowns at once:
 *
 *   auth   - the NWM disassembly reads authMode as always 1 (NONE_AUTH) with
 *            the host owning WPA, while stock ath6kl sends WPA2_PSK_AUTH.
 *   crypto - the disassembly's "wire value 4" is TKIP_CRYPT in this header's
 *            CRYPTO_TYPE enum, so a CCMP-only AP may simply never match the
 *            profile the target is searching for.
 *   bssid  - a locked BSSID and a wildcard have both been tried, but never
 *            against a known-good setting of the other three.
 *   scan   - the NWM scan policy (0xffff periods, 20 ms dwell, single-channel
 *            table) against this driver's stock scan defaults.
 *
 * One variant per connect attempt turns a single boot into the whole
 * experiment: wpa_supplicant retries roughly every 5 s and gets a dozen or
 * more attempts in, so the association that works names itself in the log.
 * The first variant that associates is latched for the rest of the session so
 * the link stays up.  n3ds_connvar pins one variant for a confirmation run.
 */
/* N3DS_AR6014_CONNECT_CTRL_FLAGS_SWEEP: the auth/crypto/bssid/channel/scan
 * sweep described above ran to completion on hardware and came back a clean
 * negative -- all 8 variants, 3 passes, roughly 36 attempts, zero ASSOCIATED
 * lines, every one ending "NWM disconnect ... reason=1" about 4.5 s after
 * submit.  The capture shows pair=1/0, pair=4/0 and pair=8/0 all going out on
 * the wire, so the TKIP-vs-CCMP reading of the NWM crypto value is settled and
 * wrong: crypto is not the problem, and neither is auth, BSSID, the channel
 * table nor the scan policy.
 *
 * Two observations from the same capture narrow what is left.  The firmware
 * plainly does validate what it is handed -- it rejected WMI_SET_CHANNEL_PARAMS
 * 40 times in a single boot with errorCode=1 (illegal parameter) -- and it
 * never once objected to WMI_CONNECT, so the 52-byte command is well formed
 * and its ID is correct.  And reason=1 is NO_NETWORK_AVAIL, which is the
 * target reporting that *its own* connect-time scan found nothing, for an AP
 * the host scan had logged at -34 dBm moments earlier.
 *
 * ctrl_flags is the one field of that command this driver has never varied:
 * it has been hardcoded to 0 on every attempt ever sent.  Bit 0x0008,
 * CONNECT_PROFILE_MATCH_DONE, tells the target the host has already matched
 * the profile and that it should associate directly instead of running the
 * internal scan that keeps coming up empty.  This table centres on that bit
 * and brackets it with the neighbouring association-policy bits; row 7 holds
 * flags=0 as the control, so one boot still contains the old behaviour to
 * compare against.  The other five axes stay in the struct at the settings
 * that got furthest before, and still vary enough to re-check them for free.
 */
struct n3ds_connect_variant {
    const char *name;
    u8 real_auth;        /* send ar->arAuthMode instead of NONE_AUTH */
    u8 auth_mode;        /* N3DS_AUTH_KEEP = derive as before, else wire value */
    u8 crypto;           /* 0 none, 4 NWM wire value, 8 Linux AES_CRYPT */
    u8 pair_crypto;      /* N3DS_CRYPTO_KEEP = derive as before, else wire value */
    u8 group_crypto;
    u8 real_bssid;       /* lock sme->bssid instead of the wildcard */
    u8 one_channel;      /* restrict the target channel table to the hint */
    u8 stock_scan;       /* stock scan params instead of the NWM policy */
    u16 ctrl_flags;      /* WMI_CONNECT_CMD.ctrl_flags, never before varied */
};

#define N3DS_AUTH_KEEP    0xff
#define N3DS_CRYPTO_KEEP  0xff

/* N3DS_AR6014_NWM_AUTH_SWEEP: #253 proved the geometry -- a pinned BSSID, a
 * single channel and CONNECT_PROFILE_MATCH_DONE are what make NWM associate
 * at all -- so those are held fixed here and no longer swept.  What varies
 * is authMode and the cipher wire values, the one axis the NWM disassembly
 * says this driver still has wrong: it reads authMode as always 1
 * (NONE_AUTH) with the host owning WPA and wire value 4 in both cipher
 * fields with zero lengths, while #256 sent 0x10 (WPA2_PSK_AUTH) with 8/8.
 * If a real authMode hands WPA to the firmware, NWM eats message 1/4 with no
 * PSK to answer it, which is exactly the observed failure.  Row 3 reproduces
 * #256 byte for byte as the control. */
static const struct n3ds_connect_variant n3ds_connect_variants[] = {
    { "dsi-ccmp-tkip", 0, 5, 0, 4, 3, 1, 1, 1, CONNECT_PROFILE_MATCH_DONE },
    { "nwm-auth",      0, N3DS_AUTH_KEEP, 4, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 1, CONNECT_PROFILE_MATCH_DONE },
    { "dsi-none-ccmp", 0, 1, 0, 4, 3, 1, 1, 1, CONNECT_PROFILE_MATCH_DONE },
    { "nwm-open",      0, N3DS_AUTH_KEEP, 0, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 1, CONNECT_PROFILE_MATCH_DONE },
    { "nwm-aes",       0, N3DS_AUTH_KEEP, 8, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 1, CONNECT_PROFILE_MATCH_DONE },
    { "match-done",    1, N3DS_AUTH_KEEP, 8, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 1, CONNECT_PROFILE_MATCH_DONE },
    { "real-nwm",      1, N3DS_AUTH_KEEP, 4, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 1, CONNECT_PROFILE_MATCH_DONE },
    { "nwm-user",      0, N3DS_AUTH_KEEP, 4, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 1,
                        CONNECT_PROFILE_MATCH_DONE | CONNECT_ASSOC_POLICY_USER },
    { "nwm-noflags",   0, N3DS_AUTH_KEEP, 4, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 1, 0 },
    { "nwm-nwmscan",   0, N3DS_AUTH_KEEP, 4, N3DS_CRYPTO_KEEP,
                        N3DS_CRYPTO_KEEP, 1, 1, 0, CONNECT_PROFILE_MATCH_DONE },
};

static int n3ds_connvar = 0;
module_param(n3ds_connvar, int, 0644);
MODULE_PARM_DESC(n3ds_connvar,
    "pin one AR6014 connect variant (-1 sweeps until one associates)");

/* N3DS_AR6014_NWM_PAIRWISE_USAGE: Nintendo's NWM firmware installs its
 * temporal key from 0x001191e8 with WMI_ADD_CIPHER_KEY keyUsage=3
 * (GROUP_USAGE|TX_USAGE), not the KEY_USAGE=0 (pairwise) the older DSi
 * host used.  Keep it tunable (sysfs
 * /sys/module/ath6kl/parameters/n3ds_pairwise_key_usage) in case a
 * different firmware revision wants the legacy 0.
 *
 * 2026-10-04: every one of the seven WMI_ADD_CIPHER_KEY formats swept that
 * day (keyUsage 0 and 3, key_macaddr zeroed and set, 45-byte DSi form and
 * 51-byte NWM form, with and without the dsiwifi WMI_SYNCHRONIZE brackets)
 * came back WMI_CMDERROR cmd=0x0016 errorCode=2.  The key message is not the
 * problem, so this knob stays at NWM's 3 and the sweep is gone.
 *
 * 2026-10-04 (later, key sweep revived): the sweep above was run while the
 * *association was open* -- an open station has no pairwise or group cipher,
 * which is why every format was rejected with "illegal state" and why 04i had
 * to fix the connect command first.  Now that the association is
 * authenticated and the keys are accepted, the question is open again, and it
 * has a strong new candidate: usage=3 is GROUP_USAGE|TX_USAGE, i.e. the PTK
 * is installed as a *group* key.  A target that then cannot decrypt the AP's
 * unicast traffic would lose the BSS exactly as observed -- the link comes up,
 * the keys go in, and 1.5 s later the target reports NO_NETWORK_AVAIL with a
 * zero BSSID (kmsg 2026-10-04 21:0x, disconnect reason=1 at 827 ms after a
 * successful handshake).  So sweep keyUsage over the semantically correct
 * PAIRWISE_USAGE as well, and the DSi's documented 45-byte form with it.
 *
 * The sweep advances once per *association* (see ar6k_cfg80211_connect_event),
 * not per connect() call: wpa_supplicant issues several connects per attempt,
 * which is what made the 04h sweep sit on one row forever.
 */
struct n3ds_key_variant {
    const char *name;
    u8 pairwise_usage;   /* WMI keyUsage for the PTK */
    u8 wire_len;         /* 45 = DSi 2Dh form, 51 = full NWM template */
};

static const struct n3ds_key_variant n3ds_key_variants[] = {
    { "nwm-51-u3",  3, 51 },   /* what the 21:0x boot sent */
    { "dsi-51-u0",  PAIRWISE_USAGE, 51 },
    { "dsi-45-u0",  PAIRWISE_USAGE, 45 },
    { "dsi-51-u1",  GROUP_USAGE, 51 },
};
#define N3DS_KEY_VARIANTS (int)(sizeof(n3ds_key_variants) / sizeof(n3ds_key_variants[0]))

static int n3ds_keyvar = -1;                 /* -1 = sweep */
module_param(n3ds_keyvar, int, 0644);
MODULE_PARM_DESC(n3ds_keyvar,
    "pin one AR6014 WMI_ADD_CIPHER_KEY variant (-1 sweeps one per association)");

static int n3ds_keyvar_current;

static unsigned int n3ds_connect_attempt;
static int n3ds_connect_latched = -1;
static int n3ds_connect_current = -1;

/* N3DS_AR6014_NO_NETWORK_RETRY: the wire tuple of the last WMI_CONNECT, and
 * how many recovery attempts this boot has spent. */
static u8 n3ds_last_auth_mode;
static u8 n3ds_last_pair_crypto;
static u8 n3ds_last_pair_len;
static u8 n3ds_last_group_crypto;
static u8 n3ds_last_group_len;
static unsigned int n3ds_no_network_retries;
#define N3DS_NO_NETWORK_RETRIES_MAX 3

/* N3DS_AR6014_SILENT_REASSOC: how many consecutive post-association drops the
 * driver papers over without telling cfg80211. */
static unsigned int n3ds_silent_reassocs;
#define N3DS_SILENT_REASSOC_MAX 6

/* N3DS_AR6014_REASSOC_GUARD: association state captured before
 * ar6k_cfg80211_connect() overwrites smeState. */
static bool n3ds_was_connected;

/* N3DS_AR6014_NWM_AUTH_SWEEP: which variant produced the most recent
 * association.  It is latched later, and only if that association went on to
 * carry a handshake frame.  n3ds_eapol_rx_count is zeroed by
 * ar6000_connect_event() on every association, so a non-zero read at the next
 * connect attempt belongs to the association this variable names. */
static int n3ds_connect_assoc_variant = -1;

/* N3DS_AR6014_ASSOC_IE_FIXUP: NWM's WMI_CONNECT_EVENT reports assocReqLen = 0,
 * so the association-request IEs the supplicant needs back never arrive with
 * the event.  Keep the blob cfg80211 gave us in connect() -- it is the same
 * RSN IE we forwarded to the target as WMI_FRAME_ASSOC_REQ, which is what the
 * AP actually saw -- and report that instead.  Without it wpa_supplicant
 * clears sm->assoc_wpa_ie and can never build message 2/4. */
static u8 n3ds_assoc_req_ie[256];
static u16 n3ds_assoc_req_ie_len;

/* numChannels 0 hands the channel list back to the target's own phy-mode
 * default, undoing a single-channel table left behind by an earlier variant. */
static int n3ds_ar6014_set_all_channels(struct ar6_softc *ar)
{
    int status = wmi_set_channelParams_cmd(ar->arWmi, 0, WMI_11G_MODE,
                                           0, NULL);

    if (status != 0)
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: full channel table failed status=%d\n", status));
    return status;
}

static int
ar6k_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
                      struct cfg80211_connect_params *sme)
{
    struct ar6_softc *ar = ar6k_priv(dev);
    const struct n3ds_connect_variant *cv;
    int status;
    CRYPTO_TYPE nwm_pairwise_crypto = NONE_CRYPT;
    CRYPTO_TYPE nwm_group_crypto = NONE_CRYPT;
    u8 nwm_pairwise_crypto_len = 0;
    u8 nwm_group_crypto_len = 0;
    AUTH_MODE nwm_auth_mode;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    /* N3DS_AR6014_REASSOC_GUARD: cfg80211/the WEXT supplicant routinely
     * issues a second connect for the SSID that has *just* associated
     * (ESSID + BSSID + WPA IE are three separate wext operations).  The
     * connect event sets SME_CONNECTED before ar6000_connect_event() gets
     * to ar->arConnected = true, so for that window the old "same SSID but
     * not connected" branch below fired ar6000_disconnect() -- an explicit
     * WMI_DISCONNECT (reason 3) -- and killed the association before
     * wpa_supplicant could run the 4-way handshake.  Remember the state
     * before smeState is overwritten. */
    n3ds_was_connected = (ar->arConnected == true) ||
                         (ar->smeState == SME_CONNECTED);
    ar->smeState = SME_CONNECTING;

    /* N3DS_AR6014_NWM_AUTH_SWEEP: latch the previous variant only if the
     * association it produced actually carried EAPOL.  Associating is not
     * winning: the connected-but-silent rows are the ones that hand WPA to
     * the firmware, which has no PSK. */
    if (n3ds_connect_latched < 0 && n3ds_connect_assoc_variant >= 0 &&
        atomic_read(&n3ds_eapol_rx_count) > 0) {
        n3ds_connect_latched = n3ds_connect_assoc_variant;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: VARIANT %d %s CARRIED EAPOL rx=%d, latching\n",
             n3ds_connect_latched,
             n3ds_connect_variants[n3ds_connect_latched].name,
             atomic_read(&n3ds_eapol_rx_count)));
    }

    if (n3ds_connvar >= 0 &&
        n3ds_connvar < (int)ARRAY_SIZE(n3ds_connect_variants))
        n3ds_connect_current = n3ds_connvar;
    else if (n3ds_connect_latched >= 0)
        n3ds_connect_current = n3ds_connect_latched;
    else
        n3ds_connect_current =
            (int)(n3ds_connect_attempt % ARRAY_SIZE(n3ds_connect_variants));
    n3ds_connect_attempt++;
    cv = &n3ds_connect_variants[n3ds_connect_current];

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready yet\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(ar->bIsDestroyProgress) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: destroy in progress\n", __func__));
        return -EBUSY;
    }

    if(!sme->ssid_len || IEEE80211_MAX_SSID_LEN < sme->ssid_len) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: ssid invalid\n", __func__));
        return -EINVAL;
    }

    if(ar->arSkipScan == true &&
       ((sme->channel && sme->channel->center_freq == 0) ||
        (sme->bssid && !sme->bssid[0] && !sme->bssid[1] && !sme->bssid[2] &&
         !sme->bssid[3] && !sme->bssid[4] && !sme->bssid[5])))
    {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s:SkipScan: channel or bssid invalid\n", __func__));
        return -EINVAL;
    }

    if(down_interruptible(&ar->arSem)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: busy, couldn't get access\n", __func__));
        return -ERESTARTSYS;
    }

    if(ar->bIsDestroyProgress) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: busy, destroy in progress\n", __func__));
        up(&ar->arSem);
        return -EBUSY;
    }

    if(ar->arTxPending[wmi_get_control_ep(ar->arWmi)]) {
        /*
        * sleep until the command queue drains
        */
        wait_event_interruptible_timeout(arEvent,
        ar->arTxPending[wmi_get_control_ep(ar->arWmi)] == 0, wmitimeout * HZ);
        if (signal_pending(current)) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: cmd queue drain timeout\n", __func__));
            up(&ar->arSem);
            return -EINTR;
        }
    }

    if(n3ds_was_connected == true &&
       ar->arSsidLen == sme->ssid_len &&
       !memcmp(ar->arSsid, sme->ssid, ar->arSsidLen)) {
        reconnect_flag = true;
        status = wmi_reconnect_cmd(ar->arWmi,
                                   ar->arReqBssid,
                                   ar->arChannelHint);

        up(&ar->arSem);
        if (status) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: wmi_reconnect_cmd failed\n", __func__));
            return -EIO;
        }
        return 0;
    }
    /* N3DS_AR6014_REASSOC_GUARD: do NOT ar6000_disconnect() here.  The CM7
     * wext supplicant issues three connects per association attempt (set
     * SSID, set BSSID, set key); the second one used to hit this branch while
     * the first was still SME_CONNECTING, send WMI_DISCONNECT (reason 3), and
     * emit an "AP == 00:00:00:00:00:00" wireless event that made
     * wpa_supplicant blacklist the BSS and re-scan.  The following
     * WMI_CONNECT supersedes whatever the target was doing anyway. */

    A_MEMZERO(ar->arSsid, sizeof(ar->arSsid));
    ar->arSsidLen = sme->ssid_len;
    memcpy(ar->arSsid, sme->ssid, sme->ssid_len);

    if(sme->channel){
        ar->arChannelHint = sme->channel->center_freq;
    }

    /* N3DS_AR6014_MESH_BSSID_WILDCARD: physical capture against a two-radio
     * same-SSID mesh AP shows cfg80211's chosen sme->bssid alternates between
     * the mesh's two real BSSIDs from one connect attempt to the next, and
     * NWM's own connect-time internal re-scan (distinct from the host
     * discovery scan that already sees both BSSes via ALL_BSS_FILTER above)
     * reports NO_NETWORK_AVAIL against whichever one was requested, every
     * single time. NWM firmware predates mesh/band-steering gear entirely,
     * so its connect-time BSSID lock was never exercised against a topology
     * where two live radios answer one SSID. Leave arReqBssid wildcard
     * (all-zero) and let NWM's own re-scan settle on whichever BSS answers
     * the requested SSID, exactly as the ALL_BSS_FILTER discovery scan
     * already does.  The sweep still tries a locked BSSID, because that
     * reading was never tested against a known-good auth/crypto tuple. */
    A_MEMZERO(ar->arReqBssid, sizeof(ar->arReqBssid));
    if (cv->real_bssid && sme->bssid)
        memcpy(ar->arReqBssid, sme->bssid, ATH_MAC_LEN);

    ar6k_set_wpa_version(ar, sme->crypto.wpa_versions);
    ar6k_set_auth_type(ar, sme->auth_type);

    if(sme->crypto.n_ciphers_pairwise) {
        ar6k_set_cipher(ar, sme->crypto.ciphers_pairwise[0], true);
    } else {
        ar6k_set_cipher(ar, IW_AUTH_CIPHER_NONE, true);
    }
    ar6k_set_cipher(ar, sme->crypto.cipher_group, false);

    if(sme->crypto.n_akm_suites) {
        ar6k_set_key_mgmt(ar, sme->crypto.akm_suites[0]);
    }

    if((sme->key_len) &&
       (NONE_AUTH == ar->arAuthMode) &&
        (WEP_CRYPT == ar->arPairwiseCrypto)) {
        struct ar_key *key = NULL;

        if(sme->key_idx < WMI_MIN_KEY_INDEX || sme->key_idx > WMI_MAX_KEY_INDEX) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                            ("%s: key index %d out of bounds\n", __func__, sme->key_idx));
            up(&ar->arSem);
            return -ENOENT;
        }

        key = &ar->keys[sme->key_idx];
        key->key_len = sme->key_len;
        memcpy(key->key, sme->key, key->key_len);
        key->cipher = ar->arPairwiseCrypto;
        ar->arDefTxKeyIndex = sme->key_idx;

        wmi_addKey_cmd(ar->arWmi, sme->key_idx,
                    ar->arPairwiseCrypto,
                    GROUP_USAGE | TX_USAGE,
                    key->key_len,
                    NULL,
                    key->key, KEY_OP_INIT_VAL, NULL,
                    NO_SYNC_WMIFLAG);
    }

    ar->arNetworkType = ar->arNextMode;

    /* N3DS_AR6014_NWM_HOST_WPA:
     * N3DS_AR6014_NWM_LIVE_PROTECTED_TUPLE: 0x00119f54's accepted 16-byte
     * protected-key path writes wire value 4 to live pairwise state +0x218
     * and group state +0x21a, leaves both length bytes zero, and always keeps
     * host-managed auth +0x217 at 1. 0x00131d40 forwards those live fields
     * verbatim into 0x00118818's 52-byte WMI_CONNECT command. Translate
     * Linux CCMP/AES 8 to NWM wire value 4 here as well as in ADD_CIPHER_KEY.
     * Open profiles retain NONE/1 and static WEP retains type 2 plus lengths. */
    if (cv->crypto == 8) {
        nwm_pairwise_crypto = (CRYPTO_TYPE)ar->arPairwiseCrypto;
        nwm_group_crypto = (CRYPTO_TYPE)ar->arGroupCrypto;
        nwm_pairwise_crypto_len = ar->arPairwiseCryptoLen;
        nwm_group_crypto_len = ar->arGroupCryptoLen;
    } else if (cv->crypto == 4) {
        if (ar->arPairwiseCrypto == AES_CRYPT &&
            ar->arGroupCrypto == AES_CRYPT) {
            nwm_pairwise_crypto = (CRYPTO_TYPE)4;
            nwm_group_crypto = (CRYPTO_TYPE)4;
        } else if (ar->arPairwiseCrypto == WEP_CRYPT &&
                   ar->arGroupCrypto == WEP_CRYPT) {
            nwm_pairwise_crypto = WEP_CRYPT;
            nwm_group_crypto = WEP_CRYPT;
            nwm_pairwise_crypto_len = ar->arPairwiseCryptoLen;
            nwm_group_crypto_len = ar->arGroupCryptoLen;
        }
    }
    /* cv->crypto == 0 leaves NONE/NONE: a bare open association with every
     * cipher left to the host supplicant. */

    /* N3DS_AR6014_DSI_CONNECT_TUPLE: rows that name the ciphers (and the auth
     * mode) outright.  This is the hole the old derivation fell into: with a
     * CCMP-pairwise/TKIP-group network neither `crypto == 4` branch matched,
     * so the command went out as an *open* association with NONE in both
     * cipher fields, and the target then refused every WMI_ADD_CIPHER_KEY
     * with errorCode=2 (illegal state) -- an open station has no pairwise or
     * group cipher to install.  Wire values are the AR6014's own numbering
     * (NONE 1, WEP 2, TKIP 3, AES 4), lengths stay zero except for WEP. */
    if (cv->pair_crypto != N3DS_CRYPTO_KEEP ||
        cv->group_crypto != N3DS_CRYPTO_KEEP) {
        nwm_pairwise_crypto = (CRYPTO_TYPE)cv->pair_crypto;
        nwm_group_crypto = (CRYPTO_TYPE)cv->group_crypto;
        if (nwm_pairwise_crypto == WEP_CRYPT)
            nwm_pairwise_crypto_len = ar->arPairwiseCryptoLen;
        if (nwm_group_crypto == WEP_CRYPT)
            nwm_group_crypto_len = ar->arGroupCryptoLen;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: NWM tuple from AP ciphers: pair=%u/%u "
             "group=%u/%u\n", nwm_pairwise_crypto, nwm_pairwise_crypto_len,
             nwm_group_crypto, nwm_group_crypto_len));
    }

    /* dsiwifi's working WMI_CONNECT carries the DSi-numbered WPA2_PSK_AUTH
     * (5); this header's AUTH_MODE enum calls that 0x10. */
    if (cv->auth_mode != N3DS_AUTH_KEEP)
        nwm_auth_mode = (AUTH_MODE)cv->auth_mode;
    else
        nwm_auth_mode = cv->real_auth ? (AUTH_MODE)ar->arAuthMode : NONE_AUTH;

    if (sme->ie_len > 0xffff) {
        up(&ar->arSem);
        return -EINVAL;
    }
    /* N3DS_AR6014_ASSOC_IE_FIXUP: stash before the command, so the copy exists
     * even if the target rejects the IE. */
    n3ds_assoc_req_ie_len = 0;
    if (sme->ie != NULL && sme->ie_len > 0) {
        n3ds_assoc_req_ie_len = (u16)min_t(size_t, sme->ie_len,
                                           sizeof(n3ds_assoc_req_ie));
        memcpy(n3ds_assoc_req_ie, sme->ie, n3ds_assoc_req_ie_len);
    }

    status = wmi_set_appie_cmd(ar->arWmi, WMI_FRAME_ASSOC_REQ,
                               (u16)sme->ie_len, (u8 *)sme->ie);
    if (status != 0) {
        up(&ar->arSem);
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("AR6002 connect: NWM APP-IE failed: %d\n", status));
        return -EIO;
    }
    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
        ("AR6002 connect: NWM APP-IE len=%u host-WPA tuple "
         "auth=%u pair=%u/%u group=%u/%u\n", (unsigned int)sme->ie_len,
         (unsigned int)nwm_auth_mode, nwm_pairwise_crypto,
         nwm_pairwise_crypto_len,
         nwm_group_crypto, nwm_group_crypto_len));

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: Connect called with authmode %d dot11 auth %d"\
                    " PW crypto %d PW crypto Len %d GRP crypto %d"\
                    " GRP crypto Len %d channel hint %u\n",
                    __func__, ar->arAuthMode, ar->arDot11AuthMode,
                    ar->arPairwiseCrypto, ar->arPairwiseCryptoLen,
                    ar->arGroupCrypto, ar->arGroupCryptoLen, ar->arChannelHint));

    reconnect_flag = 0;
    /* NWM 0x0012ad70 setup adapted to cfg80211's preselected BSS path:
     * selected slot 0 -> command 8 -> command 17 -> command 9 -> CONNECT.
     * CONNECT_SCAN makes WMI_CONNECT perform its target-side search; command
     * 7 remains exclusively the host discovery operation above. */
    status = wmi_probedSsid_cmd(ar->arWmi, 0, SPECIFIC_SSID_FLAG,
                                ar->arSsidLen, ar->arSsid);
    if (status != 0) {
        up(&ar->arSem);
        return -EIO;
    }
    if (cv->stock_scan)
        status = wmi_scanparams_cmd(ar->arWmi, 0, 0, 0, 0, 0, 0,
                                    WMI_SHORTSCANRATIO_DEFAULT,
                                    DEFAULT_SCAN_CTRL_FLAGS, 0,
                                    N3DS_NWM_SCAN_PROBES_PER_SSID);
    else
        status = wmi_scanparams_cmd(ar->arWmi, 0xffff, 0xffff, 0xffff,
                                    N3DS_NWM_CONNECT_SCAN_DWELL_MS,
                                    N3DS_NWM_CONNECT_SCAN_DWELL_MS,
                                    N3DS_NWM_CONNECT_SCAN_DWELL_MS,
                                    0, N3DS_NWM_CONNECT_SCAN_FLAGS, 0,
                                    N3DS_NWM_SCAN_PROBES_PER_SSID);
    if (status != 0) {
        wmi_probedSsid_cmd(ar->arWmi, 0, DISABLE_SSID_FLAG, 0, NULL);
        up(&ar->arSem);
        return -EIO;
    }
    if (cv->one_channel && n3ds_ar6014_channel_is_11g(ar->arChannelHint))
        status = n3ds_ar6014_set_search_channel(ar, ar->arChannelHint);
    else
        status = n3ds_ar6014_set_all_channels(ar);
    if (status != 0) {
        wmi_probedSsid_cmd(ar->arWmi, 0, DISABLE_SSID_FLAG, 0, NULL);
        up(&ar->arSem);
        return -EIO;
    }
    if (!ar->arUserBssFilter &&
        wmi_bssfilter_cmd(ar->arWmi, ALL_BSS_FILTER, 0) != 0) {
        wmi_probedSsid_cmd(ar->arWmi, 0, DISABLE_SSID_FLAG, 0, NULL);
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: Couldn't set bss filtering\n", __func__));
        up(&ar->arSem);
        return -EIO;
    }
    /* N3DS_AR6014_CONNECT_CTRL_FLAGS_SWEEP: ctrl_flags is swept now, not
     * pinned to 0 as the earlier NWM reading assumed.  Print every field of
     * the 52-byte WMI_CONNECT_CMD this driver chooses, so one capture pins the
     * whole submission instead of the half the old log covered. */
    ar->arConnectCtrlFlags = cv->ctrl_flags;

    /* N3DS_AR6014_NO_NETWORK_RETRY: remember the wire tuple actually
     * submitted, so the recovery connect after NO_NETWORK_AVAIL repeats it
     * exactly and changes nothing but the search geometry. */
    n3ds_last_auth_mode = nwm_auth_mode;
    n3ds_last_pair_crypto = nwm_pairwise_crypto;
    n3ds_last_pair_len = nwm_pairwise_crypto_len;
    n3ds_last_group_crypto = nwm_group_crypto;
    n3ds_last_group_len = nwm_group_crypto_len;
    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
        ("AR6002 connect: VARIANT %d %s auth=%u pair=%u/%u group=%u/%u "
         "bssid=%pM chan=%u onechan=%u stockscan=%u "
         "flags=0x%04x nettype=%u dot11auth=%u ssidlen=%u\n",
         n3ds_connect_current, cv->name,
         (unsigned int)nwm_auth_mode,
         nwm_pairwise_crypto, nwm_pairwise_crypto_len,
         nwm_group_crypto, nwm_group_crypto_len,
         ar->arReqBssid, ar->arChannelHint,
         cv->one_channel, cv->stock_scan,
         ar->arConnectCtrlFlags, ar->arNetworkType, ar->arDot11AuthMode,
         ar->arSsidLen));
    status = wmi_connect_cmd(ar->arWmi, ar->arNetworkType,
                            ar->arDot11AuthMode,
                            nwm_auth_mode,
                            nwm_pairwise_crypto, nwm_pairwise_crypto_len,
                            nwm_group_crypto, nwm_group_crypto_len,
                            ar->arSsidLen, ar->arSsid,
                            ar->arReqBssid, ar->arChannelHint,
                            ar->arConnectCtrlFlags);
    if (status != 0)
        wmi_probedSsid_cmd(ar->arWmi, 0, DISABLE_SSID_FLAG, 0, NULL);

    up(&ar->arSem);

    if (A_EINVAL == status) {
        A_MEMZERO(ar->arSsid, sizeof(ar->arSsid));
        ar->arSsidLen = 0;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Invalid request\n", __func__));
        return -ENOENT;
    } else if (status) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: wmi_connect_cmd failed\n", __func__));
        return -EIO;
    }

    if ((!(ar->arConnectCtrlFlags & CONNECT_DO_WPA_OFFLOAD)) &&
        ((WPA_PSK_AUTH == ar->arAuthMode) || (WPA2_PSK_AUTH == ar->arAuthMode)))
    {
        A_TIMEOUT_MS(&ar->disconnect_timer, A_DISCONNECT_TIMER_INTERVAL, 0);
    }

    ar->arConnectCtrlFlags &= ~CONNECT_DO_WPA_OFFLOAD;
    ar->arConnectPending = true;

    return 0;
}

void
ar6k_cfg80211_connect_event(struct ar6_softc *ar, u16 channel,
                u8 *bssid, u16 listenInterval,
                u16 beaconInterval,NETWORK_TYPE networkType,
                u8 beaconIeLen, u8 assocReqLen,
                u8 assocRespLen, u8 *assocInfo)
{
    u16 size = 0;
    u16 capability = 0;
    struct cfg80211_bss *bss = NULL;
    struct ieee80211_mgmt *mgmt = NULL;
    struct ieee80211_channel *ibss_channel = NULL;
    s32 signal = 50 * 100;
    u16 ie_buf_len = 0;
    unsigned char ie_buf[512];
    unsigned char *ptr_ie_buf = ie_buf;
    unsigned char *ieeemgmtbuf = NULL;
    u8 source_mac[ATH_MAC_LEN];

    u8 assocReqIeOffset = sizeof(u16)  +  /* capinfo*/
                               sizeof(u16);    /* listen interval */
    u8 assocRespIeOffset = sizeof(u16) +  /* capinfo*/
                                sizeof(u16) +  /* status Code */
                                sizeof(u16);   /* associd */
    u8 *assocReqIe = assocInfo + beaconIeLen + assocReqIeOffset;
    u8 *assocRespIe = assocInfo + beaconIeLen + assocReqLen + assocRespIeOffset;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    /* N3DS_AR6014_ASSOC_IE_FIXUP: log the target's own numbers before they are
     * touched.  beaconInterval reads back the AP's beacon period (~100 TU) on a
     * real association and 0 on a synthetic one, which is the cheapest test of
     * whether NWM actually joined the BSS. */
    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
        ("AR6002 connect: nwm evt chan=%u nettype=%u listen=%u beacon=%u "
         "beaconIeLen=%u rawReqLen=%u rawRespLen=%u\n",
         channel, (unsigned int)networkType, listenInterval, beaconInterval,
         beaconIeLen, assocReqLen, assocRespLen));

    /* N3DS_AR6014_ASSOC_IE_FIXUP: stock ath6kl subtracts the fixed-field
     * offsets unconditionally.  NWM reports both lengths as 0, so both u8
     * fields wrapped to 252 and 250 and cfg80211 was handed 502 bytes read off
     * the end of the event buffer.  Clamp, and report no IEs rather than
     * garbage ones. */
    if (assocReqLen >= assocReqIeOffset) {
        assocReqLen -= assocReqIeOffset;
    } else {
        assocReqLen = 0;
        assocReqIe = NULL;
    }
    if (assocRespLen >= assocRespIeOffset) {
        assocRespLen -= assocRespIeOffset;
    } else {
        assocRespLen = 0;
        assocRespIe = NULL;
    }
    if (assocReqIe == NULL && n3ds_assoc_req_ie_len > 0) {
        assocReqIe = n3ds_assoc_req_ie;
        assocReqLen = (u8)min_t(u16, n3ds_assoc_req_ie_len, 255);
    }

    ar->arAutoAuthStage = AUTH_IDLE;

    /* N3DS_AR6014_NWM_AUTH_SWEEP: associating is not winning.  Record the
     * variant and let the next connect attempt decide, once it can see
     * whether this association carried a handshake.  prev_eapol_rx is the
     * count still standing from the *previous* association, because
     * ar6000_connect_event() zeroes the counter just after this returns. */
    if (n3ds_connect_current >= 0) {
        n3ds_connect_assoc_variant = n3ds_connect_current;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: VARIANT %d %s ASSOCIATED channel=%u bssid=%pM "
             "prev_eapol_rx=%d\n",
             n3ds_connect_current,
             n3ds_connect_variants[n3ds_connect_current].name,
             channel, bssid, atomic_read(&n3ds_eapol_rx_count)));
    }

    /* N3DS_AR6014_NO_NETWORK_RETRY: an association is proof the geometry
     * worked, so give the next independent connect a fresh retry budget. */
    n3ds_no_network_retries = 0;

    /* N3DS_AR6014_SILENT_REASSOC: a completed association earns a fresh
     * silent-recovery budget. */
    n3ds_silent_reassocs = 0;

    /* N3DS_AR6014_KEY_VARIANT: advance the key-format row once per
     * association.  One boot therefore walks the whole table, because the
     * target tears the link down after each attempt and wpa_supplicant
     * re-associates. */
    if (n3ds_keyvar < 0) {
        n3ds_keyvar_current = (n3ds_keyvar_current + 1) % N3DS_KEY_VARIANTS;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 key: next association uses variant %d \"%s\" "
             "(usage=%u wire=%u)\n",
             n3ds_keyvar_current,
             n3ds_key_variants[n3ds_keyvar_current].name,
             n3ds_key_variants[n3ds_keyvar_current].pairwise_usage,
             n3ds_key_variants[n3ds_keyvar_current].wire_len));
    }

    if((ADHOC_NETWORK & networkType)) {
        if(NL80211_IFTYPE_ADHOC != ar->wdev->iftype) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                ("AR6002 connect: dropped, iftype %u is not ADHOC\n",
                 ar->wdev->iftype));
            return;
        }
    }

    if((INFRA_NETWORK & networkType)) {
        if(NL80211_IFTYPE_STATION != ar->wdev->iftype) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                ("AR6002 connect: dropped, iftype %u is not STATION\n",
                 ar->wdev->iftype));
            return;
        }
    }

    /* N3DS_CFG80211_CONNECT_BSS: the bss entry is built and inserted by
     * cfg80211_inform_bss_frame() below, and the exact pointer it returns is
     * handed to cfg80211 in the connect/roam notification.  The old
     * cfg80211_get_bss() probe here was overwritten immediately and only
     * leaked its reference. */
    /*
     * Earlier we were updating the cfg about bss by making a beacon frame
     * only if the entry for bss is not there. This can have some issue if
     * ROAM event is generated and a heavy traffic is ongoing. The ROAM
     * event is handled through a work queue and by the time it really gets
     * handled, BSS would have been aged out. So it is better to update the
     * cfg about BSS irrespective of its entry being present right now or
     * not.
     */

    if (ADHOC_NETWORK & networkType) {
            /* construct 802.11 mgmt beacon */
            if(ptr_ie_buf) {
		    *ptr_ie_buf++ = WLAN_EID_SSID;
		    *ptr_ie_buf++ = ar->arSsidLen;
		    memcpy(ptr_ie_buf, ar->arSsid, ar->arSsidLen);
		    ptr_ie_buf +=ar->arSsidLen;

		    *ptr_ie_buf++ = WLAN_EID_IBSS_PARAMS;
		    *ptr_ie_buf++ = 2; /* length */
		    *ptr_ie_buf++ = 0; /* ATIM window */
		    *ptr_ie_buf++ = 0; /* ATIM window */

		    /* TODO: update ibss params and include supported rates,
		     * DS param set, extened support rates, wmm. */

		    ie_buf_len = ptr_ie_buf - ie_buf;
            }

            capability |= IEEE80211_CAPINFO_IBSS;
            if(WEP_CRYPT == ar->arPairwiseCrypto) {
		    capability |= IEEE80211_CAPINFO_PRIVACY;
            }
            memcpy(source_mac, ar->arNetDev->dev_addr, ATH_MAC_LEN);
            ptr_ie_buf = ie_buf;
    } else {
            capability = *(u16 *)(&assocInfo[beaconIeLen]);
            memcpy(source_mac, bssid, ATH_MAC_LEN);
            /* N3DS_AR6014_CONNECT_BSS_BEACON_IES: what is being built here is
             * a *beacon*, and cfg80211 looks the resulting BSS back up by
             * SSID in __cfg80211_connect_result().  Stock ath6kl fills it
             * with the association-request IEs, which after the assoc-IE
             * fixup is the host's own RSN blob -- no SSID element at all.
             * cfg80211_get_bss() then missed, cr->bss came back NULL, and
             * net/wireless/sme.c:757 WARNed six times in #256, taking
             * wdev->current_bss and the pending-key upload down with it.
             * NWM supplies the AP's real beacon IEs in the first beaconIeLen
             * bytes of assocInfo; prefer those, and keep the old behaviour
             * only for a target that sent none. */
            if (assocInfo != NULL && beaconIeLen > 0) {
                ptr_ie_buf = assocInfo;
                ie_buf_len = beaconIeLen;
                AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                    ("AR6002 connect: bss ie src=beacon len=%u\n",
                     ie_buf_len));
            } else {
                ptr_ie_buf = assocReqIe;
                ie_buf_len = assocReqLen;
                AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                    ("AR6002 connect: bss ie src=assocreq len=%u\n",
                     ie_buf_len));
            }

            /* N3DS_AR6014_CONNECT_BSS_SSID: cfg80211's wext-compat
             * SIOCGIWESSID (net/wireless/wext-sme.c) reads the SSID element
             * out of wdev->current_bss.  NWM's connect-event blob does not
             * always carry it, so the CM7 WEXT supplicant got a zero-length
             * SSID back, could not match its configured network and aborted
             * the 4-way handshake with
             * "WPA: No SSID info found (msg 1 of 4)."
             * The SSID is known from the connect request, so prepend the
             * element when the blob lacks it. */
            if (ar->arSsidLen > 0 && ar->arSsidLen <= 32 &&
                (ie_buf_len < 2 || ptr_ie_buf[0] != WLAN_EID_SSID) &&
                (u16)(2 + ar->arSsidLen) + ie_buf_len <= sizeof(ie_buf)) {
                memmove(ie_buf + 2 + ar->arSsidLen, ptr_ie_buf, ie_buf_len);
                ie_buf[0] = WLAN_EID_SSID;
                ie_buf[1] = (u8)ar->arSsidLen;
                memcpy(ie_buf + 2, ar->arSsid, ar->arSsidLen);
                ptr_ie_buf = ie_buf;
                ie_buf_len = 2 + ar->arSsidLen + ie_buf_len;
                AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                    ("AR6002 connect: prepended SSID ie len=%u total=%u\n",
                     (unsigned int)ar->arSsidLen,
                     (unsigned int)ie_buf_len));
            } else if (ar->arSsidLen > 0 && ie_buf_len >= 2 &&
                       ptr_ie_buf[0] == WLAN_EID_SSID) {
                AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                    ("AR6002 connect: bss ie already has SSID len=%u\n",
                     ptr_ie_buf[1]));
            }
    }

    size = offsetof(struct ieee80211_mgmt, u)
	    + sizeof(mgmt->u.beacon)
	    + ie_buf_len;

    ieeemgmtbuf = A_MALLOC_NOWAIT(size);
    if(!ieeemgmtbuf) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                            ("%s: ieeeMgmtbuf alloc error\n", __func__));
	    cfg80211_put_bss(ar->wdev->wiphy, bss);
            return;
    }

    A_MEMZERO(ieeemgmtbuf, size);
    mgmt = (struct ieee80211_mgmt *)ieeemgmtbuf;
    mgmt->frame_control = (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON);
    memcpy(mgmt->da, bcast_mac, ATH_MAC_LEN);
    memcpy(mgmt->sa, source_mac, ATH_MAC_LEN);
    memcpy(mgmt->bssid, bssid, ATH_MAC_LEN);
    mgmt->u.beacon.beacon_int = beaconInterval;
    mgmt->u.beacon.capab_info = capability;
    memcpy(mgmt->u.beacon.variable, ptr_ie_buf, ie_buf_len);

    ibss_channel = ieee80211_get_channel(ar->wdev->wiphy, (int)channel);
    /* N3DS_CFG80211_BSS_CHANNEL_GUARD: do not let an invalid
     * firmware frequency reach cfg80211's BSS lookup. */
    if (!ibss_channel) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: dropping BSS with invalid channel %u\n",
                         __func__, channel));
        kfree(ieeemgmtbuf);
        return;
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
		    ("%s: inform bss with bssid %pM channel %d beaconInterval %d "
		     "capability 0x%x\n", __func__, mgmt->bssid,
		     ibss_channel->hw_value, beaconInterval, capability));

    bss = cfg80211_inform_bss_frame(ar->wdev->wiphy,
				    ibss_channel, mgmt,
				    le16_to_cpu(size),
				    signal, GFP_KERNEL);
    kfree(ieeemgmtbuf);

    if((ADHOC_NETWORK & networkType)) {
        cfg80211_ibss_joined(ar->arNetDev, bssid, ibss_channel, GFP_KERNEL);
        cfg80211_put_bss(ar->wdev->wiphy, bss);
        return;
    }

    if (false == ar->arConnected) {
        /* N3DS_AR6014_DISCONNECT_NOTIFY: cfg80211 has to be left holding
         * SME_CONNECTED, because ar6k_cfg80211_disconnect_event() keys its
         * notification off this field.  Writing SME_DISCONNECTED here (what
         * this fork did) meant a successful association permanently disarmed
         * the disconnect path, wdev->connected was never cleared again, and
         * every later cfg80211_connect() short-circuited with -EALREADY
         * before this driver was reached. */
        ar->smeState = SME_CONNECTED;
        /* N3DS_CFG80211_CONNECT_BSS: NWM's connect event reports both IE
         * lengths as 0 and the beacon blob it does carry does not always
         * make cfg80211_get_bss() match wdev->ssid.  cfg80211_connect_result()
         * then looked the BSS up on its own, missed, hit
         * WARN_ON(!cr->bss) at net/wireless/sme.c:757 and returned *before*
         * setting wdev->current_bss/connected.  The link worked for the
         * nl80211 supplicant (nl80211_send_connect_result() runs first) but
         * the CM7 WEXT supplicant re-reads SIOCGIWAP, got 00:00:00:00:00:00
         * and tore the association down.  cfg80211_connect_bss() lets the
         * driver name the exact BSS entry it just informed, so the successful
         * path always completes.  It consumes the reference. */
        cfg80211_connect_bss(ar->arNetDev, bssid, bss,
                             assocReqIe, assocReqLen,
                             assocRespIe, assocRespLen,
                             WLAN_STATUS_SUCCESS, GFP_KERNEL,
                             NL80211_TIMEOUT_UNSPECIFIED);
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: connect_result SUCCESS bssid=%pM reqIe=%u "
             "respIe=%u bss=%s\n", bssid, assocReqLen, assocRespLen,
             bss ? "exact" : "lookup"));
    } else {
        /* inform roam event to cfg80211 */
        struct cfg80211_roam_info roam_info = {
            .channel = ibss_channel,
            .bss = bss,
            .bssid = bssid,
            .req_ie = assocReqIe,
            .req_ie_len = assocReqLen,
            .resp_ie = assocRespIe,
            .resp_ie_len = assocRespLen,
        };
        cfg80211_roamed(ar->arNetDev, &roam_info, GFP_KERNEL);
        ar->smeState = SME_CONNECTED;
    }
}

static int
ar6k_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *dev,
                        u16 reason_code)
{
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(dev);

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: reason=%u\n", __func__, reason_code));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(ar->bIsDestroyProgress) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: busy, destroy in progress\n", __func__));
        return -EBUSY;
    }

    if(down_interruptible(&ar->arSem)) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: busy, couldn't get access\n", __func__));
        return -ERESTARTSYS;
    }

    reconnect_flag = 0;
    ar6000_disconnect(ar);
    A_MEMZERO(ar->arSsid, sizeof(ar->arSsid));
    ar->arSsidLen = 0;

    if (ar->arSkipScan == false) {
        A_MEMZERO(ar->arReqBssid, sizeof(ar->arReqBssid));
    }

    up(&ar->arSem);

    return 0;
}

void
ar6k_cfg80211_disconnect_event(struct ar6_softc *ar, u8 reason,
                               u8 *bssid, u8 assocRespLen,
                               u8 *assocInfo, u16 protocolReasonStatus)
{

    u16 status;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: reason=%u\n", __func__, reason));

    if (ar->scan_request) {
	struct cfg80211_scan_request *request;
	struct cfg80211_scan_info scan_info = { .aborted = true };
	request = xchg(&ar->scan_request, NULL);
	if (request)
		cfg80211_scan_done(request, &scan_info);
    }
    if((ADHOC_NETWORK & ar->arNetworkType)) {
        if(NL80211_IFTYPE_ADHOC != ar->wdev->iftype) {
            AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                            ("%s: ath6k not in ibss mode\n", __func__));
            return;
        }
        A_MEMZERO(bssid, ETH_ALEN);
        cfg80211_ibss_joined(ar->arNetDev, bssid, NULL, GFP_KERNEL);
        return;
    }

    if((INFRA_NETWORK & ar->arNetworkType)) {
        if(NL80211_IFTYPE_STATION != ar->wdev->iftype) {
            AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                            ("%s: ath6k not in station mode\n", __func__));
            return;
        }
    }

    /* N3DS_AR6014_DISCONNECT_NOTIFY: notification is driven by smeState the
     * way mainline ath6kl does it, not by arConnectPending.
     *
     * ar6000_connect_event() clears arConnectPending as soon as the target
     * associates, and the old gate here was `if (true == ar->arConnectPending)`.
     * So after a successful association no disconnect ever reached cfg80211,
     * wdev->connected stayed set forever, and every later cfg80211_connect()
     * failed with -EALREADY before this driver was called at all -- 22 of 22
     * supplicant attempts in the 2026-09-01 capture.  Only
     * cfg80211_disconnected() clears wdev->connected; rdev_disconnect() does
     * not.  The old gate also had no branch for LOST_LINK, AUTH_FAILED,
     * ASSOC_FAILED or BSS_DISCONNECTED, so those were swallowed even while a
     * connect was pending. */

    /* N3DS_AR6014_NO_NETWORK_RETRY: NO_NETWORK_AVAIL is the target reporting
     * that its own search for this profile came up empty.  With
     * CONNECT_PROFILE_MATCH_DONE and a single-channel table there is no search
     * left -- the target only listens on the channel the host handed it -- so
     * one stale channel hint (the AP moved, the host's BSS entry is older than
     * the AP's own re-scan, the target was left on another channel by the
     * preceding 13-channel scan) turns every attempt into a guaranteed
     * failure: the 2026-10-04 20:03 boot sent byte-identical WMI_CONNECTs to
     * the 17:40 boot that associated in 56 ms and then reported NO_NETWORK_AVAIL
     * four times and AUTH_FAILED twice with nothing delivered at all.
     *
     * Spend up to N3DS_NO_NETWORK_RETRIES_MAX retries re-submitting the same
     * command with the channel table handed back to the target (numChannels 0)
     * and CONNECT_PROFILE_MATCH_DONE cleared, so the target runs its own
     * profile search across 11g and finds the AP wherever it is.  The supplicant
     * is still waiting on this connect, so nothing is reported to cfg80211;
     * only a failed recovery falls through to the normal notification. */
    /* N3DS_AR6014_SILENT_REASSOC: the target drops a *live* association
     * 1.5 s after the keys go in (2026-10-04 21:0x: handshake complete at
     * 81.34 s, NO_NETWORK_AVAIL with a zero BSSID at 82.70 s).  Telling
     * cfg80211 about that costs a full teardown: the framework drops its
     * DHCP lease, wpa_supplicant blacklists the BSSID ("Added BSSID
     * <AP-BSSID> into blacklist") and the whole cycle restarts from a
     * scan.  The target re-associates happily on a fresh connect, so submit
     * the same recovery connect as for a pending one and let the upper layers
     * believe the link is up: the supplicant is told nothing, keeps its keys,
     * sees the next handshake come through, and DHCP keeps its address.
     * Bounded, and only while the *target* is the one that left: a
     * DISCONNECT_CMD (the framework asked) is still reported honestly. */
    if (reason == NO_NETWORK_AVAIL && ar->smeState == SME_CONNECTED &&
        n3ds_silent_reassocs < N3DS_SILENT_REASSOC_MAX) {
        u8 saved_flags = ar->arConnectCtrlFlags;

        n3ds_silent_reassocs++;
        if (down_interruptible(&ar->arSem) == 0) {
            status = n3ds_ar6014_set_all_channels(ar);
            if (status == 0) {
                ar->arConnectCtrlFlags = saved_flags & ~CONNECT_PROFILE_MATCH_DONE;
                status = wmi_connect_cmd(ar->arWmi,
                                         ar->arNetworkType,
                                         ar->arDot11AuthMode,
                                         n3ds_last_auth_mode,
                                         n3ds_last_pair_crypto,
                                         n3ds_last_pair_len,
                                         n3ds_last_group_crypto,
                                         n3ds_last_group_len,
                                         ar->arSsidLen, ar->arSsid,
                                         ar->arReqBssid, ar->arChannelHint,
                                         ar->arConnectCtrlFlags);
                ar->arConnectCtrlFlags = saved_flags;
            }
            up(&ar->arSem);
        }

        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: SILENT re-association #%u/%u after target drop, "
             "status=%d (link stays up for cfg80211)\n",
             n3ds_silent_reassocs, N3DS_SILENT_REASSOC_MAX, status));
        if (status == 0) {
            ar->arConnectPending = true;
            return;
        }
        /* Recovery failed: fall through and report the loss honestly. */
        n3ds_silent_reassocs = N3DS_SILENT_REASSOC_MAX;
    } else if (reason != NO_NETWORK_AVAIL) {
        n3ds_silent_reassocs = 0;
    }

    if (n3ds_keyvar >= 0 && n3ds_keyvar < N3DS_KEY_VARIANTS) {
        if (n3ds_keyvar_current != n3ds_keyvar) {
            n3ds_keyvar_current = n3ds_keyvar;
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                ("AR6002 key: pinned variant %d \"%s\"\n",
                 n3ds_keyvar_current,
                 n3ds_key_variants[n3ds_keyvar_current].name));
        }
    }

    if (reason == NO_NETWORK_AVAIL && ar->smeState == SME_CONNECTING &&
        n3ds_no_network_retries < N3DS_NO_NETWORK_RETRIES_MAX) {
        u8 saved_flags = ar->arConnectCtrlFlags;

        n3ds_no_network_retries++;
        if (down_interruptible(&ar->arSem)) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                ("AR6002 connect: recovery #%u could not get arSem\n",
                 n3ds_no_network_retries));
            return;
        }
        status = n3ds_ar6014_set_all_channels(ar);
        if (status == 0) {
            ar->arConnectCtrlFlags = saved_flags & ~CONNECT_PROFILE_MATCH_DONE;
            status = wmi_connect_cmd(ar->arWmi,
                                     ar->arNetworkType,
                                     ar->arDot11AuthMode,
                                     n3ds_last_auth_mode,
                                     n3ds_last_pair_crypto,
                                     n3ds_last_pair_len,
                                     n3ds_last_group_crypto,
                                     n3ds_last_group_len,
                                     ar->arSsidLen, ar->arSsid,
                                     ar->arReqBssid, ar->arChannelHint,
                                     ar->arConnectCtrlFlags);
            ar->arConnectCtrlFlags = saved_flags;
        }
        up(&ar->arSem);

        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: recovery #%u/%u full-channel connect "
             "flags=0x%04x status=%d retries_left=%u\n",
             n3ds_no_network_retries, N3DS_NO_NETWORK_RETRIES_MAX,
             (unsigned int)(saved_flags & ~CONNECT_PROFILE_MATCH_DONE),
             status, n3ds_no_network_retries));
        if (status == 0) {
            /* Still connecting: keep the supplicant waiting. */
            ar->arConnectPending = true;
            return;
        }
    }

    if (reason == DISCONNECT_CMD && ar->arAutoAuthStage &&
        ar->smeState == SME_CONNECTING &&
        ar->arDot11AuthMode == OPEN_AUTH) {
        /* WEP auto-auth: open-system was refused, so retry the same profile
         * with shared-key auth.  The connect is still pending -- cfg80211
         * must not be told anything yet. */
        struct ar_key *key = &ar->keys[ar->arDefTxKeyIndex];

        if (down_interruptible(&ar->arSem)) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                            ("%s: busy, couldn't get access\n", __func__));
            return;
        }

        ar->arDot11AuthMode = SHARED_AUTH;
        ar->arAutoAuthStage = AUTH_IDLE;

        wmi_addKey_cmd(ar->arWmi, ar->arDefTxKeyIndex,
                       ar->arPairwiseCrypto,
                       GROUP_USAGE | TX_USAGE,
                       key->key_len,
                       NULL,
                       key->key, KEY_OP_INIT_VAL, NULL,
                       NO_SYNC_WMIFLAG);

        status = wmi_connect_cmd(ar->arWmi,
                                 ar->arNetworkType,
                                 ar->arDot11AuthMode,
                                 ar->arAuthMode,
                                 ar->arPairwiseCrypto,
                                 ar->arPairwiseCryptoLen,
                                 ar->arGroupCrypto,
                                 ar->arGroupCryptoLen,
                                 ar->arSsidLen,
                                 ar->arSsid,
                                 ar->arReqBssid,
                                 ar->arChannelHint,
                                 ar->arConnectCtrlFlags);
        up(&ar->arSem);

        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 connect: OPEN->SHARED auto-auth retry status=%d\n",
             status));
        return;
    }

    /* A disconnect the host did not ask for leaves the target still trying on
     * its own; park it.  Unlike mainline we do not then return and wait for a
     * follow-up DISCONNECT_CMD event before notifying, because NWM does not
     * reliably send one.  Notifying now is safe either way: a follow-up event
     * finds smeState already SME_DISCONNECTED and notifies nothing twice.
     *
     * N3DS_AR6014_NO_NETWORK_HANDOFF: this subsumes the old handoff that ran
     * for NO_NETWORK_AVAIL alone.  That block parked the target and completed
     * the pending cfg80211 request for reason 1 only, so supplicant would not
     * wait out its unrelated ten-second timer; every other reason fell through
     * silently.  The same handoff now runs for every reason, and reports
     * protocolReasonStatus rather than the driver's own reason code, which is
     * what cfg80211_disconnected() is documented to take. */
    if (reason != DISCONNECT_CMD) {
        wmi_disconnect_cmd(ar->arWmi);
    }

    ar->arConnectPending = false;

    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
        ("AR6002 connect: disconnect reason=%u proto=%u sme=%u bssid=%pM\n",
         reason, protocolReasonStatus, ar->smeState, bssid));

    if (ar->smeState == SME_CONNECTING) {
        cfg80211_connect_result(ar->arNetDev, bssid,
                                NULL, 0, NULL, 0,
                                WLAN_STATUS_UNSPECIFIED_FAILURE,
                                GFP_KERNEL);
    } else if (ar->smeState == SME_CONNECTED) {
        cfg80211_disconnected(ar->arNetDev, protocolReasonStatus,
                              NULL, 0, false, GFP_KERNEL);
    }

    ar->smeState = SME_DISCONNECTED;
}

void
ar6k_cfg80211_scan_node(void *arg, bss_t *ni)
{
    struct wiphy *wiphy = (struct wiphy *)arg;
    u16 size;
    unsigned char *ieeemgmtbuf = NULL;
    struct ieee80211_mgmt *mgmt;
    struct ieee80211_channel *channel;
    struct ieee80211_supported_band *band;
    struct ieee80211_common_ie  *cie;
    s32 signal;
    int freq;

    cie = &ni->ni_cie;

#define CHAN_IS_11A(x)  (!((x >= 2412) && (x <= 2484)))
    if(CHAN_IS_11A(cie->ie_chan)) {
        /* 11a */
        band = wiphy->bands[NL80211_BAND_5GHZ];
    } else if((cie->ie_erp) || (cie->ie_xrates)) {
        /* 11g */
        band = wiphy->bands[NL80211_BAND_2GHZ];
    } else {
        /* 11b */
        band = wiphy->bands[NL80211_BAND_2GHZ];
    }

    size = ni->ni_framelen + offsetof(struct ieee80211_mgmt, u);
    ieeemgmtbuf = A_MALLOC_NOWAIT(size);
    if(!ieeemgmtbuf)
    {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: ieeeMgmtbuf alloc error\n", __func__));
        return;
    }

    /* Note:
       TODO: Update target to include 802.11 mac header while sending bss info.
       Target removes 802.11 mac header while sending the bss info to host,
       cfg80211 needs it, for time being just filling the da, sa and bssid fields alone.
    */
    mgmt = (struct ieee80211_mgmt *)ieeemgmtbuf;
    memcpy(mgmt->da, bcast_mac, ATH_MAC_LEN);
    memcpy(mgmt->sa, ni->ni_macaddr, ATH_MAC_LEN);
    memcpy(mgmt->bssid, ni->ni_macaddr, ATH_MAC_LEN);
    memcpy(ieeemgmtbuf + offsetof(struct ieee80211_mgmt, u),
             ni->ni_buf, ni->ni_framelen);

    freq    = cie->ie_chan;
    channel = ieee80211_get_channel(wiphy, freq);
    /* N3DS_CFG80211_BSS_CHANNEL_GUARD: a partially decoded
     * firmware node may have no usable channel.  Never pass
     * NULL into cfg80211_inform_bss_frame(), whose internal
     * lookup dereferences the channel while publishing BSS. */
    if (!channel) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: dropping BSS with invalid frequency %d\n",
                         __func__, freq));
        kfree(ieeemgmtbuf);
        return;
    }
    /* N3DS_AR6014_CFG80211_DBM_SIGNAL: cfg80211 expects mBm here.
     * ni_snr is an unsigned margin (45 in the physical capture), not dBm;
     * exporting it produced the impossible +45 dBm / 134 dB SNR result.
     * The harvester stores its conservative estimate in signed ni_rssi. */
    signal = (s32)ni->ni_rssi * 100;

    /* N3DS_AR6014_SCAN_CENSUS: print every BSS the target hands up with the
     * channel it claims and its signal estimate.  A connect attempt that dies
     * with NO_NETWORK_AVAIL is ambiguous between "we cannot hear the AP" and
     * "the AP refuses us" (2026-10-04 20:03: identical WMI_CONNECT bytes to
     * the 17:40 boot that associated in 56 ms, but 1.5 s of nothing and zero
     * delivered frames), and this line is what separates the two on the next
     * capture: signal in the -40s means the radio path is fine and the AP is
     * refusing, -80s or worse means coverage/tuning. */
    {
        static atomic_t n3ds_scan_log_left = ATOMIC_INIT(32);

        if (atomic_read(&n3ds_scan_log_left) > 0) {
            atomic_dec(&n3ds_scan_log_left);
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                ("AR6002 scan bss: bssid=%pM freq=%u rssi=%d len=%u\n",
                 ni->ni_macaddr, (unsigned int)cie->ie_chan,
                 (int)ni->ni_rssi, (unsigned int)ni->ni_framelen));
        }
    }

	AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
		("%s: bssid %pM channel %d freq %d size %d\n", __func__,
			mgmt->bssid, channel->hw_value, freq, size));
    cfg80211_inform_bss_frame(wiphy, channel, mgmt,
                              le16_to_cpu(size),
                              signal, GFP_KERNEL);

    kfree (ieeemgmtbuf);
}

/* N3DS_NWM_PROBED_SSID_SLOTS: full NWM decompilation at 0x00136528
 * proves command 10 is a 35-byte {index, flag, length, ssid[32]} payload and
 * accepts indices 0..5. NWM uses slot 0 for connect setup, leaving 1..5 for
 * cfg80211 active probes. */
#define N3DS_MAX_SCAN_PROBED_SSIDS 5

/* Host discovery owns slots 1..5.  Slot zero is reserved for the profile
 * handed to WMI_CONNECT.  An empty cfg80211 SSID is not "nothing": it is the
 * wildcard probe represented by NWM/WMI flag 2 (ANY_SSID_FLAG). */
static int
n3ds_ar6014_program_discovery_ssids(struct ar6_softc *ar,
                                    struct cfg80211_scan_request *request,
                                    bool *wildcard_scan)
{
    u8 i;
    u8 count = request->n_ssids;

    *wildcard_scan = false;
    if (count > N3DS_MAX_SCAN_PROBED_SSIDS)
        count = N3DS_MAX_SCAN_PROBED_SSIDS;

    for (i = 0; i < count; i++) {
        u8 flag = request->ssids[i].ssid_len ?
                  SPECIFIC_SSID_FLAG : ANY_SSID_FLAG;

        if (flag == ANY_SSID_FLAG)
            *wildcard_scan = true;
        if (wmi_probedSsid_cmd(ar->arWmi, i + 1, flag,
                               request->ssids[i].ssid_len,
                               request->ssids[i].ssid) != 0)
            return -EIO;
    }
    return 0;
}

static void
n3ds_ar6014_clear_discovery_ssids(struct ar6_softc *ar,
                                  struct cfg80211_scan_request *request)
{
    u8 slot;
    u8 count = request->n_ssids;

    if (count > N3DS_MAX_SCAN_PROBED_SSIDS)
        count = N3DS_MAX_SCAN_PROBED_SSIDS;
    for (slot = 1; slot <= count; slot++)
        wmi_probedSsid_cmd(ar->arWmi, slot, DISABLE_SSID_FLAG, 0, NULL);
}

static int
ar6k_cfg80211_scan(struct wiphy *wiphy,
                   struct cfg80211_scan_request *request)
{
    static unsigned int scan_trace_count;
    struct net_device *ndev = request->wdev->netdev;
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(ndev);
    u32 forceFgScan = 0;
    bool trace_scan = scan_trace_count < 16;
    bool wildcard_scan = false;
    u16 channel_list[WMI_MAX_CHANNELS];
    s8 num_channels = 0;
    u8 channel_index;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(ar->arConnected) {
        forceFgScan = 1;
    }

    /* N3DS_AR6014_2GHZ_SCAN_CHANNELS: honor cfg80211's request, but submit
     * only valid AR6014 2.4-GHz frequencies.  numChannels=0 means an
     * unrestricted firmware scan and was producing 13-17 second cycles. */
    for (channel_index = 0;
         channel_index < request->n_channels &&
         num_channels < WMI_MAX_CHANNELS;
         channel_index++) {
        u16 freq = request->channels[channel_index]->center_freq;

        /* N3DS_AR6014_11G_CHANNEL_LIST: 11G-legal frequencies only, or the
         * target rejects the whole WMI_SET_CHANNEL_PARAMS command below and
         * silently keeps its previous table. */
        if (n3ds_ar6014_channel_is_11g(freq))
            channel_list[num_channels++] = freq;
    }
    if (!num_channels) {
        static const u16 ar6014_2ghz_channels[] = {
            2412, 2417, 2422, 2427, 2432, 2437, 2442,
            2447, 2452, 2457, 2462, 2467, 2472
        };

        memcpy(channel_list, ar6014_2ghz_channels,
               sizeof(ar6014_2ghz_channels));
        num_channels = ARRAY_SIZE(ar6014_2ghz_channels);
    }

    /* N3DS_CFG80211_SCAN_REQUEST_ORDER: publish ownership before the
     * target can return an immediate completion. Every setup error below
     * releases the request and every slot armed for this discovery. */
    if (cmpxchg(&ar->scan_request, NULL, request) != NULL)
        return -EBUSY;

    /* N3DS_SCAN_WATCHDOG: firmware can silently drop a WMI setup command
     * (e.g. "Control EP full") with no error and no completion event, which
     * would otherwise leave scan_request stuck forever and every future
     * scan trigger returning -EBUSY for the rest of the boot. */
    A_TIMEOUT_MS(&ar->scan_timer, AR6000_SCAN_TIMER_INTERVAL, 0);

    if (n3ds_ar6014_program_discovery_ssids(ar, request,
                                            &wildcard_scan) != 0)
        goto setup_fail;
    /* N3DS_AR6014_DISCOVERY_SCANPARAMS_RESTORE: WMI_SET_SCAN_PARAMS is global
     * target state, and the connect path above narrows it hard (20 ms dwell,
     * CONNECT_SCAN flags) without ever putting it back.  Discovery therefore
     * ran on whatever the last connect attempt left behind, which is why BSS
     * counts decayed across a boot (12 -> 8 -> 0 -> 4 -> 3) once association
     * attempts started, instead of staying at the count the first scan found.
     * Reprogram discovery parameters on every request, exactly as the channel
     * table below is already reprogrammed every time, so a failed connect
     * cannot poison later scans.  Zero dwell times mean "use the target
     * default", the state virgin firmware scanned in and the state discovery
     * was working in before the first connect. */
    if (wmi_scanparams_cmd(ar->arWmi, 0xffff, 0xffff, 0xffff,
                           0, 0, 0,
                           WMI_SHORTSCANRATIO_DEFAULT, DEFAULT_SCAN_CTRL_FLAGS,
                           0, N3DS_NWM_SCAN_PROBES_PER_SSID) != 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 scan: discovery scan params failed\n"));
        goto setup_fail;
    }
    if (wmi_set_channelParams_cmd(ar->arWmi, 0, WMI_11G_MODE,
                                  num_channels, channel_list) != 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 scan: discovery channel table failed channels=%d\n",
             num_channels));
        goto setup_fail;
    }
    if (!ar->arUserBssFilter &&
        wmi_bssfilter_cmd(ar->arWmi,
                         (ar->arConnected ? ALL_BUT_BSS_FILTER : ALL_BSS_FILTER),
                         0) != 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("%s: Couldn't set bss filtering\n", __func__));
        goto setup_fail;
    }
    if (trace_scan)
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 scan: discovery explicit channels=%d wildcard=%u\n",
             num_channels, wildcard_scan));

    /* N3DS_BOUNDED_CFG80211_SCAN_TRACE: prove framework-to-driver scan
     * submission without recreating the former perpetual HIF log flood. */
    if (trace_scan) {
        scan_trace_count++;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 scan: cfg80211 request ssids=%u first_len=%u channels=%u submitted_2ghz=%d\n",
             request->n_ssids,
             request->n_ssids ? request->ssids[0].ssid_len : 0,
             request->n_channels, num_channels));
    }

    /* Host discovery needs command 7's explicit channel list.  #245's
     * start_list=0 adaptation was valid only for NWM's selected-profile
     * connection scan and completed with zero BSS records on hardware. */
    if(wmi_startscan_cmd(ar->arWmi, WMI_LONG_SCAN, forceFgScan, false, \
                         0, 0, num_channels, channel_list) != 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("%s: wmi_startscan_cmd failed\n", __func__));
        goto setup_fail;
    }
    if (trace_scan)
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 scan: START_SCAN submitted explicit_11g_channels=%d\n",
             num_channels));
    return 0;

setup_fail:
    A_UNTIMEOUT(&ar->scan_timer);
    n3ds_ar6014_clear_discovery_ssids(ar, request);
    cmpxchg(&ar->scan_request, request, NULL);
    return -EIO;
}

void
ar6k_cfg80211_scanComplete_event(struct ar6_softc *ar, int status)
{
    struct cfg80211_scan_request *request;
    struct cfg80211_scan_info scan_info = {
        .aborted = (status == A_ECANCELED) || (status == A_EBUSY),
    };

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: status %d\n", __func__, status));

    /* N3DS_CFG80211_SCAN_COMPLETION_OWNERSHIP: claim and clear the request
     * atomically.  cfg80211_scan_done() returns ownership to cfg80211, so no
     * request field may be read after that callback, and disconnect/deinit
     * must not be able to complete the same request on another CPU. */
    request = xchg(&ar->scan_request, NULL);
    if (!request)
        return;

    A_UNTIMEOUT(&ar->scan_timer);

    if (!scan_info.aborted) {
        /* Translate data to cfg80211 mgmt format */
        wmi_iterate_nodes(ar->arWmi, ar6k_cfg80211_scan_node,
                          ar->wdev->wiphy);

    }

    /* Slots 1..5 are request-local. Slot zero belongs to connect. */
    n3ds_ar6014_clear_discovery_ssids(ar, request);

    cfg80211_scan_done(request, &scan_info);
}

void
ar6k_cfg80211_scan_timeout(struct timer_list *t)
{
    struct ar6_softc *ar = from_timer(ar, t, scan_timer);

    AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
        ("%s: scan watchdog fired, forcing scan-complete\n", __func__));
    ar6k_cfg80211_scanComplete_event(ar, A_ECANCELED);
}

static int
ar6k_cfg80211_add_key(struct wiphy *wiphy, struct net_device *ndev,
                      u8 key_index, bool pairwise, const u8 *mac_addr,
                      struct key_params *params)
{
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(ndev);
    struct ar_key *key = NULL;
    u8 key_usage;
    u8 key_type;
    u8 wire_len = 0;
    int status = 0;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s:\n", __func__));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(key_index < WMI_MIN_KEY_INDEX || key_index > WMI_MAX_KEY_INDEX) {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                        ("%s: key index %d out of bounds\n", __func__, key_index));
        return -ENOENT;
    }

    key = &ar->keys[key_index];
    A_MEMZERO(key, sizeof(struct ar_key));

    if (pairwise || (mac_addr && !is_broadcast_ether_addr(mac_addr))) {
        /* N3DS_AR6014_KEY_VARIANT: see the table above -- pairwise_usage is
         * swept per association. */
        key_usage = (u8)n3ds_key_variants[n3ds_keyvar_current].pairwise_usage;
        wire_len = n3ds_key_variants[n3ds_keyvar_current].wire_len;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 key: variant %d \"%s\" pairwise=1 usage=%u wire=%u\n",
             n3ds_keyvar_current,
             n3ds_key_variants[n3ds_keyvar_current].name,
             key_usage, wire_len));
    } else {
        key_usage = GROUP_USAGE;
        /* N3DS_AR6014_KEY_VARIANT: the group key follows the same row's wire
         * format so a variant is a complete description of one install pair
         * and the next boot's log says which bytes were on the wire. */
        wire_len = n3ds_key_variants[n3ds_keyvar_current].wire_len;
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
            ("AR6002 key: variant %d \"%s\" pairwise=0 usage=%u wire=%u\n",
             n3ds_keyvar_current,
             n3ds_key_variants[n3ds_keyvar_current].name,
             key_usage, wire_len));
    }

    if(params) {
        if(params->key_len > WLAN_MAX_KEY_LEN ||
            params->seq_len > IW_ENCODE_SEQ_MAX_SIZE)
            return -EINVAL;

        key->key_len = params->key_len;
        memcpy(key->key, params->key, key->key_len);
        key->seq_len = params->seq_len;
        memcpy(key->seq, params->seq, key->seq_len);
        key->cipher = params->cipher;
    }

    switch (key->cipher) {
    case WLAN_CIPHER_SUITE_WEP40:
    case WLAN_CIPHER_SUITE_WEP104:
        key_type = WEP_CRYPT;
        break;

    case WLAN_CIPHER_SUITE_TKIP:
        key_type = TKIP_CRYPT;
        break;

    case WLAN_CIPHER_SUITE_CCMP:
        key_type = AES_CRYPT;
        break;

    default:
        return -ENOTSUPP;
    }

    if (((WPA_PSK_AUTH == ar->arAuthMode) || (WPA2_PSK_AUTH == ar->arAuthMode)) &&
        (GROUP_USAGE & key_usage))
    {
        A_UNTIMEOUT(&ar->disconnect_timer);
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                    ("%s: index %d, key_len %d, key_type 0x%x,"\
                    " key_usage 0x%x, seq_len %d\n",
                    __func__, key_index, key->key_len, key_type,
                    key_usage, key->seq_len));

    ar->arDefTxKeyIndex = key_index;
    /* N3DS_AR6014_NWM_KEY_MACADDR: Nintendo's own ADD_CIPHER_KEY template
     * leaves key_macaddr all-zero ("mac=zero" in the AR6002 key log); the
     * cfg80211/wext pairwise mac_addr was set to the AP BSSID, which the
     * AR6014 target also rejects.  Hand wmi_addKey_cmd NULL so the field
     * stays zeroed, exactly like NWM. */
    status = wmi_addKey_cmd_ex(ar->arWmi, ar->arDefTxKeyIndex, key_type, key_usage,
                    key->key_len, key->seq, key->key, KEY_OP_INIT_VAL,
                    NULL, SYNC_BOTH_WMIFLAG, wire_len);


    if (status) {
        return -EIO;
    }

    return 0;
}

static int
ar6k_cfg80211_del_key(struct wiphy *wiphy, struct net_device *ndev,
                      u8 key_index, bool pairwise, const u8 *mac_addr)
{
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(ndev);

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: index %d\n", __func__, key_index));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(key_index < WMI_MIN_KEY_INDEX || key_index > WMI_MAX_KEY_INDEX) {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                        ("%s: key index %d out of bounds\n", __func__, key_index));
        return -ENOENT;
    }

    if(!ar->keys[key_index].key_len) {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: index %d is empty\n", __func__, key_index));
        return 0;
    }

    ar->keys[key_index].key_len = 0;

    return wmi_deleteKey_cmd(ar->arWmi, key_index);
}


static int
ar6k_cfg80211_get_key(struct wiphy *wiphy, struct net_device *ndev,
                      u8 key_index, bool pairwise, const u8 *mac_addr,
                      void *cookie,
                      void (*callback)(void *cookie, struct key_params*))
{
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(ndev);
    struct ar_key *key = NULL;
    struct key_params params;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: index %d\n", __func__, key_index));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(key_index < WMI_MIN_KEY_INDEX || key_index > WMI_MAX_KEY_INDEX) {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                        ("%s: key index %d out of bounds\n", __func__, key_index));
        return -ENOENT;
    }

    key = &ar->keys[key_index];
    A_MEMZERO(&params, sizeof(params));
    params.cipher = key->cipher;
    params.key_len = key->key_len;
    params.seq_len = key->seq_len;
    params.seq = key->seq;
    params.key = key->key;

    callback(cookie, &params);

    return key->key_len ? 0 : -ENOENT;
}


static int
ar6k_cfg80211_set_default_key(struct wiphy *wiphy, struct net_device *ndev,
                              u8 key_index, bool unicast, bool multicast)
{
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(ndev);
    struct ar_key *key = NULL;
    int status = 0;
    u8 key_usage;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: index %d\n", __func__, key_index));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(key_index < WMI_MIN_KEY_INDEX || key_index > WMI_MAX_KEY_INDEX) {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                        ("%s: key index %d out of bounds\n",
                        __func__, key_index));
        return -ENOENT;
    }

    if(!ar->keys[key_index].key_len) {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: invalid key index %d\n",
                        __func__, key_index));
        return -EINVAL;
    }

    ar->arDefTxKeyIndex = key_index;
    key = &ar->keys[ar->arDefTxKeyIndex];
    key_usage = GROUP_USAGE;
    if (WEP_CRYPT == ar->arPairwiseCrypto) {
        key_usage |= TX_USAGE;
    }

    status = wmi_addKey_cmd(ar->arWmi, ar->arDefTxKeyIndex,
                            ar->arPairwiseCrypto, key_usage,
                            key->key_len, key->seq, key->key, KEY_OP_INIT_VAL,
                            NULL, SYNC_BOTH_WMIFLAG);
    if (status) {
        return -EIO;
    }

    return 0;
}

static int
ar6k_cfg80211_set_default_mgmt_key(struct wiphy *wiphy, struct net_device *ndev,
                                   u8 key_index)
{
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(ndev);

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: index %d\n", __func__, key_index));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: not supported\n", __func__));
    return -ENOTSUPP;
}

void
ar6k_cfg80211_tkip_micerr_event(struct ar6_softc *ar, u8 keyid, bool ismcast)
{
    AR_DEBUG_PRINTF(ATH_DEBUG_INFO,
                    ("%s: keyid %d, ismcast %d\n", __func__, keyid, ismcast));

    cfg80211_michael_mic_failure(ar->arNetDev, ar->arBssid,
                                 (ismcast ? NL80211_KEYTYPE_GROUP : NL80211_KEYTYPE_PAIRWISE),
                                 keyid, NULL, GFP_KERNEL);
}

static int
ar6k_cfg80211_set_wiphy_params(struct wiphy *wiphy, u32 changed)
{
    struct ar6_softc *ar = (struct ar6_softc *)wiphy_priv(wiphy);

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: changed 0x%x\n", __func__, changed));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if (changed & WIPHY_PARAM_RTS_THRESHOLD) {
        if (wmi_set_rts_cmd(ar->arWmi,wiphy->rts_threshold) != 0){
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: wmi_set_rts_cmd failed\n", __func__));
            return -EIO;
        }
    }

    return 0;
}

static int
ar6k_cfg80211_set_bitrate_mask(struct wiphy *wiphy, struct net_device *dev,
                               const u8 *peer,
                               const struct cfg80211_bitrate_mask *mask)
{
    AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("Setting rates: Not supported\n"));
    return -EIO;
}

/* The type nl80211_tx_power_setting replaces the following data type from 2.6.36 onwards */
static int
ar6k_cfg80211_set_txpower(struct wiphy *wiphy, struct wireless_dev *wdev, enum nl80211_tx_power_setting type, int dbm)
{
    struct ar6_softc *ar = (struct ar6_softc *)wiphy_priv(wiphy);
    u8 ar_dbm;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: type 0x%x, dbm %d\n", __func__, type, dbm));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    ar->arTxPwrSet = false;
    switch(type) {
    case NL80211_TX_POWER_AUTOMATIC:
        return 0;
    case NL80211_TX_POWER_LIMITED:
        ar->arTxPwr = ar_dbm = dbm;
        ar->arTxPwrSet = true;
        break;
    default:
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: type 0x%x not supported\n", __func__, type));
        return -EOPNOTSUPP;
    }

    wmi_set_txPwr_cmd(ar->arWmi, ar_dbm);

    return 0;
}

static int
ar6k_cfg80211_get_txpower(struct wiphy *wiphy, struct wireless_dev *wdev, int *dbm)
{
    struct ar6_softc *ar = (struct ar6_softc *)wiphy_priv(wiphy);

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if((ar->arConnected == true)) {
        ar->arTxPwr = 0;

        if(wmi_get_txPwr_cmd(ar->arWmi) != 0) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: wmi_get_txPwr_cmd failed\n", __func__));
            return -EIO;
        }

        wait_event_interruptible_timeout(arEvent, ar->arTxPwr != 0, 5 * HZ);

        if(signal_pending(current)) {
            AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Target did not respond\n", __func__));
            return -EINTR;
        }
    }

    *dbm = ar->arTxPwr;
    return 0;
}

static int
ar6k_cfg80211_set_power_mgmt(struct wiphy *wiphy,
                             struct net_device *dev,
                             bool pmgmt, int timeout)
{
    struct ar6_softc *ar = ar6k_priv(dev);
    WMI_POWER_MODE_CMD pwrMode;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: pmgmt %d, timeout %d\n", __func__, pmgmt, timeout));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(pmgmt) {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: Max Perf\n", __func__));
        pwrMode.powerMode = REC_POWER;
    } else {
        AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: Rec Power\n", __func__));
        pwrMode.powerMode = MAX_PERF_POWER;
    }

    if(wmi_powermode_cmd(ar->arWmi, pwrMode.powerMode) != 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: wmi_powermode_cmd failed\n", __func__));
        return -EIO;
    }

    return 0;
}

static struct wireless_dev *
ar6k_cfg80211_add_virtual_intf(struct wiphy *wiphy, const char *name,
            				    unsigned char name_assign_type,
            				    enum nl80211_iftype type,
            				    struct vif_params *params)
{

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: not supported\n", __func__));

    /* Multiple virtual interface is not supported.
     * The default interface supports STA and IBSS type
     */
    return ERR_PTR(-EOPNOTSUPP);
}

static int
ar6k_cfg80211_del_virtual_intf(struct wiphy *wiphy, struct wireless_dev *wdev)
{

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: not supported\n", __func__));

    /* Multiple virtual interface is not supported.
     * The default interface supports STA and IBSS type
     */
    return -EOPNOTSUPP;
}

static int
ar6k_cfg80211_change_iface(struct wiphy *wiphy, struct net_device *ndev,
                           enum nl80211_iftype type,
                           struct vif_params *params)
{
    struct ar6_softc *ar = ar6k_priv(ndev);
    struct wireless_dev *wdev = ar->wdev;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: type %u\n", __func__, type));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    switch (type) {
    case NL80211_IFTYPE_STATION:
        ar->arNextMode = INFRA_NETWORK;
        break;
    case NL80211_IFTYPE_ADHOC:
        ar->arNextMode = ADHOC_NETWORK;
        break;
    default:
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: type %u\n", __func__, type));
        return -EOPNOTSUPP;
    }

    wdev->iftype = type;

    return 0;
}

static int
ar6k_cfg80211_join_ibss(struct wiphy *wiphy, struct net_device *dev,
                        struct cfg80211_ibss_params *ibss_param)
{
    struct ar6_softc *ar = ar6k_priv(dev);
    int status;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    if(!ibss_param->ssid_len || IEEE80211_MAX_SSID_LEN < ibss_param->ssid_len) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: ssid invalid\n", __func__));
        return -EINVAL;
    }

    ar->arSsidLen = ibss_param->ssid_len;
    memcpy(ar->arSsid, ibss_param->ssid, ar->arSsidLen);

    if(ibss_param->chandef.chan) {
        ar->arChannelHint = ibss_param->chandef.chan->center_freq;
    }

    if(ibss_param->channel_fixed) {
        /* TODO: channel_fixed: The channel should be fixed, do not search for
         * IBSSs to join on other channels. Target firmware does not support this
         * feature, needs to be updated.*/
    }

    A_MEMZERO(ar->arReqBssid, sizeof(ar->arReqBssid));
    if(ibss_param->bssid) {
        if(memcmp(&ibss_param->bssid, bcast_mac, AR6000_ETH_ADDR_LEN)) {
            memcpy(ar->arReqBssid, ibss_param->bssid, sizeof(ar->arReqBssid));
        }
    }

    ar6k_set_wpa_version(ar, 0);
    ar6k_set_auth_type(ar, NL80211_AUTHTYPE_OPEN_SYSTEM);

    if(ibss_param->privacy) {
        ar6k_set_cipher(ar, WLAN_CIPHER_SUITE_WEP40, true);
        ar6k_set_cipher(ar, WLAN_CIPHER_SUITE_WEP40, false);
    } else {
        ar6k_set_cipher(ar, IW_AUTH_CIPHER_NONE, true);
        ar6k_set_cipher(ar, IW_AUTH_CIPHER_NONE, false);
    }

    ar->arNetworkType = ar->arNextMode;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: Connect called with authmode %d dot11 auth %d"\
                    " PW crypto %d PW crypto Len %d GRP crypto %d"\
                    " GRP crypto Len %d channel hint %u\n",
                    __func__, ar->arAuthMode, ar->arDot11AuthMode,
                    ar->arPairwiseCrypto, ar->arPairwiseCryptoLen,
                    ar->arGroupCrypto, ar->arGroupCryptoLen, ar->arChannelHint));

    status = wmi_connect_cmd(ar->arWmi, ar->arNetworkType,
                            ar->arDot11AuthMode, ar->arAuthMode,
                            ar->arPairwiseCrypto, ar->arPairwiseCryptoLen,
                            ar->arGroupCrypto,ar->arGroupCryptoLen,
                            ar->arSsidLen, ar->arSsid,
                            ar->arReqBssid, ar->arChannelHint,
                            ar->arConnectCtrlFlags);
    ar->arConnectPending = true;

    return 0;
}

static int
ar6k_cfg80211_leave_ibss(struct wiphy *wiphy, struct net_device *dev)
{
    struct ar6_softc *ar = (struct ar6_softc *)ar6k_priv(dev);

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    if(ar->arWmiReady == false) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wmi not ready\n", __func__));
        return -EIO;
    }

    if(ar->arWlanState == WLAN_DISABLED) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR, ("%s: Wlan disabled\n", __func__));
        return -EIO;
    }

    ar6000_disconnect(ar);
    A_MEMZERO(ar->arSsid, sizeof(ar->arSsid));
    ar->arSsidLen = 0;

    return 0;
}

#ifdef CONFIG_NL80211_TESTMODE
enum ar6k_testmode_attr {
	__AR6K_TM_ATTR_INVALID	= 0,
	AR6K_TM_ATTR_CMD	= 1,
	AR6K_TM_ATTR_DATA	= 2,

	/* keep last */
	__AR6K_TM_ATTR_AFTER_LAST,
	AR6K_TM_ATTR_MAX	= __AR6K_TM_ATTR_AFTER_LAST - 1
};

enum ar6k_testmode_cmd {
	AR6K_TM_CMD_TCMD		= 0,
	AR6K_TM_CMD_RX_REPORT		= 1,
};

#define AR6K_TM_DATA_MAX_LEN 5000

static const struct nla_policy ar6k_testmode_policy[AR6K_TM_ATTR_MAX + 1] = {
	[AR6K_TM_ATTR_CMD] = { .type = NLA_U32 },
	[AR6K_TM_ATTR_DATA] = { .type = NLA_BINARY,
				.len = AR6K_TM_DATA_MAX_LEN },
};

void ar6000_testmode_rx_report_event(struct ar6_softc *ar, void *buf,
				     int buf_len)
{
	if (down_interruptible(&ar->arSem))
		return;

	kfree(ar->tcmd_rx_report);

	ar->tcmd_rx_report = kmemdup(buf, buf_len, GFP_KERNEL);
	ar->tcmd_rx_report_len = buf_len;

	up(&ar->arSem);

	wake_up(&arEvent);
}

static int ar6000_testmode_rx_report(struct ar6_softc *ar, void *buf,
				     int buf_len, struct sk_buff *skb)
{
	int ret = 0;
	long left;

	if (down_interruptible(&ar->arSem))
		return -ERESTARTSYS;

	if (ar->arWmiReady == false) {
		ret = -EIO;
		goto out;
	}

	if (ar->bIsDestroyProgress) {
		ret = -EBUSY;
		goto out;
	}

	WARN_ON(ar->tcmd_rx_report != NULL);
	WARN_ON(ar->tcmd_rx_report_len > 0);

	if (wmi_test_cmd(ar->arWmi, buf, buf_len) < 0) {
		up(&ar->arSem);
		return -EIO;
	}

	left = wait_event_interruptible_timeout(arEvent,
					       ar->tcmd_rx_report != NULL,
					       wmitimeout * HZ);

	if (left == 0) {
		ret = -ETIMEDOUT;
		goto out;
	} else if (left < 0) {
		ret = left;
		goto out;
	}

	if (ar->tcmd_rx_report == NULL || ar->tcmd_rx_report_len == 0) {
		ret = -EINVAL;
		goto out;
	}

	NLA_PUT(skb, AR6K_TM_ATTR_DATA, ar->tcmd_rx_report_len,
		ar->tcmd_rx_report);

	kfree(ar->tcmd_rx_report);
	ar->tcmd_rx_report = NULL;

out:
	up(&ar->arSem);

	return ret;

nla_put_failure:
	ret = -ENOBUFS;
	goto out;
}

static int ar6k_testmode_cmd(struct wiphy *wiphy, void *data, int len)
{
	struct ar6_softc *ar = wiphy_priv(wiphy);
	struct nlattr *tb[AR6K_TM_ATTR_MAX + 1];
	int err, buf_len, reply_len;
	struct sk_buff *skb;
	void *buf;

	err = nla_parse(tb, AR6K_TM_ATTR_MAX, data, len,
			ar6k_testmode_policy);
	if (err)
		return err;

	if (!tb[AR6K_TM_ATTR_CMD])
		return -EINVAL;

	switch (nla_get_u32(tb[AR6K_TM_ATTR_CMD])) {
	case AR6K_TM_CMD_TCMD:
		if (!tb[AR6K_TM_ATTR_DATA])
			return -EINVAL;

		buf = nla_data(tb[AR6K_TM_ATTR_DATA]);
		buf_len = nla_len(tb[AR6K_TM_ATTR_DATA]);

		wmi_test_cmd(ar->arWmi, buf, buf_len);

		return 0;

		break;
	case AR6K_TM_CMD_RX_REPORT:
		if (!tb[AR6K_TM_ATTR_DATA])
			return -EINVAL;

		buf = nla_data(tb[AR6K_TM_ATTR_DATA]);
		buf_len = nla_len(tb[AR6K_TM_ATTR_DATA]);

		reply_len = nla_total_size(AR6K_TM_DATA_MAX_LEN);
		skb = cfg80211_testmode_alloc_reply_skb(wiphy, reply_len);
		if (!skb)
			return -ENOMEM;

		err = ar6000_testmode_rx_report(ar, buf, buf_len, skb);
		if (err < 0) {
			kfree_skb(skb);
			return err;
		}

		return cfg80211_testmode_reply(skb);
	default:
		return -EOPNOTSUPP;
	}
}
#endif

static const
u32 cipher_suites[] = {
    WLAN_CIPHER_SUITE_WEP40,
    WLAN_CIPHER_SUITE_WEP104,
    WLAN_CIPHER_SUITE_TKIP,
    WLAN_CIPHER_SUITE_CCMP,
};

bool is_rate_legacy(s32 rate)
{
	static const s32 legacy[] = { 1000, 2000, 5500, 11000,
				      6000, 9000, 12000, 18000, 24000,
				      36000, 48000, 54000 };
	u8 i;

	for (i = 0; i < ARRAY_SIZE(legacy); i++) {
		if (rate == legacy[i])
			return true;
	}

	return false;
}

bool is_rate_ht20(s32 rate, u8 *mcs, bool *sgi)
{
	static const s32 ht20[] = { 6500, 13000, 19500, 26000, 39000,
				    52000, 58500, 65000, 72200 };
	u8 i;

	for (i = 0; i < ARRAY_SIZE(ht20); i++) {
		if (rate == ht20[i]) {
			if (i == ARRAY_SIZE(ht20) - 1)
				/* last rate uses sgi */
				*sgi = true;
			else
				*sgi = false;

			*mcs = i;
			return true;
		}
	}
	return false;
}

bool is_rate_ht40(s32 rate, u8 *mcs, bool *sgi)
{
	static const s32 ht40[] = { 13500, 27000, 40500, 54000,
				    81000, 108000, 121500, 135000,
				    150000 };
	u8 i;

	for (i = 0; i < ARRAY_SIZE(ht40); i++) {
		if (rate == ht40[i]) {
			if (i == ARRAY_SIZE(ht40) - 1)
				/* last rate uses sgi */
				*sgi = true;
			else
				*sgi = false;

			*mcs = i;
			return true;
		}
	}

	return false;
}

static int ar6k_get_station(struct wiphy *wiphy, struct net_device *dev,
			    const u8 *mac, struct station_info *sinfo)
{
	struct ar6_softc *ar = ar6k_priv(dev);
	long left;
	bool sgi;
	s32 rate;
	int ret;
	u8 mcs;

	if (memcmp(mac, ar->arBssid, ETH_ALEN) != 0)
		return -ENOENT;

	if (down_interruptible(&ar->arSem))
		return -EBUSY;

	ar->statsUpdatePending = true;

	ret = wmi_get_stats_cmd(ar->arWmi);

	if (ret != 0) {
		up(&ar->arSem);
		return -EIO;
	}

	left = wait_event_interruptible_timeout(arEvent,
						ar->statsUpdatePending == false,
						wmitimeout * HZ);

	up(&ar->arSem);

	if (left == 0)
		return -ETIMEDOUT;
	else if (left < 0)
		return left;

	if (ar->arTargetStats.rx_bytes) {
		sinfo->rx_bytes = ar->arTargetStats.rx_bytes;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_BYTES64);
		sinfo->rx_packets = ar->arTargetStats.rx_packets;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_PACKETS);
	}

	if (ar->arTargetStats.tx_bytes) {
		sinfo->tx_bytes = ar->arTargetStats.tx_bytes;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BYTES64);
		sinfo->tx_packets = ar->arTargetStats.tx_packets;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_PACKETS);
	}

	sinfo->signal = ar->arTargetStats.cs_rssi;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);

	rate = ar->arTargetStats.tx_unicast_rate;

	if (is_rate_legacy(rate)) {
		sinfo->txrate.legacy = rate / 100;
	} else if (is_rate_ht20(rate, &mcs, &sgi)) {
		if (sgi) {
			sinfo->txrate.flags |= RATE_INFO_FLAGS_SHORT_GI;
			sinfo->txrate.mcs = mcs - 1;
		} else {
			sinfo->txrate.mcs = mcs;
		}

		sinfo->txrate.flags |= RATE_INFO_FLAGS_MCS;
	} else if (is_rate_ht40(rate, &mcs, &sgi)) {
		if (sgi) {
			sinfo->txrate.flags |= RATE_INFO_FLAGS_SHORT_GI;
			sinfo->txrate.mcs = mcs - 1;
		} else {
			sinfo->txrate.mcs = mcs;
		}

		sinfo->txrate.bw = RATE_INFO_BW_40;
		sinfo->txrate.flags |= RATE_INFO_FLAGS_MCS;
	} else {
		WARN(1, "invalid rate: %d", rate);
		return 0;
	}

	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);

	return 0;
}

static int ar6k_set_pmksa(struct wiphy *wiphy, struct net_device *netdev,
			  struct cfg80211_pmksa *pmksa)
{
	struct ar6_softc *ar = ar6k_priv(netdev);
	return wmi_setPmkid_cmd(ar->arWmi, (u8 *)pmksa->bssid, (u8 *)pmksa->pmkid, true);
}

static int ar6k_del_pmksa(struct wiphy *wiphy, struct net_device *netdev,
			  struct cfg80211_pmksa *pmksa)
{
	struct ar6_softc *ar = ar6k_priv(netdev);
	return wmi_setPmkid_cmd(ar->arWmi, (u8 *)pmksa->bssid, (u8 *)pmksa->pmkid, false);
}

static int ar6k_flush_pmksa(struct wiphy *wiphy, struct net_device *netdev)
{
	struct ar6_softc *ar = ar6k_priv(netdev);
	if (ar->arConnected)
		return wmi_setPmkid_cmd(ar->arWmi, ar->arBssid, NULL, false);
	return 0;
}

static struct
cfg80211_ops ar6k_cfg80211_ops = {
    .change_virtual_intf = ar6k_cfg80211_change_iface,
    .add_virtual_intf = ar6k_cfg80211_add_virtual_intf,
    .del_virtual_intf = ar6k_cfg80211_del_virtual_intf,
    .scan = ar6k_cfg80211_scan,
    .connect = ar6k_cfg80211_connect,
    .disconnect = ar6k_cfg80211_disconnect,
    .add_key = ar6k_cfg80211_add_key,
    .get_key = ar6k_cfg80211_get_key,
    .del_key = ar6k_cfg80211_del_key,
    .set_default_key = ar6k_cfg80211_set_default_key,
    .set_default_mgmt_key = ar6k_cfg80211_set_default_mgmt_key,
    .set_wiphy_params = ar6k_cfg80211_set_wiphy_params,
    .set_bitrate_mask = ar6k_cfg80211_set_bitrate_mask,
    .set_tx_power = ar6k_cfg80211_set_txpower,
    .get_tx_power = ar6k_cfg80211_get_txpower,
    .set_power_mgmt = ar6k_cfg80211_set_power_mgmt,
    .join_ibss = ar6k_cfg80211_join_ibss,
    .leave_ibss = ar6k_cfg80211_leave_ibss,
    .get_station = ar6k_get_station,
    .set_pmksa = ar6k_set_pmksa,
    .del_pmksa = ar6k_del_pmksa,
    .flush_pmksa = ar6k_flush_pmksa,
    CFG80211_TESTMODE_CMD(ar6k_testmode_cmd)
};

struct wireless_dev *
ar6k_cfg80211_init(struct device *dev)
{
    int ret = 0;
    struct wireless_dev *wdev;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    wdev = kzalloc(sizeof(struct wireless_dev), GFP_KERNEL);
    if(!wdev) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: Couldn't allocate wireless device\n", __func__));
        return ERR_PTR(-ENOMEM);
    }

    /* create a new wiphy for use with cfg80211 */
    wdev->wiphy = wiphy_new(&ar6k_cfg80211_ops, sizeof(struct ar6_softc));
    if(!wdev->wiphy) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: Couldn't allocate wiphy device\n", __func__));
        kfree(wdev);
        return ERR_PTR(-ENOMEM);
    }

    /* set device pointer for wiphy */
    set_wiphy_dev(wdev->wiphy, dev);

    wdev->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
                                   BIT(NL80211_IFTYPE_ADHOC);
    /* max num of ssids that can be probed during scanning */
    wdev->wiphy->max_scan_ssids = N3DS_MAX_SCAN_PROBED_SSIDS;
    /* N3DS_CFG80211_SCAN_IE_CAPABILITY: wiphy_new() leaves this at zero.
     * wpa_supplicant 2.10 includes generic probe-request IEs, so nl80211
     * rejected every scan with -EINVAL before this driver's .scan callback.
     * Match the maintained ath6kl driver's advertised scan-IE limit. */
    wdev->wiphy->max_scan_ie_len = 1000;
    /* N3DS_AR6014_2GHZ_ONLY_WIPHY: Nintendo's AR6014 is an
     * 802.11b/g 2.4-GHz radio.  Do not advertise generic 5-GHz
     * channels that this target cannot scan or associate on. */
    wdev->wiphy->bands[NL80211_BAND_2GHZ] = &ar6k_band_2ghz;
    wdev->wiphy->bands[NL80211_BAND_5GHZ] = NULL;
    wdev->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;

    wdev->wiphy->cipher_suites = cipher_suites;
    wdev->wiphy->n_cipher_suites = ARRAY_SIZE(cipher_suites);

    ret = wiphy_register(wdev->wiphy);
    if(ret < 0) {
        AR_DEBUG_PRINTF(ATH_DEBUG_ERR,
                        ("%s: Couldn't register wiphy device\n", __func__));
        wiphy_free(wdev->wiphy);
        return ERR_PTR(ret);
    }

    return wdev;
}

void
ar6k_cfg80211_deinit(struct ar6_softc *ar)
{
    struct wireless_dev *wdev = ar->wdev;

    AR_DEBUG_PRINTF(ATH_DEBUG_INFO, ("%s: \n", __func__));

    if(ar->scan_request) {
        struct cfg80211_scan_request *request;
        struct cfg80211_scan_info scan_info = { .aborted = true };

        A_UNTIMEOUT(&ar->scan_timer);
        request = xchg(&ar->scan_request, NULL);
        if (request)
            cfg80211_scan_done(request, &scan_info);
    }

    if(!wdev)
        return;

    wiphy_unregister(wdev->wiphy);
    wiphy_free(wdev->wiphy);
    kfree(wdev);
}



