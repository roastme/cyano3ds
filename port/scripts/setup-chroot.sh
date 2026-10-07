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

set -euo pipefail

CHROOT="${CHROOT:-/root/p3ds/cm12}"
CM7_DIR="${CM7_DIR:-/root/p3ds/cm7}"
DO_SYNC=1
[ "${1:-}" = "--no-sync" ] && DO_SYNC=0

say() { printf '\n==> %s\n' "$*"; }

# --- 1. debootstrap Ubuntu 12.04 -------------------------------------------
say "1/6  debootstrap Ubuntu 12.04 (precise) into $CHROOT"
if [ -d "$CHROOT" ] && [ -f "$CHROOT/etc/os-release" ]; then
	echo "    $CHROOT already exists — skipping debootstrap"
else
	# debootstrap needs to run on the host (not in a chroot).
	sudo debootstrap --arch=amd64 precise "$CHROOT" \
		http://old-releases.ubuntu.com/ubuntu/
	echo "    debootstrap done"
fi

# Bind mounts for the chroot (some systems tie these to the session — re-make them).
mount --bind /proc     "$CHROOT/proc"     2>/dev/null || true
mount --bind /sys      "$CHROOT/sys"      2>/dev/null || true
mount --bind /dev      "$CHROOT/dev"      2>/dev/null || true
mount --bind /dev/pts  "$CHROOT/dev/pts"  2>/dev/null || true

# Helper: run a command in the chroot with a sanitized PATH.
cr() {
	chroot "$CHROOT" /bin/bash -c \
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
	bc \
	|| echo '    (some packages may have failed — check above)'"

# --- 3. 32-bit support -------------------------------------------------------
say "3/6  install 32-bit support (for the prebuilt arm-eabi-4.4.3 toolchain)"
cr "dpkg --add-architecture i386"
cr "apt-get update -qq"
cr "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
	libc6-i386 libc6-dev-i386 \
	zlib1g:i386 libstdc++6:i386 \
	|| echo '    (some i386 packages may have failed — check above)'"

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
cr "if [ ! -f /usr/local/bin/repo ]; then \
		curl -o /usr/local/bin/repo \
			https://storage.googleapis.com/git-repo-downloads/repo; \
		chmod a+x /usr/local/bin/repo; \
		echo '    repo installed'; \
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
		chroot "$CHROOT" /bin/bash -c \
			"export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; \
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
echo "  3. cr-run.sh 'cd $CM7_DIR && . build/envsetup.sh && make droidcore -j2'"
