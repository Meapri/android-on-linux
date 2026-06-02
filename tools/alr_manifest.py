"""ALR Linux-app manifest schema — parser / validator / catalog loader.

T2 (product-ux track). This is the pure, host-verifiable model for the
"runtime + in-app catalog" packaging model (찬우 확정): the ALR runtime app is
installed once, and Linux apps are described/bundled/installed *inside* it via
manifests. Each manifest (JSON) tells the runtime how to present a Linux app in
the catalog/launcher, what rootfs overlays (§5-E stage tars) or apt packages it
needs, and which Android-level permissions it wants (the enum shared with T4 the
permission-prompt track).

This module is intentionally runtime-free: it parses + validates manifests and
loads a catalog (a list of manifests), with NO Android / NO native dependency,
so the schema can be exercised and frozen host-side before any UI or installer
wiring exists. The Kotlin/Android side (catalog screen, RootfsInstaller overlay
application, permission prompts) consumes the SAME field shapes.

Design notes:
  * `rootfs_deps` is the bridge to the existing RootfsInstaller stage-tar/overlay
    convention (tools/STAGE_TAR_SPEC.md §5-E). A dep is EITHER an overlay stage
    tar (`kind="stage-tar"`, carrying the `<name>-stage.tar` basename whose
    `.{name}-staged-<size>` marker the device extractor keys on) OR an apt
    package name (`kind="apt"`) to be installed into the rootfs at runtime. Both
    carry an `install_size_bytes` estimate so the catalog UI can show a download
    size before install.
  * `required_permissions` uses the closed enum below; an unknown permission is a
    HARD validation error (never silently dropped) so a malicious/typo'd manifest
    can't smuggle an unrecognized capability past the prompt UI.
  * `entry` is EITHER an explicit binary + argv (`kind="exec"`) OR a reference to
    an in-rootfs `.desktop` file (`kind="desktop"`). The runtime resolves a
    desktop entry's Exec/Icon at launch time; the manifest only needs the path.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath


# --------------------------------------------------------------------------- #
# Enums / closed vocabularies
# --------------------------------------------------------------------------- #

# Reverse-DNS app id, e.g. "org.gimp.GIMP" or "org.gnome.gedit". Two-or-more
# dot-separated labels; each label starts with a letter, then letters/digits/_,
# with optional internal '-' segments. Deliberately strict so the id is a safe
# directory name / intent target / catalog key.
_APP_ID = re.compile(r"^[a-zA-Z][a-zA-Z0-9_]*(-[a-zA-Z0-9_]+)*(\.[a-zA-Z][a-zA-Z0-9_]*(-[a-zA-Z0-9_]+)*)+$")

# Semantic-ish version "MAJOR.MINOR[.PATCH][-tag]" used for both the app's own
# version and the `min_runtime` floor. Numeric dotted core (so versions order),
# plus an optional free-form `-tag` suffix.
_VERSION = re.compile(r"^[0-9]+(\.[0-9]+){1,3}(-[A-Za-z0-9._]+)?$")

# Catalog categories the launcher groups apps under. Closed set: an unknown
# category is rejected (keeps the launcher's section list bounded + localizable).
CATEGORY_GRAPHICS = "graphics"
CATEGORY_DEVELOPMENT = "development"
CATEGORY_OFFICE = "office"
CATEGORY_INTERNET = "internet"
CATEGORY_MULTIMEDIA = "multimedia"
CATEGORY_GAMES = "games"
CATEGORY_SYSTEM = "system"
CATEGORY_UTILITY = "utility"
CATEGORY_EDUCATION = "education"
CATEGORY_TERMINAL = "terminal"

CATEGORIES = frozenset(
    {
        CATEGORY_GRAPHICS,
        CATEGORY_DEVELOPMENT,
        CATEGORY_OFFICE,
        CATEGORY_INTERNET,
        CATEGORY_MULTIMEDIA,
        CATEGORY_GAMES,
        CATEGORY_SYSTEM,
        CATEGORY_UTILITY,
        CATEGORY_EDUCATION,
        CATEGORY_TERMINAL,
    }
)

# Permission enum — the SHARED CONTRACT with T4 (permission-prompt track). The
# runtime maps each to the right Android-level gate (runtime permission, SAF
# picker, foreground-service type, or a pure in-app/no-op grant). T2 only needs
# the closed vocabulary so a manifest can declare intent and validation can
# reject anything outside it. Keep this list and T4's prompt-mapping in lockstep.
PERMISSION_STORAGE_READ = "storage-read"
PERMISSION_STORAGE_WRITE = "storage-write"
PERMISSION_CAMERA = "camera"
PERMISSION_MICROPHONE = "microphone"
PERMISSION_LOCATION = "location"
PERMISSION_NETWORK = "network"
PERMISSION_NOTIFICATIONS = "notifications"

PERMISSIONS = frozenset(
    {
        PERMISSION_STORAGE_READ,
        PERMISSION_STORAGE_WRITE,
        PERMISSION_CAMERA,
        PERMISSION_MICROPHONE,
        PERMISSION_LOCATION,
        PERMISSION_NETWORK,
        PERMISSION_NOTIFICATIONS,
    }
)

# Display mode for the app's top-level surface on the ALR compositor.
DISPLAY_WINDOWED = "windowed"
DISPLAY_FULLSCREEN = "fullscreen"

DISPLAY_MODES = frozenset({DISPLAY_WINDOWED, DISPLAY_FULLSCREEN})

# rootfs dependency kinds (see §5-E bridge in the module docstring).
DEP_STAGE_TAR = "stage-tar"
DEP_APT = "apt"

DEP_KINDS = frozenset({DEP_STAGE_TAR, DEP_APT})

# entry kinds.
ENTRY_EXEC = "exec"
ENTRY_DESKTOP = "desktop"

ENTRY_KINDS = frozenset({ENTRY_EXEC, ENTRY_DESKTOP})


class ManifestError(ValueError):
    """Raised when a manifest (or catalog) fails to parse or validate.

    A single ValueError-subclass for the whole module so callers (UI / installer
    glue / tests) can catch one type; the message names the offending field.
    """


# --------------------------------------------------------------------------- #
# Data model
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class RootfsDep:
    """One rootfs requirement: an overlay stage tar OR an apt package.

    kind == "stage-tar": `ref` is the overlay basename pushed/extracted by
      RootfsInstaller.extractOverlayTar (e.g. "gimp-stage.tar"); its
      `.{stem}-staged-<size>` marker gates re-extraction. Must be a bare
      filename ending in ".tar" (no path separators) — the runtime resolves it
      against its own stage-tar source dir, NOT an attacker-chosen path.
    kind == "apt": `ref` is a Debian/Ubuntu-noble package name to `apt install`
      into the rootfs.
    install_size_bytes: estimated on-device size after install, for the catalog
      "Download (X MB)" affordance. >= 0.
    """

    kind: str
    ref: str
    install_size_bytes: int = 0

    def __post_init__(self) -> None:
        if self.kind not in DEP_KINDS:
            raise ManifestError(
                f"rootfs_deps[].kind must be one of {sorted(DEP_KINDS)}, got {self.kind!r}"
            )
        if not isinstance(self.ref, str) or not self.ref:
            raise ManifestError("rootfs_deps[].ref must be a non-empty string")
        if self.install_size_bytes < 0:
            raise ManifestError("rootfs_deps[].install_size_bytes must be non-negative")
        if self.kind == DEP_STAGE_TAR:
            if "/" in self.ref or "\\" in self.ref or self.ref in {".", ".."}:
                raise ManifestError(
                    f"stage-tar ref must be a bare filename (no path), got {self.ref!r}"
                )
            if not self.ref.endswith(".tar"):
                raise ManifestError(
                    f"stage-tar ref must end with '.tar' (§5-E <name>-stage.tar), got {self.ref!r}"
                )
        else:  # DEP_APT
            if not _APT_PKG_NAME.fullmatch(self.ref):
                raise ManifestError(f"apt package name is not valid: {self.ref!r}")

    @property
    def stage_marker_stem(self) -> str:
        """For a stage-tar dep, the `<name>` in the `.{name}-staged-<size>` marker.

        Mirrors MainActivity's marker derivation: ".<name>-staged-<len>" where
        <name> is the tar basename with the trailing "-stage.tar" / ".tar"
        stripped. Lets host tooling/UI predict the on-device marker filename.
        """
        if self.kind != DEP_STAGE_TAR:
            raise ManifestError("stage_marker_stem only applies to stage-tar deps")
        base = self.ref[: -len(".tar")]
        if base.endswith("-stage"):
            base = base[: -len("-stage")]
        return base


@dataclass(frozen=True)
class AppEntry:
    """How the runtime launches the app.

    kind == "exec": `target` is the in-rootfs binary path (absolute,
      rootfs-relative-to-/, e.g. "/usr/bin/gimp"), `argv` the extra arguments.
    kind == "desktop": `target` is the in-rootfs `.desktop` path
      (e.g. "/usr/share/applications/org.gimp.GIMP.desktop"); the runtime reads
      its Exec=/Icon= at launch. `argv` must be empty for desktop entries (the
      .desktop owns the command line).
    """

    kind: str
    target: str
    argv: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if self.kind not in ENTRY_KINDS:
            raise ManifestError(
                f"entry.kind must be one of {sorted(ENTRY_KINDS)}, got {self.kind!r}"
            )
        if not isinstance(self.target, str) or not self.target.startswith("/"):
            raise ManifestError(
                f"entry.target must be an absolute in-rootfs path, got {self.target!r}"
            )
        if ".." in PurePosixPath(self.target).parts:
            raise ManifestError(f"entry.target must not contain '..': {self.target!r}")
        if not all(isinstance(a, str) for a in self.argv):
            raise ManifestError("entry.argv must be a list of strings")
        if self.kind == ENTRY_DESKTOP:
            if not self.target.endswith(".desktop"):
                raise ManifestError(
                    f"desktop entry target must end with '.desktop', got {self.target!r}"
                )
            if self.argv:
                raise ManifestError("desktop entry must not carry argv (.desktop owns Exec=)")


@dataclass(frozen=True)
class DisplaySpec:
    """Surface presentation hint for the compositor.

    mode: windowed|fullscreen. width/height: optional preferred pixel size hints
    (the compositor may clamp to the 1200×1920 device surface). 0 == unset.
    """

    mode: str = DISPLAY_WINDOWED
    width: int = 0
    height: int = 0

    def __post_init__(self) -> None:
        if self.mode not in DISPLAY_MODES:
            raise ManifestError(
                f"display.mode must be one of {sorted(DISPLAY_MODES)}, got {self.mode!r}"
            )
        if self.width < 0 or self.height < 0:
            raise ManifestError("display width/height must be non-negative")


@dataclass(frozen=True)
class AppManifest:
    """A single Linux app's catalog manifest."""

    app_id: str
    name: str
    summary: str
    entry: AppEntry
    category: str = CATEGORY_UTILITY
    description: str = ""
    icon: str = ""
    rootfs_deps: tuple[RootfsDep, ...] = ()
    required_permissions: tuple[str, ...] = ()
    display: DisplaySpec = field(default_factory=DisplaySpec)
    min_runtime: str = "0.1"
    install_size_bytes: int = 0

    def __post_init__(self) -> None:
        if not _APP_ID.fullmatch(self.app_id):
            raise ManifestError(f"app_id must be reverse-DNS (e.g. org.gimp.GIMP), got {self.app_id!r}")
        if not isinstance(self.name, str) or not self.name.strip():
            raise ManifestError("name must be a non-empty string")
        if not isinstance(self.summary, str) or not self.summary.strip():
            raise ManifestError("summary must be a non-empty string")
        if self.category not in CATEGORIES:
            raise ManifestError(
                f"category must be one of {sorted(CATEGORIES)}, got {self.category!r}"
            )
        if not isinstance(self.description, str):
            raise ManifestError("description must be a string")
        if not isinstance(self.icon, str):
            raise ManifestError("icon must be a string (rootfs path or .desktop ref)")
        if self.icon and not (self.icon.startswith("/") or self.icon.endswith(".desktop")):
            raise ManifestError(
                f"icon must be an absolute rootfs path or a .desktop ref, got {self.icon!r}"
            )
        # required_permissions: closed enum, unknown => hard error, dedup-rejected.
        seen: set[str] = set()
        for perm in self.required_permissions:
            if perm not in PERMISSIONS:
                raise ManifestError(
                    f"unknown permission {perm!r}; allowed: {sorted(PERMISSIONS)}"
                )
            if perm in seen:
                raise ManifestError(f"duplicate permission: {perm!r}")
            seen.add(perm)
        if not _VERSION.fullmatch(self.min_runtime):
            raise ManifestError(f"min_runtime must be a version (e.g. 0.4.137), got {self.min_runtime!r}")
        if self.install_size_bytes < 0:
            raise ManifestError("install_size_bytes must be non-negative")
        if not isinstance(self.entry, AppEntry):
            raise ManifestError("entry must be an AppEntry")
        if not isinstance(self.display, DisplaySpec):
            raise ManifestError("display must be a DisplaySpec")
        for dep in self.rootfs_deps:
            if not isinstance(dep, RootfsDep):
                raise ManifestError("rootfs_deps must be RootfsDep instances")

    @property
    def total_install_size_bytes(self) -> int:
        """install_size_bytes if set, else the sum of the rootfs deps' sizes.

        The catalog UI shows a single "Download X MB" number; prefer the
        authored total, falling back to the dep-size sum when the author left
        the top-level estimate at 0.
        """
        if self.install_size_bytes:
            return self.install_size_bytes
        return sum(dep.install_size_bytes for dep in self.rootfs_deps)


