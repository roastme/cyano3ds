# Wi-Fi on the Nintendo 3DS (Atheros AR6014G on SDIO)

**Goal: working Wi-Fi in Android (CM7.2).**  Wi-Fi is the part of this port
that still needs the most bring-up work, and almost everything about it is
undocumented upstream.  This document describes how the AR6014G is powered,
driven and fed its firmware.

## 1. The hardware

* The 3DS Wi-Fi is an **Atheros AR6014G** (DWM-W028 daughterboard on the
  Old3DS; soldered to the mainboard on the New3DS/New2DS).  It is **not** a
  Broadcom part, and it is **not** the ARM9/ARM11 "DS-mode" Wi-Fi.
* It is an **SDIO** card on its own SD host controller at **0x10122000**
  (the `nwm:` node in `nintendo3ds.dtsi`, driven by
  `drivers/platform/nintendo3ds/ctr_sdhc.c`).
* GBATEK (*DSi Atheros Wifi*) classifies AR6014 with the AR6002/AR6013 as the
  **"hw2" family** (the AR6003 is "hw4"):

  | | AR6002 | AR6003 | AR6004 | AR6013 | **AR6014** |
  |---|---|---|---|---|---|
  | hw name | hw2 | hw4 | hw6 | hw2 | **hw2** |
  | ROM ID hex | 20000188 | ? | ? | 23000024 | **2300006f** |
  | RAM base | 100000 | 140000 | – | 120000 | **120000** |
  | RAM host-interest | 500400 | 540600 | 400600 | 520000 | **520000** |
  | RAM start of free | 502400 | – | – | 524c00 | **524c00** |
  | BMI_DONE entry | 915000 | – | – | 927000 | **927000** |
  | module crystal | – | 26 MHz | 40 MHz | – | **40 MHz** |

