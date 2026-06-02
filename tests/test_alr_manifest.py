"""Host tests for tools/alr_manifest.py (T2 — Linux-app manifest schema)."""

from __future__ import annotations

import json

import pytest

from tools.alr_manifest import (
    AppEntry,
    AppManifest,
    Catalog,
    DisplaySpec,
    ManifestError,
    PERMISSIONS,
    RootfsDep,
    load_catalog,
    load_manifest,
    parse_catalog,
    parse_manifest,
)


# --------------------------------------------------------------------------- #
# Fixtures
# --------------------------------------------------------------------------- #


def _valid_manifest_dict() -> dict:
    """A fully-specified, valid manifest (GIMP), used as the happy-path base."""
    return {
        "app_id": "org.gimp.GIMP",
        "name": "GIMP",
        "summary": "GNU Image Manipulation Program",
        "description": "Raster graphics editor for image retouching and editing.",
        "category": "graphics",
        "icon": "/usr/share/icons/hicolor/256x256/apps/gimp.png",
        "entry": {
            "kind": "exec",
            "target": "/usr/bin/gimp",
            "argv": ["--no-splash"],
        },
        "rootfs_deps": [
            {"kind": "stage-tar", "ref": "gimp-stage.tar", "install_size_bytes": 220_000_000},
            {"kind": "apt", "ref": "gimp-data", "install_size_bytes": 30_000_000},
        ],
        "required_permissions": ["storage-read", "storage-write"],
        "display": {"mode": "windowed", "width": 1280, "height": 1024},
        "min_runtime": "0.4.137",
        "install_size_bytes": 250_000_000,
    }


def _minimal_manifest_dict() -> dict:
    """Only the required fields — defaults fill the rest."""
    return {
        "app_id": "org.foot.foot",
        "name": "foot",
        "summary": "A fast Wayland terminal",
        "entry": {"kind": "exec", "target": "/usr/bin/foot"},
    }


# --------------------------------------------------------------------------- #
# Happy paths
# --------------------------------------------------------------------------- #


def test_parse_full_manifest_ok():
    m = parse_manifest(_valid_manifest_dict())
    assert isinstance(m, AppManifest)
    assert m.app_id == "org.gimp.GIMP"
    assert m.category == "graphics"
    assert m.entry.kind == "exec"
    assert m.entry.target == "/usr/bin/gimp"
    assert m.entry.argv == ("--no-splash",)
    assert m.required_permissions == ("storage-read", "storage-write")
    assert m.display.mode == "windowed"
    assert m.display.width == 1280
    assert len(m.rootfs_deps) == 2
    assert m.total_install_size_bytes == 250_000_000


def test_parse_minimal_manifest_applies_defaults():
    m = parse_manifest(_minimal_manifest_dict())
    assert m.category == "utility"
    assert m.description == ""
    assert m.icon == ""
    assert m.rootfs_deps == ()
    assert m.required_permissions == ()
    assert m.display == DisplaySpec()
    assert m.display.mode == "windowed"
    assert m.min_runtime == "0.1"
    assert m.install_size_bytes == 0
    assert m.total_install_size_bytes == 0


def test_total_install_size_falls_back_to_dep_sum():
    data = _valid_manifest_dict()
    data["install_size_bytes"] = 0  # author left top-level estimate unset
    m = parse_manifest(data)
    assert m.total_install_size_bytes == 220_000_000 + 30_000_000


def test_desktop_entry_ok():
    data = _minimal_manifest_dict()
    data["entry"] = {
        "kind": "desktop",
        "target": "/usr/share/applications/org.gnome.gedit.desktop",
    }
    m = parse_manifest(data)
    assert m.entry.kind == "desktop"
    assert m.entry.argv == ()


def test_icon_may_be_desktop_ref():
    data = _minimal_manifest_dict()
    data["icon"] = "org.gnome.gedit.desktop"
    m = parse_manifest(data)
    assert m.icon == "org.gnome.gedit.desktop"


def test_all_permission_enum_values_accepted():
    data = _minimal_manifest_dict()
    data["required_permissions"] = sorted(PERMISSIONS)
    m = parse_manifest(data)
    assert set(m.required_permissions) == PERMISSIONS


# --------------------------------------------------------------------------- #
# Missing required fields
# --------------------------------------------------------------------------- #


@pytest.mark.parametrize("missing", ["app_id", "name", "summary", "entry"])
def test_missing_required_field_rejected(missing):
    data = _valid_manifest_dict()
    del data[missing]
    with pytest.raises(ManifestError) as exc:
        parse_manifest(data)
    assert missing in str(exc.value)


def test_entry_missing_target_rejected():
    data = _valid_manifest_dict()
    del data["entry"]["target"]
    with pytest.raises(ManifestError):
        parse_manifest(data)


