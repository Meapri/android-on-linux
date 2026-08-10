#!/data/data/com.termux/files/usr/bin/bash
# Compatibility breadth: docs/07-acceptance.md §5.
#
# Installs a curated set of Ubuntu noble packages and smoke-runs each one.
# The number this produces is the project's headline claim -- alr's case
# against grun is Ubuntu-archive breadth, not speed -- so it must be honest:
#
#   * install and run are scored SEPARATELY. A package that installs but
#     cannot run is not a pass.
#   * batch installs are for speed only; every package is then attributed
#     INDIVIDUALLY via dpkg-query, and any batch failure is retried one by one
#     so a single bad package cannot mark nine good ones as failed.
#   * failures record the actual stderr tail, not a category we guessed.
#
# Emits a TSV so runs can be diffed:  pkg <TAB> install <TAB> run <TAB> detail
#
#   ALR_SSH_KEY=<key> ./scripts/dev-push.sh breadth
set -u

cd "$(dirname "$0")/../.." 2>/dev/null || true
ALR=${ALR:-./alr}
export ALR_ROOT_DIR=${ALR_ROOT_DIR:-$HOME/alr-distros}
R="$ALR_ROOT_DIR/${ALR_DISTRO:-ubuntu-24.04}"
OUT=${OUT:-breadth-results.tsv}
BATCH=${BATCH:-8}

# ── validity gate ────────────────────────────────────────────────────────
uid=$(id -u)
sec=$(grep -oE '^Seccomp:[[:space:]]*[0-9]+' /proc/self/status | tr -dc 0-9)
if [ "${uid:-0}" -lt 10000 ] || [ "${sec:-0}" != "2" ]; then
    echo "REFUSING: not a measurable context (uid=$uid Seccomp=$sec)."
    echo "The app seccomp filter exists for zygote-spawned uid>=10000 only."
    exit 2
fi
[ -x "$ALR" ] || { echo "alr not built: $ALR"; exit 2; }
[ -d "$R" ]   || { echo "rootfs not installed: $R"; exit 2; }

# pkg <TAB> smoke command.  Commands are chosen to exit 0 on success; where a
# tool has no clean --version the smoke is a minimal real operation instead.
PKGS=$(cat <<'LIST'
gcc	gcc --version
g++	g++ --version
make	make --version
cmake	cmake --version
ninja-build	ninja --version
pkg-config	pkg-config --version
autoconf	autoconf --version
automake	automake --version
libtool	libtoolize --version
binutils	ld --version
gdb	gdb --version
ccache	ccache --version
bison	bison --version
flex	flex --version
m4	m4 --version
patch	patch --version
gettext	msgfmt --version
clang	clang --version
cppcheck	cppcheck --version
shellcheck	shellcheck --version
doxygen	doxygen --version
python3	python3 --version
python3-pip	pip3 --version
python3-venv	python3 -m venv --help
python3-dev	python3-config --includes
ruby	ruby --version
perl	perl --version
golang-go	go version
rustc	rustc --version
cargo	cargo --version
php-cli	php --version
lua5.4	lua5.4 -v
tcl	sh -c 'echo "puts ok" | tclsh'
sqlite3	sqlite3 --version
git	git --version
subversion	svn --version
mercurial	hg --version
curl	curl --version
wget	wget --version
openssh-client	ssh -V
netcat-openbsd	sh -c 'nc -h 2>&1 | head -1'
socat	socat -V
dnsutils	dig -v
iproute2	ip -V
rsync	rsync --version
gnupg	gpg --version
openssl	openssl version
ca-certificates	sh -c 'test -s /etc/ssl/certs/ca-certificates.crt'
jq	jq --version
ripgrep	rg --version
fd-find	fdfind --version
fzf	fzf --version
tree	tree --version
htop	htop --version
tmux	tmux -V
vim	vim --version
nano	nano --version
neovim	nvim --version
less	less --version
file	file --version
unzip	sh -c 'unzip -v | head -1'
zip	sh -c 'zip -v | head -2 | tail -1'
xz-utils	xz --version
zstd	zstd --version
bzip2	sh -c 'bzip2 --help 2>&1 | head -1'
p7zip-full	sh -c '7z i | head -2 | tail -1'
pandoc	pandoc --version
bc	sh -c 'echo "2+2" | bc'
parallel	sh -c 'parallel --version | head -1'
moreutils	sh -c 'echo x | sponge'
procps	ps --version
psmisc	killall --version
lsof	sh -c 'lsof -v 2>&1 | head -1'
strace	strace -V
sysstat	iostat -V
util-linux	lscpu
man-db	man --version
locales	locale --version
coreutils	sha256sum --version
findutils	find --version
grep	grep --version
sed	sed --version
gawk	gawk --version
diffutils	diff --version
time	sh -c '/usr/bin/time true'
postgresql-client	psql --version
mariadb-client	mariadb --version
redis-tools	redis-cli --version
imagemagick	sh -c 'convert --version | head -1'
ffmpeg	sh -c 'ffmpeg -version | head -1'
graphviz	dot -V
libxml2-utils	xmllint --version
libjson-perl	perl -MJSON -e 'print "ok"'
python3-requests	python3 -c 'import requests'
python3-yaml	python3 -c 'import yaml'
build-essential	sh -c 'echo "int main(void){return 0;}" > /tmp/bt.c && gcc -o /tmp/bt /tmp/bt.c && /tmp/bt'
LIST
)

