#!/usr/bin/env bash
#
# build-doh.sh — reproducibly cross-compile the ALR DNS-over-HTTPS (DoH)
# name-resolution shim (libalr_doh.so) for Android arm64 (aarch64) with zig cc.
#
# WHAT / WHY
# ----------
# On Android an untrusted_app cannot send raw UDP/53 to an arbitrary nameserver,
# so a glibc guest's getaddrinfo() hangs and `apt update` cannot resolve
# archive.ubuntu.com / deb.debian.org. This LD_PRELOAD shim interposes
# getaddrinfo() and resolves names over DNS-over-HTTPS (RFC 8484) on TCP/443 —
# the one transport an app IS permitted to use. See libalr_doh.c for the full
# design and the LD_PRELOAD device-wiring notes.
#
# Like libalr_interpose.so this is NOT built by CMake and is NOT shipped in the
# APK; it ships INSIDE the rootfs tar at:
#
#       ./usr/lib/androlinux/libalr_doh.so
#
# so the loader can put it on the guest's LD_PRELOAD (after the interposer .so)
# as the guest-visible path /usr/lib/androlinux/libalr_doh.so.
#
# TLS: the shim dlopen()s libssl.so.3 / libcrypto.so.3 from the rootfs at
# runtime (no link-time OpenSSL dependency), so this build needs no OpenSSL and
# the .so loads even on a rootfs without TLS (it then self-disables).
#
# Determinism: the toolchain target (glibc 2.36), flags, and the two .c sources
# fully determine the output; zig embeds no timestamp into a -shared object.
#
# Usage:
#   scripts/build-doh.sh [OUT_DIR]
#     OUT_DIR  directory for libalr_doh.so (default: build/doh/ under repo root).
# Environment:
#   ZIG      zig binary (default "zig"; must be >= 0.16.0).
#   TARGET   zig --target triple (default aarch64-linux-gnu.2.36).
# Exit 0 on success (prints "<sha256>  <path>"), nonzero on any error.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SRC_DIR="app/src/main/cpp/alr_doh"
SRCS=("$SRC_DIR/libalr_doh.c" "$SRC_DIR/alr_doh_wire.c")
OUT_DIR="${1:-build/doh}"
OUT_SO="${OUT_DIR}/libalr_doh.so"
ROOTFS_INSTALL_PATH="./usr/lib/androlinux/libalr_doh.so"

ZIG="${ZIG:-zig}"
TARGET="${TARGET:-aarch64-linux-gnu.2.36}"

if ! command -v "$ZIG" >/dev/null 2>&1; then
  echo "build-doh: zig not found (set ZIG=/path/to/zig)" >&2
  exit 127
fi
for s in "${SRCS[@]}"; do
  if [ ! -f "$s" ]; then echo "build-doh: source not found: $s" >&2; exit 1; fi
done

echo "build-doh: zig    = $("$ZIG" version) ($ZIG)" >&2
echo "build-doh: target = $TARGET" >&2
echo "build-doh: srcs   = ${SRCS[*]}" >&2
echo "build-doh: out    = $OUT_SO" >&2

mkdir -p "$OUT_DIR"

# -shared : a PRELOAD-able shared object. -fPIC : arbitrary base. -O2 : the
# resolution path should be cheap. No -flto / no timestamp flags (reproducible).
"$ZIG" cc \
  --target="$TARGET" \
  -shared \
  -fPIC \
  -O2 \
  -Wall \
  -I"$SRC_DIR" \
  -o "$OUT_SO" \
  "${SRCS[@]}"

if [ ! -f "$OUT_SO" ]; then
  echo "build-doh: compile produced no output" >&2
  exit 1
fi

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else echo "build-doh: no sha256 tool (need sha256sum or shasum)" >&2; return 1; fi
}
SHA="$(sha256_of "$OUT_SO")"

{
  echo "build-doh: OK"
  echo "build-doh: pack into the rootfs tar at: ${ROOTFS_INSTALL_PATH}"
  echo "build-doh:   e.g. tar --owner=0 --group=0 -rf rootfs.tar \\"
  echo "build-doh:        --transform='s#.*#${ROOTFS_INSTALL_PATH#./}#' ${OUT_SO}"
  echo "build-doh: loader then appends /usr/lib/androlinux/libalr_doh.so to LD_PRELOAD"
} >&2

# Sole stdout line: "<sha256>  <path>".
echo "${SHA}  ${OUT_SO}"
