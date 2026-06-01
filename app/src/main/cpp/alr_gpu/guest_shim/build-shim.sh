#!/usr/bin/env bash
# build-shim.sh — cross-compile the ALR GLES marshalling shim + cube for the
# aarch64 glibc guest, using zig cc (clang under the hood; zig ships the
# aarch64-linux-gnu sysroot so no external cross toolchain is needed).
#
# Produces, in $OUT (default ./out):
#   libGLESv2.so.2  (SONAME libGLESv2.so.2)  <- GLES2 shim + the shared runtime
#   libEGL.so.1     (SONAME libEGL.so.1)     <- EGL shim, NEEDED: libGLESv2.so.2
#   alr-gles-cube                            <- ordinary GLES2 app, -lEGL -lGLESv2
#
# DESIGN NOTE — single runtime copy. The shared runtime (alr_shim_runtime.c:
# the ring + virtual-ID state + emit helpers) is compiled into libGLESv2.so.2
# ONLY. libEGL.so.1 lists libGLESv2.so.2 as a NEEDED library and resolves the
# alr_shim* symbols from it at load. This guarantees ONE g_state / ONE ring /
# ONE virtual-ID space shared by both shims (rather than two private copies that
# would each attach the ring and diverge). The exported ABI of each .so is just
# its gl*/egl* entry points (+ the alr_shim* glue that libEGL imports); the
# linker is left at default visibility because the set of TUs only defines the
# intended exports anyway.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$HERE/out}"
# Pin glibc 2.34 so pthread/dl are FOLDED INTO libc.so.6 (glibc >= 2.34 merged
# libpthread + libdl into libc). Without the version suffix the .so's get
# DT_NEEDED libpthread.so.0 (and libdl.so.2), which the tiny ALR rootfs does NOT
# ship -> the guest ld.so can't resolve the EGL dlopen chain ("Error loading EGL
# library", CP-2 drain#2). With .2.34 the only NEEDED is libc.so.6. DO NOT drop
# the version suffix.
TARGET="aarch64-linux-gnu.2.34"
CC=(zig cc -target "$TARGET")
CFLAGS=(-std=c11 -O2 -fPIC -Wall -Wextra -fvisibility=default -I"$HERE")

mkdir -p "$OUT"
echo "== zig version =="
zig version

echo "== compile shared runtime (-> libGLESv2) =="
"${CC[@]}" "${CFLAGS[@]}" -c "$HERE/alr_shim_runtime.c" -o "$OUT/alr_shim_runtime.o"

echo "== compile GLES2 shim =="
"${CC[@]}" "${CFLAGS[@]}" -c "$HERE/alr_gles_shim.c" -o "$OUT/alr_gles_shim.o"

echo "== compile EGL shim =="
"${CC[@]}" "${CFLAGS[@]}" -c "$HERE/alr_egl_shim.c" -o "$OUT/alr_egl_shim.o"

echo "== link libGLESv2.so.2 (SONAME libGLESv2.so.2) =="
# NO -lpthread: the glibc-2.34 TARGET pin folds pthread into libc, so the once/mutex
# symbols resolve from libc.so.6 and the ONLY DT_NEEDED is libc.so.6. A libpthread.so.0
# NEEDED would break the guest EGL dlopen chain in the tiny rootfs (CP-2 drain#2).
"${CC[@]}" -shared -fPIC \
    -Wl,-soname,libGLESv2.so.2 \
    "$OUT/alr_gles_shim.o" "$OUT/alr_shim_runtime.o" \
    -o "$OUT/libGLESv2.so.2"
# Dev symlinks so -lGLESv2 / -lEGL find the libs at link time.
ln -sf libGLESv2.so.2 "$OUT/libGLESv2.so"

echo "== link libEGL.so.1 (SONAME libEGL.so.1, NEEDED libGLESv2.so.2) =="
# Link against libGLESv2 so the alr_shim* runtime symbols resolve there (one copy).
"${CC[@]}" -shared -fPIC \
    -Wl,-soname,libEGL.so.1 \
    "$OUT/alr_egl_shim.o" \
    -L"$OUT" -lGLESv2 \
    -o "$OUT/libEGL.so.1"
ln -sf libEGL.so.1 "$OUT/libEGL.so"

echo "== link alr-gles-cube (-lEGL -lGLESv2) =="
"${CC[@]}" -O2 -I"$HERE" \
    "$HERE/alr-gles-cube.c" \
    -L"$OUT" -Wl,-rpath,'$ORIGIN' -lEGL -lGLESv2 -lm \
    -o "$OUT/alr-gles-cube"

echo
echo "== artifacts =="
ls -l "$OUT"
echo
echo "== SONAME / NEEDED verification =="
for so in libGLESv2.so.2 libEGL.so.1; do
    echo "--- $so ---"
    # zig ships llvm-readelf as 'zig' subcommand? fall back to readelf/objdump if present.
    if command -v readelf >/dev/null 2>&1; then
        readelf -d "$OUT/$so" | grep -E 'SONAME|NEEDED' || true
    elif command -v llvm-readelf >/dev/null 2>&1; then
        llvm-readelf -d "$OUT/$so" | grep -E 'SONAME|NEEDED' || true
    else
        echo "(no readelf available; skipping dynamic-tag dump)"
    fi
done
echo
echo "BUILD OK"
