#!/data/data/com.termux/files/usr/bin/bash
# ADR 0005 T2: the same workloads natively in Termux and under alr, same
# device, warmed up, medians.  T2 asks for alr/native <= 1.2 per workload.
#
# "Native" is the Termux package of the same program; the versions differ and
# are printed with the results.  A glibc guest binary cannot run outside alr,
# so a same-binary comparison does not exist on this platform.
#
# Two shapes per workload where it matters:
#   top    `alr run X` against `X`: includes alr's own launch (~5 ms, fixed)
#   guest  N runs from one guest shell against N runs from a Termux shell:
#          what a user inside `alr shell` pays per command
#
# apt-get is reported but is NOT a like-for-like ratio: Ubuntu's package
# lists and dpkg database are many times Termux's, and the work scales with
# them.  Its line carries both sizes.
#
# Needs in Termux: hyperfine git python nodejs, and golang for the Go rows.
# In the guest: git python3 nodejs npm (alr install --with node), golang-go.
# Network once, to fill npm caches.
#
# The Go rows run in the tawcroot lane (ADR 0010): go is static, so `alr run`
# hands it to alr-lane, and everything it starts stays there.
#
#   ALR_ROOT_DIR=$PREFIX/var/lib/alr/distros ALR=./alr bash tests/device/t2_native.sh
set -u

cd "$(dirname "$0")/../.." 2>/dev/null || true
ALR=${ALR:-./alr}
export ALR_ROOT_DIR=${ALR_ROOT_DIR:-$HOME/alr-distros}
R="$ALR_ROOT_DIR/${ALR_DISTRO:-ubuntu-26.04}"
RUNS=${RUNS:-15}
OUT=${OUT:-$HOME/.cache/alr-t2}
mkdir -p "$OUT"

uid=$(id -u)
sec=$(grep -oE '^Seccomp:[[:space:]]*[0-9]+' /proc/self/status | tr -dc 0-9)
if [ "${uid:-0}" -lt 10000 ] || [ "${sec:-0}" != "2" ]; then
    echo "REFUSING: not a measurable context (uid=$uid Seccomp=$sec)."
    exit 2
fi
for t in hyperfine git python3 node npm; do
    command -v $t >/dev/null || { echo "missing in Termux: $t"; exit 2; }
done
[ -x "$ALR" ] && [ -d "$R" ] || { echo "alr or rootfs missing: $ALR $R"; exit 2; }

# median of each command in a hyperfine 2.x JSON, ratio against the first
med() {
    python3 - "$1" <<'EOF'
import json, sys
rs = json.load(open(sys.argv[1]))["results"]
m = [r["summary"]["time_wall_clock"]["median"] * 1000 for r in rs]
print(" ".join("%.1f" % x for x in m), "%.2f" % (m[1] / m[0]))
EOF
}
FAILS=0
row() {   # name, json, note
    local nat alr ratio verdict
    read -r nat alr ratio < <(med "$2")
    verdict=$(python3 -c "print('PASS' if $ratio <= 1.2 else 'OVER')")
    [ "${3:-}" = "data-bound" ] && verdict="INFO"
    [ "$verdict" = "OVER" ] && FAILS=$((FAILS + 1))
    printf "%-28s native %8s ms  alr %8s ms  x%s  %s %s\n" "$1" "$nat" "$alr" "$ratio" "$verdict" "${3:-}"
}
hf() { local j="$OUT/$1.json"; shift; hyperfine -N -w 3 -r "$RUNS" --export-json "$j" "$@" >/dev/null 2>&1 || { echo "hyperfine failed: $*"; exit 1; }; echo "$j"; }

# ── fixtures, shared by both sides (they live in the rootfs) ──────────────
if [ ! -d "$R/tmp/t2repo/.git" ]; then
    mkdir -p "$R/tmp/t2repo" && (cd "$R/tmp/t2repo" && git init -q &&
        for d in $(seq 100); do mkdir d$d; for f in $(seq 100); do echo $d$f > d$d/f$f; done; done &&
        git add -A && git -c user.name=t -c user.email=t@t commit -qm init)
fi
if [ ! -f "$R/tmp/t2npm/package-lock.json" ]; then
    mkdir -p "$R/tmp/t2npm" && (cd "$R/tmp/t2npm" &&
        printf '{"name":"t2","version":"1.0.0","private":true,"dependencies":{"express":"4.21.2","lodash":"4.17.21","chalk":"4.1.2"}}\n' > package.json &&
        npm install --no-audit --no-fund --loglevel=error >/dev/null 2>&1)
