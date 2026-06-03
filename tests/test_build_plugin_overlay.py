"""Host tests for tools/build_plugin_overlay.py — the DLOPEN-PLUGIN overlay builder.

The §5-E deb closure follows DT_NEEDED ONLY, so it DROPS dlopen-loaded plugin
modules (NSS PKCS#11 plugins, gdk-pixbuf image loaders, GIO TLS/proxy backends,
GTK immodules/print backends, gstreamer elements). This builder generalizes the
proven ``build_nss_overlay`` into a GROUP-configurable overlay that stages those
module CLASSES at their canonical Debian rootfs paths and REGENERATES the loader
manifest caches host-side.

All tests here are OFFLINE: the full end-to-end build is exercised against a tiny
synthetic .deb mirror + an injected in-memory Packages index (the same technique
the module's own ``--selftest`` uses), so no network is touched. The real
NETWORK build (noble libnss3 / libgdk-pixbuf-2.0-0 / glib-networking) + the
on-device dlopen are the integration/device gate.
"""

from __future__ import annotations

import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from tools import build_plugin_overlay as bpo
from tools.build_plugin_overlay import (
    DEFAULT_GROUPS,
    GIO_MODULE_EXTENSION_POINTS,
    GROUPS,
    LIBDIR,
    PIXBUF_LOADER_META,
    build,
    render_giomodule_cache,
    render_pixbuf_cache,
    resolve_filenames,
)
from tools.stage_tar_spec import validate_stage_tar

_HAS_AR = shutil.which("ar") is not None


# --------------------------------------------------------------------------- #
# Module file + group catalog
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bpo.__file__).is_file()


def test_default_groups_are_the_universal_three():
    # default = the most universally needed, small groups; gstreamer is NOT default
    assert DEFAULT_GROUPS == ("nss", "gdk-pixbuf", "gio")
    assert "gstreamer" not in DEFAULT_GROUPS


def test_every_default_group_is_known():
    for g in DEFAULT_GROUPS:
        assert g in GROUPS


def test_nss_group_generalizes_build_nss_overlay():
    # the proven NSS module set, now expressed as a group with flat aliases
    nss = GROUPS["nss"]
    assert nss.packages == ("libnss3",)
    for mod in ("libsoftokn3.so", "libfreebl3.so", "libfreeblpriv3.so",
                "libnssckbi.so", "libnssdbm3.so"):
        assert mod in nss.also_flat
    # the two load-bearing ones are required (build errors if missing)
    assert "libsoftokn3.so" in nss.required
    assert "libfreebl3.so" in nss.required


def test_gdk_pixbuf_group_pulls_librsvg_for_svg_loader():
    grp = GROUPS["gdk-pixbuf"]
    assert "libgdk-pixbuf-2.0-0" in grp.packages
    assert "librsvg2-common" in grp.packages  # svg loader ships separately
    assert grp.cache is not None and grp.cache.kind == "pixbuf"
    assert grp.cache.rel_path.endswith("loaders.cache")


def test_gio_group_pulls_tls_and_settings_backends():
    grp = GROUPS["gio"]
    assert "glib-networking" in grp.packages          # gio-tls-backend (gnutls)
    assert "dconf-gsettings-backend" in grp.packages  # gio-settings-backend
    assert grp.cache is not None and grp.cache.kind == "giomodule"


def test_gstreamer_is_large_and_opt_in_only():
    assert GROUPS["gstreamer"].large is True


def test_all_groups_have_at_least_one_package_and_module_dir():
    for name, grp in GROUPS.items():
        assert grp.packages, name
        assert grp.module_dirs, name


# --------------------------------------------------------------------------- #
# gdk-pixbuf loaders.cache regeneration (byte-exact text format)
# --------------------------------------------------------------------------- #

def test_pixbuf_cache_header_matches_query_loaders_format():
    body = render_pixbuf_cache([])
    assert body.startswith("# GdkPixbuf Image Loader Modules file")
    assert "# Automatically generated file, do not edit" in body


def test_pixbuf_cache_svg_stanza_is_complete():
    rel = f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so"
    body = render_pixbuf_cache([rel])
    # the module path is rendered rootfs-ABSOLUTE (how the loader resolves it)
    assert f'"/{rel}"' in body
    # id / flags / namespace / desc / license line
    assert '"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"' in body
    # mime + extension lists, each terminated with the empty-string sentinel
    assert '"image/svg+xml"' in body
    assert '"svg" "svgz" "svg.gz" ""' in body


