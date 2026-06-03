#!/usr/bin/env bash
# build-icd.sh — cross-compile the ALR guest Vulkan ICD (libvulkan.so.1) + a tiny
# vkEnumeratePhysicalDevices client for the aarch64 glibc guest, using zig cc (the
# same toolchain + glibc-2.34 NEEDED-clean pin as the GLES guest_shim/build-shim.sh).
#
# Produces, in $OUT (default ./out):
#   libvulkan.so.1  (SONAME libvulkan.so.1)  <- the ALR Vulkan ICD (ENUM rung)
#   libvulkan.so    -> libvulkan.so.1         (dev/link convenience symlink)
#   alr-vk-enum                                <- minimal Vulkan client (links -lvulkan)
#
# WHY NEEDED-CLEAN MATTERS (identical to the GLES shim): the tiny ALR rootfs ships only
# libc.so.6 (glibc >= 2.34 folded libpthread + libdl into libc). Pinning the target to
# aarch64-linux-gnu.2.34 makes pthread_once/pthread_mutex resolve from libc.so.6 so the
# ONLY DT_NEEDED of libvulkan.so.1 is libc.so.6. A libpthread.so.0 NEEDED would make the
# guest ld.so fail to load the ICD in the tiny rootfs. DO NOT drop the .2.34 suffix.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$HERE/out}"
# alr_gpu_vk_proto.hpp lives one dir up (the shared wire contract); the C ring
# producer port (alr_gpu_ring_c.h) lives in the sibling guest_shim/ dir — include both.
ALR_GPU_DIR="$(cd "$HERE/.." && pwd)"
GUEST_SHIM_DIR="$ALR_GPU_DIR/guest_shim"
TARGET="aarch64-linux-gnu.2.34"
CC=(zig cc -target "$TARGET")
# The ICD TU is pure C; the .hpp wire header is C-clean (extern "C" guarded).
CFLAGS=(-std=c11 -O2 -fPIC -Wall -Wextra -fvisibility=default
        -I"$HERE" -I"$ALR_GPU_DIR" -I"$GUEST_SHIM_DIR")

mkdir -p "$OUT"
echo "== zig version =="
zig version

echo "== compile the ALR Vulkan ICD (-> libvulkan.so.1) =="
"${CC[@]}" "${CFLAGS[@]}" -c "$HERE/alr_icd_vulkan.c" -o "$OUT/alr_icd_vulkan.o"

echo "== link libvulkan.so.1 (SONAME libvulkan.so.1) =="
# NO -lpthread: the glibc-2.34 TARGET pin folds pthread into libc, so the only DT_NEEDED
# is libc.so.6 (verified below). A libpthread.so.0 NEEDED would break loading in the
# tiny rootfs (same constraint the GLES shim documents).
"${CC[@]}" -shared -fPIC \
    -Wl,-soname,libvulkan.so.1 \
    "$OUT/alr_icd_vulkan.o" \
    -o "$OUT/libvulkan.so.1"
ln -sf libvulkan.so.1 "$OUT/libvulkan.so"

echo "== link alr-vk-enum (-lvulkan) =="
"${CC[@]}" -O2 -I"$HERE" -I"$ALR_GPU_DIR" \
    "$HERE/alr-vk-enum.c" \
    -L"$OUT" -Wl,-rpath,'$ORIGIN' -lvulkan \
    -o "$OUT/alr-vk-enum"

echo "== link alr-vk-tri (VK-M4 PRESENT: guest SPIR-V + AHB swapchain + present, -lvulkan) =="
# The guest triangle app: uploads its OWN SPIR-V (alr_vk_tri_spirv.h) and presents an
# AHB-backed swapchain (routed to the in-app compositor). Binds ONLY our libvulkan.so.1.
"${CC[@]}" -O2 -I"$HERE" -I"$ALR_GPU_DIR" \
    "$HERE/alr-vk-tri.c" \
    -L"$OUT" -Wl,-rpath,'$ORIGIN' -lvulkan \
    -o "$OUT/alr-vk-tri"

echo
echo "== artifacts =="
ls -l "$OUT"
echo
echo "== SONAME / NEEDED verification (MUST be libc.so.6 only) =="
for so in libvulkan.so.1; do
    echo "--- $so ---"
    if command -v readelf >/dev/null 2>&1; then
        readelf -d "$OUT/$so" | grep -E 'SONAME|NEEDED' || true
    elif command -v llvm-readelf >/dev/null 2>&1; then
        llvm-readelf -d "$OUT/$so" | grep -E 'SONAME|NEEDED' || true
    else
        echo "(no readelf available; skipping dynamic-tag dump)"
    fi
done
echo
echo "== exported ICD entry points (must include vk_icdGetInstanceProcAddr + vkGetInstanceProcAddr) =="
if command -v llvm-nm >/dev/null 2>&1; then
    llvm-nm -D "$OUT/libvulkan.so.1" 2>/dev/null | grep -E 'vk_icd|vkGetInstanceProcAddr|vkCreateInstance|vkEnumeratePhysicalDevices' || true
elif command -v nm >/dev/null 2>&1; then
    nm -D "$OUT/libvulkan.so.1" 2>/dev/null | grep -E 'vk_icd|vkGetInstanceProcAddr|vkCreateInstance|vkEnumeratePhysicalDevices' || true
fi
echo
echo "BUILD OK"
