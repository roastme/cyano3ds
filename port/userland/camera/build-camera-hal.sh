#!/bin/bash
# build-camera-hal.sh - build CM7's libcameraservice.so with the 3DS camera HAL
#
# CM7 for this port was built with USE_CAMERA_STUB, so its libcameraservice.so
# has CameraHardwareStub (the fake checkerboard camera) compiled in.  This
# rebuilds the library from CM7's own CameraService.cpp (gingerbread branch,
# patched: N3DS_CAMERA_RGB565) with CameraHardwareN3ds (Android3DS/Octoblimp's
# HAL for /dev/ctr_cam, moved to Gingerbread) in place of the stub, without
# the CM7 build tree:
#
#   * headers: CM7 frameworks/base include/ (sparse git clone) plus AOSP
#     gingerbread-release bionic, system/core, hardware/libhardware and
#     external/jpeg (gitiles tarballs), cached in $HDR;
#   * libraries to link against: taken from the card's system.img ($SYSIMG);
#   * compiler: the host arm-linux-gnueabi-g++, with Gingerbread's ABI
#     settings (ARMv6 + VFP softfp, int enums, no exceptions/RTTI), SysV hash
#     (Gingerbread's linker has no GNU hash) and -Bsymbolic like the platform
#     build.
#
# Output: $OUT/libcameraservice.so.  Install it as CYANO3DS/libcameraservice.so
# on the card: the initramfs copies card .so files over /system/lib at boot
# ("card libs"), so system.img stays untouched and deleting the file restores
# the stub camera.
#
# Usage: build-camera-hal.sh [SYSTEM_IMG] [OUT_DIR]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SYSIMG="${1:-${SYSIMG:-}}"
OUT="${2:-$HERE/out}"
HDR="${HDR:-$HOME/.cache/cyano3ds/gb-headers}"
CROSS="${CROSS:-arm-linux-gnueabi-}"

[ -n "$SYSIMG" ] && [ -f "$SYSIMG" ] || {
	echo "usage: $0 SYSTEM_IMG [OUT_DIR]  (the card's android/system.img)" >&2
	exit 1
}
command -v "${CROSS}g++" >/dev/null || { echo "${CROSS}g++ not found" >&2; exit 1; }
command -v debugfs >/dev/null || { echo "debugfs (e2fsprogs) not found" >&2; exit 1; }
mkdir -p "$HDR" "$OUT"

# ---------------------------------------------------------------------------
# headers
# ---------------------------------------------------------------------------
fetch_tar() {	# dir url
	[ -n "$(ls -A "$HDR/$1" 2>/dev/null)" ] && return 0
	mkdir -p "$HDR/$1"
	for try in 1 2 3 4 5; do
		if curl -sfL "$2" -o "$HDR/.tmp.tgz" && tar xzf "$HDR/.tmp.tgz" -C "$HDR/$1"; then
			rm -f "$HDR/.tmp.tgz"
			return 0
		fi
		sleep 3
	done
	echo "cannot fetch $2" >&2
	exit 1
}
G=https://android.googlesource.com/platform
R=+archive/refs/heads/gingerbread-release
fetch_tar bionic/libc/include            "$G/bionic/$R/libc/include.tar.gz"
fetch_tar bionic/libc/arch-arm/include   "$G/bionic/$R/libc/arch-arm/include.tar.gz"
fetch_tar bionic/libc/kernel/common      "$G/bionic/$R/libc/kernel/common.tar.gz"
fetch_tar bionic/libc/kernel/arch-arm    "$G/bionic/$R/libc/kernel/arch-arm.tar.gz"
fetch_tar bionic/libstdc++/include       "$G/bionic/$R/libstdc++/include.tar.gz"
fetch_tar bionic/libm/include            "$G/bionic/$R/libm/include.tar.gz"
fetch_tar system/core/include            "$G/system/core/$R/include.tar.gz"
fetch_tar hardware/libhardware/include   "$G/hardware/libhardware/$R/include.tar.gz"
fetch_tar external/jpeg                  "$G/external/jpeg/+archive/refs/heads/gingerbread-release.tar.gz"
if [ ! -f "$HDR/frameworks/base/include/camera/CameraHardwareInterface.h" ]; then
	rm -rf "$HDR/frameworks/base"
	git clone -q --filter=blob:none --no-checkout --depth 1 -b gingerbread \
		https://github.com/CyanogenMod/android_frameworks_base.git "$HDR/frameworks/base"
	git -C "$HDR/frameworks/base" sparse-checkout set --no-cone /include
	git -C "$HDR/frameworks/base" checkout -q