@dataclass(frozen=True)
class Catalog:
    """An ordered list of app manifests with unique app_ids."""

    apps: tuple[AppManifest, ...]

    def __post_init__(self) -> None:
        seen: set[str] = set()
        for app in self.apps:
            if not isinstance(app, AppManifest):
                raise ManifestError("catalog entries must be AppManifest instances")
            if app.app_id in seen:
                raise ManifestError(f"duplicate app_id in catalog: {app.app_id}")
            seen.add(app.app_id)

    def get(self, app_id: str) -> AppManifest | None:
        for app in self.apps:
            if app.app_id == app_id:
                return app
        return None


# Debian/Ubuntu package-name grammar (policy 5.6.1): lowercase letter/digit
# start, then lowercase letters, digits, '+', '-', '.'.
_APT_PKG_NAME = re.compile(r"^[a-z0-9][a-z0-9+.\-]+$")


# --------------------------------------------------------------------------- #
# Parsing
# --------------------------------------------------------------------------- #


def _require_str(data: dict, key: str, *, where: str) -> str:
    if key not in data:
        raise ManifestError(f"{where}: missing required field {key!r}")
    value = data[key]
    if not isinstance(value, str):
        raise ManifestError(f"{where}: {key!r} must be a string, got {type(value).__name__}")
    return value


