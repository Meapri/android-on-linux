#!/usr/bin/env bash
# A status token in docs/07-acceptance.md §2 must have a runner behind it.
#
# §1 states that rule -- "테스트가 없는 이름에는 상태 토큰을 붙이지 않는다" -- and
# cites two occasions the project had already broken it (M13, M12 §7).  It was
# broken a third time, at scale: of 64 tokenized names, 35 had no emitter
# anywhere and 32 of those carried PASS.  One of them was
# `ALR BASH INTERACTIVE: PASS` sitting a page above the sentence "지금까지의
# 디바이스 세션이 전부 비대화형이었고… 대화형 경로를 밟은 적이 없다".
#
# A rule stated in prose that nothing enforces gets broken.  This enforces it.
#
# The fix for an orphan is NOT to delete the line.  Three honest outcomes:
#   1. write the runner (best -- doing this found a silent exec bug)
#   2. it IS measured under an aggregate name -> drop the token, point at the
#      runner, e.g. "PRELOAD PATH ABS:   (집계) ALR PATH RULE HOST TESTS"
#   3. it is genuinely untested -> drop the token, keep the name, say why
# Only 2 and 3 are invisible to this gate, and both are honest by construction:
# neither claims a result.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

python3 - <<'PY'
import io, os, re, subprocess, sys

DOC = "docs/07-acceptance.md"
text = io.open(DOC, encoding="utf-8").read()
try:
    sec = text.split("## 2.")[1].split("\n## 3.")[0]
except IndexError:
    print("── acceptance name gate ────────────────────────────────────")
    print("  FAIL  could not find §2 in %s; the extractor is broken" % DOC)
    print("ALR ACCEPTANCE NAMES: FAIL"); sys.exit(1)

TOKEN = r'(PASS|FAIL|SKIP|KNOWN_FAIL[^\s]*|PENDING_DEVICE)'
names = [(m.group(1).strip(), m.group(2))
         for m in re.finditer(r'^([A-Z][A-Z0-9 /_-]{3,60}):\s+' + TOKEN, sec, re.M)]

print("── acceptance name gate ────────────────────────────────────")
if not names:
    # Positive control.  If the section stops matching -- a format change, a
    # renamed heading -- a clean result would mean nothing.  This repo has
    # shipped that exact shape more than once.
    print("  FAIL  found no tokenized names in §2 at all; the extractor is broken")
    print("ALR ACCEPTANCE NAMES: FAIL"); sys.exit(1)

roots = ["tests", "scripts", "src", "bench"]
orphans = []
for n, tok in names:
    hit = subprocess.run(["grep", "-rqF", n] + roots, capture_output=True).returncode == 0
    if not hit:
        orphans.append((n, tok))

print("  tokenized names: %d   with a runner: %d"
      % (len(names), len(names) - len(orphans)))
if orphans:
    print("  NO RUNNER EMITS THESE, yet they carry a status:")
    for n, tok in orphans:
        print("    %-16s %s" % (tok, n))
    print()
    print("  Write the runner, or drop the token and say where it IS measured")
    print("  (\"(집계) <runner>\") or that it is not (\"미측정 — <why>\").")
    print("────────────────────────────────────────────────────────────")
    print("ALR ACCEPTANCE NAMES: FAIL"); sys.exit(1)

print("────────────────────────────────────────────────────────────")
print("ALR ACCEPTANCE NAMES: PASS")
PY
