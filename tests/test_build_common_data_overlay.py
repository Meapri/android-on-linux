"""Host tests for tools/build_common_data_overlay.py (the one-shot COMMON-DATA overlay).

All OFFLINE — the network groups are exercised either with injected data (the CA
bundle) or against hand-built synthetic .debs (locale, CA assembly). The real
noble build (146-cert CA bundle + 921-file compiled MIME db + 808 icon files + 12
locale files, CONFORMANT) is verified by hand on the host; the on-device effect
(apps finding CA roots / MIME types / icons / machine-id / locale) is the
integration/device gate (the main session stages the produced tar).

Contract:
  * the builder module exists and exposes the documented groups/constants;
  * group→package selection is correct (ca→ca-certificates, icons→hicolor+adwaita,
    machine-id→no deb), deduped and order-stable;
  * the machine-id group is fully synthesizable with NO network and ships
    /etc/machine-id + /var/lib/dbus/machine-id (same id) + an XML dbus session stub;
  * the ca group packs the bundle at /etc/ssl/certs/ca-certificates.crt and
    assemble_ca_bundle reproduces update-ca-certificates (concat all PEM .crt,
    sorted, newline-terminated, non-cert files ignored);
  * the locale group packs ONLY usr/lib/locale/C.utf8/** (not unrelated payload);
  * every produced overlay is §5-E ./-rooted and stage_tar_spec conformant;
  * an unknown group is rejected.
"""

from __future__ import annotations

import io
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from tools import build_common_data_overlay as bcd
from tools.build_common_data_overlay import (
    ALL_GROUPS,
    CA_BUNDLE_PATH,
    DBUS_SESSION_CONF_PATH,
    LOCALE_PREFIX,
    MACHINE_ID_PATHS,
    SCHEMAS_COMPILED,
    SCHEMAS_DIR,
    STUB_MACHINE_ID,
    assemble_ca_bundle,
    build_common_data_overlay,
    packages_for_groups,
)
from tools.stage_tar_spec import validate_stage_tar

# A synthetic, self-contained CA "bundle" for the offline ca-group build path.
SYNTH_BUNDLE = (
    b"-----BEGIN CERTIFICATE-----\nMIIBfake\n-----END CERTIFICATE-----\n"
) * 3


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #

def _names_bodies(tar_path: Path):
    with tarfile.open(tar_path) as t:
        names = {m.name: m for m in t.getmembers()}
        bodies = {n: t.extractfile(m).read()
                  for n, m in names.items() if m.isfile()}
    return names, bodies


def _make_synthetic_deb(tmp: Path, name: str, files: dict[str, bytes]) -> Path:
    data_tar = tmp / "data.tar"
    with tarfile.open(data_tar, "w") as dt:
        for rel, body in files.items():
            ti = tarfile.TarInfo("./" + rel)
            ti.size = len(body)
            ti.mode = 0o644
            dt.addfile(ti, io.BytesIO(body))
    (tmp / "debian-binary").write_text("2.0\n")
    (tmp / "control.tar").write_bytes(b"")
    deb = tmp / name
    subprocess.run(
        ["ar", "qcS", deb.name, "debian-binary", "control.tar", "data.tar"],
        check=True, capture_output=True, cwd=str(tmp),
    )
    return deb


# --------------------------------------------------------------------------- #
# Builder exists + constants
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bcd.__file__).is_file()


def test_all_groups_are_the_six_documented():
    assert set(ALL_GROUPS) == {"ca", "mime", "icons", "machine-id", "locale", "schemas"}


def test_packages_for_groups_schemas():
    assert packages_for_groups(["schemas"]) == ["gsettings-desktop-schemas"]


def test_packages_for_groups_schemas_with_app_package():
    # the extra per-app schema package is appended after gsettings-desktop-schemas
    assert packages_for_groups(["schemas"], schema_packages=("gnome-calculator",)) == [
        "gsettings-desktop-schemas", "gnome-calculator",
    ]


def test_canonical_paths_are_debian_rooted():
    assert CA_BUNDLE_PATH == "etc/ssl/certs/ca-certificates.crt"
    assert DBUS_SESSION_CONF_PATH == "etc/dbus-1/session.conf"
    assert LOCALE_PREFIX == "usr/lib/locale/C.utf8/"
    assert MACHINE_ID_PATHS == ("etc/machine-id", "var/lib/dbus/machine-id")


# --------------------------------------------------------------------------- #
# group -> package selection (pure)
# --------------------------------------------------------------------------- #

def test_packages_for_groups_ca():
    assert packages_for_groups(["ca"]) == ["ca-certificates"]


def test_packages_for_groups_icons_pulls_hicolor_and_adwaita():
    assert packages_for_groups(["icons"]) == [
        "hicolor-icon-theme", "adwaita-icon-theme"]


def test_packages_for_groups_machine_id_needs_no_deb():
    assert packages_for_groups(["machine-id"]) == []


def test_packages_for_groups_dedupes():
    assert packages_for_groups(["ca", "ca", "machine-id"]) == ["ca-certificates"]


