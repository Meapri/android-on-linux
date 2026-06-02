"""freedesktop.org `.desktop` entry parser → ALR launcher entry model.

This is **pure logic** (no Android, no I/O beyond an optional file read helper) so
it can be host-verified with pytest and reused by the build pipeline that bakes a
launcher catalog into the runtime app. The same model shape the Android launcher
(`ui/LauncherScreen.kt`) consumes is produced here.

Scope (Desktop Entry Specification 1.5, the subset a launcher needs):

* INI-style groups; only the ``[Desktop Entry]`` group is interpreted.
* Recognized keys: ``Type``, ``Name``, ``GenericName``, ``Comment``, ``Exec``,
  ``Icon``, ``Categories``, ``Keywords``, ``MimeType``, ``NoDisplay``,
  ``Hidden``, ``Terminal``, ``TryExec``, ``StartupWMClass``.
* Localized keys (``Name[ko]``, ``Comment[pt_BR]`` …) resolved against a requested
  locale with the spec's fallback chain (lang_COUNTRY@MOD → lang_COUNTRY → lang).
* ``Exec`` field codes (``%f %F %u %U %i %c %k %d %D %n %N %v %m``) are expanded /
  stripped per spec when building the runnable argv; a literal ``%%`` → ``%``.
* Visibility filter: ``Type=Application`` only, excluding ``NoDisplay=true`` and
  ``Hidden=true``.
* ``Icon`` name → hicolor theme path resolution helper (no theme index parsing —
  a deterministic size/extension search, which is all the launcher needs).

Intentionally NOT implemented (out of launcher scope): ``Type=Link``/``Directory``,
DBusActivatable, Desktop Actions, X- vendor extensions beyond passthrough. These are
ignored rather than erroring so a real-world ``.desktop`` never fails to parse.

Runtime independence: the produced `LauncherEntry` carries the *parsed* command
(``exec_argv``) but launching is the runtime's job (see §5 contract in
docs/design/launcher-ui.md). This module never spawns anything.
"""

from __future__ import annotations

import re
import shlex
from dataclasses import dataclass, field, replace
from pathlib import PurePosixPath


DESKTOP_ENTRY_GROUP = "Desktop Entry"

# Field codes that take no argument and simply expand to nothing in a launcher
# context (we launch with no files/URLs). %i/%c/%k are handled specially below.
_VALUELESS_FIELD_CODES = set("fFuUdDnNvm")

# hicolor icon search: the sizes a launcher grid realistically wants, largest
# first (a launcher upsamples a smaller icon far better than it invents detail).
_HICOLOR_SIZES = (
    "512x512",
    "256x256",
    "192x192",
    "128x128",
    "96x96",
    "64x64",
    "48x48",
    "32x32",
    "24x24",
    "16x16",
)
_ICON_EXTENSIONS = ("png", "svg", "xpm")

_BOOL_TRUE = {"true", "1", "yes"}


# --------------------------------------------------------------------------- #
# Model
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class LauncherEntry:
    """A single launchable app as the launcher grid renders it.

    `app_id` is a stable identifier (the `.desktop` basename without suffix, e.g.
    ``org.gimp.GIMP``) used as the launch callback key — see LauncherScreen.kt.
    `exec_argv` is the field-code-expanded argv ready to hand to the runtime;
    `exec_raw` keeps the original ``Exec=`` line for diagnostics.
    """

    app_id: str
    name: str
    exec_argv: list[str]
    exec_raw: str = ""
    generic_name: str = ""
    comment: str = ""
    icon: str = ""
    categories: tuple[str, ...] = ()
    keywords: tuple[str, ...] = ()
    mime_types: tuple[str, ...] = ()
    terminal: bool = False
    no_display: bool = False
    hidden: bool = False

    @property
    def primary_category(self) -> str:
        """The freedesktop main category used to bucket the entry in the grid.

        Returns the first *main* (registered) category if present, else the first
        category, else ``"Other"``. Keeps the launcher's category filter stable.
        """
        for cat in self.categories:
            if cat in _MAIN_CATEGORIES:
                return cat
        return self.categories[0] if self.categories else "Other"


