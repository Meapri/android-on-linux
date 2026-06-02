"""Host tests for tools/build_chromium_net_overlay.py (CR-1 local page + CR-2 net).

All OFFLINE — the overlay is built with a synthetic CA bundle (no .deb download),
and assemble_ca_bundle is exercised against a hand-built ca-certificates-like
.deb. The NETWORK pack (real noble ca-certificates .deb → 146-cert bundle) is
verified by hand on the host (CONFORMANT, 214 KiB); the on-device CR-1 render /
CR-2 https load is the integration/device gate (WS-1 stages the tar).

Contract:
  * the builder module + the CR-1 local page (tools/chromium/cr-test.html) exist;
  * the cr-test.html is self-contained (an <h1> + CSS-colored div + inline <svg>,
    ZERO external resources) so CR-1 renders it with no network;
  * the packed overlay lists resolv.conf (8.8.8.8/1.1.1.1) + nsswitch (hosts: dns)
    + the CA bundle (/etc/ssl/certs/ca-certificates.crt) + /root/cr-test.html,
    all §5-E ./-rooted and stage_tar_spec conformant;
  * assemble_ca_bundle reproduces update-ca-certificates (concat all PEM .crt the
    .deb ships, sorted, each newline-terminated; non-cert files ignored).
"""

from __future__ import annotations

import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from tools import build_chromium_net_overlay as bcn
from tools.build_chromium_net_overlay import (
    CA_BUNDLE_PATH,
    NSSWITCH_PATH,
    RESOLV_CONF_PATH,
    TEST_PAGE_PATH,
    TEST_PAGE_SRC,
    assemble_ca_bundle,
    build_chromium_net_overlay,
    page_external_resources,
)
from tools.stage_tar_spec import validate_stage_tar

# A synthetic, self-contained CA "bundle" for the offline build path.
SYNTH_BUNDLE = (
    b"-----BEGIN CERTIFICATE-----\nMIIBfakeAAA\n-----END CERTIFICATE-----\n"
) * 3


# --------------------------------------------------------------------------- #
# Builder + asset exist
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bcn.__file__).is_file()


def test_cr_test_page_exists_on_disk():
    assert TEST_PAGE_SRC.is_file(), TEST_PAGE_SRC


# --------------------------------------------------------------------------- #
# CR-1 page is self-contained (no external fetch)
# --------------------------------------------------------------------------- #

def test_cr_test_page_has_render_markers():
    page = TEST_PAGE_SRC.read_text(encoding="utf-8")
    assert "<h1" in page                       # text/glyph + layout
    assert "#swatch" in page and "background:" in page  # CSS-colored div
    assert "<svg" in page                      # inline svg paint path


def test_cr_test_page_has_zero_external_resources():
    page = TEST_PAGE_SRC.read_text(encoding="utf-8")
    assert page_external_resources(page) == []


def test_page_external_resources_detects_real_fetches_not_prose():
    # prose/comment mentions must NOT trip the detector
    assert page_external_resources("<!-- no @import, no src=http here -->") == []
    # but a real CSS @import of a URL, or an absolute src, must
    assert page_external_resources("<style>@import url('https://x/y.css');</style>")
    assert page_external_resources('<img src="https://cdn/x.png">')
    assert page_external_resources('<script src="//cdn/x.js"></script>')  # protocol-rel


# --------------------------------------------------------------------------- #
# Offline overlay build: lists resolv.conf + ca-certs + nsswitch + page
# --------------------------------------------------------------------------- #

@pytest.fixture()
def built_overlay(tmp_path: Path):
    out = tmp_path / "chromium-net-stage.tar"
    res = build_chromium_net_overlay(
        out, ca_bundle=SYNTH_BUNDLE, ca_cert_count=3, ca_deb_filename="(synthetic)"
    )
    with tarfile.open(out) as t:
        names = {m.name: m for m in t.getmembers()}
        bodies = {
            n: t.extractfile(m).read() for n, m in names.items() if m.isfile()
        }
    return res, names, bodies


