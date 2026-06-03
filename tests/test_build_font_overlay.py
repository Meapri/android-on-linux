"""Host tests for tools/build_font_overlay.py — the FONT breadth overlay builder.

Fully OFFLINE: every test either drives the pure selection/CLI logic or builds the
overlay from a synthetic ``source_root`` (an extracted-deb stand-in). No network,
no real .deb. Validates §5-E conformance via tools.stage_tar_spec.
"""
from __future__ import annotations

import io
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.build_font_overlay import (  # noqa: E402
    ALR_FONTS_CONF,
    FAMILIES,
    FAMILIES_BY_KEY,
    FONTCONFIG_DIR,
    FONTS_CONF,
    FONT_PREFIX,
    FontFamily,
    build_font_overlay,
    main,
    select_families,
)
from tools.stage_tar_spec import validate_stage_tar  # noqa: E402


# --------------------------------------------------------------------------- #
# fixtures: a synthetic extracted-deb root with a few fake font files
# --------------------------------------------------------------------------- #

def _make_source_root(tmp_path: Path, families=None) -> Path:
    """Create an extracted-deb stand-in: each family gets 3 fake .ttf files under
    usr/share/fonts/truetype/<subdir>/ plus an irrelevant non-font file."""
    families = families or list(FAMILIES)
    root = tmp_path / "src"
    for fam in families:
        d = root / FONT_PREFIX / fam.subdir
        d.mkdir(parents=True)
        for i, face in enumerate(("Regular", "Bold", "Italic")):
            # Distinct, non-trivial bytes so total_bytes is meaningful.
            (d / f"{fam.subdir}-{face}.ttf").write_bytes(b"\x00\x01\x00\x00TTF" + bytes([i]) * 200)
        # a stray non-font file that must NOT be packed
        (d / "AUTHORS").write_text("not a font\n")
    return root


# --------------------------------------------------------------------------- #
# pure selection logic
# --------------------------------------------------------------------------- #

def test_default_selection_is_dejavu_liberation_noto_no_cjk():
    keys = [f.key for f in select_families()]
    assert keys == ["dejavu", "liberation", "noto"]
    assert "cjk" not in keys


def test_cjk_is_opt_in_and_appends_last():
    keys = [f.key for f in select_families(cjk=True)]
    assert keys == ["dejavu", "liberation", "noto", "cjk"]


def test_each_family_flag_can_be_disabled_independently():
    assert [f.key for f in select_families(dejavu=False)] == ["liberation", "noto"]
    assert [f.key for f in select_families(liberation=False)] == ["dejavu", "noto"]
    assert [f.key for f in select_families(noto=False)] == ["dejavu", "liberation"]


def test_catalog_maps_each_family_to_its_owning_noble_deb():
    assert FAMILIES_BY_KEY["dejavu"].package == "fonts-dejavu-core"
    assert FAMILIES_BY_KEY["liberation"].package == "fonts-liberation2"
    assert FAMILIES_BY_KEY["noto"].package == "fonts-noto-core"
    assert FAMILIES_BY_KEY["cjk"].package == "fonts-noto-cjk"
    # CJK is the only opt-in family.
    assert [f.key for f in FAMILIES if not f.default] == ["cjk"]


# --------------------------------------------------------------------------- #
# offline build from a synthetic source_root
# --------------------------------------------------------------------------- #

def test_build_packs_fonts_at_truetype_subdir(tmp_path):
    src = _make_source_root(tmp_path)
    out = tmp_path / "font-stage.tar"
    res = build_font_overlay(out, select_families(), source_root=src)

    with tarfile.open(out) as t:
        names = set(t.getnames())

    # DejaVu/Liberation/Noto each contribute 3 faces at the expected dir.
    assert f"./{FONT_PREFIX}/dejavu/dejavu-Regular.ttf" in names
    assert f"./{FONT_PREFIX}/liberation2/liberation2-Bold.ttf" in names
    assert f"./{FONT_PREFIX}/noto/noto-Italic.ttf" in names
    assert res.font_count == 9
    assert res.per_family == {"dejavu": 3, "liberation": 3, "noto": 3}
    # CJK not staged by default.
    assert not any("noto-cjk" in n for n in names)


def test_build_excludes_non_font_files(tmp_path):
    src = _make_source_root(tmp_path)
    out = tmp_path / "font-stage.tar"
    build_font_overlay(out, select_families(), source_root=src)
    with tarfile.open(out) as t:
        names = t.getnames()
    assert not any(n.endswith("AUTHORS") for n in names)
    assert all(n.endswith((".ttf", ".conf")) for n in names if "." in Path(n).name)


def test_build_includes_fontconfig_config(tmp_path):
    src = _make_source_root(tmp_path)
    out = tmp_path / "font-stage.tar"
    res = build_font_overlay(out, select_families(), source_root=src)

    with tarfile.open(out) as t:
        members = {m.name: m for m in t.getmembers()}
        conf = t.extractfile(f"./{FONTCONFIG_DIR}/fonts.conf").read().decode()
        alr = t.extractfile(f"./{FONTCONFIG_DIR}/conf.d/00-alr-fonts.conf").read().decode()

    assert f"./{FONTCONFIG_DIR}/fonts.conf" in members
    assert f"./{FONTCONFIG_DIR}/conf.d/00-alr-fonts.conf" in members
    assert res.config_files == (
        f"{FONTCONFIG_DIR}/fonts.conf",
        f"{FONTCONFIG_DIR}/conf.d/00-alr-fonts.conf",
    )
    # fonts.conf points at the staged tree + the conf.d dir.
    assert "<dir>/usr/share/fonts</dir>" in conf
    assert "/etc/fonts/conf.d" in conf
    # conf.d aliases the generic families to the staged faces and maps MS names.
    assert "sans-serif" in alr and "DejaVu Sans" in alr
    assert "Arial" in alr and "Liberation Sans" in alr
    assert "Times New Roman" in alr and "Liberation Serif" in alr