def _opt_str(data: dict, key: str, default: str, *, where: str) -> str:
    if key not in data:
        return default
    value = data[key]
    if not isinstance(value, str):
        raise ManifestError(f"{where}: {key!r} must be a string, got {type(value).__name__}")
    return value


def _opt_int(data: dict, key: str, default: int, *, where: str) -> int:
    if key not in data:
        return default
    value = data[key]
    # bool is an int subclass — reject it explicitly so `true` isn't read as 1.
    if isinstance(value, bool) or not isinstance(value, int):
        raise ManifestError(f"{where}: {key!r} must be an integer, got {type(value).__name__}")
    return value


def _parse_entry(data: object) -> AppEntry:
    if not isinstance(data, dict):
        raise ManifestError("entry must be an object")
    kind = _require_str(data, "kind", where="entry")
    target = _require_str(data, "target", where="entry")
    argv_raw = data.get("argv", [])
    if not isinstance(argv_raw, list) or not all(isinstance(a, str) for a in argv_raw):
        raise ManifestError("entry.argv must be a list of strings")
    return AppEntry(kind=kind, target=target, argv=tuple(argv_raw))


def _parse_display(data: object) -> DisplaySpec:
    if data is None:
        return DisplaySpec()
    if not isinstance(data, dict):
        raise ManifestError("display must be an object")
    mode = _opt_str(data, "mode", DISPLAY_WINDOWED, where="display")
    width = _opt_int(data, "width", 0, where="display")
    height = _opt_int(data, "height", 0, where="display")
    return DisplaySpec(mode=mode, width=width, height=height)


