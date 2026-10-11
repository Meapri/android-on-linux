#!/usr/bin/env bash
# Standalone tawcroot/proot/gVisor/chroot/native microbenchmark runner.
#
# This intentionally does not integrate with tawcroot/test.sh. It needs only:
#   - a rootfs directory
#   - a tawcroot binary for the current machine, if running the tawcroot mode
# Optional competitors are discovered on PATH unless passed explicitly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

ROOTFS=""
OUT=""
MODES="native,tawcroot,proot,chroot,gvisor"
ITERATIONS=10000
FILES=256
ROUNDS=3
CC_BIN="${CC:-}"
BENCH_BIN=""
TAWCROOT_BIN="${TAWCROOT_BIN:-$REPO_DIR/build/tawcroot-host/tawcroot}"
PROOT_BIN="${PROOT_BIN:-}"
RUNSC_BIN="${RUNSC_BIN:-}"
CHROOT_USE_SUDO=0
KEEP_WORK=0
INTERLEAVE=0

usage() {
    cat <<'EOF'
usage: tawcroot/perf/run-perf.sh --rootfs PATH [options]

Options:
  --modes LIST          Comma list: native,tawcroot,proot,chroot,gvisor
  --tawcroot PATH       tawcroot binary (default: build/tawcroot-host/tawcroot)
  --proot PATH          proot binary (default: PATH lookup)
  --runsc PATH          runsc binary (default: PATH lookup)
  --bench-bin PATH      prebuilt tawcroot-perf binary for this architecture
  --cc PATH             C compiler for building the benchmark binary
  --iterations N        Inner-loop iterations per run (default: 10000)
  --files N             Files in the benchmark work dir (default: 256)
  --rounds N            Repetitions per backend (default: 3)
  --out PATH            TSV output path (default: build/tawcroot-perf/results-*.tsv)
  --interleave          Run each round across all backends before the next round
  --sudo-chroot         Run chroot mode through sudo when not root
  --keep-work           Keep temporary bundle/build files

Output columns:
  backend round benchmark elapsed_ns ops ns_per_op
EOF
}

