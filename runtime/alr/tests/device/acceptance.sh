#!/data/data/com.termux/files/usr/bin/bash
# On-device acceptance suite.  Emits the canonical acceptance strings from
# docs/07-acceptance.md so results can be grepped and diffed across runs.
#
# MUST run inside a real Termux context: the Android app seccomp filter only
# exists for zygote-spawned app processes, so adb shell / run-as would produce
# false PASSes.  This refuses to run anywhere else.
#
#   ALR_SSH_KEY=<key> ./scripts/dev-push.sh accept
#
# Exit 0 iff every gate passed.  KNOWN_FAIL entries are expected and do not
# fail the run; they are the honest record of what does not work yet.
set -u

cd "$(dirname "$0")/../.." 2>/dev/null || true
ALR=${ALR:-./alr}
export ALR_ROOT_DIR=${ALR_ROOT_DIR:-$HOME/alr-distros}
R="$ALR_ROOT_DIR/${ALR_DISTRO:-ubuntu-24.04}"
ALR_DISTRO_NAME="${ALR_DISTRO:-ubuntu-24.04}"

pass=0; fail=0; known=0; skip=0; pending=0

emit() { # emit <NAME> <PASS|FAIL|SKIP|KNOWN_FAIL:reason> [detail]
    printf '%-38s %s' "$1:" "$2"
    [ $# -ge 3 ] && printf '   %s' "$3"
    printf '\n'
    case "$2" in
        PASS) pass=$((pass+1));;
        FAIL) fail=$((fail+1));;
        SKIP) skip=$((skip+1));;
        KNOWN_FAIL*) known=$((known+1));;
        PENDING_DEVICE*) pending=$((pending+1));;
    esac
}

# ck <NAME> <expected> <command...>   — compares exact stdout
ck() { local n="$1" want="$2"; shift 2
    if [ -z "$want" ]; then
        emit "$n" FAIL "unfalsifiable check: empty expectation"
        return
    fi
    local got; got=$("$@" 2>/dev/null | head -1)
    [ "$got" = "$want" ] && emit "$n" PASS || emit "$n" FAIL "got='$got' want='$want'"
}
# ckc <NAME> <substring> <command...> — substring match
ckc() { local n="$1" want="$2"; shift 2
    # An empty needle matches everything: `case "$got" in *""*)` is always
    # true, so `ckc NAME "" cmd` is a check that CANNOT FAIL.  One shipped
    # (ALR GIT STATUS 10K) and passed against a repo the harness never created.
    # Refuse it here rather than trusting every future caller to notice.
    if [ -z "$want" ]; then
        emit "$n" FAIL "unfalsifiable check: empty expectation matches any output"
        return
    fi
    # search the WHOLE output: warnings (perl locale, apt notices) precede the
    # line we care about, so matching only the first line reports false FAILs
    local got; got=$("$@" 2>&1)
    case "$got" in *"$want"*) emit "$n" PASS;;
                   *) emit "$n" FAIL "no '$want' in output; first line: $(printf '%s' "$got"|head -1)";; esac
}
# ckrc <NAME> <expected rc> <command...>
ckrc() { local n="$1" want="$2"; shift 2
    "$@" >/dev/null 2>&1; local got=$?
    [ "$got" = "$want" ] && emit "$n" PASS || emit "$n" FAIL "rc=$got want=$want"
}
# ckre <NAME> <ERE> <command...> — first line matches the regex.  Use this
# instead of ckc for anything version-shaped: a pinned literal turns a healthy
# rootfs on a newer release into a FAIL.
ckre() { local n="$1" re="$2"; shift 2
    local got; got=$("$@" 2>/dev/null | head -1)
    printf '%s' "$got" | grep -qE "$re" \
        && emit "$n" PASS "$got" || emit "$n" FAIL "got='$got' want=~'$re'"
}

# ── validity gate: everything below is meaningless without this ──────────
uid=$(id -u)
sec=$(grep -oE '^Seccomp:[[:space:]]*[0-9]+' /proc/self/status | tr -dc 0-9)
ctx=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\0')
if [ "${uid:-0}" -lt 10000 ] || [ "${sec:-0}" != "2" ]; then
    emit "ACCEPTANCE CONTEXT" FAIL "uid=$uid Seccomp=$sec ctx=$ctx"
    echo
    echo "REFUSING: not a measurable context.  The app seccomp filter is installed"
    echo "by the zygote for uid>=10000 only, so adb shell (Seccomp: 0) and run-as"
    echo "would report every syscall as ALLOWED.  See scripts/dev-bootstrap.md."
    exit 2
fi
emit "ACCEPTANCE CONTEXT" PASS "uid=$uid Seccomp=$sec ${ctx%%:c*}"
[ -x "$ALR" ] || { emit "ALR BINARY" FAIL "not built: $ALR"; exit 2; }
[ -d "$R" ]   || { emit "ALR ROOTFS" FAIL "not installed: $R"; exit 2; }
echo

# ── M3: first boot ──────────────────────────────────────────────────────
echo "── M3 첫 부팅 ──"
ckrc "ALR BOOT /bin/true"        0     $ALR run /bin/true
ck   "ALR BOOT /bin/echo"        alr   $ALR run /bin/echo alr
ckrc "ALR BOOT bash -c true"     0     $ALR run /bin/bash -c true
ckc  "ALR GUEST GLIBC VERSION"   "2.39" $ALR run /lib/aarch64-linux-gnu/libc.so.6

# The counterproofs: these are what make ADR 0001 and ADR 0002 evidence
# rather than argument.  Both MUST fail, and fail in the specific way.
LP="$R/lib/aarch64-linux-gnu:$R/usr/lib/aarch64-linux-gnu:$R/lib:$R/usr/lib"
env -i "$R/lib/ld-linux-aarch64.so.1" --library-path "$LP" --inhibit-cache \
        --argv0 true "$R/usr/bin/true" >/dev/null 2>&1
rc=$?
[ $rc -eq 159 ] && emit "ADR0001 NO SUPERVISOR DIES" PASS "exit=159 (128+31 SIGSYS)" \
                || emit "ADR0001 NO SUPERVISOR DIES" FAIL "exit=$rc want 159"
env -i "$R/usr/bin/true" >/dev/null 2>&1
rc=$?
[ $rc -ne 0 ] && emit "ADR0002 BARE EXECVE FAILS" PASS "exit=$rc (PT_INTERP unresolved)" \
              || emit "ADR0002 BARE EXECVE FAILS" FAIL "bare execve unexpectedly worked"
echo

# ── M4: path virtualization ─────────────────────────────────────────────
echo "── CLI 공통 옵션 (docs/06-cli-spec.md §1.1/§1.2) ──"
ckc "CLI ENV OPTION"             "FOO=bar"      $ALR run -e FOO=bar /usr/bin/env
ck  "CLI ENV BEATS INHERITED"    wins           env FOO=inherited $ALR run -e FOO=wins /bin/sh -c 'echo $FOO'
ck  "CLI WORKDIR OPTION"         /usr/lib       $ALR run -w /usr/lib /bin/pwd
ck  "CLI WORKDIR SETS PWD"       /usr/lib       $ALR run -w /usr/lib /bin/sh -c 'echo $PWD'
ck  "CLI EXEC DDASH"             exec-ok        $ALR exec -- /bin/echo exec-ok
# -w goes through alr_rw(), the one rewriter.  Hand-concatenating root+path
# skipped the ".."-pops-at-root clamp and `-w /../..` chdir'd ABOVE the rootfs.
ck  "CLI WORKDIR CLAMPS DOTDOT"  /              $ALR run -w /../.. /bin/pwd
ck  "CLI WORKDIR NORMALIZES"     /etc           $ALR run -w /root/../etc /bin/pwd
ck  "CLI WORKDIR PWD CANONICAL"  /etc           $ALR run -w /root/../etc /bin/sh -c 'echo $PWD'
ck  "CLI WORKDIR SYSDIR PASS"    /proc          $ALR run -w /proc /bin/pwd
# A -e must REPLACE alr's own value, not sit beside it: envp allows duplicate
# keys, getenv(3) takes the first and bash takes the last.
ck  "CLI ENV REPLACES DEFAULT"   /zzz           $ALR run -e PATH=/zzz /bin/sh -c 'echo $PATH'
# Absolute paths throughout: the point of this case is PATH=/zzz, so anything
# resolved THROUGH PATH (grep) would not be found and the check would fail on
# its own setup rather than on the behaviour.
ck  "CLI ENV NO DUPLICATE"       1              $ALR run -e PATH=/zzz /bin/sh -c '/usr/bin/env | /bin/grep -c "^PATH="'
ckrc "CLI ENV LOCPATH RESERVED"  125            $ALR run -e LOCPATH=/x /bin/true
# The host OLDPWD is a Termux path; leaked, `cd -` lands on <R>/data/... and
# fails with a bare ENOENT that reads as a corrupt rootfs.
ck  "CLI OLDPWD NOT INHERITED"   unset          env OLDPWD=/data/data/com.termux/files/home $ALR run /bin/sh -c 'echo ${OLDPWD:-unset}'
ck  "CLI PWD STILL SET"          /root          env OLDPWD=/data/data/com.termux/files/home $ALR run /bin/sh -c 'echo $PWD'
ckrc "CLI ENV RESERVED REFUSED"  125            $ALR run -e ALR_ROOT=/evil /bin/true
ckrc "CLI BAD OPTION REFUSED"    125            $ALR run --bogus /bin/true
# cwd mapping falls back to /root when the host cwd is outside the rootfs
ck  "CLI CWD FALLBACK"           /root          $ALR run /bin/pwd

