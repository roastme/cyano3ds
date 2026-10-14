#!/bin/bash
# check-deps.sh - verify host tools before a build stage starts.
#
# Usage: check-deps.sh <stage>...
#   stages: common kernel loader initramfs sd cm7
#
# Lists every missing tool for the requested stages at once, with the apt
# package that provides it on Ubuntu 24.04 / Mint 22, then exits non-zero.  The
# build scripts call this first, so a missing tool is reported before any clone
# or compile starts (complaints C-1, C-4, C-11).

set -uo pipefail

STAGES="${*:-common}"
missing=()
notes=()

need_cmd() {	# need_cmd <command> <apt-package>
	command -v "$1" >/dev/null 2>&1 || missing+=("$1 (apt: $2)")
}

check_common() {
	need_cmd git git
	need_cmd make make
	need_cmd python3 python3
	need_cmd curl curl
	need_cmd gzip gzip
	need_cmd cpio cpio
}

check_kernel() {
	need_cmd bison bison
	need_cmd flex flex
	need_cmd bc bc
	need_cmd dtc device-tree-compiler
	[ -f /usr/include/openssl/ssl.h ] || missing+=("openssl headers (apt: libssl-dev)")
	# A cross-gcc that cannot name its own include directory makes the kernel
	# build fail with a confusing <linux/types.h> error (C-10).
	if command -v arm-linux-gnueabi-gcc >/dev/null 2>&1; then
		inc="$(arm-linux-gnueabi-gcc -print-file-name=include 2>/dev/null)"
		case "$inc" in
			/*) ;;
			*) missing+=("arm-linux-gnueabi gcc builtin headers (apt: libgcc-13-dev-armel-cross, or the package that ships include/stddef.h for the armel gcc)") ;;
		esac
	else
		missing+=("arm-linux-gnueabi-gcc (apt: gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi)")
	fi
}

check_loader() {
	if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
		missing+=("arm-none-eabi-gcc (apt: gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi)")
	fi
	if ! python3 -c 'import ensurepip' >/dev/null 2>&1 || \
	   ! python3 -m venv --help >/dev/null 2>&1; then
		missing+=("python3 venv support (apt: python3-venv)")
	fi
}

check_initramfs() {
	local arch
	arch="$(uname -m)"
	# The armel busybox and the test binaries in the initramfs are ARM programs.
	# On an x86 host they must run under qemu-arm-static (C-17).
	case "$arch" in
		arm*|aarch64) ;;
		*) need_cmd qemu-arm-static qemu-user-static ;;
	esac
	need_cmd cpio cpio
	if ! command -v java >/dev/null 2>&1 || ! command -v zip >/dev/null 2>&1; then
		notes+=("java and zip are optional: without them porthelper.jar is not built and the screen times out (apt: default-jre-headless zip)")
	fi
	if ! command -v arm-linux-gnueabi-gcc >/dev/null 2>&1; then
		notes+=("arm-linux-gnueabi-gcc is needed for the static test tools (see check-deps.sh kernel)")
	fi
}

check_sd() {
	need_cmd mkfs.ext4 e2fsprogs
	need_cmd truncate coreutils
}

check_cm7() {
	need_cmd python3 python3
	need_cmd git git
	need_cmd curl curl
	need_cmd fakeroot fakeroot
	need_cmd e2fsck e2fsprogs
}

for s in $STAGES; do
	case "$s" in
		common)    check_common ;;
		kernel)    check_common; check_kernel ;;
		loader)    check_common; check_loader ;;
		initramfs) check_common; check_initramfs ;;
		sd)        check_sd ;;
		cm7)       check_common; check_cm7 ;;
		*) echo "check-deps.sh: unknown stage '$s'" >&2; exit 2 ;;
	esac
done

for n in "${notes[@]+"${notes[@]}"}"; do
	echo "note: $n"
done

if [ "${#missing[@]}" -gt 0 ]; then
	echo "Missing for stage(s): $STAGES" >&2
	for m in "${missing[@]}"; do
		echo "  - $m" >&2
	done
	exit 1
fi
echo "deps ok: $STAGES"
