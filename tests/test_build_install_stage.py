"""Host tests for tools/build_install_stage.py — the generalized install-AND-launch
stage-tar builder (v2 breadth; galculator-stage pattern applied to any catalog app).

What this proves (HOST, no device)
----------------------------------
build_install_stage generalizes the galculator staging recipe — closure overlay
PLUS the leaf ``.deb`` dropped at its apt-cache path — to any app in the
build_app_stage catalog. These tests assert that contract WITHOUT a network or a
device:

  * the builder is catalog-driven (htop/nano/xterm are real catalog entries with
    rootfs-absolute entrypoints) — the SSOT it generalizes galculator's recipe over;
  * ``inject_deb_into_cache`` (the shared galculator injector) lands the leaf .deb at
    ``var/cache/apt/archives/<deb>`` and is idempotent;
  * a stage-tar carrying both the entrypoint AND the .deb is §5-E conformant and
    reports ARM-READY; a libs-only tar (no entrypoint) or a BLOCK guard violation is
    NOT arm-ready;
  * (network-gated) each default app's full runtime closure resolves 0-unsat against
    the live noble index — the apt-installability precondition.

Honesty ceiling: ARM-READY is a host artifact (closure resolves + .deb at apt-cache
+ entrypoint present + §5-E conformant). On-device ``dpkg -i`` + launch rides the G1
exec-re-entry unlock AND a per-app MainActivity.aptDrainTargetFor entry — DEVICE-REQ
``ALR-V2-staged-apps``. These tests never claim a device install.
"""

from __future__ import annotations

import io
import socket
import tarfile
import urllib.error
import urllib.request
from pathlib import Path

import pytest

from tools.build_app_stage import APPS, overlay_has_path
from tools.build_galculator_stage import inject_deb_into_cache
from tools.build_install_stage import (
    InstallStageResult,
    _selftest,
    build_install_stage,
)

DEFAULT_APPS = ("htop", "nano", "xterm")


# --------------------------------------------------------------------------- #
# Catalog wiring — the builder generalizes over real catalog recipes
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize("name", DEFAULT_APPS)
def test_default_apps_are_real_catalog_entries(name):
    assert name in APPS
    assert APPS[name].entrypoint.startswith("/")
    assert APPS[name].packages


def test_htop_entrypoint_and_desktop():
    # htop is the new catalog entry this track added — exact paths from the noble deb.
    assert APPS["htop"].entrypoint == "/usr/bin/htop"
    assert APPS["htop"].desktop == "/usr/share/applications/htop.desktop"
    assert APPS["htop"].kind == "cli"


# --------------------------------------------------------------------------- #
# .deb injection at the apt-cache path (galculator-shape) + verdict gating
# --------------------------------------------------------------------------- #
def _overlay_with_entrypoint(tmp: Path, entrypoint: str = "/usr/bin/htop") -> Path:
    overlay = tmp / "stage.tar"
    with tarfile.open(overlay, "w") as t:
        payload = b"\x7fELF entrypoint"
        ti = tarfile.TarInfo("./" + entrypoint.lstrip("/"))
        ti.size = len(payload)
        ti.mode = 0o755
        t.addfile(ti, io.BytesIO(payload))
    return overlay


def test_inject_deb_lands_at_apt_cache_and_is_idempotent(tmp_path: Path):
    overlay = _overlay_with_entrypoint(tmp_path)
    fake_deb = tmp_path / "htop_3.3.0-4build1_arm64.deb"
    fake_deb.write_bytes(b"!<arch>\n" + b"deb" * 64)

    name, n = inject_deb_into_cache(overlay, fake_deb)
    assert name == "./var/cache/apt/archives/htop_3.3.0-4build1_arm64.deb"
    assert n == len(fake_deb.read_bytes())
    assert overlay_has_path(overlay, "var/cache/apt/archives/htop_3.3.0-4build1_arm64.deb")
    # the ./-stripped verdict path matches too (regression guard for the double-./ bug)
    assert overlay_has_path(overlay, name.lstrip("."))
    # entrypoint survives the append
    assert overlay_has_path(overlay, "/usr/bin/htop")

    # idempotent: re-injecting the same deb does not duplicate the member
    inject_deb_into_cache(overlay, fake_deb)
    with tarfile.open(overlay, "r") as t:
        cnt = sum(1 for m in t.getmembers()
                  if m.name.endswith("htop_3.3.0-4build1_arm64.deb"))
    assert cnt == 1


def _result(**kw) -> InstallStageResult:
    base = dict(
        app="htop", out_tar="x", deb_name="d", deb_cache_path="p", deb_bytes=1,
        entrypoint="/usr/bin/htop", entrypoint_present=True, desktop=None,
        desktop_present=False, deb_present=True, closure_size=1,
        reachable_libs=(), missing_soname=(), unsupported=(), violations=(),
        file_count=1,
    )
    base.update(kw)
    return InstallStageResult(**base)


def test_ok_requires_deb_entrypoint_no_unsat_no_block():
    assert _result().ok                                          # all good
    assert not _result(deb_present=False).ok                     # no install target
    assert not _result(entrypoint_present=False).ok              # no launch target
    assert not _result(missing_soname=("libx.so.1",)).ok         # unresolved DT_NEEDED
    assert not _result(violations=("BLOCK libfoo.so.1 downgrade",)).ok  # base downgrade
    # a non-BLOCK violation string does not by itself flip ok off
    assert _result(violations=("WARN something benign",)).ok


# --------------------------------------------------------------------------- #
# Offline selftest
# --------------------------------------------------------------------------- #
def test_selftest_passes():
    assert _selftest() == 0


# --------------------------------------------------------------------------- #
# NETWORK-GATED: live noble build produces an ARM-READY galculator-shape stage-tar
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
def test_live_htop_stage_is_arm_ready(tmp_path: Path):
    """End-to-end (network): build htop's install-AND-launch stage-tar against live
    noble — the .deb lands at its apt-cache path, the entrypoint ELF is present, the
    closure resolves with no missing soname, and the overlay is §5-E conformant
    against the base rootfs → ARM-READY."""
    base = Path("rootfs/tiny-rootfs.tar")
    if not base.is_file():
        pytest.skip("base rootfs/tiny-rootfs.tar not present")
    out_tar = tmp_path / "htop-stage.tar"
    res = build_install_stage(
        APPS["htop"], base, out_tar, cache_dir="/tmp/deb-cache-ubuntu",
    )
    assert res.deb_present, res.as_dict()
    assert res.entrypoint_present, res.as_dict()
    assert res.missing_soname == (), res.as_dict()
    # deb_cache_path is the ./-stripped member name → "/var/cache/apt/archives/..."
    # (leading slash; overlay_has_path is slash-agnostic, the install drain reads it
    # rootfs-relative).
    assert res.deb_cache_path.lstrip("/").startswith("var/cache/apt/archives/")
    assert res.deb_cache_path.endswith("_arm64.deb")
    assert res.ok, res.as_dict()
    # both payloads physically present
    assert overlay_has_path(out_tar, "/usr/bin/htop")
    assert overlay_has_path(out_tar, res.deb_cache_path)
