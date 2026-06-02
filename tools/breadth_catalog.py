"""Breadth catalog — HOST prediction of `apt install`-ability for many apps.

What this is
------------
ALR's universality claim is "any ARM64 glibc Linux app on non-root Android". The
GUI/GPU/loader tracks prove *specific* apps end-to-end on device; this module
answers the orthogonal, breadth question **on the host, with no device**:

    For a catalog of lightweight GUI + CLI apps, can their full runtime
    dependency closure be RESOLVED against the Ubuntu noble (ports) index with
    ZERO unsatisfied dependencies — and how big would the resulting overlay be?

It is a *prediction*, not a device proof. A 0-unsat closure means "every Depends
edge lands on a real package in the noble index" — necessary for `apt install`,
not sufficient (the terminal `unpacked=true`/launch still rides the G1
exec-re-entry unlock — see docs/research/loader-feature-gaps.md G1 and the apt
row in docs/research/alr-compat-matrix.md). We are deliberately honest about that
ceiling everywhere this module reports.

Reuse, not duplication
----------------------
The closure engine is :mod:`tools.deb_closure`. This module imports and reuses:
  * ``fetch_packages_index`` — noble (or any suite) Packages index over the net;
  * ``parse_packages`` / ``build_provides_map`` — index → name/virtual maps;
  * ``resolve_closure``     — the BFS over Depends/Pre-Depends + alternatives.
It adds only the *catalog* (which apps), the *prediction model* (0-unsat verdict,
closure/download/installed sizes, base-subtracted soname estimate, stage-tar
estimate) and a category taxonomy.

Sizes
-----
Each noble ``Packages`` stanza carries ``Size`` (compressed .deb download bytes)
and ``Installed-Size`` (KiB unpacked, per Debian policy). We sum both across the
closure. The *staged overlay* is smaller than Installed-Size because ALR
subtracts every SONAME the base already owns (the harfbuzz-downgrade class the
overlay_guard freezes) and prunes runtime-irrelevant payload (man/doc/locale).
We model that subtraction when a base soname set is supplied (offline-derivable
via :mod:`tools.base_inventory`); otherwise we report the raw Installed-Size as
an upper bound and flag the estimate as ``base_subtracted=False``.

Network vs offline
------------------
``predict_catalog(..., index=...)`` is pure: feed it any parsed index (the live
noble index, or a synthetic fixture) and it never touches the network. The
``--selftest`` path is fully OFFLINE/deterministic over an in-memory synthetic
index that exercises every code path (0-unsat, has-unsat, virtual/Provides,
base subtraction, category mapping). The ``--live`` path fetches the real noble
ports index and predicts the real catalog.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from enum import Enum

from tools.deb_closure import (
    build_provides_map,
    fetch_packages_index,
    parse_packages,
    resolve_closure,
)

# --------------------------------------------------------------------------- #
# Noble ports defaults (the ALR base is Ubuntu noble 24.04 arm64 — glibc 2.39)
# --------------------------------------------------------------------------- #
NOBLE_MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
NOBLE_SUITE = "noble"
NOBLE_ARCH = "arm64"
# Ubuntu splits libraries across main + universe; the closure needs both or many
# leaf-app deps go unsatisfied. (deb_closure documents the same.)
NOBLE_COMPONENTS = ("main", "universe")


# --------------------------------------------------------------------------- #
# Category taxonomy (self-contained — alr_manifest lives on the product-ux
# branch and is intentionally NOT a dependency here)
# --------------------------------------------------------------------------- #
class Category(str, Enum):
    """Coarse app category for the breadth report. String-valued so it serialises
    straight to JSON and compares equal to its bare name."""

    UTILITY = "utility"        # calculators, file/term helpers
    TERMINAL = "terminal"      # terminal emulators / shells UX
    EDITOR = "editor"          # text/code editors
    SYSTEM = "system"          # process/system monitors, sysinfo
    MEDIA = "media"            # audio/video/image viewers + players
    GRAPHICS = "graphics"      # vector/raster drawing
    GAME = "game"              # small games
    VIEWER = "viewer"          # document/image viewers
    NETWORK = "network"        # network clients


class Kind(str, Enum):
    GUI = "gui"      # needs the Wayland compositor / a window
    CLI = "cli"      # terminal-only


# --------------------------------------------------------------------------- #
# Catalog
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class CandidateApp:
    """One catalog entry: the apt package name(s) to install for this app.

    ``packages`` is the install target set (usually one); the closure is resolved
    over all of them together so meta/data deps are counted once.
    """

    name: str                       # human label
    packages: tuple[str, ...]       # apt install targets
    category: Category
    kind: Kind
    note: str = ""                  # why it's interesting / known caveat

    def __post_init__(self) -> None:
        if not self.name.strip():
            raise ValueError("app name is required")
        if not self.packages:
            raise ValueError(f"{self.name}: at least one package is required")
        if not isinstance(self.category, Category):
            raise TypeError(f"{self.name}: category must be a Category enum")
        if not isinstance(self.kind, Kind):
            raise TypeError(f"{self.name}: kind must be a Kind enum")


# Lightweight, broadly-representative GUI + CLI apps. Chosen so the catalog spans
# every Category/Kind and stresses different toolkits (GTK2/GTK3, Qt, raw Xlib,
# SDL) and pure-CLI closures — to make the breadth prediction meaningful, not a
# single-toolkit echo. Package names are the noble (24.04) apt names.
DEFAULT_CATALOG: tuple[CandidateApp, ...] = (
    # --- CLI -------------------------------------------------------------- #
    CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI,
                 "tiny editor; near-libc-only closure (closure floor)"),
    CandidateApp("htop", ("htop",), Category.SYSTEM, Kind.CLI,
                 "ncurses process monitor"),
    CandidateApp("ncdu", ("ncdu",), Category.SYSTEM, Kind.CLI,
                 "ncurses disk usage"),
    CandidateApp("jq", ("jq",), Category.UTILITY, Kind.CLI,
                 "json filter; oniguruma dep"),
    CandidateApp("tree", ("tree",), Category.UTILITY, Kind.CLI,
                 "directory lister; trivial closure"),
    CandidateApp("mpv", ("mpv",), Category.MEDIA, Kind.CLI,
                 "media player; large A/V codec closure (closure ceiling)"),
    # --- GUI (lightweight) ------------------------------------------------ #
    CandidateApp("galculator", ("galculator",), Category.UTILITY, Kind.GUI,
                 "GTK3 calculator"),
    CandidateApp("xcalc (x11-apps)", ("x11-apps",), Category.UTILITY, Kind.GUI,
                 "classic Xaw calculator; ships in the x11-apps bundle; pure X11 (Xwayland host)"),
    CandidateApp("xterm", ("xterm",), Category.TERMINAL, Kind.GUI,
                 "X11 terminal emulator"),
    CandidateApp("feh", ("feh",), Category.VIEWER, Kind.GUI,
                 "imlib2 image viewer; X11"),
    CandidateApp("gnome-mahjongg", ("gnome-mahjongg",), Category.GAME, Kind.GUI,
                 "GTK3 tile game"),
    CandidateApp("inkscape", ("inkscape",), Category.GRAPHICS, Kind.GUI,
                 "GTK3 vector editor; heavy closure (GUI ceiling)"),
)


# --------------------------------------------------------------------------- #
# Prediction model
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class ClosurePrediction:
    """Host prediction for one app — closure resolution outcome + size model.

    Honesty contract: ``installable_host`` is a *closure resolves with 0 unsat*
    verdict, NOT a device-install proof. The terminal apt unpack still needs the
    G1 exec-re-entry unlock (see module docstring).
    """

    app: str
    category: str
    kind: str
    packages: tuple[str, ...]
    closure: tuple[str, ...]            # resolved closure package names
    closure_size: int                   # number of packages in the closure
    unsatisfied: tuple[str, ...]        # closure log lines (dep group not in index)
    unsatisfied_count: int
    missing_targets: tuple[str, ...]    # requested packages absent from the index
    installable_host: bool              # 0-unsat AND all targets present
    download_bytes: int                 # summed .deb Size over the closure
    installed_bytes: int                # summed Installed-Size (KiB→bytes) over closure
    base_subtracted: bool               # True iff base soname subtraction was applied
    stage_tar_bytes: int                # estimated staged-overlay size
    note: str = ""

    def as_dict(self) -> dict:
        d = {
            "app": self.app,
            "category": self.category,
            "kind": self.kind,
            "packages": list(self.packages),
            "closure_size": self.closure_size,
            "unsatisfied_count": self.unsatisfied_count,
            "unsatisfied": list(self.unsatisfied),
            "missing_targets": list(self.missing_targets),
            "installable_host": self.installable_host,
            "download_bytes": self.download_bytes,
            "installed_bytes": self.installed_bytes,
            "base_subtracted": self.base_subtracted,
            "stage_tar_bytes": self.stage_tar_bytes,
            "note": self.note,
        }
        return d


def _int_field(fields: dict, key: str) -> int:
    """Parse an integer index field, tolerating absence / junk → 0."""
    raw = fields.get(key, "")
    try:
        return int(str(raw).strip())
    except (TypeError, ValueError):
        return 0


# Average shrink of unpacked payload once base-owned SONAMEs are dropped and
# man/doc/locale are pruned. Conservative: deb_closure's real builds (e.g. the
# 58-pkg apt-dpkg closure) land well under raw Installed-Size, but the exact
# factor is per-app. Used ONLY when no base soname set is supplied — clearly
# flagged base_subtracted=False so callers know it's an upper-bound heuristic.
_PRUNE_HEURISTIC = 0.65


def predict_app(
    app: CandidateApp,
    index: dict[str, dict],
    *,
    provides_map: dict[str, list[str]] | None = None,
    base_sonames: set[str] | None = None,
    soname_field: str = "soname",
) -> ClosurePrediction:
    """Resolve ``app``'s closure over ``index`` and model its overlay size.

    Pure / offline: ``index`` is any parsed Packages map (live noble or fixture).

    Size model: we sum each closure member's ``Size`` (download) and
    ``Installed-Size`` (unpacked). When ``base_sonames`` is supplied, a closure
    member that exists ONLY to ship a base-owned SONAME adds no NEW payload to the
    overlay (the base library wins — the overlay_guard freezes it), so it is
    EXCLUDED from the sums and ``base_subtracted=True`` is reported. Without a base
    set we report the raw sums shrunk only by the man/doc/locale prune heuristic
    and flag ``base_subtracted=False`` so the staged estimate is read as an
    upper bound.
    """
    if provides_map is None:
        provides_map = build_provides_map(index)

    log: list[str] = []
    closure = resolve_closure(list(app.packages), index, provides_map=provides_map, log=log)

    # missing targets: requested packages that did not resolve to anything
    present_targets = set(closure)
    missing_targets = tuple(
        p for p in app.packages
        if p not in present_targets and p not in index
    )

    unsatisfied = tuple(line for line in log if line.startswith("unsatisfied dep group"))
    # a wholly-missing target is also a target-not-in-index log line
    target_missing_logs = tuple(line for line in log if line.startswith("target not in index"))

    installable = not unsatisfied and not missing_targets and not target_missing_logs and bool(closure)

    # --- size model ------------------------------------------------------- #
    # When a base soname set is supplied, a closure member that is *purely* a
    # base-provided library (its package name is a known base soname stem) ships
    # no NEW payload — exclude it from the size sums. We match conservatively on
    # the package's own provided soname names when present.
    base_sonames = base_sonames or set()
    download = 0
    installed = 0
    for name in closure:
        fields = index.get(name, {})
        if base_sonames and _provides_only_base_soname(name, fields, base_sonames):
            continue
        download += _int_field(fields, "Size")
        # Installed-Size is in KiB per Debian policy
        installed += _int_field(fields, "Installed-Size") * 1024

    if base_sonames:
        # real subtraction applied at closure-member granularity; the staged tar
        # is the surviving installed payload minus pruned man/doc/locale.
        stage = int(installed * _PRUNE_HEURISTIC)
        base_subtracted = True
    else:
        # no base set → upper bound: raw installed shrunk only by the prune heuristic
        stage = int(installed * _PRUNE_HEURISTIC)
        base_subtracted = False

    return ClosurePrediction(
        app=app.name,
        category=app.category.value,
        kind=app.kind.value,
        packages=tuple(app.packages),
        closure=tuple(closure),
        closure_size=len(closure),
        unsatisfied=unsatisfied,
        unsatisfied_count=len(unsatisfied),
        missing_targets=missing_targets,
        installable_host=installable,
        download_bytes=download,
        installed_bytes=installed,
        base_subtracted=base_subtracted,
        stage_tar_bytes=stage,
        note=app.note,
    )


def _provides_only_base_soname(name: str, fields: dict, base_sonames: set[str]) -> bool:
    """True iff this closure member exists only to ship a base-owned SONAME.

    Heuristic at the index level (we cannot open the .deb offline): a library
    package whose name resolves to a base-owned soname stem. We compare the
    package's SONAME-shaped name (``libfoo1`` → ``libfoo.so.1`` family) against
    the base set by stem. Conservative — only fires for clear lib packages, so a
    false negative merely keeps payload in the estimate (safe over-count).
    """
    if not name.startswith("lib"):
        return False
    # base sonames look like "libfoo.so.1"; reduce to the stem "libfoo"
    stems = {s.split(".so", 1)[0] for s in base_sonames}
    # package "libfoo1" / "libfoo-1" → stem "libfoo"
    pkg_stem = name.rstrip("0123456789").rstrip("-")
    return pkg_stem in stems


def predict_catalog(
    catalog: tuple[CandidateApp, ...],
    index: dict[str, dict],
    *,
    base_sonames: set[str] | None = None,
) -> tuple[ClosurePrediction, ...]:
    """Predict every app in ``catalog`` against one shared parsed ``index``.

    Builds the provides map once (shared across apps) for determinism + speed.
    Pure / offline given an index.
    """
    provides_map = build_provides_map(index)
    return tuple(
        predict_app(app, index, provides_map=provides_map, base_sonames=base_sonames)
        for app in catalog
    )


@dataclass(frozen=True)
class CatalogSummary:
    """Roll-up across a catalog prediction run."""

    total: int
    installable: int
    blocked: int
    by_category: dict[str, int]
    by_kind: dict[str, int]
    total_download_bytes: int
    total_stage_bytes: int

    def as_dict(self) -> dict:
        return {
            "total": self.total,
            "installable": self.installable,
            "blocked": self.blocked,
            "by_category": dict(self.by_category),
            "by_kind": dict(self.by_kind),
            "total_download_bytes": self.total_download_bytes,
            "total_stage_bytes": self.total_stage_bytes,
        }


def summarize(predictions: tuple[ClosurePrediction, ...]) -> CatalogSummary:
    """Aggregate per-app predictions into a catalog roll-up (installable-only sizes)."""
    installable = [p for p in predictions if p.installable_host]
    by_cat: dict[str, int] = {}
    by_kind: dict[str, int] = {}
    for p in installable:
        by_cat[p.category] = by_cat.get(p.category, 0) + 1
        by_kind[p.kind] = by_kind.get(p.kind, 0) + 1
    return CatalogSummary(
        total=len(predictions),
        installable=len(installable),
        blocked=len(predictions) - len(installable),
        by_category=dict(sorted(by_cat.items())),
        by_kind=dict(sorted(by_kind.items())),
        total_download_bytes=sum(p.download_bytes for p in installable),
        total_stage_bytes=sum(p.stage_tar_bytes for p in installable),
    )


def _fmt_bytes(n: int) -> str:
    """Human MiB string (1 decimal) for report rows."""
    return f"{n / (1024 * 1024):.1f}MiB"


# --------------------------------------------------------------------------- #
# Synthetic fixture (OFFLINE) — exercises every prediction path
# --------------------------------------------------------------------------- #
# Mirrors a noble Packages slice: real libc, a virtual/Provides edge, a leaf with
# a satisfiable closure, and a leaf with an UNSATISFIABLE dep (to prove the
# 0-unsat verdict actually discriminates). Sizes are present so the size model is
# exercised. Names align with DEFAULT_CATALOG entries used by the selftest.
_FIXTURE_PACKAGES = """\
Package: libc6
Version: 2.39
Installed-Size: 13000
Size: 3100000
Filename: pool/main/g/glibc/libc6_2.39_arm64.deb

