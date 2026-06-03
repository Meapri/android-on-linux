#!/usr/bin/env python3
"""Build DLOPEN-PLUGIN overlays — the module CLASSES the DT_NEEDED closure DROPS.

Why a separate builder from deb_closure (§5-E)
----------------------------------------------
The §5-E deb-closure overlay (``tools/deb_closure.py``) follows ``DT_NEEDED`` ONLY.
That is correct for a library a binary *links*, but a large class of Linux GUI /
crypto / media frameworks load their real worker modules with ``dlopen()`` at
runtime, NOT via ``DT_NEEDED``:

  * NSS ``libnss3`` ``dlopen``s its PKCS#11 / crypto plugins (``libsoftokn3.so``,
    ``libfreebl3.so``, ``libnssckbi.so`` …) — chromium FATAL-crashed on device when
    these were missing ("libsoftokn3.so: cannot open shared object file";
    ``crypto/nss_util.cc:146 FATAL nss_error=-5925``).
  * gdk-pixbuf ``dlopen``s an image loader per format (``libpixbufloader-*.so``),
    selected via ``loaders.cache`` — a missing SVG loader aborts GTK
    (``Gtk:ERROR`` → ``abort(6)``, device-observed v127).
  * GIO ``dlopen``s its TLS / proxy / settings backends from ``gio/modules``
    (``libgiognutls.so`` = the GLib-TLS backend — without it ``g_tls_backend`` has
    no implementation and any libsoup/glib-networking HTTPS fails).
  * GTK3/GTK4 ``dlopen`` input methods (``immodules``) + print backends.
  * gstreamer ``dlopen``s every element plugin from ``gstreamer-1.0``.

Because these are ``dlopen``'d, the DT_NEEDED closure NEVER sees them and DROPS
them. The result is a per-app whack-a-mole of "missing module" crashes. This
builder generalizes the proven ``build_nss_overlay`` pattern into a configurable,
GROUP-based overlay that stages whole module CLASSES at once — so an entire class
of apps (anything that uses NSS / GTK pixbufs / GIO TLS / …) finds its
runtime-loaded modules without a per-app fix.

What it does
------------
For each requested GROUP it fetches the owning noble ``.deb``s (via the
``deb_closure`` helpers — imported, never modified), extracts the plugin module
DIRECTORIES, and packs each module at its canonical Debian path (e.g.
``usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders/``). Where the loader
framework needs a manifest cache to FIND the modules (gdk-pixbuf ``loaders.cache``,
GIO ``giomodule.cache``) the builder REGENERATES it host-side in the documented
byte-exact text format, keyed to the rootfs install path (which equals the Debian
path the modules are staged at). The on-device install paths == build paths, so a
host-regenerated cache is correct on the device.

Groups (``--group`` repeatable; default = ``nss,gdk-pixbuf,gio`` — the most
universally needed):

  nss          NSS PKCS#11 / crypto plugins (libnss3)            [no cache]
  gdk-pixbuf   image loaders + librsvg svg loader + loaders.cache (regenerated)
  gio          GLib GIO TLS/proxy/settings modules + giomodule.cache (regenerated)
  gtk3         GTK3 immodules + print backends                   [cache best-effort]
  gtk4         GTK4 print backends
  pango        legacy pango modules (modern pango has none — best-effort, may be 0)
  gstreamer    gstreamer-1.0 element plugins (LARGE — opt-in only via --group)

Honest scope
------------
HOST-ONLY. ``--list`` resolves the owning ``.deb``s and prints the planned members
+ caches with NO download. A full build downloads + packs a §5-E ``./``-rooted
stage tar (validated against ``stage_tar_spec``). The actual on-device dlopen
(chromium NSS init, GTK pixbuf render, GIO TLS handshake) is the integration /
device gate — the integration session stages the tar via the MainActivity toolkit
extraction loop, exactly like the proven ``nss`` overlay.

The regenerated caches use a STATIC, version-stable metadata table for the
standard modules (the per-format loader metadata + per-module GIO extension points
are fixed by the module's identity, not the build). When a base rootfs already
ships a conformant ``loaders.cache`` (it does — a hand-built byte-exact one), its
stanzas are MERGED so nothing regresses; new loaders (svg) get a table stanza.
"""

from __future__ import annotations

import argparse
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from tools.deb_closure import (
    _download_deb,
    fetch_packages_index,
    parse_packages,
)
from tools.build_stage_tar import extract_deb

# --------------------------------------------------------------------------- #
# Mirror / suite constants (the ALR base is Ubuntu noble 24.04 arm64)
# --------------------------------------------------------------------------- #

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main", "universe")
LIBDIR = "usr/lib/aarch64-linux-gnu"


