# Building Cyano3DS

Cyano3DS is assembled from four pieces: the Linux kernel, the ARM9/ARM11
firmware, the CyanogenMod 7.2 userspace and the initramfs/boot stage. The
scripts live in `port/scripts/`.

The build runs on Linux (Ubuntu). **Both checkouts — this repo and the
kernel tree — must live on a Linux filesystem**, not on NTFS — a checkout on
`/mnt/c` fails because the kernel contains files whose names (for example
`aux.c`) are reserved on Windows. Keep everything under `~/...`.

## 0. Layout and prerequisites

Pick these directories and use them for every step below. Every script takes
its paths from the environment or its arguments, and the defaults agree:
kernel, loader and initramfs artifacts go to `$HOME/p3ds/dist` unless you pass
an argument, and the CM7 packaging and card staging read `<repo>/out`. Pass
`<repo>/out` explicitly for the kernel, loader and initramfs when you want
`mksd.sh` to find them.

```bash
REPO=~/cyano3ds               # this checkout
KDIR=~/p3ds/src/linux-3ds     # pristine linux-3ds kernel clone (see step 2)
SRCDIR=~/p3ds/src             # small linux-3ds repos live here (see step 3)
OUT=$REPO/out                 # every build artifact lands here
CM7_DIR=~/p3ds/cm7            # CM7.2 source tree (step 4); about 90 GB with out/
LOCAL=~/.cache/cm7-rootless   # Ubuntu 12.04 chroot + tools (step 4); about 4 GB
```

**Nothing in the build needs root.** The only root step is installing the host
packages below with `apt`. The CM7 chroot runs under `proot` (fake root) and
its first debootstrap stage under `fakeroot`, so `out/` and the source tree
stay owned by your user. `port/scripts/cm7-rootless.sh` does the whole CM7
flow, including `run 'cmd'` for a command in the chroot. The old sudo-based
`cr-run.sh` is removed, and `setup-chroot.sh` is a thin wrapper around it. After a WSL restart, nothing
needs re-mounting: the bind mounts are made per command.

Run `bash port/scripts/check-deps.sh <stage>` before a stage to see every
missing tool with its apt package name. The stage names are `kernel`,
`loader`, `initramfs`, `sd` and `cm7`.

Rough build times: kernel minutes (longer with a cold ccache), FIRM and
initramfs a few minutes each, CM7.2 packaging a few minutes. A full CM7.2
*compile* from a fresh sync takes hours (`make droidcore -j2`) — the flow
scripts are idempotent, so re-runs after a pull only rebuild what changed.

Host packages (Ubuntu):

```bash
sudo apt-get install -y --no-install-recommends \
    build-essential bc bison flex libssl-dev libncurses-dev libelf-dev \
    device-tree-compiler u-boot-tools \
    gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi libgcc-13-dev-armel-cross \
    gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi \
    qemu-user-static \
    pkg-config unzip zip wget curl cpio rsync kmod zstd lz4 \
    python3 python3-venv dosfstools mtools parted \
    ccache git file xxd e2fsprogs fakeroot proot fuse2fs \
    default-jre-headless
```

- The kernel uses `arm-linux-gnueabi` (armel), matching linux-3ds' own CI.
  On Ubuntu 24.04 the compiler is `gcc-13-arm-linux-gnueabi`, and its builtin
  headers come from `libgcc-13-dev-armel-cross`. Without those headers the
  kernel fails with `<linux/types.h>` errors. `check-deps.sh kernel` reports
  this.
