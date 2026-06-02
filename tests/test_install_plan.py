"""Host-verified tests for tools/install_plan.py.

Real-world-shaped Packages stanzas drive: transitive closure resolution, base
exclusion (don't fetch what the rootfs already ships, and stop following its
sub-deps), cycle safety, first-alternative selection, alternative satisfied by
base, version-constraint tolerance, missing-dep tolerance, download/installed
size aggregation, and the stage-tar build instruction.
"""

from __future__ import annotations

import pytest

from tools.apt_catalog import parse_package_index
from tools.install_plan import (
    DebToFetch,
    InstallPlan,
    build_apt_transaction_v2,
    build_install_plan,
    resolve_install_closure,
    stage_tar_name,
)


# --------------------------------------------------------------------------- #
# Fixtures
# --------------------------------------------------------------------------- #
#  app → liba, libalt-meta
#  liba → libb, libc6(base)
#  libb → libcommon
#  libalt-meta → (provides nothing) Depends: libalt-real | libalt-fallback
#  libalt-real exists; libalt-fallback exists
#  libcommon → liba   (CYCLE: liba ↔ libb ↔ libcommon ↔ liba)
#  libc6 is in the base rootfs (excluded); base also satisfies "awk" virtual
PACKAGES_INDEX = """\
Package: app
Version: 1.0
Architecture: arm64
Section: utils
Depends: liba (>= 1.0), libalt-meta, missing-dep
Filename: pool/a/app_1.0_arm64.deb
Size: 1000
Installed-Size: 100
Description: the app

Package: liba
Version: 2.0
Architecture: arm64
Depends: libb, libc6 (>= 2.34)
Filename: pool/a/liba_2.0_arm64.deb
Size: 2000
Installed-Size: 200
Description: library a

Package: libb
Version: 3.0
Architecture: arm64
Depends: libcommon
Filename: pool/b/libb_3.0_arm64.deb
Size: 3000
Installed-Size: 300
Description: library b

Package: libcommon
Version: 4.0
Architecture: arm64
Depends: liba
Filename: pool/c/libcommon_4.0_arm64.deb
Size: 4000
Installed-Size: 400
Description: common (creates a cycle back to liba)

Package: libalt-meta
Version: 1.0
Architecture: arm64
Depends: libalt-real | libalt-fallback
Filename: pool/a/libalt-meta_1.0_arm64.deb
Size: 50
Installed-Size: 5
Description: chooses an alternative

Package: libalt-real
Version: 1.0
Architecture: arm64
Filename: pool/a/libalt-real_1.0_arm64.deb
Size: 60
Installed-Size: 6
Description: the first alternative

Package: libalt-fallback
Version: 1.0
Architecture: arm64
Filename: pool/a/libalt-fallback_1.0_arm64.deb
Size: 70
Installed-Size: 7
Description: the second alternative

Package: libc6
Version: 2.39
Architecture: arm64
Filename: pool/g/libc6_2.39_arm64.deb
Size: 9999
Installed-Size: 9999
Description: GNU C Library (base — should be excluded)
"""

# a virtual-package fixture for "alternative satisfied by base"
PROVIDES_INDEX = """\
Package: needs-awk
Version: 1.0
Architecture: arm64
Depends: gawk | mawk | original-awk
Filename: pool/n/needs-awk_1.0_arm64.deb
Size: 100
Installed-Size: 10
Description: wants any awk

Package: gawk
Version: 1.0
Architecture: arm64
Filename: pool/g/gawk_1.0_arm64.deb
Size: 500
Installed-Size: 50
Description: GNU awk

Package: mawk
Version: 1.0
Architecture: arm64
Provides: awk
Filename: pool/m/mawk_1.0_arm64.deb
Size: 80
Installed-Size: 8
Description: a small awk (base ships this)
"""


@pytest.fixture()
def index():
    return parse_package_index(PACKAGES_INDEX)


@pytest.fixture()
def provides_index():
    return parse_package_index(PROVIDES_INDEX)


# --------------------------------------------------------------------------- #
# closure resolution
# --------------------------------------------------------------------------- #
def test_transitive_closure(index):
    # base excludes libc6; cycle through libcommon must terminate.
    closure = resolve_install_closure(["app"], index, base={"libc6"})
    assert set(closure) == {"app", "liba", "libb", "libcommon", "libalt-meta", "libalt-real"}


def test_closure_excludes_base(index):
    closure = resolve_install_closure(["app"], index, base={"libc6"})
    assert "libc6" not in closure


def test_closure_is_cycle_safe(index):
    # libcommon → liba → libb → libcommon is a cycle; must not loop / dup
    closure = resolve_install_closure(["app"], index, base={"libc6"})
    assert len(closure) == len(set(closure))


def test_closure_first_alternative(index):
    # libalt-meta Depends: libalt-real | libalt-fallback → first wins
    closure = resolve_install_closure(["app"], index, base={"libc6"})
    assert "libalt-real" in closure
    assert "libalt-fallback" not in closure


def test_closure_missing_dep_is_non_fatal(index):
    log: list[str] = []
    closure = resolve_install_closure(["app"], index, base={"libc6"}, log=log)
    assert "app" in closure
    assert any("missing-dep" in line for line in log)