# --------------------------------------------------------------------------- #
# Wrong types
# --------------------------------------------------------------------------- #


def test_name_wrong_type_rejected():
    data = _valid_manifest_dict()
    data["name"] = 123
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_install_size_bool_rejected():
    # bool is an int subclass; ensure `true` isn't silently read as 1.
    data = _valid_manifest_dict()
    data["install_size_bytes"] = True
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_required_permissions_not_a_list_rejected():
    data = _valid_manifest_dict()
    data["required_permissions"] = "storage-read"
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_rootfs_deps_not_a_list_rejected():
    data = _valid_manifest_dict()
    data["rootfs_deps"] = {"kind": "apt", "ref": "x"}
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_argv_non_string_rejected():
    data = _valid_manifest_dict()
    data["entry"]["argv"] = ["ok", 7]
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_negative_install_size_rejected():
    data = _valid_manifest_dict()
    data["install_size_bytes"] = -1
    with pytest.raises(ManifestError):
        parse_manifest(data)


# --------------------------------------------------------------------------- #
# Unknown permission / category / display / dep kind
# --------------------------------------------------------------------------- #


def test_unknown_permission_rejected():
    data = _valid_manifest_dict()
    data["required_permissions"] = ["storage-read", "bluetooth"]
    with pytest.raises(ManifestError) as exc:
        parse_manifest(data)
    assert "bluetooth" in str(exc.value)


def test_duplicate_permission_rejected():
    data = _valid_manifest_dict()
    data["required_permissions"] = ["network", "network"]
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_unknown_category_rejected():
    data = _valid_manifest_dict()
    data["category"] = "cryptomining"
    with pytest.raises(ManifestError) as exc:
        parse_manifest(data)
    assert "category" in str(exc.value)


def test_unknown_display_mode_rejected():
    data = _valid_manifest_dict()
    data["display"]["mode"] = "kiosk"
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_unknown_entry_kind_rejected():
    data = _valid_manifest_dict()
    data["entry"]["kind"] = "shellscript"
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_unknown_dep_kind_rejected():
    data = _valid_manifest_dict()
    data["rootfs_deps"] = [{"kind": "flatpak", "ref": "x.tar"}]
    with pytest.raises(ManifestError):
        parse_manifest(data)


# --------------------------------------------------------------------------- #
# Bad id / version
# --------------------------------------------------------------------------- #


@pytest.mark.parametrize(
    "bad_id",
    [
        "gimp",  # no domain part
        "Org..GIMP",  # empty label
        "1org.gimp",  # leading digit label
        "org.gimp/GIMP",  # slash
        "org gimp",  # space
        ".org.gimp",  # leading dot
        "org.gimp.",  # trailing dot
        "",
    ],
)
def test_bad_app_id_rejected(bad_id):
    data = _valid_manifest_dict()
    data["app_id"] = bad_id
    with pytest.raises(ManifestError):
        parse_manifest(data)


@pytest.mark.parametrize("good_id", ["org.gimp.GIMP", "io.github.user.app-name", "a.b", "com.x.y.z_w"])
def test_good_app_id_accepted(good_id):
    data = _minimal_manifest_dict()
    data["app_id"] = good_id
    assert parse_manifest(data).app_id == good_id


@pytest.mark.parametrize("bad_ver", ["1", "v1.2", "1.2.3.4.5", "latest", "1.2-", "1..2"])
def test_bad_min_runtime_rejected(bad_ver):
    data = _minimal_manifest_dict()
    data["min_runtime"] = bad_ver
    with pytest.raises(ManifestError):
        parse_manifest(data)


@pytest.mark.parametrize("good_ver", ["0.1", "0.4.137", "1.0.0", "0.4.137-cp6", "2.0"])
def test_good_min_runtime_accepted(good_ver):
    data = _minimal_manifest_dict()
    data["min_runtime"] = good_ver
    assert parse_manifest(data).min_runtime == good_ver


# --------------------------------------------------------------------------- #
# entry / target / icon path safety
# --------------------------------------------------------------------------- #


def test_relative_entry_target_rejected():
    data = _valid_manifest_dict()
    data["entry"]["target"] = "usr/bin/gimp"
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_entry_target_traversal_rejected():
    data = _valid_manifest_dict()
    data["entry"]["target"] = "/usr/../etc/passwd"
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_desktop_entry_with_argv_rejected():
    data = _valid_manifest_dict()
    data["entry"] = {
        "kind": "desktop",
        "target": "/usr/share/applications/x.desktop",
        "argv": ["--bad"],
    }
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_desktop_entry_wrong_suffix_rejected():
    data = _valid_manifest_dict()
    data["entry"] = {"kind": "desktop", "target": "/usr/bin/gimp"}
    with pytest.raises(ManifestError):
        parse_manifest(data)


