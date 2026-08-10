#!/usr/bin/env bash
# Diff two `alr doctor` syscall sweeps.
#
# The question this answers is the one in docs/01-platform-facts.md §A6: is the
# zygote seccomp blocked set a CONSTANT we may bake in, or a per-device artifact
# that must be regenerated on every phone?
#
# A matching COUNT is not an answer.  Two devices can block 239 syscalls each
# and not block the same 239.  This compares the sets.
#
#   scripts/diff-sweep.sh docs/evidence/sweeps/a.txt docs/evidence/sweeps/b.txt
#
# Exit 0 = identical sets, 1 = they differ, 2 = could not compare.
set -uo pipefail

[ $# -eq 2 ] || { echo "usage: $0 <sweep-a> <sweep-b>" >&2; exit 2; }

for f in "$1" "$2"; do
    [ -r "$f" ] || { echo "diff-sweep: cannot read $f" >&2; exit 2; }
done

strip() { grep -vE '^[[:space:]]*(#|$)' "$1" | tr -dc '0-9\n' | grep -v '^$' | sort -nu; }

a=$(strip "$1"); b=$(strip "$2")
[ -n "$a" ] && [ -n "$b" ] || { echo "diff-sweep: one side is empty; refusing to call that a match" >&2; exit 2; }

na=$(printf '%s\n' "$a" | wc -l | tr -d ' ')
nb=$(printf '%s\n' "$b" | wc -l | tr -d ' ')

only_a=$(comm -23 <(printf '%s\n' "$a") <(printf '%s\n' "$b"))
only_b=$(comm -13 <(printf '%s\n' "$a") <(printf '%s\n' "$b"))
both=$(comm -12 <(printf '%s\n' "$a") <(printf '%s\n' "$b") | wc -l | tr -d ' ')

echo "── syscall sweep diff ──────────────────────────────────────"
printf '  A  %-52s %s blocked\n' "$(basename "$1")" "$na"
printf '  B  %-52s %s blocked\n' "$(basename "$2")" "$nb"
printf '  shared: %s\n\n' "$both"

rc=0
if [ -n "$only_a" ]; then
    rc=1
    echo "  blocked on A only:"; printf '%s\n' "$only_a" | sed 's/^/    /'
fi
if [ -n "$only_b" ]; then
    rc=1
    echo "  blocked on B only:"; printf '%s\n' "$only_b" | sed 's/^/    /'
fi

echo "────────────────────────────────────────────────────────────"
if [ "$rc" = 0 ]; then
    echo "ALR SWEEP DIFF: IDENTICAL ($both syscalls, both devices)"
else
    echo "ALR SWEEP DIFF: DIFFERENT — the blocked set is per-device."
    echo "  alr_sigsys_table.h cannot be treated as a constant; docs/01 §A6."
fi
exit $rc
