#!/usr/bin/env bash
# Assemble the release tarball  alr-<version>-aarch64.tar.gz  and its SHA256SUMS.
#
# This script REFUSES rather than improvises.  Every byte it ships has to be
# traceable to a pinned toolchain, so there is no fallback compiler, no "close
# enough" manifest, and no tarball with a hole in it.  Three separate incidents
# in docs/evidence/ were "the deployed binary was not what I thought it was";
# a release script that quietly substitutes something is how that becomes
# permanent instead of a bad afternoon.
#
# Two ABI sides land in one tarball (docs/01-platform-facts.md §F1/§F2):
#
#   bin/alr, bin/alr-doctor      HOST  (bionic)  NDK clang, aarch64-linux-android24
#   share/alr/libalr_preload.so  GUEST (glibc)   zig cc,    aarch64-linux-gnu.2.17
#
# The guest .so is an INPUT here, produced by scripts/build-preload.sh.  Building
# it from this script would put both toolchains in one file and make crossing
# them a one-line mistake.  Termux's own on-device clang is a dev inner loop and
# is never a release path -- this script will not use it even if it is in PATH.
#
# Usage:
#     scripts/make-release.sh                 # -> dist/
#     OUT_DIR=/tmp/rel scripts/make-release.sh
#     NDK=/path/to/android-ndk scripts/make-release.sh
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

OUT_DIR=${OUT_DIR:-$REPO/dist}

# Timestamps baked into the tarball.  Fixed by default so two runs from the same
# tree produce the same archive; the repo's reproducibility claim only survives
# if nothing here silently varies.
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-0}

PRELOAD_SO=build/libalr_preload.so
PRELOAD_MANIFEST=build/libalr_preload.manifest.json

# Every file the preload build actually reads.  Both .c files plus the headers
# they include -- alr_path_rule.h in particular IS the path rewrite semantics
# (docs/04-preload-spec.md R6), so a change there with no change to the .c files
# still produces a different .so and must move source_sha256.
PRELOAD_SOURCES="src/preload/alr_preload.c
src/common/alr_elf.c
src/common/alr_elf.h
src/common/alr_path_rule.h
src/common/alr_resolv_proto.h"

# docs/09 "AUTHORITATIVE FACTS": the alr binary is exactly these five files.
ALR_SOURCES="src/cli/alr.c
src/cli/alr_resolvd.c
src/common/alr_exec_rule.c
src/common/alr_elf.c
src/supervisor/alr_supervisor.c"

DOCTOR_SOURCES="src/cli/doctor.c"

# -O1, not -O2: the only alr binaries ever exercised on a real device were built
# at -O1 by scripts/dev-push.sh.  A release is the wrong place to introduce a
# codegen change nobody has run under the zygote seccomp filter.
#
# -Werror is deliberately NOT here.  dev-push.sh's CFLAGS_DEV omits it for these
# sources too; whether they are warning-clean under NDK headers is UNVERIFIED,
# and discovering that for the first time while cutting a release helps no one.
# Warnings are still printed.
HOST_CFLAGS="-O1 -Wall -Wextra -std=c11 -D_GNU_SOURCE -Isrc/common -Isrc/supervisor"

# Android 15 ships 16 KiB pages.  Without these the binary loads on today's
# devices and fails on tomorrow's, which is the worst possible failure shape.
HOST_LDFLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"

NDK_TRIPLE=aarch64-linux-android24

# ── small helpers ────────────────────────────────────────────────────────────

say()  { printf '%s\n' "$*"; }
warn() { printf '%s\n' "$*" >&2; }
die()  { printf 'make-release: %s\n' "$*" >&2; exit 1; }

