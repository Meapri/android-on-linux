#!/usr/bin/env bash
# The one thing that must never appear in this source tree.
#
# ADR 0001 chose a SIGNAL-ONLY ptrace supervisor: it resumes with PTRACE_CONT
# and never puts a tracee into syscall-stop mode.  That is the entire
# difference from PRoot -- docs/07-acceptance.md §3 says it plainly: if
# syscall_stops becomes non-zero "이 제품은 PRoot다" and every performance claim
# in the repo is void.
#
# There is now a RUNTIME counter for it (2026-08-03; before that the field was
# declared, printed, and gated on, and written by nothing at all -- it was
# structurally zero, so the gate that existed to make PTRACE_SYSCALL impossible
# to merge quietly could not see it).  But that counter needs a device, and CI
# has none.  This is the half CI can enforce, at commit time.
#
# Both halves are demonstrated to fire: replacing the pass-through PTRACE_CONT
# with PTRACE_SYSCALL gives syscall_stops=190 path_traps=16 and turns the
# supervisor self-tests from 12/0 into 7/5, and this gate goes red.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

python3 - <<'PY'
import os, re, sys

# Comments and string literals are allowed to name it -- the whole file is
# about it.  Only a USE counts.  Strip both rather than trying to out-clever
# them with a regex: an earlier version used negative look-alikes and failed on
# its own tree, matching seven prose mentions and one slog() format string.
def strip_c(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i+1] == '*':
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c == '/' and i + 1 < n and src[i+1] == '/':
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c == '"':
            i += 1
            while i < n and src[i] != '"':
                i += 2 if src[i] == '\\' else 1
            i += 1
        else:
            out.append(c); i += 1
    return "".join(out)

roots, scanned, hits = ["src"], 0, []
for root in roots:
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if not fn.endswith((".c", ".h")):
                continue
            p = os.path.join(dp, fn)
            body = open(p, encoding="utf-8", errors="replace").read()
            if "ptrace" in body:
                scanned += 1
            code = strip_c(body)
            for m in re.finditer(r"\bPTRACE_SYSCALL\b", code):
                hits.append(p)

print("── invariant gate ──────────────────────────────────────────")
if scanned == 0:
    # Positive control: if nothing under src/ mentions ptrace, this gate is not
    # searching the tree it believes it is, and a clean result means nothing.
    print("  FAIL  no file under src/ mentions ptrace; the scan is misdirected")
    print("ALR INVARIANT GATE: FAIL"); sys.exit(1)

if hits:
    print("  PTRACE_SYSCALL is USED (not merely mentioned) in:")
    for p in sorted(set(hits)):
        print("    " + p)
    print()
    print("  ADR 0001 forbids it.  A tracee in syscall-stop mode makes this")
    print("  product PRoot: every path syscall pays a ptrace round trip and")
    print("  every measured multiple in this repo becomes void.")
    print("────────────────────────────────────────────────────────────")
    print("ALR INVARIANT GATE: FAIL"); sys.exit(1)

print("  ptrace-using files scanned: %d   PTRACE_SYSCALL uses: 0" % scanned)
print("────────────────────────────────────────────────────────────")
print("ALR INVARIANT GATE: PASS")
PY