def test_pixbuf_cache_only_includes_known_loaders():
    known = f"{LIBDIR}/x/loaders/libpixbufloader-png.so"
    unknown = f"{LIBDIR}/x/loaders/libpixbufloader-NOTREAL.so"
    body = render_pixbuf_cache([known, unknown])
    assert "libpixbufloader-png.so" in body
    assert "libpixbufloader-NOTREAL.so" not in body


def test_pixbuf_cache_entries_sorted_for_determinism():
    a = f"{LIBDIR}/x/loaders/libpixbufloader-xpm.so"
    b = f"{LIBDIR}/x/loaders/libpixbufloader-bmp.so"
    body = render_pixbuf_cache([a, b])
    assert body.index("libpixbufloader-bmp.so") < body.index("libpixbufloader-xpm.so")
    # rebuilding with the inputs reversed yields the identical bytes
    assert render_pixbuf_cache([b, a]) == body


def test_every_pixbuf_meta_entry_renders_without_error():
    rels = [f"{LIBDIR}/x/loaders/{name}" for name in PIXBUF_LOADER_META]
    body = render_pixbuf_cache(rels)
    # one quoted module-path line per loader (the abspath line)
    for name in PIXBUF_LOADER_META:
        assert f'/x/loaders/{name}"' in body


# --------------------------------------------------------------------------- #
# GIO giomodule.cache regeneration
# --------------------------------------------------------------------------- #

def test_giomodule_cache_maps_gnutls_to_tls_backend():
    body = render_giomodule_cache([f"{LIBDIR}/gio/modules/libgiognutls.so"])
    assert "libgiognutls.so: gio-tls-backend,gio-dtls-backend" in body


def test_giomodule_cache_maps_dconf_to_settings_backend():
    body = render_giomodule_cache([f"{LIBDIR}/gio/modules/libdconfsettings.so"])
    assert "libdconfsettings.so: gio-settings-backend" in body


def test_giomodule_cache_emits_bare_line_for_unknown_module():
    body = render_giomodule_cache([f"{LIBDIR}/gio/modules/libgiomystery.so"])
    # GLib tolerates an empty extension-point list — still record the module
    assert "libgiomystery.so: " in body


def test_giomodule_cache_is_sorted_and_deterministic():
    a = f"{LIBDIR}/gio/modules/libgiognutls.so"
    b = f"{LIBDIR}/gio/modules/libdconfsettings.so"
    assert render_giomodule_cache([a, b]) == render_giomodule_cache([b, a])


def test_known_gio_extension_points_are_valid_names():
    # every mapped extension point is a real GIOExtensionPoint name (gio-*)
    for mod, eps in GIO_MODULE_EXTENSION_POINTS.items():
        assert eps, mod
        for ep in eps:
            assert ep.startswith("gio-"), (mod, ep)


# --------------------------------------------------------------------------- #
# Module discovery
# --------------------------------------------------------------------------- #

def test_discover_modules_finds_so_skips_symlinks_and_data(tmp_path: Path):
    root = tmp_path
    ldir = root / LIBDIR / "gio/modules"
    ldir.mkdir(parents=True)
    (ldir / "libgiognutls.so").write_bytes(b"\x7fELF")
    (ldir / "giomodule.cache").write_bytes(b"stale")  # not a .so → ignored
    (ldir / "alias.so").symlink_to("libgiognutls.so")  # symlink → skipped
    found = bpo._discover_modules(root, (f"{LIBDIR}/gio/modules",))
    names = {Path(r).name for r in found}
    assert "libgiognutls.so" in names
    assert "giomodule.cache" not in names
    assert "alias.so" not in names


# --------------------------------------------------------------------------- #
# CLI group parsing
# --------------------------------------------------------------------------- #

def test_parse_group_args_default(monkeypatch):
    import argparse
    ns = argparse.Namespace(all=False, groups=None)
    assert bpo._parse_group_args(ns) == list(DEFAULT_GROUPS)


def test_parse_group_args_comma_and_repeat_dedup():
    import argparse
    ns = argparse.Namespace(all=False, groups=["nss,gio", "gio", "gtk3"])
    assert bpo._parse_group_args(ns) == ["nss", "gio", "gtk3"]


