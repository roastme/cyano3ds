# Scope and hard limits

Cyano3DS runs CyanogenMod 7.2 (Android 2.3.7, API 10) on the **New Nintendo
3DS family** only. This file lists the things that are *not* going to change,
so they are not mistaken for bugs.

## Hardware

- **No GPU driver.** The 3DS has a PICA200, but there is no Linux driver for
  it. Rendering is software (`libGLES_android` / PixelFlinger), which is why
  the UI is usable but not fast. This is also why later Android releases are
  out of reach (see below).
- **One display.** Android's WindowManager and SurfaceFlinger assume a single
  screen, so Android uses the bottom screen and the top screen stays the kernel
  console. Dual-screen apps or a second Android display would need framework
  work that does not exist.
- **No full-system suspend.** Closing the lid turns the screens off and idles
  the CPU, but the whole-system sleep (`ctr_deep_sleep=1`) does not wake up,
  so it is off by default.
- **CPU.** The New 3DS runs at 804 MHz (ARM11 MPCore, ARMv6K, no NEON). Code
  must be `armeabi` (ARMv5) or `armv6`; `armeabi-v7a` will not run.
- **RAM.** 256 MiB shared with the kernel, framebuffer and DMA buffers.

## Supported console

CyanogenMod 7.2 runs on the **New Nintendo 3DS / New 3DS XL / New 2DS XL**
(`nintendo3ds_ktr.dtb`). The Old 3DS and 2DS (128 MB, one core at 268 MHz)
only have the experimental CyanogenMod 6 build; see [CM6.md](CM6.md).

## Wireless

- **Wi-Fi is 2.4 GHz only** (the AR6014G chip). Open and WPA/WPA2-Personal
  networks; WPA3-only and enterprise (802.1X) networks are not supported.
- **No cellular radio.** The 3DS has none, and Cyano3DS does not simulate one:
  there is no dialer, no SMS/telephony service and no "mobile data" over Wi-Fi.
- **No Bluetooth, GPS or motion sensors.**

## Camera

CM7 builds its stub camera HAL; the working HAL is built
separately and installed from the card. See [CM7.md](CM7.md#camera).

## Android version ceiling

**Gingerbread (API 10) is the last Android release that can work here.**

- Android 4.0 (ICS) and later require an **ARMv7** CPU, a **hardware OpenGL ES
  2.0** GPU (HWUI is the default renderer and `copybit` was removed) and
  **≥340 MB** RAM. The New 3DS is ARMv6K with no GPU driver and
  256 MiB.
- Gingerbread is the last release that still runs on the software GLES 1.x
  stack and the old `/dev/log` logging ABI.

So the app compatibility ceiling is API 10.

## Not a full-system emulator

Cyano3DS chainloads a real Linux kernel, so it needs real hardware with
custom firmware installed. It is not a Horizon-hosted app and it cannot run
under an emulator such as Citra (which emulates Horizon, not a chainloaded
Linux kernel).

## Scope of the repository

This repository carries the port's own code: the kernel patches and drivers,
the device tree, the CM7 and CM6 device trees, the initramfs/boot stage and
the build scripts. The Linux kernel, `firm_linux_loader`, `arm9linuxfw`, the
AOSP and CyanogenMod sources it builds against are fetched separately; see
[BUILD.md](BUILD.md).
