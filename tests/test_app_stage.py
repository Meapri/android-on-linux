"""Host tests for tools/build_app_stage.py (T1 — device-ready app stage-tars).

What this proves (HOST, no device)
----------------------------------
T1 turns the breadth catalog's 0-unsat lightweight picks (nano / xterm /
galculator) into REAL §5-E device-ready stage-tars. These tests assert the
contract behind "this app is device-staging ready":

  * the catalog recipes are well-formed and name the REAL packaged entrypoints;
  * the §5-E flatten + base-soname subtraction + structural conformance + the
    entrypoint-presence proof work end-to-end on a synthetic extracted root
    (OFFLINE — no network, no Debian tooling required);
  * ``AppStageBuild.ok`` ("STAGING-READY") correctly requires the launchable
    entrypoint to be in the tar (a libs-only overlay is NOT staging-ready);
  * (network-gated) each app's full runtime closure resolves with ZERO unsat
    against the live Ubuntu noble index — the apt-installability precondition.

Honesty ceiling: a STAGING-READY verdict is a host artifact (closure resolves +
overlay is conformant + entrypoint present). Real device install/launch rides the
G1 exec-re-entry unlock and is DEVICE-REQ — these tests never claim otherwise.
"""

import io
import os
import socket
import tarfile
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from tools.build_app_stage import (
    APPS,
    AppStage,
    AppStageBuild,
    build_app_stage_from_root,
    overlay_has_path,
)


# --------------------------------------------------------------------------- #
# Catalog / recipe invariants (pure)
# --------------------------------------------------------------------------- #
def test_catalog_apps_defined():
    assert set(APPS) == {"nano", "htop", "xterm", "galculator"}
    assert all(isinstance(a, AppStage) for a in APPS.values())


def test_entrypoints_are_rootfs_absolute_real_binaries():
    # rootfs-absolute, and the exact binary each leaf .deb installs (confirmed by
    # extracting the noble debs).
    assert APPS["nano"].entrypoint == "/usr/bin/nano"
    assert APPS["htop"].entrypoint == "/usr/bin/htop"
    assert APPS["xterm"].entrypoint == "/usr/bin/xterm"
    assert APPS["galculator"].entrypoint == "/usr/bin/galculator"
    for a in APPS.values():
        assert a.entrypoint.startswith("/")
        assert a.packages


def test_kind_and_desktop_consistency():
    # nano is CLI → no .desktop; htop is a CLI/ncurses TUI that still ships a
    # .desktop; the two GUI apps ship a real .desktop launcher.
    assert APPS["nano"].kind == "cli" and APPS["nano"].desktop is None
    assert APPS["htop"].kind == "cli"
    assert APPS["htop"].desktop == "/usr/share/applications/htop.desktop"
    assert APPS["xterm"].kind == "gui"
    assert APPS["xterm"].desktop == "/usr/share/applications/debian-xterm.desktop"
    assert APPS["galculator"].kind == "gui"
    assert APPS["galculator"].desktop == "/usr/share/applications/galculator.desktop"


def test_appstage_rejects_invalid_recipes():
    with pytest.raises(ValueError):
        AppStage("x", ("x",), "usr/bin/x", None, "cli")          # relative entrypoint
    with pytest.raises(ValueError):
        AppStage("x", ("x",), "/usr/bin/x", "rel.desktop", "gui")  # relative desktop
    with pytest.raises(ValueError):
        AppStage("x", ("x",), "/usr/bin/x", None, "tui")         # bad kind
    with pytest.raises(ValueError):
        AppStage("x", (), "/usr/bin/x", None, "cli")             # no package


# --------------------------------------------------------------------------- #
# overlay_has_path
# --------------------------------------------------------------------------- #
def test_overlay_has_path_present_absent_and_slash_agnostic(tmp_path: Path):
    tar_path = tmp_path / "t.tar"
    with tarfile.open(tar_path, "w") as t:
        payload = b"\x7fELFbin"
        ti = tarfile.TarInfo("./usr/bin/nano")
        ti.size = len(payload)
        ti.mode = 0o755
        t.addfile(ti, io.BytesIO(payload))
    assert overlay_has_path(tar_path, "/usr/bin/nano")
    assert overlay_has_path(tar_path, "usr/bin/nano")          # leading-slash agnostic
    assert not overlay_has_path(tar_path, "/usr/bin/missing")


# --------------------------------------------------------------------------- #
# OFFLINE build-from-root: §5-E flatten + entrypoint proof + base subtraction
# --------------------------------------------------------------------------- #
def _fixture_root(tmp: Path, app: AppStage) -> Path:
    """A synthetic extracted root mimicking the leaf-deb install: entrypoint
    binary + .desktop (if any) + a Debian-layout private lib to flatten."""
    root = tmp / f"root-{app.name}"
    binp = root / app.entrypoint.lstrip("/")
    binp.parent.mkdir(parents=True, exist_ok=True)
    binp.write_bytes(b"\x7fELF" + app.name.encode())
    binp.chmod(0o755)
    if app.desktop is not None:
        dp = root / app.desktop.lstrip("/")
        dp.parent.mkdir(parents=True, exist_ok=True)
        dp.write_text(f"[Desktop Entry]\nName={app.name}\nExec={app.entrypoint}\n")
    libdir = root / "usr/lib/aarch64-linux-gnu"
    libdir.mkdir(parents=True, exist_ok=True)
    real = libdir / f"lib{app.name}priv.so.2.0.0"
    real.write_bytes(b"PRIV" * 64)
    real.chmod(0o644)
    os.symlink(f"lib{app.name}priv.so.2.0.0", libdir / f"lib{app.name}priv.so.2")
    return root


