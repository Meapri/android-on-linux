#!/usr/bin/env python3
"""Regression gate — docs/07-acceptance.md §3.

The invariant that matters most is one line:

    supervisor.syscall_stops == 0

If that ever becomes non-zero somebody introduced PTRACE_SYSCALL, and at that
moment this product IS PRoot -- every performance claim in the repo is void.
The whole gate exists to make that impossible to merge quietly.

WHAT THIS DOES NOT DO, deliberately: it measures nothing.  Runtime invariants
can only come from a real Android app process (uid>=10000, Seccomp==2), and a
hosted CI runner is not one -- adb shell reports Seccomp: 0 and would turn every
gate into a false PASS.  So the device harnesses emit facts and this consumes
them.  Build-side invariants it can check itself, because they are properties of
the artifact rather than of a running system.

    # on the device
    ./scripts/dev-push.sh accept | tee accept.txt
    ./scripts/dev-push.sh bench-ab | tee bench.txt
    ./scripts/dev-push.sh bench    | tee rw.txt

    # anywhere
    bench/regression_gate.py --from accept.txt bench.txt rw.txt
    bench/regression_gate.py --build-only          # CI, no device output

Baselines live in bench/baseline.json.  --update rewrites them; nothing writes
them implicitly, because a gate that moves its own goalposts is not a gate.
"""

import argparse
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO, "bench", "baseline.json")

# ── hard invariants (docs/07 §3) ─────────────────────────────────────────────
HARD = [
    ("supervisor.syscall_stops", "== 0", lambda v: v == 0),
    ("supervisor.path_traps", "== 0", lambda v: v == 0),
    # The budget docs/04-preload-spec.md §13 states for the workload we claim,
    # measured rather than modelled: the call mix comes from the preload's own
    # ALR_COUNT counters and each bucket is priced with its own measured cost
    # (tests/device/rw_bench.sh).
    #
    # This REPLACED "preload.rw_abs_ns <= 100" and "rw_rel_ns <= 20" as hard
    # invariants, and that is not a goalpost move -- those two do not port.
    # Identical code measures ~79 ns/op for the absolute hit on reference
    # device #1 and 106-115 ns on reference #2, so the 100 ns line passes on
    # one supported phone and fails on the other. See M19 §7.  The per-op
    # figures are still enforced, as a per-device REGRESSION check below with
    # 1.25x tolerance, which is tighter than the fixed budgets were.
    ("preload.rw_total_us", "<= 1500", lambda v: v <= 1500),
    ("preload.glibc_verneed_max", '== "2.17"', lambda v: v == "2.17"),
]

# Per-op rewrite costs: absolute thresholds do not port between devices, but an
# order-of-magnitude REGRESSION on one device is still a regression.  Checked
# against this device's own recorded baseline; these FAIL the gate, unlike SOFT
# below which only warns.
#
# THE TOLERANCE IS 2.5x AND THAT IS NOT LAZINESS -- it is the measured noise
# floor of this instrument on a phone.  Three separate fixes were applied to
# bench/microbench/rw_cost.c and the spread survived all of them:
#
#   best-of-7 rounds            abs 76.0-102.7 ns over six runs
#   + 300 ms time-based warm-up abs 57.8-129.3 ns   (bimodal: big.LITTLE)
#   + pin to the fastest core   abs 66.6-128.9 ns   (rel settled at 7.4)
#     the app may use
#
# Android confines an untrusted app to a cpuset -- only 4 of 8 cores accept
# sched_setaffinity on reference #2 -- so we cannot take the machine somewhere
# quiet.  A tolerance under ~2x would flap on an unchanged tree, and a gate
# that cries wolf gets disabled, which is worse than one scoped honestly.
#
# WHAT THIS CAN AND CANNOT CATCH, stated plainly: it catches the failure
# docs/04 §5.1 names -- a cache or converter bolted into the rewrite path, the
# cited upstream one being 4,334 ns/op, i.e. 40x+.  It will NOT catch a 50%
# regression.  preload.rw_total_us is the gate with real resolution on the
# dominant path; this one exists because that gate is nearly blind to the
# absolute-rewrite path, which is only 0.3% of calls (28 of 10,101 measured)
# and could regress 40x while the total stayed inside 1500 us.
PER_DEVICE_HARD = [
    ("preload.rw_abs_ns", 2.5),
    ("preload.rw_rel_ns", 2.5),
    ("preload.rw_sysdir_ns", 2.5),
    ("preload.rw_under_ns", 2.5),
]