if command -v sha256sum >/dev/null 2>&1; then
    sha256()       { sha256sum "$1" | cut -d' ' -f1; }
    sha256_stdin() { sha256sum      | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256()       { shasum -a 256 "$1" | cut -d' ' -f1; }
    sha256_stdin() { shasum -a 256      | cut -d' ' -f1; }
else
    die "neither sha256sum nor shasum found; cannot produce SHA256SUMS"
fi

# The canonical source_sha256 recipe, defined here because nothing else defines
# it yet.  scripts/build-preload.sh MUST adopt this exact form or the two will
# disagree forever -- see the manifest failure message below.
#
#   sha256( concat( sha256(file) for file in sort(PRELOAD_SOURCES) ) )
#
# Hashing the per-file digests rather than the concatenated bytes means the
# result does not change if the file order or a trailing newline changes.
preload_source_sha256() {
    local f
    printf '%s\n' "$PRELOAD_SOURCES" | LC_ALL=C sort | while read -r f; do
        [ -n "$f" ] || continue
        sha256 "$f"
    done | sha256_stdin
}

# grep-based JSON probing.  Adding a jq dependency to a release script that
# reads one four-key file is not worth the extra thing that can be absent.
manifest_has()  { grep -q "\"$1\"[[:space:]]*:" "$PRELOAD_MANIFEST"; }
manifest_get()  {
    sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" \
        "$PRELOAD_MANIFEST" | head -1
}

# ── version, from the single source of truth ─────────────────────────────────

[ -f src/common/alr_version.h ] || die "src/common/alr_version.h is missing"

VERSION=$(sed -n \
    's/^#define[[:space:]][[:space:]]*ALR_VERSION[[:space:]][[:space:]]*"\([^"]*\)".*/\1/p' \
    src/common/alr_version.h)

case $(printf '%s\n' "$VERSION" | grep -c .) in
  1) : ;;
  0) die "no ALR_VERSION found in src/common/alr_version.h" ;;
  *) die "multiple ALR_VERSION defines in src/common/alr_version.h; there must be one" ;;
esac

# The termux recipe lives in another repo's world and cannot #include a header,
# so its TERMUX_PKG_VERSION is a hand-kept copy.  alr_version.h says "bump here
# and nowhere else" precisely because copies drift; catch the drift at the one
# moment it matters, when a tarball is about to be published under a version
# number the recipe does not agree with.
TERMUX_RECIPE=packaging/termux/alr/build.sh
if [ -f "$TERMUX_RECIPE" ]; then
    rv=$(sed -n 's/^TERMUX_PKG_VERSION=["'"'"']\{0,1\}\([^"'"'"']*\)["'"'"']\{0,1\}.*/\1/p' \
         "$TERMUX_RECIPE" | head -1)
    if [ "$rv" != "$VERSION" ]; then
        die "version drift: src/common/alr_version.h says $VERSION but
  $TERMUX_RECIPE says ${rv:-<none>}.
  Set TERMUX_PKG_VERSION=\"$VERSION\" there (and its _SRCURL/_SHA256 with it)."
    fi
fi

NAME="alr-$VERSION-aarch64"
TARBALL="$OUT_DIR/$NAME.tar.gz"
STAGE="$OUT_DIR/.stage-$NAME"
# Without this, a failed host compile leaves dist/ containing only a half-built
# staging tree and no tarball -- which contradicts the preflight's promise that
# nothing was written to $OUT_DIR when the release refuses.
trap 'rm -rf "$STAGE"' EXIT INT TERM

say "── alr release $VERSION ──────────────────────────────────────────"

# ── preflight: report EVERY missing component, not just the first ────────────
#
# Finding out one missing piece per run, five runs in a row, is a bad way to
# spend an evening.  Collect the whole list and print it once.

MISSING_N=0
MISSING_TXT=""
missing() {
    MISSING_N=$((MISSING_N + 1))
    MISSING_TXT="${MISSING_TXT}  - $1
      $2
"
}

for f in LICENSE README.md; do
    [ -f "$f" ] || missing "$f" \
        "required in the tarball root; write it at $REPO/$f"
done

[ -f "$PRELOAD_SO" ] || missing "$PRELOAD_SO" \
    "guest glibc interposer; build it with: scripts/build-preload.sh (zig 0.16.0)"
[ -f "$PRELOAD_MANIFEST" ] || missing "$PRELOAD_MANIFEST" \
    "emitted next to the .so by scripts/build-preload.sh"

for f in $ALR_SOURCES $DOCTOR_SOURCES; do
    [ -f "$f" ] || missing "$f" "source file listed in this script is not in the tree"
done