# --------------------------------------------------------------------------- #
# machine-id group — fully offline (no network, no ar)
# --------------------------------------------------------------------------- #

@pytest.fixture()
def machine_id_overlay(tmp_path: Path):
    out = tmp_path / "mid.tar"
    res = build_common_data_overlay(out, ["machine-id"])
    names, bodies = _names_bodies(out)
    return res, names, bodies


def test_machine_id_ships_both_machine_id_files(machine_id_overlay):
    _, names, _ = machine_id_overlay
    assert "./etc/machine-id" in names
    assert "./var/lib/dbus/machine-id" in names


def test_machine_id_is_32_hex_and_identical_in_both_files(machine_id_overlay):
    _, _, bodies = machine_id_overlay
    a = bodies["./etc/machine-id"].strip()
    b = bodies["./var/lib/dbus/machine-id"].strip()
    assert a == b == STUB_MACHINE_ID.encode()
    assert len(STUB_MACHINE_ID) == 32
    assert all(c in "0123456789abcdef" for c in STUB_MACHINE_ID)


def test_machine_id_ships_dbus_session_conf_busconfig(machine_id_overlay):
    _, names, bodies = machine_id_overlay
    assert "./" + DBUS_SESSION_CONF_PATH in names
    body = bodies["./" + DBUS_SESSION_CONF_PATH]
    assert b"<busconfig>" in body and b"<type>session</type>" in body


def test_machine_id_needs_no_deb(machine_id_overlay):
    res, _, _ = machine_id_overlay
    assert res.debs == ()


def test_machine_id_overlay_all_dot_rooted(machine_id_overlay):
    _, names, _ = machine_id_overlay
    assert all(n.startswith("./") for n in names)


def test_machine_id_overlay_is_stage_tar_conformant(tmp_path: Path):
    out = tmp_path / "mid.tar"
    build_common_data_overlay(out, ["machine-id"])
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


# --------------------------------------------------------------------------- #
# ca group — injected bundle (no network)
# --------------------------------------------------------------------------- #

def test_ca_group_packs_bundle_at_canonical_path(tmp_path: Path):
    out = tmp_path / "ca.tar"
    res = build_common_data_overlay(out, ["ca"], ca_bundle=SYNTH_BUNDLE)
    names, bodies = _names_bodies(out)
    assert "./" + CA_BUNDLE_PATH in names
    assert bodies["./" + CA_BUNDLE_PATH] == SYNTH_BUNDLE
    assert res.ca_cert_count == 3  # counted from the injected bundle
    assert res.ca_bundle_bytes == len(SYNTH_BUNDLE)


def test_ca_overlay_is_stage_tar_conformant(tmp_path: Path):
    out = tmp_path / "ca.tar"
    build_common_data_overlay(out, ["ca"], ca_bundle=SYNTH_BUNDLE, ca_cert_count=3)
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


# --------------------------------------------------------------------------- #
# combined groups (injected ca + synthesized machine-id)
# --------------------------------------------------------------------------- #

def test_ca_plus_machine_id_combined(tmp_path: Path):
    out = tmp_path / "combo.tar"
    res = build_common_data_overlay(
        out, ["ca", "machine-id"], ca_bundle=SYNTH_BUNDLE, ca_cert_count=3)
    names, _ = _names_bodies(out)
    assert "./" + CA_BUNDLE_PATH in names
    assert "./etc/machine-id" in names
    assert res.groups == ("ca", "machine-id")
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


# --------------------------------------------------------------------------- #
# unknown group rejected
# --------------------------------------------------------------------------- #

def test_unknown_group_rejected(tmp_path: Path):
    with pytest.raises(ValueError):
        build_common_data_overlay(tmp_path / "x.tar", ["does-not-exist"])


# --------------------------------------------------------------------------- #
# assemble_ca_bundle reproduces update-ca-certificates (synthetic .deb)
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to synthesize a .deb")
def test_assemble_ca_bundle_concats_all_pems_sorted(tmp_path: Path):
    deb = _make_synthetic_deb(tmp_path, "synthetic-cacerts.deb", {
        "usr/share/ca-certificates/mozilla/A.crt":
            b"-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n",
        "usr/share/ca-certificates/mozilla/B.crt":  # no trailing newline -> added
            b"-----BEGIN CERTIFICATE-----\nBBBB\n-----END CERTIFICATE-----",
        "usr/share/ca-certificates/mozilla/README": b"not a cert\n",  # ignored
    })
    bundle, count = assemble_ca_bundle(deb)
    assert count == 2
    assert b"not a cert" not in bundle
    assert bundle.index(b"AAAA") < bundle.index(b"BBBB")            # sorted by path
    assert bundle.endswith(b"-----END CERTIFICATE-----\n")          # B got its \n


