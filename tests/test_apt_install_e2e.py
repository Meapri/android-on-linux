"""Host tests for the full `apt install <pkg>` chain gate model — T2.

These tests pin the stage-by-stage ALR-gate classification of
:mod:`tools.apt_install_e2e_model`:

  * each stage's gate (PURE / G2-fakeroot / G1-exec-re-entry) and proc;
  * which stages are cleared by fakeroot ALONE vs which still require the G1
    exec-re-entry re-map;
  * the central honest conclusion (SSOT §6 concl.2): a compressed-data package's
    `unpacked=true` transitively requires a G1 stage (the compressor child), so
    "unpack is single-process / G1-independent" is FALSE — while the metadata
    stages (chown/chmod/stat) are G2 and host-proven;
  * the hello.deb (no maintainer scripts → one G1 stage) vs script-bearing
    package (a second /bin/sh configure G1 stage) branch;
  * the host-provable vs device-pending partition equals the G1 set exactly;
  * the cross-link to dpkg_unpack_model that the G2 meta-stages are HOST-proven.
"""

from __future__ import annotations

from tools.apt_install_e2e_model import (
    PROFILE_HELLO,
    PROFILE_HELLO_DPKG_I,
    PROFILE_WITH_DEVNODE,
    PROFILE_WITH_SCRIPTS,
    Entry,
    Gate,
    PackageProfile,
    Proc,
    build_chain,
    decide,
    metadata_stages_host_proof,
)


# --------------------------------------------------------------------------- #
# (0) the chain is a well-formed dependency DAG
# --------------------------------------------------------------------------- #

def test_chain_is_a_well_formed_dag():
    for profile in (PROFILE_HELLO, PROFILE_WITH_SCRIPTS, PROFILE_WITH_DEVNODE):
        stages = profile.chain()
        ids = {s.id for s in stages}
        assert len(ids) == len(stages), f"{profile.name}: duplicate stage id"
        for s in stages:
            assert s.id not in s.depends_on, f"{profile.name}: {s.id} self-loop"
            for d in s.depends_on:
                assert d in ids, f"{profile.name}: {s.id} -> missing {d}"


def test_stage_ids_unique_and_ordered_dependencies_precede():
    """Every dependency must appear earlier in the ordered list — the chain is a
    valid topological order, so the device drain can walk it front-to-back."""
    stages = PROFILE_HELLO.chain()
    seen: set[str] = set()
    for s in stages:
        for d in s.depends_on:
            assert d in seen, f"{s.id} depends on {d} which comes later"
        seen.add(s.id)


# --------------------------------------------------------------------------- #
# (1) per-stage gate classification (the decision table)
# --------------------------------------------------------------------------- #

def test_apt_index_and_fetch_are_pure():
    stages = {s.id: s for s in PROFILE_HELLO.chain()}
    assert stages["apt_index"].gate is Gate.PURE
    assert stages["apt_fetch"].gate is Gate.PURE
    assert stages["apt_fetch"].proc is Proc.NETWORK


def test_apt_spawning_dpkg_is_a_g1_exec_reentry():
    """apt fork+execs dpkg — the first guest child the loader must re-map."""
    stages = {s.id: s for s in PROFILE_HELLO.chain()}
    spawn = stages["apt_spawn_dpkg"]
    assert spawn.gate is Gate.G1
    assert spawn.proc is Proc.FORK_EXEC


def test_superuser_gate_chown_chmod_are_g2_fakeroot():
    stages = {s.id: s for s in PROFILE_HELLO.chain()}
    assert stages["dpkg_superuser_gate"].gate is Gate.G2
    assert stages["dpkg_chown"].gate is Gate.G2
    assert stages["dpkg_chmod_stat"].gate is Gate.G2
    # all in dpkg's own process (no child) — the fakeroot LD_PRELOAD slice
    for sid in ("dpkg_superuser_gate", "dpkg_chown", "dpkg_chmod_stat"):
        assert stages[sid].proc is Proc.IN_PROC


def test_compressed_data_extract_is_a_g1_fork_exec():
    """The crux: a compressed data.tar forks a zstd/xz compressor child = G1."""
    stages = {s.id: s for s in PROFILE_HELLO.chain()}
    extract = stages["dpkg_extract_data"]
    assert extract.gate is Gate.G1
    assert extract.proc is Proc.FORK_EXEC


# --------------------------------------------------------------------------- #
# (2) fakeroot-alone vs G1: which stages each gate clears
# --------------------------------------------------------------------------- #