# NDK clang.  Explicitly NOT falling back to whatever `clang` is in PATH: on a
# dev host that is the macOS/Linux clang, which would produce a binary for the
# wrong OS entirely; in Termux it is the on-device clang, which is a dev inner
# loop and not a release toolchain.
NDK=${NDK:-${ANDROID_NDK_HOME:-}}
NDK_CLANG=""
if [ -n "$NDK" ]; then
    for c in "$NDK"/toolchains/llvm/prebuilt/*/bin/clang; do
        [ -x "$c" ] && { NDK_CLANG=$c; break; }
    done
fi
if [ -z "$NDK_CLANG" ]; then
    missing "Android NDK clang" \
"set NDK=/path/to/android-ndk (r29+); looked at \
\${NDK:-\$ANDROID_NDK_HOME}=${NDK:-<unset>}"
fi

if [ "$MISSING_N" -gt 0 ]; then
    warn "make-release: REFUSING to build a release -- $MISSING_N component(s) missing:"
    warn ""
    printf '%s' "$MISSING_TXT" >&2
    warn "Nothing was written to $OUT_DIR."
    exit 1
fi

say "components: present"

# ── manifest validation ──────────────────────────────────────────────────────
#
# docs/04-preload-spec.md §1 and docs/05-provisioning-spec.md §3.4 both fix the
# manifest shape at {zig_version, target, source_sha256, output_sha256}.  That
# file gets copied into the rootfs as <R>/usr/lib/alr/manifest.json and is the
# only thing a bug reporter can quote to say WHICH preload they were running.
# A manifest that cannot answer "from which sources" is not doing its one job,
# so a mismatch is fatal here rather than a warning.

REQUIRED_KEYS="zig_version target source_sha256 output_sha256"
MISSING_KEYS=""
for k in $REQUIRED_KEYS; do
    manifest_has "$k" || MISSING_KEYS="$MISSING_KEYS $k"
done

if [ -n "$MISSING_KEYS" ]; then
    warn "make-release: REFUSING -- $PRELOAD_MANIFEST does not match the spec."
    warn ""
    warn "  required by docs/04-preload-spec.md §1 and docs/05-provisioning-spec.md §3.4:"
    warn "      { zig_version, target, source_sha256, output_sha256 }"
    warn "  missing:$MISSING_KEYS"
    if manifest_has sources && [ "$MISSING_KEYS" = " source_sha256" ]; then
        warn ""
        warn "  What is there instead:"
        warn "      \"sources\": \"$(manifest_get sources)\""
        warn "  That is a path LIST, not a digest.  It records which files were named"
        warn "  on the command line, not what was in them, so it cannot distinguish two"
        warn "  builds of different code -- which is the entire purpose of the field."
    fi
    warn ""
    warn "  Fix in scripts/build-preload.sh (owned by another agent -- do not patch"
    warn "  it from here).  Replace the \"sources\" line in its heredoc with a"
    warn "  \"source_sha256\" computed over EVERY file the build reads:"
    warn ""
    warn "      SRC_ALL=\"src/preload/alr_preload.c src/common/alr_elf.c \\"
    warn "               src/common/alr_elf.h src/common/alr_path_rule.h \\"
    warn "               src/common/alr_resolv_proto.h\""
    warn "      srcsha=\$(for f in \$(printf '%s\\n' \$SRC_ALL | LC_ALL=C sort); do"
    warn "                   shasum -a 256 \"\$f\" 2>/dev/null || sha256sum \"\$f\";"
    warn "               done | cut -d' ' -f1 | { shasum -a 256 2>/dev/null || sha256sum; })"
    warn "      srcsha=\${srcsha%% *}"
    warn ""
    warn "  Headers are in the list on purpose: alr_path_rule.h IS the rewrite"
    warn "  semantics (04-preload-spec R6), so editing it changes the .so while"
    warn "  leaving both .c files untouched."
    warn ""
    warn "  For the tree as it stands right now that recipe yields:"
    warn "      source_sha256 = $(preload_source_sha256)"
    warn ""
    warn "Nothing was written to $OUT_DIR."
    exit 1
fi

# Shape is right -- now check the contents actually describe this .so and this
# tree.  A stale manifest beside a fresh .so is the exact "the binary was not
# what I thought" failure the version command was added to catch.
m_out=$(manifest_get output_sha256)
a_out=$(sha256 "$PRELOAD_SO")
if [ "$m_out" != "$a_out" ]; then
    die "manifest output_sha256 does not describe $PRELOAD_SO
  manifest: $m_out
  actual:   $a_out
  The .so and its manifest came from different builds.  Re-run scripts/build-preload.sh."
fi

m_src=$(manifest_get source_sha256)
a_src=$(preload_source_sha256)
if [ "$m_src" != "$a_src" ]; then
    die "manifest source_sha256 does not describe this checkout
  manifest: $m_src
  tree:     $a_src
  The preload was built from different sources than the ones here.
  Re-run scripts/build-preload.sh before cutting a release."
fi

say "manifest:   ok (zig $(manifest_get zig_version), target $(manifest_get target))"

# ── ELF gates on the guest .so ───────────────────────────────────────────────
#
# stock objdump only: macOS runners have no readelf and no llvm-readelf, and a
# gate that silently does not run is worse than no gate.

OBJDUMP=$(command -v objdump || true)

elf_gates_preload() {
    local fmt verneed_bad stackflags

    if [ -z "$OBJDUMP" ]; then
        warn "make-release: WARNING objdump not found."
        warn "  UNVERIFIED for this tarball: glibc verneed floor, PT_GNU_STACK."
        warn "  The .so is shipped as-is.  Install binutils and re-run to gate it."
        return 0
    fi

    fmt=$("$OBJDUMP" -f "$PRELOAD_SO" | sed -n 's/.*file format \(.*\)/\1/p' | head -1)
    case "$fmt" in
      elf64-littleaarch64|elf64-little) : ;;
      *) die "$PRELOAD_SO is '$fmt', expected elf64-littleaarch64" ;;
    esac

    # Anything above GLIBC_2.17 in DT_VERNEED means the interposer CALLS a symbol
    # it is only allowed to DEFINE (stat/fstatat are 2.33+), or that it will
    # simply not resolve on an older rootfs.  docs/04-preload-spec.md §1.
    verneed_bad=$("$OBJDUMP" -p "$PRELOAD_SO" \
        | awk '/Version References:/{f=1} f' \
        | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' \
        | LC_ALL=C sort -u | grep -v '^GLIBC_2\.17$' || true)
    if [ -n "$verneed_bad" ]; then
        die "$PRELOAD_SO requires glibc symbols above the 2.17 floor:
$(printf '  %s\n' $verneed_bad)
  docs/04-preload-spec.md §1: the 2.17 target exists to make this a link error.
  Fix the call, do not raise the floor."
    fi

    # An RWX PT_GNU_STACK does not merely warn on Android: PROCESS__EXECSTACK is
    # not allowed, so the .so fails to LOAD (docs/01-platform-facts.md §B3).
    # A missing PT_GNU_STACK is treated the same way -- absent means "unknown",
    # and we do not ship unknown here.
    stackflags=$("$OBJDUMP" -p "$PRELOAD_SO" \
        | awk '/^ *STACK /{getline; for(i=1;i<=NF;i++) if($i=="flags"){print $(i+1); exit}}')
    [ -n "$stackflags" ] || die "$PRELOAD_SO has no PT_GNU_STACK; Android needs it present and non-exec"
    case "$stackflags" in
      *x*) die "$PRELOAD_SO has an executable stack (flags $stackflags).
  Android denies PROCESS__EXECSTACK, so this .so would fail to load, not just warn." ;;
    esac

    say "elf gates:  verneed=GLIBC_2.17 only, PT_GNU_STACK=$stackflags"
}

# 16 KiB page alignment on the host binaries.  Checked rather than assumed
# because the failure only shows up on 16 KiB-page devices, i.e. on somebody
# else's phone after the release is out.
elf_gate_pagesize() {
    local bin=$1 minalign
    [ -n "$OBJDUMP" ] || return 0
    # objdump prints "align 2**16"; strip the "2**" textually.  split() with a
    # two-character separator treats it as a REGEX, and "**" is not a valid one
    # -- awk exits with "illegal primary in regular expression".
    minalign=$("$OBJDUMP" -p "$bin" \
        | awk '/^ *LOAD /{for(i=1;i<=NF;i++) if($i=="align"){v=$(i+1); sub(/^2\*\*/,"",v); print v}}' \
        | LC_ALL=C sort -n | head -1)
    [ -n "$minalign" ] || die "$bin has no PT_LOAD segments"
    if [ "$minalign" -lt 14 ]; then
        die "$bin LOAD alignment is 2**$minalign, need >= 2**14 (16 KiB).
  $HOST_LDFLAGS did not take effect."
    fi
}

