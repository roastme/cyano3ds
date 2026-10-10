#!/bin/bash
# cm7-rootless.sh - build CyanogenMod 7.2 for the New 3DS without root.
#
# Replaces setup-chroot.sh (which needs sudo for debootstrap, mount and chroot)
# and cr-run.sh.  Nothing here calls sudo.  The 12.04 chroot runs under proot
# (fake root, ptrace based), and debootstrap's first stage runs under fakeroot.
#
# Usage:
#   bash port/scripts/cm7-rootless.sh                 # every stage, in order
#   bash port/scripts/cm7-rootless.sh tools bootstrap # selected stages
#   bash port/scripts/cm7-rootless.sh run 'cmd'       # a command inside the chroot
#
# Stages (each one is re-runnable):
#   tools      proot, fuse2fs, debootstrap 1.0.81, 2012 Ubuntu keyring
#   image      optional: a sparse ext4 image mounted with fuse2fs (see CM7_IMAGE)
#   bootstrap  Ubuntu 12.04 (precise) into $CHROOT
#   chroot     apt sources, build dependencies (OpenJDK 6, gcc 4.6, python 2.7)
#   sync       CM7.2 gb-release-7.2 source, host repo launcher (Python 3)
#   port       device tree, svox staging (host scripts)
#   build      lunch cyanogen_nintendo3ds-userdebug && make droidcore (in chroot)
#   package    build-cm7-source.sh (host)
#
# Locations (all overridable):
#   CM7_DIR     CM7.2 source tree.  Default: $HOME/p3ds/cm7
#   CM7_IMAGE   optional ext4 image that holds the source tree.  Set this when
#               the source drive cannot hold a tree (FUSE mounts, NTFS: no exec
#               bits, no chmod).  Mounted at CM7_MNT with fuse2fs -o fakeroot.
#   CM7_MNT     mount point for CM7_IMAGE.  Default: $HOME/p3ds/cm7-mnt
#   LOCAL       tools and chroot cache.  Default: $HOME/.cache/cm7-rootless
#               (local disk: debootstrap refuses a FUSE target)
#   JOBS        repo sync parallelism.  Default: 3 (GitHub rate-limits higher)
#   BUILD_J     make -j for droidcore.  Default: 2 (the docs' number)
#   LOGS        per-stage logs.  Default: $REPO/out/cm7-logs

set -euo pipefail

PORT_SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$PORT_SCRIPTS/../.." && pwd)"

CM7_DIR="${CM7_DIR:-$HOME/p3ds/cm7}"
CM7_IMAGE="${CM7_IMAGE:-}"
CM7_MNT="${CM7_MNT:-$HOME/p3ds/cm7-mnt}"
LOCAL="${LOCAL:-$HOME/.cache/cm7-rootless}"
TOOLS="$LOCAL/tools"
CHROOT="$LOCAL/chroot"
LOGS="${LOGS:-$REPO/out/cm7-logs}"
JOBS="${JOBS:-3}"
BUILD_J="${BUILD_J:-2}"

# The source tree is bind-mounted at /cm7 inside the chroot, and the repo at
# /port, so the build sees the same files the host scripts edit.
if [ -n "$CM7_IMAGE" ]; then
	SRC_ROOT="$CM7_MNT/cm7-src"
else
	SRC_ROOT="$CM7_DIR"
fi

LIBDIR="$TOOLS/usr/lib/x86_64-linux-gnu"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
PROOT="$TOOLS/usr/bin/proot"
FUSE2FS="$TOOLS/usr/bin/fuse2fs"
DBS="$TOOLS/dbs/usr/sbin/debootstrap"
export DEBOOTSTRAP_DIR="$TOOLS/dbs/usr/share/debootstrap"
KEYRING="$TOOLS/kr/usr/share/keyrings/ubuntu-archive-keyring.gpg"
UBUNTU_MIRROR="http://old-releases.ubuntu.com/ubuntu"
REPO_LAUNCHER_URL="https://storage.googleapis.com/git-repo-downloads/repo"
CM_MANIFEST="https://github.com/CyanogenMod/android.git"

say() { echo; echo "==> $*"; }
die() { echo "FATAL: $*" >&2; exit 1; }

# Downloads retry: the old-releases mirror drops connections (curl exit 52).
fetch() {
	local url="$1" out="$2" i
	[ -s "$out" ] && return 0
	for i in 1 2 3 4 5 6; do
		curl -fsSL --retry 2 -o "$out.part" "$url" && { mv "$out.part" "$out"; return 0; }
		echo "    download failed (try $i), retrying in $((i * 10))s: $url"
		sleep $((i * 10))
	done
	die "could not download $url"
}

