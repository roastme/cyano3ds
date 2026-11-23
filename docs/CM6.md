# CyanogenMod 6 (Android 2.2 Froyo) for the Old 3DS / 2DS

An experimental source build of CM6 (`froyo-stable`) for the CTR (Old) 3DS
family, which has 128 MB of RAM and one core at 268 MHz. It also boots on a New
3DS, but runs as an Old 3DS there: use the CM7.2 build on New consoles.

## Building

1. Sync: `repo init` against `port/cm6-manifest/default.xml` (a trimmed CM
   `froyo-stable` manifest: device projects dropped, `external/apache-http` and
   `prebuilt` pinned to `android-2.2_r1.3`), then `repo sync -c --no-tags`.
2. `bash port/scripts/cm6-rootless.sh bootstrap chroot port build package`
   (an i386 Ubuntu 12.04 chroot under proot, no root needed; the build takes
   about an hour).
3. `ANDROID_FLAVOR=cm6 bash port/scripts/mkinitramfs.sh out/cm6-dist`, then
   stage the card:

   ```bash
   ANDROID_FLAVOR=cm6 bash port/scripts/mksd.sh out/sd-cm6 --with-android --dist=out/cm6-dist
   ```

   `mksd.sh` picks the CM6 image pair (`out/cm6/android`), the Old 3DS device
   tree (`nintendo3ds_ctr.dtb`) and `Cyano3DS-CM6.firm` by flavor; `--dist=`
   points it at the CM6 initramfs, which is built into its own directory
   because the flavor is baked into `/init`.

The device tree is in `port/userland/device-cm6/ctr/`.

## Known issues

- The home screen shows a "Problem loading widget" toast: a stock widget's
  package is not in the trimmed tree.
- logcat reports `KeyCharacterMap: Can't open keycharmap file`; the
  `qwerty.kcm` install name does not match what Froyo's `BUILD_KEY_CHAR_MAP`
  expects.
- The microphone is silent.