def test_parse_group_args_all_excludes_large_gstreamer():
    import argparse
    ns = argparse.Namespace(all=True, groups=None)
    got = bpo._parse_group_args(ns)
    assert "gstreamer" not in got
    assert "nss" in got and "gdk-pixbuf" in got and "gio" in got


def test_unknown_group_rejected():
    with pytest.raises(ValueError):
        bpo._resolve_groups(["does-not-exist"])


# --------------------------------------------------------------------------- #
# resolve_filenames with an injected index (offline)
# --------------------------------------------------------------------------- #

def test_resolve_filenames_uses_injected_index():
    index = {"libnss3": {"Package": "libnss3", "Filename": "pool/x/libnss3.deb"}}
    got = resolve_filenames(("libnss3", "absent-pkg"), index=index)
    assert got["libnss3"] == "pool/x/libnss3.deb"
    assert got["absent-pkg"] is None


# --------------------------------------------------------------------------- #
# End-to-end build over a synthetic .deb mirror (offline, needs `ar`)
# --------------------------------------------------------------------------- #

def _make_deb(pool: Path, stage_root: Path, name: str, files: dict[str, bytes]) -> str:
    """Build a real (ar+tar) .deb under pool/ and return its pool-relative path.

    The inner data member basename MUST be ``data.tar`` (extract_deb looks for a
    member named ``data.tar*``); `ar` records members by basename, so build each
    .deb from its own staging dir.
    """
    stage = stage_root / ("_stage_" + name)
    stage.mkdir(parents=True, exist_ok=True)
    droot = stage_root / ("_d_" + name)
    for rel, data in files.items():
        fp = droot / rel
        fp.parent.mkdir(parents=True, exist_ok=True)
        fp.write_bytes(data)
    with tarfile.open(stage / "data.tar", "w") as dt:
        for rel in files:
            dt.add(droot / rel, arcname="./" + rel)
    (stage / "debian-binary").write_text("2.0\n")
    (stage / "control.tar").write_bytes(b"")
    deb = pool / (name + ".deb")
    subprocess.run(
        ["ar", "qcS", str(deb), "debian-binary", "control.tar", "data.tar"],
        check=True, capture_output=True, cwd=str(stage),
    )
    return f"pool/{name}.deb"


@pytest.fixture()
def synthetic_built(tmp_path: Path):
    """Build the nss+gdk-pixbuf+gio overlay from a tiny synthetic noble mirror."""
    if not _HAS_AR:
        pytest.skip("needs `ar` to synthesize .deb archives")
    pool = tmp_path / "pool"
    pool.mkdir()

    nss_fn = _make_deb(pool, tmp_path, "fakenss", {
        f"{LIBDIR}/libsoftokn3.so": b"\x7fELFsoftokn",
        f"{LIBDIR}/libfreebl3.so": b"\x7fELFfreebl",
        f"{LIBDIR}/libnssckbi.so": b"\x7fELFnssckbi",
    })
    pix_fn = _make_deb(pool, tmp_path, "fakepix", {
        f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-png.so": b"\x7fELFpng",
    })
    svg_fn = _make_deb(pool, tmp_path, "fakesvg", {
        f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so": b"\x7fELFsvg",
    })
    gio_fn = _make_deb(pool, tmp_path, "fakegio", {
        f"{LIBDIR}/gio/modules/libgiognutls.so": b"\x7fELFgnutls",
    })

    index = {
        "libnss3": {"Package": "libnss3", "Filename": nss_fn},
        "libgdk-pixbuf-2.0-0": {"Package": "libgdk-pixbuf-2.0-0", "Filename": pix_fn},
        "librsvg2-common": {"Package": "librsvg2-common", "Filename": svg_fn},
        "glib-networking": {"Package": "glib-networking", "Filename": gio_fn},
        "dconf-gsettings-backend": {"Package": "dconf-gsettings-backend",
                                    "Filename": None},  # absent → skipped, not fatal
    }
    out = tmp_path / "plugin-stage.tar"
    res = build(out, ["nss", "gdk-pixbuf", "gio"], tmp_path / "cache",
                mirror="file://" + str(tmp_path), index=index)
    with tarfile.open(out) as t:
        names = set(t.getnames())
        bodies = {m.name: t.extractfile(m).read()
                  for m in t.getmembers() if m.isfile()}
    return res, names, bodies, out


