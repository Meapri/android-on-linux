#!/usr/bin/env bash
# build-reentry.sh — cross-compile the ALR static re-entry stub (ADR-003-v2
# Option S) for the aarch64 guest, using zig cc (clang under the hood; zig ships
# the cross sysroot so no external toolchain is needed). Mirrors the guest_shim's
# build-shim.sh structure but targets a FREESTANDING, STATICALLY-linked ELF.
#
# Produces, in $OUT (default ./out):
#   alr-reentry   <- static aarch64 ELF, NO PT_INTERP, no libc dependency.
#
# WHY freestanding + static + no-pie
# ==================================
#  * The whole point of the stub is that the KERNEL can execve it. A glibc/musl
#    DYNAMIC ELF carries a PT_INTERP the Android kernel must resolve — exactly the
#    failure (v139 drain#17) this stub exists to bypass. So: NO PT_INTERP.
#      - `-target aarch64-linux-none`  : no libc target -> no implicit DT_NEEDED.
#      - `-nostdlib -ffreestanding`    : no crt0/libc; our own _start + raw svc.
#      - `-static`                     : no dynamic linker reference at all.
#      - `-no-pie`                     : ET_EXEC at a fixed low base; the guest's
#                                        own mappings live high, so no collision,
#                                        and there is zero self-relocation to do
#                                        before _start runs (a PIE would need its
#                                        own IRELATIVE/RELATIVE applied first).
#  * The stub then re-maps the guest ld.so + target IN-PROCESS and jumps — it IS
#    the ALR loader, repackaged as a kernel-executable bootstrap.
#
# DEPLOY (integration session, NOT this PoC): stage the artifact into the rootfs at
#   <rootfs>/usr/lib/androlinux/alr-reentry
# and have the supervisor rewrite a guest execve(target,argv,envp) into
#   execve(<that path>, [<that path>, target, argv[1..]...], envp).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-$HERE/out}"

# No-libc freestanding aarch64. (No glibc version pin like the shim — we link
# nothing, so there is no libc SONAME to fold.)
TARGET="aarch64-linux-none"
CC=(zig cc -target "$TARGET")
# -fno-stack-protector: no __stack_chk_* symbols (no libc to provide them).
# -fno-builtin-memcpy/memset: keep our freestanding m_cpy/m_set; do not let the
#   compiler lower a struct copy into a libc memcpy call we cannot resolve.
# -fcf-protection=none / -mbranch-protection=none: no PAC/BTI relocs the bare
#   loader path would have to honor.
CFLAGS=(
  -std=c11 -O2 -Wall -Wextra
  -ffreestanding -fno-stack-protector -fno-builtin
  -fno-stack-clash-protection
  -mbranch-protection=none
  -nostdlib -static -no-pie
  -Wl,-e,_start
  -I"$HERE"
)

mkdir -p "$OUT"
echo "== zig version =="
zig version

echo "== compile + link alr-reentry (freestanding static aarch64, no PT_INTERP) =="
"${CC[@]}" "${CFLAGS[@]}" "$HERE/alr_reentry.c" -o "$OUT/alr-reentry"

echo
echo "== artifacts =="
ls -l "$OUT/alr-reentry"

echo
echo "== ELF header / arch (objdump -f) =="
# objdump is the LLVM tool that actually understands aarch64 on this host; the
# repo's readelf wrapper only implements -l/-d (program headers + INTERP detect).
objdump -f "$OUT/alr-reentry"

echo
echo "== program headers — CONFIRM NO INTERP (readelf -l) =="
# The wrapper appends a "[Requesting program interpreter: ...]" line iff a .interp
# exists. Its ABSENCE here is the proof the kernel needs no interpreter.
if command -v readelf >/dev/null 2>&1; then
  readelf -l "$OUT/alr-reentry"
else
  objdump -p "$OUT/alr-reentry"
fi

echo
echo "== dynamic section — CONFIRM NONE / no NEEDED (readelf -d) =="
if command -v readelf >/dev/null 2>&1; then
  readelf -d "$OUT/alr-reentry" 2>&1 | grep -iE "NEEDED|Dynamic" || true
else
  objdump -p "$OUT/alr-reentry" | grep -iE "NEEDED|DYNAMIC" || true
fi

echo
# Hard gate: fail the build if a PT_INTERP somehow crept in (would defeat the
# entire purpose — a kernel-unresolvable interpreter).
if objdump -p "$OUT/alr-reentry" 2>/dev/null | grep -qi "INTERP"; then
  echo "FATAL: alr-reentry has a PT_INTERP — it would NOT be kernel-execve'able. ABORT." >&2
  exit 1
fi
# Hard gate: must be aarch64.
if ! objdump -f "$OUT/alr-reentry" 2>/dev/null | grep -qi "aarch64"; then
  echo "FATAL: alr-reentry is not aarch64." >&2
  exit 1
fi

echo "BUILD OK — static aarch64, no PT_INTERP, kernel-execve'able."
