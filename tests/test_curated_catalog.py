"""Host-verified tests for tools/curated_catalog.py.

추천셋 스키마(카테고리 enum 정합·중복 app_id 없음·apt 패키지명 문법·요약 비지
않음), device 증명 그룹 존재, AppManifest 변환(install_plan 인덱스 보강 포함),
install_plan 으로의 closure 해석(오프라인 픽스처)을 검증한다.
"""

from __future__ import annotations

import pytest

from tools.alr_manifest import (
    CATEGORIES,
    AppManifest,
    Catalog,
    _APT_PKG_NAME,
)
from tools.apt_catalog import app_id_for_package, parse_package_index
from tools.curated_catalog import (
    CURATED,
    DEVICE_PROVEN,
    POPULAR_LIGHTWEIGHT,
    CuratedApp,
    by_category,
    curated_packages,
    device_proven_packages,
    to_app_manifest,
    to_catalog,
)
from tools.install_plan import build_install_plan


# --------------------------------------------------------------------------- #
# 추천셋 스키마 — 닫힌 어휘 정합
# --------------------------------------------------------------------------- #
def test_curated_set_non_empty():
    assert len(CURATED) >= 12
    assert len(DEVICE_PROVEN) >= 6
    assert len(POPULAR_LIGHTWEIGHT) >= 6


def test_all_categories_in_closed_enum():
    for app in CURATED:
        assert app.category in CATEGORIES, f"{app.package}: {app.category}"


def test_all_package_names_valid_apt_grammar():
    for app in CURATED:
        assert _APT_PKG_NAME.fullmatch(app.package), app.package


def test_all_summaries_non_empty():
    for app in CURATED:
        assert app.summary.strip()


def test_no_duplicate_packages():
    names = [a.package for a in CURATED]
    assert len(names) == len(set(names))


def test_no_duplicate_app_ids():
    ids = [a.app_id for a in CURATED]
    assert len(ids) == len(set(ids)), "synthesized app_id collision in curated set"


def test_app_id_matches_apt_catalog_rule():
    for app in CURATED:
        assert app.app_id == app_id_for_package(app.package)


def test_binaries_are_absolute():
    for app in CURATED:
        assert app.entry_target.startswith("/")


def test_est_sizes_non_negative():
    for app in CURATED:
        assert app.est_install_mb >= 0
        assert app.est_install_bytes == app.est_install_mb * 1024 * 1024


# --------------------------------------------------------------------------- #
# CuratedApp 검증 가드
# --------------------------------------------------------------------------- #
def test_curated_app_rejects_bad_category():
    with pytest.raises(ValueError):
        CuratedApp(package="x", category="nope", summary="s", est_install_mb=1)


def test_curated_app_rejects_bad_package_name():
    with pytest.raises(ValueError):
        CuratedApp(package="Bad Name", category="utility", summary="s", est_install_mb=1)


def test_curated_app_rejects_empty_summary():
    with pytest.raises(ValueError):
        CuratedApp(package="x", category="utility", summary="  ", est_install_mb=1)


def test_curated_app_rejects_relative_binary():
    with pytest.raises(ValueError):
        CuratedApp(
            package="x", category="utility", summary="s",
            est_install_mb=1, binary="bin/x",
        )


def test_curated_app_rejects_negative_size():
    with pytest.raises(ValueError):
        CuratedApp(package="x", category="utility", summary="s", est_install_mb=-1)


# --------------------------------------------------------------------------- #
# device 증명 그룹
# --------------------------------------------------------------------------- #
def test_device_proven_contains_known_apps():
    names = set(device_proven_packages())
    # MEMORY device-proven set
    for expected in ("gimp", "foot", "netsurf-gtk"):
        assert expected in names


def test_device_proven_flag_consistent():
    proven = {a.package for a in CURATED if a.device_proven}
    assert proven == set(device_proven_packages())
    assert proven == {a.package for a in DEVICE_PROVEN}


def test_device_proven_have_notes():
    for app in DEVICE_PROVEN:
        assert app.note.strip(), f"{app.package} should carry a proof note"


# --------------------------------------------------------------------------- #
# 조회 / 그룹핑
# --------------------------------------------------------------------------- #
def test_curated_packages_listing():
    pkgs = curated_packages()
    assert "gimp" in pkgs
    assert "inkscape" in pkgs
    assert len(pkgs) == len(CURATED)