def test_overlay_lists_resolv_conf_with_public_resolvers(built_overlay):
    _, names, bodies = built_overlay
    assert "./" + RESOLV_CONF_PATH in names
    body = bodies["./" + RESOLV_CONF_PATH]
    assert b"nameserver 8.8.8.8" in body
    assert b"nameserver 1.1.1.1" in body


def test_overlay_lists_ca_certificates_bundle(built_overlay):
    _, names, bodies = built_overlay
    assert "./" + CA_BUNDLE_PATH in names
    assert CA_BUNDLE_PATH == "etc/ssl/certs/ca-certificates.crt"
    assert bodies["./" + CA_BUNDLE_PATH] == SYNTH_BUNDLE


def test_overlay_lists_nsswitch_files_dns(built_overlay):
    _, names, bodies = built_overlay
    assert "./" + NSSWITCH_PATH in names
    body = bodies["./" + NSSWITCH_PATH]
    assert b"hosts:" in body and b"dns" in body and b"files" in body


def test_overlay_packs_the_cr_test_page(built_overlay):
    _, names, bodies = built_overlay
    assert "./" + TEST_PAGE_PATH in names
    assert TEST_PAGE_PATH == "root/cr-test.html"
    assert bodies["./" + TEST_PAGE_PATH] == TEST_PAGE_SRC.read_bytes()


def test_overlay_all_members_dot_rooted(built_overlay):
    _, names, _ = built_overlay
    assert all(n.startswith("./") for n in names)


def test_overlay_is_stage_tar_conformant(tmp_path: Path):
    out = tmp_path / "chromium-net-stage.tar"
    build_chromium_net_overlay(
        out, ca_bundle=SYNTH_BUNDLE, ca_cert_count=3, ca_deb_filename="(synthetic)"
    )
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


def test_overlay_reports_four_files(built_overlay):
    res, _, _ = built_overlay
    assert res.file_count == 4
    assert res.ca_cert_count == 3


# --------------------------------------------------------------------------- #
# assemble_ca_bundle reproduces update-ca-certificates
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to synthesize a .deb")
def test_assemble_ca_bundle_concats_all_pems_sorted(tmp_path: Path):
    root = tmp_path / "dataroot"
    moz = root / "usr/share/ca-certificates/mozilla"
    moz.mkdir(parents=True)
    (moz / "A.crt").write_bytes(
        b"-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n")
    (moz / "B.crt").write_bytes(  # no trailing newline -> must be added
        b"-----BEGIN CERTIFICATE-----\nBBBB\n-----END CERTIFICATE-----")
    (moz / "README").write_bytes(b"not a cert\n")  # ignored

    data_tar = tmp_path / "data.tar"
    with tarfile.open(data_tar, "w") as dt:
        for n in ("A.crt", "B.crt", "README"):
            dt.add(moz / n, arcname=f"./usr/share/ca-certificates/mozilla/{n}")
    (tmp_path / "debian-binary").write_text("2.0\n")
    (tmp_path / "control.tar").write_bytes(b"")
    subprocess.run(
        ["ar", "qcS", "synthetic-cacerts.deb", "debian-binary",
         "control.tar", "data.tar"],
        check=True, capture_output=True, cwd=str(tmp_path),
    )
    bundle, count = assemble_ca_bundle(tmp_path / "synthetic-cacerts.deb")
    assert count == 2
    assert b"not a cert" not in bundle              # non-cert ignored
    assert bundle.index(b"AAAA") < bundle.index(b"BBBB")  # sorted by path
    assert bundle.endswith(b"-----END CERTIFICATE-----\n")  # B got its newline


# --------------------------------------------------------------------------- #
# Builder selftest
# --------------------------------------------------------------------------- #

def test_builder_selftest_passes():
    from tools.build_chromium_net_overlay import _selftest

    assert _selftest() == 0