def test_icon_relative_non_desktop_rejected():
    data = _valid_manifest_dict()
    data["icon"] = "gimp.png"  # neither absolute nor a .desktop ref
    with pytest.raises(ManifestError):
        parse_manifest(data)


# --------------------------------------------------------------------------- #
# rootfs_deps §5-E bridge
# --------------------------------------------------------------------------- #


def test_stage_tar_ref_must_be_bare_filename():
    with pytest.raises(ManifestError):
        RootfsDep(kind="stage-tar", ref="../evil-stage.tar")
    with pytest.raises(ManifestError):
        RootfsDep(kind="stage-tar", ref="sub/dir/x-stage.tar")


def test_stage_tar_ref_must_end_with_tar():
    with pytest.raises(ManifestError):
        RootfsDep(kind="stage-tar", ref="gimp-stage")


def test_stage_marker_stem_matches_mainactivity_convention():
    dep = RootfsDep(kind="stage-tar", ref="gimp-stage.tar")
    # MainActivity keys re-extraction on ".<name>-staged-<size>" where <name>
    # is the basename minus "-stage.tar".
    assert dep.stage_marker_stem == "gimp"
    # A tar without the "-stage" suffix keeps its full stem.
    assert RootfsDep(kind="stage-tar", ref="dpkg-db.tar").stage_marker_stem == "dpkg-db"


def test_stage_marker_stem_rejected_for_apt_dep():
    dep = RootfsDep(kind="apt", ref="gimp-data")
    with pytest.raises(ManifestError):
        _ = dep.stage_marker_stem


@pytest.mark.parametrize("bad_pkg", ["GIMP", "_leading", "has space", "a", "x;rm"])
def test_bad_apt_package_name_rejected(bad_pkg):
    with pytest.raises(ManifestError):
        RootfsDep(kind="apt", ref=bad_pkg)


@pytest.mark.parametrize("good_pkg", ["gimp", "libsdl2-2.0-0", "g++-12", "qt6-wayland"])
def test_good_apt_package_name_accepted(good_pkg):
    assert RootfsDep(kind="apt", ref=good_pkg).ref == good_pkg


def test_negative_dep_install_size_rejected():
    with pytest.raises(ManifestError):
        RootfsDep(kind="apt", ref="gimp", install_size_bytes=-5)


# --------------------------------------------------------------------------- #
# Catalog
# --------------------------------------------------------------------------- #


def test_parse_catalog_list_form():
    cat = parse_catalog([_valid_manifest_dict(), _minimal_manifest_dict()])
    assert isinstance(cat, Catalog)
    assert len(cat.apps) == 2
    assert cat.get("org.gimp.GIMP") is not None
    assert cat.get("org.foot.foot") is not None
    assert cat.get("does.not.exist") is None


def test_parse_catalog_object_form():
    cat = parse_catalog({"apps": [_minimal_manifest_dict()]})
    assert len(cat.apps) == 1


def test_catalog_duplicate_app_id_rejected():
    a = _minimal_manifest_dict()
    b = _minimal_manifest_dict()  # same app_id
    with pytest.raises(ManifestError) as exc:
        parse_catalog([a, b])
    assert "duplicate app_id" in str(exc.value)


def test_catalog_object_without_apps_rejected():
    with pytest.raises(ManifestError):
        parse_catalog({"name": "no apps here"})


def test_catalog_propagates_member_error():
    bad = _minimal_manifest_dict()
    bad["app_id"] = "not-reverse-dns"
    with pytest.raises(ManifestError):
        parse_catalog([_valid_manifest_dict(), bad])


# --------------------------------------------------------------------------- #
# File loaders + JSON errors
# --------------------------------------------------------------------------- #


def test_load_manifest_from_file(tmp_path):
    p = tmp_path / "gimp.json"
    p.write_text(json.dumps(_valid_manifest_dict()))
    m = load_manifest(p)
    assert m.app_id == "org.gimp.GIMP"


def test_load_catalog_from_file(tmp_path):
    p = tmp_path / "catalog.json"
    p.write_text(json.dumps([_valid_manifest_dict(), _minimal_manifest_dict()]))
    cat = load_catalog(p)
    assert len(cat.apps) == 2


def test_load_manifest_invalid_json(tmp_path):
    p = tmp_path / "broken.json"
    p.write_text("{ not json ")
    with pytest.raises(ManifestError):
        load_manifest(p)


def test_parse_manifest_non_object_rejected():
    with pytest.raises(ManifestError):
        parse_manifest(["not", "an", "object"])


def test_dataclass_direct_construction_validates():
    # Constructing the dataclasses directly (the path the Kotlin/installer glue
    # mirrors) must validate, not just the JSON parse path.
    with pytest.raises(ManifestError):
        AppManifest(
            app_id="bad id",
            name="x",
            summary="y",
            entry=AppEntry(kind="exec", target="/usr/bin/x"),
        )
