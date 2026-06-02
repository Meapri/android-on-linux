"""Host-verified tests for tools/apt_catalog.py.

Real-world-shaped Debian/Ubuntu ``Packages`` stanzas (RFC822-ish) exercise:
parsing of the package entry fields, Section→category mapping (incl. component
prefix + unknown fallback), Depends parsing (version constraints, alternatives,
arch qualifiers), Description split, KiB→byte size conversion, and the
PackageEntry → alr_manifest.AppManifest catalog synthesis (app_id sanitization,
category, summary, install size).
"""

from __future__ import annotations

import pytest

from tools.alr_manifest import (
    CATEGORY_DEVELOPMENT,
    CATEGORY_GRAPHICS,
    CATEGORY_INTERNET,
    CATEGORY_TERMINAL,
    CATEGORY_UTILITY,
    AppManifest,
    Catalog,
)
from tools.apt_catalog import (
    DependsAtom,
    PackageEntry,
    app_id_for_package,
    build_catalog_from_index,
    manifest_from_package,
    parse_depends_detailed,
    parse_package_index,
    section_to_category,
)


# --------------------------------------------------------------------------- #
# Fixtures — verbatim-shaped Packages stanzas (multi-component, continuations)
# --------------------------------------------------------------------------- #
PACKAGES_INDEX = """\
Package: foot
Architecture: arm64
Version: 1.16.2-1
Priority: optional
Section: x11
Installed-Size: 512
Depends: libc6 (>= 2.34), libfontconfig1 (>= 2.12.6), libutf8proc2
Filename: pool/universe/f/foot/foot_1.16.2-1_arm64.deb
Size: 184320
Description: fast, lightweight and minimalistic Wayland terminal emulator
 foot is a fast, lightweight and minimalistic Wayland terminal emulator.
 .
 It is independent of any particular toolkit.

Package: gimp
Architecture: arm64
Version: 2.10.36-3
Section: universe/graphics
Installed-Size: 21504
Depends: gimp-data, libgimp2.0 (>= 2.10.36), libc6 (>= 2.34)
Pre-Depends: dpkg (>= 1.15.6~)
Filename: pool/universe/g/gimp/gimp_2.10.36-3_arm64.deb
Size: 4194304
Description: GNU Image Manipulation Program
 The GIMP is an advanced picture editor.

Package: g++
Architecture: arm64
Version: 4:13.2.0-7
Section: devel
Installed-Size: 60
Depends: cpp (>= 4:13.2.0-7), gcc | gcc-13
Filename: pool/main/g/gcc-defaults/g++_13.2.0-7_arm64.deb
Size: 1100
Description: GNU C++ compiler

Package: firefox-esr
Architecture: arm64
Version: 115.0esr-1
Section: web
Installed-Size: 245760
Depends: libc6 (>= 2.34) [arm64], libgtk-3-0 | libgtk-4-1
Filename: pool/main/f/firefox-esr/firefox-esr_115.0esr-1_arm64.deb
Size: 62914560
Description: Mozilla Firefox web browser (ESR)

Package: weird-pkg
Architecture: arm64
Version: 0.1
Section: madeupsection
Filename: pool/universe/w/weird/weird-pkg_0.1_arm64.deb
Size: 10
Description: a package in an unknown section
"""


@pytest.fixture()
def index() -> dict[str, PackageEntry]:
    return parse_package_index(PACKAGES_INDEX)


# --------------------------------------------------------------------------- #
# Index / stanza parsing
# --------------------------------------------------------------------------- #
def test_parses_all_stanzas(index):
    assert set(index) == {"foot", "gimp", "g++", "firefox-esr", "weird-pkg"}


def test_package_entry_fields(index):
    foot = index["foot"]
    assert foot.version == "1.16.2-1"
    assert foot.section == "x11"
    assert foot.architecture == "arm64"
    assert foot.filename.endswith("foot_1.16.2-1_arm64.deb")
    assert foot.download_size == 184320
    assert foot.installed_size_kib == 512


