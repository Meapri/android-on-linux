#!/usr/bin/env bash
# Verify every relative markdown link in the repo resolves to a real file.
#
# WHY: this repo carries its argument in cross-references -- a claim in the
# README points at the evidence file that measured it, and an ADR points at the
# probe that refuted it.  A broken link does not look broken; it looks like a
# citation.  Three were sitting in the tree when this check was first written,
# two of them minutes old, and nothing had noticed.
#
# Anchors (#section) are stripped, not checked: the headings are Korean and
# GitHub's slug rules for them are not worth reimplementing here.  Existence of
# the target FILE is what this gate is for.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

python3 - "$@" <<'PY'
import os, re, sys

bad, checked = [], 0
for dirpath, dirnames, filenames in os.walk("."):
    dirnames[:] = [d for d in dirnames if d not in (".git", "build", "node_modules")]
    for fn in filenames:
        if not fn.endswith(".md"):
            continue
        path = os.path.join(dirpath, fn)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for m in re.finditer(r"\]\(([^)\s]+?)\)", text):
            target = m.group(1)
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target = target.split("#", 1)[0]
            if not target:
                continue
            checked += 1
            if not os.path.exists(os.path.normpath(os.path.join(dirpath, target))):
                line = text.count("\n", 0, m.start()) + 1
                bad.append((path, line, target))

print("── doc link gate ────────────────────────────────────────────")
print("  relative links checked: %d" % checked)
if not checked:
    # No links at all means the regex stopped matching, not that the docs are
    # clean.  Fail rather than report a green gate that measured nothing.
    print("  FAIL  found no relative links at all; the extractor is broken")
    print("ALR DOC LINKS: FAIL")
    sys.exit(1)
for path, line, target in bad:
    print("  BROKEN  %s:%d -> %s" % (path, line, target))
print("────────────────────────────────────────────────────────────")
print("ALR DOC LINKS: %s" % ("FAIL (%d broken)" % len(bad) if bad else "PASS"))
sys.exit(1 if bad else 0)
PY