def test_by_category_groups_and_sorts_proven_first():
    groups = by_category()
    # every group's categories valid
    for cat, apps in groups.items():
        assert cat in CATEGORIES
        # device_proven entries sort before non-proven
        proven_flags = [a.device_proven for a in apps]
        # once a False appears, no later True (proven-first)
        seen_false = False
        for f in proven_flags:
            if not f:
                seen_false = True
            elif seen_false:
                pytest.fail(f"category {cat} not proven-first sorted")


def test_by_category_covers_all_apps():
    groups = by_category()
    total = sum(len(v) for v in groups.values())
    assert total == len(CURATED)


# --------------------------------------------------------------------------- #
# AppManifest 변환
# --------------------------------------------------------------------------- #
def test_to_app_manifest_basic():
    app = CURATED[0]
    m = to_app_manifest(app)
    assert isinstance(m, AppManifest)
    assert m.app_id == app.app_id
    assert m.category == app.category
    assert m.summary == app.summary
    assert m.entry.kind == "exec"
    assert m.entry.target == app.entry_target
    assert m.install_size_bytes == app.est_install_bytes


def test_to_catalog_valid_and_unique():
    cat = to_catalog()
    assert isinstance(cat, Catalog)
    assert len(cat.apps) == len(CURATED)  # no app_id collisions dropped any
    # Catalog.__post_init__ already enforces unique app_ids → construction proves it


def test_to_catalog_sorts_device_proven_first():
    cat = to_catalog()
    proven_ids = {app_id_for_package(p) for p in device_proven_packages()}
    # first len(proven) entries should all be proven
    n = len(proven_ids)
    first_n = {a.app_id for a in cat.apps[:n]}
    assert first_n == proven_ids


# --------------------------------------------------------------------------- #
# install_plan 해석 — 추천 패키지가 실제 closure 로 풀림
# --------------------------------------------------------------------------- #
# 작은 오프라인 인덱스: 추천 일부(galculator/nano)를 closure 가능하게 정의.
FIXTURE_INDEX = """\
Package: galculator
Version: 2.1.4-5
Architecture: arm64
Section: universe/utils
Depends: libgtk-3-0, libc6
Filename: pool/universe/g/galculator/galculator_2.1.4-5_arm64.deb
Size: 300000
Installed-Size: 900
Description: GTK calculator

Package: nano
Version: 7.2-1
Architecture: arm64
Section: editors
Depends: libc6, libncursesw6
Filename: pool/main/n/nano/nano_7.2-1_arm64.deb
Size: 280000
Installed-Size: 850
Description: small editor

Package: libgtk-3-0
Version: 3.24.41-1
Architecture: arm64
Filename: pool/main/g/gtk/libgtk-3-0_3.24.41-1_arm64.deb
Size: 2000000
Installed-Size: 8000
Description: GTK runtime

Package: libncursesw6
Version: 6.4-2
Architecture: arm64
Filename: pool/main/n/ncurses/libncursesw6_6.4-2_arm64.deb
Size: 100000
Installed-Size: 400
Description: ncurses
"""


@pytest.fixture()
def fixture_index():
    return parse_package_index(FIXTURE_INDEX)


def test_curated_package_resolves_via_install_plan(fixture_index):
    # 'galculator' is a curated package → install_plan resolves its closure
    plan = build_install_plan(["galculator"], fixture_index, base={"libc6"})
    names = {d.name for d in plan.debs}
    assert "galculator" in names
    assert "libgtk-3-0" in names  # transitive dep pulled
    assert "libc6" not in names    # base excluded


def test_to_app_manifest_uses_index_size_when_available(fixture_index):
    galc = next(a for a in CURATED if a.package == "galculator")
    entry = fixture_index["galculator"]
    m = to_app_manifest(galc, entry=entry)
    # index Installed-Size (900 KiB) overrides the est_install_mb estimate
    assert m.install_size_bytes == 900 * 1024


def test_to_catalog_with_index_enriches_sizes(fixture_index):
    cat = to_catalog(index=fixture_index)
    galc_id = app_id_for_package("galculator")
    m = cat.get(galc_id)
    assert m is not None
    assert m.install_size_bytes == 900 * 1024  # from index, not estimate


def test_to_catalog_with_index_keeps_unindexed_curated(fixture_index):
    # gimp isn't in the tiny fixture index → still present with its estimate
    cat = to_catalog(index=fixture_index)
    gimp = cat.get(app_id_for_package("gimp"))
    assert gimp is not None
    assert gimp.install_size_bytes > 0  # estimate retained


def test_curated_packages_feed_install_plan_keys():
    # the curated package list is exactly what install_plan would take as targets
    pkgs = curated_packages()
    assert all(_APT_PKG_NAME.fullmatch(p) for p in pkgs)
