# Installing on a New Nintendo 3DS

Target: **New 3DS / New 3DS XL / New 2DS XL** with **boot9strap** (or another
bootloader) and **fastboot3DS** or **Luma3DS** installed.

Android runs on the **bottom screen**; the top screen stays the Linux console,
where the kernel messages appear.

> **Read this first.** Cyano3DS boots a FIRM payload from the SD card and does
> not write to the NAND. Even so, before you start:
>
> - open the fastboot3DS menu and make a **NAND backup** (*Miscellaneous…* →
>   *Backup NAND*);
> - make sure one boot slot still points at your normal `boot.firm` (Luma3DS).
>   You will use it to get back.

## 1. Assemble the card contents

Build the port (see [BUILD.md](BUILD.md)), then assemble the staging tree:

```bash
bash port/scripts/mksd.sh --with-android     # -> out/sd/
```

`out/sd/` contains:

```
Cyano3DS.firm                      the payload fastboot3DS boots
linux/zImage                         kernel
linux/nintendo3ds_ktr.dtb            device tree (New 3DS / XL / New 2DS XL)
linux/arm9linuxfw.bin                ARM9 firmware
linux/initramfs.cpio.gz              boot stage (must stay below 8 MiB)
android/system.img                   CM7.2 /system (ext4, loop-mounted)
android/data.img                     /data seed
```

## 2. Copy it to the card

Copy the **contents of `out/sd/` to the root of the SD card's FAT32
partition**, merging with what is already there (Luma3DS, the `luma/` folder,
your games). Existing files are left alone.

```
SD root/
├── boot.firm                <- your normal CFW, untouched
├── luma/                    <- existing
├── Cyano3DS.firm          <- new
└── linux/                   <- new
    ├── zImage
    ├── nintendo3ds_ktr.dtb
    ├── arm9linuxfw.bin
    └── initramfs.cpio.gz
```

## 3. Boot it

1. Power off, insert the card, then power on **while holding HOME** to enter
   the fastboot3DS menu.
2. Either:

   **As a boot slot (recommended)** — *Boot setup…* → *Setup [slot N]…* →
   *Select [slot N] firm* → navigate to `/Cyano3DS.firm` and press A →
   *Set [slot N] keycombo* and hold the buttons you want (**hold DOWN (D-pad)**
   is a good choice) → B → B → *Continue boot*.

   **Or ad-hoc** — *Boot from file…* → `/Cyano3DS.firm`.

From then on, hold **DOWN** while powering on to boot Android; power on
normally to get your normal system back.

If fastboot3DS refuses the payload, put the same file in `luma/payloads/` and
hold **START** at power-on to use Luma3DS' chainloader.

## 4. First boot

1. Both screens glitch for a few seconds. The **top screen** becomes the Linux
   console and shows the boot log as it happens. The **bottom screen** stays
   on the display self-test only on diagnostic boots (a file named `diag`
   in `CYANO3DS/` on the card), which also page the boot log there.
2. The boot stage installs any APKs in `CYANO3DS/`, mounts `/system` from
   the image, seeds `/data`, pre-optimises the system apps and starts Android's
   `init` as a child (not PID 1, so a crash cannot panic the kernel).
3. The **bottom screen** becomes the Android UI: ADWLauncher, the status bar
   and the notification shade.

The first boot is slow — Dalvik pre-optimises the whole system and
`PackageManager` scans every package. A couple of minutes is normal. The
result is cached on the card (`android/dalvik.tar`), so later boots are faster.

**Do not power off while the top screen is counting packages.**

## 5. Logs

The boot stage writes to `CYANO3DS/` on the card:

```
CYANO3DS/init.log            what the boot stage did (mounts, APK installs, …)
CYANO3DS/kmsg.log            kernel log, streamed from /dev/kmsg
CYANO3DS/android-log.txt     Android liblog via logdump
CYANO3DS/dmesg-at-init.log   kernel log up to init
CYANO3DS/dmesg-at-end.log    kernel log at the end of the boot
CYANO3DS/lastboot/           the previous boot's logs
```

Power off, put the card in a PC and read them there. To go back to the normal
system: power off, power on holding **HOME**, and pick the slot that points at
`boot.firm`.

## 6. Installing apps

Drop an APK into `CYANO3DS/` on the card and reboot. The boot stage:

1. binds every `CYANO3DS/*.apk` over `/system/app/NAME.apk`;
2. extracts the APK's own bundled `lib/armeabi/*.so` into `/system/lib`;
3. pre-optimises the app so it is ready when the launcher lists it.

A separate `.so` on the card is not needed (a card `.so` still overrides the
one inside the APK). Use `armeabi` (ARMv5/ARMv6) builds — `armeabi-v7a` will
not run on the ARM11.

Apps are installed, not auto-launched: open them from the launcher.

**`/data` is saved to the card.** `/data` runs in RAM, and the boot stage
saves it as `android/data-cm7.tar` (settings, accounts, Wi-Fi networks, app
data) and `android/data-cm7-apps.tar` (apps installed from inside Android).
It saves every 60 s when something changed, before the console sleeps on lid
close, and on a power-button shutdown, and restores both at boot. Holding the
power button to force the console off loses at most the last minute.

The card is also Android's external storage (`/mnt/sdcard`): the Camera saves
to `DCIM/Camera`, and the Gallery and Music apps find files there.

**Sleep:** closing the lid puts the console to sleep; opening it wakes it.
This is a screen-off sleep: the backlights go off, the power LED breathes
and the CPU idles until the lid opens. Wi-Fi stays powered and reconnects if
the access point dropped it. The deeper whole-system sleep (`ctr_deep_sleep=1`
on the kernel command line) is experimental: on hardware it did not wake up.

**Screen timeout:** the screens never turn off on their own (the POWER button
is a clean shutdown here, so nothing could turn them back on). For a timeout,
put it in milliseconds in `CYANO3DS/screen-timeout`, e.g. `120000`.

**Diagnostics:** about five minutes after Android starts, the boot stage writes
`CYANO3DS/selftest.txt`, a PASS/FAIL report on lid sleep, Wi-Fi, audio,
cameras, storage and `/data` saving. The top screen shows Android's warnings
and errors live; put a logcat level letter (`V D I W E F`) in
`CYANO3DS/console-log` to change it, or create `CYANO3DS/noconsolelog` to turn
it off.

## 7. Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Both screens stay dark | Wrong or missing DTB, or the payload never ran — check the file names under `/linux/` |
| Bottom screen never leaves the boot log | Android did not reach SurfaceFlinger; read `CYANO3DS/init.log` and `android-log.txt` |
| Touch lands in the wrong place | Create an empty `CYANO3DS/calibrate` file on the card and boot: tap the five crosses on the bottom screen. |
| An app installs but crashes on launch | It calls APIs newer than API 10, or needs `armeabi-v7a` |
| `UnsatisfiedLinkError` on launch | The APK is missing its `lib/armeabi/*.so` |
| Boot hangs before the initramfs messages | `initramfs.cpio.gz` is larger than 8 MiB, or the card is not readable |
| The screen turns off and input dies | The keep-awake helper did not start; see [CM7.md](CM7.md) §4i |
