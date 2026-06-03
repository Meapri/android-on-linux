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

echo "== link libalr_mali_icd.so (SONAME libalr_mali_icd.so) =="
# ICD DISCOVERY REDIRECT (Part B): our ICD is RENAMED from libvulkan.so.1 to
# libalr_mali_icd.so so it COEXISTS with the real Khronos Vulkan-Loader (which now
# owns libvulkan.so.1 in the same /usr/lib/androlinux dir, via the vk-loader overlay).
# ANGLE/volk dlopen("libvulkan.so.1") then reach the KHRONOS LOADER, which reads our
# alr_icd.json manifest and loads THIS file (libalr_mali_icd.so) as the ICD → Mali.
# (If our ICD kept the name libvulkan.so.1 the loader would try to load ITSELF.)
# NO -lpthread: the glibc-2.34 TARGET pin folds pthread into libc, so the only DT_NEEDED
# is libc.so.6 (verified below). A libpthread.so.0 NEEDED would break loading in the
# tiny rootfs (same constraint the GLES shim documents).
"${CC[@]}" -shared -fPIC \
    -Wl,-soname,libalr_mali_icd.so \
    "$OUT/alr_icd_vulkan.o" \
    -o "$OUT/libalr_mali_icd.so"
# Build-time -lvulkan resolution symlink: the in-tree guest test clients below link
# `-lvulkan`, so provide libvulkan.so -> our ICD HERE (in the build OUT dir only) so
# they link. NOTE: this dev symlink is NOT shipped by the vk-icd overlay (the Khronos
# loader owns libvulkan.so / libvulkan.so.1 in the rootfs); build_vk_icd_overlay.py
# drops it. The clients' DT_NEEDED thus records libalr_mali_icd.so (the SONAME), so on
# device they bind our ICD DIRECTLY (the existing direct-SONAME path, just renamed),
# while ANGLE — which dlopens the bare "libvulkan.so.1" — reaches it via the loader.
ln -sf libalr_mali_icd.so "$OUT/libvulkan.so"

echo "== link alr-vk-enum (-lvulkan -> libalr_mali_icd.so) =="
"${CC[@]}" -O2 -I"$HERE" -I"$ALR_GPU_DIR" \
    "$HERE/alr-vk-enum.c" \
    -L"$OUT" -Wl,-rpath,'$ORIGIN' -lvulkan \
    -o "$OUT/alr-vk-enum"

echo "== link alr-vk-tri (VK-M4 PRESENT: guest SPIR-V + AHB swapchain + present, -lvulkan) =="
# The guest triangle app: uploads its OWN SPIR-V (alr_vk_tri_spirv.h) and presents an
# AHB-backed swapchain (routed to the in-app compositor). Binds our ICD directly
# (DT_NEEDED libalr_mali_icd.so via the build-time -lvulkan symlink above).
"${CC[@]}" -O2 -I"$HERE" -I"$ALR_GPU_DIR" \
    "$HERE/alr-vk-tri.c" \
    -L"$OUT" -Wl,-rpath,'$ORIGIN' -lvulkan \
    -o "$OUT/alr-vk-tri"

echo
echo "== artifacts =="
ls -l "$OUT"
echo
echo "== SONAME / NEEDED verification (SONAME MUST be libalr_mali_icd.so; NEEDED libc.so.6 only) =="
for so in libalr_mali_icd.so; do
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
    llvm-nm -D "$OUT/libalr_mali_icd.so" 2>/dev/null | grep -E 'vk_icd|vkGetInstanceProcAddr|vkCreateInstance|vkEnumeratePhysicalDevices' || true
elif command -v nm >/dev/null 2>&1; then
    nm -D "$OUT/libalr_mali_icd.so" 2>/dev/null | grep -E 'vk_icd|vkGetInstanceProcAddr|vkCreateInstance|vkEnumeratePhysicalDevices' || true
fi
echo
echo "BUILD OK"