elf_gates_preload

# ── build the host (bionic) side ─────────────────────────────────────────────

say "host cc:    $NDK_CLANG"
mkdir -p "$OUT_DIR"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/share/alr"

# shellcheck disable=SC2086  # word splitting of the flag/source lists is intended
"$NDK_CLANG" --target="$NDK_TRIPLE" $HOST_CFLAGS $HOST_LDFLAGS \
    -o "$STAGE/bin/alr" $ALR_SOURCES

# shellcheck disable=SC2086
"$NDK_CLANG" --target="$NDK_TRIPLE" $HOST_CFLAGS $HOST_LDFLAGS \
    -o "$STAGE/bin/alr-doctor" $DOCTOR_SOURCES

elf_gate_pagesize "$STAGE/bin/alr"
elf_gate_pagesize "$STAGE/bin/alr-doctor"

# ── stage ────────────────────────────────────────────────────────────────────
#
# No top-level directory inside the archive.  The install is then literally
#     tar -xzf alr-<v>-aarch64.tar.gz -C "$PREFIX"
# and bin/ share/ land exactly where src/cli/alr.c already looks for them:
# prefix() is $PREFIX (default /data/data/com.termux/files/usr) and
# install_preload() probes "$PREFIX/share/alr/libalr_preload.so".
# packaging/termux/alr/build.sh depends on this shape too.

