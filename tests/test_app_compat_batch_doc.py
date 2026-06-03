"""Drift guard for the batch app-compat coverage design doc.

`docs/research/app-compat-batch.md` maps ~35 common arm64 glibc apps onto the
five missing-stuff CLASSES (dlopen-plugin / data / font / locale / restricted
syscall) and the overlays this workflow builds. Because its whole value is being
*grounded* — every dlopen dir / data path / overlay it names must be real — these
lenient tests pin three failure modes without being brittle about prose:

  A. structure — the doc exists, has the class legend + the per-category app
     matrix + the roll-up + the class-S residue section.
  B. grounding in the real builders — the overlay builders it credits as the
     batch fix actually exist on disk in tools/ (so a GREEN verdict is anchored
     to a real, host-validated builder, not a wish).
  C. dlopen-path accuracy — the plugin dirs the doc cites match what the credited
     builders actually force-keep (NSS nss/ dir, babl-0.1/gegl-0.4 dirs, the
     gdk-pixbuf loaders dir), so a path can't silently rot.

These import nothing heavy and hit no network; they read text + check file
existence + a couple of substrings, mirroring tests/test_compat_matrix_citations.py.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "research" / "app-compat-batch.md"
TOOLS = ROOT / "tools"


def _doc_text() -> str:
    assert DOC.is_file(), f"batch app-compat doc missing: {DOC}"
    return DOC.read_text(encoding="utf-8")


# --------------------------------------------------------------------------- #
# A. structure
# --------------------------------------------------------------------------- #

def test_doc_has_class_legend_and_sections():
    text = _doc_text()
    # the five missing-stuff classes must all be defined
    for cls in (
        "dlopen plugin",   # P
        "runtime data",    # D
        "font",            # F
        "locale",          # L
        "restricted syscall",  # S
    ):
        assert cls.lower() in text.lower(), f"class definition missing: {cls!r}"
    # the three verdict buckets
    for verdict in ("GREEN", "YELLOW", "RED"):
        assert verdict in text, f"verdict bucket missing: {verdict}"
    # the per-category matrix headers + the roll-up
    for marker in ("Terminals", "Editors", "Browsers", "Media", "Dev tools",
                   "Roll-up", "Class S"):
        assert marker in text, f"section marker missing: {marker!r}"


def test_doc_states_host_only_honesty():
    """GREEN must be explicitly defined as overlay-covered, NOT a device pass."""
    text = _doc_text().lower()
    assert "host" in text and "not" in text and "device" in text, (
        "doc must carry the host-only honesty caveat (GREEN != device pass)"
    )
    # it must defer device truth to the WS-5 matrix, not claim it itself
    assert "alr-compat-matrix.md" in _doc_text(), (
        "doc must point device truth at the WS-5 matrix"
    )


def test_rollup_counts_are_present_and_consistent_with_buckets():
    """The roll-up names a count for each of GREEN/YELLOW/RED."""
    text = _doc_text()
    # the roll-up section explicitly tallies each bucket
    for bucket in ("GREEN:", "YELLOW:", "RED:"):
        assert bucket in text, f"roll-up missing a tally for {bucket}"


# --------------------------------------------------------------------------- #
# B. grounding — the credited batch builders exist
# --------------------------------------------------------------------------- #

def test_credited_existing_builders_exist():
    """Every overlay builder the doc credits as ALREADY-SHIPPED is real on disk."""
    text = _doc_text()
    for builder in (
        "build_nss_overlay.py",
        "build_babl_gegl_overlay.py",
        "build_locale_overlay.py",
        "build_chromium_net_overlay.py",
        "build_gui_overlay.py",
    ):
        assert builder in text, f"doc should credit existing builder {builder}"
        assert (TOOLS / builder).is_file(), (
            f"doc credits {builder} but it does not exist under {TOOLS}"
        )


def test_doc_reuses_the_keep_prefixes_mechanism():
    """The batch design must reuse the force-keep prefix mechanism (not a new engine)."""
    text = _doc_text()
    assert "keep_prefixes" in text, (
        "doc must specify reuse of build_minimal_overlay(keep_prefixes=...) — the "
        "force-keep mechanism that ships dlopen dirs with no DT_NEEDED edge"
    )
    # and the mechanism must actually be a real parameter in the reused engine
    closure = (TOOLS / "deb_closure.py").read_text(encoding="utf-8")
    assert "keep_prefixes" in closure, (
        "deb_closure.build_minimal_overlay no longer exposes keep_prefixes — "
        "the doc's batch mechanism is stale"
    )


# --------------------------------------------------------------------------- #
# C. dlopen-path accuracy — cited dirs match the real builders
# --------------------------------------------------------------------------- #

def test_nss_dlopen_dir_matches_builder():
    """The NSS plugin dir the doc cites must match build_nss_overlay.py."""
    text = _doc_text()
    nss = (TOOLS / "build_nss_overlay.py").read_text(encoding="utf-8")
    # the builder stages into .../nss/ ; the doc must name that dlopen dir
    assert "/nss/" in nss, "build_nss_overlay no longer uses the nss/ dir"
    assert "/nss/" in text, "doc must cite the NSS dlopen dir (.../nss/)"
    # the load-bearing module that caused the device FATAL
    assert "libsoftokn3.so" in nss and "libsoftokn3.so" in text


def test_babl_gegl_dirs_match_builder():
    text = _doc_text()
    bg = (TOOLS / "build_babl_gegl_overlay.py").read_text(encoding="utf-8")
    for d in ("babl-0.1", "gegl-0.4"):
        assert d in bg, f"build_babl_gegl_overlay no longer ships {d}"
        assert d in text, f"doc must cite the {d} dlopen dir"


def test_gdk_pixbuf_loaders_dir_matches_gui_overlay():
    """The gdk-pixbuf loaders dir + loaders.cache the doc widens must be the real one."""
    text = _doc_text()
    gui = (TOOLS / "build_gui_overlay.py").read_text(encoding="utf-8")
    assert "gdk-pixbuf-2.0/2.10.0/loaders" in gui, (
        "build_gui_overlay no longer references the gdk-pixbuf loaders dir"
    )
    assert "gdk-pixbuf-2.0/2.10.0/loaders" in text, (
        "doc must cite the gdk-pixbuf loaders dlopen dir it proposes to widen"
    )
    assert "loaders.cache" in text, (
        "doc must note the loaders.cache (the class-D companion of the loader dir)"
    )


def test_dt_needed_root_cause_is_grounded():
    """The doc must name the real BFS function that drops dlopen plugins."""
    text = _doc_text()
    assert "reachable_overlay_libs" in text, (
        "doc must cite the real DT_NEEDED BFS (deb_closure.reachable_overlay_libs) "
        "as the root cause of the dropped-plugin long-tail"
    )
    closure = (TOOLS / "deb_closure.py").read_text(encoding="utf-8")
    assert "def reachable_overlay_libs" in closure, (
        "deb_closure.reachable_overlay_libs no longer exists — doc citation stale"
    )
