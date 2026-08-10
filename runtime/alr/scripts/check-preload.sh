#!/usr/bin/env bash
# Device-free release gates for the guest-side interposer.
#
# Everything here is checkable on a macOS or Linux runner with no phone
# attached, which is the whole point: the interesting failures below are all
# SILENT on the device.  A wrong-arch .so, a raised glibc floor or a stale
# manifest do not crash anything -- they degrade into "the rootfs looks
# broken", which is the most expensive class of bug this project has hit.
#
# Deliberately uses objdump and nothing else.  readelf / llvm-readelf are not
# installed on a stock macOS runner, and eu-readelf is not installed anywhere
# by default; objdump is present on both sides.  Note that on an x86 Linux
# runner binutils reads this aarch64 object through its generic `elf64-little`
# backend -- program headers and the dynamic section are arch-independent, so
# the gates below work without binutils-multiarch.  The e_machine check does
# not go through objdump at all, for the same portability reason.
#
#   bash scripts/check-preload.sh          # or: make check
#
# Exit 0 iff every gate passed.
set -u

cd "$(dirname "$0")/.." || exit 2

# Exported, not just assigned: scripts/build-preload.sh reads $ZIG too, and a
# gate that version-checks one zig while the build uses another is a gate that
# lies.
ZIG=${ZIG:-zig}
export ZIG
ZIG_REQ=0.16.0
OBJDUMP=${OBJDUMP:-objdump}

pass=0; fail=0; skip=0

emit() { # emit <NAME> <PASS|FAIL|SKIP> [detail]
    printf '%-38s %s' "$1:" "$2"
    if [ $# -ge 3 ]; then printf '   %s' "$3"; fi
    printf '\n'
    case "$2" in
        PASS) pass=$((pass+1));;
        FAIL) fail=$((fail+1));;
        SKIP) skip=$((skip+1));;
    esac
}

# A pipeline's exit status is the LAST command's, so `shasum … | awk || sha256sum`
# never reaches the fallback: awk exits 0 even when shasum is absent, and the
# function then returns the empty string.  Two empty strings compare equal, so
# the reproducibility gate would print PASS on a host that cannot hash at all --
# a gate reporting PASS when it could not run is exactly what this file forbids.
# Probe for the tool first, then define the function around it.
if command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
elif command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | awk '{print $1}'; }
else
    echo "neither shasum nor sha256sum found; cannot verify anything" >&2
    exit 2
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/alr-check-preload.XXXXXX") || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM

echo "── preload release gates ─────────────────────────────────────"

# ── toolchain pin ───────────────────────────────────────────────────────
# Not a nicety: the reproducibility gate below is meaningless without it,
# because a zig upgrade changes the bundled compiler-rt and LLVM codegen and
# would simply produce a different -- still self-consistent -- hash.
have=$("$ZIG" version 2>/dev/null)
if [ "${have:-}" = "$ZIG_REQ" ]; then
    emit "PRELOAD ZIG PIN" PASS "zig $have"
else
    emit "PRELOAD ZIG PIN" FAIL "found '${have:-none}' want $ZIG_REQ"
    echo
    echo "REFUSING: every gate below builds the .so, so none of them can run."
    echo "ALR PRELOAD GATES: FAIL"
    exit 2
fi

# ── build twice, from two cold caches ───────────────────────────────────
# Isolated ZIG_*_CACHE_DIRs are what make the second build evidence rather
# than a cache hit: with the shared cache, "identical" would only prove that
# zig recognised the same inputs, not that it recompiles to the same bytes.
build() { # build <n>  -> $TMP/<n>/libalr_preload.so
    ZIG_LOCAL_CACHE_DIR="$TMP/cache-$1/local" \
    ZIG_GLOBAL_CACHE_DIR="$TMP/cache-$1/global" \
    OUT="$TMP/$1/libalr_preload.so" \
        bash scripts/build-preload.sh >"$TMP/build-$1.log" 2>&1
}