def test_fakeroot_alone_clears_pure_and_g2_but_not_g1():
    for s in PROFILE_HELLO.chain():
        if s.gate in (Gate.PURE, Gate.G2):
            assert s.cleared_by_fakeroot_alone is True, s.id
        else:  # G1
            assert s.cleared_by_fakeroot_alone is False, s.id


def test_metadata_stages_are_all_cleared_by_fakeroot_alone():
    """The §7 mid-sub-gate: getuid/chown/chmod/stat are satisfiable by fakeroot
    with NO exec-re-entry (they run in dpkg's own process)."""
    dec = decide(PROFILE_HELLO)
    g2 = [s for s in dec.stages if s.gate is Gate.G2]
    assert g2, "expected G2 meta-stages"
    assert all(s.cleared_by_fakeroot_alone for s in g2)


# --------------------------------------------------------------------------- #
# (3) the central honest conclusion — unpacked=true needs G1
# --------------------------------------------------------------------------- #

def test_hello_unpacked_requires_g1_because_data_is_compressed():
    """SSOT §6 concl.2: for a .zst package the terminal `unpacked=true` is reached
    only THROUGH the compressor child → it transitively requires a G1 stage. So
    'unpack is single-process / G1-independent' is FALSE."""
    dec = decide(PROFILE_HELLO)
    assert dec.unpacked_requires_g1 is True


def test_hello_via_apt_has_two_g1_stages():
    """Via `apt install`, hello rides TWO G1 stages: apt forking dpkg AND the
    data.tar compressor child."""
    dec = decide(PROFILE_HELLO)
    assert dec.g1_stage_count == 2
    assert set(dec.g1_stages) == {"apt_spawn_dpkg", "dpkg_extract_data"}


def test_hello_via_dpkg_i_has_exactly_one_g1_stage_the_extract_child():
    """Via `dpkg -i <local.deb>` (the SSOT §5 device-drain form), the apt outer
    stages drop out, so the ONLY G1 stage is the compressor child."""
    dec = decide(PROFILE_HELLO_DPKG_I)
    assert dec.g1_stage_count == 1
    assert dec.g1_stages == ("dpkg_extract_data",)
    # and there is no apt outer stage at all
    ids = {s.id for s in dec.stages}
    assert "apt_spawn_dpkg" not in ids
    assert "apt_index" not in ids
    # the terminal `unpacked=true` still requires that one G1 stage
    assert dec.unpacked_requires_g1 is True


def test_uncompressed_data_no_scripts_via_dpkg_i_needs_zero_g1():
    """If a package had an UNCOMPRESSED data.tar and no scripts AND is installed via
    `dpkg -i` (no apt child), dpkg reads it in-process — then `unpacked=true` is
    G1-independent (the one case where the 'single-process' intuition holds).
    Proves the gate model discriminates on the real cause (the compressor child),
    not a blanket assumption."""
    dec = decide(PackageProfile(
        "uncompressed", has_compressed_data=False, has_maintainer_scripts=False,
        entry=Entry.DPKG_I))
    assert dec.g1_stage_count == 0
    assert dec.unpacked_requires_g1 is False
    extract = next(s for s in dec.stages if s.id == "dpkg_extract_data")
    assert extract.gate is Gate.PURE and extract.proc is Proc.IN_PROC


# --------------------------------------------------------------------------- #
# (4) hello (no scripts) vs script-bearing package branch
# --------------------------------------------------------------------------- #

def test_hello_has_no_maintainer_script_stages():
    ids = {s.id for s in PROFILE_HELLO.chain()}
    assert "dpkg_run_preinst" not in ids
    assert "dpkg_run_postinst" not in ids


def test_hello_configure_is_pure_no_shell_child():
    stages = {s.id: s for s in PROFILE_HELLO.chain()}
    assert "dpkg_configure_noscript" in stages
    cfg = stages["dpkg_configure_noscript"]
    assert cfg.gate is Gate.PURE
    assert cfg.proc is Proc.IN_PROC


def test_scripted_package_adds_a_second_g1_stage():
    """A package with preinst/postinst forks /bin/sh at configure → a SECOND G1
    stage beyond the extract child, and `installed=true` then needs G1."""
    hello = decide(PROFILE_HELLO)
    scripted = decide(PROFILE_WITH_SCRIPTS)
    assert scripted.g1_stage_count > hello.g1_stage_count
    assert scripted.installed_requires_g1 is True


def test_scripted_maintainer_stages_fork_shell():
    stages = {s.id: s for s in PROFILE_WITH_SCRIPTS.chain()}
    for sid in ("dpkg_run_preinst", "dpkg_run_postinst"):
        assert sid in stages
        assert stages[sid].proc is Proc.FORK_EXEC
        assert stages[sid].gate is Gate.G1
        assert stages[sid].optional is True


