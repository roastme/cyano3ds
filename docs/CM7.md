# CyanogenMod 7.2 on the Nintendo 3DS

Cyano3DS runs **CyanogenMod 7.2** (Android 2.3.7, `PLATFORM_VERSION=2.3.7`,
API level 10) compiled from source for a real `device/nintendo/nintendo3ds`
tree. It boots on real hardware to **ADWLauncher** with SystemUI, Settings and
the stock CM apps.

This document is the technical record of the CM7 port. For the display and
Wi-Fi bring-up, see [DISPLAY.md](DISPLAY.md) and [WIFI.md](WIFI.md); for the
hard limits, [SCOPE.md](SCOPE.md).

## What CM7.2 needs from the platform

CM7 is API 10, 32-bit binder protocol 7, and uses the old `/dev/log` logging
ABI. It still ships the generic renderer path — `libGLES_android` plus the
generic `gralloc.default` — so it needs nothing from the kernel that the base
port does not already provide:

- `/dev/ashmem` and `/dev/binder` (32-bit protocol),
- a double-buffered RGB565 framebuffer that can be flipped,
- evdev input,
- the old `/dev/log` driver (optional but used).

Because of that, **the kernel and FIRM are shared** with the rest of the port;
the CM7 work is almost entirely userspace.

## Device tree and product

- `device/nintendo/nintendo3ds/` (installed by
  `port/scripts/apply-cm7-device.sh`).
- Product: `cyanogen_nintendo3ds`, inheriting `device_nintendo3ds.mk` and
  `vendor/cyanogen/products/common_full.mk` (ADWLauncher, CMParts, DSPManager,
  FileManager, …), and deliberately **not** `gsm.mk` (the 3DS has no modem).
- CPU variant: `armv6-vfp` / `armeabi-v6l`, matching msm7k-class devices such
  as Samsung *cooper*.
- Graphics: no `egl.cfg`, so `libEGL` uses the default `"android"` tag, i.e.
  `libGLES_android.so`.

## Building

CM7.2 will not build on a modern host — make 4.x, Python 3 and JDK 17 all
break it. The working recipe is an **Ubuntu 12.04 chroot** with OpenJDK 6, GNU
make 3.81, Python 2.7 and gcc 4.6:

```bash
# inside the chroot
repo init -u https://github.com/CyanogenMod/android.git -b gb-release-7.2
repo sync
bash /path/to/port/scripts/apply-cm7-device.sh /path/to/cm7
cd /path/to/cm7 && . build/envsetup.sh && make droidcore -j2

# back on the host
CM7_DIR=/path/to/cm7 bash port/scripts/build-cm7-source.sh
bash port/scripts/mkinitramfs.sh
bash port/scripts/mksd.sh --with-android
```

The CM fork of `external/svox` was removed from GitHub, so the manifest drops
that project and the AOSP `android-2.3.7_r1` source is staged in its place.

**Use `make droidcore`, not `brunch nintendo3ds`.**  `brunch` / `make bacon`
are guarded by `ifneq ($(TARGET_NO_KERNEL),true)` in `build/core/Makefile`,
and the 3DS `BoardConfig.mk` sets `TARGET_NO_KERNEL := true` (the kernel is
the linux-3ds zImage, not an Android boot.img).  `droidcore` builds
systemimage + ramdisk + userdata without the OTA packaging that a no-kernel
device cannot use.  Run the build in the foreground of a long-running shell —
background builds may be killed when the launching session exits on some systems.

See [BUILD.md](BUILD.md) for the host setup.

## Build problems that cost a failed build

Each of these has made a CM7 build "look like it built" while producing
nothing bootable:

1. **`set -u` breaks CM7's `build/envsetup.sh`.** It reads `LUNCH_MENU_CHOICES`
   before it is set. Do not run the build under `set -u`.
2. **`device/<vendor>/<device>/` is mandatory.** `build/core/config.mk`
   resolves `TARGET_DEVICE` with the glob
   `device/*/$(TARGET_DEVICE)/BoardConfig.mk`, so a flat `device/nintendo3ds/`
   is invisible.
