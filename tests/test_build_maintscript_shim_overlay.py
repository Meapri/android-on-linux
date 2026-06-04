"""Tests for tools.build_maintscript_shim_overlay — the postinst neutralizer (TASK-A1).

The overlay ships no-op maintainer-script helpers (a stub debconf confmodule, policy-rc.d
returning 101, and exit-0 stubs for ucf/update-rc.d/invoke-rc.d/deb-systemd-helper/
dpkg-reconfigure) so the gnome-platform exit-73 cascade (libpaper1 postinst exit 2,
x11-common postinst exit 127, session-migration) instead exits 0 and `dpkg --configure`
completes. These tests pin the stub set, the §5-E shape, and — the key behaviour — that the
stub confmodule drives the REAL libpaper1 postinst body to exit 0.
"""

import subprocess
import tarfile
from pathlib import Path

import pytest

from tools import build_maintscript_shim_overlay as bms
from tools.build_maintscript_shim_overlay import (
    STUB_FILES,
    build_maintscript_shim_overlay,
)
from tools.stage_tar_spec import validate_stage_tar


def test_module_selftest_passes():
    assert bms._selftest() == 0


@pytest.fixture(scope="module")
def overlay(tmp_path_factory):
    out = tmp_path_factory.mktemp("shim") / "maintscript-shim-stage.tar"
    res = build_maintscript_shim_overlay(out)
    with tarfile.open(out) as t:
        names = {m.name for m in t.getmembers()}
        bodies = {m.name: t.extractfile(m).read() for m in t.getmembers() if m.isfile()}
        modes = {m.name: m.mode for m in t.getmembers() if m.isfile()}
    return out, res, names, bodies, modes


def test_ships_the_full_stub_set(overlay):
    _, res, names, _, _ = overlay
    for rel in STUB_FILES:
        assert "./" + rel in names, f"missing stub {rel}"
    assert res.file_count == len(STUB_FILES)


def test_confmodule_defines_db_functions(overlay):
    _, _, _, bodies, _ = overlay
    cm = bodies["./usr/share/debconf/confmodule"]
    for fn in (b"db_get()", b"db_set()", b"db_purge()", b"db_input()", b"db_go()"):
        assert fn in cm, f"confmodule must define {fn!r}"


def test_policy_rc_d_denies_and_is_executable(overlay):
    _, _, _, bodies, modes = overlay
    assert b"exit 101" in bodies["./usr/sbin/policy-rc.d"]
    assert modes["./usr/sbin/policy-rc.d"] & 0o111, "policy-rc.d must be executable"


def test_all_members_dot_rooted_and_conformant(overlay):
    out, _, names, _, _ = overlay
    assert all(n.startswith("./") for n in names)
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


def test_stub_confmodule_drives_libpaper1_postinst_to_exit_0(overlay, tmp_path):
    # The device-proven exit-2 culprit: libpaper1's postinst sources the confmodule then
    # `set -e; db_get …`. With the stub confmodule that whole body must exit 0.
    _, _, _, bodies, _ = overlay
    cm = tmp_path / "confmodule"
    cm.write_bytes(bodies["./usr/share/debconf/confmodule"])
    etc = tmp_path / "etc"
    etc.mkdir()
    post = tmp_path / "libpaper1.postinst"
    post.write_text(
        "#!/bin/sh\n"
        f". {cm}\n"
        "set -e\n"
        'if [ "$1" ]; then\n'
        "  db_get libpaper/defaultpaper\n"
        f'  echo "$RET" > {etc}/papersize.dpkg-inst\n'
        f"  cp {etc}/papersize.dpkg-inst {etc}/papersize\n"
        "fi\n"
        "exit 0\n"
    )
    r = subprocess.run(["sh", str(post), "configure"], capture_output=True, timeout=10)
    assert r.returncode == 0, r.stderr.decode(errors="replace")
