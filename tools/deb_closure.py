"""Overlay BUILD ENGINE (WS-4 / L4 rootfs) — Debian dependency-closure overlays.

The M2 (netsurf-gtk / SDL2 / Qt6) and M4 (xwayland) overlays must be buildable
from a host that has no Debian build environment. This module is that engine.

Pipeline
--------
1. parse the suite ``Packages`` index (``dists/<suite>/main/binary-<arch>/Packages.gz``)
   into name -> stanza dicts (``parse_packages``);
2. resolve the *runtime* dependency closure of one or more target packages over
   ``Depends`` + ``Pre-Depends``, satisfying ``a | b`` alternatives and virtual
   packages via ``Provides`` (``resolve_closure``);
3. SUBTRACT what the base rootfs already provides: every shared-library SONAME the
   base ships as a real flat file is FROZEN (the harfbuzz-downgrade class the
   ``overlay_guard`` blocks), so any extracted ``.so`` whose SONAME the base owns is
   dropped before we tar — the base library always wins (``base_soname_set`` +
   ``drop_base_sonames``);
4. download each closure package's ``.deb`` (cached), extract it into one merged
   root via :func:`tools.build_stage_tar.extract_deb`, drop the base-owned SONAME
   files, and flatten the merged root to a §5-E ``./``-rooted overlay tar via
   :func:`tools.build_stage_tar.build_stage_tar`;
5. re-check the produced overlay against the base with
   :func:`tools.overlay_guard.scan_overlay_violations` and return the verdict.

Only the ``build_overlay`` path touches the network. ``--selftest`` is fully
OFFLINE and deterministic (it exercises parsing, closure resolution and base
subtraction against in-memory fixtures).

This module reuses, and does NOT duplicate:
  * ``tools.build_stage_tar`` — extract_deb + build_stage_tar (the §5-E flattener)
  * ``tools.overlay_guard``   — build_base_soname_index + scan_overlay_violations
  * ``tools.safe_tar``        — (transitively, via the two above)
"""

from __future__ import annotations

import argparse
import gzip
import os
import sys
import tempfile
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

from tools.build_stage_tar import build_stage_tar, extract_deb
from tools.overlay_guard import build_base_soname_index, parse_solib, scan_overlay_violations
from tools.safe_tar import inspect_tar_members


def _urlopen_ua(url):
    """urlopen with an apt-like User-Agent — ports.ubuntu.com 403s the default
    Python-urllib UA. Used as the default opener for all mirror fetches."""
    req = urllib.request.Request(url, headers={"User-Agent": "Debian APT-HTTP/1.3 (ALR deb_closure)"})
    return urllib.request.urlopen(req)


# --------------------------------------------------------------------------- #
# Packages index parsing
# --------------------------------------------------------------------------- #

def parse_packages(text: str) -> dict[str, dict]:
    """Parse a Debian ``Packages`` file into ``{name: {field: value}}``.

    Stanzas are separated by blank lines; each is a set of ``Field: value`` lines
    where a continuation line begins with a space/tab. We keep the fields the
    closure engine needs (Package, Version, Depends, Pre-Depends, Provides,
    Filename) plus any other simple scalar field encountered. The dict is keyed by
    the ``Package`` name; later stanzas for the same name win (mirrors apt).
    """
    index: dict[str, dict] = {}
    for stanza in _iter_stanzas(text):
        fields = _parse_stanza(stanza)
        name = fields.get("Package")
        if name:
            index[name] = fields
    return index


def _iter_stanzas(text: str):
    """Yield raw stanza strings (split on blank lines), skipping empties."""
    current: list[str] = []
    for line in text.splitlines():
        if line.strip() == "":
            if current:
                yield "\n".join(current)
                current = []
        else:
            current.append(line)
    if current:
        yield "\n".join(current)


def _parse_stanza(stanza: str) -> dict[str, str]:
    """Parse one RFC822-ish stanza into a {field: value} dict (folding continuations)."""
    fields: dict[str, str] = {}
    key: str | None = None
    for line in stanza.split("\n"):
        if not line:
            continue
        if line[0] in " \t":
            # continuation of the previous field
            if key is not None:
                fields[key] = fields[key] + "\n" + line.strip()
            continue
        head, sep, value = line.partition(":")
        if not sep:
            continue
        key = head.strip()
        fields[key] = value.strip()
    return fields


def parse_depends(field: str) -> list[list[str]]:
    """Parse a Depends/Pre-Depends field into groups of alternatives.

    Comma-separated groups; each group is a list of alternatives (``a | b``).
    Version constraints ``(>= x)``, arch qualifiers ``:any`` / ``:arm64`` and
    surrounding whitespace are stripped. Empty entries are ignored.

    >>> parse_depends("libc6 (>= 2.34), libfoo | libbar, libbaz:any (>= 1.0)")
    [['libc6'], ['libfoo', 'libbar'], ['libbaz']]
    """
    if not field:
        return []
    groups: list[list[str]] = []
    for raw_group in field.replace("\n", " ").split(","):
        alternatives: list[str] = []
        for raw_alt in raw_group.split("|"):
            name = _strip_dep_name(raw_alt)
            if name:
                alternatives.append(name)
        if alternatives:
            groups.append(alternatives)
    return groups


