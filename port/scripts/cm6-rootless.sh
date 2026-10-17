#!/bin/bash
# cm6-rootless.sh - build CyanogenMod 6 (Android 2.2 Froyo, froyo-stable) for
#                   the OLD Nintendo 3DS / 2DS without root.
#
# Froyo's host tools are 32-bit only (no 64-bit host support, no -m32 fixups),
# so unlike cm7-rootless.sh this uses an i386 Ubuntu 12.04 chroot.  The x86_64
# kernel runs i386 binaries natively (CONFIG_IA32_EMULATION), proot only fakes
# root.  The proot / debootstrap / keyring tools are shared with cm7-rootless.sh.
#
# Usage:
#   bash port/scripts/cm6-rootless.sh                      # tools bootstrap chroot
#   bash port/scripts/cm6-rootless.sh bootstrap chroot     # selected stages
#   bash port/scripts/cm6-rootless.sh run 'cmd'            # a command inside the chroot
#   bash port/scripts/cm6-rootless.sh port build package   # after the sync
#
# Stages: bootstrap chroot port build package.  The source sync is
# port/cm6-manifest/ + `repo sync` (see docs/CM6.md).
#
# Locations (all overridable):
#   CM6_DIR   Froyo source tree.   Default: $BASE/src
#   BASE      Default: <repo>/../cm6
#   LOCAL     chroot + cache.  Default: <repo>/../toolchain/cm6-cache
#   TOOLS     proot, debootstrap, keyring (shared with CM7).  Default: ../toolchain/cm7-cache/tools

set -euo pipefail

PORT_SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$PORT_SCRIPTS/../.." && pwd)"
ROOT="$(cd "$REPO/.." && pwd)"

BASE="${BASE:-$ROOT/cm6}"
CM6_DIR="${CM6_DIR:-$BASE/src}"
LOCAL="${LOCAL:-$ROOT/toolchain/cm6-cache}"
TOOLS="${TOOLS:-$ROOT/toolchain/cm7-cache/tools}"
CHROOT="$LOCAL/chroot"
LOGS="${LOGS:-$REPO/out/cm6-logs}"
BUILD_J="${BUILD_J:-6}"
TMP_DIR="$BASE/tmp"
PRODUCT="${PRODUCT:-cyanogen_ctr-eng}"

LIBDIR="$TOOLS/usr/lib/x86_64-linux-gnu"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
PROOT="$TOOLS/usr/bin/proot"
DBS="$TOOLS/dbs/usr/sbin/debootstrap"
export DEBOOTSTRAP_DIR="$TOOLS/dbs/usr/share/debootstrap"
KEYRING="$TOOLS/kr/usr/share/keyrings/ubuntu-archive-keyring.gpg"
UBUNTU_MIRROR="http://old-releases.ubuntu.com/ubuntu"

say() { echo; echo "==> $*"; }
die() { echo "FATAL: $*" >&2; exit 1; }

# Run a command inside the i386 12.04 chroot as fake root.  linux32 makes
# uname report i686, which Froyo's build/core/envsetup.mk keys on.
inchroot() {
	mkdir -p "$TMP_DIR"
	"$PROOT" -0 -r "$CHROOT" -b /proc -b /dev -b "$REPO:/port" -b "$CM6_DIR:/cm6" \
		-b "$TMP_DIR:/tmp" \
		-w "${WD:-/}" \
		/usr/bin/env -i HOME=/root TERM=xterm LANG=C \
		PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
		/usr/bin/linux32 /bin/bash -c "$*"
}

# ---------------------------------------------------------------------------
fix_initscripts_deb() {
	local marker="$CHROOT/var/lib/cm6-rootless-initscripts"
	[ -f "$marker" ] && return 0
	local deb
	deb=$(ls "$CHROOT"/var/cache/apt/archives/initscripts_*_i386.deb 2>/dev/null | head -1)
	[ -n "$deb" ] || die "no cached initscripts deb under $CHROOT/var/cache/apt/archives"
	local work="$LOCAL/work-initscripts"
	rm -rf "$work"
	dpkg-deb -R "$deb" "$work"
	printf '#!/bin/sh\n# rootless build: init-script setup calls mount, which needs root\nexit 0\n' \
		> "$work/DEBIAN/postinst"
	chmod 755 "$work/DEBIAN/postinst"
	dpkg-deb -Zgzip -b "$work" "$LOCAL/initscripts-rootless.deb" >/dev/null
	mv "$LOCAL/initscripts-rootless.deb" "$deb"
	touch "$marker"
	echo "    repacked initscripts deb"
}

