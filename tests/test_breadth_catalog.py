"""Tests for tools.breadth_catalog — HOST apt-install-ability prediction.

The breadth catalog reuses tools.deb_closure to resolve each app's runtime
dependency closure over the Ubuntu noble (ports) index and PREDICTS whether it
would `apt install` (0 unsatisfied deps) plus the staged-overlay size. These
tests pin the prediction *logic* on a fully OFFLINE synthetic index (no network,
deterministic): the 0-unsat verdict, virtual/Provides resolution, the
unsatisfiable-dep and missing-target negatives, the byte-level size model, the
base-soname subtraction, the category/kind taxonomy, and the summary roll-up.

A single LIVE test (against the real noble index) is gated behind
``ALR_BREADTH_NET=1`` so the default `uvx pytest` run stays offline/hermetic —
matching the project's network-gated convention (cf. ALR_V2_STAGING_NET).
"""

import os

import pytest

from tools.breadth_catalog import (
    Category,
    Kind,
    CandidateApp,
    ClosurePrediction,
    DEFAULT_CATALOG,
    NOBLE_COMPONENTS,
    NOBLE_MIRROR,
    NOBLE_SUITE,
    NOBLE_ARCH,
    _selftest,
    predict_app,
    predict_catalog,
    summarize,
)
from tools.deb_closure import parse_packages

