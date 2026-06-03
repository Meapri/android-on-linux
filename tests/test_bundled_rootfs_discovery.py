"""Golden discovery test for the BUNDLED rootfs the launcher ships with.

NativeAlrRuntime (Phase 1) discovers installed Linux apps by scanning the rootfs
`usr/share/applications` for `.desktop` files via DesktopEntryScanner.kt — a faithful
Kotlin mirror of tools/desktop_entry.py (already unit-tested in test_desktop_entry.py).

This test pins the ACTUAL discovery target the integration session will device-test:
the one `.desktop` the bundled tiny-rootfs.tar contains (gimp.desktop). It asserts the
real file maps to the launcher entry the runtime expects (Exec field-code stripping,
visible non-terminal Application, Graphics category, binary present), so a regression in
the bundled rootfs or the parsing rules is caught host-side before a device run.

The .desktop body is extracted from the shipped tar so the fixture can NEVER drift from
what actually lands on the device. Skipped (not failed) if the payload is absent — large
binary assets may be excluded from some checkouts.
"""

from __future__ import annotations

import tarfile
from pathlib import Path

import pytest

from tools.desktop_entry import (
    app_id_from_path,
    expand_exec,
    is_visible,
    parse_desktop_entry,
)

ROOT = Path(__file__).resolve().parents[1]
ROOTFS_TAR = ROOT / "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
DESKTOP_MEMBER = "./usr/share/applications/gimp.desktop"
# The Exec program token; resolved against the rootfs bin dirs by the Kotlin scanner.
EXPECTED_BIN_BASENAME = "gimp-3.0"
EXPECTED_BIN_PATH = "./usr/bin/gimp-3.0"


def _tar_member_text(tar_path: Path, member: str) -> str | None:
    with tarfile.open(tar_path, "r:*") as tf:
        try:
            f = tf.extractfile(member)
        except KeyError:
            return None
        if f is None:
            return None
        return f.read().decode("utf-8", errors="replace")


@pytest.fixture(scope="module")
def gimp_desktop_text() -> str:
    if not ROOTFS_TAR.is_file():
        pytest.skip(f"bundled rootfs payload absent: {ROOTFS_TAR}")
    text = _tar_member_text(ROOTFS_TAR, DESKTOP_MEMBER)
    if text is None:
        pytest.skip(f"{DESKTOP_MEMBER} not in {ROOTFS_TAR.name}")
    return text


def test_bundled_rootfs_ships_one_known_desktop_entry():
    """The launcher's Phase-1 device target: exactly the gimp .desktop is present."""
    if not ROOTFS_TAR.is_file():
        pytest.skip(f"bundled rootfs payload absent: {ROOTFS_TAR}")
    with tarfile.open(ROOTFS_TAR, "r:*") as tf:
        desktops = [
            n for n in tf.getnames()
            if n.startswith("./usr/share/applications/") and n.endswith(".desktop")
        ]
    assert DESKTOP_MEMBER in desktops, desktops
    # The launcher binary the entry runs must actually exist in the rootfs.
    with tarfile.open(ROOTFS_TAR, "r:*") as tf:
        names = set(tf.getnames())
    assert EXPECTED_BIN_PATH in names


def test_gimp_desktop_parses_to_runnable_launcher_entry(gimp_desktop_text: str):
    """Real gimp.desktop → a visible, non-terminal Application with a clean argv."""
    entry = parse_desktop_entry(
        gimp_desktop_text, path="/usr/share/applications/gimp.desktop"
    )
    assert entry is not None
    assert entry.app_id == "gimp"  # basename without .desktop = stable launch key
    assert entry.name  # localized Name resolves (non-empty)
    assert is_visible(entry)  # NoDisplay/Hidden are not set
    assert entry.terminal is False  # GUI app — Phase 1 runs it (terminal apps are skipped)
    # Exec=gimp-3.0 %U → the %U field code is stripped, leaving just the program token.
    assert entry.exec_argv == [EXPECTED_BIN_BASENAME]
    # Categories=Graphics;... → first registered main category is Graphics (→ AppCategory.GRAPHICS).
    assert entry.primary_category == "Graphics"


def test_gimp_exec_field_codes_stripped():
    """Spec field codes never leak into the launched argv (the Kotlin scanner mirrors this)."""
    assert expand_exec("gimp-3.0 %U") == ["gimp-3.0"]
    assert expand_exec("gimp-3.0 %F %f %u") == ["gimp-3.0"]
    assert expand_exec("app --flag %k", desktop_path="/x/app.desktop") == [
        "app",
        "--flag",
        "/x/app.desktop",
    ]


def test_app_id_from_desktop_basename():
    assert app_id_from_path("/usr/share/applications/gimp.desktop") == "gimp"
