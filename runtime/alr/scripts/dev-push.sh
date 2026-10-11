#!/usr/bin/env bash
# Push the tree to the device and run something inside a REAL Termux context.
#
# Why ssh and not `adb shell`: the Android app seccomp filter is installed by
# the zygote only for uid >= 10000, so adb shell (u:r:shell:s0, Seccomp: 0) and
# run-as (u:r:runas_app:s0, Seccomp: 0) both measure a device that does not
# exist.  Only a process forked from the Termux app carries the filter.
# See scripts/dev-bootstrap.md for the one-time setup.
set -euo pipefail

PORT="${ALR_SSH_PORT:-8022}"
KEY="${ALR_SSH_KEY:?set ALR_SSH_KEY to the private key authorised in Termux}"
HOST="${ALR_SSH_HOST:-localhost}"
REMOTE="${ALR_REMOTE_DIR:-alr}"

SSH=(ssh -p "$PORT" -i "$KEY" -o StrictHostKeyChecking=no
     -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "$HOST")
SCP=(scp -P "$PORT" -i "$KEY" -o StrictHostKeyChecking=no
     -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

adb forward "tcp:$PORT" "tcp:$PORT" >/dev/null

assert_context() {
    local out
    out=$("${SSH[@]}" 'id -u; grep -oE "^Seccomp:[[:space:]]*[0-9]+" /proc/self/status | tr -dc 0-9; echo; cat /proc/self/attr/current')
    local uid seccomp ctx
    uid=$(sed -n 1p <<<"$out")
    seccomp=$(sed -n 2p <<<"$out")
    ctx=$(sed -n 3p <<<"$out")
    if [ "${uid:-0}" -lt 10000 ] || [ "${seccomp:-0}" != "2" ]; then
        echo "REFUSING: not a measurable context (uid=$uid Seccomp=$seccomp ctx=$ctx)" >&2
        echo "Results from here would be false ALLOWEDs. See scripts/dev-bootstrap.md." >&2
        exit 2
    fi
    echo "context OK: uid=$uid Seccomp=$seccomp ${ctx%%:c*}"
}

sync_tree() {
    "${SSH[@]}" "mkdir -p ~/$REMOTE/src/common ~/$REMOTE/src/cli \
                          ~/$REMOTE/src/supervisor ~/$REMOTE/tests/host \
                          ~/$REMOTE/tests/device ~/$REMOTE/tests/cases"
    "${SCP[@]}" src/common/*.h src/common/*.c     "$HOST:$REMOTE/src/common/"
    "${SCP[@]}" src/cli/*.c                       "$HOST:$REMOTE/src/cli/"
    "${SCP[@]}" src/supervisor/*.h src/supervisor/*.c "$HOST:$REMOTE/src/supervisor/"
    "${SCP[@]}" tests/host/*.c                    "$HOST:$REMOTE/tests/host/"
    "${SCP[@]}" tests/device/*.c tests/device/*.sh "$HOST:$REMOTE/tests/device/"
    "${SCP[@]}" tests/cases/*.tsv                 "$HOST:$REMOTE/tests/cases/"
    "${SSH[@]}" "mkdir -p ~/$REMOTE/bench/microbench"
    "${SCP[@]}" bench/microbench/*.c              "$HOST:$REMOTE/bench/microbench/"
    # alr-lane (ADR 0010): tawcroot, built beside alr so lane_path() finds it.
    "${SSH[@]}" "rm -rf ~/$REMOTE/third_party/tawcroot && mkdir -p ~/$REMOTE/third_party"
    "${SCP[@]}" -r third_party/tawcroot           "$HOST:$REMOTE/third_party/"
}

# Freestanding static non-PIE, as upstream builds it.  -mno-outline-atomics:
# without it the link wants __aarch64_cas4_acq & co. from compiler-rt, which a
# -nostdlib binary does not get.
# -DTAWCROOT_ALR selects the local changes listed in VENDORING.md.
LANE_BUILD='clang -O2 -Wall -Wframe-larger-than=1024 -Werror=frame-larger-than -ffreestanding -fno-stack-protector -fno-pie \
    -mno-outline-atomics -D_GNU_SOURCE -DTAWCROOT_ALR -Ithird_party/tawcroot/include \
    -static -nostdlib -nostartfiles -Wl,--image-base=0x10000000 \
    -o alr-lane third_party/tawcroot/src/*.c third_party/tawcroot/src/arch/aarch64_*.S'

CFLAGS_DEV='-O1 -Wall -Wextra -std=c11 -D_GNU_SOURCE'

case "${1:-test}" in
test)
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang -O1 -Wall -Wextra -Werror -std=c11 -Isrc/common -o test_path tests/host/test_path.c && \
        clang -O1 -Wall -Wextra -Werror -std=c11 -Isrc/common -o test_exec_rule \
              tests/host/test_exec_rule.c src/common/alr_elf.c src/common/alr_exec_rule.c && \
        ./test_path tests/cases/paths.tsv && ./test_exec_rule"
    ;;
accept)
    assert_context
    sync_tree
    # ALR_DISTROS_DIR selects which rootfs `preload` deploys into; the CLI reads
    # ALR_ROOT_DIR. Without this bridge, hand-testing against one rootfs while
    # the suite ran against the default made 9 passing tests report FAIL --
    # the code was fine, the suite was running an older preload.
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -Isrc/common -Isrc/supervisor -o alr \
              src/cli/alr.c src/cli/alr_resolvd.c src/common/alr_exec_rule.c src/common/alr_elf.c src/supervisor/alr_supervisor.c && \
        $LANE_BUILD && \
        clang $CFLAGS_DEV -o alr-doctor src/cli/doctor.c && \
        chmod +x tests/device/acceptance.sh && \
        ${ALR_DISTROS_DIR:+ALR_ROOT_DIR=\$HOME/$ALR_DISTROS_DIR }ALR=./alr tests/device/acceptance.sh"
    ;;
install-gate)
    # The install path is the one thing acceptance.sh cannot cover: it refuses
    # to run unless the rootfs already exists.  Separate subcommand because it
    # costs a full extraction.
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -Isrc/common -Isrc/supervisor -o alr \
              src/cli/alr.c src/cli/alr_resolvd.c src/common/alr_exec_rule.c src/common/alr_elf.c src/supervisor/alr_supervisor.c && \
        $LANE_BUILD && \
        chmod +x tests/device/install_gate.sh && \
        ALR=./alr tests/device/install_gate.sh"
    ;;
bench)
    # Was: build rw_cost and run it bare.  That printed per-op costs and a
    # MODELED total; it could not produce the total-cost budget docs/04 §13
    # actually states, because that needs the call mix as well.  rw_bench.sh
    # measures both halves on the device and prices one with the other.
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -Isrc/common -Isrc/supervisor -o alr \
              src/cli/alr.c src/cli/alr_resolvd.c src/common/alr_exec_rule.c src/common/alr_elf.c src/supervisor/alr_supervisor.c && \
        $LANE_BUILD && \
        chmod +x tests/device/rw_bench.sh && \
        ${ALR_DISTROS_DIR:+ALR_ROOT_DIR=\$HOME/$ALR_DISTROS_DIR }ALR=./alr tests/device/rw_bench.sh"
    ;;
bench-ab)
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -Isrc/common -Isrc/supervisor -o alr \
              src/cli/alr.c src/cli/alr_resolvd.c src/common/alr_exec_rule.c src/common/alr_elf.c src/supervisor/alr_supervisor.c && \
        $LANE_BUILD && \
        chmod +x tests/device/bench.sh && \
        ${ALR_DISTROS_DIR:+ALR_ROOT_DIR=\$HOME/$ALR_DISTROS_DIR }ALR=./alr tests/device/bench.sh"
    ;;
breadth)
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -Isrc/common -Isrc/supervisor -o alr \
              src/cli/alr.c src/cli/alr_resolvd.c src/common/alr_exec_rule.c src/common/alr_elf.c src/supervisor/alr_supervisor.c && \
        $LANE_BUILD && \
        chmod +x tests/device/breadth.sh && \
        ${ALR_DISTROS_DIR:+ALR_ROOT_DIR=\$HOME/$ALR_DISTROS_DIR }ALR=./alr tests/device/breadth.sh"
    ;;
preload)
    assert_context
    ./scripts/build-preload.sh
    R="${ALR_DISTROS_DIR:-alr-distros}/${ALR_DISTRO:-ubuntu-26.04}"
    "${SSH[@]}" "mkdir -p ~/$R/usr/lib/alr"
    "${SCP[@]}" build/libalr_preload.so build/libalr_preload.manifest.json \
                "$HOST:$R/usr/lib/alr/"
    # ALSO the host-side copy next to the alr binary.  There are two: the one
    # the guest LOADS (in the rootfs, above) and the one `alr version` reports
    # and `alr update-components` copies FROM.  Deploying only the first let the
    # acceptance suite silently revert the rootfs to the older build, because
    # CLI UPDATE COMPONENTS re-installs from this path -- so a freshly deployed
    # preload was overwritten by a stale one mid-suite and two new checks
    # failed against code that was actually correct.
    "${SCP[@]}" build/libalr_preload.so "$HOST:$REMOTE/libalr_preload.so"
    "${SSH[@]}" "cd ~/$R/usr/lib/alr && \
        echo '-- glibc versions needed (must be 2.17 only) --' && \
        llvm-readelf -V libalr_preload.so 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tr '\n' ' ' && echo && \
        echo '-- defined FUNC symbols --' && \
        echo -n 'defined FUNC symbols: ' && \
        llvm-readelf --dyn-symbols libalr_preload.so 2>/dev/null | grep -c 'FUNC .*DEFAULT *[0-9]'"
    ;;
alr)
    assert_context
    ./scripts/build-preload.sh >/dev/null 2>&1 || true
    sync_tree
    "${SCP[@]}" build/libalr_preload.so "$HOST:$REMOTE/" 2>/dev/null || true
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -Isrc/common -Isrc/supervisor -o alr \
              src/cli/alr.c src/cli/alr_resolvd.c src/common/alr_exec_rule.c src/common/alr_elf.c src/supervisor/alr_supervisor.c && \
        $LANE_BUILD && \
        echo built"
    ;;
trap-cost)
    # ADR 0004 D6 / spike S2: in-process SECCOMP_RET_TRAP round trip.  Run once
    # unpinned and once per CPU given in ALR_TRAP_CPUS (e.g. "0 7" for a LITTLE
    # and a big core); the D6 verdict is printed per run.
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -o trap_cost bench/microbench/trap_cost.c && \
        ./trap_cost && for c in ${ALR_TRAP_CPUS:-}; do echo; ./trap_cost \$c; done"
    ;;
supervisor)
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && \
        clang $CFLAGS_DEV -Isrc/supervisor -o test_supervisor \
              tests/device/test_supervisor.c src/supervisor/alr_supervisor.c && \
        ./test_supervisor"
    ;;
doctor)
    assert_context
    sync_tree
    "${SSH[@]}" "cd ~/$REMOTE && clang -O1 -o alr-doctor src/cli/doctor.c && ./alr-doctor"
    ;;
shell)
    # With no argument this is an interactive login.  With one it runs that
    # command -- without which `dev-push.sh shell 'some command'` SILENTLY
    # dropped the command and opened a shell that read EOF and exited 0.  A
    # harness that exits 0 having run nothing is the worst kind.
    shift
    if [ "$#" -eq 0 ]; then exec "${SSH[@]}"; fi
    exec "${SSH[@]}" "$*"
    ;;
*)
    echo "usage: $0 {test|supervisor|preload|alr|accept|bench|trap-cost|doctor|shell}" >&2; exit 1;;
esac
