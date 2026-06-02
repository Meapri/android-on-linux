"""Host-verified tests for tools/desktop_entry.py.

Real-world `.desktop` fixtures (GIMP, foot, Firefox, plus NoDisplay/hidden helper
entries) exercise field-code stripping, NoDisplay/Type filtering, multi-value
Categories, localized Name resolution, and hicolor icon-path resolution.
"""

from __future__ import annotations

from tools.desktop_entry import (
    LauncherEntry,
    app_id_from_path,
    categories_present,
    expand_exec,
    is_visible,
    load_launcher_entries,
    localized_value,
    matches_query,
    parse_desktop_entry,
    parse_groups,
    resolve_icon_path,
)


# --------------------------------------------------------------------------- #
# Fixtures — verbatim-shaped real .desktop files
# --------------------------------------------------------------------------- #
GIMP_DESKTOP = """\
[Desktop Entry]
Version=1.0
Type=Application
Name=GNU Image Manipulation Program
Name[ko]=GNU 이미지 조작 프로그램
GenericName=Image Editor
GenericName[ko]=이미지 편집기
Comment=Create images and edit photographs
Comment[ko]=이미지를 만들고 사진을 편집합니다
Exec=gimp-3.0 %U
TryExec=gimp-3.0
Icon=org.gimp.GIMP
Terminal=false
Categories=Graphics;2DGraphics;RasterGraphics;GTK;
Keywords=GIMP;graphic;design;illustration;painting;
MimeType=image/png;image/jpeg;image/x-xcf;
StartupWMClass=gimp-3.0
"""

FOOT_DESKTOP = """\
[Desktop Entry]
Type=Application
Name=foot
GenericName=Terminal
Comment=A fast, lightweight and minimalistic Wayland terminal emulator
Exec=foot
Icon=foot
Terminal=false
Categories=System;TerminalEmulator;
Keywords=terminal;shell;prompt;command;commandline;cmd;
"""

FIREFOX_DESKTOP = """\
[Desktop Entry]
Version=1.0
Name=Firefox Web Browser
Name[ko]=파이어폭스 웹 브라우저
Comment=Browse the World Wide Web
GenericName=Web Browser
Exec=firefox %u
Terminal=false
Type=Application
Icon=firefox
Categories=Network;WebBrowser;
MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;
"""

# NoDisplay helper entry (e.g. a URL handler) — must be filtered out of the grid.
NODISPLAY_DESKTOP = """\
[Desktop Entry]
Type=Application
Name=GIMP Data Folder Helper
Exec=gimp-helper %f
NoDisplay=true
Categories=Graphics;
"""

# Hidden (deleted by a user override) entry — also filtered out.
HIDDEN_DESKTOP = """\
[Desktop Entry]
Type=Application
Name=Obsolete Tool
Exec=obsolete
Hidden=true
"""

# A non-Application type (Link) — never an app entry.
LINK_DESKTOP = """\
[Desktop Entry]
Type=Link
Name=Homepage
URL=https://example.org/
"""

# A terminal app, with %i icon field code, to test Terminal + %i expansion.
HTOP_DESKTOP = """\
[Desktop Entry]
Type=Application
Name=htop
Comment=Show System Processes
Exec=htop %i
Icon=htop
Terminal=true
Categories=System;Monitor;
"""


# --------------------------------------------------------------------------- #
# Group / INI parsing
# --------------------------------------------------------------------------- #
def test_parse_groups_isolates_desktop_entry_group():
    text = "[Desktop Entry]\nName=A\n\n[Desktop Action New]\nName=New\n"
    groups = parse_groups(text)
    assert set(groups) == {"Desktop Entry", "Desktop Action New"}
    assert groups["Desktop Entry"].entries["Name"] == "A"
    assert groups["Desktop Action New"].entries["Name"] == "New"


def test_parse_groups_skips_comments_and_blank_and_malformed():
    text = "# comment\n[Desktop Entry]\n\nNoEqualsSignHere\nName=Ok\n"
    groups = parse_groups(text)
    assert groups["Desktop Entry"].entries == {"Name": "Ok"}


# --------------------------------------------------------------------------- #
# Exec field-code handling
# --------------------------------------------------------------------------- #
def test_exec_strips_file_and_url_field_codes():
    assert expand_exec("gimp-3.0 %U") == ["gimp-3.0"]
    assert expand_exec("firefox %u") == ["firefox"]
    assert expand_exec("editor %F --new") == ["editor", "--new"]


def test_exec_keeps_quoted_arguments_whole():
    argv = expand_exec('myapp --title "Hello World" %f')
    assert argv == ["myapp", "--title", "Hello World"]