def test_hello_installed_still_requires_g1_via_the_extract_child():
    """Even hello's `installed=true` transitively requires a G1 stage — not from a
    configure script (it has none) but because reaching Unpacked first crossed the
    extract child. So the whole chain to `installed` rides at least one G1 stage."""
    dec = decide(PROFILE_HELLO)
    assert dec.installed_requires_g1 is True


# --------------------------------------------------------------------------- #
# (5) host-provable vs device-pending partition == the G1 set
# --------------------------------------------------------------------------- #

def test_device_pending_stages_are_exactly_the_g1_stages():
    for profile in (PROFILE_HELLO, PROFILE_WITH_SCRIPTS):
        dec = decide(profile)
        assert set(dec.device_pending_stages) == set(dec.g1_stages)


def test_host_and_device_partition_is_a_clean_split():
    for profile in (PROFILE_HELLO, PROFILE_WITH_SCRIPTS, PROFILE_WITH_DEVNODE):
        dec = decide(profile)
        host = set(dec.host_provable_stages)
        dev = set(dec.device_pending_stages)
        allids = {s.id for s in dec.stages}
        assert host | dev == allids
        assert not (host & dev)


def test_g1_stages_have_device_ceiling_others_host():
    for s in PROFILE_WITH_SCRIPTS.chain():
        if s.gate is Gate.G1:
            assert s.host_provable is False
        else:
            assert s.host_provable is True


# --------------------------------------------------------------------------- #
# (6) device-node (mknod) branch
# --------------------------------------------------------------------------- #

def test_devnode_package_has_a_mknod_stage_hello_does_not():
    dev_ids = {s.id for s in PROFILE_WITH_DEVNODE.chain()}
    hello_ids = {s.id for s in PROFILE_HELLO.chain()}
    assert "dpkg_mknod" in dev_ids
    assert "dpkg_mknod" not in hello_ids


def test_mknod_stage_is_g2_meta_op_not_a_child():
    """The mknod stage is in-process (a meta-op the fakeroot shim would handle),
    NOT a fork+exec — it is the residual hard wall, not a G1 exec-re-entry."""
    stages = {s.id: s for s in PROFILE_WITH_DEVNODE.chain()}
    mknod = stages["dpkg_mknod"]
    assert mknod.proc is Proc.IN_PROC
    assert mknod.gate is Gate.G2
    assert mknod.optional is True


# --------------------------------------------------------------------------- #
# (7) cross-link: the G2 meta-stages are actually HOST-PROVEN
# --------------------------------------------------------------------------- #

def test_g2_meta_stages_are_host_proven_via_dpkg_unpack_model():
    """Close the loop: the stages this model tags G2 (host-provable) are actually
    proven by tools.dpkg_unpack_model — without fakeroot the meta-sequence hits the
    wall, with fakeroot it passes and is self-consistent."""
    proof = metadata_stages_host_proof()
    assert proof["without_fakeroot_unpacked"] is False
    assert "requires superuser privilege" in proof["without_fakeroot_wall"]
    assert proof["with_fakeroot_unpacked"] is True
    assert proof["with_fakeroot_self_consistent"] is True


# --------------------------------------------------------------------------- #
# (8) build_chain branch knobs are honoured
# --------------------------------------------------------------------------- #

def test_build_chain_knobs_toggle_optional_stages():
    base = {s.id for s in build_chain(
        has_compressed_data=True, has_maintainer_scripts=False,
        has_device_nodes=False)}
    with_scripts = {s.id for s in build_chain(
        has_compressed_data=True, has_maintainer_scripts=True,
        has_device_nodes=False)}
    with_dev = {s.id for s in build_chain(
        has_compressed_data=True, has_maintainer_scripts=False,
        has_device_nodes=True)}
    assert {"dpkg_run_preinst", "dpkg_run_postinst"} <= with_scripts
    assert {"dpkg_run_preinst", "dpkg_run_postinst"} & base == set()
    assert "dpkg_mknod" in with_dev
    assert "dpkg_mknod" not in base


def test_as_dict_serialises_every_stage_with_gate_and_ceiling():
    import json

    d = decide(PROFILE_HELLO).as_dict()
    assert len(d["stages"]) == len(PROFILE_HELLO.chain())
    for st in d["stages"]:
        assert "gate" in st and "ceiling" in st and "host_provable" in st
    # fully JSON round-trippable
    assert json.loads(json.dumps(d))["profile"] == "hello"


# --------------------------------------------------------------------------- #
# (9) the module selftest is green
# --------------------------------------------------------------------------- #

def test_module_selftest_passes():
    from tools.apt_install_e2e_model import _selftest

    assert _selftest() == 0
