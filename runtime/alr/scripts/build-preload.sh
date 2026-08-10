#!/usr/bin/env bash
# Build the guest-side glibc interposer.
#
# The 2.17 floor is deliberate and load-bearing: it makes the linker REFUSE any
# accidental *call* into the stat family (stat/fstatat are GLIBC_2.33+), which
# the interposer must only DEFINE.  If this build starts failing with an
# undefined reference to stat/fstatat, that is the guard working -- fix the
# call, do not raise the floor.
set -euo pipefail

ZIG=${ZIG:-zig}
ZIG_REQ=0.16.0

# SCOPE OF THE REPRODUCIBILITY CLAIM, measured 2026-08-03 on the v0.2.0 tag:
# byte-identical rebuilds hold PER HOST OS, not across them.  The same sources
# (source_sha256 3a312d54...) and the same zig 0.16.0 produced
#   b30dd81e...  on macOS/arm64
#   16167c4e...  on the ubuntu-latest release runner
# and each host rebuilt its own output identically from a cold cache.  So
# "reproducible" here means "your rebuild matches your rebuild", which is what
# scripts/check-preload.sh gates.  Someone verifying a published release by
# rebuilding needs the SAME host OS as the release job (Linux), otherwise a
# differing hash is expected and is not evidence of tampering.  The shipped
# manifest's output_sha256 always describes the shipped bytes -- that is the
# check docs/INSTALL.md asks the user to make, and it is unaffected.
TARGET=aarch64-linux-gnu.2.17
OUT=${OUT:-build/libalr_preload.so}
SRC="src/preload/alr_preload.c src/common/alr_elf.c"

# ── keep one toolchain's artifacts out of another's cache ───────────────
#
# zig compiles compiler-rt and its libc stubs once and caches them in
# $ZIG_GLOBAL_CACHE_DIR, shared by every zig on the machine.  Two different
# zig 0.16.0 INSTALLS are not the same toolchain -- Homebrew's links against
# Homebrew LLVM, the official tarball bundles its own -- and the second one to
# run happily reuses the first one's objects.
#
# MEASURED 2026-08-04.  Same sources, same `zig version` output of 0.16.0:
#   Homebrew zig                       e46688bd...   .comment: Homebrew clang 21.1.8
#   official tarball, contaminated     6790d602...   .comment: Homebrew clang 21.1.8  (!)
#   official tarball, clean cache      052e7410...   .comment: clang 21.1.0
#   CI (official tarball, Linux)       052e7410...   -- byte-identical
#
# The middle line is the dangerous one: the OFFICIAL compiler produced a binary
# stamped with the OTHER one's identity, because the pieces it linked came from
# the shared cache.  So the version pin above was never enough on its own, and
# the reproducibility gate could not see it -- it gives both of its builds
# their own cache directories, so they agreed with each other while disagreeing
# with every other machine.
#
# Keying the cache on the interpreter's own path and version keeps the speedup
# (compiler-rt is not rebuilt per invocation) while making the reuse impossible
# across installs.  An explicit ZIG_*_CACHE_DIR from the caller still wins;
# check-preload.sh sets both.
if [ -z "${ZIG_GLOBAL_CACHE_DIR:-}" ] && [ -z "${ZIG_LOCAL_CACHE_DIR:-}" ]; then
    _zig_real=$(cd "$(dirname "$ZIG")" 2>/dev/null && pwd -P)/$(basename "$ZIG")
    _zig_id=$(printf '%s\n%s\n' "$_zig_real" "$("$ZIG" version 2>/dev/null)" \
              | { command -v sha256sum >/dev/null 2>&1 && sha256sum || shasum -a 256; } \
              | cut -c1-16)
    ZIG_GLOBAL_CACHE_DIR="${TMPDIR:-/tmp}/alr-zig-cache-$_zig_id/global"
    ZIG_LOCAL_CACHE_DIR="${TMPDIR:-/tmp}/alr-zig-cache-$_zig_id/local"
    export ZIG_GLOBAL_CACHE_DIR ZIG_LOCAL_CACHE_DIR
fi

have=$("$ZIG" version)
if [ "$have" != "$ZIG_REQ" ]; then
    echo "zig $ZIG_REQ required, found $have." >&2
    echo "Reproducibility only holds under an exact pin: a zig upgrade changes" >&2
    echo "the bundled compiler-rt and LLVM codegen." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"

