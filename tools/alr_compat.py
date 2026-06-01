"""ALR current app-compatibility state (WS-4 / L4, milestone M5).

A living, honest record of which Linux apps run under ALR, by toolkit, with the
evidence source. Built on the `tools.compat_matrix` model. Results follow the
project rule "no completion claim without device evidence": only entries with a
cited device-evidence source are PASS; wired-but-not-yet-device-checked apps are
PENDING; the chromium overlay is KNOWN_FAIL (gated off + ptrace wall).

Device evidence cited here was produced by sibling sessions (attributed in
`evidence_ref`); this module records their status, it does not re-claim them.

Run: `python -m tools.alr_compat --report` (markdown) / `--json` / `--selftest`.
"""

from __future__ import annotations

import argparse

from tools.compat_matrix import (
    AppCompat,
    CompatMatrix,
    TOOLKIT_CLI,
    TOOLKIT_GTK3,
    TOOLKIT_TERMINAL,
    assess_universality,
)


def current_matrix() -> CompatMatrix:
    """ALR's compatibility state as of the WS-4 integration on main (c34051c+)."""
    return CompatMatrix(
        entries=(
            # --- device-verified (sibling-session evidence) ---
            AppCompat(
                app="GIMP",
                toolkit="gtk3",
                result="PASS",
                evidence_ref="memory:alr-gui-android-native-polish (v111, device)",
                notes="fully usable by touch on device (File>New canvas, brush strokes)",
            ),
            AppCompat(
                app="gtk3-widget-factory",
                toolkit="gtk3",
                result="PASS",
                stage_tar="gtk3demo-stage.tar",
                evidence_ref="commit c084895 (v127, CP-1 device render)",
                notes="rendered after WS-1 XKB_CONFIG_ROOT keymap fix",
            ),
            AppCompat(
                app="native CLI (hello/dash/glibc-hello)",
                toolkit="cli",
                result="PASS",
                evidence_ref="memory:device-evidence-mali-android16 (v73/v76/v79, device)",
                notes="glibc static + in-process dynamic native exec, PRoot-free",
            ),
            # --- wired + guarded, device-pending ---
            AppCompat(
                app="gtk3-demo / gtk3-icon-browser",
                toolkit="gtk3",
                result="PENDING",
                stage_tar="gtk3demo-stage.tar",
                notes="overlay wired through guarded extractOverlayTar; device-pending",
            ),
            AppCompat(
                app="foot",
                toolkit="terminal",
                result="PENDING",
                stage_tar="foot-stage.tar",
                notes="overlay wired+guarded; pty under untrusted_app unverified",
            ),
            # --- M2 targets (overlay buildable via tools/deb_closure) ---
            AppCompat(
                app="netsurf-gtk",
                toolkit="browser",
                result="PENDING",
                notes="M2 easiest win: GTK3 frontend reuses base closure; GDK_BACKEND=wayland",
            ),
            AppCompat(
                app="SDL2 demo",
                toolkit="sdl2",
                result="PENDING",
                notes="M2: SDL_VIDEODRIVER=wayland; needs libxkbcommon0/libwayland-egl1/libdecor",
            ),
            AppCompat(
                app="Qt6 widgets demo",
                toolkit="qt6",
                result="PENDING",
                notes="M2: qt6-wayland, QT_QPA_PLATFORM=wayland, software backend (no dmabuf)",
            ),
            # --- M4 target ---
            AppCompat(
                app="xterm / xeyes (Xwayland)",
                toolkit="x11",
                result="PENDING",
                notes="M4: rootful Xwayland (no XWM in compositor); -ac -shm, fs X11 socket",
            ),
            # --- known-failing ---
            AppCompat(
                app="chromium-headless-shell",
                toolkit="browser",
                result="KNOWN_FAIL",
                stage_tar="chromium-stage.tar",
                guard_skipped=(
                    "libharfbuzz.so.0",
                    "liblcms2.so.2",
                    "libopenjp2.so.7",
                ),
                evidence_ref="memory:chromium-native-goal",
                notes="multithread ptrace syscall-storm wall; overlay gated off per user hold; "
                "lib-downgrade guard skips the 3 base-lib downgrades it ships",
            ),
        ),
    )


# Toolkit classes that must each have >=1 PASS for a baseline "universal native GUI"
# claim. (browser/qt6/sdl2/x11 are stretch goals tracked separately.)
BASELINE_REQUIRED = frozenset({TOOLKIT_GTK3, TOOLKIT_TERMINAL, TOOLKIT_CLI})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="alr_compat",
        description="ALR current app-compatibility matrix (WS-4 M5).",
    )
    parser.add_argument("--report", action="store_true", help="print the matrix as markdown")
    parser.add_argument("--json", action="store_true", help="print the matrix as JSON")
    parser.add_argument("--selftest", action="store_true", help="run built-in tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    matrix = current_matrix()
    if args.json:
        print(matrix.to_json())
    else:
        print(matrix.to_markdown())
        assessment = assess_universality(matrix, BASELINE_REQUIRED)
        print()
        print(f"baseline-GUI claim ({', '.join(sorted(BASELINE_REQUIRED))}): "
              f"{'CLAIMABLE' if assessment.can_claim else 'NOT YET'} — {assessment.reason}")
        if assessment.missing:
            print(f"  missing PASS coverage: {', '.join(sorted(assessment.missing))}")
    return 0


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    matrix = current_matrix()
    check("matrix builds + validates (frozen models)", len(matrix.entries) >= 8)
    md = matrix.to_markdown()
    check("markdown renders with app names", "GIMP" in md and "netsurf-gtk" in md)
    js = matrix.to_json()
    check("json renders non-empty", len(js) > 100 and "chromium" in js)

    assessment = assess_universality(matrix, BASELINE_REQUIRED)
    # gtk3 (GIMP/widget-factory), terminal (foot is PENDING!), cli (PASS).
    # foot is the only terminal entry and it's PENDING → terminal has no PASS →
    # baseline NOT yet claimable. This asserts the honest state.
    check("baseline GUI NOT yet claimable (terminal has no device PASS)", not assessment.can_claim)
    check("terminal flagged as missing PASS coverage", TOOLKIT_TERMINAL in assessment.missing)

    # gtk3 + cli alone ARE both satisfied by a device PASS.
    gtk_cli = assess_universality(matrix, frozenset({TOOLKIT_GTK3, TOOLKIT_CLI}))
    check("gtk3+cli baseline IS claimable (both have device PASS)", gtk_cli.can_claim)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
