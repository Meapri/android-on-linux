#!/usr/bin/env bash
# A line that reports PASS must be able to report FAIL.
#
# WHY THIS EXISTS. The device report is this project's evidence, and several of
# its verdicts were string literals:
#
#   alr_runtime_launcher.cpp   return "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS";
#                              ...two functions above a can_execute_guest() that
#                              returned 0. The two disagreed and neither had
#                              asked the kernel anything.
#   alr_hook.cpp               report << "ALR HOOK LOAD: PASS";
#                              printed directly below code that had already
#                              computed whether the open and stat succeeded.
#   alr_interposer.cpp         same shape, twice.
#
# None of them could ever say FAIL, so none of them was a verdict -- they were
# labels wearing one, and a reader of the report cannot tell the difference.
# The cost was real: docs/evidence/v98 asserted app-data exec policy while the
# actual kernel answer was unmeasured, and the W^X model shipped a release
# contradicting the device.
#
# A verdict emitted as a literal is the failure this catches. Emitting it from
# an expression -- a ternary, pass_fail(), a variable -- is fine, because then
# something decides.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

echo "── unconditional verdict gate ──────────────────────────────"

hits=$(python3 - <<'PY'
import re, io, os, sys

roots = ["app/src/main/cpp"]
bad = []
for root in roots:
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            if not name.endswith((".cpp", ".cc", ".hpp", ".h")):
                continue
            path = os.path.join(dirpath, name)
            for i, line in enumerate(io.open(path, encoding="utf-8", errors="replace"), 1):
                # Comments describe what the code emits; they are not
                # emissions. Two of this gate's first three "findings" were
                # header comments explaining a probe's output contract.
                s = line.lstrip()
                if s.startswith("//") or s.startswith("*") or s.startswith("/*") or s.startswith("#"):
                    continue
                # A quoted verdict...
                m = re.search(r'"([A-Z][A-Z0-9 ^/&+.:_-]{3,60}): (PASS|FAIL)[^"]*"', line)
                if not m:
                    continue
                # ...that is NOT produced by something that decides.
                if any(tok in line for tok in ("?", "pass_fail", "verdict", "ok ?", "<< (")):
                    continue
                # Only PASS is the dangerous direction. An unconditional FAIL
                # sits in an error branch -- reaching it IS the decision, and
                # flagging it makes the gate cry wolf on honest code.
                if m.group(2) == "FAIL":
                    continue
                # A literal being SEARCHED FOR is not a literal being emitted.
                # alr_trampoline.cpp looks for its own marker in a child's
                # stdout, which is a real execution check, and an earlier
                # version of this gate called it a fake.
                if any(tok in line for tok in (".find(", "contains(", "strstr", "==", "!=")):
                    continue
                bad.append((path, i, m.group(1), m.group(2)))
for p, i, n, v in bad:
    print(f"{p}:{i}\t{n}: {v}")
sys.exit(0)
PY
)

if [ -z "$hits" ]; then
    echo "  no unconditional PASS/FAIL literals in native report code"
    echo "────────────────────────────────────────────────────────────"
    echo "ALR UNCONDITIONAL VERDICTS: PASS"; exit 0
fi

echo "  THESE CANNOT FAIL, YET THEY REPORT A VERDICT:"
printf '%s\n' "$hits" | while IFS=$'\t' read -r loc rest; do
    printf '    %-56s %s\n' "$loc" "$rest"
done
echo
echo "  Emit the verdict from something that decides -- a ternary, pass_fail(),"
echo "  a bool computed above -- or make the line a plain label without a"
echo "  PASS/FAIL token so nobody reads it as evidence."
echo "────────────────────────────────────────────────────────────"
echo "ALR UNCONDITIONAL VERDICTS: FAIL"
exit 1