# Run a command inside the 12.04 chroot as fake root.
inchroot() {
	mkdir -p "$TMP_DIR"
	"$PROOT" -0 -r "$CHROOT" -b /proc -b /dev -b "$REPO:/port" -b "$SRC_ROOT:/cm7" \
		-b "$TMP_DIR:/tmp" \
		-w "${WD:-/}" \
		/usr/bin/env -i HOME=/root TERM=xterm LANG=C \
		PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
		/bin/bash -c "$*"
}

TMP_DIR="$SRC_ROOT/../cm7-tmp"

# ---------------------------------------------------------------------------
stage_tools() {
	say "tools: proot, fuse2fs, libtalloc, debootstrap 1.0.81, precise keyring"
	bash "$PORT_SCRIPTS/check-deps.sh" cm7
	mkdir -p "$TOOLS/debs"
	cd "$TOOLS/debs"
	# apt-get download needs no root, only a populated package index.
	for p in fuse2fs proot libtalloc2; do
		ls "$p"_*.deb >/dev/null 2>&1 || apt-get download "$p" \
			|| die "apt-get download $p failed (run: apt-get update)"
	done
	fetch "$UBUNTU_MIRROR/pool/main/d/debootstrap/debootstrap_1.0.81ubuntu3.3_all.deb" \
		debootstrap_1.0.81ubuntu3.3_all.deb
	fetch "$UBUNTU_MIRROR/pool/main/u/ubuntu-keyring/ubuntu-keyring_2012.05.19_all.deb" \
		ubuntu-keyring_2012.05.19_all.deb

	[ -x "$PROOT" ] || { for d in fuse2fs_*.deb proot_*.deb libtalloc2_*.deb; do
		dpkg -x "$d" "$TOOLS"; done; }
	[ -x "$DBS" ] || { mkdir -p "$TOOLS/dbs" && dpkg -x debootstrap_1.0.81ubuntu3.3_all.deb "$TOOLS/dbs"; }
	[ -f "$KEYRING" ] || { mkdir -p "$TOOLS/kr" && dpkg -x ubuntu-keyring_2012.05.19_all.deb "$TOOLS/kr"; }

	# debootstrap mounts /proc and /sys in the target.  Rootless, proot binds
	# /proc and /dev instead, so those mount calls are skipped.  Idempotent.
	sed -i 's|^\t\tin_target mount -t proc proc /proc$|\t\ttrue  # rootless: proot bind-mounts /proc|; s|^\t\t\tin_target mount -t sysfs sysfs /sys$|\t\t\ttrue|' \
		"$DEBOOTSTRAP_DIR/functions"

	"$PROOT" -V >/dev/null 2>&1 || die "proot does not run (see $TOOLS)"
	echo "    tools ok"
}

# ---------------------------------------------------------------------------
# Optional: keep the source tree in an ext4 image, for a drive that cannot
# hold one directly.  Needs fuse2fs from stage tools.
stage_image() {
	[ -n "$CM7_IMAGE" ] || { echo "    CM7_IMAGE not set: source tree is $CM7_DIR (no image needed)"; return 0; }
	say "image: $CM7_IMAGE mounted with fuse2fs at $CM7_MNT"
	[ -f "$CM7_IMAGE" ] || {
		local avail
		avail=$(df --output=avail -BG "$(dirname "$CM7_IMAGE")" | tail -1 | tr -dc 0-9)
		[ "$avail" -ge 60 ] || die "only ${avail}G free for $CM7_IMAGE (need about 60G)"
		truncate -s "${CM7_IMAGE_SIZE:-120G}" "$CM7_IMAGE"
		mkfs.ext4 -q -F -L cm7 \
			-E root_owner="$(id -u):$(id -g)",lazy_itable_init=1,lazy_journal_init=1 "$CM7_IMAGE"
	}
	mkdir -p "$CM7_MNT"
	if ! grep -q " $CM7_MNT fuse" /proc/mounts; then
		# An image left dirty by a crash refuses a read-write mount.
		e2fsck -fy "$CM7_IMAGE" >/dev/null 2>&1 || true
		# fakeroot: git creates pack and object files with O_RDWR and mode 0444;
		# without this option FUSE refuses the open.  default_permissions does not help.
		"$FUSE2FS" -o rw,fakeroot "$CM7_IMAGE" "$CM7_MNT"
		sleep 1
	fi
	grep -q " $CM7_MNT fuse" /proc/mounts || die "image did not mount at $CM7_MNT"
	touch "$CM7_MNT/.write-test" && rm -f "$CM7_MNT/.write-test"
	mkdir -p "$SRC_ROOT"
	echo "    mounted at $CM7_MNT"
}

