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
MODELS = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/AppModels.kt"


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
        pm = re.search(r"provision\s*=\s*ProvisionKind\.(\w+)", body)
        # default provision is APT (the field is omitted on normal apt entries).
        entry["provision"] = pm.group(1) if pm else "APT"
        lm = re.search(r'launchActivity\s*=\s*"([^"]+)"', body)
        if lm:
            entry["launchActivity"] = lm.group(1)
        om = re.search(r'overlayBinaryPath\s*=\s*"([^"]+)"', body)
        if om:
            entry["overlayBinaryPath"] = om.group(1)
        out.append(entry)
    return out


def _apt_entries(block: str) -> list[dict[str, str]]:
    """Only the normal apt-provisioned entries (provision==APT).

    The apt-specific invariants (a /usr/bin EXEC target, an APT RootfsDep, appId==apt-name==
    binary basename) apply ONLY to apt apps. OVERLAY-provisioned apps (chromium) are a
    distinct class with their own contract (see the chromium tests below), so they are
    excluded here.
    """
    return [e for e in _entries(block) if e.get("provision") == "APT"]


def test_catalog_has_entries(catalog_block: str):
    entries = _entries(catalog_block)
    assert len(entries) >= 8, f"expected the full bundled catalog, got {len(entries)}"


def test_every_entry_well_formed(catalog_block: str):
    # All entries declare an EXEC entrypoint; the apt-specific shape (a /usr/bin target + an APT
    # RootfsDep) is required only of the apt-provisioned class (chromium is OVERLAY — exempt).
    for e in _entries(catalog_block):
        assert "appId" in e, f"entry missing appId: {e}"
        assert e.get("entryKind") == "EXEC", f"{e['appId']}: expected EXEC entry"
    for e in _apt_entries(catalog_block):
        assert e.get("entryTarget", "").startswith("/usr/bin/"), (
            f"{e['appId']}: EXEC target must be an absolute /usr/bin path, got "
            f"{e.get('entryTarget')!r}"
        )
        assert e.get("depKind") == "APT", f"{e['appId']}: bundled apt entries are apt-installable"
        assert e.get("depRef"), f"{e['appId']}: missing apt package ref"


def test_appid_equals_apt_name_equals_binary_basename(catalog_block: str):
    # The reconciliation contract: appId == .desktop basename. For the simple single-leaf
    # apps that is also == apt package name == the EXEC binary basename (galculator,
    # qalculate ships qalculate-gtk for all three). GNOME-platform apps are the exception:
    # their .desktop basename is the reverse-DNS app-id (org.gnome.Calculator) while the apt
    # package + binary are the short name (gnome-calculator) — so for those we require
    # apt-ref == binary basename, and the appId to be the reverse-DNS form of the binary.
    # OVERLAY apps (chromium) have NO apt ref and a reverse-DNS id that is NOT a GNOME pair, so
    # they are excluded here (their contract is checked by the chromium tests below).
    for e in _apt_entries(catalog_block):
        app_id = e["appId"]
        binary = e["entryTarget"].rsplit("/", 1)[-1]
        if "." in app_id:  # reverse-DNS GNOME-platform app-id
            assert e["depRef"] == binary, (
                f"{app_id}: apt ref {e['depRef']!r} must equal the EXEC binary basename "
                f"{binary!r} for a reverse-DNS GNOME app"
            )
            # the reverse-DNS id's last segment should relate to the binary (Calculator ↔
            # gnome-calculator) — assert the binary is gnome-* and the id is org.gnome.*.
            assert app_id.startswith("org.gnome.") and binary.startswith("gnome-"), (
                f"{app_id}: reverse-DNS app-id must be an org.gnome.* / gnome-* pair, got "
                f"binary {binary!r}"
            )
        else:
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
    # The galculator-class LIKELY-PASS set, the gnome-platform org.gnome.Calculator, AND the
    # general-maintscript-shim re-adds: the X11-image-viewer class (nsxiv/feh/qiv/xpdf) + the
    # apt-Qt-GUI class (qpdfview), all now LIKELY-PASS (x11-common/libpaper1 shim-neutralized).
    expected = {
        "galculator", "htop", "sakura", "l3afpad", "gpicview", "xarchiver",
        "viewnior", "xzgv", "xli", "qalculate-gtk", "mate-calc", "geany",
        "org.gnome.Calculator",
        # re-added via the general shim:
        "nsxiv", "feh", "qiv", "xpdf", "qpdfview",
    }
    missing = expected - ids
    assert not missing, f"catalog missing expected entries: {sorted(missing)}"