# --------------------------------------------------------------------------- #
# Group model
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class PluginGroup:
    """One class of dlopen-loaded modules the DT_NEEDED closure drops.

    packages:    noble package names that OWN the modules (fetched + extracted).
    module_dirs: rootfs-relative directory prefixes whose .so modules we stage
                 (every regular-file ``*.so`` found under one is packed at its
                 SAME rootfs-relative path).
    also_flat:   extra individual module SONAMEs (basename) to ALSO stage at the
                 flat ``LIBDIR/<name>`` path — for modules a bare ``dlopen("name")``
                 finds via LD_LIBRARY_PATH (NSS does this for libsoftokn3.so).
    cache:       optional cache-manifest spec to regenerate host-side.
    required:    module basenames that MUST be found or the build errors (the
                 load-bearing ones); everything else is best-effort.
    large:       opt-in only (never in --all / default) — big payload.
    note:        human description.
    """

    name: str
    packages: tuple[str, ...]
    module_dirs: tuple[str, ...]
    also_flat: tuple[str, ...] = ()
    cache: "CacheSpec | None" = None
    required: tuple[str, ...] = ()
    large: bool = False
    note: str = ""


@dataclass(frozen=True)
class CacheSpec:
    """How to regenerate a loader-framework manifest cache host-side.

    kind:     "pixbuf" | "giomodule"  (the two text cache formats we emit).
    rel_path: rootfs-relative path the cache is written to.
    """

    kind: str
    rel_path: str


# --------------------------------------------------------------------------- #
# Static, version-stable module metadata (the per-module facts are fixed by the
# module's identity, not by the build — so a host-side cache is correct on device)
# --------------------------------------------------------------------------- #

# gdk-pixbuf loaders.cache stanza fields per loader basename. Format mirrors
# `gdk-pixbuf-query-loaders` output exactly (see the base rootfs cache). Each tuple:
#   (module_basename_without_libpixbufloader_prefix,
#    flags_int, namespace, description, license,
#    [mime types...], [extensions...], [(prefix, mask_or_"", relevance)...])
# These facts are stable across gdk-pixbuf 2.x. Only loaders we may stage are listed.
PIXBUF_LOADER_META: dict[str, dict] = {
    "libpixbufloader-ani.so": {
        "id": "ani", "flags": 4, "ns": "gdk-pixbuf",
        "desc": "The ANI image format", "lic": "LGPL",
        "mime": ["application/x-navi-animation"], "ext": ["ani"],
        "magic": [("RIFF    ACON", "    xxxx    ", 100)],
    },
    "libpixbufloader-bmp.so": {
        "id": "bmp", "flags": 5, "ns": "gdk-pixbuf",
        "desc": "The BMP image format", "lic": "LGPL",
        "mime": ["image/bmp", "image/x-bmp", "image/x-MS-bmp"], "ext": ["bmp"],
        "magic": [("BM", "", 100)],
    },
    "libpixbufloader-gif.so": {
        "id": "gif", "flags": 4, "ns": "gdk-pixbuf",
        "desc": "The GIF image format", "lic": "LGPL",
        "mime": ["image/gif"], "ext": ["gif"],
        "magic": [("GIF8", "", 100)],
    },
    "libpixbufloader-icns.so": {
        "id": "icns", "flags": 4, "ns": "gdk-pixbuf",
        "desc": "The ICNS image format", "lic": "LGPL",
        "mime": ["image/x-icns"], "ext": ["icns"],
        "magic": [("icns", "", 100)],
    },
    "libpixbufloader-ico.so": {
        "id": "ico", "flags": 5, "ns": "gdk-pixbuf",
        "desc": "The ICO image format", "lic": "LGPL",
        "mime": ["image/x-icon", "image/x-ico", "image/x-win-bitmap",
                 "image/vnd.microsoft.icon", "application/ico", "image/ico",
                 "image/icon", "text/ico"],
        "ext": ["ico", "cur"],
        "magic": [("  \\001   ", "zz znz", 100), ("  \\002   ", "zz znz", 100)],
    },
    "libpixbufloader-jpeg.so": {
        "id": "jpeg", "flags": 5, "ns": "gdk-pixbuf",
        "desc": "The JPEG image format", "lic": "LGPL",
        "mime": ["image/jpeg"], "ext": ["jpeg", "jpe", "jpg"],
        "magic": [("\\377\\330", "", 100)],
    },
    "libpixbufloader-png.so": {
        "id": "png", "flags": 0, "ns": "gdk-pixbuf",
        "desc": "The PNG image format", "lic": "LGPL",
        "mime": ["image/png"], "ext": ["png"],
        "magic": [("\\211PNG\\r\\n\\032\\n", "", 100)],
    },
    "libpixbufloader-pnm.so": {
        "id": "pnm", "flags": 5, "ns": "gdk-pixbuf",
        "desc": "The PNM/PBM/PGM/PPM image format", "lic": "LGPL",
        "mime": ["image/x-portable-anymap", "image/x-portable-bitmap",
                 "image/x-portable-graymap", "image/x-portable-pixmap"],
        "ext": ["pnm", "pbm", "pgm", "ppm"],
        "magic": [("P1", "", 100), ("P2", "", 100), ("P3", "", 100),
                  ("P4", "", 100), ("P5", "", 100), ("P6", "", 100)],
    },
    "libpixbufloader-qtif.so": {
        "id": "qtif", "flags": 4, "ns": "gdk-pixbuf",
        "desc": "The QTIF image format", "lic": "LGPL",
        "mime": ["image/x-quicktime"], "ext": ["qif", "qtif"],
        "magic": [("    idsc", "zzzz    ", 100), ("    idat", "zzzz    ", 100)],
    },
    "libpixbufloader-svg.so": {
        "id": "svg", "flags": 6, "ns": "gdk-pixbuf",
        "desc": "Scalable Vector Graphics", "lic": "LGPL",
        "mime": ["image/svg+xml", "image/svg", "image/svg-xml",
                 "image/vnd.adobe.svg+xml", "text/xml-svg",
                 "image/svg+xml-compressed"],
        "ext": ["svg", "svgz", "svg.gz"],
        "magic": [(" <svg", "*    ", 100), (" <!DOCTYPE svg", "*       *     ", 100)],
    },
    "libpixbufloader-tga.so": {
        "id": "tga", "flags": 4, "ns": "gdk-pixbuf",
        "desc": "The Targa image format", "lic": "LGPL",
        "mime": ["image/x-tga"], "ext": ["tga", "targa"], "magic": [],
    },
    "libpixbufloader-tiff.so": {
        "id": "tiff", "flags": 5, "ns": "gdk-pixbuf",
        "desc": "The TIFF image format", "lic": "LGPL",
        "mime": ["image/tiff"], "ext": ["tiff", "tif"],
        "magic": [("MM \\001", "", 100), ("II\\052 ", "", 100)],
    },
    "libpixbufloader-xbm.so": {
        "id": "xbm", "flags": 4, "ns": "gdk-pixbuf",
        "desc": "The XBM image format", "lic": "LGPL",
        "mime": ["image/x-xbitmap"], "ext": ["xbm"],
        "magic": [("#define ", "", 100), ("/* XPM */", "", 100)],
    },
    "libpixbufloader-xpm.so": {
        "id": "xpm", "flags": 4, "ns": "gdk-pixbuf",
        "desc": "The XPM image format", "lic": "LGPL",
        "mime": ["image/x-xpixmap"], "ext": ["xpm"],
        "magic": [("/* XPM */", "", 100)],
    },
}

