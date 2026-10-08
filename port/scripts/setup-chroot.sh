#!/bin/bash
# setup-chroot.sh - create the Ubuntu 12.04 chroot that CM7.2 builds in.
#
# CM7.2 does not build on a modern host (make 4.x, Python 3, JDK 17 all break
# it).  The working recipe is an Ubuntu 12.04 chroot with OpenJDK 6, GNU make
# 3.81, Python 2.7 and gcc 4.6, plus 32-bit support for the prebuilt
# `arm-eabi-4.4.3` toolchain and the host tools (which are i386).
#
# This script is idempotent — safe to re-run.  It:
#   1. debootstrap's Ubuntu 12.04 (precise) into $CHROOT
#   2. installs the build dependencies (OpenJDK 6, make 3.81, gcc 4.6, python2.7)
#   3. installs 32-bit support (libc6-i386, libc6-dev-i386, zlib1g:i386, libstdc++6:i386)
#   4. symlinks /usr/include/asm -> x86_64-linux-gnu/asm (precise has no linux-libc-dev-i386)
#   5. installs the `repo` tool
#   6. syncs the CM7.2 source (gb-release-7.2)
#
# Usage:
#   bash port/scripts/setup-chroot.sh              # full setup + sync
#   bash port/scripts/setup-chroot.sh --no-sync    # chroot only, skip the sync
#
# After this, build with:
#   cr-run.sh 'cd /root/p3ds/cm7 && . build/envsetup.sh && make droidcore -j2'
#
# (cr-run.sh is the chroot helper — it re-makes the bind mounts and sanitizes
# PATH.  See docs/BUILD.md.)
#
# debootstrap, mount and chroot need root, so those steps go through the
# root() helper below — the script works as a normal user (it shells out to
# sudo) and as root.

set -euo pipefail

CHROOT="${CHROOT:-$HOME/p3ds/cm12}"
CM7_DIR="${CM7_DIR:-$CHROOT/root/p3ds/cm7}"
DO_SYNC=1
[ "${1:-}" = "--no-sync" ] && DO_SYNC=0

say() { printf '\n==> %s\n' "$*"; }

# Run a command as root — the chroot steps (debootstrap, mount, chroot)
# need it.  As a normal user this shells out to sudo; as root it is a no-op.
root() {
	if [ "$(id -u)" -eq 0 ]; then
		"$@"
	else
		sudo "$@"
	fi
}

# --- 1. debootstrap Ubuntu 12.04 -------------------------------------------
say "1/6  debootstrap Ubuntu 12.04 (precise) into $CHROOT"
if [ -d "$CHROOT" ] && [ -f "$CHROOT/etc/os-release" ]; then
	echo "    $CHROOT already exists — skipping debootstrap"
else
	# debootstrap needs to run on the host (not in a chroot).
	root debootstrap --arch=amd64 precise "$CHROOT" \
		http://old-releases.ubuntu.com/ubuntu/
	echo "    debootstrap done"
fi

# Bind mounts for the chroot (some systems tie these to the session — re-make
# them; a WSL restart loses them).  mountpoint -q keeps this idempotent and
# a real failure aborts loudly instead of being swallowed.
for m in proc sys dev; do
	mountpoint -q "$CHROOT/$m" || root mount --bind "/$m" "$CHROOT/$m"
done
mountpoint -q "$CHROOT/dev/pts" || root mount --bind /dev/pts "$CHROOT/dev/pts"

# Helper: run a command in the chroot with a sanitized PATH.
cr() {
	root chroot "$CHROOT" /bin/bash -c \
		"export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; $*"
}

# --- 2. build dependencies ---------------------------------------------------
say "2/6  install build dependencies (OpenJDK 6, make 3.81, gcc 4.6, python2.7)"
cr "apt-get update -qq"
cr "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
	openjdk-6-jdk \
	make gcc g++ \
	python python-dev \
	git-core curl wget zip unzip \
	bison flex libssl-dev libncurses-dev \
	bc gperf \
	|| echo '    (some packages may have failed — check above)'"

# --- 3. 32-bit support -------------------------------------------------------
say "3/6  install 32-bit support (for the prebuilt arm-eabi-4.4.3 toolchain)"
# NB: no `dpkg --add-architecture i386` here.  precise ships dpkg 1.16, which
# predates that flag (added in dpkg 1.17.2), and the script dies on it under
# `set -e`.  It is also unnecessary: apt installs the i386 packages fine
# without it.
cr "apt-get update -qq"
cr "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
	libc6-i386 libc6-dev-i386 \
	zlib1g:i386 libstdc++6:i386 \
	|| echo '    (some i386 packages may have failed — check above)'"
