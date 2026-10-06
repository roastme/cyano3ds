#!/bin/bash
# make-egl-preload.sh - build a libEGL.so that preloads libagl.so into zygote
#
# Why this exists
# ---------------
# Every SDK system library is "prelinked": the last 8 bytes of the file are a
# 4-byte load address followed by "PRE ".  Android's bionic linker refuses to
# load a prelinked library anywhere but that address (and the library's
# absolutely-resolved pointers only make sense there).  libagl.so's address is
# 0xacc00000.
#
# libEGL.so only dlopen()s libhgl.so -> libagl.so when an app calls
# eglGetDisplay(), i.e. *after* the app has loaded its own big native library.
# On this port the game's library takes the address libagl needs, libagl is
# mapped elsewhere, bionic rejects the load, EGL never initialises, and the
# game dereferences a NULL glGetString() result:
#
#   libEGL: couldn't load <libagl.so> library (reserve_mem_region[711]: OOPS:
#           prelinked library 'libagl.so' mapped at 0xb001e000, not at 0xacc00000)
#   libEGL: eglInitialize:710 error 3001 (EGL_NOT_INITIALIZED)
#   signal 11 (SIGSEGV), fault addr 00000036   # strlen(0x36)
#
# The fix is to make libEGL.so depend on libagl.so (DT_NEEDED).  libEGL.so is
# already loaded by libandroid_runtime.so in the *zygote*, so adding that
# dependency maps libagl at its prelinked address in the clean zygote address
# space, and every forked app inherits the mapping.
#
# Why not patchelf directly
# -------------------------
# patchelf rewrites the layout and zeroes the trailing prelink tag, so bionic
# would then load the library at a random address and crash on its pre-applied
# pointers.  We keep patchelf's added DT_NEEDED but re-append the original tag,
# and verify that the load segments' virtual addresses were not moved.
#
# Usage: make-egl-preload.sh [src-libEGL.so] [out-libEGL.so]
#   defaults: /root/p3ds/src/donut-system/lib/libEGL.so  (legacy prebuilt
#             tree name kept from the archived flavor; only the comments here
#             were genericised, the path itself is functional)

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${1:-/root/p3ds/src/donut-system/lib/libEGL.so}"
OUT="${2:-$REPO/out/sd/CYANO3DS/libEGL.so}"

need_cmd() { command -v "$1" >/dev/null || { echo "missing tool: $1" >&2; exit 1; }; }
need_cmd patchelf
need_cmd arm-linux-gnueabi-readelf

[ -f "$SRC" ] || { echo "missing source library: $SRC" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$(dirname "$OUT")"
cp "$SRC" "$TMP/orig.so"

# --- original prelink tag + load-segment vaddrs (must be preserved) ---------
TAG="$(tail -c 8 "$TMP/orig.so" | xxd -p)"
case "$TAG" in
	*50524520) : ;;                       # ... "PRE "
	*) echo "source is not prelinked (tag=$TAG)" >&2; exit 1 ;;
esac
BASE="$(printf '%s' "${TAG:0:8}" | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')"
orig_loads="$(arm-linux-gnueabi-readelf -lW "$TMP/orig.so" | awk '$1=="LOAD"{print $3}' | paste -sd, -)"

# --- add the dependency, then restore the prelink tag -----------------------
cp "$TMP/orig.so" "$TMP/patched.so"
patchelf --add-needed libagl.so "$TMP/patched.so"
python3 - "$TMP/patched.so" "$TAG" <<'PY'
import sys, pathlib, binascii
p = pathlib.Path(sys.argv[1])
tag = binascii.unhexlify(sys.argv[2])
d = p.read_bytes()
# patchelf zeroes the tag; drop whatever it left and append the real one so it
# is again the last 8 bytes of the file (which is where bionic reads it).
if d[-8:] != b"\x00" * 8:
    # patchelf may have kept the old tag; strip the trailing tag either way
    d = d[:-8]
p.write_bytes(d + tag)
PY

# --- verify -----------------------------------------------------------------
new_loads="$(arm-linux-gnueabi-readelf -lW "$TMP/patched.so" | awk '$1=="LOAD"{print $3}' | paste -sd, -)"
case "$new_loads" in
	"$orig_loads"*) : ;;
	*) echo "FAIL: load-segment vaddrs moved ($orig_loads -> $new_loads)" >&2; exit 1 ;;
esac
[ "$(tail -c 8 "$TMP/patched.so" | xxd -p)" = "$TAG" ] || {
	echo "FAIL: prelink tag not restored" >&2; exit 1; }
arm-linux-gnueabi-readelf -dW "$TMP/patched.so" | grep -q 'NEEDED.*\[libagl.so\]' || {
	echo "FAIL: DT_NEEDED libagl.so missing" >&2; exit 1; }

cp "$TMP/patched.so" "$OUT"
chmod 0644 "$OUT"

echo "==> $OUT"
echo "    source base    : 0x$BASE"
echo "    original loads : $orig_loads"
echo "    patched  loads : $new_loads (tag preserved)"
echo "    size           : $(stat -c%s "$SRC") -> $(stat -c%s "$OUT") bytes"
arm-linux-gnueabi-readelf -dW "$OUT" | grep -E 'NEEDED|SONAME'