# -D_FORTIFY_SOURCE=0 is NOT optional: distro/CI default CFLAGS often add
# -D_FORTIFY_SOURCE=2, which would make this library call into the very
# __*_chk symbols it defines -- infinite self-recursion.
"$ZIG" cc --target="$TARGET" \
    -shared -fPIC -O2 -D_GNU_SOURCE \
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -fno-stack-protector -fvisibility=default \
    -Wall -Wextra -Werror \
    `# The nonnull-attributed params below ARE checked for NULL on purpose:` \
    `# glibc declares them nonnull, but real callers pass NULL anyway and a` \
    `# preload that segfaults is far worse than one returning EINVAL. Keeping` \
    `# the checks is deliberate, so the diagnostic is suppressed rather than` \
    `# the code weakened. docs/04-preload-spec.md §1 mandates -Werror.` \
    -Wno-pointer-bool-conversion \
    `# Reproducible ACROSS MACHINES, which is what the gate's contract says:` \
    `# "a bug report quoting the sha256 is only actionable if anyone can` \
    `# rebuild the same bytes from the same tag".  Without this the build` \
    `# ROOT is embedded, and v0.4.0's .so built on a laptop hashed` \
    `# a3f3ed7b... against CI's 3b05ad6d... from identical sources --` \
    `# 50,737 differing bytes, all of them "/Users/naen/Git/Android on` \
    `# Linux/alr-termux" vs "/home/runner/work/alr-termux/alr-termux".` \
    `# -ffile-prefix-map covers debug info and __FILE__ together.` \
    -ffile-prefix-map="$PWD"=. \
    `# clang takes DW_AT_comp_dir from the CWD, and whether -ffile-prefix-map` \
    `# covers it has varied across clang versions.  Set it explicitly so the` \
    `# answer does not depend on which clang zig bundles this month.` \
    -fdebug-compilation-dir=. \
    `# The build-id is a hash of the content, so it is deterministic -- but` \
    `# only if everything feeding it already is.  Dropping it removes one` \
    `# more thing to reason about in an artifact nobody debugs from a core.` \
    -Wl,--build-id=none \
    `# AND DROP DWARF ENTIRELY.  -ffile-prefix-map only rewrites paths under` \
    `# $PWD; the debug info also names the TOOLCHAIN's own headers, and CI` \
    `# unpacks zig INTO the checkout, so those landed inside $PWD for a build` \
    `# run from the repo root and outside it for one run from anywhere else:` \
    `#   build 1:  zig-x86_64-linux-0.16.0/lib/include` \
    `#   build 2:  /home/runner/work/alr-termux/alr-termux/zig-.../lib/include` \
    `# Chasing that with more prefix maps means tracking where each machine` \
    `# keeps its compiler.  A shipped interposer is not debugged from a core` \
    `# dump -- it is diagnosed with ALR_LOG -- so the DWARF buys nothing and` \
    `# costs the one property the printed sha256 exists to provide.` \
    -Wl,--strip-debug \
    -I src/common \
    ${ALR_CFLAGS_EXTRA:-} \
    -o "$OUT" $SRC

if command -v sha256sum >/dev/null 2>&1; then
    _sha()       { sha256sum "$1" | cut -d' ' -f1; }
    _sha_stdin() { sha256sum      | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    _sha()       { shasum -a 256 "$1" | cut -d' ' -f1; }
    _sha_stdin() { shasum -a 256      | cut -d' ' -f1; }
else
    echo "neither sha256sum nor shasum found" >&2; exit 2
fi

sha=$(_sha "$OUT")
# docs/04-preload-spec.md §1 and docs/05-provisioning-spec.md §3.4 require
# source_sha256 -- a digest of the inputs, not the path list that used to be
# emitted here.  Without it a manifest cannot answer "was this .so built from
# THIS tree", which is the whole point of shipping one.
#
# The recipe and the file list are CANONICALLY defined in scripts/make-release.sh
# (preload_source_sha256 / PRELOAD_SOURCES) and reproduced verbatim here:
#
#     sha256( concat( sha256(file) for file in sort(SOURCES) ) )
#
# Sorting the file NAMES and hashing only the digests keeps the result stable
# against file order.  The first v0.1.0 tag failed because these two disagreed
# -- this side sorted the digest LINES and folded the file names in.
SRC_ALL="src/preload/alr_preload.c
src/common/alr_elf.c
src/common/alr_elf.h
src/common/alr_path_rule.h
src/common/alr_resolv_proto.h"
srcsha=$(printf '%s\n' "$SRC_ALL" | LC_ALL=C sort | while read -r f; do
             [ -n "$f" ] || continue
             _sha "$f"
         done | _sha_stdin)
# The compiler's own identity, read back out of the .so's .comment section.
#
# "zig_version": "0.16.0" is NOT enough to identify a toolchain: Homebrew's zig
# links against Homebrew LLVM and the official tarball bundles its own, and
# both answer 0.16.0.  They produce different bytes -- correctly, they are
# different compilers -- and without this field a hash mismatch between two
# machines has no explanation attached to it.
cc_id=$(strings "$OUT" 2>/dev/null | grep -m1 -i "clang version" || true)
: "${cc_id:=unknown}"

cat > "${OUT%.so}.manifest.json" <<EOF
{
  "zig_version": "$have",
  "cc_identity": "$cc_id",
  "target": "$TARGET",
  "sources": "$SRC",
  "source_sha256": "$srcsha",
  "output_sha256": "$sha"
}
EOF

echo "built $OUT"
echo "  $(grep -oE 'GLIBC_[0-9.]+' "$OUT" | sort -uV | tr '\n' ' ')"