# Absolute per-op CEILINGS -- a ratchet-stop, not a performance budget.
#
# These exist because every other per-op check turned out to be SKIPPABLE, and
# the fallback cannot fail.  REPRODUCED against this file's own first version:
# a run reporting rw_abs_ns = 4334.7 ns with PRELOAD RW PINNED 0 printed four
# SKIP lines and "ALR REGRESSION GATE: PASS".  Pinning is an Android cpuset
# property (only 4 of 8 cpus accept sched_setaffinity on reference #2), so a
# backgrounded Termux loses it for reasons that have nothing to do with the
# code -- and with it went the only check with real resolution.
#
# rw_total_us cannot cover for it.  With this repo's own baseline mix
# (28/10072/1/30) the absolute path is 3.4% of the total, so it must regress
# ~548x before 1500 us goes red, and the 4,334.7 ns/op converter docs/04 §5.1
# names as the enemy totals 195 us -- PASS with 7.7x headroom.  rw_total_us is
# a workload-shape detector (an extra syscall per rw() is 3.4 ms, caught 2.3x
# over); it is not a per-op performance gate and must not be asked to be one.
#
# These are NOT the spec's 100/20/40 ns budgets.  Those are properties of the
# phone (~79 ns on ref #1, 106-115 on ref #2) and that is exactly why they were
# removed as hard gates.  These sit ~4x above the worst figure ever recorded on
# either reference device, so no device-to-device spread, thermal state or loss
# of pinning can reach them, while the converter class (40x+) trips them on any
# phone in any state.
#
# They also bound where --update can walk a baseline: without a ceiling,
# repeated drifts just under the 2.5x tolerance ratchet the reference anywhere.
PER_OP_CEILING = [
    ("preload.rw_abs_ns", 500),     # worst ever recorded 129.3
    ("preload.rw_rel_ns", 40),      # worst ever recorded   7.4
    ("preload.rw_sysdir_ns", 150),  # worst ever recorded  32.0
    ("preload.rw_under_ns", 500),   # worst ever recorded 116.7
]

# preload.malloc_calls is specified as a hard invariant and NOTHING measures it.
# It is listed here so the gap is visible in the gate's own output rather than
# only in a doc footnote.  Do not quietly drop it to make the gate green.
UNENFORCED = [
    ("preload.malloc_calls", "== 0",
     "no runner counts allocations on the rewrite path; R1 is held by code "
     "review only, and getaddrinfo is a documented exception"),
]

SOFT = [
    # 1.35, not 1.10.
    #
    # MEASURED 2026-08-04, SM-X236N, EIGHT bench-ab runs of IDENTICAL code in
    # one session: 45, 46, 52, 53, 54, 57, 62 ms -- a 38% spread around a
    # 51 ms baseline.  A 10% band on a metric whose own noise is 38% fires on
    # noise, and a warning that cries wolf is worse than no warning: it teaches
    # the reader to scroll past the one that matters.
    #
    # This is a NOISE-FLOOR correction, not a regression being accepted.  The
    # hard checks (per-op costs against this device's own baseline, the
    # supervisor invariants, the total rewrite budget) are untouched, and the
    # A/B ratio against PRoot -- the number anyone actually quotes -- is
    # computed per-run from both legs measured back to back, so it is immune to
    # the thermal drift that moves this one.  docs/00 §3 already records the
    # same phone giving 330 vs 707 exec/s depending on how warm it was.
    ("node_cold_ms", 1.35),
    ("exec_per_sec", None),      # higher is better; handled separately
]

# Listed in docs/07-acceptance.md §3's soft gates and measured by NOTHING.
# git_status_10k_ms and npm_ci_ms have no PAT entry, so the SOFT loop -- which
# skips absent keys silently, unlike HARD -- could never have compared them.
# §3 told a reader that an npm ci regression past 10% gets warned about; it
# does not.  Same shape as preload.malloc_calls, so it gets the same treatment:
# printed as UNENFORCED every run, where the gap is visible in the gate's own
# output rather than only to whoever reads the source.
UNMEASURED_SOFT = [
    ("git_status_10k_ms", "no harness emits it; the git status A/B is run by "
                          "hand (M19 §6.1), not by tests/device/"),
    ("npm_ci_ms", "no harness emits it; npm ci was measured once by hand "
                  "(M12 §4) and never wired into bench.sh"),
]

DEVICE_PAT = r"ALR BENCH DEVICE:\s*(\S+)"