# freedesktop "Main Categories" (Menu spec) — used only to pick a primary bucket.
_MAIN_CATEGORIES = {
    "AudioVideo",
    "Audio",
    "Video",
    "Development",
    "Education",
    "Game",
    "Graphics",
    "Network",
    "Office",
    "Science",
    "Settings",
    "System",
    "Utility",
}


# --------------------------------------------------------------------------- #
# Low-level INI parsing
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class _Group:
    name: str
    # Preserve insertion order; localized keys live alongside their base key.
    entries: dict[str, str] = field(default_factory=dict)


def parse_groups(text: str) -> dict[str, _Group]:
    """Parse a `.desktop` file body into ``{group_name: _Group}``.

    Tolerant by design: blank lines and ``#`` comments are skipped; a malformed
    line without ``=`` is ignored rather than fatal. Later duplicate keys win
    (matching how most desktop-file readers behave).
    """
    groups: dict[str, _Group] = {}
    current: _Group | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1].strip()
            current = groups.get(name) or _Group(name=name)
            groups[name] = current
            continue
        if current is None:
            # Keys before any group header are invalid per spec; skip.
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        current.entries[key.strip()] = value.strip()
    return groups


# --------------------------------------------------------------------------- #
# Localized key resolution
# --------------------------------------------------------------------------- #
def _locale_candidates(locale: str | None) -> list[str]:
    """Build the spec fallback chain for a requested locale.

    For ``pt_BR.UTF-8@dialect`` the chain is
    ``[pt_BR@dialect, pt_BR, pt@dialect, pt]`` (encoding stripped), most specific
    first. ``None``/empty → just the unlocalized key.
    """
    if not locale:
        return []
    # Strip encoding: lang_COUNTRY.ENCODING@MODIFIER
    base = locale
    modifier = ""
    if "@" in base:
        base, modifier = base.split("@", 1)
    if "." in base:
        base = base.split(".", 1)[0]
    lang = base
    country = ""
    if "_" in base:
        lang, country = base.split("_", 1)

    out: list[str] = []
    if country and modifier:
        out.append(f"{lang}_{country}@{modifier}")
    if country:
        out.append(f"{lang}_{country}")
    if modifier:
        out.append(f"{lang}@{modifier}")
    out.append(lang)
    # De-dup preserving order.
    seen: set[str] = set()
    return [c for c in out if not (c in seen or seen.add(c))]


def localized_value(group: _Group, key: str, locale: str | None = None) -> str:
    """Return ``key[locale]`` with spec fallback, else the unlocalized ``key``."""
    for cand in _locale_candidates(locale):
        localized = f"{key}[{cand}]"
        if localized in group.entries:
            return group.entries[localized]
    return group.entries.get(key, "")


# --------------------------------------------------------------------------- #
# Exec field-code expansion
# --------------------------------------------------------------------------- #
def expand_exec(
    exec_value: str,
    *,
    icon: str = "",
    desktop_path: str = "",
    name: str = "",
) -> list[str]:
    """Expand an ``Exec=`` value into a runnable argv list.

    Implements the Desktop Entry Spec "The Exec key" rules:

    * The value is split with shell quoting (``shlex``) — quoted args stay whole.
    * ``%%`` → a literal ``%``.
    * ``%f %F %u %U %d %D %n %N %v %m`` → removed (no files/URLs at launch).
    * ``%i`` → ``--icon <icon>`` (two argv items) when an icon is set, else removed.
    * ``%c`` → the entry's translated ``Name``.
    * ``%k`` → the `.desktop` file path (or empty).
    * A deprecated field code mid-token is stripped, and any argv item that becomes
      empty as a result is dropped.

    Returns ``[]`` for an empty/whitespace ``Exec``.
    """
    if not exec_value.strip():
        return []
    try:
        tokens = shlex.split(exec_value)
    except ValueError:
        # Unbalanced quotes — fall back to a naive split so we still produce argv.
        tokens = exec_value.split()

    argv: list[str] = []
    for token in tokens:
        expanded = _expand_token(token, icon=icon, desktop_path=desktop_path, name=name)
        argv.extend(expanded)
    return argv


