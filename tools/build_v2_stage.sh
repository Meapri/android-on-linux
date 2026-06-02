#!/usr/bin/env bash
#
# build_v2_stage.sh — one-shot builder for the v2 "universal app install"
# staging tars the device must extract before a non-root `apt install` /
# `dpkg -i hello.deb` can be attempted.
#
# It produces, into OUT_DIR (default ./out/v2-stage):
#
#   fakeroot-stage.tar   ./usr/lib/androlinux/libalr_fakeroot.so  (credential/
#                        chown/stat shim that CHAINS onto libalr_interpose.so)
#   apt-dpkg-stage.tar   apt+dpkg runtime closure (base-subtracted, §5-E) +
#                        dpkg admindir scaffold (+ self-contained front-ends)
#   hello_2.10-3build1_arm64.deb   the trivial on-device unpack target (separate
#                        asset, NOT packed into the overlay)
#
# and prints the exact chained-LD_PRELOAD dpkg invocation the device drain runs
# (the §5 contract: fakeroot .so FIRST, interpose .so KEPT — never replaced).
#
# HONEST SCOPE: HOST-ONLY. This builds + verifies the staging tars. It does NOT
# touch a device, does NOT change the APK build stamp, and does NOT itself prove
# the on-device unpack passes — that needs the exec-re-entry re-map in the loader
# plus the chained preload on the device.
#
# Requirements:
#   * zig   (cross-compiles the fakeroot shim for aarch64) — REQUIRED.
#   * ar    (cracks .deb members) + network to ports.ubuntu.com — REQUIRED for
#           the apt-dpkg closure. Without them, pass --fakeroot-only.
#   * python3 with the repo on PYTHONPATH (run from the repo root).
#
# Usage:
#   tools/build_v2_stage.sh [--out DIR] [--base ROOTFS_TAR] [--rootfs DEVICE_DIR]
#                           [--fakeroot-only] [--no-self-contained]
#
set -euo pipefail

# Resolve repo root from this script's location so it runs from anywhere.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

OUT_DIR="$REPO/out/v2-stage"
BASE="$REPO/rootfs/tiny-rootfs.tar"
DEVICE_ROOTFS="/data/data/dev.chanwoo.androlinux/files/rootfs"
FAKEROOT_ONLY=0
SELF_CONTAINED="--self-contained"
CACHE="${TMPDIR:-/tmp}/alr-deb-cache"

while [ $# -gt 0 ]; do
  case "$1" in
    --out)             OUT_DIR="$2"; shift 2 ;;
    --base)            BASE="$2"; shift 2 ;;
    --rootfs)          DEVICE_ROOTFS="$2"; shift 2 ;;
    --fakeroot-only)   FAKEROOT_ONLY=1; shift ;;
    --no-self-contained) SELF_CONTAINED=""; shift ;;
    --cache)           CACHE="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

PY="${PYTHON:-python3}"
mkdir -p "$OUT_DIR"

echo "== [1/3] fakeroot-stage.tar (LD_PRELOAD credential shim) =="
"$PY" -m tools.build_fakeroot_overlay \
  --out "$OUT_DIR/fakeroot-stage.tar" \
  --keep-so "$OUT_DIR/libalr_fakeroot.so"
"$PY" -m tools.stage_tar_spec --overlay "$OUT_DIR/fakeroot-stage.tar"

if [ "$FAKEROOT_ONLY" -eq 1 ]; then
  echo
  echo "== fakeroot-only: skipping apt-dpkg closure =="
else
  echo
  echo "== [2/3] apt-dpkg-stage.tar (apt+dpkg closure + admindir) =="
  echo "   base   = $BASE"
  echo "   cache  = $CACHE"
  # shellcheck disable=SC2086
  "$PY" -m tools.build_apt_dpkg_overlay \
    --out "$OUT_DIR/apt-dpkg-stage.tar" \
    --base "$BASE" \
    --cache "$CACHE" \
    $SELF_CONTAINED
  "$PY" -m tools.stage_tar_spec \
    --overlay "$OUT_DIR/apt-dpkg-stage.tar" --base "$BASE"

  echo
  echo "== [3/3] hello.deb (on-device unpack target) =="
  "$PY" -m tools.build_apt_dpkg_overlay \
    --fetch-test-deb "$OUT_DIR" --cache "$CACHE"
fi

echo
echo "== device drain contract (§5) — fakeroot FIRST, interpose KEPT =="
"$PY" -m tools.build_fakeroot_overlay --device-cmd --rootfs "$DEVICE_ROOTFS"

echo
echo "== staged into: $OUT_DIR =="
ls -la "$OUT_DIR"
