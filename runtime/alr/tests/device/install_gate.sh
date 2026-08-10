#!/data/data/com.termux/files/usr/bin/bash
# The install path has no automated coverage, and that is the largest hole in
# this repo's testing.
#
# tests/device/acceptance.sh exits 2 unless the rootfs already exists
# (acceptance.sh's own validity gate), so every one of its 114 checks measures
# a rootfs provisioned by an EARLIER binary.  A change that broke extraction,
# mangled /etc, or failed to place the preload would leave that number
# untouched.  This repo's most expensive recurring bug is "the rootfs was not
# what I thought" -- and nothing ran the code that builds it.
#
# This provisions a throwaway rootfs from the CACHED tarball (no network) and
# asserts properties of the RESULT, then deliberately breaks two things and
# asserts the failures are loud.  It is separate from acceptance.sh because it
# costs a full extraction; run it when cmd_install changes.
#
#   ALR_SSH_KEY=<key> ./scripts/dev-push.sh install-gate
set -u

cd "$(dirname "$0")/../.." 2>/dev/null || true
ALR=${ALR:-./alr}
CACHE="$PREFIX/var/lib/alr/cache"
SRC="$CACHE/ubuntu-24.04.tar.gz"
GATE_ROOT="$HOME/alr-gate"
NAME=alrgate
G="$GATE_ROOT/$NAME"

pass=0; fail=0; skip=0
emit() {
    printf '%-40s %s' "$1:" "$2"
    [ $# -ge 3 ] && printf '   %s' "$3"
    printf '\n'
    case "$2" in PASS) pass=$((pass+1));; FAIL) fail=$((fail+1));; SKIP) skip=$((skip+1));; esac
}

cleanup() { rm -rf "$GATE_ROOT" "$CACHE/$NAME.tar.gz" "$CACHE/$NAME-trunc.tar.gz" 2>/dev/null; }
trap cleanup EXIT INT TERM

# ── validity gate: same rule as every other harness here ────────────────
uid=$(id -u)
sec=$(grep -oE '^Seccomp:[[:space:]]*[0-9]+' /proc/self/status | tr -dc 0-9)
if [ "${uid:-0}" -lt 10000 ] || [ "${sec:-0}" != "2" ]; then
    echo "REFUSING: not a measurable context (uid=$uid Seccomp=$sec)."
    exit 2
fi
[ -x "$ALR" ] || { echo "alr not built: $ALR"; exit 2; }

# A missing input is a REFUSAL, not a SKIP: a token on something that was never
# run is exactly what docs/07-acceptance.md:39 forbids.
if [ ! -r "$SRC" ]; then
    echo "REFUSING: no cached tarball at $SRC"
    echo "  seed it with:  alr install ubuntu-24.04"
    exit 2
fi
# Never let this point at the real rootfs.
if [ "$GATE_ROOT" = "${ALR_ROOT_DIR:-$HOME/alr-distros}" ]; then
    echo "REFUSING: GATE_ROOT equals the live ALR_ROOT_DIR; this harness deletes it"
    exit 2
fi

echo "── alr install gate ─────────────────────────────────────────"
echo "  source $SRC"
echo "  target $G   (deleted on exit)"
echo

cleanup
mkdir -p "$GATE_ROOT"

# ── 1. a real install, from the cache, no network ───────────────────────
# --url makes cmd_install skip release discovery entirely, and the distro name
# keys the cache entry, so `alrgate` forces a fetch of the file:// URL rather
# than silently reusing the ubuntu-24.04 entry.
ALR_ROOT_DIR="$GATE_ROOT" "$ALR" install "$NAME" --url "file://$SRC" \
    >"$GATE_ROOT/install.log" 2>&1
rc=$?
[ "$rc" = 0 ] && emit "INSTALL EXIT" PASS || emit "INSTALL EXIT" FAIL "rc=$rc"

