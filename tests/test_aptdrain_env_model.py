"""Host tests for the §5 v2-apt-drain launch ENV contract (T2).

Verifies tools.aptdrain_env_model: the LD_PRELOAD chain (fakeroot FIRST, interpose
KEPT, absolute rootfs paths, de-duplicated, guest preloads preserved), the drain
env block (ALR_ROOTFS / FAKEROOT* guest env + ALR_FAKEROOT / ALR_REEXEC_INPROC app
gates), and the marker→drain decision (default-OFF, every prerequisite required).

Cross-checks the model against the two read-only loader sources it mirrors:
  * tools/build_fakeroot_overlay.device_cmd  (the fakeroot chain shape)
  * the interpose .so path hardcoded in runtime_report.cpp / alr_exec.cpp

All pure host logic — no aarch64 .so, no device, no fs writes.
"""

from __future__ import annotations

import pytest

from tools.aptdrain_env_model import (
    DRAIN_MARKER,
    FAKEROOT_GATE_ENV,
    FAKEROOT_REL,
    INTERPOSE_REL,
    REEXEC_GATE_ENV,
    chain_ld_preload,
    decide_drain_env,
    decide_drain_gate,
    resolve,
)

ROOTFS = "/data/data/dev.chanwoo.androlinux/files/rootfs"
FAKEROOT_SO = f"{ROOTFS}/{FAKEROOT_REL}"
INTERPOSE_SO = f"{ROOTFS}/{INTERPOSE_REL}"


# --------------------------------------------------------------------------- #
# 1. LD_PRELOAD chain
# --------------------------------------------------------------------------- #

def test_chain_fakeroot_first_interpose_kept():
    chain = chain_ld_preload(ROOTFS, fakeroot=True)
    assert chain == f"{FAKEROOT_SO}:{INTERPOSE_SO}"
    parts = chain.split(":")
    # fakeroot FIRST so its credential wrappers run outermost.
    assert parts[0] == FAKEROOT_SO
    # interpose MUST still be in the chain (path mediation never dropped).
    assert INTERPOSE_SO in parts


def test_chain_interpose_only_when_no_fakeroot():
    chain = chain_ld_preload(ROOTFS, fakeroot=False)
    assert chain == INTERPOSE_SO
    assert FAKEROOT_SO not in chain.split(":")


def test_chain_entries_are_absolute_rootfs_paths():
    chain = chain_ld_preload(ROOTFS, fakeroot=True)
    for entry in chain.split(":"):
        # R3: absolute rootfs host path, never a guest "/usr/..." path.
        assert entry.startswith(ROOTFS + "/"), entry
        assert entry.startswith("/")


def test_chain_preserves_guest_preloads_after_ours():
    guest = "/opt/thing/libfoo.so:/opt/thing/libbar.so"
    chain = chain_ld_preload(ROOTFS, fakeroot=True, existing=guest)
    parts = chain.split(":")
    assert parts == [
        FAKEROOT_SO,
        INTERPOSE_SO,
        "/opt/thing/libfoo.so",
        "/opt/thing/libbar.so",
    ]


def test_chain_is_idempotent_dedups_our_shims():
    # A child whose parent we already injected inherits our two shims in its
    # LD_PRELOAD; re-chaining must NOT duplicate them.
    already = f"{FAKEROOT_SO}:{INTERPOSE_SO}:/opt/x/libg.so"
    chain = chain_ld_preload(ROOTFS, fakeroot=True, existing=already)
    parts = chain.split(":")
    assert parts.count(FAKEROOT_SO) == 1
    assert parts.count(INTERPOSE_SO) == 1
    assert parts == [FAKEROOT_SO, INTERPOSE_SO, "/opt/x/libg.so"]


def test_chain_dedups_even_when_guest_listed_interpose_only():
    # Guest already had the interpose .so (the loader's first-launch env). Adding
    # fakeroot must put fakeroot first and keep interpose exactly once.
    chain = chain_ld_preload(ROOTFS, fakeroot=True, existing=INTERPOSE_SO)
    assert chain == f"{FAKEROOT_SO}:{INTERPOSE_SO}"


def test_chain_ignores_blank_and_whitespace_guest_entries():
    chain = chain_ld_preload(ROOTFS, fakeroot=True, existing="  : :/opt/x/y.so: ")
    assert chain == f"{FAKEROOT_SO}:{INTERPOSE_SO}:/opt/x/y.so"


def test_chain_normalises_trailing_slash_on_rootfs():
    a = chain_ld_preload(ROOTFS + "/", fakeroot=True)
    b = chain_ld_preload(ROOTFS, fakeroot=True)
    assert a == b
    assert "//" not in a


# --------------------------------------------------------------------------- #
# 2. Drain env block
# --------------------------------------------------------------------------- #

def test_drain_env_guest_block():
    env = decide_drain_env(ROOTFS, fakeroot=True, inproc_reexec=True)
    assert env.active
    g = env.guest_env()
    assert g["LD_PRELOAD"] == f"{FAKEROOT_SO}:{INTERPOSE_SO}"
    assert g["ALR_ROOTFS"] == ROOTFS
    assert g["FAKEROOTUID"] == "0"
    assert g["FAKEROOTGID"] == "0"


def test_drain_env_app_gates():
    env = decide_drain_env(ROOTFS, fakeroot=True, inproc_reexec=True)
    # The loader reads these from its OWN process env (getenv), not the guest's.
    assert env.app_env[FAKEROOT_GATE_ENV] == "1"
    assert env.app_env[REEXEC_GATE_ENV] == "1"
    # And they are NOT leaked into the guest env block.
    assert FAKEROOT_GATE_ENV not in env.guest_env()
    assert REEXEC_GATE_ENV not in env.guest_env()


