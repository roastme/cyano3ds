#!/bin/bash
# stage-svox.sh - stage AOSP's svox into a CM7.2 tree.
#
# The CyanogenMod fork of external/svox was removed from GitHub (DMCA takedown),
# so the gb-release-7.2 sync excludes it (see cm7-local-manifests/remove-broken.xml).
# CM7's build still needs external/svox, so this stages the AOSP source at the
# android-2.3.7_r1 tag in its place — the same source CM7.2 was released against.
#
# Run this AFTER repo sync and BEFORE the build (and after cleanup-cm7-tree.sh,
# which no longer touches external/svox).
#
# Usage:  bash port/scripts/stage-svox.sh [CM7_DIR]
#         CM7_DIR=/root/p3ds/cm7 bash port/scripts/stage-svox.sh

set -euo pipefail

CM7_DIR="${1:-${CM7_DIR:-$HOME/p3ds/cm7}}"
DEST="$CM7_DIR/external/svox"

say() { printf '\n==> %s\n' "$*"; }

[ -d "$CM7_DIR/build" ] || {
	echo "no CyanogenMod tree at $CM7_DIR" >&2
	exit 1
}

if [ -d "$DEST" ]; then
	echo "    $DEST already present — leaving it"
	exit 0
fi

say "cloning AOSP platform/external/svox (android-2.3.7_r1) into external/svox"
git clone --quiet --depth 1 --branch android-2.3.7_r1 \
	https://android.googlesource.com/platform/external/svox "$DEST"

say "done"