PAT = {
    "supervisor.syscall_stops": r"syscall_stops=(\d+)",
    "supervisor.path_traps": r"path_traps=(\d+)",
    # rw_cost.c prints "PRELOAD RW ABS COST   61.0 ns  <=  100  PASS" -- aligned
    # columns, no colon.  An earlier pattern assumed the colon form used in
    # docs/07 §2 and matched nothing, which the gate correctly reported as
    # ABSENT rather than passing on facts it never saw.
    "preload.rw_abs_ns": r"PRELOAD RW ABS COST\s+([\d.]+)\s*ns",
    "preload.rw_rel_ns": r"PRELOAD RW REL COST\s+([\d.]+)\s*ns",
    "preload.rw_sysdir_ns": r"PRELOAD RW SYSDIR COST\s+([\d.]+)\s*ns",
    "preload.rw_under_ns": r"PRELOAD RW UNDER COST\s+([\d.]+)\s*ns",
    # us, not ns -- and anchored to its own line for the reason above.
    "preload.rw_total_us": r"PRELOAD RW TOTAL COST\s+([\d.]+)\s*us",
    "preload.rw_pinned": r"PRELOAD RW PINNED\s+([01])\b",
    "preload.rw_calls": r"PRELOAD RW CALLS\s+(\d+)",
    # `[^\n]*?`, NOT `.*?` under re.S.  The dot-all version was allowed to run
    # off the end of the NODE COLD line and keep looking; when the harness
    # started printing a spread ("alr 56 [51-60] / proot ..."), the anchor no
    # longer matched on its own line, the search crossed into EXEC THROUGHPUT,
    # and node_cold_ms silently picked up the exec figure -- 683 "ms" -- which
    # --update then wrote into the baseline as fact.  A scraper must not be
    # able to answer a question using a different line's number.
    "node_cold_ms": r"ALR BENCH NODE COLD vs PROOT:[^\n]*?alr\s+(\d+)\s",
    "exec_per_sec": r"ALR BENCH EXEC THROUGHPUT:\s*(\d+)\s*exec/s",
}


def scrape(paths):
    """Pull facts out of harness output.  Absent is absent -- never defaulted."""
    blob = ""
    for p in paths:
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                blob += f.read() + "\n"
        except OSError as e:
            die("cannot read %s: %s" % (p, e))
    out = {}
    dev = sorted(set(re.findall(DEVICE_PAT, blob)))
    if len(dev) > 1:
        die("the given output mixes %d devices: %s. Per-device baselines are "
            "meaningless across phones; pass one session's files."
            % (len(dev), ", ".join(dev)))
    if dev:
        out["_device"] = dev[0]
    for key, pat in PAT.items():
        hits = re.findall(pat, blob, re.S)
        if not hits:
            continue
        # syscall_stops/path_traps appear once per run; the gate must see the
        # WORST value, not the last one printed.
        vals = [float(h) for h in hits]
        # Budget-shaped keys (anything compared with <=) must report the WORST
        # value in the given output, never the last one printed.  Otherwise
        # concatenating two runs -- or a cold and a warm one -- lets the gate
        # answer with whichever happened to print last.  This already applied
        # to supervisor.*; it applies for the same reason to every cost.
        worst_max = (key.startswith("supervisor.")
                     or key.startswith("preload.rw_"))
        out[key] = max(vals) if worst_max else vals[-1]
        if key.startswith("supervisor.") or key == "exec_per_sec" \
                or key in ("preload.rw_pinned", "preload.rw_calls"):
            out[key] = int(out[key])
    return out


def build_facts():
    """Artifact properties, checkable without a device."""
    so = os.path.join(REPO, "build", "libalr_preload.so")
    if not os.path.exists(so):
        return {}, ["build/libalr_preload.so absent; run scripts/build-preload.sh"]
    objdump = os.environ.get("OBJDUMP", "objdump")
    try:
        p = subprocess.run([objdump, "-p", so], capture_output=True, text=True,
                           check=False)
    except FileNotFoundError:
        return {}, ["%s not found; cannot read the ELF" % objdump]
    if p.returncode != 0:
        return {}, ["%s -p failed on the .so" % objdump]
    vers = sorted(set(re.findall(r"GLIBC_(\d+\.\d+)", p.stdout)),
                  key=lambda v: tuple(int(x) for x in v.split(".")))
    if not vers:
        return {}, ["no GLIBC_ version references found; is this the right file?"]
    return {"preload.glibc_verneed_max": vers[-1]}, []


def die(msg):
    print("regression_gate: %s" % msg, file=sys.stderr)
    sys.exit(2)