def test_x11_viewer_and_qt_apps_readded_via_general_shim(catalog_block: str):
    # xpdf + nsxiv (+ feh/qiv) were RE-ADDED once the maintscript-shim was generalized to every
    # install: their only blocker (x11-common exit 127 + libpaper1 exit 2) is now neutralized.
    # qpdfview is the new apt-Qt-GUI entry (x11-common-only blocker). The source must document
    # the unlock (the general shim, the empirical exit codes it neutralizes).
    ids = {e["appId"] for e in _entries(catalog_block)}
    for app in ("xpdf", "nsxiv", "feh", "qiv", "qpdfview"):
        assert app in ids, f"{app} must be RE-ADDED (general maintscript-shim unlock)"
    # the unlock must be explained with the empirical exit codes + the general-shim mechanism.
    assert "x11-common" in catalog_block and "libpaper1" in catalog_block
    assert "exit 127" in catalog_block and "exit 2" in catalog_block
    assert "maintscript-shim" in catalog_block
    # qpdfview must note it is the apt-Qt-GUI class routed through Xwayland.
    assert "qpdfview" in catalog_block and "Qt" in catalog_block


def test_readded_x11_apps_marked_needs_xwayland(catalog_block: str):
    # The re-added X11/Qt apps must be flagged needsXwayland=true so XwaylandLaunch routes them
    # (libX11/Motif/Qt-xcb, no libwayland-client). Scrape each entry's needsXwayland line.
    for m in re.finditer(r"CatalogApp\(", catalog_block):
        i = m.end()
        depth = 1
        while i < len(catalog_block) and depth:
            if catalog_block[i] == "(":
                depth += 1
            elif catalog_block[i] == ")":
                depth -= 1
            i += 1
        body = catalog_block[m.end():i]
        am = re.search(r'appId\s*=\s*"([^"]+)"', body)
        if am and am.group(1) in ("nsxiv", "feh", "qiv", "xpdf", "qpdfview", "xzgv", "xli"):
            assert "needsXwayland = true" in body, (
                f"{am.group(1)} (X11-only/Qt-xcb) must be needsXwayland=true for routing"
            )


def test_gnome_calculator_documents_the_two_part_fix(catalog_block: str):
    # The gnome-calculator entry must explain BOTH halves of the TASK-A unlock so the
    # catalog stays self-documenting: (1) install-configure neutralizer + precompiled
    # gschemas, (2) the runtime session-dbus shim.
    assert "org.gnome.Calculator" in catalog_block
    assert "gschemas" in catalog_block.lower() or "gschemas.compiled" in catalog_block
    assert "dbus-daemon" in catalog_block or "dbus-run-session" in catalog_block
    # honest device-pending note (do not overclaim a device run)
    assert "device" in catalog_block.lower()


# --------------------------------------------------------------------------- #
# Chromium — the OVERLAY-provisioned, ChromiumStandalone-launched special case
# --------------------------------------------------------------------------- #
def _chromium_entry(block: str) -> dict[str, str]:
    hits = [e for e in _entries(block) if e["appId"] == "org.chromium.Chromium"]
    assert hits, "Chromium catalog entry (appId=org.chromium.Chromium) must be present"
    return hits[0]


def test_chromium_entry_present_in_internet_category(catalog_block: str):
    e = _chromium_entry(catalog_block)
    assert e.get("entryKind") == "EXEC"
    # chromium's binary lives under /usr/lib/chromium (NOT /usr/bin) — the overlay layout.
    assert e.get("entryTarget") == "/usr/lib/chromium/chromium"
    assert e.get("category") == "INTERNET", "Chromium belongs in the INTERNET category"


def test_chromium_is_overlay_provisioned_not_apt(catalog_block: str):
    e = _chromium_entry(catalog_block)
    # The defining special-case: provision=OVERLAY, NO apt RootfsDep (it is NOT apt-installable;
    # noble's chromium apt package is a snap stub).
    assert e.get("provision") == "OVERLAY", "Chromium must be provision=ProvisionKind.OVERLAY"
    assert "depRef" not in e, "Chromium must NOT carry an apt RootfsDep (it is overlay-provisioned)"
    # the installed-probe binary path (rootfs-relative) must be declared.
    assert e.get("overlayBinaryPath") == "usr/lib/chromium/chromium", (
        "Chromium must declare overlayBinaryPath = usr/lib/chromium/chromium (the installed-probe)"
    )