abs_path() {
    local p="$1"
    if [[ "$p" = /* ]]; then
        printf '%s\n' "$p"
    else
        printf '%s/%s\n' "$PWD" "$p"
    fi
}

have_mode() {
    local needle="$1"
    case ",$MODES," in
        *",$needle,"*) return 0 ;;
        *) return 1 ;;
    esac
}

find_tool() {
    local var_name="$1"
    local default_name="$2"
    local current="${!var_name}"
    if [[ -n "$current" ]]; then
        printf '%s\n' "$current"
        return 0
    fi
    command -v "$default_name" 2>/dev/null || true
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rootfs) ROOTFS="${2:?}"; shift 2 ;;
        --modes) MODES="${2:?}"; shift 2 ;;
        --tawcroot) TAWCROOT_BIN="${2:?}"; shift 2 ;;
        --proot) PROOT_BIN="${2:?}"; shift 2 ;;
        --runsc) RUNSC_BIN="${2:?}"; shift 2 ;;
        --bench-bin) BENCH_BIN="${2:?}"; shift 2 ;;
        --cc) CC_BIN="${2:?}"; shift 2 ;;
        --iterations) ITERATIONS="${2:?}"; shift 2 ;;
        --files) FILES="${2:?}"; shift 2 ;;
        --rounds) ROUNDS="${2:?}"; shift 2 ;;
        --out) OUT="${2:?}"; shift 2 ;;
        --interleave) INTERLEAVE=1; shift ;;
        --sudo-chroot) CHROOT_USE_SUDO=1; shift ;;
        --keep-work) KEEP_WORK=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "ERROR: unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$ROOTFS" ]]; then
    echo "ERROR: --rootfs is required" >&2
    usage >&2
    exit 2
fi
ROOTFS="$(abs_path "$ROOTFS")"
if [[ ! -d "$ROOTFS" ]]; then
    echo "ERROR: rootfs is not a directory: $ROOTFS" >&2
    exit 2
fi

WORK_ROOT="$REPO_DIR/build/tawcroot-perf"
mkdir -p "$WORK_ROOT"
if [[ -z "$OUT" ]]; then
    OUT="$WORK_ROOT/results-$(date +%Y%m%d-%H%M%S).tsv"
else
    OUT="$(abs_path "$OUT")"
    mkdir -p "$(dirname "$OUT")"
fi

build_bench() {
    if [[ -n "$BENCH_BIN" ]]; then
        BENCH_BIN="$(abs_path "$BENCH_BIN")"
        [[ -x "$BENCH_BIN" ]] || { echo "ERROR: --bench-bin is not executable: $BENCH_BIN" >&2; exit 2; }
        return
    fi

    local out="$WORK_ROOT/tawcroot-perf"
    local candidates=()
    if [[ -n "$CC_BIN" ]]; then candidates+=("$CC_BIN"); fi
    if command -v musl-gcc >/dev/null 2>&1; then candidates+=("musl-gcc"); fi
    candidates+=("cc" "gcc" "clang")

    local cc
    for cc in "${candidates[@]}"; do
        command -v "$cc" >/dev/null 2>&1 || continue
        if "$cc" -O2 -static -Wall -Wextra -o "$out" "$SCRIPT_DIR/tawcroot-perf.c" >/dev/null 2>"$WORK_ROOT/build.log"; then
            BENCH_BIN="$out"
            return
        fi
    done

    echo "ERROR: failed to build a static benchmark binary." >&2
    echo "       Install musl-gcc/static libc, pass --cc, or pass --bench-bin." >&2
    echo "       Last compiler log: $WORK_ROOT/build.log" >&2
    exit 1
}

install_bench_into_rootfs() {
    local guest_dir="$ROOTFS/tmp/tawcroot-perf"
    mkdir -p "$guest_dir"
    cp "$BENCH_BIN" "$guest_dir/tawcroot-perf"
    chmod 0755 "$guest_dir/tawcroot-perf"
    chmod 0777 "$guest_dir"
}

append_results() {
    local backend="$1"
    local round="$2"
    awk -v backend="$backend" -v round="$round" '
        BEGIN { ok = 0 }
        $1 == "RESULT" {
            print backend "\t" round "\t" $2 "\t" $3 "\t" $4 "\t" $5
            ok = 1
        }
        END { if (!ok) exit 42 }
    ' >>"$OUT"
}

run_native() {
    "$ROOTFS/tmp/tawcroot-perf/tawcroot-perf" \
        --root-prefix "$ROOTFS" \
        --self "$ROOTFS/tmp/tawcroot-perf/tawcroot-perf" \
        --iterations "$ITERATIONS" \
        --files "$FILES" \
        --setup
}

run_tawcroot() {
    "$TAWCROOT_BIN" -r "$ROOTFS" -- \
        /tmp/tawcroot-perf/tawcroot-perf \
        --self /tmp/tawcroot-perf/tawcroot-perf \
        --iterations "$ITERATIONS" \
        --files "$FILES" \
        --setup
}

run_proot() {
    "$PROOT_BIN" -0 -r "$ROOTFS" -w / \
        /tmp/tawcroot-perf/tawcroot-perf \
        --self /tmp/tawcroot-perf/tawcroot-perf \
        --iterations "$ITERATIONS" \
        --files "$FILES" \
        --setup
}

run_chroot() {
    local argv=(
        chroot "$ROOTFS"
        /tmp/tawcroot-perf/tawcroot-perf
        --self /tmp/tawcroot-perf/tawcroot-perf
        --iterations "$ITERATIONS"
        --files "$FILES"
        --setup
    )
    if [[ "$(id -u)" = "0" ]]; then
        "${argv[@]}"
    elif [[ "$CHROOT_USE_SUDO" = "1" ]]; then
        sudo "${argv[@]}"
    else
        echo "SKIP: chroot needs root; pass --sudo-chroot to use sudo" >&2
        return 77
    fi
}

json_quote() {
    local s="$1"
    s=${s//\\/\\\\}
    s=${s//\"/\\\"}
    s=${s//$'\n'/\\n}
    printf '"%s"' "$s"
}

run_gvisor() {
    local dir id root_dir bundle
    dir="$(mktemp -d "$WORK_ROOT/runsc.XXXXXX")"
    id="tawcroot-perf-$$-$RANDOM"
    root_dir="$dir/root"
    bundle="$dir/bundle"
    mkdir -p "$root_dir" "$bundle"
    cat >"$bundle/config.json" <<EOF
{
  "ociVersion": "1.0.2",
  "process": {
    "terminal": false,
    "user": { "uid": 0, "gid": 0 },
    "args": [
      "/tmp/tawcroot-perf/tawcroot-perf",
      "--self", "/tmp/tawcroot-perf/tawcroot-perf",
      "--iterations", $(json_quote "$ITERATIONS"),
      "--files", $(json_quote "$FILES"),
      "--setup"
    ],
    "env": [ "PATH=/usr/bin:/bin" ],
    "cwd": "/"
  },
  "root": {
    "path": $(json_quote "$ROOTFS"),
    "readonly": false
  },
  "hostname": "tawcroot-perf",
  "linux": {
    "namespaces": [
      { "type": "pid" },
      { "type": "ipc" },
      { "type": "uts" },
      { "type": "mount" }
    ]
  }
}
EOF
    "$RUNSC_BIN" --rootless --network=none --root "$root_dir" run --bundle "$bundle" "$id"
    if [[ "$KEEP_WORK" != "1" ]]; then rm -rf "$dir"; fi
}

run_backend() {
    local backend="$1"
    case "$backend" in
        native) run_native ;;
        tawcroot) run_tawcroot ;;
        proot) run_proot ;;
        chroot) run_chroot ;;
        gvisor) run_gvisor ;;
        *) echo "ERROR: unknown backend: $backend" >&2; return 2 ;;
    esac
}

validate_backend() {
    local backend="$1"
    case "$backend" in
        tawcroot)
            TAWCROOT_BIN="$(abs_path "$TAWCROOT_BIN")"
            if [[ ! -x "$TAWCROOT_BIN" ]]; then
                echo "SKIP: tawcroot binary not executable: $TAWCROOT_BIN" >&2
                return 1
            fi
            ;;
        proot)
            if [[ -z "$PROOT_BIN" || ! -x "$PROOT_BIN" ]]; then
                echo "SKIP: proot not found; pass --proot PATH" >&2
                return 1
            fi
            ;;
        gvisor)
            if [[ -z "$RUNSC_BIN" || ! -x "$RUNSC_BIN" ]]; then
                echo "SKIP: runsc not found; pass --runsc PATH" >&2
                return 1
            fi
            ;;
        native|chroot) ;;
        *) echo "ERROR: unknown backend: $backend" >&2; exit 2 ;;
    esac
}

run_backend_round() {
    local backend="$1"
    local round="$2"
    local tmp="$WORK_ROOT/$backend.$round.out"
    local status=0

    run_backend "$backend" >"$tmp" 2>"$tmp.err" || status=$?
    if [[ "$status" = "0" ]]; then
        append_results "$backend" "$round" <"$tmp" || {
            echo "ERROR: no RESULT lines from $backend round $round" >&2
            cat "$tmp.err" >&2
            exit 1
        }
    elif [[ "$backend" = "chroot" && "$status" = "77" ]]; then
        # The SKIP explanation went into $tmp.err (run_backend's stderr
        # is captured); surface it so the user sees why nothing ran.
        cat "$tmp.err" >&2
        return 77
    else
        echo "ERROR: backend failed: $backend round $round" >&2
        cat "$tmp.err" >&2
        exit 1
    fi
}

build_bench
install_bench_into_rootfs

PROOT_BIN="$(find_tool PROOT_BIN proot)"
RUNSC_BIN="$(find_tool RUNSC_BIN runsc)"

{
    printf '# rootfs=%s\n' "$ROOTFS"
    printf '# bench_bin=%s\n' "$BENCH_BIN"
    printf '# iterations=%s\n' "$ITERATIONS"
    printf '# files=%s\n' "$FILES"
    printf 'backend\tround\tbenchmark\telapsed_ns\tops\tns_per_op\n'
} >"$OUT"

IFS=',' read -r -a RAW_MODE_ARR <<<"$MODES"
MODE_ARR=()
for backend in "${RAW_MODE_ARR[@]}"; do
    backend="${backend//[[:space:]]/}"
    [[ -n "$backend" ]] || continue
    validate_backend "$backend" || continue
    MODE_ARR+=("$backend")
done

if [[ "$INTERLEAVE" = "1" ]]; then
    for ((round = 1; round <= ROUNDS; round++)); do
        echo "== round $round =="
        for backend in "${MODE_ARR[@]}"; do
            echo "-- $backend --"
            status=0
            run_backend_round "$backend" "$round" || status=$?
            [[ "$status" = "77" ]] && continue
        done
    done
else
    for backend in "${MODE_ARR[@]}"; do
        echo "== $backend =="
        for ((round = 1; round <= ROUNDS; round++)); do
            status=0
            run_backend_round "$backend" "$round" || status=$?
            [[ "$status" = "77" ]] && break
        done
    done
fi

echo "wrote $OUT"
awk '
    BEGIN { FS = "\t" }
    /^#/ || $1 == "backend" { next }
    {
        key = $1 SUBSEP $3
        if (!(key in seen)) {
            seen[key] = 1
            keys[++nkeys] = key
        }
        sum[key] += $6
        n = ++count[key]
        vals[key, n] = $6 + 0
    }
    function sort_vals(key, n,    i, j, tmp) {
        for (i = 1; i <= n; i++) {
            for (j = i + 1; j <= n; j++) {
                if (vals[key, j] < vals[key, i]) {
                    tmp = vals[key, i]
                    vals[key, i] = vals[key, j]
                    vals[key, j] = tmp
                }
            }
        }
    }
    END {
        print "summary_ns_per_op:"
        print "backend\tbenchmark\trounds\tmean\tmedian\tmin\tmax"
        for (ki = 1; ki <= nkeys; ki++) {
            key = keys[ki]
            n = count[key]
            sort_vals(key, n)
            if (n % 2) {
                median = vals[key, (n + 1) / 2]
            } else {
                median = (vals[key, n / 2] + vals[key, n / 2 + 1]) / 2
            }
            split(key, parts, SUBSEP)
            printf "%s\t%s\t%d\t%.2f\t%.2f\t%.2f\t%.2f\n",
                   parts[1], parts[2], n, sum[key] / n, median,
                   vals[key, 1], vals[key, n]
        }
    }
' "$OUT"
