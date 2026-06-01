#!/usr/bin/env bash
# build-wire-check.sh — off-device wire-format cross-check (the verification the
# README describes; sources restored by WS-2). Proves the GUEST shim's encoder
# (alr_gles_shim.c) produces bytes the COMMITTED host decoder (alr_gpu_decode.hpp)
# reads back correctly — the one seam the device probes (which use the host-side
# Encoder, not the guest shim) never exercise.
#
#   STAGE 1  wc_emit:  real alr_gles_shim.c + linear-buffer stub runtime (wc_emit.c)
#                      -> drive the cube GL sequence -> dump wire bytes.
#   STAGE 2  wc_decode: committed alr_gpu_decode.hpp + recording GL stubs
#                      (decode_check.cpp) -> decode the bytes -> assert they match.
#
# Host-native (runs on macOS/Linux with cc/c++); needs no device, NDK, or GL driver.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_CPP="$(cd "$HERE/../.." && pwd)"     # app/src/main/cpp (so "alr_gpu/..." resolves)
OUT="${OUT:-/tmp/wc_build}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
mkdir -p "$OUT"

echo "== stage 1: build wc_emit (real shim + stub runtime) =="
"$CC"  -std=c11 -O2 -Wall -Wextra -I"$HERE" -c "$HERE/alr_gles_shim.c" -o "$OUT/shim.o"
"$CC"  -std=c11 -O2 -Wall -Wextra -I"$HERE" -c "$HERE/wc_emit.c"       -o "$OUT/wc_emit.o"
"$CC"  "$OUT/wc_emit.o" "$OUT/shim.o" -lpthread -o "$OUT/wc_emit"

echo "== run wc_emit (capture the guest shim's wire bytes) =="
"$OUT/wc_emit" "$OUT/stream.bin"

echo "== stage 2: build wc_decode (committed decoder + recording stubs) =="
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$HERE/stubinc" -I"$REPO_CPP" \
    -c "$HERE/decode_check.cpp" -o "$OUT/wc_decode.o"
"$CXX" "$OUT/wc_decode.o" -o "$OUT/wc_decode"

echo "== run wc_decode (decode + assert round-trip) =="
"$OUT/wc_decode" "$OUT/stream.bin"
