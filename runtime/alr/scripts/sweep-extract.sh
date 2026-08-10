#!/usr/bin/env bash
# Pull the raw blocked-syscall set out of `alr doctor` output.
#
# WHY THIS EXISTS: src/supervisor/alr_sigsys_table.h was generated from a real
# device sweep, but only the ~30 entries whose emulated return differs from the
# default were kept.  The other ~209 blocked numbers were discarded.  When a
# second device arrived there was nothing to diff against -- the question
# "is the blocked set device-independent?" was unanswerable from the repo alone,
# and re-answering it needed the first device back in hand.
#
# So every sweep now gets checked in verbatim under docs/evidence/sweeps/.
# The curated table stays what it is: a table of EXCEPTIONS.  This is the
# ground truth it was curated from.
#
#   ./scripts/dev-push.sh doctor | scripts/sweep-extract.sh > docs/evidence/sweeps/<device>.txt
set -euo pipefail

# `|| true`: with set -euo pipefail a no-match grep would kill the script here,
# exiting before the diagnostic below ever prints.  The check wants to own the
# empty case, so the pipeline must be allowed to come back empty.
out=$(grep -oE '^[[:space:]]*\{[[:space:]]*[0-9]+,' "${1:--}" 2>/dev/null \
      | tr -dc '0-9\n' | grep -v '^$' | sort -n || true)

# An empty extract means the input was not doctor output -- do not emit a file
# that a later diff would read as "this device blocks nothing".
if [ -z "$out" ]; then
    echo "sweep-extract: no '{ N, ... }' table rows found in the input." >&2
    echo "  Expected the C table that 'alr doctor' prints for its P2 sweep." >&2
    exit 2
fi

printf '%s\n' "$out"