# The identity `alr version` reports must be the one the guest LOADS.  It was
# not: install_preload() copies the .so into the rootfs once and cmd_install
# returns early when the rootfs exists, so upgrading alr left the old copy in
# place while `alr version` printed the new hash (MEASURED 2026-08-03, guest
# 48efc48b vs reported 16167c4e).  The restore is unconditional -- a failure
# here must not leave the rootfs running a stale preload for every later check.
ckc "CLI VERSION SHOWS GUEST PRELOAD" "preload (guest)" $ALR version
_gp="$R/usr/lib/alr/libalr_preload.so"
if [ -r "$_gp" ] && [ -r "$PREFIX/share/alr/libalr_preload.so" ] \
   && ! cmp -s "$_gp" "$PREFIX/share/alr/libalr_preload.so"; then
    cp "$_gp" "$TMPDIR/.alr-preload-save" 2>/dev/null
    cp "$PREFIX/share/alr/libalr_preload.so" "$_gp" 2>/dev/null
    "$ALR" version >/dev/null 2>&1; _rc=$?
    cp "$TMPDIR/.alr-preload-save" "$_gp" 2>/dev/null; rm -f "$TMPDIR/.alr-preload-save"
    [ "$_rc" = 1 ] && emit "CLI VERSION DETECTS STALE PRELOAD" PASS \
                   || emit "CLI VERSION DETECTS STALE PRELOAD" FAIL "rc=$_rc want=1"
    cmp -s "$_gp" "$PREFIX/share/alr/libalr_preload.so" \
        && emit "CLI VERSION TEST RESTORED" FAIL "rootfs left with the wrong .so" \
        || emit "CLI VERSION TEST RESTORED" PASS
else
    emit "CLI VERSION DETECTS STALE PRELOAD" SKIP "no second .so to swap in"
fi
ckrc "CLI UPDATE COMPONENTS"     0  $ALR update-components
ckc  "CLI LIST"                  "$(basename "$R")"  $ALR list
# docs/03-supervisor-spec.md §6 names these two and neither existed in the
# struct.  Both are asserted to be PRESENT and, for passthrough, to actually
# move -- a counter nobody has seen change is the same defect one step along.
ckc  "SUPERVISOR STATS ELAPSED"  "elapsed_ms=" \
     env ALR_LOG=1 $ALR run /bin/true
_ps=$(env ALR_LOG=1 $ALR run /bin/bash -c 'kill -WINCH $$; kill -USR1 $$ 2>/dev/null; echo ok' 2>&1 \
      | grep -oE 'passthrough_signals=[0-9]+' | tail -1 | cut -d= -f2)
[ "${_ps:-0}" -ge 1 ] && emit "SUPERVISOR SIGNAL PASSTHROUGH" PASS "$_ps forwarded" \
                      || emit "SUPERVISOR SIGNAL PASSTHROUGH" FAIL "counter stayed at ${_ps:-unset}"
# elapsed_ms must track real time, not be a constant: a 0.4 s sleep has to
# report more than a trivial command.
_e1=$(env ALR_LOG=1 $ALR run /bin/true 2>&1 | grep -oE 'elapsed_ms=[0-9]+' | cut -d= -f2)
_e2=$(env ALR_LOG=1 $ALR run /bin/bash -c 'sleep 0.4' 2>&1 | grep -oE 'elapsed_ms=[0-9]+' | cut -d= -f2)
[ "${_e2:-0}" -gt "${_e1:-0}" ] && [ "${_e2:-0}" -ge 300 ] \
    && emit "SUPERVISOR ELAPSED TRACKS TIME" PASS "true=${_e1}ms sleep0.4=${_e2}ms" \
    || emit "SUPERVISOR ELAPSED TRACKS TIME" FAIL "true=${_e1} sleep0.4=${_e2}"
# A distro name becomes a path component AND is interpolated into a shell
# command, and `remove` deletes what it resolves to.  Nothing validated it.
ckrc "CLI REJECTS DOTDOT DISTRO" 125  $ALR -d ../escape run /bin/true
ckrc "CLI REJECTS ABS DISTRO"    125  $ALR -d /abs run /bin/true
# remove: the confirmation must actually protect, --force must actually skip
# it, and neither may touch a directory that is not one of ours.
_dsp="$ALR_ROOT_DIR/alrdisposable"
mkdir -p "$_dsp/usr/lib/alr"
echo wrong-name | $ALR remove alrdisposable >/dev/null 2>&1
[ -d "$_dsp" ] && emit "CLI REMOVE NEEDS CONFIRMATION" PASS \
               || emit "CLI REMOVE NEEDS CONFIRMATION" FAIL "deleted without a matching name"
echo alrdisposable | $ALR remove alrdisposable >/dev/null 2>&1
[ -d "$_dsp" ] && emit "CLI REMOVE ON CONFIRMATION" FAIL "survived a matching name" \
               || emit "CLI REMOVE ON CONFIRMATION" PASS
mkdir -p "$_dsp/usr/lib/alr"; $ALR remove alrdisposable --force >/dev/null 2>&1
[ -d "$_dsp" ] && emit "CLI REMOVE FORCE" FAIL "survived --force" \
               || emit "CLI REMOVE FORCE" PASS
# The refusal that matters: a directory with neither a guest ld.so nor
# usr/lib/alr is not ours, and --force must not override that.
_na="$ALR_ROOT_DIR/alrnotours"; mkdir -p "$_na/somefile.d"
$ALR remove alrnotours --force >/dev/null 2>&1
[ -d "$_na" ] && emit "CLI REMOVE REFUSES FOREIGN DIR" PASS \
              || emit "CLI REMOVE REFUSES FOREIGN DIR" FAIL "deleted a non-rootfs directory"
rm -rf "$_na" "$_dsp"
# Booting without the guest preload is not a degraded mode, it is a different
# product: every path resolves against Android.  It used to be silent --
# prepare() simply omits --preload -- and `alr install` even reported success
# after failing to install it, because the return value was discarded.  The
# restore is unconditional so a failure here cannot leave the rootfs crippled
# for every later check.
_pl="$R/usr/lib/alr/libalr_preload.so"
if [ -r "$_pl" ]; then
    mv "$_pl" "$TMPDIR/.alr-pl-save" 2>/dev/null
    _w=$("$ALR" run /bin/echo hi 2>&1 | grep -c 'reason=preload-missing-in-rootfs')
    _seen=$("$ALR" run /bin/cat /etc/os-release 2>/dev/null | head -1)
    mv "$TMPDIR/.alr-pl-save" "$_pl" 2>/dev/null
    [ "${_w:-0}" -ge 1 ] && emit "CLI WARNS UNVIRTUALIZED BOOT" PASS \
                         || emit "CLI WARNS UNVIRTUALIZED BOOT" FAIL "no warning"
    # Positive control for the warning's claim: with no preload the guest must
    # NOT be able to read the rootfs's /etc/os-release.
    [ -z "$_seen" ] && emit "CLI UNVIRTUALIZED SEES ANDROID" PASS \
                    || emit "CLI UNVIRTUALIZED SEES ANDROID" FAIL "read guest file: $_seen"
    [ -r "$_pl" ] && emit "CLI PRELOAD RESTORED" PASS \
                  || emit "CLI PRELOAD RESTORED" FAIL "rootfs left without a preload"
else
    emit "CLI WARNS UNVIRTUALIZED BOOT" SKIP "no preload in the rootfs to move"
fi
# A static guest binary runs but its paths resolve against ANDROID, not the
# rootfs (ADR 0008).  alr knew and said nothing at every verbosity; the first
# attempt at this warning fired on /usr/bin/git, because alr_classify only
# walks PT_INTERP when given an interp buffer and the caller passed NULL.
if [ -x "$R/usr/local/bin/codex" ]; then
    ckc "CLI STATIC BINARY WARNS"  "reason=unhooked-static-binary" \
        $ALR run /usr/local/bin/codex --version
else
    emit "CLI STATIC BINARY WARNS" SKIP "codex not installed"
fi
n=$($ALR run /usr/bin/git --version 2>&1 | grep -c 'unhooked-static-binary')
[ "${n:-1}" -eq 0 ] && emit "CLI DYNAMIC NO FALSE WARN" PASS \
                    || emit "CLI DYNAMIC NO FALSE WARN" FAIL "warned on a dynamic binary"

echo "── 워크로드 (docs/07 §2 이름들, 지금까지 러너가 없던 것) ──"
# These names carried PASS in docs/07 §2 with nothing behind them.  Measuring
# is better than de-tokenising: the names describe things we actually want.
# ALR LDSO INVOKE means "alr invokes the GUEST loader" (ADR 0002).  Running
# ld.so itself is not the way to see that -- it prints nothing through our own
# loader invocation.  What proves it is that the guest's ldd reports the guest
# glibc, i.e. the program was linked against the rootfs, not Termux's bionic.
ckc "ALR LDSO INVOKE"    "Ubuntu GLIBC"  $ALR run /usr/bin/ldd --version
ck  "ALR PIPELINE"       ok       $ALR run /bin/bash -c 'echo ok | /bin/cat | /usr/bin/head -1'
ckc "ALR FAKEROOT IDENTITY" "uid=0" env ALR_FAKEROOT=1 $ALR run /usr/bin/id
# NOT -qq: that is quiet by design and prints none of the lines this asserts.
# Was: matched "ports.ubuntu.com", which apt also prints in EVERY failure
# message ("Failed to fetch http://ports.ubuntu.com/..."). Assert the outcome
# apt only prints on success.
ckc "ALR APT UPDATE"     "Reading package lists" \
    env ALR_FAKEROOT=1 $ALR run /usr/bin/apt-get update
