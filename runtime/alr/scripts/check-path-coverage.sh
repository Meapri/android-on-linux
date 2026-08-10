#!/usr/bin/env bash
# Every libc entry point that takes a path must be interposed, or named here as
# deliberately not.
#
# WHY THIS EXISTS.  bind() and connect() were never interposed, for the entire
# life of the project, so every AF_UNIX address in the guest resolved against
# Android.  Nothing pointed at them.  There was no gate that could have, because
# the coverage argument was "we wrapped the ones we thought of" -- which is not
# an argument, it is a list of what somebody happened to remember.  The bug
# surfaced only because writing an unrelated test (docs/07 §2 had `ALR PTY
# TMUX: PASS` with no runner) put a real server in front of it.
#
# Running this the first time found, in one pass:
#   getxattr/listxattr/setxattr/removexattr and the l* variants  (8)
#   inotify_add_watch, pathconf, glob/glob64, ftw/ftw64, nftw/nftw64
#   getsockname/getpeername/accept4  (the OUT side of the bind fix)
# tests/device/probe_pathcov.c then measured all of them wrong on device, and
# right afterwards.
#
# TWO KINDS OF ENTRY, and both have to be here:
#   want    must appear in the .so's dynamic symbol table
#   skip    must NOT be interposed, WITH A REASON.  These are load-bearing:
#           `glob` results must come back in guest form but `basename` must not
#           be touched at all, and a future "let's be thorough" pass needs to
#           find the reasoning rather than rediscover it by breaking something.
#           The list is short on purpose: the FIRST run of this gate rejected
#           five entries the author had reasoned into `skip` (setmntent,
#           pivot_root, name_to_handle_at, tmpnam, tempnam) that the code had
#           already interposed for stated reasons.  Reasoning about which calls
#           matter is exactly what this replaces.
#
# This gate proves a symbol is EXPORTED.  It cannot prove the rewrite is
# correct -- only tests/device/probe_pathcov.c on a real device does that.  The
# two are a pair; neither is sufficient.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

SO=${1:-build/libalr_preload.so}
OBJDUMP=${OBJDUMP:-objdump}

echo "── path coverage gate ──────────────────────────────────────"

if [ ! -f "$SO" ]; then
    echo "  FAIL  $SO not built; run scripts/build-preload.sh"
    echo "ALR PATH COVERAGE: FAIL"; exit 1
fi

# Defined (not UND) global functions.
have=$("$OBJDUMP" -T "$SO" 2>/dev/null | awk '/ g *DF /{print $NF}' | sort -u)

if [ -z "$have" ]; then
    # Positive control.  A .so this gate cannot read must not report a clean
    # sweep -- that failure mode has shipped in this repo before.
    echo "  FAIL  read no defined symbols from $SO; the extractor is broken"
    echo "ALR PATH COVERAGE: FAIL"; exit 1
fi

want="
open open64 openat openat64 creat creat64
stat stat64 lstat lstat64 fstatat fstatat64 __xstat __lxstat __fxstatat statx
access faccessat euidaccess
opendir scandir scandir64
readlink readlinkat realpath canonicalize_file_name
unlink unlinkat rmdir remove
rename renameat renameat2
link linkat symlink symlinkat
mkdir mkdirat mknod mknodat
chmod fchmodat chown lchown fchownat
truncate truncate64
utime utimes utimensat
statfs statfs64 statvfs statvfs64
chdir chroot getcwd
execve execv execvp execvpe execl execlp execle
fopen fopen64 freopen freopen64 popen
getxattr lgetxattr setxattr lsetxattr
listxattr llistxattr removexattr lremovexattr
mkstemp mkstemps mkdtemp mkostemp
dlopen
bind connect accept accept4 getsockname getpeername
inotify_add_watch pathconf
glob glob64 ftw ftw64 nftw nftw64
setmntent pivot_root name_to_handle_at tmpnam tmpnam_r tempnam
mkfifo mkfifoat ttyname_r
"

# name:reason -- the reason is the point of the line.
skip="
basename:operates_on_the_string_never_the_filesystem
dirname:same
fts_open:coreutils_and_find_carry_gnulibs_fts_which_uses_the_PLT_MEASURED_ok
swapon:privileged_and_the_syscall_is_blocked_by_the_zygote_filter
swapoff:same
quotactl:privileged
acct:privileged
"

miss=0 wrong=0 n=0
for f in $want; do
    n=$((n + 1))
    if ! grep -qx "$f" <<<"$have"; then
        printf '  MISSING   %s\n' "$f"
        miss=$((miss + 1))
    fi
done

for e in $skip; do
    f=${e%%:*}
    if grep -qx "$f" <<<"$have"; then
        printf '  UNEXPECTED %s is interposed, but is listed as skip (%s)\n' \
               "$f" "${e#*:}"
        wrong=$((wrong + 1))
    fi
done

printf '  inventory: %d required, %d exempt   exported: %d\n' \
       "$n" "$(wc -w <<<"$skip" | tr -d ' ')" "$(wc -l <<<"$have" | tr -d ' ')"

if [ "$miss" -ne 0 ] || [ "$wrong" -ne 0 ]; then
    echo
    echo "  A path-taking libc call that is not interposed resolves against"
    echo "  ANDROID.  That is silent: the call succeeds against the wrong file,"
    echo "  or fails with a bare ENOENT nobody traces back."
    echo "  Interpose it, or add it to \$skip WITH the reason."
    echo "────────────────────────────────────────────────────────────"
    echo "ALR PATH COVERAGE: FAIL"; exit 1
fi

echo "────────────────────────────────────────────────────────────"
echo "ALR PATH COVERAGE: PASS"
