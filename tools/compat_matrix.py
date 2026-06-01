from __future__ import annotations

import argparse
import json
from collections import Counter
from dataclasses import dataclass, field


# --------------------------------------------------------------------------- #
# Toolkit classes
# --------------------------------------------------------------------------- #

TOOLKIT_GTK3 = "gtk3"
TOOLKIT_GTK4 = "gtk4"
TOOLKIT_QT5 = "qt5"
TOOLKIT_QT6 = "qt6"
TOOLKIT_SDL2 = "sdl2"
TOOLKIT_TERMINAL = "terminal"
TOOLKIT_BROWSER = "browser"
TOOLKIT_X11 = "x11"
TOOLKIT_CLI = "cli"

TOOLKITS = frozenset(
    {
        TOOLKIT_GTK3,
        TOOLKIT_GTK4,
        TOOLKIT_QT5,
        TOOLKIT_QT6,
        TOOLKIT_SDL2,
        TOOLKIT_TERMINAL,
        TOOLKIT_BROWSER,
        TOOLKIT_X11,
        TOOLKIT_CLI,
    }
)

# Aliases collapse the many ways a toolkit gets named in evidence/logs onto the
# canonical class above. Bare "gtk"/"qt" pick the version we actually target.
_TOOLKIT_ALIASES = {
    "gtk": TOOLKIT_GTK3,
    "gtk2": TOOLKIT_GTK3,
    "gtk+3": TOOLKIT_GTK3,
    "gtk-3": TOOLKIT_GTK3,
    "gtk+4": TOOLKIT_GTK4,
    "gtk-4": TOOLKIT_GTK4,
    "qt": TOOLKIT_QT6,
    "qt-5": TOOLKIT_QT5,
    "qt-6": TOOLKIT_QT6,
    "qtwidgets": TOOLKIT_QT6,
    "sdl": TOOLKIT_SDL2,
    "libsdl2": TOOLKIT_SDL2,
    "term": TOOLKIT_TERMINAL,
    "tty": TOOLKIT_TERMINAL,
    "vte": TOOLKIT_TERMINAL,
    "web": TOOLKIT_BROWSER,
    "chromium": TOOLKIT_BROWSER,
    "cef": TOOLKIT_BROWSER,
    "xwayland": TOOLKIT_X11,
    "xorg": TOOLKIT_X11,
    "x": TOOLKIT_X11,
    "commandline": TOOLKIT_CLI,
    "console": TOOLKIT_CLI,
    "headless": TOOLKIT_CLI,
}


# --------------------------------------------------------------------------- #
# Results
# --------------------------------------------------------------------------- #

RESULT_PASS = "PASS"
RESULT_FAIL = "FAIL"
RESULT_PENDING = "PENDING"
RESULT_KNOWN_FAIL = "KNOWN_FAIL"

RESULTS = frozenset(
    {
        RESULT_PASS,
        RESULT_FAIL,
        RESULT_PENDING,
        RESULT_KNOWN_FAIL,
    }
)

_RESULT_ALIASES = {
    "ok": RESULT_PASS,
    "pass": RESULT_PASS,
    "passed": RESULT_PASS,
    "fail": RESULT_FAIL,
    "failed": RESULT_FAIL,
    "pending": RESULT_PENDING,
    "todo": RESULT_PENDING,
    "wip": RESULT_PENDING,
    "blocked": RESULT_PENDING,
    "known_fail": RESULT_KNOWN_FAIL,
    "known-fail": RESULT_KNOWN_FAIL,
    "knownfail": RESULT_KNOWN_FAIL,
    "xfail": RESULT_KNOWN_FAIL,
}


# --------------------------------------------------------------------------- #
# Normalization helpers
# --------------------------------------------------------------------------- #

def normalize_toolkit(value: str) -> str:
    normalized = value.strip().lower().replace(" ", "").replace("_", "").replace(".", "")
    normalized = normalized.replace("+", "+")  # keep '+' so gtk+3 etc. resolve
    return _TOOLKIT_ALIASES.get(normalized, normalized)


def normalize_result(value: str) -> str:
    normalized = value.strip().lower()
    mapped = _RESULT_ALIASES.get(normalized, normalized)
    return mapped.upper()


# --------------------------------------------------------------------------- #
# Data model
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class AppCompat:
    app: str
    toolkit: str
    result: str
    stage_tar: str = ""
    evidence_ref: str = ""
    guard_skipped: tuple[str, ...] = ()
    notes: str = ""

    def __post_init__(self) -> None:
        if not self.app.strip():
            raise ValueError("app is required")
        normalized_toolkit = normalize_toolkit(self.toolkit)
        if normalized_toolkit not in TOOLKITS:
            raise ValueError(
                f"unknown toolkit {self.toolkit!r} (normalized {normalized_toolkit!r}); "
                f"expected one of {sorted(TOOLKITS)}"
            )
        object.__setattr__(self, "toolkit", normalized_toolkit)
        normalized_result = normalize_result(self.result)
        if normalized_result not in RESULTS:
            raise ValueError(
                f"unknown result {self.result!r} (normalized {normalized_result!r}); "
                f"expected one of {sorted(RESULTS)}"
            )
        object.__setattr__(self, "result", normalized_result)
        object.__setattr__(self, "guard_skipped", tuple(self.guard_skipped))