def _parse_dep(data: object) -> RootfsDep:
    if not isinstance(data, dict):
        raise ManifestError("rootfs_deps[] must be an object")
    kind = _require_str(data, "kind", where="rootfs_deps[]")
    ref = _require_str(data, "ref", where="rootfs_deps[]")
    size = _opt_int(data, "install_size_bytes", 0, where="rootfs_deps[]")
    return RootfsDep(kind=kind, ref=ref, install_size_bytes=size)


def parse_manifest(data: object) -> AppManifest:
    """Build + validate an AppManifest from a decoded JSON object (a dict).

    Raises ManifestError on any structural/type/enum violation.
    """
    if not isinstance(data, dict):
        raise ManifestError("manifest must be a JSON object")

    app_id = _require_str(data, "app_id", where="manifest")
    name = _require_str(data, "name", where="manifest")
    summary = _require_str(data, "summary", where="manifest")

    if "entry" not in data:
        raise ManifestError("manifest: missing required field 'entry'")
    entry = _parse_entry(data["entry"])

    deps_raw = data.get("rootfs_deps", [])
    if not isinstance(deps_raw, list):
        raise ManifestError("rootfs_deps must be a list")
    rootfs_deps = tuple(_parse_dep(d) for d in deps_raw)

    perms_raw = data.get("required_permissions", [])
    if not isinstance(perms_raw, list) or not all(isinstance(p, str) for p in perms_raw):
        raise ManifestError("required_permissions must be a list of strings")

    return AppManifest(
        app_id=app_id,
        name=name,
        summary=summary,
        entry=entry,
        category=_opt_str(data, "category", CATEGORY_UTILITY, where="manifest"),
        description=_opt_str(data, "description", "", where="manifest"),
        icon=_opt_str(data, "icon", "", where="manifest"),
        rootfs_deps=rootfs_deps,
        required_permissions=tuple(perms_raw),
        display=_parse_display(data.get("display")),
        min_runtime=_opt_str(data, "min_runtime", "0.1", where="manifest"),
        install_size_bytes=_opt_int(data, "install_size_bytes", 0, where="manifest"),
    )


def load_manifest(path: str | Path) -> AppManifest:
    """Read + parse a single manifest JSON file."""
    text = Path(path).read_text()
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ManifestError(f"invalid JSON in {path}: {exc}") from exc
    return parse_manifest(data)


def parse_catalog(data: object) -> Catalog:
    """Build a Catalog from a decoded JSON object.

    Accepts either a bare list of manifest objects, or an object with an
    "apps" list (so a catalog file can also carry top-level metadata later).
    """
    if isinstance(data, dict):
        apps_raw = data.get("apps")
        if apps_raw is None:
            raise ManifestError("catalog object must have an 'apps' list")
    elif isinstance(data, list):
        apps_raw = data
    else:
        raise ManifestError("catalog must be a JSON list or an object with 'apps'")
    if not isinstance(apps_raw, list):
        raise ManifestError("catalog 'apps' must be a list")
    apps = tuple(parse_manifest(item) for item in apps_raw)
    return Catalog(apps=apps)


def load_catalog(path: str | Path) -> Catalog:
    """Read + parse a catalog JSON file (list of manifests)."""
    text = Path(path).read_text()
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ManifestError(f"invalid JSON in {path}: {exc}") from exc
    return parse_catalog(data)