- The bare-metal ARM9/ARM11 firmware uses `arm-none-eabi`.
- `firmtool` produces the `.firm` payload. `build-loader.sh` builds it from
  its git repo (the upstream loader's Dockerfile does the same) into a private
  venv under `$SRCDIR/.venv-firmtool`. The package is not on PyPI under that
  name, so there is nothing to install by hand. Override the source with
  `FIRMTOOL_REPO=`.
- `python3-venv` is required for that venv. The old `--break-system-packages`
  install is gone.
- `default-jre-headless` + `zip` are for `PortHelper` (step 5): without Java
  the initramfs still builds, but the keep-awake helper is skipped and the
  screen will time out on device.

Two files are fetched once into the checkout (they are not in git):

```bash
cd "$REPO"
mkdir -p src tools firmware/ath6k/AR6002/nwm

# 1. The port's Android staging files live in port/android-staging, but
#    apply-kernel-port.sh and mkinitramfs.sh look for them under src/.
#    Link them into place:
ln -sfn ../port/android-staging src/android-staging

# 2. Static armel busybox sources — mkinitramfs.sh builds a static busybox
#    from this tarball automatically on its first run:
wget -P src https://busybox.net/downloads/busybox-1.36.1.tar.bz2

# 3. smali — mkinitramfs.sh assembles port/initramfs/PortHelper.smali with it.
#    The Maven Central jar has no Main-Class manifest and needs its deps,
#    so build a fat executable jar (one-time):
bash port/scripts/build-smali-jar.sh
#    (or manually: wget the smali-2.5.2.jar from Maven Central and repackage
#     with jcommander, guava, antlr, dexlib2, util, stringtemplate)
```

(`src/`, `tools/smali.jar`, `busybox-*.tar.bz2` and `firmware/` are all
build-local; none of them is committed.)

## 1. Sources

| Piece | Where |
| --- | --- |
| `linux-3ds/linux` | <https://github.com/linux-3ds/linux> — the kernel tree |
| `linux-3ds/firm_linux_loader` | <https://github.com/linux-3ds/firm_linux_loader> |
| `linux-3ds/arm9linuxfw` | <https://github.com/linux-3ds/arm9linuxfw> |
| CyanogenMod 7.2 | `repo init -u https://github.com/CyanogenMod/android.git -b gb-release-7.2` |

Clone the kernel once (the loader script clones its own two small repos
itself in step 3):

```bash
mkdir -p "$(dirname "$KDIR")"
[ -d "$KDIR" ] || git clone https://github.com/linux-3ds/linux.git "$KDIR"
```

**If the `git clone` times out** (large repo, flaky network), download the tarball instead and extract it:

```bash
mkdir -p "$(dirname "$KDIR")"
[ -d "$KDIR" ] || {
	curl -L -o /tmp/linux-3ds.tar.gz \
		https://github.com/linux-3ds/linux/archive/refs/heads/master.tar.gz
	tar xzf /tmp/linux-3ds.tar.gz -C "$(dirname "$KDIR")"
	mv "$(dirname "$KDIR")/linux-master" "$KDIR"
}
```

## 2. Kernel

```bash
bash port/scripts/apply-kernel-port.sh "$KDIR"   # patch a pristine linux-3ds tree
bash port/scripts/build-kernel.sh "$KDIR" "$OUT" # -> out/zImage, out/nintendo3ds_ktr.dtb
```

`apply-kernel-port.sh` must run before `build-kernel.sh`: it installs the
port's drivers and patches (ashmem, binder, `/dev/log`, the LCD driver, the
touchscreen driver, the Wi-Fi driver, the DTS changes, …).
`build-kernel.sh` does not copy sources into the tree itself. Both are
idempotent — re-running them after a `git pull` is safe.

The only device tree is `nintendo3ds_ktr.dtb` (New 3DS / New 3DS XL /
New 2DS XL).

## 3. ARM9 firmware and FIRM payload

```bash
bash port/scripts/build-loader.sh "$SRCDIR" "$OUT"
# -> out/Cyano3DS.firm, out/arm9linuxfw.bin
```

This clones `firm_linux_loader` and `arm9linuxfw` into `$SRCDIR` if they are
not there yet, patches them (writable SD for Linux, the 804 MHz clock), and
builds the FIRM payload. `Cyano3DS.firm` loads
`linux/{zImage,nintendo3ds_ktr.dtb,initramfs.cpio.gz,arm9linuxfw.bin}` from
the SD card.

## 4. CyanogenMod 7.2 userspace

CM7.2 does not build on a modern host (make 4.x, Python 3 and JDK 17 all break
it). The working setup is a **Ubuntu 12.04 chroot** with the `repo` tool,
OpenJDK 6, GNU make 3.81, Python 2.7 and gcc 4.6, plus the CM7.2 tree synced
from `gb-release-7.2`. See [docs/CM7.md](CM7.md) for the full recipe and the
build bugs that had to be fixed.

