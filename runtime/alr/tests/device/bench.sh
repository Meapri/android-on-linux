#!/data/data/com.termux/files/usr/bin/bash
# alr bench — the A/B harness for docs/07-acceptance.md §2 M8.
#
# §4.4 of that file records the two remaining M8 lines as PENDING_DEVICE and
# says plainly why: "막는 것은 기기가 아니라 하네스다 — 기기는 있고 alr bench 가
# 없다".  This is that harness.
#
#   ALR BENCH NODE COLD vs PROOT
#   ALR BENCH EXEC THROUGHPUT
#
# DELIBERATE DEVIATION from docs/06-cli-spec.md §1, recorded rather than
# silently taken: the spec lists `alr bench` as a CLI subcommand.  It is
# delivered here as a device harness instead, because every number it produces
# needs proot-distro on the other side of the comparison, and orchestrating a
# second runtime does not belong inside the runtime under test.  This matches
# tests/device/breadth.sh, which likewise emits an acceptance line from a shell
# harness rather than from the C binary.
#
# THE ONE RULE THAT MAKES THESE NUMBERS MEAN ANYTHING: both sides must run the
# SAME binary.  M8's git comparison was weakened by three different git builds
# (2.55 / 2.43 / 2.53) and the doc says so.  npm ci was measured after copying
# node, npm, the lockfile and the npm cache to both sides, and that is the model
# followed here -- the exec probe is compiled once and copied, and node is
# already identical on both sides from the npm ci run.
#
#   ALR_SSH_KEY=<key> ./scripts/dev-push.sh bench-ab
set -u

cd "$(dirname "$0")/../.." 2>/dev/null || true
ALR=${ALR:-./alr}
export ALR_ROOT_DIR=${ALR_ROOT_DIR:-$HOME/alr-distros}
R="$ALR_ROOT_DIR/${ALR_DISTRO:-ubuntu-24.04}"
PROOT_RFS="$PREFIX/var/lib/proot-distro/containers/ubuntu/rootfs"
REPS=${REPS:-5}

emit() {
    printf '%-34s %s' "$1:" "$2"
    [ $# -ge 3 ] && printf '  %s' "$3"
    printf '\n'
}

# ── validity gate ────────────────────────────────────────────────────────
uid=$(id -u)
sec=$(grep -oE '^Seccomp:[[:space:]]*[0-9]+' /proc/self/status | tr -dc 0-9)
if [ "${uid:-0}" -lt 10000 ] || [ "${sec:-0}" != "2" ]; then
    echo "REFUSING: not a measurable context (uid=$uid Seccomp=$sec)."
    echo "The app seccomp filter exists for zygote-spawned uid>=10000 only;"
    echo "adb shell reports Seccomp: 0 and would make every number a fiction."
    exit 2
fi
[ -x "$ALR" ] || { echo "alr not built: $ALR"; exit 2; }
[ -d "$R" ]   || { echo "rootfs not installed: $R"; exit 2; }

have_proot=0
if command -v proot-distro >/dev/null 2>&1 && [ -d "$PROOT_RFS" ]; then have_proot=1; fi

# Median of REPS runs, in milliseconds.  Median rather than mean: one scheduler
# hiccup on a phone skews a mean badly and there is no reason to let it.
#
# Timed with bash's own clock, not /usr/bin/time -- GNU time is not installed on
# the Termux HOST (the guest has it, but the stopwatch has to be on this side).
# An earlier version used it and every measurement came back empty.
now_ms() { printf '%s' "$(( ${EPOCHREALTIME/./} / 1000 ))"; }

# One discarded warm-up run, then REPS timed ones.
#
# WHY THE WARM-UP IS NOT OPTIONAL: run immediately after the breadth suite
# (96 package installs) this harness reported 330 exec/s and 61 ms node cold.
# Three runs later on the same phone, same binaries, it reported 687 / 52 --
# the first number was off by 2x.  It was measuring a thermally throttled,
# cache-cold device, and a median of five does not help when all five are in
# that state.  MEASURE AN IDLE PHONE, and let the spread show when it is not.
#
# Prints "<median> <min>-<max>" on one line.  It does NOT set a global: callers
# use it as $(median_ms ...), which is a subshell, so an assignment inside would
# never reach the caller.  An earlier version tried exactly that and every
# spread printed as an empty "[]" -- visible only because the placeholder was
# printed rather than silently omitted.
median_ms() {
    local i t0 t1 all
    "$@" >/dev/null 2>&1                      # warm-up, discarded
    all=$(for i in $(seq 1 "$REPS"); do
            t0=$(now_ms)
            "$@" >/dev/null 2>&1
            t1=$(now_ms)
            echo $((t1 - t0))
        done | sort -n)
    printf '%s\n' "$all" | awk '{a[NR]=$1} END{ if(NR) printf "%d %d-%d", a[int((NR+1)/2)], a[1], a[NR] }'
}

ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b>0 && a>0) printf "%.2f", a/b; else printf "?" }'; }

# Machine-readable device identity.  bench/regression_gate.py keys its soft
# baselines on this: comparing this phone's node cold start against another
# phone's recorded value is not a regression check, it is a device difference
# wearing one.  The two reference devices differ by ~7% on that number with
# identical code.
DEVICE_ID="$(getprop ro.product.model 2>/dev/null || echo unknown)/$(getprop ro.board.platform 2>/dev/null || echo unknown)/android$(getprop ro.build.version.release 2>/dev/null || echo '?')/$(uname -r | cut -d. -f1,2)"

echo "── alr bench (A/B) ──────────────────────────────────────────"
echo "ALR BENCH DEVICE: $DEVICE_ID"
echo "  reps=$REPS  guest=$R"
[ "$have_proot" = 1 ] && echo "  proot=$PROOT_RFS" || echo "  proot=(absent — A/B lines will be SKIP)"
echo