def _expand_token(token: str, *, icon: str, desktop_path: str, name: str) -> list[str]:
    # %i expands to TWO arguments ("--icon", value); handle when the whole token
    # is exactly %i (the spec's only sanctioned use).
    if token == "%i":
        return ["--icon", icon] if icon else []

    out_chars: list[str] = []
    i = 0
    produced_extra: list[str] = []
    while i < len(token):
        ch = token[i]
        if ch != "%":
            out_chars.append(ch)
            i += 1
            continue
        # We have a field code: look at the next char.
        if i + 1 >= len(token):
            # Trailing lone '%' — drop it.
            i += 1
            continue
        code = token[i + 1]
        i += 2
        if code == "%":
            out_chars.append("%")
        elif code in _VALUELESS_FIELD_CODES:
            pass  # removed
        elif code == "c":
            out_chars.append(name)
        elif code == "k":
            out_chars.append(desktop_path)
        elif code == "i":
            # %i embedded inside a larger token is malformed; emit value inline.
            if icon:
                produced_extra.append(icon)
        else:
            # Unknown/deprecated field code — strip it.
            pass

    result: list[str] = []
    text = "".join(out_chars)
    if text:
        result.append(text)
    result.extend(produced_extra)
    return result


# --------------------------------------------------------------------------- #
# Entry construction / filtering
# --------------------------------------------------------------------------- #
def _split_list(value: str) -> tuple[str, ...]:
    """Split a ``;``-separated spec list, dropping the trailing empty field."""
    if not value:
        return ()
    parts = [p for p in value.split(";")]
    # Spec lists end with a ';' → trailing empty element; drop empties.
    return tuple(p for p in parts if p)


def _bool(value: str) -> bool:
    return value.strip().lower() in _BOOL_TRUE


def app_id_from_path(path: str) -> str:
    """``/usr/share/applications/org.gimp.GIMP.desktop`` → ``org.gimp.GIMP``."""
    base = PurePosixPath(path).name
    if base.endswith(".desktop"):
        base = base[: -len(".desktop")]
    return base


def parse_desktop_entry(
    text: str,
    *,
    path: str = "",
    locale: str | None = None,
) -> LauncherEntry | None:
    """Parse a `.desktop` file body into a :class:`LauncherEntry`.

    Returns ``None`` when the file is not a launchable application entry, i.e.:

    * no ``[Desktop Entry]`` group, or
    * ``Type`` is not ``Application``, or
    * required ``Name`` or ``Exec`` is missing.

    Visibility flags (``NoDisplay``/``Hidden``) are *recorded* on the entry but do
    not by themselves make this return ``None`` — use :func:`load_launcher_entries`
    (or filter on ``entry.no_display``/``entry.hidden``) to drop hidden entries.
    This split lets callers that need every entry (e.g. MIME association) still see
    them.
    """
    groups = parse_groups(text)
    group = groups.get(DESKTOP_ENTRY_GROUP)
    if group is None:
        return None
    if group.entries.get("Type", "").strip() != "Application":
        return None

    name = localized_value(group, "Name", locale)
    exec_raw = group.entries.get("Exec", "")
    if not name or not exec_raw:
        return None

    icon = group.entries.get("Icon", "")
    app_id = app_id_from_path(path) if path else name
    exec_argv = expand_exec(exec_raw, icon=icon, desktop_path=path, name=name)

    return LauncherEntry(
        app_id=app_id,
        name=name,
        exec_argv=exec_argv,
        exec_raw=exec_raw,
        generic_name=localized_value(group, "GenericName", locale),
        comment=localized_value(group, "Comment", locale),
        icon=icon,
        categories=_split_list(group.entries.get("Categories", "")),
        keywords=_split_list(localized_value(group, "Keywords", locale)),
        mime_types=_split_list(group.entries.get("MimeType", "")),
        terminal=_bool(group.entries.get("Terminal", "")),
        no_display=_bool(group.entries.get("NoDisplay", "")),
        hidden=_bool(group.entries.get("Hidden", "")),
    )