3. **`AndroidBoard.mk` must not re-include the device's `Android.mk`s.**
   `build/core/main.mk` already walks the tree; including them again defines
   every module twice.
4. **No `copybit`/`fbtest` HAL.** The 3DS composites in software with the
   generic gralloc, so the device tree builds no `copybit` HAL.

## Kernel support used by CM7

The kernel is the base port plus the CM7-relevant fixes:

- **ashmem, binder (32-bit), `/dev/log`** — the Android legacy interfaces.
- **`ctr_lcd_fb`** — the bottom-screen RGB565 framebuffer with the portrait
  transpose done in the kernel, so CM7's stock gralloc and `libGLES_android`
  work unmodified. See [DISPLAY.md](DISPLAY.md).
- **`cacheflush`/PAN fix** — without it nothing reaches the panel.
- **evdev input** — touchscreen and buttons.
- **cgroup/SchedPolicy fix** — CM7's `SchedPolicy` setgid storm (`EACCES`) had
  to be fixed in the kernel.
- **MCU battery/RTC** — `BAT0`/`ADP0`, with a poll so the framework notices the
  charger.
- **CSND/I2S2 + TSC2117 audio** — an ALSA PCM card, no DSP firmware needed.
- **AR6014 Wi-Fi** — the pre-mainline AR6K driver booting Nintendo's NWM
  firmware; see [WIFI.md](WIFI.md).

## Graphics

CM7 composites in software. `ctr_lcd_fb` presents a normal double-buffered
RGB565 fbdev with `FBIOPAN_DISPLAY`; the panel's portrait wiring is hidden in
the driver. `libEGL` loads `libGLES_android`, which renders through
PixelFlinger. Expect a usable but not fast UI.

## Audio

The XpertTeak DSP needs Nintendo's signed firmware, so the port does not use
it. The TSC2117/AIC3010 codec has a second I2S input fed by the **CSND** DMA
sound engine; `ctr_snd.c` drives codec and CSND and exposes an **ALSA PCM
card**. CM7's ALSA HAL (`BOARD_USES_ALSA_AUDIO`) then plays through `/dev/snd`.

The CSND channel is mono, so the driver downmixes the interleaved ALSA ring.
Playback is correct-pitch mono.

## Input and the screen

The touchscreen uses the direct-touch evdev driver from Android3DS/Octoblimp
(median of the FIFO samples, `INPUT_PROP_DIRECT`, fixed calibration), with the
circle pad on a separate trackball device. Buttons are mapped through
`qwerty.kl`; A is OK, B is Back, ZL is Menu, ZR is Search and HOME is Home.

CM7's screen-timeout would turn the panel off and then drop all input. A small
helper (`com.cyano3ds.PortHelper`, built as `/bin/porthelper.jar` by
`mkinitramfs.sh`) holds a `SCREEN_BRIGHT_WAKE_LOCK` and
refreshes user activity, which keeps the screen on. A card file
`CYANO3DS/nohelper` disables it.

## First boot

- The boot stage pre-optimises the system apps and saves the result to
  `android/dalvik.tar`, so the first boot is slow and later boots are faster.
- **`/data` is a small tmpfs**, seeded from `data.img`; app data does not
  persist yet. Watch `No space left on device` in the logs.
- CM ships extras that need hardware the 3DS does not have (DSPManager, FM,
  camera, Bluetooth, CMStats/updates, `rild`). They should fail gracefully;
  anything that crash-loops can be removed from `/system/app`.

## Current status

- Boots to ADWLauncher + SystemUI; Settings, Contacts and Calculator run.
- Touch, buttons, audio and the battery indicator work.
- Wi-Fi is being brought up (driver and firmware load; joining a network is
  the remaining piece).
- No GPU, no second Android screen, no sleep, no camera — see
  [SCOPE.md](SCOPE.md).