def test_build_stages_nss_flat_and_nss_search_paths(synthetic_built):
    _, names, _, _ = synthetic_built
    # the proven NSS dual-path staging (flat + nss/ search dir)
    assert f"./{LIBDIR}/libsoftokn3.so" in names
    assert f"./{LIBDIR}/nss/libsoftokn3.so" in names
    assert f"./{LIBDIR}/libfreebl3.so" in names
    assert f"./{LIBDIR}/nss/libfreebl3.so" in names


def test_build_stages_pixbuf_loaders_at_canonical_paths(synthetic_built):
    _, names, _, _ = synthetic_built
    assert (f"./{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-png.so"
            in names)
    assert (f"./{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so"
            in names)


def test_build_regenerates_loaders_cache_with_staged_loaders(synthetic_built):
    _, names, bodies, _ = synthetic_built
    cache_rel = f"./{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders.cache"
    assert cache_rel in names
    body = bodies[cache_rel]
    assert b'"svg"' in body and b'"png"' in body
    # cache references the rootfs-absolute loader path
    assert b'"/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so"' in body


def test_build_regenerates_giomodule_cache(synthetic_built):
    _, names, bodies, _ = synthetic_built
    cache_rel = f"./{LIBDIR}/gio/modules/giomodule.cache"
    assert cache_rel in names
    assert b"libgiognutls.so: gio-tls-backend" in bodies[cache_rel]


def test_build_modules_are_executable(synthetic_built):
    # ALR file-backed PROT_EXEC dlopen rejects a non-x .so — modules MUST be 0755
    _, _, _, out = synthetic_built
    with tarfile.open(out) as t:
        for m in t.getmembers():
            if m.isfile() and (m.name.endswith(".so") or ".so." in m.name):
                assert (m.mode & 0o111), m.name


def test_build_all_members_dot_rooted_and_no_symlinks(synthetic_built):
    _, names, _, out = synthetic_built
    assert all(n.startswith("./") or n == "." for n in names)
    with tarfile.open(out) as t:
        assert not any(m.issym() or m.islnk() for m in t.getmembers())


def test_build_missing_package_is_skipped_not_fatal(synthetic_built):
    res, _, _, _ = synthetic_built
    gio = next(g for g in res.groups if g.group == "gio")
    # dconf-gsettings-backend had Filename=None in the fixture index
    assert "dconf-gsettings-backend" in gio.skipped_packages


def test_build_required_module_gate_passes_when_present(synthetic_built):
    res, _, _, _ = synthetic_built
    nss = next(g for g in res.groups if g.group == "nss")
    assert nss.missing_required == ()


def test_build_required_module_gate_flags_missing(tmp_path: Path):
    if not _HAS_AR:
        pytest.skip("needs `ar`")
    pool = tmp_path / "pool"
    pool.mkdir()
    # a libnss3 .deb that is MISSING libfreebl3.so (a required module)
    nss_fn = _make_deb(pool, tmp_path, "brokennss", {
        f"{LIBDIR}/libsoftokn3.so": b"\x7fELFsoftokn",
    })
    index = {"libnss3": {"Package": "libnss3", "Filename": nss_fn}}
    out = tmp_path / "p.tar"
    res = build(out, ["nss"], tmp_path / "cache",
                mirror="file://" + str(tmp_path), index=index)
    nss = next(g for g in res.groups if g.group == "nss")
    assert "libfreebl3.so" in nss.missing_required


def test_built_overlay_is_stage_tar_conformant(synthetic_built):
    _, _, _, out = synthetic_built
    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors


def test_built_overlay_conformant_against_real_base_rootfs(synthetic_built):
    # the overlay is purely additive — it must NOT downgrade any base library
    _, _, _, out = synthetic_built
    base = Path(__file__).resolve().parents[1] / \
        "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
    if not base.is_file():
        pytest.skip("base rootfs tar not present")
    rep = validate_stage_tar(str(out), base=str(base))
    assert rep.conformant, rep.errors
    assert rep.errors == []


# --------------------------------------------------------------------------- #
# Builder selftest
# --------------------------------------------------------------------------- #

def test_builder_selftest_passes():
    assert bpo._selftest() == 0