def test_chromium_launches_via_chromium_standalone_alias(catalog_block: str):
    e = _chromium_entry(catalog_block)
    # It must launch via the LEAN runChromiumStandalone path — the .ui.ChromiumStandalone alias —
    # NOT the generic RunningSurfaceActivity (which would OOM the browser+renderer re-maps).
    assert e.get("launchActivity") == ".ui.ChromiumStandalone", (
        "Chromium must set launchActivity = .ui.ChromiumStandalone (the lean memory-headroom path)"
    )


def test_chromium_entry_documents_the_two_special_cases(catalog_block: str):
    # The entry must be self-documenting about WHY it is special: (1) overlay-provisioned (snap
    # stub / pre-built overlay tar), (2) launched via the lean ChromiumStandalone path (OOM
    # otherwise). And it must stay honest about device-pending.
    i = catalog_block.index('appId = "org.chromium.Chromium"')
    body = catalog_block[max(0, i - 4000):i + 1500]  # the entry + its leading doc-comment
    assert "snap" in body.lower(), "must note noble's chromium apt pkg is a snap stub"
    assert "chromium-gui-stage.tar" in body, "must name the pre-built overlay tar"
    assert "runChromiumStandalone" in body, "must name the lean launch path"
    assert "OOM" in body or "memory" in body.lower(), "must explain the OOM/memory rationale"
    assert "device" in body.lower(), "must keep the honest device-pending note"


def test_runtime_exposes_overlay_provision_and_launch_activity_ssot(kt_text: str):
    # BundledCatalog must expose the launch-activity + overlay-provision SSOTs (derived from the
    # entries) so the launcher routes chromium without duplicating the app-id anywhere.
    assert "fun launchActivityFor(appId: String)" in kt_text
    assert "fun overlayAppFor(appId: String)" in kt_text
    assert "fun isOverlayProvisioned(appId: String)" in kt_text
    # the SSOTs are derived from the entries (cannot drift).
    assert "c.launchActivity?.let" in kt_text
    assert "it.provision == ProvisionKind.OVERLAY" in kt_text


def test_runtime_install_routes_overlay_apps_away_from_apt(kt_text: str):
    # install() must short-circuit OVERLAY apps to the extractOverlay path BEFORE the aptRefFor
    # lookup (so chromium never goes through AptInstaller), and the overlay installer must extract
    # the staged tar / report honestly when absent.
    assert "BundledCatalog.overlayAppFor(appId)" in kt_text
    assert "installOverlayApp(appId, overlayApp" in kt_text
    assert "fun installOverlayApp(" in kt_text
    # honest no-tar path (no fake apt install).
    assert "구성요소" in kt_text or "must be provided" in kt_text.lower()
    # the synthesized-installed path for the no-.desktop overlay browser.
    assert "toInstalledOverlayApp" in kt_text
    assert "overlayBinaryPresent" in kt_text


def test_models_declare_overlay_provision_fields():
    # AppModels.kt must declare the OVERLAY-provision model surface: a ProvisionKind enum and the
    # three CatalogApp fields (provision / overlayBinaryPath / launchActivity), all defaulting so
    # existing apt entries stay byte-identical.
    m = MODELS.read_text(encoding="utf-8")
    assert "enum class ProvisionKind" in m
    assert "APT(" in m and "OVERLAY(" in m
    assert "val provision: ProvisionKind = ProvisionKind.APT" in m
    assert "val overlayBinaryPath: String? = null" in m
    assert "val launchActivity: String? = null" in m


def test_launcher_routes_dedicated_launch_activity(catalog_block: str):
    # LauncherActivity.startRunningSurface must consult the launchActivity SSOT and start the
    # named Activity by explicit Intent instead of RunningSurfaceActivity for chromium.
    launcher = (
        ROOT / "app/src/main/java/dev/chanwoo/androlinux/ui/LauncherActivity.kt"
    ).read_text(encoding="utf-8")
    assert "BundledCatalog.launchActivityFor(req.appId)" in launcher
    assert "setClassName(" in launcher
    # falls back to RunningSurfaceActivity for normal apps (launchActivity == null).
    assert "RunningSurfaceActivity::class.java" in launcher