def test_drain_env_inproc_can_be_disabled():
    env = decide_drain_env(ROOTFS, fakeroot=True, inproc_reexec=False)
    assert REEXEC_GATE_ENV not in env.app_env
    assert env.app_env[FAKEROOT_GATE_ENV] == "1"


def test_drain_env_interpose_only_has_no_fakeroot_identity():
    env = decide_drain_env(ROOTFS, fakeroot=False, inproc_reexec=True)
    g = env.guest_env()
    assert g["LD_PRELOAD"] == INTERPOSE_SO
    assert g["FAKEROOTUID"] == ""
    assert g["FAKEROOTGID"] == ""
    assert FAKEROOT_GATE_ENV not in env.app_env


def test_drain_env_no_rootfs_is_inactive():
    env = decide_drain_env("", fakeroot=True)
    assert not env.active
    assert env.reason == "no-rootfs"
    assert env.guest_env() == {}


def test_drain_env_rootfs_trailing_slash_normalised():
    env = decide_drain_env(ROOTFS + "/", fakeroot=True)
    assert env.alr_rootfs == ROOTFS
    assert "//" not in env.guest_env()["ALR_ROOTFS"]


# --------------------------------------------------------------------------- #
# 3. Marker → drain gate
# --------------------------------------------------------------------------- #

def _gate(**over):
    base = dict(
        program="/usr/bin/dpkg",
        marker_present=True,
        fakeroot_so_staged=True,
        interpose_so_staged=True,
    )
    base.update(over)
    return decide_drain_gate(**base)


def test_gate_armed_when_all_prereqs_hold():
    g = _gate()
    assert g.active
    assert g.reason == "drain-armed"


def test_gate_marker_absent_is_default_off():
    g = _gate(marker_present=False)
    assert not g.active
    assert g.reason == "marker-absent"


def test_gate_non_pkg_manager_not_armed_even_with_marker():
    # fakeroot must NOT be forced on a normal app launch even under the marker.
    g = _gate(program="/usr/bin/hello")
    assert not g.active
    assert g.reason == "not-pkg-manager"


@pytest.mark.parametrize(
    "program",
    ["dpkg", "/usr/bin/dpkg", "apt", "/usr/bin/apt-get", "dpkg-deb"],
)
def test_gate_matches_pkg_manager_family(program):
    assert _gate(program=program).active


def test_gate_matches_newline_joined_argv():
    # The JNI probe takes program as "argv0\n--flag\n…"; only argv0 names the bin.
    prog = "/usr/bin/dpkg\n--force-not-root\n-i\n/var/cache/apt/archives/hello.deb"
    assert _gate(program=prog).active


def test_gate_interpose_not_staged():
    g = _gate(interpose_so_staged=False)
    assert not g.active
    assert g.reason == "interpose-not-staged"


def test_gate_fakeroot_not_staged():
    g = _gate(fakeroot_so_staged=False)
    assert not g.active
    assert g.reason == "fakeroot-not-staged"


# --------------------------------------------------------------------------- #
# 4. resolve(): gate THEN env
# --------------------------------------------------------------------------- #

def test_resolve_armed_returns_full_env():
    env = resolve(
        ROOTFS,
        program="/usr/bin/dpkg",
        marker_present=True,
        fakeroot_so_staged=True,
        interpose_so_staged=True,
    )
    assert env.active
    assert env.reason == "fakeroot-drain"
    assert env.guest_env()["LD_PRELOAD"] == f"{FAKEROOT_SO}:{INTERPOSE_SO}"
    assert env.app_env[FAKEROOT_GATE_ENV] == "1"


def test_resolve_closed_gate_carries_gate_reason():
    env = resolve(
        ROOTFS,
        program="/usr/bin/dpkg",
        marker_present=False,
        fakeroot_so_staged=True,
        interpose_so_staged=True,
    )
    assert not env.active
    assert env.reason == "marker-absent"
    assert env.guest_env() == {}


# --------------------------------------------------------------------------- #
# 5. Cross-check against the read-only loader sources this model mirrors
# --------------------------------------------------------------------------- #

def test_marker_path_matches_ssot_convention():
    assert DRAIN_MARKER == "/data/local/tmp/.alr-aptdrain"


def test_chain_matches_build_fakeroot_overlay_device_cmd():
    """The model's chain order/shape must equal build_fakeroot_overlay's
    device_cmd (the other host source of the fakeroot chain). Both must put
    fakeroot FIRST and KEEP interpose."""
    from tools.build_fakeroot_overlay import device_cmd

    cmd = device_cmd(ROOTFS)
    # Both shims present, fakeroot first, interpose kept — exactly our chain.
    assert f"{FAKEROOT_SO}:{INTERPOSE_SO}" in cmd
    assert chain_ld_preload(ROOTFS, fakeroot=True) == f"{FAKEROOT_SO}:{INTERPOSE_SO}"
    # FAKEROOTUID/GID identity matches.
    assert "FAKEROOTUID=0" in cmd and "FAKEROOTGID=0" in cmd


def test_interpose_rel_matches_loader_hardcoded_path():
    """The interpose .so rootfs-relative path must match what the loader pushes
    (runtime_report.cpp L1633: <rootfs>/usr/lib/androlinux/libalr_interpose.so).
    Guards against the model drifting from the source it mirrors."""
    from pathlib import Path

    rr = (
        Path(__file__).resolve().parents[1]
        / "app/src/main/cpp/runtime_report.cpp"
    )
    text = rr.read_text(encoding="utf-8", errors="replace")
    assert f"/{INTERPOSE_REL}" in text or INTERPOSE_REL in text
