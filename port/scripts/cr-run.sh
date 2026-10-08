#!/bin/bash
# cr-run.sh - run a command inside the CM7.2 build chroot.
#
# The chroot's bind mounts (/proc, /sys, /dev, /dev/pts) are tied to the session
# that created them and are lost when the WSL VM restarts, so they are re-made
# here on every invocation.  PATH is sanitized so the chroot's old tools are
# found ahead of anything leaked in from the host.
#
# chroot(2) and mount(2) need root, so the script re-executes itself with
# sudo when it is not already running as root.
#
# Usage:
#   cr-run.sh 'cd /root/p3ds/cm7 && . build/envsetup.sh && make droidcore -j2'
#
# Environment:
#   CHROOT   chroot path (default $HOME/p3ds/cm12)

set -euo pipefail

CHROOT="${CHROOT:-$HOME/p3ds/cm12}"

if [ "$(id -u)" -ne 0 ]; then
	exec sudo "$0" "$@"
fi

# mountpoint -q keeps this idempotent; a real mount failure aborts loudly
# (set -e) instead of being swallowed.
for m in proc sys dev; do
	mountpoint -q "$CHROOT/$m" || mount --bind "/$m" "$CHROOT/$m"
done
mountpoint -q "$CHROOT/dev/pts" || mount --bind /dev/pts "$CHROOT/dev/pts"

exec chroot "$CHROOT" /bin/bash -c \
	"export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; $*"