@pytest.mark.parametrize("name", sorted(APPS))
def test_build_from_root_is_conformant_and_carries_entrypoint(name, tmp_path: Path):
    app = APPS[name]
    root = _fixture_root(tmp_path, app)
    out_tar = tmp_path / f"{name}-stage.tar"
    build = build_app_stage_from_root(app, root, out_tar)

    assert isinstance(build, AppStageBuild)
    assert build.conformant, build.violations
    assert build.entrypoint_present, f"{app.entrypoint} not staged"
    assert build.ok, "should be STAGING-READY"
    assert build.extract_estimate_bytes > 0

    members = set()
    with tarfile.open(out_tar, "r:*") as tar:
        members = set(tar.getnames())
    # §5-E flatten: Debian versioned lib → bare SONAME real file, versioned dropped.
    assert f"./usr/lib/aarch64-linux-gnu/lib{name}priv.so.2" in members
    assert f"./usr/lib/aarch64-linux-gnu/lib{name}priv.so.2.0.0" not in members
    # the entrypoint binary is a ./-rooted member
    assert "./" + app.entrypoint.lstrip("/") in members
    # .desktop presence matches the recipe
    if app.desktop is not None:
        assert build.desktop_present
        assert "./" + app.desktop.lstrip("/") in members
    else:
        assert not build.desktop_present


def test_missing_entrypoint_is_not_staging_ready(tmp_path: Path):
    """An overlay that stages data but NOT the launchable binary must be flagged
    NOT staging-ready — the whole point of the entrypoint proof."""
    app = AppStage("ghost", ("ghost",), "/usr/bin/ghost", None, "cli")
    root = tmp_path / "root-ghost"
    (root / "usr/share").mkdir(parents=True)
    (root / "usr/share/data.txt").write_text("no binary\n")
    out_tar = tmp_path / "ghost-stage.tar"
    build = build_app_stage_from_root(app, root, out_tar)
    assert not build.entrypoint_present
    assert not build.ok


def test_flat_over_flat_base_is_never_a_block(tmp_path: Path):
    """When the base already provides the same flat SONAME the overlay ships, it
    is a conformant flat-over-flat restore (WARN at most), never a BLOCK — proves
    base-subtraction is integrated, not just claimed."""
    app = APPS["nano"]
    root = _fixture_root(tmp_path, app)
    base_tar = tmp_path / "base.tar"
    with tarfile.open(base_tar, "w") as t:
        payload = b"OLD-PRIV" * 8
        ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libnanopriv.so.2")
        ti.size = len(payload)
        t.addfile(ti, io.BytesIO(payload))
    out_tar = tmp_path / "nano-vs-base.tar"
    build = build_app_stage_from_root(app, root, out_tar, base=base_tar)
    assert build.conformant
    assert build.entrypoint_present


# --------------------------------------------------------------------------- #
# AppStageBuild.ok aggregation
# --------------------------------------------------------------------------- #
def test_ok_aggregation():
    base = dict(
        app="x", out_tar="/tmp/x.tar", entrypoint="/usr/bin/x", desktop=None,
        file_count=3, closure_size=5, reachable_libs=("liba",),
        missing_soname=(), violations=(), unsupported=(),
        conformant=True, entrypoint_present=True, desktop_present=False,
    )
    assert AppStageBuild(**base).ok
    assert not AppStageBuild(**{**base, "conformant": False}).ok
    assert not AppStageBuild(**{**base, "violations": ("BLOCK foo",)}).ok
    assert not AppStageBuild(**{**base, "missing_soname": ("libz.so.9",)}).ok
    assert not AppStageBuild(**{**base, "entrypoint_present": False}).ok


def test_selftest_passes():
    from tools.build_app_stage import _selftest
    assert _selftest() == 0


# --------------------------------------------------------------------------- #
# NETWORK-GATED: live noble closure for each app resolves with 0 unsat
# --------------------------------------------------------------------------- #
def _noble_reachable() -> bool:
    try:
        req = urllib.request.Request(
            "http://ports.ubuntu.com/ubuntu-ports/dists/noble/main/binary-arm64/Packages.gz",
            headers={"User-Agent": "Debian APT-HTTP/1.3"},
        )
        with urllib.request.urlopen(req, timeout=8) as r:
            return bool(r.read(64))
    except (urllib.error.URLError, socket.timeout, OSError):
        return False


@pytest.mark.skipif(not _noble_reachable(), reason="ports.ubuntu.com noble index unreachable (offline)")
def test_live_closure_zero_unsat_for_each_app():
    """apt-installability precondition: every app's full runtime closure resolves
    against the live noble index with ZERO unsatisfied dep groups, and the leaf
    package itself is present in the index."""
    from tools.deb_closure import (
        build_provides_map,
        fetch_packages_index,
        parse_packages,
        resolve_closure,
    )

    mirror = "http://ports.ubuntu.com/ubuntu-ports"
    index = parse_packages(
        fetch_packages_index(mirror, "noble", "arm64", components=("main", "universe"))
    )
    provides_map = build_provides_map(index)

    for name, app in APPS.items():
        for pkg in app.packages:
            assert pkg in index, f"{name}: leaf package {pkg} not in noble index"
        log: list[str] = []
        closure = resolve_closure(list(app.packages), index, provides_map=provides_map, log=log)
        unsat = [l for l in log if l.startswith("unsatisfied dep group")]
        assert unsat == [], f"{name}: unsatisfied deps {unsat}"
        assert closure, f"{name}: empty closure"