# ── the nine-line first-boot report (docs/05 §4) ────────────────────────
# All nine names, in order, or the report is not the report.  Checked by NAME
# and not by counting lines: a report that dropped BOOT and printed something
# else twice would have the same line count.
missing=
for n in "INSTALL DOWNLOAD" "INSTALL VERIFY SHA256" "INSTALL EXTRACT" \
         "INSTALL REPAIR" "INSTALL LDSO PRESENT" "INSTALL LDSO OPTIONS" \
         "INSTALL BOOT /bin/true" "INSTALL BOOT /bin/echo" \
         "INSTALL GLIBC VERSION"; do
    grep -q "^$n:" "$GATE_ROOT/install.log" || missing="$missing $n"
done
[ -z "$missing" ] && emit "INSTALL REPORT NINE LINES" PASS \
                  || emit "INSTALL REPORT NINE LINES" FAIL "absent:$missing"

# The two that are checks rather than restatements of what just happened: the
# loader must offer the four options ADR 0002 depends on, and the rootfs must
# actually execute something through the full `alr run` path.
grep -q "^INSTALL LDSO OPTIONS: *PASS  argv0=yes preload=yes library-path=yes inhibit-cache=yes" \
     "$GATE_ROOT/install.log" \
    && emit "INSTALL REPORT LDSO OPTIONS" PASS \
    || emit "INSTALL REPORT LDSO OPTIONS" FAIL \
            "$(grep -m1 "^INSTALL LDSO OPTIONS" "$GATE_ROOT/install.log")"
grep -q "^INSTALL BOOT /bin/echo: *PASS  stdout=\"alr\"" "$GATE_ROOT/install.log" \
    && emit "INSTALL REPORT BOOT ECHO" PASS \
    || emit "INSTALL REPORT BOOT ECHO" FAIL \
            "$(grep -m1 "^INSTALL BOOT /bin/echo" "$GATE_ROOT/install.log")"

# An unverified download must not be laundered into PASS.  This install used
# --url, so there is no digest and the only honest token is SKIP.
grep -q "^INSTALL VERIFY SHA256: *SKIP" "$GATE_ROOT/install.log" \
    && emit "INSTALL REPORT UNVERIFIED IS SKIP" PASS \
    || emit "INSTALL REPORT UNVERIFIED IS SKIP" FAIL \
            "$(grep -m1 "^INSTALL VERIFY SHA256" "$GATE_ROOT/install.log")"

# Hardlink members are the ones that broke silently before: tar cannot create
# them under Android's SELinux policy and M3 recorded these exact two vanishing.
for f in usr/bin/uncompress usr/bin/perl5.38.2; do
    [ -f "$G/$f" ] && emit "INSTALL HARDLINK $(basename $f)" PASS \
                   || emit "INSTALL HARDLINK $(basename $f)" FAIL "missing"
done
[ -r "$G/usr/lib/alr/libalr_preload.so" ] \
    && emit "INSTALL PRELOAD PRESENT" PASS || emit "INSTALL PRELOAD PRESENT" FAIL "absent"
[ -x "$G/lib/ld-linux-aarch64.so.1" ] \
    && emit "INSTALL LDSO PRESENT" PASS || emit "INSTALL LDSO PRESENT" FAIL "absent"

# ── 2. the rootfs actually boots, and is actually virtualized ───────────
ALR_ROOT_DIR="$GATE_ROOT" ALR_DISTRO="$NAME" "$ALR" run /bin/true >/dev/null 2>&1
rc=$?
[ "$rc" = 0 ] && emit "INSTALL BOOT" PASS || emit "INSTALL BOOT" FAIL "rc=$rc"

# THE ONE THAT PROVES VIRTUALIZATION.  /bin/true passing proves nothing: it
# touches no guest path and succeeds on an unvirtualized rootfs too.  Android
# has no /usr, so reading a file BY ITS GUEST PATH can only work if rewriting
# is in effect.
got=$(ALR_ROOT_DIR="$GATE_ROOT" ALR_DISTRO="$NAME" "$ALR" run \
        /bin/cat /etc/os-release 2>/dev/null | head -1)
case "$got" in
    *Ubuntu*) emit "INSTALL VIRTUALIZED" PASS "$got";;
    *)        emit "INSTALL VIRTUALIZED" FAIL "guest could not read its own /etc/os-release: '$got'";;
esac

# ── 3. negative controls: the failures must be LOUD ─────────────────────
# Without these the gate above cannot distinguish "install works" from "install
# reports success no matter what" -- which is precisely the bug that made this
# harness necessary (the return of install_preload() was discarded).