# ---------------------------------------------------------------------------
# Rootless fix to the precise initscripts package: its postinst runs `mount`,
# which needs root.  The cached .deb is repacked with a no-op postinst
# (gzip, because precise's dpkg cannot read zst).
fix_initscripts_deb() {
	local marker="$CHROOT/var/lib/cm7-rootless-initscripts"
	[ -f "$marker" ] && return 0
	local deb="$CHROOT/var/cache/apt/archives/initscripts_2.88dsf-13.10ubuntu11_amd64.deb"
	[ -f "$deb" ] || die "no cached initscripts deb at $deb"
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
	say "bootstrap: debootstrap precise into $CHROOT (fakeroot stage 1, proot stage 2)"
	[ -x "$PROOT" ] || die "run the tools stage first"
	if ! grep -q precise "$CHROOT/etc/lsb-release" 2>/dev/null; then
		# Stage 1 only.  fakeroot, not proot: proot's tar/dpkg-deb extraction
		# fails silently on this debootstrap (rc 2 after "Extracting adduser").
		# The chroot lives on local disk: debootstrap refuses a FUSE target.
		mkdir -p "$CHROOT"
		fakeroot /bin/sh "$DBS" --foreign --arch=amd64 \
			--keyring="$KEYRING" precise "$CHROOT" "$UBUNTU_MIRROR"
	fi
	fix_initscripts_deb
	if [ ! -f "$CHROOT/var/lib/cm7-stage2-done" ]; then
		inchroot "/debootstrap/debootstrap --second-stage"
		# debootstrap's own completion check looks for etc/os-release, which
		# precise does not have.  Check dpkg instead.
		inchroot "dpkg --configure -a && test -x /usr/bin/dpkg"
		touch "$CHROOT/var/lib/cm7-stage2-done"
	fi
	echo "    bootstrap ok"
}

# ---------------------------------------------------------------------------
stage_chroot() {
	say "chroot: apt sources, build dependencies"
	[ -f "$CHROOT/var/lib/cm7-stage2-done" ] || die "run the bootstrap stage first"
	cat > "$CHROOT/etc/apt/sources.list" <<-EOF
		deb $UBUNTU_MIRROR precise main restricted universe multiverse
		deb $UBUNTU_MIRROR precise-updates main restricted universe multiverse
		deb $UBUNTU_MIRROR precise-security main restricted universe multiverse
	EOF
	# proot has no real uids for the apt sandbox user.
	mkdir -p "$CHROOT/etc/apt/apt.conf.d"
	echo 'APT::Sandbox::User "root";' > "$CHROOT/etc/apt/apt.conf.d/99cm7-rootless"
	cp /etc/resolv.conf "$CHROOT/etc/resolv.conf"

	inchroot "apt-get update -qq"
	inchroot "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq apt"
	inchroot "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
		openjdk-6-jdk make gcc g++ python python-dev \
		git-core curl wget zip unzip bison flex libssl-dev libncurses-dev bc gperf"
	inchroot "DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
		libc6-i386 libc6-dev-i386 zlib1g:i386 libstdc++6:i386"
	inchroot "ln -sf /usr/lib/i386-linux-gnu/libstdc++.so.6 /usr/lib/gcc/x86_64-linux-gnu/4.6/32/libstdc++.so 2>/dev/null || true"
	inchroot "ln -sf /lib/i386-linux-gnu/libz.so.1 /lib/i386-linux-gnu/libz.so 2>/dev/null || true"
	inchroot "[ -e /usr/include/asm ] || ln -s x86_64-linux-gnu/asm /usr/include/asm"

	# No repo tool in the chroot: the sync runs on the host (stage sync).
	inchroot "git config --global user.name cm7-build; git config --global user.email cm7-build@localhost; git config --global color.ui false"
	echo "    chroot ok"
}

