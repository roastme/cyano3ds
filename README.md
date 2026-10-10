# Cyano3DS

CyanogenMod 7.2 (Android 2.3.7, API 10) running natively on the **New Nintendo
3DS / New 3DS XL / New 2DS XL**.

Cyano3DS is not an emulator and not a Horizon app. `firm_linux_loader` starts a
linux-3ds kernel, and Android's own init, Dalvik, SurfaceFlinger and framework
run on top of it. The Android UI is on the bottom screen; the top screen is the
kernel console.

```
        CyanogenMod 7.2 userspace  (bionic · dalvik · surfaceflinger · software GLES)
        ───────────────────────────────────────────────────────────────────────
        linux-3ds kernel 5.11  +  ashmem  +  mainline binder  +  ctr_lcd_fb
        ───────────────────────────────────────────────────────────────────────
        firm_linux_loader.firm (ARM11)   ·   arm9linuxfw.bin (ARM9, SD + PXI)
        ───────────────────────────────────────────────────────────────────────
        fastboot3DS  →  boot9  →  Nintendo New 3DS hardware
```

It boots as a **FIRM payload** from the SD card (`Cyano3DS.firm`), launched
from fastboot3DS or Luma3DS; nothing on the NAND is written.

## What works

| Area | Status |
| --- | --- |
| Boot | fastboot3DS → FIRM payload → Linux → CM7.2 → ADWLauncher + SystemUI on the bottom screen |
| Display | bottom screen 320x240 RGB565, rotated in the kernel (`ctr_lcd_fb`); software GLES (`libGLES_android`) |
| Input | resistive touchscreen (direct-touch evdev) and the full button set |
| Audio | CSND + TSC2117/AIC3010 codec through CM7's ALSA HAL (mono) |
| Storage | read-only `/system` image; `/data` seeded from the card at boot |
| Battery / RTC | MCU (`BAT0`/`ADP0`) |
| CPU | 804 MHz, all four cores (New 3DS) |
| Wi-Fi | AR6014G driver (pre-mainline staging AR6K) + Nintendo NWM firmware; joining a network is still being brought up |

## Not working yet

- **No GPU driver.** Rendering is software only (`libGLES_android` /
  PixelFlinger).
- **One display.** Android uses the bottom screen; the top screen stays the
  kernel console.
- **No sleep/suspend.** The port keeps the screen awake because there is no
  resume path on this hardware.
- **Wi-Fi station mode** is not finished. See [docs/WIFI.md](docs/WIFI.md).
- **No camera** (CM's camera is a stub), no Bluetooth, GPS, sensors or
  telephony.

The hard limits of the platform are collected in
[docs/SCOPE.md](docs/SCOPE.md).

## Controls

| 3DS | Android |
| --- | --- |
| Touchscreen | Touch |
| D-pad | Navigation |
| A | Select / OK |
| B | Back |
| X / Y | `[` / `]` |
| START | Enter |
| SELECT | Tab |
| L / R | Volume down / up |
| ZL | Menu |
| ZR | Search |
| HOME | Home |

## Installing

Build output is assembled in `out/sd/`. Copy its contents to the root of the
3DS's SD card and boot `Cyano3DS.firm` from fastboot3DS (*Boot setup…* →
slot → pick the payload, or *Boot from file…*).

The full card layout, first-boot expectations and troubleshooting are in
[docs/INSTALL.md](docs/INSTALL.md). **Make a NAND backup first** and keep a
boot slot pointing at your normal `boot.firm`.

## Building

The build runs on Linux or WSL2 (Ubuntu). It needs the ARM cross-toolchains, a
device-tree compiler, `qemu-user-static`, `cpio`, Python 3 and a JDK. Nothing
in the build needs root except installing those packages with `apt`.
`bash port/scripts/check-deps.sh <stage>` lists anything missing, with the
apt package name, before a stage starts.

Stages run in this order. Step 3 (CM7.2) takes hours on the first run and is
resumable. Steps 4 and 5 need its output.

```bash
# 1. kernel: apply the port to a pristine linux-3ds tree and build
bash port/scripts/apply-kernel-port.sh
bash port/scripts/build-kernel.sh           # -> out/zImage, out/nintendo3ds_ktr.dtb

# 2. FIRM payload + ARM9 firmware
bash port/scripts/build-loader.sh           # -> out/Cyano3DS.firm, out/arm9linuxfw.bin

# 3. userspace: CM7.2 in a rootless 12.04 chroot (see docs/BUILD.md step 4)
bash port/scripts/cm7-rootless.sh           # tools, bootstrap, chroot, sync
bash port/scripts/cm7-rootless.sh port build package   # -> out/android/{system,data}.img

# 4. initramfs (needs step 3's out/cm7-init/; ALLOW_NO_ANDROID=1 to skip it)
bash port/scripts/mkinitramfs.sh            # -> out/initramfs.cpio.gz

# 5. assemble the card staging (needs step 3's system.img and data.img)
bash port/scripts/mksd.sh --with-android    # -> out/sd/
```

See [docs/BUILD.md](docs/BUILD.md) for host setup, the CM7 source build and the
firmware the build needs.

### Firmware

Cyano3DS ships no Nintendo firmware. A build needs the four Wi-Fi blobs from
your own console's **NWM** system module; they are dumped with GodMode9 and
carved with `port/scripts/nwm-extract.py`. See [docs/BUILD.md](docs/BUILD.md)
and [docs/WIFI.md](docs/WIFI.md).

## AI disclosure

Cyano3DS is developed with the assistance of AI coding tools. AI-generated
code, patches and documentation are reviewed and tested by the human
maintainer before they are committed; the maintainer is responsible for
everything that ships.

## Credits and license

Cyano3DS's own files are licensed under the Apache License 2.0; kernel code is
GPL-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

It builds on:

- [linux-3ds](https://github.com/linux-3ds) — the kernel port, ARM11 FIRM
  loader and ARM9 SD/PXI firmware;
- [Android3DS / Octoblimp](https://github.com/Octoblimp/Android-Eclair-3DS) —
  an earlier Android port to the same hardware, whose AR6K Wi-Fi driver, SDIO
  host changes and touchscreen driver are reused here;
- CyanogenMod and the Android Open Source Project — the userspace.

Android is a trademark of Google LLC. Nintendo 3DS is a trademark of Nintendo.
This project is not affiliated with or endorsed by either.