# (a) a truncated tarball must not produce a successful install
head -c 2000000 "$SRC" > "$CACHE/$NAME-trunc.tar.gz" 2>/dev/null
rm -rf "$GATE_ROOT/trunc" "$CACHE/trunc.tar.gz"
ALR_ROOT_DIR="$GATE_ROOT" "$ALR" install trunc \
    --url "file://$CACHE/$NAME-trunc.tar.gz" >/dev/null 2>&1
rc=$?
[ "$rc" != 0 ] && emit "INSTALL REJECTS TRUNCATED" PASS "rc=$rc" \
               || emit "INSTALL REJECTS TRUNCATED" FAIL "reported success on a truncated tarball"
rm -rf "$GATE_ROOT/trunc"

# (a2) THE NEGATIVE CONTROL FOR THE BOOT CHECK ITSELF.  A truncated tarball
# dies at verify_rootfs, BEFORE the report, so it proves nothing about the boot
# check.  This builds a tarball that has exactly the four files verify_rootfs
# looks for and nothing that works -- so extraction succeeds, the file check
# succeeds, and the only thing that can catch it is actually running it.
# Without this, INSTALL BOOT /bin/true: PASS would be a line that has never
# been observed to say anything else.
FAKE="$GATE_ROOT/fakesrc"
rm -rf "$FAKE"; mkdir -p "$FAKE/lib" "$FAKE/bin" "$FAKE/usr/bin" "$FAKE/etc"
printf 'not an elf' > "$FAKE/lib/ld-linux-aarch64.so.1"
printf 'not an elf' > "$FAKE/bin/sh"
printf 'not an elf' > "$FAKE/usr/bin/env"
printf 'ID=ubuntu\n'  > "$FAKE/etc/os-release"
chmod 755 "$FAKE/lib/ld-linux-aarch64.so.1" "$FAKE/bin/sh" "$FAKE/usr/bin/env"
tar -czf "$CACHE/fake.tar.gz" -C "$FAKE" . 2>/dev/null
rm -rf "$GATE_ROOT/fake"
out=$(ALR_ROOT_DIR="$GATE_ROOT" "$ALR" install fake \
      --url "file://$CACHE/fake.tar.gz" 2>&1)
rc=$?
if [ "$rc" != 0 ] && grep -q 'reason=rootfs-unbootable' <<<"$out"; then
    # ...and it must be LEFT IN PLACE, not deleted (docs/05 §4).
    if [ -d "$GATE_ROOT/fake" ]; then
        emit "INSTALL REJECTS UNBOOTABLE" PASS "rc=$rc, rootfs left for inspection"
    else
        emit "INSTALL REJECTS UNBOOTABLE" FAIL "correctly refused, but DELETED the rootfs"
    fi
else
    emit "INSTALL REJECTS UNBOOTABLE" FAIL \
         "rc=$rc on a rootfs whose /bin/sh is the text 'not an elf'"
fi
rm -rf "$GATE_ROOT/fake" "$CACHE/fake.tar.gz" "$FAKE"

# (a2b) A tarball with a symlink that escapes the extraction root.  GNU tar
# creates `x -> ../../../etc/passwd` exactly as written -- correct behaviour for
# tar, and the one docs/05 §2 rule the shell-out never satisfied (ADR 0009).
# Written as a test because a rule stated in a spec and never violated on
# purpose has never been shown to do anything.
ESC="$GATE_ROOT/escsrc"
rm -rf "$ESC"; mkdir -p "$ESC/lib" "$ESC/bin" "$ESC/usr/bin" "$ESC/etc" "$ESC/deep/er"
cp "$G/lib/ld-linux-aarch64.so.1" "$ESC/lib/" 2>/dev/null
cp "$G/bin/sh" "$ESC/bin/" 2>/dev/null
cp "$G/usr/bin/env" "$ESC/usr/bin/" 2>/dev/null
printf 'ID=ubuntu\n' > "$ESC/etc/os-release"
ln -s ../../../../../../etc/hosts "$ESC/deep/er/escape"
ln -s ../../etc/os-release        "$ESC/deep/er/legit"
tar -czf "$CACHE/esc.tar.gz" -C "$ESC" . 2>/dev/null
rm -rf "$GATE_ROOT/esc"
out=$(ALR_ROOT_DIR="$GATE_ROOT" "$ALR" install esc \
      --url "file://$CACHE/esc.tar.gz" 2>&1)
