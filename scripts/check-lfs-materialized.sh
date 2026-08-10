#!/usr/bin/env bash
# Every Git LFS file must be the real payload, not a pointer.
#
# WHY THIS EXISTS. app/src/main/assets/rootfs/payloads/tiny-rootfs.tar was
# committed through LFS, and on a checkout without `git lfs pull` it is 134
# bytes of text:
#
#     version https://git-lfs.github.com/spec/v1
#     oid sha256:7adc9444...
#     size 252743680
#
# That still builds. It still installs. RootfsInstaller then rejects it on the
# size check and silently skips extraction, so the app comes up with NO ROOTFS
# and every downstream probe reports FAIL for a reason that has nothing to do
# with what it is testing. A whole device-evidence report
# (docs/evidence/v98-targetsdk28-app-report.txt) was captured in that state and
# had to be discarded -- 1,592 lines of results about a rootfs that was never
# extracted.
#
# The failure is silent at every step that could have caught it, which is
# exactly what a gate is for. Run before packaging.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

echo "── git-lfs materialization gate ────────────────────────────"

if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "  SKIP  not a git checkout"
    echo "ALR LFS MATERIALIZED: SKIP"; exit 0
fi

# Check the BYTES, not the filename: "is this a pointer" is the property that
# actually matters, and a .gitattributes pattern that stops matching is itself
# a way to lose coverage.
#
# No `mapfile` here -- macOS ships bash 3.2, where it does not exist, and the
# first version of this gate died with "mapfile: command not found" and then
# "unbound variable" while still printing its failure banner. A gate that
# crashes into its own error path is indistinguishable from one that found
# something.
ptr_list=$(git ls-files 2>/dev/null | while IFS= read -r f; do
    [ -f "$f" ] || continue
    sz=$(wc -c < "$f" 2>/dev/null | tr -d ' ')
    [ -n "$sz" ] && [ "$sz" -le 1024 ] || continue
    if head -c 45 "$f" 2>/dev/null | grep -q "^version https://git-lfs"; then
        printf '%s\t%s\n' "$sz" "$f"
    fi
done)

if [ -z "$ptr_list" ]; then
    echo "  every LFS-tracked file is materialized"
    echo "────────────────────────────────────────────────────────────"
    echo "ALR LFS MATERIALIZED: PASS"; exit 0
fi

echo "  THESE ARE POINTERS, NOT PAYLOADS:"
printf '%s\n' "$ptr_list" | while IFS=$'\t' read -r sz f; do
    printf '    %8sB  %s\n' "$sz" "$f"
done
echo
echo "  Run:  git lfs install && git lfs pull"
echo "  Packaging these ships an app whose rootfs never extracts, and every"
echo "  probe downstream then fails for the wrong reason."
echo "────────────────────────────────────────────────────────────"
echo "ALR LFS MATERIALIZED: FAIL"
exit 1