def test_exec_literal_percent_and_unknown_code():
    assert expand_exec("app --rate 50%% %f") == ["app", "--rate", "50%"]
    # unknown/deprecated code is stripped, surrounding text kept
    assert expand_exec("app pre%xpost") == ["app", "prepost"]


def test_exec_icon_field_code_expands_to_two_args():
    assert expand_exec("htop %i", icon="htop") == ["htop", "--icon", "htop"]
    # No icon → %i removed entirely
    assert expand_exec("htop %i") == ["htop"]


def test_exec_c_and_k_field_codes():
    argv = expand_exec("app %c %k", name="My App", desktop_path="/x/app.desktop")
    assert argv == ["app", "My App", "/x/app.desktop"]


def test_exec_empty_returns_empty():
    assert expand_exec("") == []
    assert expand_exec("   ") == []


# --------------------------------------------------------------------------- #
# Localized key resolution
# --------------------------------------------------------------------------- #
def test_localized_value_ko_and_fallback():
    groups = parse_groups(GIMP_DESKTOP)
    g = groups["Desktop Entry"]
    assert localized_value(g, "Name", "ko") == "GNU 이미지 조작 프로그램"
    assert localized_value(g, "Name", "ko_KR.UTF-8") == "GNU 이미지 조작 프로그램"
    # Unknown locale → unlocalized base value
    assert localized_value(g, "Name", "fr") == "GNU Image Manipulation Program"
    assert localized_value(g, "Name", None) == "GNU Image Manipulation Program"


def test_localized_value_country_falls_back_to_language():
    text = "[Desktop Entry]\nType=Application\nName=Base\nName[pt]=Portugues\n"
    g = parse_groups(text)["Desktop Entry"]
    # pt_BR has no exact key → falls back to pt
    assert localized_value(g, "Name", "pt_BR") == "Portugues"


# --------------------------------------------------------------------------- #
# Full entry parsing
# --------------------------------------------------------------------------- #
def test_parse_gimp_entry_full():
    entry = parse_desktop_entry(
        GIMP_DESKTOP, path="/usr/share/applications/org.gimp.GIMP.desktop"
    )
    assert isinstance(entry, LauncherEntry)
    assert entry.app_id == "org.gimp.GIMP"
    assert entry.name == "GNU Image Manipulation Program"
    assert entry.exec_argv == ["gimp-3.0"]  # %U stripped
    assert entry.exec_raw == "gimp-3.0 %U"
    assert entry.generic_name == "Image Editor"
    assert entry.icon == "org.gimp.GIMP"
    assert entry.categories == ("Graphics", "2DGraphics", "RasterGraphics", "GTK")
    assert entry.mime_types == ("image/png", "image/jpeg", "image/x-xcf")
    assert "GIMP" in entry.keywords
    assert entry.terminal is False
    assert entry.no_display is False
    assert is_visible(entry)
    # Multi-category → primary main category
    assert entry.primary_category == "Graphics"


def test_parse_gimp_entry_korean_locale():
    entry = parse_desktop_entry(
        GIMP_DESKTOP,
        path="/usr/share/applications/org.gimp.GIMP.desktop",
        locale="ko_KR.UTF-8",
    )
    assert entry is not None
    assert entry.name == "GNU 이미지 조작 프로그램"
    assert entry.generic_name == "이미지 편집기"
    assert entry.comment == "이미지를 만들고 사진을 편집합니다"


def test_parse_foot_terminal_entry():
    entry = parse_desktop_entry(FOOT_DESKTOP, path="/x/foot.desktop")
    assert entry is not None
    assert entry.app_id == "foot"
    assert entry.exec_argv == ["foot"]
    assert entry.primary_category == "System"
    assert "TerminalEmulator" in entry.categories


def test_parse_htop_terminal_flag_and_icon_expansion():
    entry = parse_desktop_entry(HTOP_DESKTOP, path="/x/htop.desktop")
    assert entry is not None
    assert entry.terminal is True
    assert entry.exec_argv == ["htop", "--icon", "htop"]


def test_nodisplay_entry_parsed_but_not_visible():
    entry = parse_desktop_entry(NODISPLAY_DESKTOP, path="/x/helper.desktop")
    assert entry is not None  # parseable
    assert entry.no_display is True
    assert not is_visible(entry)


def test_hidden_entry_not_visible():
    entry = parse_desktop_entry(HIDDEN_DESKTOP, path="/x/obsolete.desktop")
    assert entry is not None
    assert entry.hidden is True
    assert not is_visible(entry)


def test_link_type_is_not_an_application():
    assert parse_desktop_entry(LINK_DESKTOP, path="/x/home.desktop") is None


def test_missing_name_or_exec_rejected():
    assert parse_desktop_entry("[Desktop Entry]\nType=Application\nExec=x\n") is None
    assert parse_desktop_entry("[Desktop Entry]\nType=Application\nName=X\n") is None