def test_closure_order_is_bfs_stable(index):
    a = resolve_install_closure(["app"], index, base={"libc6"})
    b = resolve_install_closure(["app"], index, base={"libc6"})
    assert a == b
    assert a[0] == "app"  # target first (BFS root)


def test_base_target_is_excluded(index):
    # choosing libc6 itself (already base) yields nothing to install
    log: list[str] = []
    closure = resolve_install_closure(["libc6"], index, base={"libc6"}, log=log)
    assert closure == []
    assert any("already in base" in line for line in log)


def test_base_stops_subtree(index):
    # if liba is base, its sub-deps (libb, libcommon) must not be pulled
    closure = resolve_install_closure(["app"], index, base={"libc6", "liba"})
    assert "liba" not in closure
    assert "libb" not in closure
    assert "libcommon" not in closure
    assert "app" in closure


# --------------------------------------------------------------------------- #
# alternative satisfied by base (Provides)
# --------------------------------------------------------------------------- #
def test_alternative_satisfied_by_base_real_pkg(provides_index):
    # base ships gawk → "gawk | mawk | original-awk" stops at gawk, nothing fetched
    closure = resolve_install_closure(["needs-awk"], provides_index, base={"gawk"})
    assert closure == ["needs-awk"]
    assert "mawk" not in closure


def test_alternative_satisfied_by_base_via_provides(provides_index):
    # base provides the virtual "awk" via mawk → first alt gawk NOT in base,
    # but the group is satisfied by a base provider → fetch nothing extra
    closure = resolve_install_closure(
        ["needs-awk"], provides_index, base={"mawk"}
    )
    assert "gawk" not in closure   # don't fetch gawk when base mawk satisfies awk-group
    assert closure == ["needs-awk"]


def test_alternative_first_when_base_has_none(provides_index):
    closure = resolve_install_closure(["needs-awk"], provides_index, base=set())
    assert "gawk" in closure       # first alternative
    assert "mawk" not in closure


# --------------------------------------------------------------------------- #
# InstallPlan + size aggregation
# --------------------------------------------------------------------------- #
def test_build_install_plan_shape(index):
    plan = build_install_plan(["app"], index, base={"libc6"})
    assert isinstance(plan, InstallPlan)
    assert plan.root_packages == ("app",)
    names = {d.name for d in plan.debs}
    assert names == {"app", "liba", "libb", "libcommon", "libalt-meta", "libalt-real"}
    assert all(isinstance(d, DebToFetch) for d in plan.debs)


def test_plan_download_size_sum(index):
    plan = build_install_plan(["app"], index, base={"libc6"})
    # 1000 + 2000 + 3000 + 4000 + 50 + 60
    assert plan.download_size == 1000 + 2000 + 3000 + 4000 + 50 + 60


def test_plan_installed_size_sum(index):
    plan = build_install_plan(["app"], index, base={"libc6"})
    # KiB→bytes: (100+200+300+400+5+6) * 1024
    assert plan.installed_size == (100 + 200 + 300 + 400 + 5 + 6) * 1024


def test_plan_excludes_base_from_size(index):
    plan = build_install_plan(["app"], index, base={"libc6"})
    # libc6's huge size (9999) must not appear in either total
    assert all(d.name != "libc6" for d in plan.debs)
    assert plan.download_size < 9999 + 10000  # libc6 not summed in


def test_plan_stage_tar_name(index):
    plan = build_install_plan(["app"], index, base={"libc6"})
    assert plan.stage_tar == "app-stage.tar"


def test_plan_stage_tar_override(index):
    plan = build_install_plan(["app"], index, base={"libc6"}, stage_tar="bundle.tar")
    assert plan.stage_tar == "bundle.tar"


def test_plan_fetch_filenames(index):
    plan = build_install_plan(["app"], index, base={"libc6"})
    fns = plan.fetch_filenames
    assert "pool/a/app_1.0_arm64.deb" in fns
    assert all(f.endswith(".deb") for f in fns)
    assert "pool/g/libc6_2.39_arm64.deb" not in fns  # base excluded


def test_plan_excluded_base_targets_recorded(index):
    plan = build_install_plan(["app", "libc6"], index, base={"libc6"})
    assert "libc6" in plan.excluded_base


def test_plan_as_dict_roundtrip(index):
    plan = build_install_plan(["app"], index, base={"libc6"})
    d = plan.as_dict()
    assert d["stage_tar"] == "app-stage.tar"
    assert d["download_size"] == plan.download_size
    assert d["installed_size"] == plan.installed_size
    assert len(d["debs"]) == len(plan.debs)


def test_stage_tar_name_helper():
    assert stage_tar_name("gimp") == "gimp-stage.tar"
    assert stage_tar_name("firefox-esr") == "firefox-esr-stage.tar"


# --------------------------------------------------------------------------- #
# v2 transition hook
# --------------------------------------------------------------------------- #
def test_v2_hook_is_not_implemented():
    with pytest.raises(NotImplementedError):
        build_apt_transaction_v2(["app"])