cp "$PRELOAD_SO"       "$STAGE/share/alr/libalr_preload.so"
cp "$PRELOAD_MANIFEST" "$STAGE/share/alr/manifest.json"
cp LICENSE README.md   "$STAGE/"

chmod 0755 "$STAGE/bin/alr" "$STAGE/bin/alr-doctor" "$STAGE/share/alr/libalr_preload.so"
chmod 0644 "$STAGE/share/alr/manifest.json" "$STAGE/LICENSE" "$STAGE/README.md"

MEMBERS="bin/alr
bin/alr-doctor
share/alr/libalr_preload.so
share/alr/manifest.json
LICENSE
README.md"

for m in $MEMBERS; do
    [ -f "$STAGE/$m" ] || die "internal: $m did not reach the staging tree"
done

# ── archive ──────────────────────────────────────────────────────────────────
#
# GNU tar can normalise order, ownership and mtime; bsdtar (the macOS default)
# cannot --sort, so the archive is byte-reproducible only under GNU tar.  Say
# which one happened instead of claiming reproducibility either way.

TAR_REPRO=""
REPRO_NOTE="NOT byte-reproducible (bsdtar: no --sort/--mtime; mtimes vary)"
if tar --version 2>/dev/null | head -1 | grep -q 'GNU tar'; then
    TAR_REPRO="--sort=name --owner=0 --group=0 --numeric-owner --mtime=@$SOURCE_DATE_EPOCH"
    REPRO_NOTE="byte-reproducible (GNU tar, SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH)"
fi

rm -f "$TARBALL"
# gzip -n, not `tar czf`: the gzip header otherwise carries a timestamp and two
# identical trees produce two different files.
# shellcheck disable=SC2086
(cd "$STAGE" && tar $TAR_REPRO -cf - $MEMBERS) | gzip -n -9 > "$TARBALL"

rm -rf "$STAGE"

( cd "$OUT_DIR" && sha256 "$NAME.tar.gz" | \
  { read -r h; printf '%s  %s\n' "$h" "$NAME.tar.gz"; } > SHA256SUMS )

say ""
say "tarball:    $TARBALL"
say "sha256:     $(sha256 "$TARBALL")"
say "sums:       $OUT_DIR/SHA256SUMS"
say "archive:    $REPRO_NOTE"
say ""
say "contents:"
tar -tzf "$TARBALL" | sed 's/^/  /'
say ""
say "install:    tar -xzf $NAME.tar.gz -C \"\$PREFIX\""
