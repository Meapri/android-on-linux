#!/usr/bin/env bash
# Every `reason=` code the code emits must be in the spec, and every code the
# spec lists must be emitted.  Both directions.
#
# WHY BOTH: goal G6 ("failures are not silent -- every failure carries a stable
# reason= code") is measured by that vocabulary, and it was measured by nobody.
# At the time this was written the code emitted 22 codes, docs/05 §7 listed 17,
# and the OVERLAP WAS FIVE.  A vocabulary neither side honours cannot make G6
# true or false, so G6 was not a goal, it was a sentence.
#
# One-directional would not have caught it either way round:
#   spec -> code only : the 17 documented codes stay unreachable forever
#   code -> spec only : new codes accumulate undocumented
#
# Run:  scripts/check-reasons.sh      (also part of `make check`)
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

SPEC=docs/05-provisioning-spec.md

# Emitted: three shapes exist and all count.
#   die("code", detail)               -- prints "reason=code" from the helper
#   die_staged(part, "code", detail)  -- same, after removing <R>.part
#   a literal reason=code inside a printf/fprintf string
#
# The die_staged form was added on 2026-08-04 and this gate immediately
# reported four codes as "in the spec but never emitted" -- which was the gate
# working: a wrapper it did not know about is indistinguishable from a deleted
# call site.  Matching the argument BEFORE the string keeps that property: a
# fourth wrapper will fail here too rather than silently pass.
emitted=$( { grep -rhoE 'die\("[a-z0-9-]+"' src/ | sed 's/die("//; s/"//'
             grep -rhoE 'die_staged\([A-Za-z_]+, *"[a-z0-9-]+"' src/ \
                 | sed 's/.*, *"//; s/"//'
             grep -rhoE 'reason=[a-z0-9-]+'   src/ | sed 's/reason=//'
           } | sort -u )

# Documented: the fenced block in the spec's failure-classification section.
documented=$(awk '/^## 7\. 실패 분류/{s=1} s&&/^```$/{f++; next} s&&f==1{print} f==2{exit}' \
             "$SPEC" | tr -s ' \t' '\n' | grep -E '^[a-z0-9-]+$' | sort -u)

echo "── reason= registry gate ────────────────────────────────────"

# An empty side is an extractor failure, not a clean tree.  Same rule as
# scripts/check-docs.sh: a gate that reports green on nothing is worse than no
# gate, because it looks like evidence.
if [ -z "$emitted" ]; then
    echo "  FAIL  found no emitted codes at all; the extractor is broken"
    echo "ALR REASON REGISTRY: FAIL"; exit 1
fi
if [ -z "$documented" ]; then
    echo "  FAIL  found no documented codes in $SPEC §7; the extractor is broken"
    echo "ALR REASON REGISTRY: FAIL"; exit 1
fi

undocumented=$(comm -23 <(printf '%s\n' "$emitted") <(printf '%s\n' "$documented"))
unreachable=$(comm -13 <(printf '%s\n' "$emitted") <(printf '%s\n' "$documented"))

printf '  emitted: %s   documented: %s\n' \
    "$(printf '%s\n' "$emitted" | wc -l | tr -d ' ')" \
    "$(printf '%s\n' "$documented" | wc -l | tr -d ' ')"

rc=0
if [ -n "$undocumented" ]; then
    rc=1
    echo "  EMITTED BUT NOT IN THE SPEC — add them to $SPEC §7:"
    printf '%s\n' "$undocumented" | sed 's/^/    /'
fi
if [ -n "$unreachable" ]; then
    rc=1
    echo "  IN THE SPEC BUT NEVER EMITTED — implement or remove:"
    printf '%s\n' "$unreachable" | sed 's/^/    /'
    echo "    (a code no site can produce is a promise, not a contract)"
fi

echo "────────────────────────────────────────────────────────────"
[ $rc -eq 0 ] && echo "ALR REASON REGISTRY: PASS" || echo "ALR REASON REGISTRY: FAIL"
exit $rc