def _strip_dep_name(token: str) -> str:
    """Reduce a single dependency token to its bare package name."""
    text = token.strip()
    if not text:
        return ""
    # drop a version constraint "(>= 1.0)" / build profile "<...>" / anything after
    paren = text.find("(")
    if paren >= 0:
        text = text[:paren]
    angle = text.find("<")
    if angle >= 0:
        text = text[:angle]
    text = text.strip()
    # drop an architecture qualifier ":any", ":arm64", etc.
    colon = text.find(":")
    if colon >= 0:
        text = text[:colon]
    return text.strip()


# --------------------------------------------------------------------------- #
# Closure resolution
# --------------------------------------------------------------------------- #

def build_provides_map(index: dict[str, dict]) -> dict[str, list[str]]:
    """Map each virtual package name to the real packages that Provide it.

    ``Provides`` entries may carry a version (``foo (= 1.0)``) which is stripped.
    A real package is recorded under each virtual name it provides; the lists are
    kept in index-iteration order for determinism.
    """
    provides_map: dict[str, list[str]] = {}
    for name, fields in index.items():
        provides = fields.get("Provides", "")
        if not provides:
            continue
        for raw in provides.split(","):
            virtual = _strip_dep_name(raw)
            if not virtual or virtual == name:
                continue
            bucket = provides_map.setdefault(virtual, [])
            if name not in bucket:
                bucket.append(name)
    return provides_map


def _resolve_alternative(
    alternatives: list[str],
    index: dict[str, dict],
    provides_map: dict[str, list[str]],
) -> str | None:
    """Pick the first alternative that resolves to a concrete package, or None.

    An alternative resolves if it is a real package in ``index`` directly, or if a
    real package Provides it (first provider wins).
    """
    for alt in alternatives:
        if alt in index:
            return alt
        providers = provides_map.get(alt)
        if providers:
            return providers[0]
    return None


def resolve_closure(
    targets: list[str],
    index: dict[str, dict],
    *,
    provides_map: dict[str, list[str]] | None = None,
    log: list[str] | None = None,
) -> list[str]:
    """BFS the runtime dependency closure of ``targets`` over the index.

    For each package we expand ``Depends`` + ``Pre-Depends``; an alternative group
    is satisfied by the first alternative present in the index or satisfied via
    ``Provides``. Dependencies absent from the index are skipped (and appended to
    ``log`` when provided), never fatal. Targets that are themselves missing are
    likewise skipped with a log note.

    Returns the ordered, de-duplicated list of concrete package names (the targets
    that exist plus their transitive deps), in stable BFS discovery order.
    """
    if provides_map is None:
        provides_map = build_provides_map(index)

    ordered: list[str] = []
    seen: set[str] = set()
    queue: list[str] = []

    def enqueue(name: str) -> None:
        if name not in seen:
            seen.add(name)
            queue.append(name)

    for target in targets:
        if target in index:
            enqueue(target)
        else:
            resolved = _resolve_alternative([target], index, provides_map)
            if resolved is not None:
                enqueue(resolved)
            elif log is not None:
                log.append(f"target not in index: {target}")

    while queue:
        name = queue.pop(0)
        ordered.append(name)
        fields = index.get(name, {})
        dep_field = " , ".join(
            v for v in (fields.get("Pre-Depends", ""), fields.get("Depends", "")) if v
        )
        for group in parse_depends(dep_field):
            resolved = _resolve_alternative(group, index, provides_map)
            if resolved is not None:
                enqueue(resolved)
            elif log is not None:
                log.append(f"unsatisfied dep group {group} (from {name})")

    return ordered


# --------------------------------------------------------------------------- #
# Base subtraction
# --------------------------------------------------------------------------- #

def base_soname_set(base: str | Path) -> set[str]:
    """The set of SONAMEs the base rootfs provides as real library files.

    Derived from :func:`tools.overlay_guard.build_base_soname_index`; each entry's
    ``soname`` (e.g. ``libharfbuzz.so.0``) is a frozen name the overlay must not
    ship — the base library wins to prevent downgrades.
    """
    return {entry.soname for entry in build_base_soname_index(base).values()}


def drop_base_sonames(merged_root: str | Path, base_sonames: set[str]) -> list[str]:
    """Delete, from an extracted merged root, every shared-library file whose
    SONAME is already provided by the base.

    A file is removed if it is a real (non-symlink) ``.so`` whose parsed SONAME is
    in ``base_sonames``. SONAME / dev symlinks for those libraries are removed too
    (they would otherwise survive ``build_stage_tar`` as dangling-or-dropped, and
    repointing a frozen base soname is exactly what the guard blocks). Non-library
    files and libraries the base does NOT own are left untouched.

    Returns the sorted list of rootfs-relative paths removed (deterministic).
    """
    root = Path(merged_root)
    removed: list[str] = []
    if not root.is_dir():
        return removed

    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            full = Path(dirpath) / fname
            rel = full.relative_to(root).as_posix()
            soname = _soname_of(fname, full)
            if soname is not None and soname in base_sonames:
                try:
                    full.unlink()
                except OSError:
                    continue
                removed.append(rel)

    removed.sort()
    return removed


