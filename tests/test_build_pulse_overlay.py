"""Host tests for tools/build_pulse_overlay.py (ALR audio sink staging overlay).

All OFFLINE — the overlay is built from a SYNTHETIC extracted closure root (a few
fake-ELF libs + a base-owned lib that must be subtracted), never a .deb download.
The NETWORK pack (real noble libpulse0 closure → libpulse.so.0 + deps) and the
on-device "guest app plays sound" are the integration/device gate (WS-1 stages the
tar + starts the native AlrPulseServer).

Contract (design android-audio-sink.md §3/§9):
  * the builder module exists;
  * /etc/pulse/client.conf is ADDRESS-FREE (no default-server) — the address is the
    PULSE_SERVER env (runtime_report.cpp) — and nails off autospawn + shm/memfd so
    libpulse uses the socket-copy PCM path our server reads with recv();
  * only CLIENT libs are staged (never the `pulseaudio` server package — we ARE the
    server);
  * the packed overlay subtracts base-owned libs (no downgrade — passes overlay_guard),
    prunes man/doc, is §5-E ./-rooted + stage_tar_spec conformant;
  * --with-alsa adds the stock ALSA->pulse /etc/asound.conf.
"""

from __future__ import annotations

import tarfile
from pathlib import Path

import pytest

from tools import build_pulse_overlay as bpo
from tools.build_pulse_overlay import (
    ALSA_BRIDGE_TARGETS,
    ASOUND_CONF_BODY,
    ASOUND_CONF_PATH,
    CLIENT_CONF_BODY,
    CLIENT_CONF_PATH,
    PULSE_TARGETS,
    build_pulse_overlay,
)
from tools.stage_tar_spec import validate_stage_tar

# Fake-ELF blob so deb_closure's SONAME classifier treats a file as a library.
_ELF = b"\x7fELF" + b"\x00" * 60


def _make_closure_root(tmp_path: Path, name: str = "merged") -> Path:
    """A synthetic extracted closure: libpulse.so.0 + a common lib + a base-owned
    libc.so.6 (must be subtracted) + a doc file (must be pruned)."""
    root = tmp_path / name
    libdir = root / "usr/lib/aarch64-linux-gnu"
    libdir.mkdir(parents=True)
    (libdir / "libpulse.so.0").write_bytes(_ELF)
    (libdir / "libpulsecommon-16.1.so").write_bytes(_ELF)
    (libdir / "libc.so.6").write_bytes(_ELF)  # base-owned
    doc = root / "usr/share/doc/libpulse0"
    doc.mkdir(parents=True)
    (doc / "copyright").write_text("doc\n")
    return root


def _make_base(tmp_path: Path) -> Path:
    """A synthetic base rootfs that already owns libc.so.6."""
    base = tmp_path / "base"
    blib = base / "usr/lib/aarch64-linux-gnu"
    blib.mkdir(parents=True)
    (blib / "libc.so.6").write_bytes(_ELF)
    return base


# --------------------------------------------------------------------------- #
# Module + config contract
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bpo.__file__).is_file()


def test_client_conf_is_address_free():
    # The authoritative server address is the PULSE_SERVER env, NOT client.conf.
    assert "default-server" not in CLIENT_CONF_BODY


def test_client_conf_nails_off_autospawn_and_shm():
    assert "autospawn = no" in CLIENT_CONF_BODY
    assert "enable-shm = no" in CLIENT_CONF_BODY
    assert "enable-memfd = no" in CLIENT_CONF_BODY


def test_targets_are_client_libs_only():
    # We are the server: never stage the `pulseaudio` server package.
    assert "pulseaudio" not in PULSE_TARGETS
    assert "libpulse0" in PULSE_TARGETS
    assert "libpulse-mainloop-glib0" in PULSE_TARGETS


def test_alsa_bridge_target_is_stock_plugins():
    assert "libasound2-plugins" in ALSA_BRIDGE_TARGETS


def test_asound_conf_routes_default_to_pulse():
    assert "pcm.!default { type pulse }" in ASOUND_CONF_BODY
    assert "ctl.!default { type pulse }" in ASOUND_CONF_BODY


# --------------------------------------------------------------------------- #
# Offline overlay build (synthetic closure root)
# --------------------------------------------------------------------------- #

@pytest.fixture()
def built_overlay(tmp_path: Path):
    root = _make_closure_root(tmp_path)
    base = _make_base(tmp_path)
    out = tmp_path / "pulse-stage.tar"
    res = build_pulse_overlay(out, base, with_alsa=False, merged_root_override=root)
    with tarfile.open(out) as t:
        names = {m.name for m in t.getmembers()}
        bodies = {m.name: t.extractfile(m).read()
                  for m in t.getmembers() if m.isfile()}
    return res, names, bodies


def test_overlay_ships_client_conf(built_overlay):
    _, names, bodies = built_overlay
    assert "./" + CLIENT_CONF_PATH in names
    assert b"autospawn = no" in bodies["./" + CLIENT_CONF_PATH]
    assert b"default-server" not in bodies["./" + CLIENT_CONF_PATH]


def test_overlay_stages_libpulse(built_overlay):
    _, names, _ = built_overlay
    # flat-soname classifier may relocate it; accept any path ending in the soname.
    assert any(n.endswith("libpulse.so.0") for n in names)


def test_overlay_subtracts_base_owned_lib(built_overlay):
    _, names, _ = built_overlay
    # the base already owns libc.so.6 → it must NOT be in the overlay (no downgrade).
    assert not any(n.endswith("/libc.so.6") for n in names)


def test_overlay_prunes_doc(built_overlay):
    _, names, _ = built_overlay
    assert not any("/usr/share/doc/" in n for n in names)


def test_overlay_all_members_dot_rooted(built_overlay):
    _, names, _ = built_overlay
    assert all(n.startswith("./") for n in names)


def test_overlay_no_guard_violations(built_overlay):
    res, _, _ = built_overlay
    assert res.violations == ()


def test_overlay_reports_subtracted_base(built_overlay):
    res, _, _ = built_overlay
    assert any("libc.so.6" in s for s in res.skipped_base)


def test_overlay_is_stage_tar_conformant_against_base(tmp_path: Path):
    root = _make_closure_root(tmp_path)
    base = _make_base(tmp_path)
    out = tmp_path / "pulse-stage.tar"
    build_pulse_overlay(out, base, with_alsa=False, merged_root_override=root)
    rep = validate_stage_tar(str(out), base=base)
    assert rep.conformant, rep.errors


# --------------------------------------------------------------------------- #
# --with-alsa adds /etc/asound.conf
# --------------------------------------------------------------------------- #

def test_with_alsa_adds_asound_conf(tmp_path: Path):
    root = _make_closure_root(tmp_path, name="merged_alsa")
    base = _make_base(tmp_path)
    out = tmp_path / "pulse-alsa-stage.tar"
    res = build_pulse_overlay(out, base, with_alsa=True, merged_root_override=root)
    with tarfile.open(out) as t:
        names = {m.name for m in t.getmembers()}
    assert "./" + ASOUND_CONF_PATH in names
    assert res.with_alsa is True
    assert "./" + ASOUND_CONF_PATH in res.config_members


def test_without_alsa_omits_asound_conf(built_overlay):
    _, names, _ = built_overlay
    assert "./" + ASOUND_CONF_PATH not in names


# --------------------------------------------------------------------------- #
# Builder selftest
# --------------------------------------------------------------------------- #

def test_builder_selftest_passes():
    from tools.build_pulse_overlay import _selftest

    assert _selftest() == 0