def test_no_config_flag_packs_only_fonts(tmp_path):
    src = _make_source_root(tmp_path)
    out = tmp_path / "font-stage.tar"
    res = build_font_overlay(out, select_families(), source_root=src, include_config=False)
    with tarfile.open(out) as t:
        names = t.getnames()
    assert not any(n.endswith(".conf") for n in names)
    assert res.config_files == ()


def test_cjk_family_is_staged_when_requested(tmp_path):
    src = _make_source_root(tmp_path)  # includes a noto-cjk subdir
    out = tmp_path / "font-stage.tar"
    res = build_font_overlay(out, select_families(cjk=True), source_root=src)
    with tarfile.open(out) as t:
        names = t.getnames()
    assert any(n.startswith(f"./{FONT_PREFIX}/noto-cjk/") for n in names)
    assert res.per_family.get("cjk") == 3


def test_missing_family_fonts_raise(tmp_path):
    # source_root with NO font files for the requested family -> hard error.
    root = tmp_path / "src"
    (root / FONT_PREFIX / "dejavu").mkdir(parents=True)  # empty dir, no .ttf
    out = tmp_path / "font-stage.tar"
    with pytest.raises(RuntimeError, match="no font files"):
        build_font_overlay(out, [FAMILIES_BY_KEY["dejavu"]], source_root=root)


# --------------------------------------------------------------------------- #
# §5-E conformance + base safety
# --------------------------------------------------------------------------- #

def test_overlay_is_stage_tar_conformant(tmp_path):
    src = _make_source_root(tmp_path)
    out = tmp_path / "font-stage.tar"
    build_font_overlay(out, select_families(), source_root=src)
    rep = validate_stage_tar(out)
    assert rep.conformant, rep.errors
    assert not rep.errors


def test_overlay_adds_only_new_paths_vs_base(tmp_path):
    """Against a base that ships fontconfig libs but no fonts, the overlay is pure
    new payload — no downgrade, no §5-E violation."""
    src = _make_source_root(tmp_path)
    out = tmp_path / "font-stage.tar"
    build_font_overlay(out, select_families(), source_root=src)

    base = tmp_path / "base.tar"
    with tarfile.open(base, "w") as bt:
        # base provides libfontconfig (a lib) + an empty /usr/share/fonts dir.
        ti = tarfile.TarInfo("./usr/lib/aarch64-linux-gnu/libfontconfig.so.1")
        payload = b"\x7fELF-fontconfig"
        ti.size = len(payload)
        bt.addfile(ti, io.BytesIO(payload))
        d = tarfile.TarInfo("./usr/share/fonts/")
        d.type = tarfile.DIRTYPE
        bt.addfile(d)

    rep = validate_stage_tar(out, base=base)
    assert rep.conformant, rep.errors


def test_all_members_are_dot_rooted_and_world_readable(tmp_path):
    src = _make_source_root(tmp_path)
    out = tmp_path / "font-stage.tar"
    build_font_overlay(out, select_families(cjk=True), source_root=src)
    with tarfile.open(out) as t:
        for m in t.getmembers():
            assert m.name.startswith("./"), m.name
            assert not m.issym()  # font overlay ships no symlinks
            if m.isfile():
                assert m.mode & 0o444, oct(m.mode)  # readable


# --------------------------------------------------------------------------- #
# config content sanity (static, no I/O)
# --------------------------------------------------------------------------- #

def test_static_config_is_well_formed_xml():
    import xml.dom.minidom as minidom
    # Both configs must parse as XML (DTD is external; minidom ignores it).
    minidom.parseString(FONTS_CONF)
    minidom.parseString(ALR_FONTS_CONF)


def test_config_declares_a_guest_writable_cachedir():
    # The guest rebuilds its own cache on first run — config must name a cachedir.
    assert "<cachedir>" in FONTS_CONF
    assert "/var/cache/fontconfig" in FONTS_CONF


# --------------------------------------------------------------------------- #
# CLI --list (no network)
# --------------------------------------------------------------------------- #

def test_cli_list_runs_offline_and_names_packages(capsys):
    rc = main(["--list"])
    assert rc == 0
    out = capsys.readouterr().out
    for pkg in ("fonts-dejavu-core", "fonts-liberation2", "fonts-noto-core", "fonts-noto-cjk"):
        assert pkg in out
    assert FONT_PREFIX in out
    assert "00-alr-fonts.conf" in out


def test_cli_list_via_subprocess_module_invocation():
    """The literal HOST GATE: `python3 -m tools.build_font_overlay --list` exits 0."""
    proc = subprocess.run(
        [sys.executable, "-m", "tools.build_font_overlay", "--list"],
        cwd=str(ROOT), capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr
    assert "fonts-dejavu-core" in proc.stdout
