"""Tests for tools.build_dbus_overlay — the session-D-Bus daemon overlay (TASK-A2).

The overlay must stage /usr/bin/dbus-daemon (+ dbus-run-session) so the launcher's
gnome-platform shim can start a private session bus (the base ships only the libdbus
CLIENT). These tests pin the build LOGIC offline (the module selftest drives a synthetic
extracted-root through build_stage_tar) and the path-probe helper; a single network-gated
test builds the real noble overlay and asserts the daemon binary is present + conformant.
"""

import os

import pytest

from tools import build_dbus_overlay as bdo
from tools.build_dbus_overlay import (
    DBUS_DAEMON_PATH,
    DBUS_RUN_SESSION_PATH,
    DBUS_TARGETS,
    _has_path,
)


def test_module_selftest_passes():
    assert bdo._selftest() == 0


def test_targets_and_entrypoints_are_dbus():
    assert "dbus-daemon" in DBUS_TARGETS
    assert DBUS_DAEMON_PATH == "/usr/bin/dbus-daemon"
    assert DBUS_RUN_SESSION_PATH == "/usr/bin/dbus-run-session"


def test_has_path_is_slash_agnostic(tmp_path):
    import tarfile

    out = tmp_path / "x.tar"
    with tarfile.open(out, "w") as t:
        import io

        ti = tarfile.TarInfo("./usr/bin/dbus-daemon")
        ti.size = 3
        t.addfile(ti, io.BytesIO(b"ELF"))
    assert _has_path(out, "/usr/bin/dbus-daemon")
    assert _has_path(out, "usr/bin/dbus-daemon")
    assert not _has_path(out, "/usr/bin/absent")


@pytest.mark.skipif(
    os.environ.get("ALR_AUDIT_NET") != "1",
    reason="network-gated; set ALR_AUDIT_NET=1 to build the real noble dbus overlay",
)
def test_live_build_stages_dbus_daemon(tmp_path):
    import shutil
    from pathlib import Path

    if shutil.which("zstd") is None:
        pytest.skip("noble .debs are zstd; need the `zstd` CLI on PATH")
    base = Path(__file__).resolve().parents[1] / (
        "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
    )
    if not base.is_file():
        pytest.skip("base rootfs payload absent")
    out = tmp_path / "dbus-daemon-stage.tar"
    res = bdo.build_dbus_overlay(out, base, components=("main",), cache_dir="/tmp/deb-cache-ubuntu")
    assert res.daemon_present, "overlay must carry /usr/bin/dbus-daemon"
    assert not res.missing_soname, f"unresolved sonames: {res.missing_soname}"
    assert not res.violations, f"base-downgrade violations: {res.violations}"
    assert res.ok
