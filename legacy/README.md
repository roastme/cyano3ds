# legacy/

Build scripts for the earlier userspace flavours of this port.  They are kept
for reference and are **not** part of the current CyanogenMod 7.2 port.

| script | what it built |
| --- | --- |
| `build-android.sh` | Android 1.6 (Donut), from the prebuilt SDK `system.img` |
| `build-froyo.sh` | Android 2.2 (Froyo / FRF91), from the prebuilt SDK image |
| `build-gingerbread.sh` | Android 2.3.3 (Gingerbread / GRI34), from the SDK image |
| `build-cm7.sh` | CyanogenMod 7.2 from a kanged `cm-7.2.0-cooper.zip` |
| `apply-userland-port.sh` | install `device/nintendo3ds/` into an AOSP/Donut tree |
| `yaffs2-extract.py` | unpack a yaffs2 `system.img` (Android SDK images) |

The current userspace is built from source by
`port/scripts/build-cm7-source.sh`; `port/scripts/mkinitramfs.sh`
then packs the boot stage for whichever flavour is on the card.  These scripts
still use `$REPO/port` and `$REPO/src`, so they run from here unchanged.