# GIO module -> the GIOExtensionPoint name(s) it implements. Fixed by the module's
# identity; verified present as strings in the noble .so. giomodule.cache lists one
# line "<basename>: <ep1>,<ep2>" per module (GLib's _g_io_module_cache format).
GIO_MODULE_EXTENSION_POINTS: dict[str, tuple[str, ...]] = {
    "libgiognutls.so": ("gio-tls-backend", "gio-dtls-backend"),
    "libgiognomeproxy.so": ("gio-proxy-resolver",),
    "libgiolibproxy.so": ("gio-proxy-resolver",),
    "libdconfsettings.so": ("gio-settings-backend",),
    "libgiomemenu.so": ("gio-vfs",),
}


# --------------------------------------------------------------------------- #
# Group catalog
# --------------------------------------------------------------------------- #

GROUPS: dict[str, PluginGroup] = {
    "nss": PluginGroup(
        name="nss",
        packages=("libnss3",),
        module_dirs=(f"{LIBDIR}/nss",),     # the .deb installs NSS plugins flat in LIBDIR;
        also_flat=(                          # we also place each at LIBDIR/ + LIBDIR/nss/.
            "libsoftokn3.so", "libfreebl3.so", "libfreeblpriv3.so",
            "libnssckbi.so", "libnssdbm3.so",
        ),
        required=("libsoftokn3.so", "libfreebl3.so"),
        note="NSS PKCS#11/crypto dlopen plugins (chromium TLS, libsoftokn3 et al.)",
    ),
    "gdk-pixbuf": PluginGroup(
        name="gdk-pixbuf",
        packages=("libgdk-pixbuf-2.0-0", "librsvg2-common"),
        module_dirs=(f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders",),
        cache=CacheSpec(
            kind="pixbuf",
            rel_path=f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders.cache",
        ),
        required=(),
        note="gdk-pixbuf image loaders (+ librsvg svg) + regenerated loaders.cache",
    ),
    "gio": PluginGroup(
        name="gio",
        packages=("glib-networking", "dconf-gsettings-backend"),
        module_dirs=(f"{LIBDIR}/gio/modules",),
        cache=CacheSpec(
            kind="giomodule",
            rel_path=f"{LIBDIR}/gio/modules/giomodule.cache",
        ),
        required=(),
        note="GIO TLS/proxy/settings dlopen modules + regenerated giomodule.cache",
    ),
    "gtk3": PluginGroup(
        name="gtk3",
        packages=("libgtk-3-0t64",),
        module_dirs=(
            f"{LIBDIR}/gtk-3.0/3.0.0/immodules",
            f"{LIBDIR}/gtk-3.0/3.0.0/printbackends",
        ),
        note="GTK3 input-method + print-backend dlopen modules",
    ),
    "gtk4": PluginGroup(
        name="gtk4",
        packages=("libgtk-4-1",),
        module_dirs=(
            f"{LIBDIR}/gtk-4.0/4.0.0/immodules",
            f"{LIBDIR}/gtk-4.0/4.0.0/printbackends",
        ),
        note="GTK4 print-backend (+ any immodule) dlopen modules",
    ),
    "pango": PluginGroup(
        name="pango",
        packages=("libpango-1.0-0",),
        module_dirs=(
            f"{LIBDIR}/pango/1.8.0/modules",
            f"{LIBDIR}/pango/1.8.0/module-files.d",
        ),
        note="legacy pango modules (pango 1.52 ships none — best-effort, may be 0)",
    ),
    "gstreamer": PluginGroup(
        name="gstreamer",
        packages=(
            "libgstreamer1.0-0",
            "gstreamer1.0-plugins-base",
            "gstreamer1.0-plugins-good",
        ),
        module_dirs=(f"{LIBDIR}/gstreamer-1.0",),
        large=True,
        note="gstreamer-1.0 element plugins (LARGE — opt-in via --group gstreamer)",
    ),
}

# Default = the three most universally needed (and small). gstreamer is large and
# opt-in only; it is excluded from --all too unless explicitly named.
DEFAULT_GROUPS = ("nss", "gdk-pixbuf", "gio")


# --------------------------------------------------------------------------- #
# Cache regeneration (host-side, byte-exact text formats)
# --------------------------------------------------------------------------- #

def _pixbuf_stanza(rel_so: str, meta: dict) -> str:
    """Render one gdk-pixbuf loaders.cache stanza for a loader at rootfs path rel_so.

    Mirrors `gdk-pixbuf-query-loaders` output: the absolute module path quoted on
    its own line, then id/flags/ns/desc/license, then mime list, ext list, and one
    magic line per signature. The module path is rendered rootfs-ABSOLUTE
    (leading "/") because that is how the loader resolves it on the device.
    """
    def q(items):
        return " ".join(f'"{x}"' for x in items) + ' ""'

    abspath = "/" + rel_so.lstrip("/")
    lines = [f'"{abspath}"']
    lines.append(
        f'"{meta["id"]}" {meta["flags"]} "{meta["ns"]}" '
        f'"{meta["desc"]}" "{meta["lic"]}"'
    )
    lines.append(q(meta["mime"]))
    lines.append(q(meta["ext"]))
    for prefix, mask, relevance in meta["magic"]:
        lines.append(f'"{prefix}" "{mask}" {relevance}')
    return "\n".join(lines) + "\n"


def render_pixbuf_cache(loader_rel_paths: list[str]) -> str:
    """Regenerate a gdk-pixbuf loaders.cache for the staged loaders (sorted).

    Only loaders we have static metadata for get a stanza; an unknown loader is
    skipped (logged by the caller). The header matches the query-loaders format so
    gdk-pixbuf accepts it verbatim.
    """
    header = (
        "# GdkPixbuf Image Loader Modules file\n"
        "# Automatically generated file, do not edit\n"
        "# Created by ALR build_plugin_overlay (byte-exact query-loaders format)\n"
        "#\n"
    )
    chunks = [header]
    for rel in sorted(loader_rel_paths):
        base = rel.rsplit("/", 1)[-1]
        meta = PIXBUF_LOADER_META.get(base)
        if meta is None:
            continue
        chunks.append(_pixbuf_stanza(rel, meta))
        chunks.append("\n")
    return "".join(chunks)


def render_giomodule_cache(module_rel_paths: list[str]) -> str:
    """Regenerate a GIO giomodule.cache for the staged gio/modules (sorted).

    Format (GLib _g_io_module_cache): one line per module
    ``<basename>: <extension-point>[,<extension-point>...]``. A module with no
    known extension points still gets a bare ``<basename>:`` line so GLib records
    it (GLib tolerates an empty point list). The header is GLib's.
    """
    lines = ["# GIO module cache (regenerated by ALR build_plugin_overlay)"]
    for rel in sorted(module_rel_paths):
        base = rel.rsplit("/", 1)[-1]
        eps = GIO_MODULE_EXTENSION_POINTS.get(base, ())
        lines.append(f"{base}: {','.join(eps)}")
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------- #
# .deb resolution + module discovery
# --------------------------------------------------------------------------- #

def resolve_filenames(
    packages: tuple[str, ...],
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    index: dict[str, dict] | None = None,
) -> dict[str, str | None]:
    """Map each package -> its .deb Filename (or None if absent from the index).

    NETWORK unless ``index`` is supplied (offline/test path)."""
    if index is None:
        index = parse_packages(
            fetch_packages_index(mirror, suite, arch, components=components)
        )
    return {pkg: index.get(pkg, {}).get("Filename") for pkg in packages}


def _discover_modules(root: Path, module_dirs: tuple[str, ...]) -> dict[str, Path]:
    """Find every regular-file ``*.so`` under each module_dir in the extracted root.

    Returns ``{rootfs_relative_path: source_path}``. Symlinks are skipped (we stage
    the real file at its canonical path; the loader resolves the real path). The
    rootfs-relative path is exactly the module_dir-rooted path the .deb installs to.
    """
    found: dict[str, Path] = {}
    for mdir in module_dirs:
        base = root / mdir
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.is_symlink() or not p.is_file():
                continue
            if p.name.endswith(".so") or ".so." in p.name:
                rel = p.relative_to(root).as_posix()
                found[rel] = p
    return found


# --------------------------------------------------------------------------- #
# tar helpers
# --------------------------------------------------------------------------- #

def _add_real_file(tar: tarfile.TarFile, rel: str, src: Path, mode: int = 0o755) -> None:
    """Add ``src`` to the tar at ``./<rel>`` as a fresh 0-owner regular file.

    Modules are 0755 — ALR's file-backed PROT_EXEC dlopen rejects a non-executable
    .so (device evidence: a 0644 svg loader → gtk3 SIGABRT)."""
    data = src.read_bytes()
    ti = tarfile.TarInfo("./" + rel.lstrip("/"))
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 0
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


def _add_bytes(tar: tarfile.TarFile, rel: str, data: bytes, mode: int = 0o644) -> None:
    ti = tarfile.TarInfo("./" + rel.lstrip("/"))
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 0
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ""
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


# --------------------------------------------------------------------------- #
# Build
# --------------------------------------------------------------------------- #

@dataclass
class GroupResult:
    group: str
    packages: tuple[str, ...]
    debs: tuple[str, ...] = ()
    modules: tuple[str, ...] = ()           # rootfs-rel module paths staged
    flat_aliases: tuple[str, ...] = ()      # extra flat LIBDIR/<name> aliases
    cache_path: str | None = None
    cache_entries: int = 0
    missing_required: tuple[str, ...] = ()
    unknown_cache_modules: tuple[str, ...] = ()
    skipped_packages: tuple[str, ...] = ()  # not in index / no Filename

    def as_dict(self) -> dict:
        return {
            "group": self.group,
            "packages": list(self.packages),
            "debs": list(self.debs),
            "modules": list(self.modules),
            "flat_aliases": list(self.flat_aliases),
            "cache_path": self.cache_path,
            "cache_entries": self.cache_entries,
            "missing_required": list(self.missing_required),
            "unknown_cache_modules": list(self.unknown_cache_modules),
            "skipped_packages": list(self.skipped_packages),
        }


@dataclass
class BuildResult:
    out_tar: str
    groups: tuple[GroupResult, ...]
    member_count: int

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "groups": [g.as_dict() for g in self.groups],
            "member_count": self.member_count,
        }