def test_no_desktop_entry_group_rejected():
    assert parse_desktop_entry("[Desktop Action New]\nName=New\n") is None


# --------------------------------------------------------------------------- #
# Catalog loading + filtering
# --------------------------------------------------------------------------- #
def test_load_launcher_entries_filters_and_sorts():
    files = {
        "/a/org.gimp.GIMP.desktop": GIMP_DESKTOP,
        "/a/foot.desktop": FOOT_DESKTOP,
        "/a/firefox.desktop": FIREFOX_DESKTOP,
        "/a/helper.desktop": NODISPLAY_DESKTOP,  # filtered
        "/a/obsolete.desktop": HIDDEN_DESKTOP,  # filtered
        "/a/home.desktop": LINK_DESKTOP,  # filtered (Link)
    }
    entries = load_launcher_entries(files)
    ids = [e.app_id for e in entries]
    assert ids == ["firefox", "foot", "org.gimp.GIMP"]  # casefold name sort
    # NoDisplay / Hidden / Link all excluded
    assert "helper" not in ids
    assert "obsolete" not in ids
    assert "home" not in ids


def test_load_launcher_entries_include_hidden():
    files = {
        "/a/foot.desktop": FOOT_DESKTOP,
        "/a/helper.desktop": NODISPLAY_DESKTOP,
    }
    entries = load_launcher_entries(files, include_hidden=True)
    assert {e.app_id for e in entries} == {"foot", "helper"}


def test_load_launcher_entries_dedup_later_wins():
    override = FOOT_DESKTOP.replace("Name=foot", "Name=foot (user)")
    files = {
        "/system/foot.desktop": FOOT_DESKTOP,
        "/user/foot.desktop": override,
    }
    entries = load_launcher_entries(files)
    assert len(entries) == 1
    assert entries[0].name == "foot (user)"


def test_categories_present_for_filter_chips():
    files = {
        "/a/org.gimp.GIMP.desktop": GIMP_DESKTOP,
        "/a/foot.desktop": FOOT_DESKTOP,
        "/a/firefox.desktop": FIREFOX_DESKTOP,
    }
    entries = load_launcher_entries(files)
    assert categories_present(entries) == ["Graphics", "Network", "System"]


# --------------------------------------------------------------------------- #
# Search
# --------------------------------------------------------------------------- #
def test_matches_query_name_generic_comment_keywords():
    entry = parse_desktop_entry(GIMP_DESKTOP, path="/x/g.desktop")
    assert entry is not None
    assert matches_query(entry, "")  # empty matches everything
    assert matches_query(entry, "image")  # name + comment
    assert matches_query(entry, "editor")  # generic name
    assert matches_query(entry, "illustration")  # keyword
    assert not matches_query(entry, "spreadsheet")


# --------------------------------------------------------------------------- #
# Icon path resolution
# --------------------------------------------------------------------------- #
def test_resolve_icon_absolute_path_passthrough():
    assert resolve_icon_path("/opt/app/icon.png") == "/opt/app/icon.png"


def test_resolve_icon_prefers_largest_available_hicolor_size():
    available = {
        "/usr/share/icons/hicolor/48x48/apps/foot.png",
        "/usr/share/icons/hicolor/256x256/apps/foot.png",
    }
    got = resolve_icon_path("foot", available=available)
    assert got == "/usr/share/icons/hicolor/256x256/apps/foot.png"


def test_resolve_icon_scalable_svg_fallback():
    available = {"/usr/share/icons/hicolor/scalable/apps/org.gimp.GIMP.svg"}
    got = resolve_icon_path("org.gimp.GIMP", available=available)
    assert got == "/usr/share/icons/hicolor/scalable/apps/org.gimp.GIMP.svg"


def test_resolve_icon_pixmaps_fallback():
    available = {"/usr/share/pixmaps/firefox.png"}
    got = resolve_icon_path("firefox", available=available)
    assert got == "/usr/share/pixmaps/firefox.png"


def test_resolve_icon_none_when_unavailable():
    assert resolve_icon_path("nope", available=set()) is None
    assert resolve_icon_path("", available=set()) is None


def test_resolve_icon_pure_mode_returns_first_candidate():
    # available=None → pure (no fs); returns the largest-size png candidate.
    got = resolve_icon_path("foot")
    assert got == "/usr/share/icons/hicolor/512x512/apps/foot.png"


# --------------------------------------------------------------------------- #
# app_id derivation
# --------------------------------------------------------------------------- #
def test_app_id_from_path():
    assert app_id_from_path("/usr/share/applications/org.gimp.GIMP.desktop") == "org.gimp.GIMP"
    assert app_id_from_path("foot.desktop") == "foot"
    assert app_id_from_path("/x/no-suffix") == "no-suffix"