stage_bootstrap() {
	say "bootstrap: debootstrap precise (i386) into $CHROOT"
	[ -x "$PROOT" ] || die "no proot at $PROOT (run cm7-rootless.sh tools first)"
	if ! grep -q precise "$CHROOT/etc/lsb-release" 2>/dev/null; then
		mkdir -p "$CHROOT"
		fakeroot /bin/sh "$DBS" --foreign --arch=i386 \
			--keyring="$KEYRING" precise "$CHROOT" "$UBUNTU_MIRROR"
	fi
	fix_initscripts_deb
	if [ ! -f "$CHROOT/var/lib/cm6-stage2-done" ]; then
		inchroot "/debootstrap/debootstrap --second-stage"
		inchroot "dpkg --configure -a && test -x /usr/bin/dpkg"
		touch "$CHROOT/var/lib/cm6-stage2-done"
	fi
	echo "    bootstrap ok"
}

# ---------------------------------------------------------------------------
stage_chroot() {
	say "chroot: apt sources, Froyo build dependencies"
	[ -f "$CHROOT/var/lib/cm6-stage2-done" ] || die "run the bootstrap stage first"
	cat > "$CHROOT/etc/apt/sources.list" <<-EOF
		deb $UBUNTU_MIRROR precise main restricted universe multiverse
		deb $UBUNTU_MIRROR precise-updates main restricted universe multiverse
		deb $UBUNTU_MIRROR precise-security main restricted universe multiverse
	EOF
	mkdir -p "$CHROOT/etc/apt/apt.conf.d"
	echo 'APT::Sandbox::User "root";' > "$CHROOT/etc/apt/apt.conf.d/99cm6-rootless"
	cp /etc/resolv.conf "$CHROOT/etc/resolv.conf"

	inchroot "apt-get update -qq"
	inchroot "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq apt"
	# gcc-4.4: Froyo's host code predates gcc 4.6's stricter warnings/-Werror.
	inchroot "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
		openjdk-6-jdk make gcc-4.4 g++-4.4 gcc g++ python python-dev \
		git-core curl wget zip unzip bison flex gperf libncurses5-dev zlib1g-dev \
		libssl-dev bc libxml2-utils xsltproc lzma"
	# Froyo wants gcc/g++ 4.4 as the host compiler.
	inchroot "ln -sf /usr/bin/gcc-4.4 /usr/local/bin/gcc; ln -sf /usr/bin/g++-4.4 /usr/local/bin/g++; \
		ln -sf /usr/bin/gcc-4.4 /usr/local/bin/cc; ln -sf /usr/bin/g++-4.4 /usr/local/bin/c++"
	inchroot "git config --global user.name cm6-build; git config --global user.email cm6-build@localhost; git config --global color.ui false"
	inchroot "gcc --version | head -1; java -version 2>&1 | head -1; python --version 2>&1; make --version | head -1; uname -m"
	echo "    chroot ok"
}

# ---------------------------------------------------------------------------
stage_port() {
	say "port: Nintendo (old) 3DS device tree into $CM6_DIR"
	bash "$PORT_SCRIPTS/apply-cm6-device.sh" "$CM6_DIR"
}

stage_build() {
	say "build: lunch $PRODUCT && make -j$BUILD_J (hours)"
	[ -f "$CM6_DIR/build/envsetup.sh" ] || die "no CM6 tree at $CM6_DIR (run the sync)"
	WD=/cm6 inchroot ". build/envsetup.sh && lunch $PRODUCT && make -k -j$BUILD_J"
	echo "    build ok"
}

stage_package() {
	say "package: build-cm6-source.sh on the host"
	CM6_DIR="$CM6_DIR" bash "$PORT_SCRIPTS/build-cm6-source.sh"
}

# ---------------------------------------------------------------------------
if [ "${1:-}" = "run" ]; then
	shift
	[ -d "$CHROOT" ] || die "no chroot at $CHROOT (run the bootstrap stage)"
	WD=/cm6 inchroot "$*"
	exit $?
fi

mkdir -p "$LOGS" "$LOCAL"
STAGES="${*:-bootstrap chroot}"
for s in $STAGES; do
	log="$LOGS/$s.log"
	echo "### stage $s (log: $log)"
	CUR_STAGE="$s"
	trap 'echo "FATAL: stage $CUR_STAGE failed (full output: $LOGS/$CUR_STAGE.log)" >&2' ERR
	"stage_$s" 2>&1 | tee -a "$log"
	trap - ERR
done
echo; echo "all requested stages done"