def test_installed_size_kib_to_bytes(index):
    # Installed-Size is KiB per the index convention.
    assert index["foot"].installed_size_bytes == 512 * 1024


def test_description_split_summary_and_body(index):
    foot = index["foot"]
    assert foot.description == "fast, lightweight and minimalistic Wayland terminal emulator"
    # the ' .' continuation line becomes a blank line in the body
    assert "independent of any particular toolkit" in foot.long_description
    assert "\n\n" in foot.long_description  # blank line preserved


def test_short_description_only(index):
    assert index["g++"].description == "GNU C++ compiler"
    assert index["g++"].long_description == ""


# --------------------------------------------------------------------------- #
# Section → category mapping
# --------------------------------------------------------------------------- #
def test_section_known(index):
    assert index["foot"].category == CATEGORY_GRAPHICS   # x11
    assert index["gimp"].category == CATEGORY_GRAPHICS   # universe/graphics (component prefix)
    assert index["g++"].category == CATEGORY_DEVELOPMENT  # devel
    assert index["firefox-esr"].category == CATEGORY_INTERNET  # web


def test_section_component_prefix_stripped():
    assert section_to_category("universe/graphics") == CATEGORY_GRAPHICS
    assert section_to_category("restricted/net") == CATEGORY_INTERNET


def test_section_unknown_falls_back_to_utility(index):
    assert index["weird-pkg"].category == CATEGORY_UTILITY
    assert section_to_category("totally-made-up") == CATEGORY_UTILITY
    assert section_to_category("") == CATEGORY_UTILITY
    assert section_to_category(None) == CATEGORY_UTILITY


def test_section_shells_is_terminal():
    assert section_to_category("shells") == CATEGORY_TERMINAL


# --------------------------------------------------------------------------- #
# Depends parsing — constraints / alternatives / arch qualifiers
# --------------------------------------------------------------------------- #
def test_depends_groups_names_only(index):
    # closure form: deb_closure.parse_depends — names only, no constraints
    groups = index["gimp"].depends_groups
    flat = [name for g in groups for name in g]
    assert "dpkg" in flat          # from Pre-Depends
    assert "gimp-data" in flat
    assert "libgimp2.0" in flat
    assert "libc6" in flat


def test_depends_detailed_keeps_version_constraint():
    groups = parse_depends_detailed("libc6 (>= 2.34), libfontconfig1 (>= 2.12.6)")
    assert groups[0][0] == DependsAtom(name="libc6", constraint=">= 2.34")
    assert groups[1][0].constraint == ">= 2.12.6"


def test_depends_detailed_alternatives():
    groups = parse_depends_detailed("gcc | gcc-13")
    assert [a.name for a in groups[0]] == ["gcc", "gcc-13"]


def test_depends_detailed_arch_qualifier():
    groups = parse_depends_detailed("libc6 (>= 2.34) [arm64], libgtk-3-0 | libgtk-4-1")
    assert groups[0][0].arch == "arm64"
    assert groups[0][0].applies_to_arch is True
    assert [a.name for a in groups[1]] == ["libgtk-3-0", "libgtk-4-1"]


def test_depends_atom_arch_filter():
    assert DependsAtom("x", arch="arm64").applies_to_arch is True
    assert DependsAtom("x", arch="amd64").applies_to_arch is False
    assert DependsAtom("x", arch="!amd64").applies_to_arch is True   # negated, arm64 not excluded
    assert DependsAtom("x", arch="!arm64").applies_to_arch is False  # negated, arm64 excluded
    assert DependsAtom("x", arch="").applies_to_arch is True         # unqualified


def test_empty_depends_is_empty():
    assert parse_depends_detailed("") == []