# ---------------------------------------------------------------------------
stage_sync() {
	# Runs on the host, not under proot.  git writes pack files with mode 0444
	# and then writes to them; proot's permission emulation refuses that.  The
	# gb-release-7.2 manifest is a plain manifest, so the current repo launcher
	# (Python 3) reads it fine.  The chroot's Python 2 repo is not needed here.
	say "sync: CM7.2 gb-release-7.2 into $CM7_DIR (host git, host python3)"
	bash "$PORT_SCRIPTS/check-deps.sh" cm7
	REPO_LAUNCHER="$LOCAL/repo"
	fetch "$REPO_LAUNCHER_URL" "$REPO_LAUNCHER"
	chmod +x "$REPO_LAUNCHER"
	mkdir -p "$SRC_ROOT"
	mkdir -p "$SRC_ROOT/.repo/local_manifests"
	cp "$REPO/port/cm7-local-manifests/remove-broken.xml" "$SRC_ROOT/.repo/local_manifests/"
	# Commit identity for repo's own commits, without touching the host git config.
	export GIT_AUTHOR_NAME=cm7-build GIT_AUTHOR_EMAIL=cm7-build@localhost \
		GIT_COMMITTER_NAME=cm7-build GIT_COMMITTER_EMAIL=cm7-build@localhost
	cd "$SRC_ROOT"
	if [ ! -d .repo/manifests ]; then
		python3 "$REPO_LAUNCHER" init --no-repo-verify -u "$CM_MANIFEST" -b gb-release-7.2
	fi
	# GitHub resets connections and rate-limits parallel fetches.  repo resumes
	# finished projects, so a retry only fetches what is left.  The back-off
	# grows so a retry does not deepen the rate limit (complaint C-32).
	local i wait
	for i in 1 2 3 4 5 6 7 8; do
		echo "    repo sync attempt $i of 8 (-j$JOBS)"
		if python3 "$REPO_LAUNCHER" sync -j"$JOBS" -c --no-tags --no-clone-bundle; then
			echo "    sync ok"
			return 0
		fi
		wait=$((i * 60))
		echo "    sync failed (attempt $i), waiting ${wait}s before retrying"
		sleep "$wait"
	done
	die "repo sync did not finish after 8 attempts (re-run this stage: it resumes)"
}

# ---------------------------------------------------------------------------
stage_port() {
	say "port: device tree, svox staging (host scripts)"
	bash "$PORT_SCRIPTS/cleanup-cm7-tree.sh" "$SRC_ROOT"
	bash "$PORT_SCRIPTS/apply-cm7-device.sh" "$SRC_ROOT"
	bash "$PORT_SCRIPTS/stage-svox.sh" "$SRC_ROOT"
	echo "    port ok"
}

# ---------------------------------------------------------------------------
stage_build() {
	say "build: lunch cyanogen_nintendo3ds-userdebug && make droidcore -j$BUILD_J (hours)"
	[ -f "$SRC_ROOT/build/envsetup.sh" ] || die "no CM7 tree at $SRC_ROOT (run sync)"
	mkdir -p "$TMP_DIR"
	WD=/cm7 inchroot ". build/envsetup.sh && lunch cyanogen_nintendo3ds-userdebug && make droidcore -j$BUILD_J"
	echo "    build ok"
}

# ---------------------------------------------------------------------------
stage_package() {
	say "package: build-cm7-source.sh on the host"
	CM7_DIR="$SRC_ROOT" bash "$PORT_SCRIPTS/build-cm7-source.sh"
	echo "    package ok. Next: bash $PORT_SCRIPTS/mkinitramfs.sh"
	echo "                      bash $PORT_SCRIPTS/mksd.sh --with-android"
}

# ---------------------------------------------------------------------------
if [ "${1:-}" = "run" ]; then
	shift
	[ -d "$CHROOT" ] || die "no chroot at $CHROOT (run the bootstrap stage)"
	WD=/cm7 inchroot "$*"
	exit $?
fi

mkdir -p "$LOGS" "$LOCAL"
ALL="tools image bootstrap chroot sync port build package"
STAGES="${*:-$ALL}"
for s in $STAGES; do
	log="$LOGS/$s.log"
	echo "### stage $s (log: $log)"
	# Not inside an `if`: bash ignores set -e there, so a failing step would
	# be swallowed.  The ERR trap names the stage instead.
	CUR_STAGE="$s"
	trap 'echo "FATAL: stage $CUR_STAGE failed (full output: $LOGS/$CUR_STAGE.log)" >&2' ERR
	"stage_$s" 2>&1 | tee -a "$log"
	trap - ERR
done
echo; echo "all requested stages done"