* On the SDIO bus the chip reports **`vendor=0x0271 device=0x0201`**
  (GBATEK's AR6013/AR6014 MANFID `0x02010271`), and BMI identifies the target
  as **`ver=0x2300006f type=0x2`** — the AR6002/"hw2" family.
* **Power/reset**: **GPIO4 bit 0 at 0x10147028** is "WiFi enable" (GBATEK:
  `0 = Reset, need re-upload wifi firmware, 1 = On`).  The bootloader leaves
  it low, so the chip is dark and the SDIO bus enumerates nothing until the
  driver asserts it.  GPIO4 is a direction-less output-only port; `bgpio`'s
  `bgpio_simple_dir_out` handles it, so the ordinary gpiolib consumer API
  works.  An early linux-3ds attempt to bring up this chip never even saw a
  card because this GPIO was not touched.

## 2. The driver: the pre-mainline staging AR6K ("ath6k_legacy")

Mainline `ath6kl` knows AR6003/AR6004 only; the AR6002 "hw2" family — which
is what the AR6014 *is* — was supported solely by the old **staging** ath6kl
("AR6K") codebase that was removed from the kernel around 3.2.

This port uses the staging driver as resurrected and cfg80211/WEXT-ported by
the Octoblimp/Android3DS project.  `port/scripts/fix-wifi-octoblimp.py`
(called by `apply-kernel-port.sh`) installs it:

* `port/kernel/drivers/staging/ath6k_legacy/` — the driver
  (`CONFIG_ATH6K_LEGACY`, built into `zImage`);
* `port/kernel/drivers/platform/nintendo3ds/ctr_sdhc.{c,h}` — Octoblimp's SDIO
  host changes (AR6002 PIO edge fix, card-IRQ timer fallback, `wifi_recover`
  sysfs) *plus* the Wi-Fi power handling: it requests the `wifi-enable` GPIO,
  toggles reset (100 ms low, 50 ms settle before SDIO) and powers the chip
  back up for a recovery;
* the canonical `nwm` node in `nintendo3ds.dtsi`, including
  `wifi-enable-gpios = <&gpio4 0 GPIO_ACTIVE_HIGH>`.

Two build gotchas that this script enforces:

* **Mainline `ath6kl` must stay disabled** (`build-kernel.sh`): both drivers
  claim the same SDIO ids and the `wlan%d` interface.
* The `nwm` node must keep its **`reg`** property.  Without it
  `devm_platform_ioremap_resource()` fails and the probe never reaches the
  GPIO — the symptom is a dead bus, not an obvious error.

The driver is built in (it probes before any rootfs exists, so
`FW_LOADER_USER_HELPER` is off and the firmware must be inside the
initramfs) and boots the AR6014 directly from **Nintendo's own NWM blobs** at
`ath6k/AR6002/nwm/` instead of SDK `.z77` images.  Defaults: `fwmode = 1`
(STA), `wlaninitmode = WLAN_INIT_MODE_DRV`.

## 3. The firmware: Nintendo's NWM module

The AR6014 (like every AR60xx) runs **Xtensa firmware that must be uploaded
into its RAM at every power-up**; the on-chip ROM is only a BMI loader.  That
firmware is **Nintendo's**, shipped inside the console's **NWM** module
(`3ds:\title\00040130\00002d02\content\000000vv.app` in normal mode; safe-mode
variants `00002d03`/`20002d03` exist as well).  GBATEK (*3DS Files - Module
NWM*) describes its layout: a database, a `Stub` block, and three "Main"
Xtensa images (Type1 = internet, Type4 = AP mode, Type5 =
MacFilter/GameId), plus ARM11 code that uploads them.

The **generic AR6002** firmware in old `linux-firmware`
(`ath6k/AR6002/{athwlan.bin.z77,data.patch.hw2_0.bin,eeprom.bin}`) is **not**
a substitute: the AR6014 ROM has a different function table / litbase, so the
generic image cannot run on it.

**Use the Type4 Main image.**  GBATEK warns that newer *Type1* firmware
revisions hang, and Type4 is what serves normal internet access; the block
sizes it lists (`1B1Bh`) match the newest Type1 in current NWM titles.
`mkinitramfs.sh` therefore installs `main_type4.bin` as the driver's Main
image (and keeps `main_type1.bin` alongside for A/B runs when present).

### Getting it

The port has no NAND driver, so the console itself cannot read the module:

1. Dump it on the console with GodMode9 — `port/gm9/DumpNWMWifi.lua` (modern
   GM9) or `port/gm9/DumpNWMWifi.gm9` (legacy) — which copies the NWM `.app`
   and the decrypted, decompressed ExeFS `.code` to `0:/gm9/out/wifi/`.
   See [port/gm9/README.md](../port/gm9/README.md).
2. Carve the six Xtensa blocks (`stub_data`, `stub_code`, `database`,
   `main_type1/4/5`) on the PC with `port/scripts/nwm-extract.py` (GBATEK
   literal-pool recipe; the invariable block sizes — Stub.data `0x38`,
   Stub.code `0x316`, Database `0x1E8` — validate the detected code base).
   This writes `stub_data.bin`, `stub_code.bin`, `database.bin` and
   `main_type1/4/5.bin` (plus `blocks.json`) into a `<name>.code.blocks/`
   directory. (`port/scripts/nwm-to-ath6k.py` is a separate converter that
   produces the SDK-style `ath6k/AR6014/` set instead; it is not needed
   here.)
3. Copy the four files the driver needs into `firmware/ath6k/AR6002/nwm/`
   (or point `NWM_DIR` at a directory holding them): `stub_data.bin`,
   `stub_code.bin`, `main_type4.bin` and `database.bin`, keeping
   `main_type1.bin` alongside for A/B runs when present. Use the Type4
   Main image: newer Type1 revisions hang (see GBATEK NWM). They are
   installed into the initramfs at `/lib/firmware/ath6k/AR6002/nwm/`.

See [BUILD.md](BUILD.md#firmware).  Cyano3DS ships none of this: it is
Nintendo firmware, dumped from your own console.

## 4. How the AR6014 boots: memory map and upload order

The four NWM blocks correspond to the AR6002 SDK's own files, confirmed by
byte comparison with the generic AR6002 set in `linux-firmware`, and the
upload sequence/addresses were recovered from Nintendo's ARM11 downloader
(Thumb-2 disassembly of the literal pool at `.code` offset 0x2b9d0):

| NWM block | AR6002 SDK file | AR6014 destination | transfer |
|---|---|---|---|
| `stub_data`  | `eeprom.data` | 0x00524c00 | raw `BMI_WRITE_MEMORY` |
| `stub_code`  | `eeprom.bin`  | 0x00527000 | raw, then `BMI_EXECUTE` 0x00927000 |
| `main_type4` | `athwlan.bin.z77` | 0x00524c00 | `BMI_LZ_DATA` (compressed) |
| `database`   | `data.patch.hw2_0.bin` | 0x0053fe18 | raw, `hi_dset_list_head = 0x0053fe18` |

Only the Main descriptor is flagged compressed; the ROM's own LZ
decompressor handles Nintendo's bytes, so the host streams them unchanged.
The execute address `0x00927000` is `stub.code.dst + 0x400000`, which is
GBATEK's AR6014 "BMI_DONE entry".

The **host-interest struct is at `0x00520000`**.  `host_interest_s` is only
`0x100` bytes, and three independent sources agree:

* **GBATEK** (*Host Interest Area in RAM* + the AR60xx chip table): AR6013/AR6014
  RAM Host Interest = `520000h` (AR6002 = `500400h`, AR6003 = `540600h`).
* **3dbrew FIRM**: the Atheros config struct copied to ARM11 memory has
  "Atheros RAM Vars/Host Interest address" = **0x520000 on 3DS** (RAM base
  0x520000, size 0x20000).
* **DSi AR6013 firmware header**: `Part 2 RAM vars/base/size = (00520000h,
  00520000h, 00020000h)`.

(Values near `0x520400` found by scanning the NWM blocks are coincidences:
the Main blocks are LZ-compressed payloads, not raw Xtensa images.)

The AR6002-family bring-up differs from AR6003 in ways that matter:
sleep-clocks disabled, `CPU_CLOCK_STANDARD` at **40/44 MHz** (not 80/88),
`LPO_CAL_ENABLE` gated on `hi_ext_clk_detected`, **no** analog-PLL write, and
the AR6002 SDK leaves board data to the eeprom stub (the host must **not**
claim `hi_board_data`).

## 5. Board data and MAC

The Wi-Fi **calibration and MAC are not in NWM** and not in the config
savegame (that only holds the three saved AP profiles,
`BlkID 0x00080000..2`).  They live in the Wi-Fi module's **I2C EEPROM**
(`300h` bytes used, DSi board-data format), which the boot stub reads itself
before the Main image runs.  Consequences when bring-up fails:

* a non-zero return from the stub's `BMI_EXECUTE`, or `hi_board_data = 0x0`
  in the pre-upload dump, means the target really cannot read its EEPROM;
* the fallback is to supply a board-data blob built from a dumped module
  EEPROM (or a synthetic MAC, for association tests only).

## 6. Android side

The driver is built into the kernel, so CM7's stock
`libhardware_legacy` Wi-Fi HAL would fail its `insmod wlan.ko` and show
"Failed to load Wi-Fi driver".  `port/scripts/fix-cm7-wifi.py` teaches the
HAL to treat an existing `wlan0` as "driver loaded" (no insmod/rmmod), and
`CFG80211_WEXT` gives the CM7 `wpa_supplicant_6` "wext" backend its API.
The supplicant and `dhcpcd` configs ship with the device tree
(`port/userland/device/nintendo3ds/wifi/`).

## 7. Status and what to look at

**Verified working:** Wi-Fi power GPIO → SDIO enumeration (`mmc0: new SDIO
card`, `vendor=0x0271 device=0x0201`), target identification
(`ver=0x2300006f type=0x2`), the NWM carve → convert → initramfs pipeline,
and a full BMI upload of Stub + Type4 Main + Database.  **Not yet working:
station mode** — association is the open item, which is why the README lists
Wi-Fi as "still being brought up".

The initramfs boot log contains a `--- wifi (SDIO Atheros AR6014G) ---` block
(`/sys/class/mmc_host`, `/sys/bus/sdio/devices/*` vendor/device/class, the
matching dmesg lines, and which NWM files were found).  On a failed bring-up
the useful evidence is:

* `wifi-enable` GPIO / reset lines from `3ds-sdhc` — did the chip power up at
  all;
* `new SDIO card` + target info — did BMI answer;
* how far the upload sequence gets (`BMI_EXECUTE` return, `BMI_LZ_DATA`,
  `BMI_DONE`) and whether HTC/WMI ever becomes ready;
* the `hi_*` fields (host-interest base, `hi_app_host_interest`,
  `hi_board_data`, `hi_refclk_hz`) — the struct must be written at
  `0x00520000`; writes at any 0x400-shifted address land in the firmware's
  data area and break exactly the steps between "upload finished" and "HTC
  ready";
* `cfg80211: regulatory.db` load errors — harmless here (no userhelper), the
  chip is single-domain 2.4 GHz anyway.

*Historical note:* `port/scripts/fix-wifi.py`, `fix-ath6kl.py` and
`fix-ath6kl-diag.py` belong to an earlier attempt that extended mainline
`ath6kl` with the AR6002 family.  They are kept for reference but are **not**
run by `apply-kernel-port.sh`; do not extend them.
