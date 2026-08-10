#!/data/data/com.termux/files/usr/bin/bash
# alr rw bench — the producer for the ONE budget that protects the claim.
#
# docs/04-preload-spec.md §13 states two budgets for path rewriting:
#
#   per-op:  abs hit <= 100 ns, rel miss <= 20 ns, sysdir <= 40 ns
#   total:   git status over 10k files, rewrite total cost <= 1.5 ms
#
# Only the per-op ones were ever enforced, and they DO NOT PORT between
# devices: the same code measures ~79 ns for the absolute hit on reference #1
# and 106-115 ns on reference #2, so a fixed 100 ns line passes on one phone
# and fails on the other (docs/evidence/2026-08-03-m19-snapdragon.md §7).
#
# The total-cost budget is the portable one, because it is a budget on real
# work rather than on a machine's clock.  Nothing produced a measured value for
# it -- rw_cost.c printed a MODELED figure that multiplied the ABSOLUTE-hit
# cost by a flat 13,500 calls, when the measured mix is 28 rewrites against
# 10,072 relative misses.  Wrong in both terms, and roughly 24x high.
#
# This harness measures both halves on the device under test, in one session:
#   1. the call mix, from the preload's own ALR_COUNT counters
#   2. the per-op costs, from rw_cost
# and hands (1) to (2) so the total is arithmetic on two measurements rather
# than a model.
#
#   ALR_SSH_KEY=<key> ./scripts/dev-push.sh bench
set -u

cd "$(dirname "$0")/../.." 2>/dev/null || true
ALR=${ALR:-./alr}
export ALR_ROOT_DIR=${ALR_ROOT_DIR:-$HOME/alr-distros}
DISTRO=${ALR_DISTRO:-ubuntu-24.04}
R="$ALR_ROOT_DIR/$DISTRO"
REPO_IN_GUEST=/tmp/bigrepo
NFILES=${NFILES:-10000}
TMP=${TMPDIR:-$PREFIX/tmp}

# ── validity gate ────────────────────────────────────────────────────────
uid=$(id -u)
sec=$(grep -oE '^Seccomp:[[:space:]]*[0-9]+' /proc/self/status | tr -dc 0-9)
if [ "${uid:-0}" -lt 10000 ] || [ "${sec:-0}" != "2" ]; then
    echo "REFUSING: not a measurable context (uid=$uid Seccomp=$sec)."
    exit 2
fi
[ -x "$ALR" ] || { echo "alr not built: $ALR"; exit 2; }
[ -d "$R" ]   || { echo "rootfs not installed: $R"; exit 2; }

# The gate keys per-op baselines on this token.  Without it, facts from this
# harness get filed under whatever device string a DIFFERENT harness printed in
# the same batch of files -- which happened to be right, and only by accident.
DEVICE_ID="$(getprop ro.product.model 2>/dev/null || echo unknown)/$(getprop ro.board.platform 2>/dev/null || echo unknown)/android$(getprop ro.build.version.release 2>/dev/null || echo '?')/$(uname -r | cut -d. -f1,2)"

echo "── alr rw bench ─────────────────────────────────────────────"
echo "ALR BENCH DEVICE: $DEVICE_ID"
echo "  guest=$R  files=$NFILES"

# ── 1. the workload, built with HOST tools ───────────────────────────────
# The tree is created by Termux, not by the guest: on Ubuntu 26.04 the guest's
# mkdir is uutils and does not work (ADR 0006), and a first attempt at this
# measurement silently timed git erroring on a directory that was never
# created.  git itself runs in the guest -- it is a normal dynamic glibc
# binary and works on both releases.
if [ ! -d "$R$REPO_IN_GUEST/.git" ] || \
   [ "$(ls "$R$REPO_IN_GUEST" 2>/dev/null | wc -l)" -ne "$NFILES" ]; then
    echo "  building the $NFILES-file repo (host-side)…"
    rm -rf "$R$REPO_IN_GUEST"; mkdir -p "$R$REPO_IN_GUEST"
    i=0; while [ "$i" -lt "$NFILES" ]; do echo x > "$R$REPO_IN_GUEST/f$i"; i=$((i+1)); done
    "$ALR" run /usr/bin/git -C "$REPO_IN_GUEST" init -q .            >/dev/null 2>&1
    "$ALR" run /usr/bin/git -C "$REPO_IN_GUEST" add -A               >/dev/null 2>&1
    "$ALR" run /usr/bin/git -C "$REPO_IN_GUEST" \
           -c user.email=a@b -c user.name=c commit -qm init          >/dev/null 2>&1
fi
tracked=$("$ALR" run /usr/bin/git -C "$REPO_IN_GUEST" ls-files 2>/dev/null | wc -l | tr -d ' ')
echo "  tracked files: $tracked"
# Two-sided: a LARGER repo inflates the call count and therefore the total,
# and NFILES is env-overridable, so "at least N" is not the check we want.
if [ "${tracked:-0}" -ne "$NFILES" ]; then
    echo "PRELOAD RW INSTRUMENT: FAIL  repo has $tracked tracked files, wanted $NFILES"
    echo "  (refusing to price a workload that is not the one in the budget)"
    exit 1