# A canned harness transcript with known answers.  This exists because the
# scraper once matched the EXEC THROUGHPUT number for node_cold_ms and --update
# wrote it to the baseline; nothing caught it, because a regex that matches the
# wrong thing looks exactly like a regex that works.  The sample deliberately
# includes the spread brackets and puts a decoy number on every line.
SELFTEST_SAMPLE = """\
── alr bench (A/B) ──────────────────────────────────────────
ALR BENCH DEVICE: TEST-1/plat/android16/6.6
  reps=5  guest=/x/ubuntu-24.04

ALR BENCH NODE COLD vs PROOT:      5.25x  MEASURED  alr 56 [51-60] / proot 294 [290-301] ms  (identical node binary)
ALR BENCH EXEC THROUGHPUT:         683 exec/s  MEASURED  alr 683 / proot 219 exec/s  (alr 293 ms [290-297] over 200 execs)
ALR MEDIATION INVARIANT:           PASS  path_traps=0 syscall_stops=0  MEASURED
  PRELOAD RW PINNED 1 cpu=4
    PRELOAD RW ABS COST         113.3 ns  (ref <=  100)  RECORDED
    PRELOAD RW REL COST           6.8 ns  (ref <=   20)  RECORDED
    PRELOAD RW SYSDIR COST       24.9 ns  (ref <=   40)  RECORDED
    PRELOAD RW UNDER COST       102.4 ns  (ref <=  100)  RECORDED
    rewritten=28  relative=10072  sysdir=1  underroot=0
    PRELOAD RW TOTAL COST       72.1 us  <= 1500  PASS
"""

SELFTEST_EXPECT = {
    "_device": "TEST-1/plat/android16/6.6",
    "supervisor.syscall_stops": 0,
    "supervisor.path_traps": 0,
    "preload.rw_abs_ns": 113.3,
    "preload.rw_rel_ns": 6.8,
    "preload.rw_sysdir_ns": 24.9,
    "preload.rw_under_ns": 102.4,
    "preload.rw_total_us": 72.1,
    "preload.rw_pinned": 1,
    "node_cold_ms": 56.0,          # NOT 683, NOT 294
    "exec_per_sec": 683,
}


# The refusal forms the harness can emit.  None of them may yield a
# preload.rw_* fact: a gate that scrapes a number out of a failure message
# passes on a measurement that never happened.
SELFTEST_NEGATIVE = """\
── alr rw bench ─────────────────────────────────────────────
ALR BENCH DEVICE: TEST-1/plat/android16/6.6
PRELOAD RW INSTRUMENT: FAIL  the ALR_COUNT instrument emitted nothing
PRELOAD RW INSTRUMENT: FAIL  repo has 0 tracked files, wanted 10000
PRELOAD RW INSTRUMENT: FAIL  buckets sum to 900 but total=1000
  PRELOAD RW TOTAL COST  ABSENT  no call mix supplied;
"""