@dataclass(frozen=True)
class CompatMatrix:
    entries: tuple[AppCompat, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "entries", tuple(self.entries))

    def by_toolkit(self) -> dict[str, tuple[AppCompat, ...]]:
        grouped: dict[str, list[AppCompat]] = {}
        for entry in self.entries:
            grouped.setdefault(entry.toolkit, []).append(entry)
        return {tk: tuple(items) for tk, items in grouped.items()}

    def results_for(self, toolkit: str) -> Counter[str]:
        normalized = normalize_toolkit(toolkit)
        counter: Counter[str] = Counter()
        for entry in self.entries:
            if entry.toolkit == normalized:
                counter[entry.result] += 1
        return counter

    def to_markdown(self) -> str:
        lines: list[str] = []
        lines.append("# ALR compatibility matrix")
        lines.append("")
        lines.append("| App | Toolkit | Result | Stage tar | Evidence | Notes |")
        lines.append("| --- | --- | --- | --- | --- | --- |")
        for entry in self.entries:
            notes = entry.notes
            if entry.guard_skipped:
                skipped = "guard-skipped: " + ", ".join(entry.guard_skipped)
                notes = f"{notes}; {skipped}" if notes else skipped
            row = [
                _md_cell(entry.app),
                _md_cell(entry.toolkit),
                _md_cell(entry.result),
                _md_cell(entry.stage_tar),
                _md_cell(entry.evidence_ref),
                _md_cell(notes),
            ]
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")
        lines.append("## Per-toolkit summary")
        lines.append("")
        lines.append("| Toolkit | Apps | PASS | FAIL | PENDING | KNOWN_FAIL |")
        lines.append("| --- | --- | --- | --- | --- | --- |")
        grouped = self.by_toolkit()
        for toolkit in sorted(grouped):
            counts = self.results_for(toolkit)
            total = sum(counts.values())
            lines.append(
                "| {tk} | {total} | {p} | {f} | {pend} | {kf} |".format(
                    tk=toolkit,
                    total=total,
                    p=counts.get(RESULT_PASS, 0),
                    f=counts.get(RESULT_FAIL, 0),
                    pend=counts.get(RESULT_PENDING, 0),
                    kf=counts.get(RESULT_KNOWN_FAIL, 0),
                )
            )
        return "\n".join(lines)

    def to_json(self) -> str:
        payload = {
            "entries": [
                {
                    "app": entry.app,
                    "toolkit": entry.toolkit,
                    "result": entry.result,
                    "stage_tar": entry.stage_tar,
                    "evidence_ref": entry.evidence_ref,
                    "guard_skipped": list(entry.guard_skipped),
                    "notes": entry.notes,
                }
                for entry in self.entries
            ],
            "summary": {
                toolkit: dict(self.results_for(toolkit))
                for toolkit in sorted(self.by_toolkit())
            },
        }
        return json.dumps(payload, indent=2, sort_keys=True)


def _md_cell(value: str) -> str:
    # Escape pipes/newlines so a stray cell value can't break the table.
    return value.replace("|", "\\|").replace("\n", " ").strip() or "-"


# --------------------------------------------------------------------------- #
# Universality gate
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class UniversalityAssessment:
    can_claim: bool
    covered: frozenset[str]
    missing: frozenset[str]
    failing_apps: tuple[str, ...]
    reason: str


def assess_universality(
    matrix: CompatMatrix, required_toolkits: frozenset[str]
) -> UniversalityAssessment:
    required = frozenset(normalize_toolkit(tk) for tk in required_toolkits)

    passing_toolkits = {
        entry.toolkit for entry in matrix.entries if entry.result == RESULT_PASS
    }
    covered = frozenset(required & passing_toolkits)
    missing = frozenset(required - passing_toolkits)

    # A hard FAIL on any required toolkit blocks the claim (KNOWN_FAIL/PENDING
    # do not — they are expected/incomplete, not regressions).
    failing_apps = tuple(
        entry.app
        for entry in matrix.entries
        if entry.toolkit in required and entry.result == RESULT_FAIL
    )

    if missing:
        return UniversalityAssessment(
            can_claim=False,
            covered=covered,
            missing=missing,
            failing_apps=failing_apps,
            reason="missing PASS evidence for required toolkit(s)",
        )
    if failing_apps:
        return UniversalityAssessment(
            can_claim=False,
            covered=covered,
            missing=missing,
            failing_apps=failing_apps,
            reason="one or more required-toolkit apps are FAIL",
        )
    return UniversalityAssessment(
        can_claim=True,
        covered=covered,
        missing=frozenset(),
        failing_apps=tuple(),
        reason="every required toolkit has a passing app and no required-toolkit FAILs",
    )


# --------------------------------------------------------------------------- #
# Sample matrix
# --------------------------------------------------------------------------- #

