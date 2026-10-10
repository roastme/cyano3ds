#!/bin/bash
# setup-chroot.sh - create the Ubuntu 12.04 chroot that CM7.2 builds in.
#
# This used to need root (sudo for debootstrap, mount and chroot).  It is now a
# wrapper around cm7-rootless.sh, which runs the same steps without root, so
# the sudo prompt is gone.  Kept so existing instructions still work.
#
# Usage:
#   bash port/scripts/setup-chroot.sh              # tools, bootstrap, chroot, sync
#   bash port/scripts/setup-chroot.sh --no-sync    # without the source sync
#
# Then build with:
#   bash port/scripts/cm7-rootless.sh port build package
#   bash port/scripts/cm7-rootless.sh run 'make droidcore -j2'   # any command in the chroot

set -euo pipefail

STAGES="tools bootstrap chroot sync"
[ "${1:-}" = "--no-sync" ] && STAGES="tools bootstrap chroot"

exec bash "$(dirname "$0")/cm7-rootless.sh" $STAGES
