#!/usr/bin/env bash
# Two implementations of the guest path rule live in this repo. They must agree.
#
#   runtime/alr/src/common/alr_path_rule.h   the product path, 73 assertions
#   app/src/main/cpp/alr_runtime/alr_path.cpp  the in-process research loader
#
# Duplication is tolerable. DIVERGENCE is not: a path the two disagree about
# resolves to two different files, and nothing reports it.
#
# When this gate was first written, 7 of 14 shared cases disagreed:
#   /proc/self/exe, /sys/kernel, /dev/null  the loader prefixed sysdirs INTO the
#       rootfs, so it would read /proc out of a directory with no kernel behind it
#   <R>/etc/os-release                      already-host paths were prefixed a
#       SECOND time -> <R><R>/etc/os-release
# The loader now delegates to alr_path_rule.h for those, and this keeps it there.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

echo "── path rule divergence gate ───────────────────────────────"
CXX=${CXX:-clang++}
out=$(mktemp -t alr-divergence)
if ! "$CXX" -std=c++20 -I app/src/main/cpp -I runtime/alr/src/common \
        -o "$out" app/src/test/native/path_rule_divergence.cpp \
        app/src/main/cpp/alr_runtime/alr_path.cpp 2>&1; then
    echo "  FAIL  the differential test did not compile"
    echo "ALR PATH RULE DIVERGENCE: FAIL"; rm -f "$out"; exit 1
fi
"$out"; rc=$?
rm -f "$out"
echo "────────────────────────────────────────────────────────────"
exit $rc