# Scenario table for the DECISION half of the self-test.
#
# The first version of this self-test only checked that the scraper pulled the
# right numbers out of a transcript.  Three false-greens shipped underneath it,
# because none of them was a scraping bug -- an unpinned run skipped every
# per-op check and passed a 46x regression, --update wrote a failing run into
# the baseline, and an all-zero call mix scored 0.0 us PASS.  A gate's output
# is a VERDICT; the test has to assert the verdict.
#
# (name, transcript, must_fail)
SELFTEST_DECISIONS = [
    ("unpinned run with a 46x absolute regression", """\
ALR BENCH DEVICE: SELFTEST-1/plat/android16/6.6
  PRELOAD RW PINNED 0 cpu=-1
    PRELOAD RW ABS COST        4334.7 ns  (ref <=  100)  RECORDED
    PRELOAD RW REL COST           7.0 ns  (ref <=   20)  RECORDED
    PRELOAD RW SYSDIR COST       32.0 ns  (ref <=   40)  RECORDED
    PRELOAD RW UNDER COST       116.7 ns  (ref <=  100)  RECORDED
    PRELOAD RW TOTAL COST      195.4 us  <= 1500  PASS
    PRELOAD RW CALLS 10101
path_traps=0 syscall_stops=0
""", True),
    ("pinned run, converter on the absolute path", """\
ALR BENCH DEVICE: SELFTEST-1/plat/android16/6.6
  PRELOAD RW PINNED 1 cpu=5
    PRELOAD RW ABS COST        4334.7 ns  (ref <=  100)  RECORDED
    PRELOAD RW REL COST           7.0 ns  (ref <=   20)  RECORDED
    PRELOAD RW SYSDIR COST       32.0 ns  (ref <=   40)  RECORDED
    PRELOAD RW UNDER COST       116.7 ns  (ref <=  100)  RECORDED
    PRELOAD RW TOTAL COST      195.4 us  <= 1500  PASS
    PRELOAD RW CALLS 10101
path_traps=0 syscall_stops=0
""", True),
    ("clean pinned run on an unseen device", """\
ALR BENCH DEVICE: SELFTEST-UNSEEN/plat/android16/6.6
  PRELOAD RW PINNED 1 cpu=5
    PRELOAD RW ABS COST          93.0 ns  (ref <=  100)  RECORDED
    PRELOAD RW REL COST           7.0 ns  (ref <=   20)  RECORDED
    PRELOAD RW SYSDIR COST       32.0 ns  (ref <=   40)  RECORDED
    PRELOAD RW UNDER COST       116.7 ns  (ref <=  100)  RECORDED
    PRELOAD RW TOTAL COST       76.6 us  <= 1500  PASS
    PRELOAD RW CALLS 10101
path_traps=0 syscall_stops=0
""", True),   # unbaselined device must not be silently unfailable
    ("clean pinned run on a known device (must PASS)", """\
ALR BENCH DEVICE: SELFTEST-1/plat/android16/6.6
  PRELOAD RW PINNED 1 cpu=5
    PRELOAD RW ABS COST          95.0 ns  (ref <=  100)  RECORDED
    PRELOAD RW REL COST           7.1 ns  (ref <=   20)  RECORDED
    PRELOAD RW SYSDIR COST       32.4 ns  (ref <=   40)  RECORDED
    PRELOAD RW UNDER COST       118.0 ns  (ref <=  100)  RECORDED
    PRELOAD RW TOTAL COST       77.0 us  <= 1500  PASS
    PRELOAD RW CALLS 10101
path_traps=0 syscall_stops=0
""", False),
    ("supervisor started stopping syscalls", """\
ALR BENCH DEVICE: SELFTEST-1/plat/android16/6.6
  PRELOAD RW PINNED 1 cpu=5
    PRELOAD RW ABS COST          93.0 ns  (ref <=  100)  RECORDED
    PRELOAD RW REL COST           7.0 ns  (ref <=   20)  RECORDED
    PRELOAD RW SYSDIR COST       32.0 ns  (ref <=   40)  RECORDED
    PRELOAD RW UNDER COST       116.7 ns  (ref <=  100)  RECORDED
    PRELOAD RW TOTAL COST       76.6 us  <= 1500  PASS
    PRELOAD RW CALLS 10101
path_traps=0 syscall_stops=9912
""", True),
]


def _run_decision(transcript, baseline, extra_args=None, return_baseline=False):
    """Run the gate's decision logic on a transcript against a fixed baseline."""
    import tempfile
    global BASELINE
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False,
                                     encoding="utf-8") as f:
        f.write(transcript); tpath = f.name
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False,
                                     encoding="utf-8") as f:
        json.dump(baseline, f); bpath = f.name
    # Stub build_facts: the decision test must depend ONLY on the transcript.
    # Without this it passes locally and fails in CI, because CI has no
    # build/libalr_preload.so, preload.glibc_verneed_max comes back ABSENT, and
    # the "must PASS" scenario fails for a reason that has nothing to do with
    # the decision being tested.  A self-test that reads ambient build state is
    # testing the workspace, not the code.
    global build_facts
    saved_bf, build_facts = build_facts, lambda: ({"preload.glibc_verneed_max": "2.17"}, [])
    saved, BASELINE = BASELINE, bpath
    import io as _io, contextlib
    buf = _io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            try:
                rc = main_with(["--from", tpath] + list(extra_args or []))
            except SystemExit as e:
                # die() exits rather than returning, and the --update refusal
                # goes through die().  Without this the first scenario that
                # refuses aborts the whole self-test before it prints a verdict
                # -- which is how it behaved for exactly one run.
                rc = e.code if isinstance(e.code, int) else 2
        written = None
        if return_baseline:
            try:
                with open(bpath, encoding="utf-8") as bf: written = json.load(bf)
            except Exception:
                written = None
    finally:
        BASELINE = saved
        build_facts = saved_bf
        os.unlink(tpath)
        try: os.unlink(bpath)
        except OSError: pass
    if return_baseline:
        return rc, buf.getvalue(), written
    return rc, buf.getvalue()


