"""Host-side structural check on BundledCatalog (NativeAlrRuntime.kt).

The BundledCatalog is the set of apps the in-app launcher offers to `apt install`.
The integration session device-tests them, but several invariants are cheaply and
deterministically checkable on the host from the Kotlin source — so a regression in a
catalog entry (mismatched appId/apt-name, missing binary path, an entry that
re-introduces the false "no systemd/dbus closure" reasoning) is caught before a device
run. This mirrors the project's other `*_sources.py` host tests that assert on source
text.

Pinned invariants:
  * the curation-bar doc-comment states the CORRECTED model (delta cascade-trigger
    test), not the disproven "closure must be base-GTK3-only / no systemd-dbus" claim;
  * every `CatalogApp` has appId / apt RootfsDep / a `/usr/bin/...` EXEC target;
  * for each non-terminal entry the appId equals the apt package name equals the EXEC
    binary basename (the `.desktop`-basename reconciliation contract);
  * the new host-audited LIKELY-PASS additions are present.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
KT = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/NativeAlrRuntime.kt"


@pytest.fixture(scope="module")
def kt_text() -> str:
    return KT.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def catalog_block(kt_text: str) -> str:
    """The BundledCatalog region: its curation-bar KDoc plus the `object { ... }` body.

    The curation-bar reasoning lives in the `/** ... */` doc-comment immediately above
    `object BundledCatalog`, so the block starts at that comment (the last `/**` before
    the object) to cover both the doc-comment assertions and the entry assertions.
    """
    obj = kt_text.index("object BundledCatalog")
    doc = kt_text.rfind("/**", 0, obj)
    start = doc if doc != -1 else obj
    return kt_text[start:]


# --------------------------------------------------------------------------- #
# entry extraction
# --------------------------------------------------------------------------- #
def _entries(block: str) -> list[dict[str, str]]:
    """Parse each `CatalogApp( ... )` into a {field: value} dict (string scrape).

    Robust enough for the pinned fields: appId, the apt RootfsDep ref, and the EXEC
    target. Values are taken from the first occurrence of each key inside the entry.
    """
    out: list[dict[str, str]] = []
    for m in re.finditer(r"CatalogApp\(", block):
        # find the matching close paren for this CatalogApp(
        i = m.end()
        depth = 1
        while i < len(block) and depth:
            if block[i] == "(":
                depth += 1
            elif block[i] == ")":
                depth -= 1
            i += 1
        body = block[m.end():i]
        entry: dict[str, str] = {}
        am = re.search(r'appId\s*=\s*"([^"]+)"', body)
        if am:
            entry["appId"] = am.group(1)
        em = re.search(r'LaunchEntry\(LaunchEntry\.EntryKind\.(\w+),\s*"([^"]+)"', body)
        if em:
            entry["entryKind"] = em.group(1)
            entry["entryTarget"] = em.group(2)
        rm = re.search(r'RootfsDep\(RootfsDepKind\.(\w+),\s*"([^"]+)"', body)
        if rm:
            entry["depKind"] = rm.group(1)
            entry["depRef"] = rm.group(2)
        cm = re.search(r"category\s*=\s*AppCategory\.(\w+)", body)
        if cm:
            entry["category"] = cm.group(1)
        out.append(entry)
    return out


def test_catalog_has_entries(catalog_block: str):
    entries = _entries(catalog_block)
    assert len(entries) >= 8, f"expected the full bundled catalog, got {len(entries)}"


def test_every_entry_well_formed(catalog_block: str):
    for e in _entries(catalog_block):
        assert "appId" in e, f"entry missing appId: {e}"
        assert e.get("entryKind") == "EXEC", f"{e['appId']}: expected EXEC entry"
        assert e.get("entryTarget", "").startswith("/usr/bin/"), (
            f"{e['appId']}: EXEC target must be an absolute /usr/bin path, got "
            f"{e.get('entryTarget')!r}"
        )
        assert e.get("depKind") == "APT", f"{e['appId']}: bundled entries are apt-installable"
        assert e.get("depRef"), f"{e['appId']}: missing apt package ref"


def test_appid_equals_apt_name_equals_binary_basename(catalog_block: str):
    # The reconciliation contract: appId == .desktop basename, and for these single-leaf
    # apps appId == apt package name == the EXEC binary basename. (Qalculate ships
    # qalculate-gtk for all three; x-prefixed Xlib apps likewise.)
    for e in _entries(catalog_block):
        app_id = e["appId"]
        binary = e["entryTarget"].rsplit("/", 1)[-1]
        assert e["depRef"] == app_id, (
            f"{app_id}: apt ref {e['depRef']!r} must equal appId for tile reconciliation"
        )
        assert binary == app_id, (
            f"{app_id}: EXEC binary basename {binary!r} must equal appId"
        )


def test_appids_unique(catalog_block: str):
    ids = [e["appId"] for e in _entries(catalog_block)]
    assert len(ids) == len(set(ids)), f"duplicate appIds: {ids}"


# --------------------------------------------------------------------------- #
# the corrected curation-bar reasoning
# --------------------------------------------------------------------------- #
def test_curation_bar_states_corrected_delta_model(catalog_block: str):
    # The doc-comment must explain the DELTA cascade-trigger model and explicitly note
    # that galculator's own proven closure drags systemd/dbus (the disproof of the old
    # "no systemd/dbus closure" rule).
    assert "app_closure_audit.py" in catalog_block
    assert "DELTA" in catalog_block
    assert "cascade" in catalog_block.lower()
    assert "galculator" in catalog_block
    # the specific exit-73 trigger families must be named
    for trig in ("perl-base", "gsettings-desktop-schemas", "session-migration"):
        assert trig in catalog_block, f"curation bar should name trigger {trig}"


def test_no_stale_no_systemd_dbus_closure_claim(catalog_block: str):
    # The disproven phrasing must be gone from the gpicview/xarchiver/sakura entries.
    # (We assert the specific false sentence fragments are absent.)
    stale_fragments = [
        "systemd/dbus/appstream 유지보수 스크립트가\n                // 없어",  # xarchiver old
        "systemd/dbus/appstream/policykit/accountsservice/dconf-service 전무",  # gpicview old
        "닫힘 전체가 base GTK3 스택",  # xarchiver old
    ]
    for frag in stale_fragments:
        assert frag not in catalog_block, f"stale disproven claim still present: {frag!r}"


# --------------------------------------------------------------------------- #
# the new host-audited additions are present
# --------------------------------------------------------------------------- #
def test_new_likely_pass_apps_present(catalog_block: str):
    ids = {e["appId"] for e in _entries(catalog_block)}
    # existing audited + new additions
    expected = {
        "galculator", "htop", "sakura", "l3afpad", "gpicview", "xarchiver",
        "viewnior", "xzgv", "xpdf", "qalculate-gtk", "nsxiv",
    }
    missing = expected - ids
    assert not missing, f"catalog missing expected entries: {sorted(missing)}"


def test_xpdf_and_nsxiv_marked_x11_path(catalog_block: str):
    # The Xlib apps must document the Xwayland (rootful) requirement, since they don't
    # use the Wayland GDK backend the GTK apps do.
    assert "Xwayland" in catalog_block
    # nsxiv must document its NoDisplay caveat (no auto-tile).
    assert "NoDisplay=true" in catalog_block