fi

# GCC 4.4 accepted "template<> template <typename K, typename V> struct
# trait_...<key_value_pair_t<K, V> >"; GCC 13 wants the plain partial
# specialization, which is what it meant.
perl -0pi -e 's/template<>\s*\n(template <typename K, typename V>)/$1/g' \
	"$HDR/frameworks/base/include/utils/TypeHelpers.h"

# ---------------------------------------------------------------------------
# libraries from system.img
# ---------------------------------------------------------------------------
LIBS="$OUT/syslibs"
mkdir -p "$LIBS"
for l in libui libutils libbinder liblog libcutils libmedia libcamera_client \
	 libsurfaceflinger_client libjpeg libc libstdc++ libm libdl; do
	debugfs -R "dump /lib/$l.so $LIBS/$l.so" "$SYSIMG" >/dev/null 2>&1
	[ -s "$LIBS/$l.so" ] || { echo "system.img has no /lib/$l.so" >&2; exit 1; }
done

# ---------------------------------------------------------------------------
# compile + link
# ---------------------------------------------------------------------------
GCCINC="$(${CROSS}gcc -print-file-name=include)"
CFLAGS=(
	-nostdinc -nostdinc++ -isystem "$GCCINC"
	-include "$HDR/system/core/include/arch/linux-arm/AndroidConfig.h"
	-I "$HDR/system/core/include/arch/linux-arm"
	-I "$HERE"
	-I "$HDR/frameworks/base/include"
	-I "$HDR/system/core/include"
	-I "$HDR/hardware/libhardware/include"
	-I "$HDR/external/jpeg"
	-isystem "$HDR/bionic/libc/arch-arm/include"
	-isystem "$HDR/bionic/libc/include"
	-isystem "$HDR/bionic/libstdc++/include"
	-isystem "$HDR/bionic/libc/kernel/common"
	-isystem "$HDR/bionic/libc/kernel/arch-arm"
	-isystem "$HDR/bionic/libm/include"
	-isystem "$HDR/bionic/libm/include/arch/arm"
	-DANDROID -DNDEBUG -DLOG_NDEBUG=1
	-march=armv6 -mfpu=vfp -mfloat-abi=softfp -marm
	-fpic -fno-short-enums -fno-exceptions -fno-rtti -fno-threadsafe-statics
	-fno-stack-protector -fno-strict-aliasing -ffunction-sections
	-fvisibility-inlines-hidden -std=gnu++98 -O2 -g0
	-Wno-psabi -Wno-deprecated -Wno-write-strings -Wno-attributes
)
OBJ="$OUT/obj"
mkdir -p "$OBJ"
for src in CameraService.cpp CameraHardwareN3ds.cpp; do
	echo "  CXX $src"
	"${CROSS}g++" "${CFLAGS[@]}" -c "$HERE/$src" -o "$OBJ/${src%.cpp}.o"
done
# __dso_handle normally comes from Gingerbread's crtbegin_so.o
printf 'void *__dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;\n' \
	> "$OBJ/dso_handle.c"
"${CROSS}gcc" -march=armv6 -marm -fpic -c "$OBJ/dso_handle.c" -o "$OBJ/dso_handle.o"

echo "  LD  libcameraservice.so"
"${CROSS}g++" -nostdlib -shared -Wl,-soname,libcameraservice.so \
	-Wl,--hash-style=sysv -Wl,-Bsymbolic -Wl,--gc-sections \
	-Wl,-z,noexecstack -Wl,--no-undefined -Wl,--build-id=none \
	"$OBJ/CameraService.o" "$OBJ/CameraHardwareN3ds.o" "$OBJ/dso_handle.o" \
	-L"$LIBS" -lui -lutils -lbinder -llog -lcutils -lmedia -lcamera_client \
	-lsurfaceflinger_client -ljpeg -lc -lstdc++ -lm -ldl \
	"$(${CROSS}gcc -march=armv6 -print-libgcc-file-name)" \
	-o "$OUT/libcameraservice.so"
"${CROSS}strip" --strip-unneeded "$OUT/libcameraservice.so"
ls -l "$OUT/libcameraservice.so"