fi

# ── 2. the call mix ──────────────────────────────────────────────────────
# The counters go to a DEDICATED HIGH FD, not to stderr.
#
# With ALR_LOG_FD=2 this measurement is silently wrong: GNU coreutils registers
# gnulib's close_stdout with atexit, which closes stderr, and glibc runs atexit
# handlers before the library destructor that prints the totals.  MEASURED:
# `ALR_COUNT=1 alr run /bin/echo hi` emitted nothing 10 times out of 10 while
# /bin/true emitted 10 out of 10.  Summing over a pipeline would have dropped
# every coreutils process and reported a total smaller than the truth.
#
# The preload now re-duplicates its log fd to a high number at init, so fd 9
# here is only about keeping the collected lines away from the program's own
# stderr.  Both halves of that fix matter: keep them.
COUNTS="$TMP/alr-rw-counts.txt"
: > "$COUNTS"
ALR_COUNT=1 ALR_LOG_FD=9 "$ALR" run /usr/bin/git -C "$REPO_IN_GUEST" \
    status --porcelain 9>"$COUNTS" >/dev/null 2>/dev/null

# NOT `grep -c ... || echo 0`: on a file with no match grep PRINTS "0" and
# EXITS 1, so the fallback fires too and nlines becomes the two-line string
# "0\n0".  The integer test below then errors with "integer expression
# expected" and, depending on the shell, falls through -- i.e. the guard
# against a dead instrument was itself dead.  Count with awk, which cannot
# fail this way.
nlines=$(awk '/^alr rw:/ {n++} END {print n+0}' "$COUNTS" 2>/dev/null)
nlines=${nlines:-0}
if [ "$nlines" -eq 0 ] 2>/dev/null || [ "$nlines" = 0 ]; then
    echo "PRELOAD RW INSTRUMENT: FAIL  the ALR_COUNT instrument emitted nothing"
    echo "  This is an instrument failure, not a measurement of zero cost."
    echo "  Most likely cause first: the DEPLOYED preload predates the log-fd"
    echo "  fix (F_DUPFD_CLOEXEC) or the underroot counter -- redeploy it."
    echo "  Then check that ALR_COUNT and ALR_LOG_FD reach the guest."
    exit 1
fi

# One line per PROCESS, so sum them: git status spawns children and each
# contributes its own share of the workload's rewrite traffic.
eval "$(awk '
    /^alr rw:/ {
        for (i = 1; i <= NF; i++) {
            n = split($i, a, "=")
            if (n == 2) s[a[1]] += a[2]
        }
    }
    END {
        printf "C_TOTAL=%d C_RW=%d C_REL=%d C_SYS=%d C_UNDER=%d\n",
               s["total"], s["rewritten"], s["relative"], s["sysdir"], s["underroot"]
    }' "$COUNTS")"

echo "  counter lines: $nlines (summed over processes)"
echo "  mix: total=$C_TOTAL rewritten=$C_RW relative=$C_REL sysdir=$C_SYS underroot=$C_UNDER"

# The classification must be total -- every counted call in exactly one bucket.
# It was not: an absolute path that was neither rewritten nor a sysdir fell
# through an if/else-if chain with no else and was counted only in `total`.
# A bucket with no counter is a bucket the cost model prices at zero.
sum=$((C_RW + C_REL + C_SYS + C_UNDER))
if [ "$sum" -ne "$C_TOTAL" ]; then
    echo "PRELOAD RW INSTRUMENT: FAIL  buckets sum to $sum but total=$C_TOTAL"
    echo "  The classification in rw() is not exhaustive, or the counters are"
    echo "  racing.  Either way the cost model would be pricing calls it cannot"
    echo "  see; refusing to emit a number."
    exit 1
fi

# ── 3. per-op costs, priced against that mix ─────────────────────────────
clang -O2 -Wall -Isrc/common -o rw_cost bench/microbench/rw_cost.c || {
    echo "PRELOAD RW INSTRUMENT: FAIL  rw_cost did not build"; exit 1; }

echo
./rw_cost "$R" "${ITERS:-2000000}" "$C_RW" "$C_REL" "$C_SYS" "$C_UNDER"
rc=$?

echo
echo "─────────────────────────────────────────────────────────────"
echo "  device: $(getprop ro.product.model 2>/dev/null || echo '?')/$(getprop ro.board.platform 2>/dev/null || echo '?')/android$(getprop ro.build.version.release 2>/dev/null || echo '?')/$(uname -r | cut -d. -f1,2)"
echo "  NOTE the per-op figures above are RECORDED, not absolutely gated --"
echo "  they differ by ~40% between the two reference devices for identical"
echo "  code.  bench/regression_gate.py checks them against THIS device's own"
echo "  recorded baseline, which is a tighter test than the fixed budget was."
exit $rc