rc=$?
if [ "$rc" != 0 ] && grep -q 'reason=extract-traversal-reject' <<<"$out"; then
    emit "INSTALL REJECTS ESCAPING SYMLINK" PASS "rc=$rc"
else
    emit "INSTALL REJECTS ESCAPING SYMLINK" FAIL "rc=$rc"
fi
# ...and the POSITIVE control: a relative symlink that stays inside must not
# trip it.  Nearly every symlink in a real rootfs is one of these (20 in
# ubuntu-base), so a check that rejected them would fail every install.
rm -rf "$GATE_ROOT/esc" "$ESC/deep/er/escape"
tar -czf "$CACHE/esc.tar.gz" -C "$ESC" . 2>/dev/null
ALR_ROOT_DIR="$GATE_ROOT" "$ALR" install esc \
    --url "file://$CACHE/esc.tar.gz" >/dev/null 2>&1
[ -L "$GATE_ROOT/esc/deep/er/legit" ] \
    && emit "INSTALL KEEPS INTERNAL SYMLINK" PASS \
    || emit "INSTALL KEEPS INTERNAL SYMLINK" FAIL "an in-rootfs symlink was rejected"
rm -rf "$GATE_ROOT/esc" "$CACHE/esc.tar.gz" "$ESC"

# (a3) `alr install -d <name>` -- the form every other subcommand accepts --
# used to install a rootfs literally NAMED "-d" and report success, because the
# option scan took argv[i+1] as the distro whatever it was and "-d" is legal
# under distro_name_ok().
rm -rf "$GATE_ROOT/dashd"
ALR_ROOT_DIR="$GATE_ROOT" "$ALR" install -d dashd \
    --url "file://$SRC" >/dev/null 2>&1
if [ -d "$GATE_ROOT/dashd" ] && [ ! -e "$GATE_ROOT/-d" ]; then
    emit "INSTALL DASH D IS A FLAG" PASS
else
    emit "INSTALL DASH D IS A FLAG" FAIL \
         "$([ -e "$GATE_ROOT/-d" ] && echo 'installed a rootfs named -d' || echo 'did not install dashd')"
fi
rm -rf "$GATE_ROOT/dashd" "$GATE_ROOT/-d"

o=$(ALR_ROOT_DIR="$GATE_ROOT" "$ALR" install "$NAME" --bogus 2>&1)
grep -q 'reason=install-unknown-option' <<<"$o" \
    && emit "INSTALL REFUSES UNKNOWN OPTION" PASS \
    || emit "INSTALL REFUSES UNKNOWN OPTION" FAIL "$o"

# (b) an unusable preload must not boot silently.  glibc WARNS and IGNORES an
# LD_PRELOAD object it cannot load, so the guest comes up unvirtualized -- the
# exact silent mode alr now refuses.
cp "$G/usr/lib/alr/libalr_preload.so" "$G/.pl.save" 2>/dev/null
rm -f "$G/usr/lib/alr/libalr_preload.so"
w=$(ALR_ROOT_DIR="$GATE_ROOT" ALR_DISTRO="$NAME" "$ALR" run /bin/echo hi 2>&1 \
      | grep -c 'reason=preload-missing-in-rootfs')
cp "$G/.pl.save" "$G/usr/lib/alr/libalr_preload.so" 2>/dev/null; rm -f "$G/.pl.save"
[ "${w:-0}" -ge 1 ] && emit "INSTALL WARNS ON MISSING PRELOAD" PASS \
                    || emit "INSTALL WARNS ON MISSING PRELOAD" FAIL "silent"

echo
echo "─────────────────────────────────────────────────────────────"
echo "  PASS=$pass  FAIL=$fail  SKIP=$skip"
echo "ALR INSTALL GATE: $([ "$fail" = 0 ] && echo PASS || echo FAIL)"
[ "$fail" = 0 ]
