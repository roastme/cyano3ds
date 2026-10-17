# CyanogenMod 6 (Android 2.2 Froyo) for the old 3DS / 2DS

Source build of CM6 (`froyo-stable`) for the CTR (old) 3DS, 128 MB RAM.

1. Sync: `repo init` against `port/cm6-manifest/default.xml` (a trimmed CM `froyo-stable` manifest: device projects dropped, `external/apache-http` and `prebuilt` pinned to `android-2.2_r1.3`), then `repo sync -c --no-tags`.
2. `bash port/scripts/cm6-rootless.sh bootstrap chroot port build package` (i386 Ubuntu 12.04 chroot under proot, no root needed; the build takes about 1 h).
3. `ANDROID_FLAVOR=cm6 bash port/scripts/mkinitramfs.sh OUTDIR`, then put `out/cm6/android/{system,data}.img` and the usual `linux/` files (old-3DS DTB `nintendo3ds_ctr.dtb`) on the card.

Device tree: `port/userland/device-cm6/ctr/`. Known gaps: see the work note (widget toast, keychar map).