# precise has no 32-bit libstdc++/zlib *dev* packages, so the unversioned .so
# symlinks the linker needs for the 32-bit host tools (aidl, aapt, ...) do
# not exist.  Point them at the 32-bit runtimes — the same class of
# workaround as the /usr/include/asm symlink in step 4.
cr "ln -sf /usr/lib/i386-linux-gnu/libstdc++.so.6 /usr/lib/gcc/x86_64-linux-gnu/4.6/32/libstdc++.so 2>/dev/null || true"
cr "ln -sf /lib/i386-linux-gnu/libz.so.1 /lib/i386-linux-gnu/libz.so 2>/dev/null || true"

# --- 4. /usr/include/asm symlink --------------------------------------------
say "4/6  symlink /usr/include/asm -> x86_64-linux-gnu/asm"
# precise has no linux-libc-dev-i386; the x86_64 asm headers work for 32-bit x86.
cr "if [ ! -e /usr/include/asm ]; then \
		ln -s x86_64-linux-gnu/asm /usr/include/asm; \
		echo '    symlinked /usr/include/asm'; \
	else \
		echo '    /usr/include/asm already exists'; \
	fi"

# --- 5. repo tool -----------------------------------------------------------
say "5/6  install the repo tool"
# The current repo launcher requires Python 3, but this chroot is Python 2.7.
# v2.7 is the last Python-2-compatible release, so fetch that tag from the
# canonical git-repo repo (the storage.googleapis.com copy is always the
# latest, which refuses to run here).  REPO_REV (step 6) keeps the launcher's
# self-bootstrap from upgrading itself to a Python-3-only version.
cr "if [ ! -f /usr/local/bin/repo ]; then \
		curl -fsSL 'https://gerrit.googlesource.com/git-repo/+/refs/tags/v2.7/repo?format=TEXT' \
			| base64 -d > /usr/local/bin/repo; \
		chmod a+x /usr/local/bin/repo; \
		echo '    repo v2.7 installed (last Python-2-compatible release)'; \
	else \
		echo '    repo already installed'; \
	fi"

# --- 6. sync CM7.2 source ---------------------------------------------------
if [ "$DO_SYNC" = 1 ]; then
	say "6/6  sync CM7.2 source (gb-release-7.2) into $CM7_DIR"
	if [ -d "$CM7_DIR/.repo" ]; then
		echo "    $CM7_DIR/.repo already exists — skipping sync"
	else
		mkdir -p "$CM7_DIR"
		# Two gb-release-7.2 projects are permanently gone from GitHub and
		# abort the sync: the CM svox fork (DMCA takedown) and
		# android_hardware_ti_wpan (no gb-release-7.2 branch).  Exclude them
		# via a local manifest; AOSP's svox is staged into external/svox
		# afterwards (see stage-svox.sh / docs/BUILD.md).
		mkdir -p "$CM7_DIR/.repo/local_manifests"
		cp "$(dirname "$0")/../cm7-local-manifests/remove-broken.xml" \
			"$CM7_DIR/.repo/local_manifests/"
		cr "export REPO_REV=v2.7; \
			 cd $CM7_DIR && \
			 repo init -u https://github.com/CyanogenMod/android.git -b gb-release-7.2 && \
			 repo sync -j4"
		echo "    sync done"
	fi
else
	say "6/6  skipping CM7.2 sync (--no-sync)"
fi

say "chroot ready: $CHROOT"
echo
echo "Next steps:"
echo "  1. bash port/scripts/cleanup-cm7-tree.sh $CM7_DIR   # remove stale trees"
echo "  2. bash port/scripts/apply-cm7-device.sh $CM7_DIR   # install device tree"
echo "  3. bash port/scripts/stage-svox.sh $CM7_DIR          # stage AOSP svox (CM fork is gone)"
echo "  4. cr-run.sh 'cd $CM7_DIR && . build/envsetup.sh && lunch cyanogen_nintendo3ds-userdebug && make droidcore -j2'"
echo "     (cr-run.sh works as a normal user — it re-makes the bind mounts and"
echo "      self-escalates to root.  Run it once after a WSL restart.)"
echo "  5. sudo chown -R \$USER $CM7_DIR/out   # the build runs as root inside the chroot"
echo "  6. CM7_DIR=$CM7_DIR bash port/scripts/build-cm7-source.sh"