`cm7-rootless.sh` runs every CM7 step without root. Each stage is re-runnable,
and its output goes to `<repo>/out/cm7-logs/<stage>.log`:

```bash
bash port/scripts/cm7-rootless.sh                  # tools, bootstrap, chroot, sync (once; resumes)
bash port/scripts/cm7-rootless.sh port             # device tree, svox staging (re-run after a port update)
bash port/scripts/cm7-rootless.sh build            # lunch + make droidcore -j2 (hours)
bash port/scripts/cm7-rootless.sh package          # out/android/{system,data}.img, out/cm7-init/
bash port/scripts/cm7-rootless.sh run 'cmd'        # any command inside the 12.04 chroot
```

- `port` and `build` are idempotent: re-run `port` after pulling any port
  update, or the build may pick up a stale device tree.
- Run `build` in the foreground of a long-running shell. Background builds
  may be killed when the launching session exits on some systems.
- The target is `droidcore` with `lunch cyanogen_nintendo3ds-userdebug`
  (see [CM7.md](CM7.md)). Without the `lunch` the build produces the generic
  product and `package` cannot find `out/target/product/nintendo3ds/`.
- **Disk.** The chroot and tools need about 4 GB on local disk (debootstrap
  refuses a FUSE target). The source tree plus `out/` need about 90 GB, put in
  `CM7_DIR`. If that drive cannot hold a tree (FUSE mounts and NTFS have no
  exec bits), set `CM7_IMAGE=/path/cm7.img` and `CM7_MNT=/path/mnt`: the script
  creates a sparse ext4 image and mounts it with `fuse2fs -o fakeroot`.
- `JOBS` (repo sync parallelism, default 3) and `BUILD_J` (make, default 2)
  are the knobs. GitHub rate-limits high `JOBS` values.
- The repo sync runs on the host with the current `repo` launcher (Python 3).
  The gb-release-7.2 manifest is a plain manifest and does not need the old
  Python 2 `repo` v2.7, and the chroot has no `repo` tool.

`build-cm7-source.sh` always writes into `<repo>/out` (that is also where
`mkinitramfs.sh` looks for `cm7-init/` in the next step, so no path argument
is needed here). Dropping APKs into `sd-staging/CYANO3DS/` before running it
bakes them into `/system`; the directory is optional and may not exist.

## 5. Initramfs

```bash
bash port/scripts/mkinitramfs.sh "$OUT"   # -> out/initramfs.cpio.gz
```

This builds the boot stage: it compiles the static busybox (step 0) and the
`PortHelper` keep-awake jar (needs Java + `tools/smali.jar`, otherwise it
prints a warning and the screen will time out), starts Android's `init` as a
child (not PID 1), mounts `/system` from the image, seeds `/data`, installs
APKs dropped in `CYANO3DS/` on the card, and keeps a log on the SD card. It
asserts the 8 MiB initramfs size limit that `firm_linux_loader` enforces.

Wi-Fi firmware is optional here: without the blobs from step 6 the script
prints a warning and the build still boots, just without Wi-Fi.

## 6. Assemble the SD card

```bash
bash port/scripts/mksd.sh --with-android     # -> out/sd/
```

`mksd.sh` reads the artifacts from `<repo>/out`, which is why steps 2, 3 and
5 must all be told to write there (`"$OUT"`). `out/sd/` is a directory tree;
copy its contents to the root of the SD card. See
[docs/INSTALL.md](INSTALL.md).

## 7. Firmware (optional — Wi-Fi only)

Cyano3DS ships no Nintendo firmware. A Wi-Fi-capable build needs four blobs
from the console's own **NWM** system module. A build without them works;
only Wi-Fi is missing.

1. Dump the NWM title with GodMode9 using
   `port/gm9/DumpNWMWifi.lua` (it writes the decrypted `.code` to
   `0:/gm9/out/wifi/`).