def _soname_of(fname: str, full: Path) -> str | None:
    """Return the SONAME a library FILE or SONAME-SYMLINK maps to, else None.

    A real versioned/flat file ``libfoo.so.1[.2.3]`` -> ``libfoo.so.1``. A SONAME
    symlink (``libfoo.so.1`` -> ``libfoo.so.1.2.3``) -> ``libfoo.so.1``. A bare
    ``libfoo.so`` dev symlink resolves to its target's soname when that target is a
    versioned name; otherwise None (we only drop things we can attribute to a base
    soname).
    """
    lib = parse_solib(fname)
    if lib is not None:
        return lib.soname
    # bare "libfoo.so" dev symlink: attribute via its link target's soname.
    if fname.endswith(".so") and full.is_symlink():
        try:
            target = os.readlink(full)
        except OSError:
            return None
        target_lib = parse_solib(PurePosixPath(target).name)
        if target_lib is not None:
            return target_lib.soname
    return None


def _mergedusr_aliases(rel: str):
    """Yield a path plus its merged-usr counterpart(s).

    Debian merged-usr makes ``/lib`` -> ``/usr/lib`` (also bin/sbin/lib64), so a
    base file at ``usr/lib/aarch64-linux-gnu/libc.so.6`` and a freshly-extracted
    .deb file at ``lib/aarch64-linux-gnu/libc.so.6`` are the SAME file. Treat both
    spellings as equivalent for subtraction — otherwise libc6 & friends leak into
    the overlay and would shadow the base runtime.
    """
    yield rel
    for top in ("lib", "bin", "sbin", "lib64"):
        if rel.startswith(f"usr/{top}/"):
            yield rel[len("usr/"):]
        elif rel.startswith(f"{top}/"):
            yield f"usr/{rel}"


def base_path_set(base: str | Path) -> set[str]:
    """Every rootfs-relative file/symlink path the base already provides, expanded
    with merged-usr aliases.

    Used to subtract base-duplicate payload (charset converters, libc runtime,
    data, configs, unversioned helpers) that ``drop_base_sonames`` leaves behind —
    so the overlay carries only what is genuinely NEW. The base wins for any path
    it already owns, under either the ``/lib`` or ``/usr/lib`` spelling.
    """
    base_path = Path(base)
    raw: set[str] = set()
    if base_path.is_dir():
        base_path = base_path.resolve()
        for dirpath, _dirs, files in os.walk(base_path):
            for fname in files:
                raw.add((Path(dirpath) / fname).relative_to(base_path).as_posix())
    else:
        raw = {m.name for m in inspect_tar_members(base_path) if m.kind != "dir"}
    out: set[str] = set()
    for rel in raw:
        out.update(_mergedusr_aliases(rel))
    return out


def drop_base_paths(merged_root: str | Path, base_paths: set[str]) -> list[str]:
    """Delete every merged-root FILE whose rootfs-relative path the base already
    provides (path-level subtraction, complementing the SONAME-level one).

    Leaves files at paths NOT in the base — i.e. the package's genuinely new
    payload. Returns the sorted list of removed paths (deterministic).
    """
    root = Path(merged_root)
    removed: list[str] = []
    if not root.is_dir():
        return removed
    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            full = Path(dirpath) / fname
            rel = full.relative_to(root).as_posix()
            if rel in base_paths:
                try:
                    full.unlink()
                except OSError:
                    continue
                removed.append(rel)
    removed.sort()
    return removed


# Path prefixes that are runtime-irrelevant for an ALR guest (no systemd, no docs,
# C.UTF-8 only, no dev). Dropping them shrinks overlays a lot with zero runtime
# impact. Deliberately NOT pruned: usr/share/icons (GTK needs them), fonts, mime,
# usr/share/perl (some apps exec perl), libicu, gconv.
DEFAULT_PRUNE_PREFIXES = (
    "usr/share/man/",
    "usr/share/doc/",
    "usr/share/doc-base/",
    "usr/share/info/",
    "usr/share/lintian/",
    "usr/share/bug/",
    "usr/share/locale/",
    "usr/share/gtk-doc/",
    "usr/share/pkgconfig/",
    "usr/include/",
    "lib/systemd/",
    "usr/lib/systemd/",
    "etc/systemd/",
    "lib/tmpfiles.d/",
    "usr/lib/tmpfiles.d/",
    "var/lib/dpkg/",
    "var/cache/",
    "var/log/",
)


def prune_paths(merged_root: str | Path, prefixes) -> list[str]:
    """Delete merged-root files under any runtime-irrelevant prefix.

    The prefixes match BOTH ``X`` and ``usr/X`` via merged-usr aliasing. Returns the
    sorted list of removed rootfs-relative paths.
    """
    root = Path(merged_root)
    removed: list[str] = []
    if not root.is_dir() or not prefixes:
        return removed
    aliased: set[str] = set()
    for p in prefixes:
        aliased.update(_mergedusr_aliases(p.rstrip("/") + "/"))
    aliased_t = tuple(aliased)
    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            full = Path(dirpath) / fname
            rel = full.relative_to(root).as_posix()
            if rel.startswith(aliased_t):
                try:
                    full.unlink()
                except OSError:
                    continue
                removed.append(rel)
    removed.sort()
    return removed