# build_elsewhere <n> -- same sources, DIFFERENT directory.
#
# build() above only varies the output path, so for the life of this gate it
# proved determinism and nothing more.  The contract it prints is stronger:
# "anyone can rebuild the same bytes from the same tag" -- across machines,
# where the checkout is not at the same path.  That was FALSE and the gate
# could not see it: v0.4.0's .so hashed a3f3ed7b on a laptop and 3b05ad6d in
# CI from identical sources, 50,737 bytes apart, every one of them the
# embedded build root.  Fixed with -ffile-prefix-map; this is the check that
# would have caught it, and will catch the next thing that leaks an absolute
# path (a __FILE__, a -I, an embedded compiler note).
build_elsewhere() {
    local d="$TMP/elsewhere/a-different-checkout-path"
    mkdir -p "$d" || return 1
    cp -R src scripts "$d/" || return 1
    ( cd "$d" &&
      ZIG_LOCAL_CACHE_DIR="$TMP/cache-$1/local" \
      ZIG_GLOBAL_CACHE_DIR="$TMP/cache-$1/global" \
      OUT="$TMP/$1/libalr_preload.so" \
          bash scripts/build-preload.sh ) >"$TMP/build-$1.log" 2>&1
}

if build 1; then
    emit "PRELOAD BUILD" PASS "$(sha256 "$TMP/1/libalr_preload.so" | cut -c1-16)…"
else
    emit "PRELOAD BUILD" FAIL "see below"
    sed 's/^/    /' "$TMP/build-1.log"
    echo
    echo "ALR PRELOAD GATES: FAIL"
    exit 1
fi
SO="$TMP/1/libalr_preload.so"
MANIFEST="$TMP/1/libalr_preload.manifest.json"

# ── ELF identity ────────────────────────────────────────────────────────
# A host-arch .so is the nastiest failure mode in this file: the glibc loader
# does not abort on a wrong-ELFCLASS/EM preload, it warns and carries on.  The
# guest then boots with no path virtualization at all and resolves /etc, PATH
# and everything else against ANDROID -- indistinguishable from a corrupt
# rootfs unless you already suspect the .so.
b=$(od -An -v -tx1 -N 20 "$SO" | awk '{for (i=1;i<=NF;i++) printf "%s ", $i}')
# shellcheck disable=SC2086
set -- $b
if [ $# -ge 20 ]; then
    magic="$1$2$3$4"; eclass="$5"; edata="$6"
    etype="${18}${17}"; emachine="${20}${19}"   # both little-endian halfwords
else
    magic=""; eclass=""; edata=""; etype=""; emachine=""
fi
if [ "$magic" = "7f454c46" ] && [ "$eclass" = "02" ] && [ "$edata" = "01" ] \
   && [ "$etype" = "0003" ] && [ "$emachine" = "00b7" ]; then
    emit "PRELOAD ELF IDENTITY" PASS "ELF64 LSB ET_DYN EM_AARCH64"
else
    emit "PRELOAD ELF IDENTITY" FAIL \
        "magic=$magic class=$eclass data=$edata type=$etype machine=$emachine"
fi

# ── objdump availability ────────────────────────────────────────────────
if "$OBJDUMP" -p "$SO" >"$TMP/objdump.txt" 2>"$TMP/objdump.err"; then
    emit "PRELOAD OBJDUMP READS SO" PASS "$("$OBJDUMP" --version 2>/dev/null | head -1)"
    have_objdump=1
else
    emit "PRELOAD OBJDUMP READS SO" FAIL "$(head -1 "$TMP/objdump.err")"
    have_objdump=0
fi

# ── glibc verneed floor ─────────────────────────────────────────────────
# The 2.17 floor is a CALL guard, not a compatibility knob.  stat/fstatat are
# GLIBC_2.33+ symbols that this library must only DEFINE; the moment it also
# *calls* one, a GLIBC_2.33 verneed appears here.  On the device that reads as
# a stat interposition that silently self-recurses into libc.
if [ "$have_objdump" = 1 ]; then
    vers=$(awk '
        /^Version References:/ { inref = 1; next }
        inref && /^[^[:space:]]/ { inref = 0 }
        inref { for (i = 1; i <= NF; i++) if ($i ~ /^GLIBC_/) print $i }
    ' "$TMP/objdump.txt" | sort -u | tr '\n' ' ')
    vers=${vers% }
    if [ "$vers" = "GLIBC_2.17" ]; then
        emit "PRELOAD GLIBC VERNEED FLOOR" PASS "$vers"
    else
        emit "PRELOAD GLIBC VERNEED FLOOR" FAIL "got '${vers:-none}' want 'GLIBC_2.17'"
    fi
else
    emit "PRELOAD GLIBC VERNEED FLOOR" FAIL "objdump could not read the .so"
fi

# ── PT_GNU_STACK ────────────────────────────────────────────────────────
# The loader takes the UNION of PT_GNU_STACK across the whole image, so one X
# bit here makes every guest process' stack executable -- a property nothing
# in the target workload needs, and one an Android app domain may refuse
# outright rather than merely dislike.  An absent header is also a FAIL: the
# default is then the kernel's, which is not ours to assume.
if [ "$have_objdump" = 1 ]; then
    sflags=$(awk '
        /^[[:space:]]*STACK off/ {
            getline
            for (i = 1; i <= NF; i++) if ($i == "flags") { print $(i+1); exit }
        }
    ' "$TMP/objdump.txt")
    case "${sflags:-}" in
        "")   emit "PRELOAD GNU_STACK NOEXEC" FAIL "no PT_GNU_STACK program header";;
        *x*)  emit "PRELOAD GNU_STACK NOEXEC" FAIL "flags $sflags";;
        *)    emit "PRELOAD GNU_STACK NOEXEC" PASS "flags $sflags";;
    esac