def is_visible(entry: LauncherEntry) -> bool:
    """A launchable, non-hidden application entry the grid should show."""
    return not entry.no_display and not entry.hidden


def load_launcher_entries(
    files: dict[str, str],
    *,
    locale: str | None = None,
    include_hidden: bool = False,
) -> list[LauncherEntry]:
    """Parse a ``{path: file_text}`` map into sorted, deduplicated launcher entries.

    * Non-application / nameless / Exec-less files are dropped.
    * ``NoDisplay``/``Hidden`` entries are dropped unless ``include_hidden``.
    * Later files with the same ``app_id`` override earlier ones (the spec's
      ``$XDG_DATA_DIRS`` precedence — caller orders ``files`` accordingly).
    * Result is sorted case-insensitively by display name for a stable grid.
    """
    by_id: dict[str, LauncherEntry] = {}
    for path, text in files.items():
        entry = parse_desktop_entry(text, path=path, locale=locale)
        if entry is None:
            continue
        if not include_hidden and not is_visible(entry):
            continue
        by_id[entry.app_id] = entry
    return sorted(by_id.values(), key=lambda e: e.name.casefold())


# --------------------------------------------------------------------------- #
# Icon → hicolor theme path resolution
# --------------------------------------------------------------------------- #
def resolve_icon_path(
    icon: str,
    *,
    theme_root: str = "/usr/share/icons/hicolor",
    pixmaps_dir: str = "/usr/share/pixmaps",
    available: set[str] | None = None,
    preferred_sizes: tuple[str, ...] = _HICOLOR_SIZES,
) -> str | None:
    """Resolve an ``Icon=`` value to a concrete file path under the hicolor theme.

    Resolution order (Icon Theme Spec, launcher-relevant subset):

    1. If ``icon`` is already an absolute path, return it unchanged.
    2. Search ``<theme_root>/<size>/apps/<icon>.<ext>`` for each preferred size
       (largest first) and extension (png, svg, xpm).
    3. Fall back to ``<theme_root>/scalable/apps/<icon>.svg``.
    4. Fall back to ``<pixmaps_dir>/<icon>.<ext>``.

    ``available`` (a set of existing paths) lets callers/tests resolve without a
    filesystem — when given, only paths in the set are returned. When ``available``
    is ``None`` the first *candidate* path is returned (caller verifies existence),
    which keeps this function pure for host tests.
    """
    if not icon:
        return None
    if icon.startswith("/"):
        return icon
    # If the icon name itself carries an extension, treat as a bare filename.
    name = icon

    def pick(candidate: str) -> str | None:
        if available is None:
            return candidate
        return candidate if candidate in available else None

    candidates: list[str] = []
    for size in preferred_sizes:
        for ext in _ICON_EXTENSIONS:
            candidates.append(f"{theme_root}/{size}/apps/{name}.{ext}")
    candidates.append(f"{theme_root}/scalable/apps/{name}.svg")
    for ext in _ICON_EXTENSIONS:
        candidates.append(f"{pixmaps_dir}/{name}.{ext}")

    if available is None:
        return candidates[0]
    for cand in candidates:
        if cand in available:
            return cand
    return None


# --------------------------------------------------------------------------- #
# Search / category helpers (used by the launcher's search bar + filter chips)
# --------------------------------------------------------------------------- #
def matches_query(entry: LauncherEntry, query: str) -> bool:
    """Case-insensitive substring match over name/generic/comment/keywords.

    Mirrors what the Compose search bar filters on so the host-tested ranking and
    the on-device behavior stay identical.
    """
    q = query.strip().casefold()
    if not q:
        return True
    haystacks = [entry.name, entry.generic_name, entry.comment, *entry.keywords]
    return any(q in (h or "").casefold() for h in haystacks)


def categories_present(entries: list[LauncherEntry]) -> list[str]:
    """The sorted set of primary categories across ``entries`` (for filter chips)."""
    return sorted({e.primary_category for e in entries})