# --------------------------------------------------------------------------- #
# Build path (network)
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class OverlayBuild:
    closure: tuple[str, ...]
    skipped_base: tuple[str, ...]          # rootfs-rel lib files dropped (base owns soname)
    unsupported: tuple[str, ...]           # packages skipped (e.g. .zst data member)
    missing: tuple[str, ...]               # closure log lines (deps not in index)
    out_tar: str
    sidecar: str
    file_count: int
    violations: tuple[str, ...]            # rendered overlay_guard violations
    pruned: tuple[str, ...] = ()           # runtime-irrelevant paths dropped (man/doc/…)

    def as_dict(self) -> dict:
        return {
            "closure": list(self.closure),
            "skipped_base": list(self.skipped_base),
            "unsupported": list(self.unsupported),
            "missing": list(self.missing),
            "out_tar": self.out_tar,
            "sidecar": self.sidecar,
            "file_count": self.file_count,
            "violations": list(self.violations),
            "pruned": list(self.pruned),
        }


def fetch_packages_index(
    mirror: str, suite: str, arch: str, *, components=("main",), opener=_urlopen_ua
) -> str:
    """Download + gunzip the Packages index for each component and concatenate.

    Debian arm64: ``deb.debian.org/debian`` suite ``bookworm`` component ``main``.
    Ubuntu arm64 (the ALR base is Ubuntu noble): ``ports.ubuntu.com/ubuntu-ports``
    suite ``noble`` components ``("main", "universe")`` — Ubuntu splits libraries
    across main/universe, so fetching only ``main`` misses many deps. Stanzas from
    all components are concatenated (parse_packages handles the merge; first wins).
    """
    if isinstance(components, str):
        components = (components,)
    parts: list[str] = []
    for comp in components:
        url = f"{mirror.rstrip('/')}/dists/{suite}/{comp}/binary-{arch}/Packages.gz"
        with opener(url) as resp:
            blob = resp.read()
        parts.append(gzip.decompress(blob).decode("utf-8", "replace"))
    return "\n\n".join(parts)


def _download_deb(
    mirror: str, filename: str, cache_dir: Path, *, opener=_urlopen_ua
) -> Path:
    """Download ``mirror/<Filename>`` into cache_dir (skip if already cached)."""
    dest = cache_dir / PurePosixPath(filename).name
    if dest.is_file() and dest.stat().st_size > 0:
        return dest
    url = f"{mirror.rstrip('/')}/{filename.lstrip('/')}"
    cache_dir.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    with opener(url) as resp:
        tmp.write_bytes(resp.read())
    tmp.replace(dest)
    return dest


def build_overlay(
    targets: list[str],
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = "http://deb.debian.org/debian",
    suite: str = "bookworm",
    arch: str = "arm64",
    components=("main",),
    cache_dir: str | Path | None = None,
    prune=DEFAULT_PRUNE_PREFIXES,
    opener=_urlopen_ua,
) -> dict:
    """Resolve, download, base-subtract and flatten an overlay for ``targets``.

    NETWORK PATH. Downloads the suite Packages index and each closure ``.deb`` into
    ``cache_dir`` (cached across runs), extracts them into one merged root, drops
    every library whose SONAME the base already owns, then flattens to a §5-E
    overlay tar via ``build_stage_tar`` and re-checks it with
    ``overlay_guard.scan_overlay_violations``.

    Resilient: a ``.deb`` whose data member is zstd-compressed (``extract_deb``
    raises ``NotImplementedError`` on the host) is recorded as ``unsupported`` and
    skipped, never fatal.

    Returns the :meth:`OverlayBuild.as_dict` mapping.
    """
    cache = Path(cache_dir) if cache_dir is not None else Path(tempfile.mkdtemp(prefix="deb-closure-"))
    cache.mkdir(parents=True, exist_ok=True)

    text = fetch_packages_index(mirror, suite, arch, components=components, opener=opener)
    index = parse_packages(text)
    provides_map = build_provides_map(index)

    log: list[str] = []
    closure = resolve_closure(targets, index, provides_map=provides_map, log=log)

    base_sonames = base_soname_set(base)

    unsupported: list[str] = []
    merged_root = cache / "_merged_root"
    if merged_root.exists():
        # start clean so repeated runs are deterministic
        import shutil

        shutil.rmtree(merged_root)
    merged_root.mkdir(parents=True)

    for name in closure:
        fields = index.get(name, {})
        filename = fields.get("Filename")
        if not filename:
            unsupported.append(f"{name} (no Filename in index)")
            continue
        try:
            deb_path = _download_deb(mirror, filename, cache, opener=opener)
        except Exception as exc:  # network / IO — record and continue
            unsupported.append(f"{name} (download failed: {exc})")
            continue
        try:
            extract_deb(deb_path, merged_root)
        except NotImplementedError:
            unsupported.append(f"{name} (zstd .deb — extract in Debian env)")
        except Exception as exc:
            unsupported.append(f"{name} (extract failed: {exc})")

    # Subtract what the base already provides: SONAME-frozen libraries first
    # (downgrade protection), then any remaining base-duplicate path (gconv, data,
    # configs) — leaving only the package's genuinely new payload.
    skipped_sonames = drop_base_sonames(merged_root, base_sonames)
    skipped_paths = drop_base_paths(merged_root, base_path_set(base))
    skipped_base = sorted(set(skipped_sonames) | set(skipped_paths))

    # Drop runtime-irrelevant payload (man/doc/locale/systemd/dev …) to keep the
    # overlay practical — the full transitive Depends closure over-includes.
    pruned = prune_paths(merged_root, prune)

    result = build_stage_tar(merged_root, out_tar)

    violations = scan_overlay_violations(base, out_tar)

    build = OverlayBuild(
        closure=tuple(closure),
        skipped_base=tuple(skipped_base),
        unsupported=tuple(unsupported),
        missing=tuple(log),
        out_tar=result.out_tar,
        sidecar=result.sidecar,
        file_count=result.file_count,
        violations=tuple(v.render() for v in violations),
        pruned=tuple(pruned),
    )
    return build.as_dict()