# A self-contained noble-shaped index slice. Mirrors the module's internal
# fixture but lives here too so the tests are independent of module internals.
FIXTURE = """\
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


@pytest.fixture(scope="module")
def index():
    return parse_packages(FIXTURE)


# --------------------------------------------------------------------------- #
# closure resolution + 0-unsat verdict
# --------------------------------------------------------------------------- #
def test_clean_closure_is_installable(index):
    app = CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI)
    pred = predict_app(app, index)
    assert isinstance(pred, ClosurePrediction)
    assert pred.installable_host is True
    assert pred.unsatisfied_count == 0
    assert pred.missing_targets == ()
    # transitive closure resolved
    assert {"nano", "libc6", "libncursesw6", "libtinfo6"} <= set(pred.closure)
    assert pred.closure_size == len(pred.closure)


def test_virtual_dependency_resolved_via_provides(index):
    # htop depends on libnl-3-virtual, provided only by libnl-3-200.
    app = CandidateApp("htop", ("htop",), Category.SYSTEM, Kind.CLI)
    pred = predict_app(app, index)
    assert pred.installable_host is True
    assert pred.unsatisfied_count == 0
    assert "libnl-3-200" in pred.closure


def test_unsatisfiable_dependency_blocks_install(index):
    app = CandidateApp("brokenapp", ("brokenapp",), Category.UTILITY, Kind.CLI)
    pred = predict_app(app, index)
    assert pred.installable_host is False
    assert pred.unsatisfied_count >= 1
    assert any("libdoesnotexist-1" in line for line in pred.unsatisfied)


def test_missing_target_is_not_installable(index):
    app = CandidateApp("ghost", ("totally-absent",), Category.UTILITY, Kind.CLI)
    pred = predict_app(app, index)
    assert pred.installable_host is False
    assert "totally-absent" in pred.missing_targets
    assert pred.closure_size == 0


# --------------------------------------------------------------------------- #
# size model
# --------------------------------------------------------------------------- #
def test_size_model_sums_download_and_installed(index):
    app = CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI)
    pred = predict_app(app, index)
    # download = sum of Size over the 4-package closure
    assert pred.download_bytes == 280000 + 110000 + 90000 + 3100000
    # installed = sum of Installed-Size (KiB) * 1024
    assert pred.installed_bytes == (2400 + 600 + 500 + 13000) * 1024
    # staged estimate is shrunk by the prune heuristic, never larger than installed
    assert 0 < pred.stage_tar_bytes < pred.installed_bytes


def test_base_subtraction_drops_base_owned_payload_and_sets_flag(index):
    app = CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI)
    no_base = predict_app(app, index)
    # libc6 ships libc.so.6, libtinfo6 ships libtinfo.so.6 — mark both base-owned.
    with_base = predict_app(app, index, base_sonames={"libc.so.6", "libtinfo.so.6"})
    assert no_base.base_subtracted is False
    assert with_base.base_subtracted is True
    # the base-owned library packages are excluded from the size sums → smaller.
    assert with_base.download_bytes < no_base.download_bytes
    assert with_base.installed_bytes < no_base.installed_bytes
    # still installable — subtraction only changes the *size estimate*, not the verdict.
    assert with_base.installable_host is True


def test_base_subtraction_excludes_only_base_lib_packages(index):
    # nano itself is not a base soname → its own bytes must survive subtraction.
    app = CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI)
    with_base = predict_app(app, index, base_sonames={"libc.so.6"})
    # libc6 (3.1MB download) excluded; nano's own 280000 still counted.
    assert with_base.download_bytes == 280000 + 110000 + 90000
    assert "nano" in with_base.closure  # closure membership is unchanged


# --------------------------------------------------------------------------- #
# taxonomy
# --------------------------------------------------------------------------- #
def test_category_and_kind_serialize_to_strings(index):
    app = CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI)
    pred = predict_app(app, index)
    assert pred.category == "editor"
    assert pred.kind == "cli"
    # round-trips through as_dict for JSON output
    d = pred.as_dict()
    assert d["category"] == "editor" and d["kind"] == "cli"
    assert d["installable_host"] is True


def test_candidate_app_validates_inputs():
    with pytest.raises(ValueError):
        CandidateApp("", ("pkg",), Category.UTILITY, Kind.CLI)
    with pytest.raises(ValueError):
        CandidateApp("noPkgs", (), Category.UTILITY, Kind.CLI)
    with pytest.raises(TypeError):
        CandidateApp("badcat", ("pkg",), "utility", Kind.CLI)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        CandidateApp("badkind", ("pkg",), Category.UTILITY, "cli")  # type: ignore[arg-type]


# --------------------------------------------------------------------------- #
# catalog-level prediction + summary
# --------------------------------------------------------------------------- #
def test_predict_catalog_and_summary(index):
    catalog = (
        CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI),
        CandidateApp("htop", ("htop",), Category.SYSTEM, Kind.CLI),
        CandidateApp("brokenapp", ("brokenapp",), Category.UTILITY, Kind.CLI),
        CandidateApp("ghost", ("totally-absent",), Category.UTILITY, Kind.CLI),
    )
    preds = predict_catalog(catalog, index)
    assert len(preds) == 4
    summary = summarize(preds)
    assert summary.total == 4
    assert summary.installable == 2          # nano, htop
    assert summary.blocked == 2              # brokenapp (unsat), ghost (missing)
    assert summary.by_kind.get("cli") == 2
    assert summary.by_category.get("editor") == 1
    assert summary.by_category.get("system") == 1
    # installable-only size sums are positive
    assert summary.total_download_bytes > 0
    assert summary.total_stage_bytes > 0


def test_prediction_is_deterministic(index):
    catalog = (CandidateApp("nano", ("nano",), Category.EDITOR, Kind.CLI),)
    a = predict_catalog(catalog, index)
    b = predict_catalog(catalog, index)
    assert a[0].closure == b[0].closure
    assert a[0].as_dict() == b[0].as_dict()


# --------------------------------------------------------------------------- #
# the real DEFAULT_CATALOG is well-formed
# --------------------------------------------------------------------------- #
def test_default_catalog_hygiene():
    names = [a.name for a in DEFAULT_CATALOG]
    assert len(names) == len(set(names)), "catalog app names must be unique"
    assert any(a.kind is Kind.GUI for a in DEFAULT_CATALOG), "catalog must span GUI"
    assert any(a.kind is Kind.CLI for a in DEFAULT_CATALOG), "catalog must span CLI"
    assert all(isinstance(a.category, Category) for a in DEFAULT_CATALOG)
    assert all(a.packages for a in DEFAULT_CATALOG)


def test_module_selftest_passes(capsys):
    rc = _selftest()
    captured = capsys.readouterr()
    assert rc == 0, captured.out
    assert "ALL PASS" in captured.out


# --------------------------------------------------------------------------- #
# LIVE — real noble index (network-gated; off by default)
# --------------------------------------------------------------------------- #
@pytest.mark.skipif(
    os.environ.get("ALR_BREADTH_NET") != "1",
    reason="network-gated; set ALR_BREADTH_NET=1 to fetch the real noble ports index",
)
def test_live_default_catalog_resolves_against_noble():
    from tools.breadth_catalog import fetch_packages_index

    text = fetch_packages_index(
        NOBLE_MIRROR, NOBLE_SUITE, NOBLE_ARCH, components=NOBLE_COMPONENTS
    )
    idx = parse_packages(text)
    preds = predict_catalog(DEFAULT_CATALOG, idx)
    summary = summarize(preds)
    # every catalog app must at least RESOLVE (closure non-empty) — names valid.
    for p in preds:
        assert p.closure_size > 0, f"{p.app} resolved to an empty closure (bad pkg name?)"
    # the majority of the lightweight catalog should be 0-unsat installable.
    assert summary.installable >= summary.total - 1, (
        f"only {summary.installable}/{summary.total} installable: "
        f"{[p.app for p in preds if not p.installable_host]}"
    )