else
    emit "PRELOAD GNU_STACK NOEXEC" FAIL "objdump could not read the .so"
fi

# ── reproducible build ──────────────────────────────────────────────────
# The user-visible contract: `alr version` prints the preload sha256, and a
# bug report quoting it is only actionable if anyone can rebuild the same
# bytes from the same tag.
h1=$(sha256 "$SO")
if build_elsewhere 2; then
    h2=$(sha256 "$TMP/2/libalr_preload.so")
    if [ "$h1" = "$h2" ]; then
        # Print the compiler identity beside the hash.  The two belong
        # together: "zig 0.16.0" names two different toolchains (Homebrew's
        # links Homebrew LLVM, the official tarball bundles its own) that
        # legitimately produce different bytes, and a hash quoted without it
        # cannot be matched against anyone else's.
        cc=$(strings "$SO" 2>/dev/null | grep -m1 -i "clang version" || true)
        emit "PRELOAD REPRODUCIBLE" PASS "sha256=$h1"
        echo "    another path: same bytes.   cc: ${cc:-unknown}"
    else
        emit "PRELOAD REPRODUCIBLE" FAIL "build1=$h1 build2=$h2"
        echo "    The second build ran from a different directory, so a"
        echo "    difference is almost always an embedded absolute path."
        # Say WHAT differs.  A gate that reports two hashes and leaves the
        # cause to be guessed at costs a CI round trip per hypothesis; this
        # one cost several before the diagnostics were added.
        echo "    first differing byte:"
        cmp -l "$SO" "$TMP/2/libalr_preload.so" 2>/dev/null | head -1 | sed 's/^/      /'
        echo "    strings present in one build and not the other:"
        strings "$SO" 2>/dev/null | sort -u > "$TMP/s1.txt"
        strings "$TMP/2/libalr_preload.so" 2>/dev/null | sort -u > "$TMP/s2.txt"
        diff "$TMP/s1.txt" "$TMP/s2.txt" 2>/dev/null | head -12 | sed 's/^/      /'
        echo "    sizes: $(wc -c < "$SO") vs $(wc -c < "$TMP/2/libalr_preload.so")"
    fi
else
    emit "PRELOAD REPRODUCIBLE" FAIL "second build did not complete"
    sed 's/^/    /' "$TMP/build-2.log"
fi