def sample_matrix() -> CompatMatrix:
    return CompatMatrix(
        entries=(
            AppCompat(
                app="GIMP",
                toolkit="gtk3",
                result="PASS",
                stage_tar="stage/gimp.tar",
                evidence_ref="docs/research/alr-gui-android-native-polish.md",
                notes="fully usable by touch on device (v111)",
            ),
            AppCompat(
                app="gtk3-demo",
                toolkit="gtk",
                result="PASS",
                stage_tar="stage/gtk3-demo.tar",
                evidence_ref="logcat:ALR_GUI",
            ),
            AppCompat(
                app="foot",
                toolkit="terminal",
                result="PASS",
                stage_tar="stage/foot.tar",
                evidence_ref="logcat:ALR_TERM",
            ),
            AppCompat(
                app="qt6-hello",
                toolkit="qt6",
                result="PENDING",
                evidence_ref="docs/research/orchestration-5session-plan.md",
                notes="not yet device-run",
            ),
            AppCompat(
                app="chromium",
                toolkit="browser",
                result="KNOWN_FAIL",
                stage_tar="stage/chromium.tar",
                evidence_ref="logcat:ALR_BROWSER",
                guard_skipped=("libharfbuzz.so.0",),
                notes="--dump-dom deadlock (v123)",
            ),
        )
    )


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        print(f"  [{status}] {label}")

    matrix = sample_matrix()

    # Normalization / validation.
    check("normalize_toolkit gtk -> gtk3", normalize_toolkit("gtk") == TOOLKIT_GTK3)
    check("normalize_toolkit qt -> qt6", normalize_toolkit("Qt") == TOOLKIT_QT6)
    check("normalize_toolkit xwayland -> x11", normalize_toolkit("xwayland") == TOOLKIT_X11)
    check("normalize_result ok -> PASS", normalize_result("ok") == RESULT_PASS)

    check(
        "AppCompat normalizes alias toolkit",
        matrix.entries[1].toolkit == TOOLKIT_GTK3,
    )

    raised = False
    try:
        AppCompat(app="", toolkit="gtk3", result="PASS")
    except ValueError:
        raised = True
    check("empty app raises ValueError", raised)

    raised = False
    try:
        AppCompat(app="x", toolkit="not-a-toolkit", result="PASS")
    except ValueError:
        raised = True
    check("invalid toolkit raises ValueError", raised)

    raised = False
    try:
        AppCompat(app="x", toolkit="gtk3", result="MAYBE")
    except ValueError:
        raised = True
    check("invalid result raises ValueError", raised)

    # Rendering.
    md = matrix.to_markdown()
    check("to_markdown non-empty", bool(md.strip()))
    check("to_markdown contains app names", "GIMP" in md and "chromium" in md and "foot" in md)
    check("to_markdown contains per-toolkit summary", "Per-toolkit summary" in md)
    check("to_markdown surfaces guard_skipped", "libharfbuzz.so.0" in md)

    js = matrix.to_json()
    check("to_json non-empty", bool(js.strip()))
    parsed = json.loads(js)
    check("to_json round-trips entries", len(parsed["entries"]) == len(matrix.entries))
    check("to_json contains app names", "GIMP" in js and "chromium" in js)
    check(
        "to_json guard_skipped preserved",
        parsed["entries"][-1]["guard_skipped"] == ["libharfbuzz.so.0"],
    )

    # Universality gate.
    ok = assess_universality(matrix, frozenset({TOOLKIT_GTK3, TOOLKIT_TERMINAL}))
    check("required={gtk3,terminal} can_claim True", ok.can_claim is True)
    check("required={gtk3,terminal} no missing", ok.missing == frozenset())
    check(
        "required={gtk3,terminal} covered both",
        ok.covered == frozenset({TOOLKIT_GTK3, TOOLKIT_TERMINAL}),
    )

    no = assess_universality(matrix, frozenset({TOOLKIT_GTK3, TOOLKIT_TERMINAL, TOOLKIT_QT6}))
    check("required+qt6 can_claim False", no.can_claim is False)
    check("required+qt6 qt6 in missing", TOOLKIT_QT6 in no.missing)

    # A required-toolkit FAIL must block even when a PASS also exists.
    with_fail = CompatMatrix(
        entries=matrix.entries
        + (AppCompat(app="broken-gtk", toolkit="gtk3", result="FAIL"),)
    )
    blocked = assess_universality(with_fail, frozenset({TOOLKIT_GTK3}))
    check("required-toolkit FAIL blocks claim", blocked.can_claim is False)
    check("failing app reported", "broken-gtk" in blocked.failing_apps)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="compat_matrix",
        description="ALR compatibility matrix: which Linux apps run under ALR, by toolkit.",
    )
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    parser.add_argument("--demo", action="store_true", help="print a sample matrix as markdown")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if args.demo:
        print(sample_matrix().to_markdown())
        return 0
    parser.error("nothing to do (use --selftest or --demo)")
    return 2  # unreachable; parser.error exits


if __name__ == "__main__":
    raise SystemExit(main())