# --------------------------------------------------------------------------- #
# locale group — packs ONLY usr/lib/locale/C.utf8/** (synthetic libc-bin .deb)
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to synthesize a .deb")
def test_locale_group_packs_only_c_utf8(tmp_path: Path):
    deb = _make_synthetic_deb(tmp_path, "libc-bin.deb", {
        "usr/lib/locale/C.utf8/LC_CTYPE": b"CTYPE-DATA",
        "usr/lib/locale/C.utf8/LC_MESSAGES/SYS_LC_MESSAGES": b"MSG",
        "usr/bin/iconv": b"\x7fELF-unrelated",   # must NOT be packed
    })
    cache = tmp_path / "cache"
    cache.mkdir()
    shutil.copy(deb, cache / "libc-bin.deb")
    idx = {"libc-bin": {"Filename": "pool/x/libc-bin.deb"}}
    out = tmp_path / "loc.tar"
    res = build_common_data_overlay(out, ["locale"], index=idx, cache_dir=cache)
    names, _ = _names_bodies(out)
    assert "./usr/lib/locale/C.utf8/LC_CTYPE" in names
    assert "./usr/lib/locale/C.utf8/LC_MESSAGES/SYS_LC_MESSAGES" in names  # nested
    assert "./usr/bin/iconv" not in names                                  # unrelated
    assert res.locale_file_count == 2
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


# --------------------------------------------------------------------------- #
# schemas group (gnome-platform unlock) — host glib-compile-schemas
# --------------------------------------------------------------------------- #

_APP_SCHEMA = (
    b'<?xml version="1.0" encoding="UTF-8"?>\n<schemalist>\n'
    b'  <schema id="org.test.App" path="/org/test/App/">\n'
    b'    <key name="width" type="i"><default>800</default></key>\n'
    b'  </schema>\n</schemalist>\n'
)
_BASE_SCHEMA = (
    b'<?xml version="1.0" encoding="UTF-8"?>\n<schemalist>\n'
    b'  <schema id="org.gtk.Base" path="/org/gtk/Base/">\n'
    b'    <key name="theme" type="s"><default>"x"</default></key>\n'
    b'  </schema>\n</schemalist>\n'
)


def _schemas_build(tmp_path: Path):
    """Build the schemas overlay offline (synthetic schema deb + base) → (out, res, names)."""
    deb = _make_synthetic_deb(tmp_path, "gsds.deb", {
        SCHEMAS_DIR + "/org.test.App.gschema.xml": _APP_SCHEMA,
        "usr/share/doc/x/README": b"not a schema\n",   # must NOT be packed
    })
    base_tar = tmp_path / "base.tar"
    with tarfile.open(base_tar, "w") as bt:
        ti = tarfile.TarInfo("./" + SCHEMAS_DIR + "/org.gtk.Base.gschema.xml")
        ti.size = len(_BASE_SCHEMA); ti.mode = 0o644
        bt.addfile(ti, io.BytesIO(_BASE_SCHEMA))
    cache = tmp_path / "cache"; cache.mkdir()
    shutil.copy(deb, cache / "gsds.deb")
    idx = {"gsettings-desktop-schemas": {"Filename": "pool/x/gsds.deb"}}
    out = tmp_path / "schemas.tar"
    res = build_common_data_overlay(out, ["schemas"], index=idx, cache_dir=cache,
                                    base=base_tar)
    names, _ = _names_bodies(out)
    return out, res, names


@pytest.mark.skipif(shutil.which("ar") is None, reason="needs `ar` to synthesize a .deb")
def test_schemas_group_ships_new_xml_not_base_and_skips_unrelated(tmp_path: Path):
    out, res, names = _schemas_build(tmp_path)
    new_xml = "./" + SCHEMAS_DIR + "/org.test.App.gschema.xml"
    base_xml = "./" + SCHEMAS_DIR + "/org.gtk.Base.gschema.xml"
    assert new_xml in names, "the new app .gschema.xml must be shipped"
    assert base_xml not in names, "base .gschema.xml must NOT be re-shipped"
    assert "./usr/share/doc/x/README" not in names, "unrelated file must not be packed"
    assert res.schema_xml_count == 1
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


@pytest.mark.skipif(
    shutil.which("ar") is None or shutil.which("glib-compile-schemas") is None,
    reason="needs `ar` + glib-compile-schemas to compile the gschemas binary",
)
def test_schemas_group_compiles_superset_gschemas(tmp_path: Path):
    out, res, names = _schemas_build(tmp_path)
    comp = "./" + SCHEMAS_COMPILED
    assert comp in names, "gschemas.compiled must be shipped when the compiler is present"
    assert res.schemas_compiled is True
    # the compiled binary is a SUPERSET: it must contain BOTH the base and the app schema id
    # (so re-compiling never drops the base GTK schemas).
    with tarfile.open(out) as t:
        blob = t.extractfile(comp).read()
    assert b"org.gtk.Base" in blob, "compiled binary lost the base schema (not a superset)"
    assert b"org.test.App" in blob, "compiled binary missing the new app schema"


# --------------------------------------------------------------------------- #
# Builder selftest
# --------------------------------------------------------------------------- #

def test_builder_selftest_passes():
    assert bcd._selftest() == 0