def reachable_overlay_libs(start_rels, needed_fn, soname_path: dict, base_sonames: set, elf_rels: set):
    """BFS the DT_NEEDED graph from ``start_rels`` (leaf ELF rel-paths).

    ``needed_fn(rel) -> iterable[soname]`` gives a file's DT_NEEDED. A need the BASE
    already provides stops the walk (the base ships it + its own transitive deps). A
    need provided by the overlay (``soname_path``) is kept and recursed into if it is
    itself an ELF (``elf_rels``). Returns (keep_rels:set, missing_sonames:set). Pure /
    offline-testable — no filesystem or ELF parsing of its own.
    """
    from collections import deque

    keep: set[str] = set()
    missing: set[str] = set()
    queue = deque(start_rels)
    seen = set(queue)
    while queue:
        rel = queue.popleft()
        for need in needed_fn(rel):
            if need in base_sonames:
                continue
            tgt = soname_path.get(need)
            if tgt is None:
                missing.add(need)
                continue
            keep.add(tgt)
            if tgt in elf_rels and tgt not in seen:
                seen.add(tgt)
                queue.append(tgt)
    return keep, missing


def _leaf_file_paths(leaf_pkgs, index, mirror, cache: Path, opener) -> set[str]:
    """Extract each leaf .deb on its own and return the set of rootfs-rel paths it
    installs (binaries + plugins + data) — these are kept ENTIRELY in a minimal build."""
    import shutil

    paths: set[str] = set()
    for name in leaf_pkgs:
        fields = index.get(name, {})
        filename = fields.get("Filename")
        if not filename:
            continue
        try:
            deb = _download_deb(mirror, filename, cache, opener=opener)
        except Exception:
            continue
        leaf_root = cache / f"_leaf_{name.replace('/', '_').replace(':', '_')}"
        if leaf_root.exists():
            shutil.rmtree(leaf_root)
        leaf_root.mkdir(parents=True)
        try:
            extract_deb(deb, leaf_root)
        except Exception:
            continue
        for dirpath, _dirs, files in os.walk(leaf_root):
            for fname in files:
                paths.add((Path(dirpath) / fname).relative_to(leaf_root).as_posix())
    return paths


