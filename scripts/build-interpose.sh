#!/usr/bin/env bash
#
# build-interpose.sh — reproducibly cross-compile the ALR LD_PRELOAD path
# interposer (libalr_interpose.so) for Android arm64 (aarch64) with zig cc.
#
# The interposer is the in-process half of the PC-gated path-mediation design
# (docs/design/pcgate-seccomp.md). It is NOT built by CMake and is NOT shipped
# in the APK — it ships INSIDE the rootfs tar at:
#
#       ./usr/lib/androlinux/libalr_interpose.so
#
# so that, once the rootfs is extracted on-device, the loader can point
# LD_PRELOAD at /usr/lib/androlinux/libalr_interpose.so (the guest-visible path).
#
# Why a separate script (not CMake): the host build box has zig + clang but no
# Android NDK, so it cannot drive the Gradle/CMake APK build. zig cc is a
# self-contained cross toolchain that targets a pinned glibc ABI, which is
# exactly what an Ubuntu 24.04 arm64 guest expects. This script is the single
# source of truth for how the .so is produced; CI and the rootfs-packer call it.
#
# Determinism: the toolchain target (glibc 2.36), the flags, and the single .c
# source fully determine the output. We print the sha256 so a repacked rootfs
# can be audited against a known-good build. zig embeds no build timestamp into
# a -shared object, so repeated runs on the same zig version are bit-identical.
#
# Usage:
#   scripts/build-interpose.sh [OUT_DIR]
#
#   OUT_DIR   directory to write libalr_interpose.so into.
#             Default: build/interpose/ under the repo root.
#
# Environment:
#   ZIG       zig binary to use (default: "zig" from PATH). Must be >= 0.16.0.
#   TARGET    zig --target triple (default: aarch64-linux-gnu.2.36).
#
# Exit status: 0 on success (prints "<sha256>  <path>"), non-zero on any error.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SRC="app/src/main/cpp/alr_interpose/libalr_interpose.c"
OUT_DIR="${1:-build/interpose}"
OUT_SO="${OUT_DIR}/libalr_interpose.so"

# The guest-visible install location inside the rootfs tar. Kept in lockstep
# with the LD_PRELOAD path the loader injects (runtime_report.cpp) and the
# contract in docs/design/pcgate-seccomp.md.
ROOTFS_INSTALL_PATH="./usr/lib/androlinux/libalr_interpose.so"

ZIG="${ZIG:-zig}"
# Pin the glibc ABI: an Ubuntu 24.04 arm64 guest ships glibc >= 2.36, and
# pinning to 2.36 keeps the .so loadable on the broadest set of 24.04 images
# while still resolving every symbol the interposer references.
TARGET="${TARGET:-aarch64-linux-gnu.2.36}"

if ! command -v "$ZIG" >/dev/null 2>&1; then
  echo "build-interpose: zig not found (set ZIG=/path/to/zig)" >&2
  exit 127
fi

if [ ! -f "$SRC" ]; then
  echo "build-interpose: source not found: $SRC" >&2
  exit 1
fi

echo "build-interpose: zig    = $("$ZIG" version) ($ZIG)" >&2
echo "build-interpose: target = $TARGET" >&2
echo "build-interpose: source = $SRC" >&2
echo "build-interpose: out    = $OUT_SO" >&2

mkdir -p "$OUT_DIR"

# Flags mirror the documented build line in libalr_interpose.c's header comment:
#   --target=aarch64-linux-gnu.2.36 -shared -fPIC -O2
# -shared : a PRELOAD-able shared object (no main).
# -fPIC   : position-independent; ld.so maps it at an arbitrary base.
# -O2     : the path hot path must be cheap; matches the .so's design intent.
# We deliberately do NOT pass -flto or any timestamping flag, to keep the
# output reproducible across runs of the same zig version.
"$ZIG" cc \
  --target="$TARGET" \
  -shared \
  -fPIC \
  -O2 \
  -o "$OUT_SO" \
  "$SRC"

if [ ! -f "$OUT_SO" ]; then
  echo "build-interpose: compile produced no output" >&2
  exit 1
fi

# sha256: prefer the portable tools available on macOS (shasum) and Linux
# (sha256sum). Print "<sha256>  <path>" on stdout — the only stdout line — so a
# caller can capture it directly.
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    echo "build-interpose: no sha256 tool (need sha256sum or shasum)" >&2
    return 1
  fi
}

SHA="$(sha256_of "$OUT_SO")"

{
  echo "build-interpose: OK"
  echo "build-interpose: pack into the rootfs tar at: ${ROOTFS_INSTALL_PATH}"
  echo "build-interpose:   e.g. tar --owner=0 --group=0 -rf rootfs.tar \\"
  echo "build-interpose:        --transform='s#.*#${ROOTFS_INSTALL_PATH#./}#' ${OUT_SO}"
  echo "build-interpose: loader then sets LD_PRELOAD=/usr/lib/androlinux/libalr_interpose.so"
} >&2

# Sole stdout line: "<sha256>  <path>".
echo "${SHA}  ${OUT_SO}"