def self_test():
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False,
                                     encoding="utf-8") as f:
        f.write(SELFTEST_SAMPLE)
        path = f.name
    try:
        got = scrape([path])
    finally:
        os.unlink(path)

    print("── regression gate self-test ────────────────────────────────")
    bad = 0
    for key, want in SELFTEST_EXPECT.items():
        have = got.get(key, "<absent>")
        ok = have == want
        print("  %-28s %s  got %r want %r" % (key, "ok  " if ok else "FAIL", have, want))
        bad += 0 if ok else 1
    extra = set(got) - set(SELFTEST_EXPECT)
    if extra:
        print("  unexpected keys scraped: %s" % sorted(extra))
        bad += 1
    # Decision control: the gate must FAIL each scenario below.  Scraping the
    # right numbers is not the same as reaching the right verdict, and all
    # three false-greens this test now covers were verdict bugs.
    base = {"devices": {"SELFTEST-1/plat/android16/6.6": {
        "preload.rw_abs_ns": 93.0, "preload.rw_rel_ns": 7.0,
        "preload.rw_sysdir_ns": 32.0, "preload.rw_under_ns": 116.7,
        "preload.rw_pinned": 1}}}
    for name, transcript, must_fail in SELFTEST_DECISIONS:
        rc, out = _run_decision(transcript, base)
        ok = (rc != 0) if must_fail else (rc == 0)
        print("  %-28s %s  %s -> rc=%d"
              % ("decision", "ok  " if ok else "FAIL", name, rc))
        if not ok:
            bad += 1
            for line in out.splitlines():
                print("        | %s" % line)

    # --update must never absorb a REAL failure.
    #
    # The guard used to read
    #     if failed and not (args.allow_new_device and failed == 1 and new_device)
    # and the "new device" FAIL is only counted into `failed` when
    # --allow-new-device is ABSENT.  So with the flag set, `failed == 1` meant
    # one genuine hard failure -- and `new_device` was still truthy, so the
    # exemption fired and the baseline was written from a failing run.  On the
    # documented first-baseline command, no less.
    #
    # Scenario: a device with no baseline (so --allow-new-device is needed) AND
    # a violated supervisor invariant.  It must refuse, and must not write.
    upd = """\
ALR BENCH DEVICE: SELFTEST-NEW/plat/android16/6.6
  PRELOAD RW PINNED 1 cpu=0
    PRELOAD RW ABS COST          58.8 ns  (ref <=  100)  RECORDED
    PRELOAD RW REL COST           4.4 ns  (ref <=   20)  RECORDED
    PRELOAD RW SYSDIR COST       13.8 ns  (ref <=   40)  RECORDED
    PRELOAD RW UNDER COST        53.4 ns  (ref <=  100)  RECORDED
    PRELOAD RW TOTAL COST       46.3 us  <= 1500  PASS
    PRELOAD RW CALLS 10101
path_traps=7 syscall_stops=0
"""
    rc, out, written = _run_decision(upd, base,
                                     ["--allow-new-device", "--update"],
                                     return_baseline=True)
    wrote_new = bool(written and "SELFTEST-NEW/plat/android16/6.6"
                     in written.get("devices", {}))
    ok = rc != 0 and not wrote_new
    print("  %-28s %s  --update refuses a real failure even with "
          "--allow-new-device -> rc=%d wrote=%s"
          % ("decision", "ok  " if ok else "FAIL", rc, wrote_new))
    if not ok:
        bad += 1
        for line in out.splitlines():
            print("        | %s" % line)

    # Negative control: refusals must produce no facts at all.
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False,
                                     encoding="utf-8") as f:
        f.write(SELFTEST_NEGATIVE)
        npath = f.name
    try:
        ngot = scrape([npath])
    finally:
        os.unlink(npath)
    leaked = sorted(k for k in ngot if k.startswith("preload.rw_"))
    if leaked:
        print("  %-28s FAIL  refusal output leaked facts: %s"
              % ("negative control", leaked))
        bad += 1
    else:
        print("  %-28s ok    refusals produce no preload.rw_* facts"
              % "negative control")

    print("────────────────────────────────────────────────────────────")
    print("ALR GATE SELF-TEST: %s" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


def main_with(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="inputs", nargs="*", default=[],
                    help="harness output files to scrape")
    ap.add_argument("--build-only", action="store_true",
                    help="check artifact invariants only (CI, no device)")
    ap.add_argument("--update", action="store_true",
                    help="rewrite bench/baseline.json from these facts")
    ap.add_argument("--allow-new-device", action="store_true",
                    help="permit recording a first baseline for an unseen device")
    ap.add_argument("--self-test", action="store_true",
                    help="check the scraper against a canned transcript (no device)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    facts, notes = build_facts()
    if not args.build_only:
        if not args.inputs:
            die("no harness output given. Pass --from <files>, or --build-only "
                "if you have no device output. Refusing to pass a gate on "
                "facts it never saw.")
        facts.update(scrape(args.inputs))

    baseline = {}
    if os.path.exists(BASELINE):
        try:
            with open(BASELINE, encoding="utf-8") as f:
                baseline = json.load(f)
        except (OSError, ValueError) as e:
            die("baseline unreadable: %s" % e)

    print("── regression gate ─────────────────────────────────────────")
    for n in notes:
        print("  NOTE  %s" % n)

    failed = warned = checked = new_device = 0

    for key, desc, ok in HARD:
        if key not in facts:
            # Missing is NOT pass.  A gate that silently skips what it cannot
            # see reports green on an empty file.
            if args.build_only and not key.startswith("preload.glibc"):
                print("  %-28s SKIP    build-only run" % key)
            else:
                print("  %-28s ABSENT  not in the given output" % key)
                failed += 1
            continue
        checked += 1
        v = facts[key]
        if ok(v):
            print("  %-28s PASS    %s (%s)" % (key, v, desc))
        else:
            print("  %-28s FAIL    %s, want %s" % (key, v, desc))
            failed += 1
            if key == "supervisor.syscall_stops":
                print("        ^ PTRACE_SYSCALL is back. This is PRoot's model;")
                print("          every performance claim in the repo is void.")

    for key, desc, why in UNENFORCED:
        print("  %-28s UNENFORCED  %s" % (key, why))
    for key, why in UNMEASURED_SOFT:
        print("  %-28s UNMEASURED  %s" % (key, why))

    # Soft baselines are PER DEVICE.  Timing numbers from one phone are not a
    # yardstick for another: the two reference devices differ by ~7% on node
    # cold start running byte-identical code, which is large enough to both
    # mask a real regression and manufacture a fake one.
    device = facts.get("_device")
    devices = baseline.get("devices", {})
    if device is None:
        if any(k in facts for k, _ in SOFT):
            print("  %-28s NOTE    harness emitted no 'ALR BENCH DEVICE:' line;"
                  % "soft baselines")
            print("        soft comparisons skipped rather than run against "
                  "another device's numbers")
        dev_baseline = None
    else:
        print("  %-28s %s" % ("device", device))
        dev_baseline = devices.get(device)
        if dev_baseline is None:
            print("  %-28s NEW     no baseline recorded for this device"
                  % "soft baselines")
            dev_baseline = {}

    # Per-device HARD regression checks (see PER_DEVICE_HARD).  Absent is a
    # failure, exactly like the absolute hard invariants: a gate that skips
    # what it cannot see reports green on an empty file.
    # Absolute ceilings run FIRST and are never skipped -- not for an unpinned
    # run, not for a device with no baseline, not for a pin-state mismatch.
    # An unpinned measurement is imprecise, not free.
    for key, ceil in PER_OP_CEILING:
        if key not in facts:
            if args.build_only:
                print("  %-28s SKIP    build-only run" % (key + " ceiling"))
            else:
                print("  %-28s ABSENT  not in the given output" % (key + " ceiling"))
                failed += 1
            continue
        checked += 1
        v = facts[key]
        if v > ceil:
            print("  %-28s FAIL    %.1f > %d (absolute ceiling)"
                  % (key + " ceiling", v, ceil))
            failed += 1
        else:
            print("  %-28s PASS    %.1f <= %d" % (key + " ceiling", v, ceil))

    # An unpinned run's per-op numbers are not comparable with a pinned
    # baseline -- that difference is precisely what pinning exists to remove
    # (bimodal big.LITTLE placement, a 1.7x effect).  Refuse the comparison
    # rather than run it against a mismatched reference.
    pin_now = facts.get("preload.rw_pinned")
    pin_base = (dev_baseline or {}).get("preload.rw_pinned")
    pin_ok = True
    if pin_now is not None:
        print("  %-28s %s" % ("rw pinned", "yes" if pin_now else "NO"))
        if not pin_now:
            print("        ^ per-op comparison skipped: unpinned numbers are "
                  "not reproducible on this class of device")
            pin_ok = False
        elif pin_base is not None and pin_base != pin_now:
            print("        ^ per-op comparison skipped: baseline was recorded "
                  "%s pinning" % ("with" if pin_base else "without"))
            pin_ok = False

    for key, tol in PER_DEVICE_HARD:
        if not pin_ok:
            # Not a pass, and not unchecked either: the absolute ceiling above
            # ran for this same key regardless of pin state.  What is skipped
            # is only the tight per-device tolerance.
            print("  %-28s SKIP    pin state (ceiling above still applied)" % key)
            continue
        if key not in facts:
            # Device facts cannot exist on a hosted runner; --build-only says
            # so explicitly rather than the gate guessing.
            if args.build_only:
                print("  %-28s SKIP    build-only run" % key)
            else:
                print("  %-28s ABSENT  not in the given output" % key)
                failed += 1
            continue
        v = facts[key]
        prev = (dev_baseline or {}).get(key)
        if prev is None:
            # A first run on a new device is otherwise unfailable by
            # construction, and whatever it measured becomes the permanent
            # reference -- including a regression.  Recording one has to be a
            # deliberate act.
            print("  %-28s BASELINE  %.1f (no baseline for this device)" % (key, v))
            new_device += 1
            continue
        checked += 1
        if v > prev * tol:
            print("  %-28s FAIL    %.1f > %.1f*%.2f  (regression on this device)"
                  % (key, v, prev, tol))
            failed += 1
        else:
            print("  %-28s PASS    %.1f (baseline %.1f, tol %.2fx)" % (key, v, prev, tol))

    for key, tol in SOFT:
        if key not in facts:
            continue
        if dev_baseline is None:
            continue
        prev = dev_baseline.get(key)
        v = facts[key]
        if prev is None:
            print("  %-28s BASELINE  %s (no previous value for this device)" % (key, v))
            continue
        if tol is None:                       # higher is better
            if v < prev * 0.90:
                print("  %-28s WARN    %s < %s*0.90" % (key, v, prev))
                warned += 1
            else:
                print("  %-28s ok      %s (prev %s)" % (key, v, prev))
        elif v > prev * tol:
            print("  %-28s WARN    %s > %s*%.2f" % (key, v, prev, tol))
            warned += 1
        else:
            print("  %-28s ok      %s (prev %s)" % (key, v, prev))

    if new_device and not args.allow_new_device:
        print("  %-28s FAIL    %d per-op key(s) have no baseline for this device"
              % ("new device", new_device))
        print("        the tolerance check cannot run; the absolute ceilings did.")
        print("        Re-run with --allow-new-device --update to record one.")
        failed += 1

    if args.update:
        # Refusing to write a baseline from a failing run is the whole point.
        # REPRODUCED on the first version of this file: a run failing
        # "rw_abs_ns 4334.7 > 93.0*2.50" still wrote 4334.7 into
        # bench/baseline.json and exited 1, so the NEXT run was green.  That is
        # a goalpost move implemented inside the gate.
        # NO EXEMPTION HERE.  The previous form was
        #     if failed and not (args.allow_new_device and failed == 1 and new_device)
        # and it had a hole exactly where it mattered: the "new device" FAIL is
        # only counted into `failed` when --allow-new-device is ABSENT, so with
        # the flag set `failed == 1` means one REAL hard failure -- and
        # `new_device` was still truthy, so the exemption fired and wrote a
        # baseline from a run whose supervisor invariant or per-op ceiling had
        # just failed.  That is the documented first-baseline command
        # (--allow-new-device --update), so the hole was on the path most
        # likely to be taken on a brand-new device.
        #
        # The two cases are already disjoint without any special case:
        #   without the flag -> the unbaselined device IS a failure, refuse
        #   with the flag    -> it is not counted, so anything left is real
        if failed:
            die("refusing --update on a failing run: %d hard check(s) failed. "
                "Writing these numbers to the baseline would make the next run "
                "green by redefining the reference -- the exact move this gate "
                "exists to prevent. Fix the regression first. (--allow-new-device "
                "excuses ONLY an unbaselined device, never a failed check.)"
                % failed)
        if device is None:
            die("--update needs a device identity. Run tests/device/bench.sh so "
                "the output carries an 'ALR BENCH DEVICE:' line; a baseline "
                "with no device attached is what this gate was fixed to stop.")
        keep = {k: facts[k] for k, _ in SOFT if k in facts}
        keep.update({k: facts[k] for k, _ in PER_DEVICE_HARD if k in facts})
        # Record the pin state alongside, so a later run can tell whether the
        # baseline it is comparing against was measured the same way.
        if "preload.rw_pinned" in facts:
            keep["preload.rw_pinned"] = facts["preload.rw_pinned"]
        devices[device] = {**devices.get(device, {}), **keep}
        baseline["devices"] = devices
        with open(BASELINE, "w", encoding="utf-8") as f:
            json.dump(baseline, f, indent=2, sort_keys=True)
            f.write("\n")
        print("  baseline written for %s: %s" % (device, BASELINE))

    print("────────────────────────────────────────────────────────────")
    print("  hard checked=%d failed=%d   soft warnings=%d" % (checked, failed, warned))
    print("ALR REGRESSION GATE: %s" % ("FAIL" if failed else "PASS"))
    return 1 if failed else 0


def main():
    return main_with()


if __name__ == "__main__":
    sys.exit(main())
