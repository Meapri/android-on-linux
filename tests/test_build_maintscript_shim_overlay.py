"""Tests for tools.build_maintscript_shim_overlay — the postinst neutralizer (TASK-A1).

The overlay ships no-op maintainer-script helpers (a stub debconf confmodule, policy-rc.d
returning 101, exit-0 stubs for ucf/update-rc.d/invoke-rc.d/deb-systemd-helper/
dpkg-reconfigure, and a dpkg-maintscript-helper stub) so the gnome-platform exit-73 cascade
(libpaper1 postinst exit 2, x11-common postinst exit 127, session-migration, and — the
pinned gnome-calculator blocker — appstream's `dpkg-maintscript-helper rm_conffile …`
preinst) instead exits 0 and `dpkg --configure` completes. These tests pin the stub set, the
§5-E shape, and the key behaviours: the stub confmodule drives the REAL libpaper1 postinst
body to exit 0, and the dpkg-maintscript-helper stub answers `supports`→0 / operations→0 and
drives the REAL noble appstream preinst body to exit 0.
"""

import os
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


# --------------------------------------------------------------------------------------- #
# dpkg-maintscript-helper stub — the appstream-class rm_conffile neutralizer.
# --------------------------------------------------------------------------------------- #

# Verbatim preinst of the REAL noble `appstream_1.0.2-1build6_arm64.deb` control archive
# (dh_installdeb/13.14.1ubuntu5). It is the pinned gnome-calculator install blocker: the
# `dpkg-maintscript-helper rm_conffile … -- "$@"` line is called UNCONDITIONALLY under
# `set -e`, so a non-operational helper aborts the preinst → dpkg exit 73 → closure cascade.
# Embedded as a literal so the test is offline/hermetic (no ports.ubuntu.com fetch).
_REAL_APPSTREAM_PREINST = (
    "#!/bin/sh\n"
    "set -e\n"
    "# Automatically added by dh_installdeb/13.14.1ubuntu5\n"
    'dpkg-maintscript-helper rm_conffile /etc/appstream.conf 1.0.0-1 -- "$@"\n'
    "# End automatically added section\n"
)


def _maintscript_helper(overlay):
    """The dpkg-maintscript-helper stub body shipped by the overlay."""
    _, _, _, bodies, _ = overlay
    return bodies["./usr/bin/dpkg-maintscript-helper"]


def _write_helper(tmp_path: Path, overlay) -> Path:
    p = tmp_path / "dpkg-maintscript-helper"
    p.write_bytes(_maintscript_helper(overlay))
    p.chmod(0o755)
    return p


def test_overlay_ships_dpkg_maintscript_helper_stub(overlay):
    _, _, names, _, modes = overlay
    assert "./usr/bin/dpkg-maintscript-helper" in names
    assert modes["./usr/bin/dpkg-maintscript-helper"] & 0o111, "must be executable"
    assert "usr/bin/dpkg-maintscript-helper" in STUB_FILES


def test_supports_returns_0_for_documented_commands(overlay, tmp_path):
    h = _write_helper(tmp_path, overlay)
    for cmd in ("rm_conffile", "mv_conffile", "symlink_to_dir", "dir_to_symlink"):
        r = subprocess.run([str(h), "supports", cmd], capture_output=True, timeout=10)
        assert r.returncode == 0, f"`supports {cmd}` must be 0 so the helper branch is taken"


def test_supports_returns_1_for_unknown_command(overlay, tmp_path):
    # Faithful to the real helper: `supports <unknown>` -> 1 (so a probing script branches
    # exactly as dpkg would, not into a path dpkg wouldn't take).
    h = _write_helper(tmp_path, overlay)
    r = subprocess.run([str(h), "supports", "no_such_command"],
                       capture_output=True, timeout=10)
    assert r.returncode == 1


@pytest.mark.parametrize(
    "argv",
    [
        ["rm_conffile", "/etc/appstream.conf", "1.0.0-1", "--", "configure"],
        ["mv_conffile", "/etc/a.conf", "/etc/b.conf", "1.0-1", "--", "configure"],
        ["symlink_to_dir", "/etc/foo", "bar", "1.0-1", "--", "configure"],
        ["dir_to_symlink", "/etc/foo", "bar", "1.0-1", "--", "configure"],
        ["finish_rm_conffile", "/etc/appstream.conf"],  # unknown first-arg → still 0
        ["totally_unknown_subcommand"],                  # never abort a set -e script
    ],
)
def test_every_operation_exits_0_with_no_side_effect(overlay, tmp_path, argv):
    h = _write_helper(tmp_path, overlay)
    # Pre-seed a conffile the stub must NOT touch (it skips, it does not emulate).
    victim = tmp_path / "appstream.conf"
    victim.write_text("keep me\n")
    r = subprocess.run([str(h), *argv], capture_output=True, timeout=10)
    assert r.returncode == 0, r.stderr.decode(errors="replace")
    assert victim.read_text() == "keep me\n", "stub must perform no file operation"


def test_stub_drives_real_appstream_preinst_to_exit_0(overlay, tmp_path):
    """THE pinned blocker: the verbatim noble appstream preinst (unconditional rm_conffile
    under `set -e`) must exit 0 with the stub on PATH (instead of the device exit-73 abort)."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_helper(bindir, overlay)  # ship the stub as `bin/dpkg-maintscript-helper`
    preinst = tmp_path / "appstream.preinst"
    preinst.write_text(_REAL_APPSTREAM_PREINST)
    env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ.get("PATH", ""))
    r = subprocess.run(["sh", str(preinst), "install"],
                       capture_output=True, timeout=10, env=env)
    assert r.returncode == 0, r.stderr.decode(errors="replace")


def test_real_appstream_preinst_aborts_without_a_working_helper(overlay, tmp_path):
    """Guards the premise: the SAME real preinst ABORTS (non-zero) when the helper on PATH is
    non-operational (the device condition the stub fixes). Proves the test isn't vacuous."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    broken = bindir / "dpkg-maintscript-helper"
    broken.write_text(
        "#!/bin/sh\n"
        'if [ "$1" = "rm_conffile" ]; then echo "helper: error" >&2; exit 1; fi\n'
        "exit 0\n"
    )
    broken.chmod(0o755)
    preinst = tmp_path / "appstream.preinst"
    preinst.write_text(_REAL_APPSTREAM_PREINST)
    env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ.get("PATH", ""))
    r = subprocess.run(["sh", str(preinst), "install"],
                       capture_output=True, timeout=10, env=env)
    assert r.returncode != 0, "premise check: the real preinst must abort with a broken helper"