# ── manifest agrees with the bytes ──────────────────────────────────────
# manifest.json ships in the tarball as share/alr/manifest.json and is what a
# packager diffs against.  A manifest that disagrees with the .so beside it is
# worse than no manifest: it turns the one verifiable claim into a false one.
if [ -f "$MANIFEST" ]; then
    msha=$(sed -n 's/.*"output_sha256"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$MANIFEST")
    mzig=$(sed -n 's/.*"zig_version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MANIFEST")
    mtgt=$(sed -n 's/.*"target"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MANIFEST")
    if [ "$msha" = "$h1" ] && [ "$mzig" = "$ZIG_REQ" ] \
       && [ "$mtgt" = "aarch64-linux-gnu.2.17" ]; then
        emit "PRELOAD MANIFEST AGREES" PASS "$mtgt zig $mzig"
    else
        emit "PRELOAD MANIFEST AGREES" FAIL \
            "sha=${msha:-none} zig=${mzig:-none} target=${mtgt:-none}"
    fi
else
    emit "PRELOAD MANIFEST AGREES" FAIL "no manifest written next to the .so"
fi

echo
# --- PRELOAD CHK SYMBOLS PRESENT (docs/04-preload-spec.md §14) ---------------
# The gate that would have caught six missing exec entry points and eighteen
# other §6 symbols.  wrappers.def records what the SPEC requires; this reports
# the difference against what the binary actually defines.  A list dumped from
# the binary would be a tautology -- it must be authored from the spec.
DEF="src/preload/wrappers.def"   # cwd is the repo root (line 23)
if [ ! -f "$DEF" ]; then
    emit "PRELOAD CHK SYMBOLS PRESENT" FAIL "src/preload/wrappers.def missing"
else
    want=$(grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$DEF" | LC_ALL=C sort -u)
    have=$(nm -D --defined-only "$SO" 2>/dev/null \
           | awk '$(NF-1) ~ /^[TWiDB]$/ {print $NF}' | LC_ALL=C sort -u)
    absent=$(comm -23 <(printf '%s\n' "$want") <(printf '%s\n' "$have"))
    if [ -n "$absent" ]; then
        emit "PRELOAD CHK SYMBOLS PRESENT" FAIL \
             "declared in wrappers.def but not defined: $(printf '%s' "$absent" | tr '\n' ' ')"
    else
        emit "PRELOAD CHK SYMBOLS PRESENT" PASS \
             "$(printf '%s\n' "$want" | wc -l | tr -d ' ') symbols"
    fi
fi

# --- PRELOAD MANIFEST SOURCE DIGEST -----------------------------------------
# scripts/make-release.sh and scripts/build-preload.sh must agree on what
# source_sha256 means.  They did not, and the first v0.1.0 tag died in the
# release job because of it -- a drift that only surfaced at release time.
# Recompute the canonical recipe here so a push catches it instead:
#     sha256( concat( sha256(file) for file in sort(SOURCES) ) )
DEFSRC="src/preload/alr_preload.c
src/common/alr_elf.c
src/common/alr_elf.h
src/common/alr_path_rule.h
src/common/alr_resolv_proto.h"
want_src=$(printf '%s\n' "$DEFSRC" | LC_ALL=C sort | while read -r f; do
               [ -n "$f" ] || continue
               sha256 "$f"
           done | { if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi; } \
           | cut -d' ' -f1)
got_src=$(sed -n 's/.*"source_sha256"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
              "$MANIFEST" | head -1)
if [ -z "$got_src" ]; then
    emit "PRELOAD MANIFEST SOURCE DIGEST" FAIL "manifest has no source_sha256"
elif [ "$got_src" = "$want_src" ]; then
    emit "PRELOAD MANIFEST SOURCE DIGEST" PASS "${want_src%"${want_src#????????????????}"}…"
else
    emit "PRELOAD MANIFEST SOURCE DIGEST" FAIL \
         "manifest=$got_src tree=$want_src"
fi

echo "──────────────────────────────────────────────────────────────"
echo "  PASS=$pass  FAIL=$fail  SKIP=$skip"
echo "ALR PRELOAD GATES: $([ $fail -eq 0 ] && echo PASS || echo FAIL)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