# The workload the copy fallback exists for -- ADR 0004's own test matrix opens
# with it and it had never been run.
# The setup's own status is NOT discarded any more.  It used to be, and it was
# failing the whole time -- `dpkg-deb -b` refuses a control directory with mode
# 0700, which is what Termux's umask 077 produced inside the guest until alr
# started booting it with Ubuntu's 022.  The check above still passed, because
# its needle "alrtest" also appears in dpkg's "cannot access archive
# /tmp/alrtest.deb" error.  Two independent holes, agreeing.
_debsetup=$($ALR run /bin/bash -c 'cd /tmp && rm -rf dpkgt alrtest.deb &&
    mkdir -p dpkgt/DEBIAN &&
    printf "Package: alrtest\nVersion: 1\nArchitecture: all\nMaintainer: t\nDescription: t\n" \
      > dpkgt/DEBIAN/control && dpkg-deb -b dpkgt /tmp/alrtest.deb' 2>&1)
if [ $? -ne 0 ]; then
    emit "ALR DPKG DEB BUILD" FAIL "$(printf '%s' "$_debsetup" | tail -1)"
else
    emit "ALR DPKG DEB BUILD" PASS
fi
# Was: matched "alrtest", a substring of the path dpkg echoes back on ERROR
# too ("cannot access archive /tmp/alrtest.deb"), with the .deb-building
# setup's own status discarded.  Assert the database instead: the package is
# only listed as installed if dpkg actually did it.
ckc "ALR DPKG LOCAL INSTALL" "install ok installed" \
    env ALR_FAKEROOT=1 $ALR run /bin/sh -c \
    'dpkg -i /tmp/alrtest.deb >/dev/null 2>&1; dpkg-query -W -f="\${Status}" alrtest'
# git clone --local is the other hardlink-heavy path (ADR 0004).
$ALR run /bin/bash -c 'rm -rf /tmp/gsrc /tmp/gdst && mkdir -p /tmp/gsrc && cd /tmp/gsrc &&
    git init -q . && echo x > a && git add -A &&
    git -c user.email=a@b -c user.name=c commit -qm i' >/dev/null 2>&1
ckc "ALR GIT CLONE LOCAL" "done" \
    $ALR run /usr/bin/git clone --local /tmp/gsrc /tmp/gdst
# symlinkat's target must NOT be rewritten while its dirfd path is -- the
# asymmetry docs/04 §5.3 calls out.
ck  "PRELOAD SYMLINKAT ASYMMETRY" ../etc/os-release \
    $ALR run /bin/bash -c 'rm -f /tmp/sla && ln -s ../etc/os-release /tmp/sla && readlink /tmp/sla'
# A shebang chain whose interpreter is ITSELF a shebang script.  alr appended
# each level's script path instead of prepending, so the final argv was
# "/bin/sh /tmp/s2 /tmp/s1" instead of "/bin/sh /tmp/s1 /tmp/s2": sh read the
# outer script, treated its "#!" line as a comment, and exited 0 printing
# nothing.  Native Termux runs the same pair correctly.
$ALR run /bin/bash -c 'printf "#!/bin/sh\necho deep-ok\n" > /tmp/s1 &&
    printf "#!/tmp/s1\n" > /tmp/s2 && chmod +x /tmp/s1 /tmp/s2' >/dev/null 2>&1
ck  "PRELOAD EXEC SHEBANG RECURSION" deep-ok  $ALR run /tmp/s2

# More docs/07 §2 names that carried PASS with no runner.
# Was: ckc "ALR GIT STATUS 10K" "" -- an empty needle, against /tmp/bigrepo,
# which nothing in this file creates.  It passed with git deleted from the
# rootfs.  Build the repo, then assert git actually walked it: `status
# --porcelain` on a clean tree prints NOTHING, so the falsifiable assertion is
# the rc plus a count from `status --porcelain` after a modification.
$ALR run /bin/sh -c 'rm -rf /tmp/bigrepo && mkdir -p /tmp/bigrepo && cd /tmp/bigrepo &&
    git init -q . && for i in $(seq 1 200); do echo x > f$i; done &&
    git add -A && git -c user.email=a@b -c user.name=c commit -qm init &&
    echo changed > f7 && echo new > untracked' >/dev/null 2>&1
ckc "ALR GIT STATUS 10K" " M f7" \
    $ALR run /usr/bin/git -C /tmp/bigrepo status --porcelain
# git hooks are a shebang+exec path through the guest, which is exactly what
# ADR 0002's loader invocation has to get right.
$ALR run /bin/bash -c 'cd /tmp/gsrc 2>/dev/null || exit 0;
    printf "#!/bin/sh\necho hook-ran\n" > .git/hooks/pre-commit &&
    chmod +x .git/hooks/pre-commit' >/dev/null 2>&1
ckc "ALR GIT HOOKS" "hook-ran" $ALR run /bin/bash -c \
    'cd /tmp/gsrc && echo y >> a && git add -A && git -c user.email=a@b -c user.name=c commit -m h'
# Was: matched "done", which the command's own trailing `; echo done` prints
# unconditionally -- it passed with no network, no git, and no clone.  Assert
# the artifact instead: a successful clone leaves a readable HEAD.
if $ALR run /bin/sh -c 'command -v git >/dev/null' >/dev/null 2>&1; then
    $ALR run /bin/bash -c \
        'rm -rf /tmp/ghttps && git clone -q --depth 1 https://github.com/git/git /tmp/ghttps' \
        >/dev/null 2>&1
    if $ALR run /bin/sh -c '[ -f /tmp/ghttps/.git/HEAD ]' >/dev/null 2>&1; then
        ckc "ALR GIT CLONE HTTPS" "ref:" $ALR run /bin/cat /tmp/ghttps/.git/HEAD
    else
        emit "ALR GIT CLONE HTTPS" SKIP "clone did not complete (no network?)"
    fi
else
    emit "ALR GIT CLONE HTTPS" SKIP "git not installed"
fi
# dlopen with an absolute guest path must be rewritten like any other path.
ckc "PRELOAD DLOPEN ABS PATH" "ok" $ALR run /usr/bin/python3 -c \
    'import ctypes; ctypes.CDLL("/lib/aarch64-linux-gnu/libm.so.6"); print("ok")'
# syscall(2) called directly must be rewritten too.  A C probe, not ctypes:
# CDLL("libc.so.6") dlsym's libc's OWN syscall and never consults the global
# scope where LD_PRELOAD lives, so the ctypes version measured dlsym semantics
# and reported a failure the interposer had not made.  See probe_syscall.c.
if [ -r "$R/usr/include/stdio.h" ]; then
    cp tests/device/probe_syscall.c "$R/tmp/probe_syscall.c" 2>/dev/null
    if $ALR run /usr/bin/gcc -O1 -o /tmp/probe_syscall /tmp/probe_syscall.c >/dev/null 2>&1; then
        ck "PRELOAD SYSCALL REWRITE" syscall-rewrite-ok $ALR run /tmp/probe_syscall
    else
        emit "PRELOAD SYSCALL REWRITE" SKIP "probe did not compile"
    fi
else
    emit "PRELOAD SYSCALL REWRITE" SKIP "no libc6-dev in the guest"
fi
# envp re-injection must be idempotent: a guest that execs again must not
# accumulate LD_PRELOAD entries.
ckc "PRELOAD EXEC ENVP IDEMPOTENT" "1" $ALR run /bin/bash -c \
    '/bin/bash -c "/bin/bash -c \"echo \$LD_PRELOAD\"" | tr : \\n | grep -c libalr_preload'
# The supervisor forwards signals it did not originate (§4.3).
ckc "SUPERVISOR SIGNAL FORWARD" "got-term" $ALR run /bin/bash -c \
    'trap "echo got-term" TERM; (sleep 0.2; kill -TERM $$) & sleep 0.5'
# bash reading a script from stdin is the shape an interactive session uses.
ck  "ALR BASH INTERACTIVE" bash-stdin $ALR run /bin/bash -s <<< 'echo bash-stdin'

# AF_UNIX addresses carry a path and nothing rewrote it: bind()/connect() were
# not interposed.  tmux was the visible victim; dbus, gpg-agent, X11 and every
# database socket under /var/run are in the same class.  The abstract-namespace
# case is the control that keeps the fix honest -- prefixing sun_path[0]=='\0'
# would invent a different address.
if [ -r "$R/usr/include/stdio.h" ]; then
    cp tests/device/probe_unixsock.c "$R/tmp/probe_unixsock.c" 2>/dev/null
    if $ALR run /usr/bin/gcc -O1 -o /tmp/probe_unixsock /tmp/probe_unixsock.c >/dev/null 2>&1; then
        ck "PRELOAD UNIX SOCKET PATH" "unix-path-ok abstract-ok" $ALR run /tmp/probe_unixsock
    else
        emit "PRELOAD UNIX SOCKET PATH" SKIP "probe did not compile"
    fi
else
    emit "PRELOAD UNIX SOCKET PATH" SKIP "no libc6-dev in the guest"
fi

# The rest of the path-taking surface, found by scripts/check-path-coverage.sh
# after bind()/connect() showed that "the calls we thought of" is not coverage.
# Seven checks, six positive and one negative:
#   xattr        `ls -l` printed "drwx------?" -- coreutils prints '?' when the
#                ACL probe ERRORS, and getxattr on an unrewritten path is a
#                bare ENOENT.  Under cp -a, tar --xattrs, rsync -X, getfacl.
#   inotify      every file watcher: node's fs.watch/chokidar and so every JS
#                dev server, inotifywait, entr.  Silently never fires.
#   pathconf     configure scripts; glibc's own readdir sizing.
#   getsockname  the OUT side of the bind fix -- the kernel hands back the
#                rewritten sun_path, so the guest learned the ANDROID address.
#   nftw         glibc walks with internal opendir/lstat that bypass the PLT.
#                The probe checks the CALLBACK's path and FTW.base, not just
#                that the walk succeeded.  (coreutils is unaffected: du, rm -r
#                and find carry gnulib's fts, which does go through the PLT.)
#   setmntent    regression guard; it is interposed because glibc's copy calls
#                fopen through an internal alias.
#   glob-guest   the NEGATIVE control.  glob() must return GUEST paths; a
#                "be thorough" pass that stopped rewriting the results would
#                break every caller, and this catches that.
if [ -r "$R/usr/include/stdio.h" ]; then
    cp tests/device/probe_pathcov.c "$R/tmp/probe_pathcov.c" 2>/dev/null
    if $ALR run /usr/bin/gcc -O1 -o /tmp/probe_pathcov /tmp/probe_pathcov.c >/dev/null 2>&1; then
        ck "PRELOAD PATH COVERAGE" \
           "xattr=ok inotify=ok pathconf=ok getsockname=ok nftw=ok setmntent=ok glob-guest=ok" \
           $ALR run /tmp/probe_pathcov
    else
        emit "PRELOAD PATH COVERAGE" SKIP "probe did not compile"
    fi
else
    emit "PRELOAD PATH COVERAGE" SKIP "no libc6-dev in the guest"
fi

# getaddrinfo's SERVICE argument was dropped for every name answered from
# /etc/hosts -- port 0, instant ECONNREFUSED, for `curl localhost:8080` and
# every database client and dev server in the guest.  The three RESOLV checks
# above could not see it: they compare ADDRESSES, never a port.
if [ -r "$R/usr/include/stdio.h" ]; then
    cp tests/device/probe_getaddrinfo_port.c "$R/tmp/probe_gaiport.c" 2>/dev/null
    if $ALR run /usr/bin/gcc -O1 -o /tmp/probe_gaiport /tmp/probe_gaiport.c >/dev/null 2>&1; then
        ck "RESOLV SERVICE PORT" "numeric=8080 named=80 none=0" $ALR run /tmp/probe_gaiport
    else
        emit "RESOLV SERVICE PORT" SKIP "probe did not compile"
    fi
else
    emit "RESOLV SERVICE PORT" SKIP "no libc6-dev in the guest"
fi

# ── static guests: host-form env + host-side wrappers (ADR 0008) ────────
# A static binary has no PT_INTERP, so LD_PRELOAD never loads and nothing it
# does is rewritten.  The answer is not to hook it -- that is impossible -- but
# to hand it an environment in which its UNREWRITTEN paths are already right:
# host-form HOME/TMPDIR/PWD, no LD_* (which its children would inherit and
# choke on), and a PATH of host-side wrappers that re-enter `alr run`.
_hb="$R/usr/lib/alr/hostbin"
if [ -d "$_hb" ]; then
    n=$(ls "$_hb" 2>/dev/null | wc -l)
    [ "${n:-0}" -ge 20 ] && emit "CLI HOSTBIN PRESENT" PASS "$n wrappers" \
                         || emit "CLI HOSTBIN PRESENT" FAIL "only ${n:-0}"
    # The wrapper must reach the GUEST's copy, not a host one: git is 2.43.0 in
    # the rootfs and Termux ships no git at all, so the version is the proof.
    ckc "CLI HOSTBIN RUNS GUEST TOOL" "git version 2." "$_hb/git" --version
    # And it must work with NOTHING inherited -- every child of a static binary
    # starts from that binary's environment, not the harness's.  The first
    # version read ALR_ROOT_DIR from the environment and silently resolved
    # against the default rootfs.
    ckc "CLI HOSTBIN SELF CONTAINED" "git version 2." \
        env -i PATH=/data/data/com.termux/files/usr/bin "$_hb/git" --version
else
    emit "CLI HOSTBIN PRESENT" FAIL "no $_hb; run alr update-components"
    emit "CLI HOSTBIN RUNS GUEST TOOL" SKIP "no hostbin"
    emit "CLI HOSTBIN SELF CONTAINED" SKIP "no hostbin"
fi

# codex is the reason this exists: 269 MB of static musl Rust, the exact shape
# ADR 0008 called unhookable.  `codex doctor` is the oracle because it reports
# whether codex can actually USE the tools it needs -- and it needs a nested
# `alr run` to reach them, which only works because an inner alr detects it is
# already traced and inherits the outer supervisor instead of failing to start
# a second one (MEASURED before that: pids=0, rc=126).
if [ -x "$R/usr/local/bin/codex" ]; then
    ckc "ALR CODEX FINDS GUEST GIT"  "git version 2."  \
        $ALR run /usr/local/bin/codex doctor
    ckc "ALR CODEX FINDS GUEST RG"   "ripgrep "        \
        $ALR run /usr/local/bin/codex doctor
    # Its config/state land in the rootfs, which only happens when HOME is
    # host-form.  With the guest form codex wrote to ANDROID's /root and
    # reported "could not create PATH aliases: Read-only file system".
    ckc "ALR CODEX CONFIG IN ROOTFS" "config       loaded" \
        $ALR run /usr/local/bin/codex doctor
else
    emit "ALR CODEX FINDS GUEST GIT"  SKIP "codex not installed"
    emit "ALR CODEX FINDS GUEST RG"   SKIP "codex not installed"
    emit "ALR CODEX CONFIG IN ROOTFS" SKIP "codex not installed"
fi

# ── alr doctor P11/P12 (docs/01-platform-facts.md §D) ───────────────────
# P11 and P12 sat in that table as design for the life of the project and were
# never implemented; other documents meanwhile claimed "P1~P12 를 전부 실행한다"
# and showed example output.  Now that they exist they get exercised, because
# P11's default is SKIP -- a probe that only ever reports SKIP has never been
# shown to distinguish anything.
#
# Three deterministic targets, all always present, one per branch:
#   /bin/true    dynamic C, calls libc for everything            -> 0, PASS
#   ld.so        has no PT_INTERP, so it reads as STATIC, and a
#                loader necessarily issues its own syscalls      -> STATIC
#   libc.so.6    the note in the output exists because of this one
if [ -x ./alr-doctor ]; then
    ckc "DOCTOR P11 DYNAMIC CLEAN" '0 `svc #0` -> PASS' \
        ./alr-doctor --scan "$R/bin/true"
    ckc "DOCTOR P11 STATIC FLAGGED" "STATIC (no PT_INTERP)" \
        ./alr-doctor --scan "$R/lib/ld-linux-aarch64.so.1"
    # The scan must READ the file, not merely accept the argument: a stub that
    # printed PASS for everything would pass the first check above.
    ckc "DOCTOR P11 COUNTS NONZERO" "-> WARN" \
        ./alr-doctor --scan "$R/lib/ld-linux-aarch64.so.1"
    ckc "DOCTOR P11 REJECTS NON ELF" "not a 64-bit ELF" \
        ./alr-doctor --scan "$R/etc/os-release"
    # A DIRECTORY sweeps -- docs/06 §3.1's shape, and the one that answers the
    # question a user has ("which of my binaries will not be virtualized?").
    # Scoped to /usr/lib/go-1.22 when present because the Go toolchain is the
    # canonical offender: static, and every one of them issues its own
    # syscalls.  Sweeping the whole rootfs takes ~18 s and belongs in doctor,
    # not in a suite that runs on every change.
    if [ -d "$R/usr/lib/go-1.22/bin" ]; then
        ckc "DOCTOR P11 SWEEPS DIR" "STATIC," ./alr-doctor --scan "$R/usr/lib/go-1.22/bin"
    else
        # /usr/bin always exists; assert the sweep RAN and counted, which is
        # the part a stub could not fake.
        ckc "DOCTOR P11 SWEEPS DIR" "executables scanned" ./alr-doctor --scan "$R/usr/bin"
    fi
    # Shared libraries must stay OUT of a sweep: /usr/lib/aarch64-linux-gnu is
    # nothing but libc and friends, each stuffed with svc, and a sweep that
    # counted them would report every rootfs as broken.
    ckc "DOCTOR P11 SKIPS LIBS" "0 issue their own syscalls or are static" \
        ./alr-doctor --scan "$R/usr/lib/aarch64-linux-gnu"
    ckc "DOCTOR P12 PHANTOM" "live descendants" ./alr-doctor
else
    for n in "DOCTOR P11 DYNAMIC CLEAN" "DOCTOR P11 STATIC FLAGGED" \
             "DOCTOR P11 COUNTS NONZERO" "DOCTOR P11 REJECTS NON ELF" \
             "DOCTOR P12 PHANTOM"; do
        emit "$n" SKIP "alr-doctor not built"
    done
fi

# `alr list` read only ALR_ROOT_DIR, so after `alr config set paths.root X`
# every other subcommand used X and this one reported "no rootfs installed".
_lh="$ALR_ROOT_DIR/alrlisthome"; rm -rf "$_lh"; mkdir -p "$_lh"
env HOME="$_lh" $ALR config set paths.root "$ALR_ROOT_DIR" >/dev/null 2>&1
ckc "CLI LIST HONOURS CONFIG ROOT" "$ALR_DISTRO_NAME" \
    env HOME="$_lh" ALR_ROOT_DIR= $ALR list
# `alr version` resolved the rootfs from ALR_DISTRO alone, so -d inspected a
# different rootfs than the one named and reported on the wrong preload.
ckc "CLI VERSION HONOURS -d" "alrnosuchdistro" \
    $ALR version -d alrnosuchdistro
rm -rf "$_lh"

# ── alr config (docs/06-cli-spec.md §2) ─────────────────────────────────
# Run under a scratch HOME so this never touches the user's real
# ~/.alr/config.toml -- a test suite that edits the machine it measures is not
# a test suite.
_cfghome="$ALR_ROOT_DIR/alrcfghome"; rm -rf "$_cfghome"; mkdir -p "$_cfghome"
ck  "CLI CONFIG GET DEFAULT" "ubuntu-24.04" \
    env HOME="$_cfghome" $ALR config get default_distro
# The confirmation line must report the value AFTER the write.  cfg() memoises
# and main() has already called it, so the first version printed the value from
# before the write -- "runtime.fakeroot = false" one line after writing true.
ckc "CLI CONFIG SET REPORTS NEW" "runtime.fakeroot = true   (config)" \
    env HOME="$_cfghome" $ALR config set runtime.fakeroot true
# The one that matters: does the file change what the runtime DOES.  A config
# command that only round-trips its own file proves nothing.
ck  "CLI CONFIG AFFECTS RUNTIME" "0" \
    env HOME="$_cfghome" $ALR run id -u
# ...and a flag still outranks it (§1.1: 기본 "설정값").
ck  "CLI CONFIG FLAG OUTRANKS" "$(id -u)" \
    env HOME="$_cfghome" $ALR run --no-fakeroot id -u
# An unknown key must be refused, not accepted-and-ignored: a typo'd setting
# that looks accepted is the classic config-file failure.
ckc "CLI CONFIG UNKNOWN KEY" "reason=config-unknown-key" \
    env HOME="$_cfghome" $ALR config get no_such_setting
ckc "CLI CONFIG BAD VALUE" "reason=config-bad-value" \
    env HOME="$_cfghome" $ALR config set runtime.log notanumber
rm -rf "$_cfghome"

# The Termux uid has no entry in the guest's user databases, so every ownership
# display degrades to a raw number -- MEASURED before the fix: `ls -ld /root`
# printed "6 10297 10297" and `whoami` said "cannot find name for user ID
# 10297".  docs/05 §3.2.  True in BOTH fakeroot modes: fakeroot lies about the
# process credentials, not about st_uid.
ck "CLI GUEST USERDB" "alr" $ALR run /usr/bin/whoami
ckc "CLI GUEST USERDB LS" " alr alr " $ALR run /bin/sh -c \
    'ls -ld /root | tr -s " "'
# tmux is the workload that exposed it.  A detached session that outlives the
# check, so `tmux ls` is answered by a live server rather than a race.
# Server and query in ONE `alr run`: the supervisor sets PTRACE_O_EXITKILL, so
# every tracee dies when it exits.  A daemonising process therefore cannot
# outlive the invocation that started it -- `tmux new-session -d` in one run
# and `tmux ls` in the next reports "no server running", and that is by design
# rather than a tmux failure.  Interactively the server lives as long as the
# `alr shell` that owns it.
if [ -x "$R/usr/bin/tmux" ]; then
    ckc "ALR PTY TMUX" "alracc" $ALR run /bin/bash -c \
        'tmux kill-server 2>/dev/null; tmux new-session -d -s alracc "sleep 5" && tmux ls'
else
    emit "ALR PTY TMUX" SKIP "tmux not installed"
fi

echo "── M4 경로 가상화 ──"
ckc "PRELOAD GUEST ETC"          "Ubuntu 24.04" $ALR run /bin/cat /etc/os-release
ck  "PRELOAD PROC SELF EXE"      /bin/readlink  $ALR run /bin/readlink /proc/self/exe
# bash's builtin `pwd` answers from its OWN logical $PWD and never calls
# getcwd, so the original form of this check proved nothing about the
# interposer.  `pwd -P` does call it.
ck  "PRELOAD GETCWD CANON"       /usr/bin       $ALR run /bin/bash -c 'cd /usr/bin && pwd -P'
# The ALLOCATING form, getcwd(NULL, 0), which coreutils reaches through
# xgetcwd().  It returned the raw HOST path until 2026-08-03: the copy-back
# was guarded on `n < sz` and sz is 0 for that form.
ck  "PRELOAD GETCWD ALLOCATING"  /etc           $ALR run /bin/bash -c 'cd /etc && /bin/pwd'
ck  "PRELOAD REALPATH CANON"     /usr/bin/dash  $ALR run /bin/bash -c 'readlink -f /bin/sh'
ckc "PRELOAD PATH SEARCH GUEST"  "Ubuntu GLIBC" $ALR run /bin/bash -c 'ldd --version'
# The escape that the normalize-before-sysdir ordering exists to stop: with the
# checks in the wrong order this reads ANDROID's /etc/os-release.
ck  "PRELOAD NORMALIZE BEFORE SYSDIR" "ID=ubuntu" \
    $ALR run /bin/bash -c 'grep -m1 ^ID= /proc/../etc/os-release'
ckc "PRELOAD SYSDIR PASSTHROUGH"  "Name:" $ALR run /bin/bash -c 'grep -m1 Name /proc/self/status'
# /proc/self/cmdline must show the GUEST argv, not the loader invocation.  Left
# raw it exposed every host path in --library-path to anything reading cmdline,
# and docs/04-preload-spec.md §7 required synthesising it from the start.
# Read it with `cat` directly and translate on the HOST side: routing through a
# guest shell would report the SHELL's argv, and putting the host-path pattern
# in the guest command line makes the check match itself.
cl=$($ALR run /bin/cat /proc/self/cmdline 2>/dev/null | tr '\0' ' ' | sed 's/ *$//')
[ "$cl" = "/bin/cat /proc/self/cmdline" ] \
    && emit "PRELOAD PROC CMDLINE" PASS \
    || emit "PRELOAD PROC CMDLINE" FAIL "got='$cl'"
case "$cl" in
    */data/data/*|*ld-linux*) emit "PRELOAD CMDLINE NO HOST PATH" FAIL "$cl";;
    *)                        emit "PRELOAD CMDLINE NO HOST PATH" PASS;;
esac
ckrc "PRELOAD OPENDIR"            0 $ALR run /bin/ls /
n=$($ALR run /bin/ls / 2>/dev/null | wc -l | tr -d ' ')
[ "${n:-0}" -ge 15 ] && emit "PRELOAD LS ENTRY COUNT" PASS "$n entries" \
                     || emit "PRELOAD LS ENTRY COUNT" FAIL "$n entries"

# An absolute symlink TARGET is resolved by the kernel against the real host
# root, below anything we can hook, so it must be stored relative instead.
# Leaving it absolute is what emptied apt's /tmp/apt-dpkg-install-XXXXXX and
# broke `alr install --with git`.
ck  "PRELOAD SYMLINK RELATIVIZED"  ../etc/os-release \
    $ALR run /bin/bash -c 'ln -sf /etc/os-release /tmp/alrsl && readlink /tmp/alrsl'
ckc "PRELOAD SYMLINK DEREFERENCES" "Ubuntu" \
    $ALR run /bin/bash -c 'head -1 /tmp/alrsl'
ck  "PRELOAD SYMLINK DEPTH"        ../../../etc/os-release \
    $ALR run /bin/bash -c 'ln -sf /etc/os-release /usr/local/bin/alrsl && readlink /usr/local/bin/alrsl'
# /proc, /sys and /dev are NOT rewritten, so targets into them must stay
# absolute -- relativizing those would break them.
ck  "PRELOAD SYMLINK SYSDIR ABS"   /proc/self/exe \
    $ALR run /bin/bash -c 'ln -sf /proc/self/exe /tmp/alrsy && readlink /tmp/alrsy'
$ALR run /bin/bash -c 'rm -f /tmp/alrsl /tmp/alrsy /usr/local/bin/alrsl' >/dev/null 2>&1

# The image itself ships absolute symlinks, created by tar and so never seen by
# the preload.  ubuntu-base has 20; /usr/bin/awk -> /etc/alternatives/awk was
# among them, which is why ucf, locales, mercurial and php died with
# "awk: not found".  `alr install` relativizes them after extraction.
#
# Scoped to the IMAGE, not to runtime state.  A guest program may legitimately
# create an absolute symlink at runtime, and for a STATIC guest the host form
# is the correct one -- codex writes
#   <R>/root/.codex/tmp/arg0/codex-arg0XXXX/apply_patch
#     -> <R>/usr/local/bin/codex
# so it can re-exec itself under a different argv[0].  Those still resolve for
# a dynamic guest too, because rw() passes an already-under-root path through
# unchanged.  Counting them made this check fail the moment codex started
# working, which is the opposite of what it is for.
n=$(find "$R" -type l -lname '/*' 2>/dev/null \
    | grep -vE '/(proc|sys|dev)/' \
    | grep -vE "^$R/(tmp|var/tmp|run)/" \
    | grep -vE "^$R/root/\\." | wc -l | tr -d ' ')
[ "${n:-1}" -eq 0 ] && emit "ROOTFS NO ABSOLUTE SYMLINKS" PASS \
                    || emit "ROOTFS NO ABSOLUTE SYMLINKS" FAIL "$n broken link(s)"
# Run an awk PROGRAM rather than matching its --version banner.  The banner was
# matched for the substring "awk", which held for 24.04's mawk ("mawk 1.3.4")
# and broke the moment breadth.sh installed gawk, whose banner is "GNU Awk" with
# a capital A and no lowercase "awk" anywhere in its output.  The test's subject
# is that /usr/bin/awk resolves through the guest and executes -- which this
# checks more strictly, and without caring which awk won the alternatives race.
ckc "ROOTFS AWK RESOLVES" "awkok" $ALR run /usr/bin/awk 'BEGIN{print "awkok"}'
# The ldconfig no-op must be registered as a dpkg diversion, not merely written:
# a libc-bin upgrade restores the real wrapper, that wrapper runs the STATIC
# ldconfig.real, and its failure leaves libc-bin half-configured -- which makes
# every LATER apt install die too.  Check the database, not the file.
n=$(grep -c '^/usr/sbin/ldconfig$' "$R/var/lib/dpkg/diversions" 2>/dev/null)
[ "${n:-0}" -ge 1 ] && emit "ROOTFS LDCONFIG DIVERTED" PASS \
                    || emit "ROOTFS LDCONFIG DIVERTED" FAIL "no diversion registered"
# Was: expected rc=1 from `dpkg -l | grep -q "^i[^i]"`.  grep returns 1 when
# it matches nothing -- including when dpkg is absent and the output is empty,
# so deleting dpkg from the rootfs produced the same PASS as a healthy one.
# Two assertions now: dpkg must actually list packages, and none may be in a
# half-configured state.
ckc "ROOTFS DPKG LISTS PACKAGES" "ii " $ALR run /bin/sh -c 'dpkg -l | head -20'
ckrc "ROOTFS NO BROKEN PACKAGES" 1 \
    $ALR run /bin/sh -c 'dpkg -l 2>/dev/null | grep -q "^i[^i]"' 

# glibc 2.34+ builds nss_files into libc and opens the LITERAL /etc/passwd and
# /etc/group through an internal fopen, so without our own files backend every
# lookup answers from Android's /system/etc instead of the rootfs.
ckc "PRELOAD NSS GETPWNAM"  "root:x:0:0:"  $ALR run /usr/bin/getent passwd root
ck  "PRELOAD NSS GETGRNAM"  "root:x:0:"    $ALR run /usr/bin/getent group root
ckc "PRELOAD NSS GETPWUID"  "root"         $ALR run /usr/bin/getent passwd 0
n=$($ALR run /usr/bin/getent passwd 2>/dev/null | wc -l | tr -d ' ')
[ "${n:-0}" -ge 10 ] && emit "PRELOAD NSS ENUMERATE" PASS "$n entries" \
                     || emit "PRELOAD NSS ENUMERATE" FAIL "$n entries"

# Covers two interpositions at once: shadow aborts outright if the NETLINK_AUDIT
# socket fails with anything but EPROTONOSUPPORT, and then cannot lock unless
# lckpwdf() reaches the rootfs's /etc/.pwd.lock rather than the host's.
if $ALR run /usr/bin/test -x /sbin/groupadd >/dev/null 2>&1; then
    out=$(env ALR_FAKEROOT=1 $ALR run /sbin/groupadd -g 65123 alrck 2>&1)
    env ALR_FAKEROOT=1 $ALR run /sbin/groupdel alrck >/dev/null 2>&1
    case "$out" in
        *"cannot lock"*)     emit "PRELOAD SHADOW LOCK" FAIL "$out" ;;
        *"audit interface"*) emit "PRELOAD SHADOW AUDIT" FAIL "$out" ;;
        "")                  emit "PRELOAD SHADOW LOCK" PASS ;;
        *)                   emit "PRELOAD SHADOW LOCK" FAIL "$out" ;;
    esac
else
    emit "PRELOAD SHADOW LOCK" SKIP "no /sbin/groupadd"
fi
echo

# ── M5: exec continuity ─────────────────────────────────────────────────
echo "── M5 exec 연속성 ──"
ck  "PRELOAD EXEC DYNAMIC"       OLLEH $ALR run /bin/bash -c 'echo hello | tr a-z A-Z | rev'
ck  "PRELOAD EXEC NESTED"        NEST_OK $ALR run /bin/bash -c 'bash -c "bash -c \"echo NEST_OK\""'
ck  "PRELOAD EXEC SHEBANG"       SHEBANG_OK $ALR run /bin/bash -c \
    'printf "#!/bin/sh\necho SHEBANG_OK\n" > /tmp/t.sh; chmod +x /tmp/t.sh; /tmp/t.sh'
ck  "PRELOAD EXEC SHEBANG ENV"   ENV_OK $ALR run /bin/bash -c \
    'printf "#!/usr/bin/env sh\necho ENV_OK\n" > /tmp/e.sh; chmod +x /tmp/e.sh; /tmp/e.sh'
ck  "PRELOAD EXEC FIND"          /etc/os-release \
    $ALR run /usr/bin/find /etc -maxdepth 1 -name os-release
echo

# ── M6 preflight (package managers respond; transactions need fakeroot) ──
echo "── M6 사전점검 ──"
ckc "ALR APT VERSION"            "apt 2." $ALR run /usr/bin/apt-get --version
ck  "ALR DPKG ARCH"              arm64    $ALR run /usr/bin/dpkg --print-architecture
ckc "ALR DPKG VERSION"           "Debian" $ALR run /usr/bin/dpkg --version
echo

# ── M6: package manager (needs fakeroot) ────────────────────────────────
echo "── M6 패키지 매니저 ──"
if $ALR run /usr/bin/git --version >/dev/null 2>&1; then
    ck  "ALR GIT VERSION"  "git version 2.43.0" $ALR run /usr/bin/git --version
    ckc "ALR APT INSTALLED git" "git is already" \
        env ALR_FAKEROOT=1 $ALR run /usr/bin/apt-get install -y --no-install-recommends git
    ckc "ALR PERL DLOPEN"  "OK" $ALR run /usr/bin/perl -e 'use Cwd; print "OK\n"'
    # git's own workloads
    ckc "ALR GIT INIT"     "INIT_OK" $ALR run /bin/bash -c \
        'rm -rf /tmp/r && mkdir -p /tmp/r && cd /tmp/r && git init -q . && echo INIT_OK'
    ckc "ALR GIT COMMIT"   "COMMIT_OK" $ALR run /bin/bash -c \
        'cd /tmp/r && git config user.email a@b && git config user.name a && \
         echo x > f && git add f && git commit -qm t && echo COMMIT_OK'
    ckc "ALR GIT STATUS"   "STATUS_OK" $ALR run /bin/bash -c \
        'cd /tmp/r && git status --porcelain >/dev/null && echo STATUS_OK'
    ckc "ALR GIT CLONE LOCAL" "CLONE_OK" $ALR run /bin/bash -c \
        'rm -rf /tmp/r2 && git clone -q /tmp/r /tmp/r2 && echo CLONE_OK'
else
    emit "ALR GIT VERSION" SKIP "git not installed; run apt-get install git"
fi
echo

# ── M7: target workloads ────────────────────────────────────────────────
echo "── M7 타깃 워크로드 ──"
if [ -x "$R/opt/node/bin/node" ]; then
    # Any working major -- pinning v22 made a v24 rootfs report FAIL for a
    # product that was fine. Assert the shape, not the release.
    ckre "ALR NODE VERSION"  '^v[0-9][0-9]*\.[0-9]' $ALR run /opt/node/bin/node --version
    # process.execPath must be the GUEST path: npm/npx respawn through it
    ck  "ALR NODE EXECPATH"  /opt/node/bin/node \
        $ALR run /opt/node/bin/node -e 'console.log(process.execPath)'
    # libuv issues raw path syscalls; without syscall() interposition this ENOENTs
    ckc "ALR NODE FS STAT"   STAT_OK $ALR run /opt/node/bin/node -e \
        'console.log(require("fs").statSync("/etc/os-release").size>0?"STAT_OK":"BAD")'
    # Node>=20 calls io_uring_setup at loop init and dies on SIGSYS without rescue
    ckc "ALR NODE IO_URING SURVIVE" URING_OK $ALR run /opt/node/bin/node -e \
        'require("fs").promises.readFile("/etc/hostname").then(()=>console.log("URING_OK")).catch(()=>console.log("URING_OK"))'
    ckre "ALR NPM VERSION"   '^[0-9][0-9]*\.[0-9]' $ALR run /usr/local/bin/npm --version
    ckc "ALR NODE SHEBANG"   NODE_SHEBANG_OK $ALR run /bin/bash -c \
        'printf "#!/usr/bin/env node\nconsole.log(\"NODE_SHEBANG_OK\")\n">/tmp/s.js; chmod +x /tmp/s.js; /tmp/s.js'
else
    emit "ALR NODE VERSION" SKIP "node not installed"
fi
if [ -x "$R/usr/local/bin/codex" ]; then
    # codex ships as a STATIC binary (no INTERP, no NEEDED), so LD_PRELOAD cannot
# reach it: it runs with NO path virtualization and sees ANDROID's filesystem,
# not the rootfs.  `codex --version` succeeding proves the binary executes -- it
# does NOT prove codex operates inside the guest.  Track the linkage so a future
# dynamic build is noticed rather than assumed.
# POSITIVE CONTROL FIRST.  Without it this check cannot fail: if llvm-readelf
# is absent the pipeline produces nothing, grep matches nothing, and the else
# branch reports "static as expected" -- a missing TOOL and a real measurement
# give the same PASS.  That is the identical defect fixed ten lines below in
# PRELOAD LINK FALLBACK; a negative is evidence only when the instrument is
# known to answer.  /bin/bash is dynamic on any sane rootfs, so if the tool
# cannot find NEEDED in THAT, it is the tool that is broken.
if ! llvm-readelf -d "$R/bin/bash" 2>/dev/null | grep -q NEEDED; then
    emit "ALR CODEX LINKAGE" SKIP "llvm-readelf unusable; cannot classify"
elif llvm-readelf -d "$R/usr/local/bin/codex" 2>/dev/null | grep -q NEEDED; then
    emit "ALR CODEX LINKAGE" PASS "dynamic - preload reaches it"
else
    # NOT a KNOWN_FAIL: that token means "we want this and it does not work".
    # ADR 0008 removed codex from G5 -- static linking leaves no dynamic symbol
    # to interpose, so this is an observation, not a defect.  The check stays
    # because it is the instrument that would tell us if codex ever ships a
    # dynamic build.
    emit "ALR CODEX LINKAGE" PASS \
         "static as expected (ADR 0008 non-goal): no path virtualization"
fi
ckc "ALR CODEX VERSION" "codex-cli" $ALR run /usr/local/bin/codex --version
else
    emit "ALR CODEX VERSION" SKIP "codex not installed"
fi
echo

# ── supervisor invariants — the line separating this from PRoot ─────────
echo "── 슈퍼바이저 불변식 ──"
stats=$(ALR_LOG=1 $ALR run /bin/bash -c 'ls / >/dev/null' 2>&1 >/dev/null | tail -1)
case "$stats" in
  *"path_traps=0"*"syscall_stops=0"*) emit "SUPERVISOR NO SYSCALL STOPS" PASS "$stats";;
  *) emit "SUPERVISOR NO SYSCALL STOPS" FAIL "$stats";;
esac
sig=$(printf '%s' "$stats" | grep -oE 'sigsys=[0-9]+' | cut -d= -f2)
[ "${sig:-99}" -le 8 ] && emit "SUPERVISOR SIGSYS PER RUN" PASS "sigsys=$sig (<=8)" \
                       || emit "SUPERVISOR SIGSYS PER RUN" FAIL "sigsys=$sig"
echo

# ── stability ───────────────────────────────────────────────────────────
ok=0; for i in 1 2 3 4 5 6 7 8 9 10; do $ALR run /bin/ls / >/dev/null 2>&1 && ok=$((ok+1)); done
[ $ok -eq 10 ] && emit "ALR STABILITY 10x" PASS "10/10" || emit "ALR STABILITY 10x" FAIL "$ok/10"

# ── documented gaps: asserted so they cannot regress silently ───────────
echo
echo "── 프로세스 생성 (docs/04-preload-spec.md §9.4) ──"
# glibc's posix_spawn/system/popen reach the kernel through internal aliases
# (__spawnix, an internal execve), never our execve, so their children were
# exec'd with PT_INTERP resolved against the ANDROID root.  Every shell-based
# test passed while this was broken -- so verify with the REAL consumers:
# GNU make 4.3+ spawns each recipe line with posix_spawn, and CPython's
# os.system is libc system().
if $ALR run /usr/bin/test -x /usr/bin/make >/dev/null 2>&1; then
    ckc "SPAWN MAKE RECIPE" MAKEOK $ALR run /bin/sh -c \
        'cd /tmp && printf "all:\n\t@echo MAKEOK\n" > alrmk.mk && make -s -f alrmk.mk'
else
    emit "SPAWN MAKE RECIPE" SKIP "make not installed"
fi
if $ALR run /usr/bin/test -x /usr/bin/python3 >/dev/null 2>&1; then
    ckc "SPAWN LIBC SYSTEM"    SYSOK  $ALR run /usr/bin/python3 -c 'import os;os.system("echo SYSOK")'
    ckc "SPAWN LIBC SYSTEM RC" "rc=0" $ALR run /usr/bin/python3 -c 'import os;print("rc=%d"%os.system("true"))'
else
    emit "SPAWN LIBC SYSTEM" SKIP "python3 not installed"
fi
# posix_spawn and popen have no dependable CLI consumer, so bind them directly.
# Guard on the HEADER, not just the compiler: `apt install gcc
# --no-install-recommends` leaves libc6-dev out, and the probe then fails to
# build with "spawn.h: No such file" -- which reads identically to the runtime
# failure it is meant to detect.
if $ALR run /usr/bin/test -f /usr/include/spawn.h >/dev/null 2>&1; then
    cp tests/device/probe_spawn.c "$R/tmp/" 2>/dev/null
    if $ALR run /usr/bin/gcc -o /tmp/alrspawn /tmp/probe_spawn.c >/dev/null 2>&1; then
        ckc "SPAWN POSIX_SPAWN" PSOK    $ALR run /tmp/alrspawn
        ckc "SPAWN POPEN"       POPENOK $ALR run /tmp/alrspawn
    else
        emit "SPAWN POSIX_SPAWN" FAIL "probe did not compile"
    fi
else
    emit "SPAWN POSIX_SPAWN" SKIP "no libc6-dev (spawn.h absent)"
fi

# Android's SELinux ioctlcmd filter denies TCGETS2/TIOCGSID/TIOCGETD/TIOCEXCL on
# a PTY slave; §11 answers them locally.  TIOCSTI must STAY denied -- it is
# neverallowxperm and pretending otherwise would silently drop injected input.
# MEASURED: FIONREAD is allowed natively, which falsified §11's premise that it
# needed emulating through the master fd.
if $ALR run /usr/bin/test -f /usr/include/spawn.h >/dev/null 2>&1; then
    cp tests/device/probe_ioctl.c "$R/tmp/" 2>/dev/null
    if $ALR run /usr/bin/gcc -o /tmp/alrio /tmp/probe_ioctl.c >/dev/null 2>&1; then
        out=$($ALR run /tmp/alrio 2>&1)
        bad=$(printf '%s\n' "$out" | awk '/^(TCGETS2|TIOCGSID|TIOCGETD|TIOCEXCL|FIONREAD) / && $2 != "ok"')
        [ -z "$bad" ] && emit "IOCTL PTY TRANSLATED" PASS \
                      || emit "IOCTL PTY TRANSLATED" FAIL "$(printf '%s' "$bad" | tr '\n' ';')"
        printf '%s\n' "$out" | grep -q '^TIOCSTI *Permission denied' \
            && emit "IOCTL TIOCSTI STAYS DENIED" PASS \
            || emit "IOCTL TIOCSTI STAYS DENIED" FAIL "TIOCSTI must never succeed"
    else
        emit "IOCTL PTY TRANSLATED" FAIL "probe did not compile"
    fi
else
    emit "IOCTL PTY TRANSLATED" SKIP "no libc6-dev (spawn.h absent)"
fi

echo
echo "── 호스트 이름 해석 ──"
# The bridge answers from BIONIC, so it reads Android's /system/etc/hosts --
# the rootfs's own /etc/hosts had no consumer on either path until the files
# backend landed.  Use an address Android cannot possibly supply.
$ALR run /bin/sh -c \
    'grep -q alracc /etc/hosts || echo "10.77.77.77 alracc.invalid" >> /etc/hosts' \
    >/dev/null 2>&1
ckc "RESOLV HOSTS FILES"    10.77.77.77    $ALR run /usr/bin/getent hosts  alracc.invalid
ckc "RESOLV AHOSTS FILES"   10.77.77.77    $ALR run /usr/bin/getent ahosts alracc.invalid
ckc "RESOLV REVERSE FILES"  alracc.invalid $ALR run /usr/bin/getent hosts  10.77.77.77
# `getent ahosts` is the getaddrinfo path (already bridged); `getent hosts` is
# the legacy gethostby* path, which was NOT.  Both must resolve.
if $ALR run /usr/bin/getent ahosts ports.ubuntu.com >/dev/null 2>&1; then
    ckc "RESOLV LEGACY DNS" ports.ubuntu.com $ALR run /usr/bin/getent hosts ports.ubuntu.com
else
    emit "RESOLV LEGACY DNS" SKIP "no network"
fi

# THE BRIDGE-ABSENT PATH.  getaddrinfo falls back to glibc's own resolver when
# alr's resolver daemon is not running -- documented as correct for devices
# without Private DNS or a VPN, so it is a supported path, not an error path.
#
# It ABORTED.  glibc allocates an addrinfo chain as ONE block, with ai_addr and
# ai_canonname pointing inside it, while our freeaddrinfo() frees the three
# separately because that is how the bridge path allocates them.  Handing back
# glibc's structure made the caller's freeaddrinfo() free an interior pointer:
#
#   $ ALR_RESOLV_SOCK= alr run dig -v
#   free(): invalid pointer
#
# One failed daemon away from crashing every name lookup in the guest, and
# nothing here noticed, because all three RESOLV checks above answer from
# /etc/hosts and never reach the bridge at all.
ckc "RESOLV BRIDGE ABSENT" "localhost" \
    env ALR_RESOLV_SOCK= $ALR run /usr/bin/getent hosts localhost
if [ -x "$R/usr/bin/dig" ]; then
    # dig is what actually caught both bugs, via the breadth sweep rather than
    # this suite.  It is here now: it calls getaddrinfo during startup even for
    # `-v`, so it exercises the bridge without needing a network.
    ckc "ALR DIG VERSION" "DiG 9." $ALR run /usr/bin/dig -v
    ckc "ALR DIG VERSION NO BRIDGE" "DiG 9." \
        env ALR_RESOLV_SOCK= $ALR run /usr/bin/dig -v
else
    emit "ALR DIG VERSION" SKIP "dnsutils not installed"
    emit "ALR DIG VERSION NO BRIDGE" SKIP "dnsutils not installed"
fi

echo
echo "── 알려진 미구현 (회귀 감시) ──"
# Android denies /proc/stat to app UIDs, so libuv finds no cpuN lines and
# os.cpus() returned [].  The COUNT is real (sched_getaffinity works); the time
# fields are zeros and must not be read as utilisation.
if [ -x "$R/opt/node/bin/node" ]; then
    n=$($ALR run /opt/node/bin/node -e 'console.log(require("os").cpus().length)' 2>/dev/null)
    [ "${n:-0}" -ge 1 ] && emit "PROC STAT CPU COUNT" PASS "$n cpus" \
                        || emit "PROC STAT CPU COUNT" FAIL "os.cpus() = ${n:-?}"
    ckc "PROC CPUINFO UNTOUCHED" "CPU part" $ALR run /bin/grep -m1 "CPU part" /proc/cpuinfo
else
    emit "PROC STAT CPU COUNT" SKIP "node not installed"
fi
# Scan the WHOLE table, not just line 1: a leak on any row is a leak.  Checks
# /proc/mounts, /proc/self/mountinfo and the setmntent() path df uses.
leak=""
for f in /proc/mounts /proc/self/mounts /proc/self/mountinfo; do
    t=$($ALR run /bin/cat "$f" 2>/dev/null)
    case "$t" in
      *"/dev/block"*|*erofs*|*/apex/*|*/vendor*|*/data/data/*|*/storage/*)
        leak="$f: $(printf '%s' "$t" | grep -m1 -E '/dev/block|erofs|/apex/|/vendor|/data/data/|/storage/')";;
    esac
    [ -n "$t" ] || leak="$f: empty"
done
[ -z "$leak" ] && emit "PRELOAD PROC MOUNTS" PASS \
               || emit "PRELOAD PROC MOUNTS" FAIL "$leak"
ckc "PRELOAD DF READS SYNTH" "/dev/root" $ALR run /bin/df /
nm=$($ALR run /bin/bash -c 'grep -m1 Name /proc/self/status' 2>/dev/null)
case "$nm" in
  *ld-linux*) emit "PRELOAD PROC SELF STATUS" FAIL "loader name visible: $nm";;
  *grep*)     emit "PRELOAD PROC SELF STATUS" PASS "$nm";;
  *)          emit "PRELOAD PROC SELF STATUS" FAIL "unexpected: $nm";;
esac
# comm and status must agree -- one prctl feeds both, so a mismatch means the
# name was set somewhere else and only half the views are corrected.
ck "PRELOAD PROC SELF COMM" cat $ALR run /bin/cat /proc/self/comm
# The identity must travel through exec re-dispatch, not just the first program:
# ALR_GUEST_EXE feeds /proc/self/exe, and npm re-spawns node from it through a
# shell wrapper.  Before this was fixed, every child reported its PARENT.
ck "PRELOAD EXEC IDENTITY EXE"  /usr/bin/readlink \
    $ALR run /bin/bash -c 'readlink /proc/self/exe'
ck "PRELOAD EXEC IDENTITY COMM" cat \
    $ALR run /bin/bash -c 'bash -c "cat /proc/self/comm"' 
$ALR run /bin/bash -c 'f=$(mktemp /tmp/alrXXXXXX) && echo ok > "$f" && cat "$f"' 2>/dev/null \
    | grep -q ok && emit "PRELOAD MKSTEMP" PASS \
                 || emit "PRELOAD MKSTEMP" KNOWN_FAIL:mkstemp-not-interposed
# stat() and fstat() must agree on a copy-fallback "hardlink" -- docs/04 §6.2
# documents the failure: without fstat/fstat64 interposed, stat(path) reports
# nlink=2 while fstat(fd) on the same file reports 1 with a DIFFERENT inode, and
# dpkg's integrity check breaks on the disagreement.  Measured control (M13):
# with the wrappers removed this prints MISMATCH, ino 355816 vs 355817.
# Must be ONE process -- the identity table is per-process by design (§8.2), so
# a shell test with `ln` and the check in separate processes proves nothing.
if $ALR run /usr/bin/test -f /usr/include/spawn.h >/dev/null 2>&1; then
    cp tests/device/probe_nlink.c "$R/tmp/" 2>/dev/null
    if $ALR run /usr/bin/gcc -o /tmp/alrnl /tmp/probe_nlink.c >/dev/null 2>&1; then
        ckc "PRELOAD LINK IDENTITY NLINK" NLINKOK $ALR run /tmp/alrnl
    else
        emit "PRELOAD LINK IDENTITY NLINK" FAIL "probe did not compile"
    fi
else
    emit "PRELOAD LINK IDENTITY NLINK" SKIP "no libc6-dev (spawn.h absent)"
fi
# THIS CHECK COULD NOT FAIL.  It grepped ln's output for "denied|not
# permitted" and emitted PASS when nothing matched -- but the preload's link()
# falls back to copy_path()+lid_record() on EACCES/EPERM/EXDEV, so ln SUCCEEDS,
# nothing ever matched, and PASS was unconditional.  It was inside the headline
# number.
#
# What the fallback actually promises: ln succeeds, and the result has the same
# CONTENT as the source.  Assert both, and read them from one guest invocation
# so a boot failure cannot look like success.
l2s=$($ALR run /bin/bash -c '
        rm -f /tmp/l2s.probe
        ln /etc/os-release /tmp/l2s.probe 2>/dev/null || { echo "rc-fail"; exit 0; }
        cmp -s /etc/os-release /tmp/l2s.probe && echo "ok" || echo "content-differs"
      ' 2>/dev/null | tail -1)
case "$l2s" in
    ok)              emit "PRELOAD LINK FALLBACK" PASS "ln succeeded, content identical";;
    rc-fail)         emit "PRELOAD LINK FALLBACK" FAIL "ln failed; the copy fallback did not fire";;
    content-differs) emit "PRELOAD LINK FALLBACK" FAIL "ln succeeded but the copy differs";;
    *)               emit "PRELOAD LINK FALLBACK" FAIL "guest produced no answer: '$l2s'";;
esac
# Probe with a WRITE, not an open: `: < /dev/full` is O_RDONLY and would pass
# on a /dev/zero redirect that never delivers ENOSPC -- exactly the shortcut
# docs/04-preload-spec.md §12 forbids.  /dev/full is a deliberate non-goal
# (docs/RISKS.md): serving it means interposing write(), the hottest syscall in
# the process, for a device node no target workload touches, and every stdio
# symbol missed would fail SILENTLY as a successful write.
$ALR run /bin/bash -c 'echo x > /dev/full' 2>&1 | grep -qi 'no space left' \
    && emit "PRELOAD DEV FULL ENOSPC" PASS \
    || emit "PRELOAD DEV FULL ENOSPC" KNOWN_FAIL:non-goal-devfull

echo
echo "─────────────────────────────────────────────────────────────"
# PENDING_DEVICE is in the status vocabulary (docs/07 §1) and the milestone
# rules depend on it, and until now emit() had no arm for it and no runner used
# it -- a status nothing could produce or count is a word, not a status.
if [ "${pending:-0}" -gt 0 ]; then
    echo "  PASS=$pass  FAIL=$fail  KNOWN_FAIL=$known  SKIP=$skip  PENDING_DEVICE=$pending"
else
    echo "  PASS=$pass  FAIL=$fail  KNOWN_FAIL=$known  SKIP=$skip"
fi
echo "ALR DEVICE ACCEPTANCE: $([ $fail -eq 0 ] && echo PASS || echo FAIL)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