fi
(cd "$R/tmp/t2npm" && npm ci --no-audit --no-fund --loglevel=error >/dev/null 2>&1)
$ALR run /bin/sh -c 'cd /tmp/t2npm && npm ci --no-audit --no-fund --loglevel=error' >/dev/null 2>&1

# the guest's newest node: `alr install --with node` (closest to Termux's)
GN=/usr/bin/node; [ -x "$R/usr/local/bin/node" ] && GN=/usr/local/bin/node

echo "── T2 native comparison  ($(getprop ro.product.model) / Android $(getprop ro.build.version.release), runs=$RUNS) ──"
echo "native: $(git --version), node $(node --version), $(python3 --version), npm $(npm --version)"
echo "guest : $($ALR run /usr/bin/git --version), node $($ALR run $GN --version), $($ALR run /usr/bin/python3 --version)"

row "git status 10k (top)"  "$(hf git "git -C $R/tmp/t2repo status --porcelain" "$ALR run /usr/bin/git -C /tmp/t2repo status --porcelain")"
row "node -e 0 (top)"       "$(hf node "node -e 0" "$ALR run $GN -e 0")"
row "python3 -c pass (top)" "$(hf py "python3 -c pass" "$ALR run /usr/bin/python3 -c pass")"
row "node -e 0 x20 (guest)" "$(RUNS=5 hf nodeg "bash -c 'for i in \$(seq 20); do node -e 0; done'" "$ALR run /bin/bash -c 'for i in \$(seq 20); do $GN -e 0; done'")"
row "git status x20 (guest)" "$(RUNS=5 hf gitg "bash -c 'for i in \$(seq 20); do git -C $R/tmp/t2repo status --porcelain; done'" "$ALR run /bin/bash -c 'for i in \$(seq 20); do git -C /tmp/t2repo status --porcelain; done'")"
if command -v go >/dev/null && [ -x "$R/usr/bin/go" ]; then
    mkdir -p "$R/tmp/t2go" &&
        printf 'package main\nimport "fmt"\nfunc main(){fmt.Println("hi")}\n' > "$R/tmp/t2go/m.go" &&
        printf 'module t2go\n' > "$R/tmp/t2go/go.mod"
    (cd "$R/tmp/t2go" && CGO_ENABLED=0 go build -o /dev/null .)
    $ALR run -w /tmp/t2go /usr/bin/env CGO_ENABLED=0 /usr/bin/go build -o /dev/null .
    echo "go    : native $(go version | cut -d' ' -f3), guest $($ALR run /usr/bin/go version | cut -d' ' -f3)"
    row "go version (top, lane)" "$(hf gov "go version" "$ALR run /usr/bin/go version")"
    row "go build, cached (lane)" "$(RUNS=8 hf gob "sh -c 'cd $R/tmp/t2go && CGO_ENABLED=0 go build -o /dev/null .'" "$ALR run -w /tmp/t2go /usr/bin/env CGO_ENABLED=0 /usr/bin/go build -o /dev/null .")"
    row "gofmt -l net (lane)" "$(RUNS=8 hf gofmt "sh -c 'gofmt -l $PREFIX/lib/go/src/net >/dev/null'" "$ALR run /bin/sh -c 'gofmt -l /usr/lib/go/src/net >/dev/null'")"
fi
row "npm ci, 74 deps (top)" "$(RUNS=8 hf npm "sh -c 'cd $R/tmp/t2npm && npm ci --offline --no-audit --no-fund --loglevel=error'" "$ALR run /bin/sh -c 'cd /tmp/t2npm && npm ci --offline --no-audit --no-fund --loglevel=error'")"
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -q tree >/dev/null 2>&1
ALR_FAKEROOT=1 $ALR run /usr/bin/apt-get install -y -q tree >/dev/null 2>&1
row "apt-get reinstall tree"  "$(RUNS=8 hf apt "apt-get install -y -q --reinstall tree" "env ALR_FAKEROOT=1 $ALR run /usr/bin/apt-get install -y -q --reinstall tree")" "data-bound"
echo "  apt data: lists native $(du -sh $PREFIX/var/lib/apt/lists | cut -f1) guest $(du -sh $R/var/lib/apt/lists | cut -f1); dpkg info native $(du -sh $PREFIX/var/lib/dpkg/info | cut -f1) guest $(du -sh $R/var/lib/dpkg/info | cut -f1)"

echo
[ "$FAILS" -eq 0 ] && echo "ALR T2 NATIVE: PASS" || echo "ALR T2 NATIVE: OVER ($FAILS workload(s) above x1.2)"
