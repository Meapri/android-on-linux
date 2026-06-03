#!/usr/bin/env bash
# build-usb-shim.sh — cross-compile the ALR libusb backend shim for the aarch64
# glibc guest, producing a standalone drop-in libusb-1.0.so.0.
#
# Mirrors alr_gpu/guest_shim/build-shim.sh: uses `zig cc` (clang + a bundled
# aarch64-linux-gnu sysroot — no external cross toolchain) and pins glibc 2.34
# so pthread/dl fold into libc.so.6 (the tiny ALR rootfs ships no
# libpthread.so.0 / libdl.so.2 — a stray DT_NEEDED on either would break the
# guest ld.so resolution, exactly as documented for the GLES shim).
#
# Produces, in $OUT (default ./out):
#   libusb-1.0.so.0   (SONAME libusb-1.0.so.0)   <- the shim; the ONLY DT_NEEDED is libc.so.6
#   libusb-1.0.so      (dev symlink so -lusb-1.0 links at build time)
#
# It is overlaid into the rootfs (or LD_PRELOAD'd) so it shadows the distro
# libusb-1.0.so.0; an unmodified guest app then forwards its libusb_* calls to
# the Android UsbHostBridge over $ALR_USB_SOCK.  HOST-ONLY build; no device.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$HERE/out}"

# glibc 2.34 pin — same rationale as the GLES shim: fold pthread into libc so the
# only DT_NEEDED is libc.so.6. DO NOT drop the version suffix.
TARGET="${ALR_USB_TARGET:-aarch64-linux-gnu.2.34}"
CC=(zig cc -target "$TARGET")
CFLAGS=(-std=c11 -O2 -fPIC -Wall -Wextra -fvisibility=default -I"$HERE")

mkdir -p "$OUT"
echo "== zig version =="
zig version

echo "== compile libusb shim =="
"${CC[@]}" "${CFLAGS[@]}" -c "$HERE/alr_libusb_shim.c" -o "$OUT/alr_libusb_shim.o"

echo "== link libusb-1.0.so.0 (SONAME libusb-1.0.so.0) =="
# No -lpthread: the glibc-2.34 TARGET pin folds pthread into libc.so.6.
"${CC[@]}" -shared -fPIC \
    -Wl,-soname,libusb-1.0.so.0 \
    "$OUT/alr_libusb_shim.o" \
    -o "$OUT/libusb-1.0.so.0"
ln -sf libusb-1.0.so.0 "$OUT/libusb-1.0.so"

echo
echo "== artifacts =="
ls -l "$OUT"
echo
echo "== SONAME / NEEDED verification =="
so="$OUT/libusb-1.0.so.0"
if command -v readelf >/dev/null 2>&1; then
    readelf -d "$so" | grep -E 'SONAME|NEEDED' || true
    echo "--- exported libusb_* symbols (count) ---"
    readelf -sW "$so" | awk '$8 ~ /^libusb_/ && $4=="FUNC" {print $8}' | sort -u | tee "$OUT/.exports" | wc -l
elif command -v llvm-readelf >/dev/null 2>&1; then
    llvm-readelf -d "$so" | grep -E 'SONAME|NEEDED' || true
else
    echo "(no readelf available; skipping dynamic-tag dump)"
fi
echo
echo "BUILD OK"