# ── identical-binary setup ───────────────────────────────────────────────
# A 3-line C program, compiled once inside the guest, copied to the other side.
# Nothing about it can differ between runtimes.
PROBE_SRC=/tmp/alrexec.c
cat > "$R$PROBE_SRC" <<'EOF'
int main(void) { return 0; }
EOF
if ! $ALR run /usr/bin/gcc -O1 -o /tmp/alrexec "$PROBE_SRC" >/dev/null 2>&1; then
    emit "ALR BENCH EXEC THROUGHPUT" SKIP "no gcc in the guest; cannot build an identical probe"
    probe_ok=0
else
    probe_ok=1
    [ "$have_proot" = 1 ] && cp "$R/tmp/alrexec" "$PROOT_RFS/tmp/alrexec" 2>/dev/null
fi

# ── ALR BENCH NODE COLD vs PROOT ─────────────────────────────────────────
# Cold start is the weakest workload for this design and docs/00-product.md §4
# says so -- it is measured to keep that claim honest, not to headline it.
NODE_G=/opt/node/bin/node
if [ ! -x "$R$NODE_G" ]; then
    emit "ALR BENCH NODE COLD vs PROOT" SKIP "node not installed in the guest"
elif [ "$have_proot" != 1 ] || [ ! -x "$PROOT_RFS$NODE_G" ]; then
    read -r a as <<<"$(median_ms "$ALR" run "$NODE_G" -e 0)"
    emit "ALR BENCH NODE COLD vs PROOT" SKIP "alr ${a:-?} ms [${as:-?}]; proot side has no identical node"
else
    read -r a as <<<"$(median_ms "$ALR" run "$NODE_G" -e 0)"
    read -r p ps <<<"$(median_ms proot-distro login ubuntu -- "$NODE_G" -e 0)"
    if [ -n "$a" ] && [ -n "$p" ]; then
        emit "ALR BENCH NODE COLD vs PROOT" "$(ratio "$p" "$a")x" \
             "MEASURED  alr $a [$as] / proot $p [$ps] ms  (identical node binary)"
    else
        emit "ALR BENCH NODE COLD vs PROOT" FAIL "timing failed (alr='${a:-}' proot='${p:-}')"
    fi
fi

# ── ALR BENCH EXEC THROUGHPUT ────────────────────────────────────────────
# execs/sec, which is where this design pays its own tax: every guest execve
# maps one more DSO and re-resolves the loader (docs/01-platform-facts.md §D2),
# and RISKS R5 exists because that could eat the hero benchmark.
if [ "${probe_ok:-0}" = 1 ]; then
    N=${EXEC_N:-200}
    loop='i=0; while [ $i -lt '"$N"' ]; do /tmp/alrexec; i=$((i+1)); done'
    read -r a as <<<"$(median_ms "$ALR" run /bin/sh -c "$loop")"
    if [ "$have_proot" = 1 ] && [ -x "$PROOT_RFS/tmp/alrexec" ]; then
        read -r p ps <<<"$(median_ms proot-distro login ubuntu -- /bin/sh -c "$loop")"
    else
        p=""
    fi
    if [ -n "$a" ] && [ "$a" -gt 0 ] 2>/dev/null; then
        aps=$(awk -v n="$N" -v ms="$a" 'BEGIN{printf "%.0f", n/(ms/1000)}')
        if [ -n "$p" ] && [ "$p" -gt 0 ] 2>/dev/null; then
            pps=$(awk -v n="$N" -v ms="$p" 'BEGIN{printf "%.0f", n/(ms/1000)}')
            emit "ALR BENCH EXEC THROUGHPUT" "${aps} exec/s" \
                 "MEASURED  alr ${aps} / proot ${pps} exec/s  (alr ${a} ms [$as] over $N execs)"
        else
            emit "ALR BENCH EXEC THROUGHPUT" "${aps} exec/s" \
                 "MEASURED  alr only; no proot side"
        fi
    else
        emit "ALR BENCH EXEC THROUGHPUT" FAIL "timing failed"
    fi
fi

# ── ALR MEDIATION INVARIANT ──────────────────────────────────────────────
# The line that separates this design from PRoot, re-checked under load rather
# than only on a trivial command.
inv=$(ALR_LOG=2 ALR_LOG_FD=2 "$ALR" run /bin/sh -c \
        'i=0; while [ $i -lt 50 ]; do /bin/true; i=$((i+1)); done' 2>&1 \
      | grep -o 'path_traps=[0-9]* syscall_stops=[0-9]*' | tail -1)
case "$inv" in
    "path_traps=0 syscall_stops=0") emit "ALR MEDIATION INVARIANT" PASS "$inv  MEASURED";;
    "")                             emit "ALR MEDIATION INVARIANT" SKIP "supervisor counters not reported";;
    *)                              emit "ALR MEDIATION INVARIANT" FAIL "$inv";;
esac

echo
echo "─────────────────────────────────────────────────────────────"
echo "  NOTE single-session numbers from the device this just ran on:"
echo "    $(getprop ro.product.model 2>/dev/null || echo '?') / $(getprop ro.board.platform 2>/dev/null || echo '?') / Android $(getprop ro.build.version.release 2>/dev/null || echo '?') / $(uname -r)"
echo "  Ratios are workload- AND device-specific -- the same suite gives"
echo "  6.60x on MediaTek MT8775 and 5.23x on Snapdragon 8 Elite for node"
echo "  cold start.  Quote a multiplier with the device beside it."
echo "  docs/00-product.md §2 has the coverage table."