2. Carve the blocks on the PC (this writes `stub_data.bin`, `stub_code.bin`,
   `database.bin`, `main_type1/4/5.bin` plus `blocks.json` into a
   `<name>.code.blocks/` directory next to the input):

   ```bash
   python3 port/scripts/nwm-extract.py nwm_*.code
   ```

3. Install the four files `mkinitramfs.sh` looks for — either into the
   checkout default or anywhere pointed at by `NWM_DIR`:

   ```bash
   mkdir -p firmware/ath6k/AR6002/nwm
   cp <name>.code.blocks/{stub_data.bin,stub_code.bin,main_type4.bin,database.bin} \
      firmware/ath6k/AR6002/nwm/
   # optional: also copy main_type1.bin alongside for A/B runs
   ```

   They are installed at `/lib/firmware/ath6k/AR6002/nwm/` in the initramfs.
   (`port/scripts/nwm-to-ath6k.py` is a separate converter that produces the
   SDK-style `ath6k/AR6014/` set instead; it is not needed for this step.)

The details, including which NWM revision is needed, are in
[docs/WIFI.md](WIFI.md).

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `apply-kernel-port.sh` fails on `cd .../src` or misses `ashmem-*.c` | Step 0 symlink missing — create `src/android-staging` pointing at `port/android-staging`. |
| `mkinitramfs.sh` cannot find `busybox-1.36.1.tar.bz2` | Step 0 download missing — fetch it into `src/`. |
| `could not build porthelper.jar` warning | Java/smali/zip missing — install `default-jre-headless` + `zip`, fetch `tools/smali.jar` (step 0). The build still completes, but the screen will time out on device. |
| `no .../stub_code.bin (ath6k will fail its firmware load)` | No Wi-Fi blobs — expected without step 7. The build still boots; only Wi-Fi is missing. To silence it, provide `NWM_DIR` or `firmware/ath6k/AR6002/nwm/`. |
| `mksd.sh: missing ... (run the build scripts first)` | The kernel, loader and initramfs artifacts went to `~/p3ds/dist` (the script defaults). Re-run steps 2, 3 and 5 with `"$OUT"` set to `$REPO/out`. |
| Kernel checkout on `/mnt/c` fails with odd filename errors | NTFS limitation — move the checkout and `$KDIR` onto a Linux filesystem (step 0). |
| `debootstrap`: `Release signed by unknown key (key id 40976EAF437D05B5)` | The host's `ubuntu-archive-keyring` no longer trusts the 2012-era precise signing key. Re-add the old key to `/usr/share/keyrings/ubuntu-archive-keyring.gpg` (or use `debootstrap --no-check-gpg`). |
| `repo: error: Python 2 is no longer supported` | The sync must run on the host with Python 3 (`cm7-rootless.sh sync`). Do not run `repo` inside the Python 2 chroot. |
| `dpkg: error: unknown option --add-architecture` | Not used any more: precise's dpkg 1.16 predates it and the 32-bit packages install without it. |
| `No rule to make target external/svox/…` | The CM svox fork is gone from GitHub. Run `port/scripts/stage-svox.sh` after the sync (and after `cleanup-cm7-tree.sh`, which no longer deletes it). |
| `"common_full.mk" does not exist` / `common.mk: No such file` | `cleanup-cm7-tree.sh` used to delete the `common_*.mk`/`themes*.mk` files the product inherits. Fixed — they are kept now. |
| `ld: cannot find -lstdc++` / `-lz` (32-bit host tools) | precise has no 32-bit libstdc++/zlib dev packages. The `chroot` stage of `cm7-rootless.sh` creates the `.so` symlinks. |
| `sh: gperf: not found` | `gperf` is in the chroot's package list now. |
| `PermissionError: …/out/.../build.prop` | Not expected any more: the build runs under `proot -0`, which keeps `out/` owned by your user. If you see it, the tree was created by an older root build; `sudo chown -R $USER $CM7_DIR/out` once. |
| `Don't have a product spec for: 'cyanogen_nintendo3ds'` / builds `generic` | No `lunch` was run. Use `lunch cyanogen_nintendo3ds-userdebug` before `make droidcore`. |