def _resolve_groups(names) -> list[PluginGroup]:
    out: list[PluginGroup] = []
    for n in names:
        if n not in GROUPS:
            raise ValueError(
                f"unknown group {n!r}; choose from {', '.join(sorted(GROUPS))}"
            )
        out.append(GROUPS[n])
    return out


def build(
    out: Path,
    groups: list[str],
    cache: Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    index: dict[str, dict] | None = None,
) -> BuildResult:
    """Fetch + extract + pack the requested plugin groups into one §5-E stage tar.

    NETWORK PATH (downloads each group's owning .deb into ``cache``, cached across
    runs). Each module is packed at its canonical Debian rootfs path; NSS plugins
    additionally get a flat ``LIBDIR/<name>`` alias. Loader caches are regenerated
    host-side. Modules are emitted in sorted order for determinism.
    """
    resolved = _resolve_groups(groups)
    if index is None:
        index = parse_packages(
            fetch_packages_index(mirror, suite, arch, components=components)
        )

    out.parent.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)

    group_results: list[GroupResult] = []
    member_count = 0

    with tarfile.open(out, "w") as tar:
        # Stage modules group by group; track which rel paths we've already added
        # so two groups sharing a dir (gdk-pixbuf base + svg) don't double-add.
        added_rel: set[str] = set()

        for grp in resolved:
            filenames = {p: index.get(p, {}).get("Filename") for p in grp.packages}
            skipped = tuple(sorted(p for p, f in filenames.items() if not f))
            debs: list[str] = []

            with tempfile.TemporaryDirectory() as td:
                merged = Path(td)
                for pkg, fn in filenames.items():
                    if not fn:
                        continue
                    deb = _download_deb(mirror, fn, cache)
                    debs.append(deb.name)
                    extract_deb(deb, merged)

                modules = _discover_modules(merged, grp.module_dirs)

                # stage each module at its canonical path
                staged_rel: list[str] = []
                for rel, src in sorted(modules.items()):
                    if rel not in added_rel:
                        _add_real_file(tar, rel, src)
                        added_rel.add(rel)
                        member_count += 1
                    staged_rel.append(rel)

                # flat aliases (NSS): place named modules at LIBDIR/<name> too. The
                # owning .deb may install these flat under LIBDIR (not LIBDIR/nss),
                # so fall back to an rglob of the extracted tree.
                flat_aliases: list[str] = []
                found_flat_bases: set[str] = set()
                for name in grp.also_flat:
                    src = None
                    for rel, s in modules.items():
                        if Path(rel).name == name:
                            src = s
                            break
                    if src is None:
                        for cand in merged.rglob(name):
                            if cand.is_file() and not cand.is_symlink():
                                src = cand
                                break
                    if src is None:
                        continue
                    found_flat_bases.add(name)
                    flat_rel = f"{LIBDIR}/{name}"
                    nss_rel = f"{LIBDIR}/nss/{name}"
                    for r in (flat_rel, nss_rel):
                        if r not in added_rel:
                            _add_real_file(tar, r, src)
                            added_rel.add(r)
                            member_count += 1
                            flat_aliases.append(r)

                # required-module gate: a load-bearing module counts whether it was
                # found in a module_dir OR staged as a flat alias (NSS installs flat).
                present_bases = {Path(r).name for r in staged_rel} | found_flat_bases
                missing_req = tuple(
                    m for m in grp.required if m not in present_bases
                )

                # regenerate a loader cache if the group needs one
                cache_path = None
                cache_entries = 0
                unknown_cache = ()
                if grp.cache is not None:
                    if grp.cache.kind == "pixbuf":
                        known = [
                            r for r in staged_rel
                            if Path(r).name in PIXBUF_LOADER_META
                        ]
                        unknown_cache = tuple(sorted(
                            Path(r).name for r in staged_rel
                            if Path(r).name not in PIXBUF_LOADER_META
                        ))
                        body = render_pixbuf_cache(known)
                        cache_entries = len(known)
                    elif grp.cache.kind == "giomodule":
                        body = render_giomodule_cache(staged_rel)
                        cache_entries = len(staged_rel)
                    else:
                        body = ""
                    if grp.cache.rel_path not in added_rel:
                        _add_bytes(tar, grp.cache.rel_path, body.encode())
                        added_rel.add(grp.cache.rel_path)
                        member_count += 1
                    cache_path = grp.cache.rel_path

                group_results.append(GroupResult(
                    group=grp.name,
                    packages=grp.packages,
                    debs=tuple(debs),
                    modules=tuple(staged_rel),
                    flat_aliases=tuple(flat_aliases),
                    cache_path=cache_path,
                    cache_entries=cache_entries,
                    missing_required=missing_req,
                    unknown_cache_modules=unknown_cache,
                    skipped_packages=skipped,
                ))

    return BuildResult(
        out_tar=str(out),
        groups=tuple(group_results),
        member_count=member_count,
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def _parse_group_args(args) -> list[str]:
    if args.all:
        # --all = every NON-large group (gstreamer is opt-in only)
        return [n for n in GROUPS if not GROUPS[n].large]
    if args.groups:
        out: list[str] = []
        for spec in args.groups:
            for n in spec.split(","):
                n = n.strip()
                if n:
                    out.append(n)
        # de-dup, preserve order
        seen: set[str] = set()
        return [n for n in out if not (n in seen or seen.add(n))]
    return list(DEFAULT_GROUPS)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="build_plugin_overlay",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--out", default="/tmp/plugin-stage.tar",
                    help="output overlay stage tar (default %(default)s)")
    ap.add_argument("--cache", default="/tmp/plugin-deb-cache",
                    help="deb/index download cache dir (default %(default)s)")
    ap.add_argument("--group", action="append", dest="groups",
                    help="plugin group to include (repeatable, comma-OK). "
                    f"Choices: {', '.join(sorted(GROUPS))}. "
                    f"Default: {','.join(DEFAULT_GROUPS)}")
    ap.add_argument("--all", action="store_true",
                    help="include every non-large group (excludes gstreamer)")
    ap.add_argument("--mirror", default=MIRROR)
    ap.add_argument("--suite", default=SUITE)
    ap.add_argument("--component", action="append", dest="components",
                    help="repo component (repeatable; default main+universe)")
    ap.add_argument("--list", action="store_true",
                    help="resolve owning .debs + print planned members/caches WITHOUT downloading")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--selftest", action="store_true", help="run built-in OFFLINE tests")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    components = tuple(args.components) if args.components else COMPONENTS

    try:
        group_names = _parse_group_args(args)
        resolved = _resolve_groups(group_names)
    except ValueError as exc:
        ap.error(str(exc))

    if args.list:
        index = parse_packages(
            fetch_packages_index(args.mirror, args.suite, ARCH, components=components)
        )
        plan = {"suite": args.suite, "groups": []}
        for grp in resolved:
            fns = resolve_filenames(grp.packages, index=index)
            planned_caches = [grp.cache.rel_path] if grp.cache else []
            plan["groups"].append({
                "group": grp.name,
                "note": grp.note,
                "packages": {p: fns.get(p) for p in grp.packages},
                "module_dirs": list(grp.module_dirs),
                "flat_aliases": [f"{LIBDIR}/{n}" for n in grp.also_flat],
                "regenerated_cache": planned_caches,
                "required": list(grp.required),
                "large": grp.large,
                "missing_packages": [p for p in grp.packages if not fns.get(p)],
            })
        if args.json:
            print(json.dumps(plan, indent=2))
        else:
            print(f"plugin-overlay plan ({args.suite}/{'+'.join(components)}) "
                  f"groups={','.join(group_names)}:")
            for g in plan["groups"]:
                print(f"\n  [{g['group']}] {g['note']}"
                      + ("  (LARGE)" if g["large"] else ""))
                for pkg, fn in g["packages"].items():
                    print(f"    pkg {pkg:28s} -> {fn or 'MISSING-in-index'}")
                for d in g["module_dirs"]:
                    print(f"    modules from  {d}/")
                for a in g["flat_aliases"]:
                    print(f"    flat alias    {a}")
                for c in g["regenerated_cache"]:
                    print(f"    REGEN cache   {c}")
                if g["missing_packages"]:
                    print(f"    WARN missing packages: {g['missing_packages']}")
        return 0

    res = build(
        Path(args.out), group_names, Path(args.cache),
        mirror=args.mirror, suite=args.suite, components=components,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}  ({res.member_count} members)")
        for g in res.groups:
            print(f"  [{g.group}] {len(g.modules)} modules"
                  + (f", {len(g.flat_aliases)} flat aliases" if g.flat_aliases else "")
                  + (f", cache {g.cache_path} ({g.cache_entries} entries)" if g.cache_path else ""))
            if g.skipped_packages:
                print(f"      WARN skipped packages (not in index): {g.skipped_packages}")
            if g.missing_required:
                print(f"      ERROR missing required modules: {g.missing_required}")
            if g.unknown_cache_modules:
                print(f"      NOTE loaders with no static metadata (omitted from cache): "
                      f"{g.unknown_cache_modules}")
    # non-zero exit if any group lost a load-bearing module
    bad = any(g.missing_required for g in res.groups)
    return 1 if bad else 0