total=$(printf '%s\n' "$PKGS" | grep -c .)
echo "── 호환성 폭 측정: $total 패키지, rootfs=$R ──"
printf 'pkg\tinstall\trun\tdetail\n' > "$OUT"

# Termux has no host /tmp -- $PREFIX/tmp is the real one. Redirecting to
# /tmp makes every apt_install fail on the REDIRECT, before apt even runs,
# which reads downstream as "all 96 packages unavailable".
APTLOG=${TMPDIR:-$PREFIX/tmp}/alr-breadth-apt.log
: > "$APTLOG" || { echo "cannot write $APTLOG"; exit 2; }

apt_install() { # apt_install <pkg...>  -> rc
    env ALR_FAKEROOT=1 DEBIAN_FRONTEND=noninteractive \
        "$ALR" run /usr/bin/apt-get install -y --no-install-recommends "$@" \
        >>"$APTLOG" 2>&1
}

env ALR_FAKEROOT=1 "$ALR" run /usr/bin/apt-get update >/dev/null 2>&1

# ── install, in batches for speed ────────────────────────────────────────
names=$(printf '%s\n' "$PKGS" | cut -f1)
echo "$names" | xargs -n "$BATCH" | while read -r batch; do
    if apt_install $batch; then
        echo "  batch ok: $batch"
    else
        # Attribute individually: one unavailable package must not condemn
        # the rest of its batch.
        echo "  batch FAILED, 개별 재시도: $batch"
        for p in $batch; do apt_install "$p" >/dev/null 2>&1 || true; done
    fi
done

# ── attribute + smoke-run, one package at a time ─────────────────────────
ok_i=0; ok_r=0; bad_i=0; bad_r=0
printf '%s\n' "$PKGS" | while IFS=$'\t' read -r pkg cmd; do
    [ -n "$pkg" ] || continue
    st=$("$ALR" run /usr/bin/dpkg-query -W -f='${db:Status-Status}' "$pkg" 2>/dev/null)
    if [ "$st" != "installed" ]; then
        printf '%s\tFAIL\tSKIP\tnot-installed(%s)\n' "$pkg" "${st:-absent}" >> "$OUT"
        continue
    fi
    # `set -o pipefail` INSIDE the guest shell.  Nine of these commands are
    # pipelines ending in head/tail, and without it the exit status is head's,
    # which is 0 no matter how the left-hand side died -- so a binary that
    # printed "CANNOT LINK EXECUTABLE" and exited 127 scored PASS.  The point
    # of this sweep is which packages RUN; a scoring rule that cannot see a
    # failed exec is not measuring that.
    # bash, not sh: Ubuntu's /bin/sh is dash, which has no `set -o pipefail`
    # and, being a special builtin failing in a non-interactive shell, EXITS --
    # so the first attempt at this scored the whole sweep 0/96 with rc=2.
    #
    # pipefail matters because nine of these commands are pipelines ending in
    # head/tail: without it the exit status is head's, which is 0 no matter how
    # the left-hand side died, so a binary printing "CANNOT LINK EXECUTABLE"
    # and exiting 127 scored PASS.  The point of this sweep is which packages
    # RUN, and a rule that cannot see a failed exec does not measure that.
    out=$(env ALR_FAKEROOT=1 "$ALR" run /bin/bash -o pipefail -c "$cmd" 2>&1); rc=$?
    if [ $rc -eq 0 ]; then
        printf '%s\tPASS\tPASS\t%s\n' "$pkg" "$(printf '%s' "$out" | head -1 | cut -c1-60)" >> "$OUT"
    else
        printf '%s\tPASS\tFAIL\trc=%d %s\n' "$pkg" "$rc" \
            "$(printf '%s' "$out" | tail -1 | cut -c1-90)" >> "$OUT"
    fi
done

inst=$(awk -F'\t' 'NR>1 && $2=="PASS"' "$OUT" | wc -l | tr -d ' ')
runs=$(awk -F'\t' 'NR>1 && $3=="PASS"' "$OUT" | wc -l | tr -d ' ')
echo
echo "─────────────────────────────────────────────────────────────"
echo "  설치 성공   $inst / $total"
echo "  실행 성공   $runs / $total"
echo
echo "── 실패 상세 ──"
awk -F'\t' 'NR>1 && ($2!="PASS" || $3!="PASS") { printf "  %-22s %-6s %-6s %s\n", $1, $2, $3, $4 }' "$OUT"
echo
echo "ALR BREADTH: install=$inst/$total run=$runs/$total"