def build_minimal_overlay(
    leaf_pkgs: list[str],
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = "http://deb.debian.org/debian",
    suite: str = "bookworm",
    arch: str = "arm64",
    components=("main",),
    cache_dir: str | Path | None = None,
    prune=DEFAULT_PRUNE_PREFIXES,
    keep_prefixes=(),
    opener=_urlopen_ua,
) -> dict:
    """DT_NEEDED-MINIMAL overlay: keep the leaf package's own files + ONLY the shared
    libs reachable from them via DT_NEEDED (transitively) that the base does NOT
    provide. Drops the rest of the conservative Debian Depends closure (perl pulled
    via ca-certificates, unused ICU, full icon themes, …) that bloats a full build.

    GTK/Qt runtime modules that are dlopen'd (not DT_NEEDED) live either in the base
    (gdk-pixbuf loaders, pango modules) or in the leaf package itself (Qt's qtwayland
    platform plugin) — the leaf's own files are kept entirely, so those survive. Use
    ``keep_prefixes`` to force-keep extra data dirs if a toolkit needs them.
    """
    from tools.elf_needed import read_elf_dynamic  # lazy: parser may post-date this import

    cache = Path(cache_dir) if cache_dir is not None else Path(tempfile.mkdtemp(prefix="deb-closure-"))
    cache.mkdir(parents=True, exist_ok=True)
    index = parse_packages(fetch_packages_index(mirror, suite, arch, components=components, opener=opener))
    provides_map = build_provides_map(index)
    log: list[str] = []
    closure = resolve_closure(list(leaf_pkgs), index, provides_map=provides_map, log=log)

    import shutil

    unsupported: list[str] = []
    merged_root = cache / "_merged_root"
    if merged_root.exists():
        shutil.rmtree(merged_root)
    merged_root.mkdir(parents=True)
    for name in closure:
        fields = index.get(name, {})
        filename = fields.get("Filename")
        if not filename:
            unsupported.append(f"{name} (no Filename)")
            continue
        try:
            deb = _download_deb(mirror, filename, cache, opener=opener)
        except Exception as exc:
            unsupported.append(f"{name} (download failed: {exc})")
            continue
        try:
            extract_deb(deb, merged_root)
        except NotImplementedError:
            unsupported.append(f"{name} (zstd .deb)")
        except Exception as exc:
            unsupported.append(f"{name} (extract failed: {exc})")

    leaf_files = _leaf_file_paths(leaf_pkgs, index, mirror, cache, opener)

    # Map each SONAME provided by the overlay to its REAL file (skip symlinks; the
    # flat name is synthesized by build_stage_tar). Record which rel paths are ELF.
    soname_path: dict[str, str] = {}
    elf_rel: set[str] = set()
    for dirpath, _dirs, files in os.walk(merged_root):
        for fname in files:
            full = Path(dirpath) / fname
            if full.is_symlink():
                continue
            rel = full.relative_to(merged_root).as_posix()
            lib = parse_solib(fname)
            if lib is not None:
                soname_path.setdefault(lib.soname, rel)
            try:
                ed = read_elf_dynamic(full)
            except OSError:
                continue
            if ed.is_elf:
                elf_rel.add(rel)
                if ed.soname:
                    soname_path.setdefault(ed.soname, rel)

    base_sonames = base_soname_set(base)

    def _needed(rel: str):
        try:
            return read_elf_dynamic(merged_root / rel).needed
        except OSError:
            return ()

    keep, missing_soname = reachable_overlay_libs(
        [rel for rel in leaf_files if rel in elf_rel],
        _needed,
        soname_path,
        base_sonames,
        elf_rel,
    )

    keep_set = set(leaf_files) | keep
    kp = tuple(p.rstrip("/") + "/" for p in keep_prefixes)
    dropped: list[str] = []
    for dirpath, _dirs, files in os.walk(merged_root):
        for fname in files:
            full = Path(dirpath) / fname
            rel = full.relative_to(merged_root).as_posix()
            if rel in keep_set or (kp and rel.startswith(kp)):
                continue
            try:
                full.unlink()
            except OSError:
                continue
            dropped.append(rel)

    skipped_sonames = drop_base_sonames(merged_root, base_sonames)
    skipped_paths = drop_base_paths(merged_root, base_path_set(base))
    skipped_base = sorted(set(skipped_sonames) | set(skipped_paths))
    pruned = prune_paths(merged_root, prune)

    result = build_stage_tar(merged_root, out_tar)
    violations = scan_overlay_violations(base, out_tar)

    return {
        "closure": list(closure),
        "leaf_files": len(leaf_files),
        "reachable_libs": sorted(keep),
        "missing_soname": sorted(missing_soname),
        "dropped_unreachable": len(dropped),
        "skipped_base": len(skipped_base),
        "pruned": len(pruned),
        "unsupported": unsupported,
        "out_tar": result.out_tar,
        "sidecar": result.sidecar,
        "file_count": result.file_count,
        "violations": [v.render() for v in violations],
    }


# --------------------------------------------------------------------------- #
# CLI + selftest
# --------------------------------------------------------------------------- #