Package: libtinfo6
Version: 6.4
Installed-Size: 500
Size: 90000
Depends: libc6
Filename: pool/main/n/ncurses/libtinfo6_6.4_arm64.deb

Package: libncursesw6
Version: 6.4
Installed-Size: 600
Size: 110000
Depends: libc6, libtinfo6
Filename: pool/main/n/ncurses/libncursesw6_6.4_arm64.deb

Package: nano
Version: 7.2
Installed-Size: 2400
Size: 280000
Depends: libc6, libncursesw6, libtinfo6
Filename: pool/main/n/nano/nano_7.2_arm64.deb

Package: htop
Version: 3.3.0
Installed-Size: 900
Size: 180000
Depends: libc6, libncursesw6, libnl-3-virtual
Filename: pool/main/h/htop/htop_3.3.0_arm64.deb

Package: libnl-3-200
Version: 3.7
Installed-Size: 300
Size: 60000
Provides: libnl-3-virtual
Depends: libc6
Filename: pool/main/libn/libnl3/libnl-3-200_3.7_arm64.deb

Package: brokenapp
Version: 1.0
Installed-Size: 100
Size: 20000
Depends: libc6, libdoesnotexist-1
Filename: pool/main/b/brokenapp/brokenapp_1.0_arm64.deb
"""

# A tiny catalog over the fixture (so the selftest is self-contained).
_FIXTURE_CATALOG: tuple[CandidateApp, ...] = (
    CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI, "closure floor"),
    CandidateApp("htop", ("htop",), Category.SYSTEM, Kind.CLI, "virtual/Provides edge"),
    CandidateApp("brokenapp", ("brokenapp",), Category.UTILITY, Kind.CLI, "has unsat dep"),
    CandidateApp("ghost", ("totally-absent",), Category.UTILITY, Kind.CLI, "missing target"),
)


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        print(f"  [{status}] {label}")

    index = parse_packages(_FIXTURE_PACKAGES)
    preds = {p.app: p for p in predict_catalog(_FIXTURE_CATALOG, index)}

    # --- nano: clean closure, 0 unsat -------------------------------------- #
    nano = preds["nano"]
    check("nano installable (0 unsat)", nano.installable_host is True)
    check("nano unsatisfied_count == 0", nano.unsatisfied_count == 0)
    check("nano closure pulls libc6+ncurses+tinfo",
          {"nano", "libc6", "libncursesw6", "libtinfo6"} <= set(nano.closure))
    check("nano closure_size == 4", nano.closure_size == 4)
    check("nano download_bytes summed", nano.download_bytes == 280000 + 110000 + 90000 + 3100000)
    check("nano installed_bytes in bytes (KiB*1024)",
          nano.installed_bytes == (2400 + 600 + 500 + 13000) * 1024)
    check("nano stage < installed (prune applied)", 0 < nano.stage_tar_bytes < nano.installed_bytes)
    check("nano base_subtracted False without base set", nano.base_subtracted is False)
    check("nano category/kind serialised", nano.category == "editor" and nano.kind == "cli")

    # --- htop: resolves a virtual dep via Provides ------------------------- #
    htop = preds["htop"]
    check("htop installable (virtual dep satisfied via Provides)", htop.installable_host is True)
    check("htop closure includes real provider libnl-3-200", "libnl-3-200" in htop.closure)
    check("htop unsatisfied_count == 0", htop.unsatisfied_count == 0)

    # --- brokenapp: an unsatisfiable dep MUST be detected ------------------ #
    broken = preds["brokenapp"]
    check("brokenapp NOT installable (unsat dep)", broken.installable_host is False)
    check("brokenapp unsatisfied_count >= 1", broken.unsatisfied_count >= 1)
    check("brokenapp names the missing dep",
          any("libdoesnotexist-1" in u for u in broken.unsatisfied))

    # --- ghost: a wholly-missing target ------------------------------------ #
    ghost = preds["ghost"]
    check("ghost NOT installable (missing target)", ghost.installable_host is False)
    check("ghost reports missing target", "totally-absent" in ghost.missing_targets)
    check("ghost empty closure", ghost.closure_size == 0)

    # --- base subtraction toggles the estimate flag + shrinks payload ------ #
    # libc6 ships SONAME libc.so.6; mark it base-owned. Its package (libc6) maps
    # to stem "libc" → excluded from the size sums.
    base = {"libc.so.6", "libtinfo.so.6"}
    nano_sub = predict_app(_FIXTURE_CATALOG[0], index, base_sonames=base)
    check("base subtraction sets base_subtracted True", nano_sub.base_subtracted is True)
    check("base subtraction drops libc6 download bytes",
          nano_sub.download_bytes < nano.download_bytes)
    check("base subtraction still installable", nano_sub.installable_host is True)

    # --- summary roll-up ---------------------------------------------------- #
    all_preds = predict_catalog(_FIXTURE_CATALOG, index)
    summary = summarize(all_preds)
    check("summary total == 4", summary.total == 4)
    check("summary installable == 2 (nano, htop)", summary.installable == 2)
    check("summary blocked == 2 (broken, ghost)", summary.blocked == 2)
    check("summary by_kind counts CLI installables", summary.by_kind.get("cli") == 2)
    check("summary by_category has editor+system",
          summary.by_category.get("editor") == 1 and summary.by_category.get("system") == 1)

    # --- determinism -------------------------------------------------------- #
    again = {p.app: p for p in predict_catalog(_FIXTURE_CATALOG, index)}
    check("prediction deterministic across runs",
          again["nano"].closure == nano.closure and again["htop"].closure == htop.closure)

    # --- catalog hygiene (the real DEFAULT_CATALOG) ------------------------- #
    names = [a.name for a in DEFAULT_CATALOG]
    check("DEFAULT_CATALOG names unique", len(names) == len(set(names)))
    check("DEFAULT_CATALOG spans GUI and CLI",
          any(a.kind is Kind.GUI for a in DEFAULT_CATALOG)
          and any(a.kind is Kind.CLI for a in DEFAULT_CATALOG))
    check("DEFAULT_CATALOG every category is a Category enum",
          all(isinstance(a.category, Category) for a in DEFAULT_CATALOG))

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _render_table(predictions: tuple[ClosurePrediction, ...]) -> str:
    rows = [
        "| app | kind | category | closure | unsat | installable(host) | download | stage(est) | note |",
        "|-----|------|----------|--------:|------:|:-----------------:|---------:|-----------:|------|",
    ]
    for p in predictions:
        verdict = "YES" if p.installable_host else "NO"
        rows.append(
            f"| {p.app} | {p.kind} | {p.category} | {p.closure_size} | "
            f"{p.unsatisfied_count} | {verdict} | "
            f"{_fmt_bytes(p.download_bytes)} | {_fmt_bytes(p.stage_tar_bytes)} | {p.note} |"
        )
    return "\n".join(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="breadth_catalog",
        description="HOST prediction of apt-install closure resolvability for a catalog of apps "
                    "(reuses tools.deb_closure). 0-unsat = closure resolves; NOT a device-install proof "
                    "(terminal apt unpack rides the G1 exec-re-entry unlock).",
    )
    parser.add_argument("--selftest", action="store_true",
                        help="run the OFFLINE deterministic selftest and exit")
    parser.add_argument("--live", action="store_true",
                        help="fetch the real noble ports index and predict DEFAULT_CATALOG")
    parser.add_argument("--mirror", default=NOBLE_MIRROR)
    parser.add_argument("--suite", default=NOBLE_SUITE)
    parser.add_argument("--arch", default=NOBLE_ARCH)
    parser.add_argument("--component", action="append", dest="components",
                        help="repeatable; defaults to main+universe")
    parser.add_argument("--json", action="store_true", help="emit JSON instead of a table")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.live:
        parser.error("supply --selftest (offline) or --live (network noble index)")

    components = tuple(args.components) if args.components else NOBLE_COMPONENTS
    text = fetch_packages_index(args.mirror, args.suite, args.arch, components=components)
    index = parse_packages(text)
    predictions = predict_catalog(DEFAULT_CATALOG, index)
    summary = summarize(predictions)

    if args.json:
        print(json.dumps(
            {"predictions": [p.as_dict() for p in predictions], "summary": summary.as_dict()},
            indent=2, sort_keys=True,
        ))
    else:
        print(_render_table(predictions))
        print()
        print(f"installable(host): {summary.installable}/{summary.total}  "
              f"blocked: {summary.blocked}  "
              f"total download: {_fmt_bytes(summary.total_download_bytes)}  "
              f"total stage(est): {_fmt_bytes(summary.total_stage_bytes)}")
        print("NOTE: 0-unsat closure = HOST prediction; device install rides G1 exec-re-entry.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