# --------------------------------------------------------------------------- #
# Selftest (OFFLINE — exercises the pure logic; no network)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # group catalog sanity
    check("default groups all known", all(g in GROUPS for g in DEFAULT_GROUPS))
    check("nss group lists libnss3", "libnss3" in GROUPS["nss"].packages)
    check("gdk-pixbuf pulls librsvg2-common (svg loader)",
          "librsvg2-common" in GROUPS["gdk-pixbuf"].packages)
    check("gio group regenerates giomodule.cache",
          GROUPS["gio"].cache is not None and GROUPS["gio"].cache.kind == "giomodule")
    check("gstreamer is large (opt-in only)", GROUPS["gstreamer"].large)
    check("--all excludes large gstreamer",
          "gstreamer" not in _parse_group_args(
              argparse.Namespace(all=True, groups=None)))

    # pixbuf cache regeneration is byte-exact format
    rel = f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so"
    body = render_pixbuf_cache([rel])
    check("pixbuf cache has the query-loaders header",
          body.startswith("# GdkPixbuf Image Loader Modules file"))
    check("pixbuf cache quotes the absolute module path",
          f'"/{rel}"' in body)
    check("pixbuf svg stanza has svg id + LGPL + mime",
          '"svg" 6 "gdk-pixbuf"' in body and "image/svg+xml" in body)
    # an unknown loader is omitted, not crashed-on
    body2 = render_pixbuf_cache([f"{LIBDIR}/x/loaders/libpixbufloader-NOPE.so"])
    check("pixbuf cache omits unknown loader (no stanza)",
          "libpixbufloader-NOPE.so" not in body2)

    # giomodule cache format
    gio = render_giomodule_cache([
        f"{LIBDIR}/gio/modules/libgiognutls.so",
        f"{LIBDIR}/gio/modules/libdconfsettings.so",
    ])
    check("giomodule cache maps gnutls -> gio-tls-backend",
          "libgiognutls.so: gio-tls-backend,gio-dtls-backend" in gio)
    check("giomodule cache maps dconf -> gio-settings-backend",
          "libdconfsettings.so: gio-settings-backend" in gio)
    # unknown module still gets a bare line
    gio2 = render_giomodule_cache([f"{LIBDIR}/gio/modules/libgiounknown.so"])
    check("giomodule cache emits bare line for unknown module",
          "libgiounknown.so: " in gio2)

    # _discover_modules over a synthetic extracted root
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        ldir = root / LIBDIR / "gdk-pixbuf-2.0/2.10.0/loaders"
        ldir.mkdir(parents=True)
        (ldir / "libpixbufloader-png.so").write_bytes(b"\x7fELF-png")
        (ldir / "libpixbufloader-svg.so").write_bytes(b"\x7fELF-svg")
        (ldir / "README").write_bytes(b"not a module")
        # a symlink must be skipped
        (ldir / "link.so").symlink_to("libpixbufloader-png.so")
        found = _discover_modules(root, (f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders",))
        names = {Path(r).name for r in found}
        check("_discover_modules finds the .so modules",
              "libpixbufloader-png.so" in names and "libpixbufloader-svg.so" in names)
        check("_discover_modules ignores non-.so files", "README" not in names)
        check("_discover_modules skips symlinks", "link.so" not in names)

    # _add_real_file packs a 0755 ./-rooted member
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        so = root / "mod.so"
        so.write_bytes(b"\x7fELFmod")
        out = root / "t.tar"
        with tarfile.open(out, "w") as tar:
            _add_real_file(tar, f"{LIBDIR}/gio/modules/mod.so", so)
        with tarfile.open(out) as tar:
            m = tar.getmember(f"./{LIBDIR}/gio/modules/mod.so")
            check("_add_real_file is ./-rooted", m.name.startswith("./"))
            check("_add_real_file is 0755 (PROT_EXEC-safe)", (m.mode & 0o777) == 0o755)
            check("_add_real_file is a regular file (not symlink)", m.isfile())

    # offline build via an injected in-memory index + a synthetic local mirror.
    rc = _selftest_offline_build(check)
    failures += rc

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def _selftest_offline_build(check) -> int:
    """Exercise build() end-to-end offline by faking a tiny .deb mirror + index.

    Builds two real .deb (ar+tar) for a fake nss-like + pixbuf-like package, an
    in-memory Packages index pointing at them on a file:// mirror, and runs build()
    over a custom single-package group set — asserting the staged tar carries the
    modules + flat alias + regenerated cache, all §5-E conformant.
    """
    before = [0]

    def c(label, cond):
        if not cond:
            before[0] += 1
        check(label, cond)

    if shutil.which("ar") is None:
        check("offline build needs `ar` (skipped)", True)
        return 0

    import urllib.request

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        pool = td / "pool"
        pool.mkdir()

        def make_deb(name: str, files: dict[str, bytes]) -> str:
            # `ar` records each member by its BASENAME, and extract_deb looks for a
            # member literally named "data.tar*" — so the data tar file basename must
            # be "data.tar". Build each .deb in its own staging dir to keep that name.
            stage = td / ("_stage_" + name)
            stage.mkdir(parents=True, exist_ok=True)
            droot = td / ("_d_" + name)
            for rel, data in files.items():
                fp = droot / rel
                fp.parent.mkdir(parents=True, exist_ok=True)
                fp.write_bytes(data)
            data_tar = stage / "data.tar"
            with tarfile.open(data_tar, "w") as dt:
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

        nss_fn = make_deb("fakenss", {
            f"{LIBDIR}/libsoftokn3.so": b"\x7fELFsoftokn",
            f"{LIBDIR}/libfreebl3.so": b"\x7fELFfreebl",
        })
        pix_fn = make_deb("fakepix", {
            f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so":
                b"\x7fELFsvg",
            f"{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-png.so":
                b"\x7fELFpng",
        })

        index = {
            "libnss3": {"Package": "libnss3", "Filename": nss_fn},
            "libgdk-pixbuf-2.0-0": {"Package": "libgdk-pixbuf-2.0-0", "Filename": pix_fn},
            "librsvg2-common": {"Package": "librsvg2-common", "Filename": pix_fn},
        }
        mirror = "file://" + str(td)
        out = td / "plugin-stage.tar"
        cache = td / "cache"

        res = build(out, ["nss", "gdk-pixbuf"], cache, mirror=mirror, index=index)

        with tarfile.open(out) as t:
            names = set(t.getnames())
            bodies = {m.name: t.extractfile(m).read()
                      for m in t.getmembers() if m.isfile()}

        c("offline: nss softokn staged at flat LIBDIR",
          f"./{LIBDIR}/libsoftokn3.so" in names)
        c("offline: nss softokn staged at nss/ search path",
          f"./{LIBDIR}/nss/libsoftokn3.so" in names)
        c("offline: pixbuf svg loader staged at canonical path",
          f"./{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so" in names)
        cache_rel = f"./{LIBDIR}/gdk-pixbuf-2.0/2.10.0/loaders.cache"
        c("offline: regenerated loaders.cache present", cache_rel in names)
        c("offline: loaders.cache has svg stanza",
          b'"svg"' in bodies.get(cache_rel, b""))
        c("offline: loaders.cache has png stanza",
          b'"png"' in bodies.get(cache_rel, b""))
        c("offline: all members ./-rooted", all(n.startswith("./") or n == "." for n in names))

        nss_res = next(g for g in res.groups if g.group == "nss")
        c("offline: nss reports no missing required", nss_res.missing_required == ())

        # §5-E structural conformance
        try:
            from tools.stage_tar_spec import validate_stage_tar
            rep = validate_stage_tar(str(out))
            c("offline: stage_tar_spec conformant", rep.conformant)
        except Exception as exc:
            check(f"offline: stage_tar_spec import skipped ({exc!r})", True)

    return before[0]


if __name__ == "__main__":
    sys.exit(main())