# --------------------------------------------------------------------------- #
# PackageEntry → AppManifest synthesis
# --------------------------------------------------------------------------- #
def test_app_id_sanitization():
    assert app_id_for_package("foot") == "org.debian.foot"
    assert app_id_for_package("g++") == "org.debian.gplusplus"
    assert app_id_for_package("libgtk2.0") == "org.debian.libgtk2-0"
    # must be a valid AppManifest app_id (constructed manifest validates it)


def test_manifest_from_package_basic(index):
    m = manifest_from_package(index["foot"])
    assert isinstance(m, AppManifest)
    assert m.app_id == "org.debian.foot"
    assert m.name == "foot"
    assert m.category == CATEGORY_GRAPHICS
    assert m.summary.startswith("fast, lightweight")
    assert m.entry.kind == "exec"
    assert m.entry.target == "/usr/bin/foot"
    assert m.install_size_bytes == 512 * 1024


def test_manifest_with_stage_tar_ref(index):
    m = manifest_from_package(index["gimp"], stage_tar_ref="gimp-stage.tar")
    assert len(m.rootfs_deps) == 1
    dep = m.rootfs_deps[0]
    assert dep.kind == "stage-tar"
    assert dep.ref == "gimp-stage.tar"
    assert dep.stage_marker_stem == "gimp"
    assert dep.install_size_bytes == 21504 * 1024


def test_manifest_gplusplus_app_id_valid(index):
    # the tricky case: g++ must produce a schema-valid app_id (no '+'/'.')
    m = manifest_from_package(index["g++"])
    assert m.app_id == "org.debian.gplusplus"
    assert m.category == CATEGORY_DEVELOPMENT


def test_manifest_entry_path_override(index):
    m = manifest_from_package(index["gimp"], entry_path="/usr/bin/gimp-2.10")
    assert m.entry.target == "/usr/bin/gimp-2.10"


def test_summary_fallback_when_no_description():
    entry = PackageEntry(
        name="nodesc", version="1", section="utils", architecture="arm64",
        description="", long_description="", depends="", pre_depends="",
        filename="pool/n/nodesc.deb", download_size=1, installed_size_kib=1,
        raw={"Package": "nodesc"},
    )
    m = manifest_from_package(entry)
    assert m.summary == "nodesc (apt package)"


# --------------------------------------------------------------------------- #
# build_catalog_from_index
# --------------------------------------------------------------------------- #
def test_build_full_catalog():
    cat = build_catalog_from_index(PACKAGES_INDEX)
    assert isinstance(cat, Catalog)
    ids = {a.app_id for a in cat.apps}
    assert "org.debian.foot" in ids
    assert "org.debian.gplusplus" in ids
    assert len(cat.apps) == 5


def test_catalog_deterministic_order():
    a = build_catalog_from_index(PACKAGES_INDEX)
    b = build_catalog_from_index(PACKAGES_INDEX)
    assert [x.app_id for x in a.apps] == [x.app_id for x in b.apps]
    # ordered by source package NAME (not the synthesized app_id): g++ < gimp
    assert [x.name for x in a.apps] == sorted(x.name for x in a.apps)


def test_catalog_include_filter():
    cat = build_catalog_from_index(PACKAGES_INDEX, include=["foot", "gimp"])
    assert {a.name for a in cat.apps} == {"foot", "gimp"}


def test_catalog_section_filter():
    cat = build_catalog_from_index(PACKAGES_INDEX, sections=["graphics", "x11"])
    assert {a.name for a in cat.apps} == {"foot", "gimp"}


def test_catalog_section_filter_component_prefixed():
    # gimp's section is "universe/graphics"; filtering on "graphics" tail must match
    cat = build_catalog_from_index(PACKAGES_INDEX, sections=["graphics"])
    assert "gimp" in {a.name for a in cat.apps}


def test_catalog_categories_are_all_valid():
    # every synthesized manifest must carry a category in the closed enum
    cat = build_catalog_from_index(PACKAGES_INDEX)
    from tools.alr_manifest import CATEGORIES

    for app in cat.apps:
        assert app.category in CATEGORIES