_FIXTURE_PACKAGES = """\
Package: app
Version: 1.0
Depends: liba, libvirt-x, missing-dep
Filename: pool/main/a/app/app_1.0_arm64.deb

Package: liba
Version: 2.0
Depends: libc6 (>= 2.34)
Filename: pool/main/a/liba/liba_2.0_arm64.deb

Package: libc6
Version: 2.36
Filename: pool/main/g/glibc/libc6_2.36_arm64.deb

Package: libreal
Version: 3.1
Provides: libvirt-x (= 3.1), other-virtual
Filename: pool/main/l/libreal/libreal_3.1_arm64.deb
"""


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        status = "PASS" if cond else "FAIL"
        if not cond:
            failures += 1
        print(f"  [{status}] {label}")

    # --- parse_depends -----------------------------------------------------
    pd = parse_depends("libc6 (>= 2.34), libfoo | libbar, libbaz:any (>= 1.0)")
    check(
        "parse_depends strips versions/arch and splits alternatives",
        pd == [["libc6"], ["libfoo", "libbar"], ["libbaz"]],
    )
    check("parse_depends empty field -> []", parse_depends("") == [])
    check(
        "parse_depends ignores empty alternatives",
        parse_depends("liba, , libb") == [["liba"], ["libb"]],
    )

    # --- parse_packages ----------------------------------------------------
    index = parse_packages(_FIXTURE_PACKAGES)
    check("parse_packages found all 4 stanzas", set(index) == {"app", "liba", "libc6", "libreal"})
    check("parse_packages keeps Filename", index["app"]["Filename"].endswith("app_1.0_arm64.deb"))
    check("parse_packages keeps Depends", index["liba"]["Depends"] == "libc6 (>= 2.34)")
    check("parse_packages keeps Provides", index["libreal"]["Provides"].startswith("libvirt-x"))
    check("parse_packages keeps Version", index["libc6"]["Version"] == "2.36")

    # continuation-line folding
    folded = parse_packages("Package: x\nDescription: line one\n more text\nVersion: 9\n")
    check(
        "parse_packages folds continuation lines",
        folded["x"]["Description"] == "line one\nmore text" and folded["x"]["Version"] == "9",
    )

    # --- build_provides_map ------------------------------------------------
    pm = build_provides_map(index)
    check("provides_map: libvirt-x -> libreal", pm.get("libvirt-x") == ["libreal"])
    check("provides_map: other-virtual -> libreal", pm.get("other-virtual") == ["libreal"])

    # --- resolve_closure ---------------------------------------------------
    log: list[str] = []
    closure = resolve_closure(["app"], index, provides_map=pm, log=log)
    check("closure includes app", "app" in closure)
    check("closure includes liba (direct dep)", "liba" in closure)
    check("closure includes libc6 (transitive)", "libc6" in closure)
    check("closure includes libreal (via Provides libvirt-x)", "libreal" in closure)
    check("closure is order-stable (BFS)", closure == ["app", "liba", "libreal", "libc6"])
    check(
        "closure skipped the missing dep without crashing",
        any("missing-dep" in line for line in log),
    )
    check("closure has no duplicates", len(closure) == len(set(closure)))

    # determinism: same inputs -> same order
    closure2 = resolve_closure(["app"], index, provides_map=pm)
    check("closure deterministic across calls", closure == closure2)

    # missing target is non-fatal
    log2: list[str] = []
    closure3 = resolve_closure(["nope"], index, log=log2)
    check("missing target -> empty closure, logged", closure3 == [] and any("nope" in l for l in log2))

    # --- base subtraction (pure logic) -------------------------------------
    # synthetic: drop_base_sonames operates on a real tree, so build a tiny one.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "merged"
        libdir = root / "usr/lib/aarch64-linux-gnu"
        libdir.mkdir(parents=True)
        (root / "usr/bin").mkdir(parents=True)

        # base owns libharfbuzz.so.0 (flat) -> drop overlay's harfbuzz files
        (libdir / "libharfbuzz.so.0.60000.0").write_bytes(b"HB-6")     # real versioned
        os.symlink("libharfbuzz.so.0.60000.0", libdir / "libharfbuzz.so.0")  # SONAME link
        os.symlink("libharfbuzz.so.0", libdir / "libharfbuzz.so")            # dev link
        # base does NOT own libwebp.so.7 -> keep
        (libdir / "libwebp.so.7.1.0").write_bytes(b"WEBP")
        os.symlink("libwebp.so.7.1.0", libdir / "libwebp.so.7")
        # non-library payload -> always keep
        (root / "usr/bin/app").write_bytes(b"\x7fELF")

        base_sonames = {"libharfbuzz.so.0"}
        removed = drop_base_sonames(root, base_sonames)

        removed_set = set(removed)
        check(
            "drop_base_sonames removed the base-owned real harfbuzz file",
            "usr/lib/aarch64-linux-gnu/libharfbuzz.so.0.60000.0" in removed_set,
        )
        check(
            "drop_base_sonames removed the harfbuzz SONAME symlink",
            "usr/lib/aarch64-linux-gnu/libharfbuzz.so.0" in removed_set,
        )
        check(
            "drop_base_sonames removed the harfbuzz dev symlink",
            "usr/lib/aarch64-linux-gnu/libharfbuzz.so" in removed_set,
        )
        check(
            "drop_base_sonames KEPT non-base libwebp real file",
            (libdir / "libwebp.so.7.1.0").exists(),
        )
        check(
            "drop_base_sonames KEPT non-base libwebp soname symlink",
            (libdir / "libwebp.so.7").is_symlink(),
        )
        check(
            "drop_base_sonames KEPT non-library payload (usr/bin/app)",
            (root / "usr/bin/app").exists(),
        )
        check(
            "drop_base_sonames removed exactly the 3 harfbuzz entries",
            len(removed) == 3,
        )
        check("drop_base_sonames return is sorted/deterministic", removed == sorted(removed))

        # prune_paths: drop runtime-irrelevant payload (merged-usr aliased), keep the rest.
        (root / "usr/share/man/man1").mkdir(parents=True)
        (root / "usr/share/man/man1/foo.1").write_bytes(b"man")
        (root / "usr/lib/systemd/system").mkdir(parents=True)
        (root / "usr/lib/systemd/system/x.service").write_bytes(b"unit")
        (root / "lib/systemd").mkdir(parents=True)  # merged-usr spelling
        (root / "lib/systemd/y").write_bytes(b"u")
        pruned = prune_paths(root, DEFAULT_PRUNE_PREFIXES)
        check("prune dropped man page", "usr/share/man/man1/foo.1" in pruned)
        check("prune dropped usr/lib/systemd unit", "usr/lib/systemd/system/x.service" in pruned)
        check("prune dropped merged-usr lib/systemd", "lib/systemd/y" in pruned)
        check("prune KEPT the app binary", (root / "usr/bin/app").exists())
        check("prune KEPT libwebp", (libdir / "libwebp.so.7.1.0").exists())

    # --- reachable_overlay_libs (DT_NEEDED minimal closure, pure/offline) ----
    # leaf links libA (overlay) + libgtk (base, stops); libA links libB (overlay);
    # libC is an unrelated overlay lib (unreachable) -> must be dropped.
    soname_path = {"libA.so.1": "lib/libA", "libB.so.1": "lib/libB", "libC.so.1": "lib/libC"}
    needed = {
        "bin/leaf": ["libA.so.1", "libgtk-3.so.0"],
        "lib/libA": ["libB.so.1"],
        "lib/libB": [],
        "lib/libC": ["libB.so.1"],
    }
    keep, missing = reachable_overlay_libs(
        ["bin/leaf"], lambda r: needed.get(r, []), soname_path,
        {"libgtk-3.so.0"}, set(needed),
    )
    check("reachable keeps DT_NEEDED-linked overlay libs (A,B)", keep == {"lib/libA", "lib/libB"})
    check("reachable DROPS unreachable overlay lib (C)", "lib/libC" not in keep)
    check("reachable: base-provided need does not appear / no missing", missing == set())
    k2, m2 = reachable_overlay_libs(["bin/leaf"], lambda r: ["libZ.so.9"] if r == "bin/leaf" else [],
                                    soname_path, set(), {"bin/leaf"})
    check("reachable reports unresolved soname as missing", m2 == {"libZ.so.9"} and k2 == set())

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="deb_closure",
        description="Build a §5-E overlay tar from a Debian package dependency closure.",
    )
    parser.add_argument(
        "--package",
        action="append",
        dest="packages",
        help="target package name (repeatable)",
    )
    parser.add_argument("--base", help="base rootfs directory or tar (for soname subtraction)")
    parser.add_argument("--out", help="output overlay stage tar path")
    parser.add_argument(
        "--mirror",
        default="http://deb.debian.org/debian",
        help="Debian mirror base URL (default: %(default)s)",
    )
    parser.add_argument("--suite", default="bookworm", help="suite (default: %(default)s)")
    parser.add_argument("--arch", default="arm64", help="architecture (default: %(default)s)")
    parser.add_argument(
        "--component", action="append", dest="components",
        help="repository component (repeatable; default main). The ALR base is "
        "Ubuntu noble → use --mirror http://ports.ubuntu.com/ubuntu-ports "
        "--suite noble --component main --component universe",
    )
    parser.add_argument("--cache", help="package/index cache directory")
    parser.add_argument(
        "--minimal",
        action="store_true",
        help="DT_NEEDED-minimal: keep only the libs the leaf package actually links "
        "(transitively) that the base lacks — drops perl/ICU/icon-theme bloat",
    )
    parser.add_argument("--selftest", action="store_true", help="run built-in OFFLINE tests")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.packages or not args.base or not args.out:
        parser.error("--package, --base and --out are required (or use --selftest)")

    components = tuple(args.components) if args.components else ("main",)

    if args.minimal:
        m = build_minimal_overlay(
            args.packages, args.base, args.out,
            mirror=args.mirror, suite=args.suite, arch=args.arch,
            components=components, cache_dir=args.cache,
        )
        print(f"wrote {m['out_tar']} (DT_NEEDED-minimal)")
        print(f"  files:            {m['file_count']}")
        print(f"  leaf files:       {m['leaf_files']}")
        print(f"  reachable libs:   {len(m['reachable_libs'])}")
        print(f"  dropped unreached:{m['dropped_unreachable']}")
        print(f"  pruned:           {m['pruned']}")
        if m["missing_soname"]:
            print(f"  MISSING sonames:  {m['missing_soname']}")
        if m["unsupported"]:
            print(f"  unsupported pkgs: {len(m['unsupported'])}")
        print("  overlay_guard: " + ("OK" if not m["violations"] else f"{len(m['violations'])} violations"))
        return 0 if not m["violations"] else 1

    result = build_overlay(
        args.packages,
        args.base,
        args.out,
        mirror=args.mirror,
        suite=args.suite,
        arch=args.arch,
        components=components,
        cache_dir=args.cache,
    )

    print(f"wrote {result['out_tar']}")
    print(f"  sidecar:        {result['sidecar']}")
    print(f"  files:          {result['file_count']}")
    print(f"  closure:        {len(result['closure'])} package(s)")
    for name in result["closure"]:
        print(f"    - {name}")
    if result["skipped_base"]:
        print(f"  dropped (base owns soname): {len(result['skipped_base'])}")
        for rel in result["skipped_base"]:
            print(f"    - {rel}")
    if result["unsupported"]:
        print(f"  unsupported/skipped packages: {len(result['unsupported'])}")
        for note in result["unsupported"]:
            print(f"    - {note}")
    if result["missing"]:
        print(f"  unsatisfied/missing deps: {len(result['missing'])}")
        for note in result["missing"]:
            print(f"    - {note}")
    if result["violations"]:
        print(f"  overlay_guard violations: {len(result['violations'])}")
        for v in result["violations"]:
            print(f"    {v}")
    else:
        print("  overlay_guard: OK (no base-lib downgrade)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
