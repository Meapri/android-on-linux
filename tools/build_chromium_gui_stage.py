#!/usr/bin/env python3
"""Build the FULL-BROWSER chromium GUI stage-tar (chromium-gui-stage.tar).

Why a GUI build (not the headless shell)
----------------------------------------
The prior chromium staging shipped only ``chromium-headless-shell`` (the
``--headless`` render engine), which has **no window backend** — it can
``--dump-dom`` / screenshot off-screen but never opens an on-screen window. A
*demo* of "a Linux browser running on Android" needs the **full ``chromium``
browser binary** (``/usr/lib/chromium/chromium``), which links the Ozone platform
layer (X11 **and Wayland**) and can open a real top-level window via
``--ozone-platform=wayland`` against the ALR compositor.

Where the binary comes from (and why Debian, not Ubuntu)
-------------------------------------------------------
The ALR base rootfs is Ubuntu noble (glibc 2.39), but **Ubuntu ships chromium as
a snap, not a .deb** — ``apt`` on noble cannot fetch a real ELF. **Debian** does
ship a chromium .deb. We use Debian **bookworm** chromium 147 (glibc 2.36); a
2.36-linked binary runs against the noble 2.39 runtime (glibc is
backward-compatible: newer libc, older binary = fine). The full ``chromium`` and
``chromium-headless-shell`` binaries live in the **same** ``chromium`` .deb under
``/usr/lib/chromium/`` and share all bundled data (``icudtl.dat``,
``resources.pak``, the V8 snapshots, ``libGLESv2``/``libEGL``/swiftshader); the
``chromium-common`` .deb adds the shared resources/policy.

What this builder produces
--------------------------
A §5-E ``./``-rooted overlay tar (the unit ``RootfsInstaller.extractOverlayTar``
lays over the base) carrying, with **zero missing .so** in the runtime closure:

  1. The full ``chromium`` browser binary + ALL its ``/usr/lib/chromium/`` data
     (the leaf .deb's own files are kept entirely). The headless-shell binary is
     **dropped** (we only need the GUI browser; saves ~186 MB).
  2. Every shared library the ``chromium`` binary pulls via **DT_NEEDED**
     (transitively) that the **base rootfs does not already provide** — computed
     by ``deb_closure.build_minimal_overlay`` (flat-SONAME, base-subtracted, no
     base-library downgrade). The base already ships 36 of chromium's 47 direct
     DT_NEEDED libs (libc, glib, cairo, pango, X11, NSS core, …); this overlay
     adds the remaining ~11 + their transitive deps (libFLAC/libdav1d/libopenh264/
     libpulse/…).
  3. **libpulsecommon-<v>.so flattened.** libpulse's RUNPATH is the *absolute*
     ``/usr/lib/aarch64-linux-gnu/pulseaudio`` dir (its libpulsecommon is
     DT_NEEDED but only resolvable there). The ALR loader resolves DT_NEEDED via
     ``LD_LIBRARY_PATH`` = the flat ``usr/lib/aarch64-linux-gnu/`` dir and does
     not honor that absolute RUNPATH, so we additionally place libpulsecommon at
     the flat path. (The only non-flat .so in the closure — same fix the first
     device run needed.)
  4. **NSS dlopen plugin modules.** chromium initializes NSS for its cert store
     during TLS; NSS ``dlopen()``s libsoftokn3 / libfreebl3 / libfreeblpriv3 /
     libnssckbi / libnssdbm3 at runtime. Those are NOT DT_NEEDED, so the
     DT_NEEDED-only closure drops them and chromium FATALs ``nss_error=-5925``
     ("Error initializing NSS … libsoftokn3.so: cannot open shared object file").
     We pack them at BOTH the flat path and ``…/nss/`` (the NSS search path) — the
     same set the standalone ``build_nss_overlay`` ships, folded in here so this
     single tar is self-contained for an on-screen demo.

Honest scope
------------
HOST-ONLY. This builds + validates the tar host-side (closure resolution, §5-E
conformance, entrypoint-present, ozone-wayland token check). It does NOT run
chromium on a device — the on-screen ``--ozone-platform=wayland`` window is the
device/compositor gate. "0 missing .so" is over the binary's **DT_NEEDED** graph
plus the two dlopen sets we add by hand (pulse, NSS); a real render may still want
runtime config (fonts, ``/dev/dri``, the net overlay for DNS/TLS) supplied by the
other overlays.

Reuse, not duplication
----------------------
  * tools.deb_closure.build_minimal_overlay — the DT_NEEDED-minimal §5-E engine
  * tools.build_stage_tar (extract_deb)      — .deb crack/extract for the addons
  * tools.stage_tar_spec.validate_stage_tar  — §5-E structural conformance
  * tools.overlay_guard (via the two above)  — base-downgrade gate
"""

from __future__ import annotations

import argparse
import io
import json
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.deb_closure import (
    build_minimal_overlay,
    fetch_packages_index,
    parse_packages,
    _download_deb,
)
from tools.build_stage_tar import extract_deb
from tools.overlay_guard import parse_solib

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

# Debian (chromium is a real .deb here; Ubuntu ships it as a snap).
MIRROR = "http://deb.debian.org/debian"
SUITE = "bookworm"
ARCH = "arm64"
COMPONENTS = ("main",)

# Leaf packages: the full browser binary + its shared resources/policy.
PACKAGES = ("chromium", "chromium-common")

# Rootfs-absolute entrypoint the device loader maps (the ELF, not the /usr/bin
# shell wrapper). build_minimal_overlay keeps the leaf .deb's own files, so this
# binary is carried verbatim.
ENTRYPOINT = "/usr/lib/chromium/chromium"

# The headless-shell binary in the SAME .deb — we only ship the GUI browser, so
# drop it from the leaf's kept files (saves ~186 MB; it is never the entrypoint).
DROP_LEAF = ("usr/lib/chromium/chromium-headless-shell",)

ARCH_TRIPLET = "aarch64-linux-gnu"
LIBDIR = f"usr/lib/{ARCH_TRIPLET}"

# NSS plugin modules chromium dlopen()s at TLS init (NOT DT_NEEDED → the closure
# drops them; chromium FATALs nss_error=-5925 without them). Fetched from the
# bookworm libnss3 .deb and placed flat + under nss/. .chk integrity sidecars are
# carried alongside each .so when the .deb ships them.
NSS_PACKAGE = "libnss3"
NSS_MODULES = (
    "libsoftokn3.so",
    "libfreebl3.so",
    "libfreeblpriv3.so",
    "libnssckbi.so",
    "libnssdbm3.so",
)

# Ozone tokens that PROVE the binary is the full GUI browser (Wayland backend
# compiled in), not the headless shell.
OZONE_WAYLAND_TOKENS = (b"ozone_platform_wayland.cc", b"ozone-platform")

DEFAULT_OUT = "out/v2-stage/chromium-gui-stage.tar"
DEFAULT_CACHE = "/tmp/deb-cache-bookworm"


# --------------------------------------------------------------------------- #
# Result
# --------------------------------------------------------------------------- #
@dataclass
class GuiStageBuild:
    out_tar: str
    file_count: int
    tar_bytes: int
    extract_bytes: int
    entrypoint: str
    entrypoint_present: bool
    closure_pkgs: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    unsatisfied_needed: tuple[str, ...]
    pulsecommon_flattened: str | None
    nss_modules_added: tuple[str, ...]
    compat_aliases: tuple[str, ...]
    ozone_wayland_ok: bool
    conformant: bool
    violations: tuple[str, ...]
    unsupported: tuple[str, ...] = ()

    @property
    def ok(self) -> bool:
        """Demo-ready iff: the GUI browser entrypoint is present, EVERY DT_NEEDED
        of the chromium binary is satisfied by base ∪ this overlay (0 unsatisfied),
        NSS init won't FATAL (softokn+freebl present), libpulsecommon is flat, the
        Wayland Ozone backend is compiled in, §5-E conformant, no base downgrade."""
        nss_hard = {"libsoftokn3.so", "libfreebl3.so"}
        return (
            self.entrypoint_present
            and not self.unsatisfied_needed
            and nss_hard.issubset(set(self.nss_modules_added))
            and self.pulsecommon_flattened is not None
            and self.ozone_wayland_ok
            and self.conformant
            and not self.violations
        )

    def as_dict(self) -> dict:
        d = dict(self.__dict__)
        for k in ("reachable_libs", "missing_soname", "unsatisfied_needed",
                  "nss_modules_added", "compat_aliases", "violations", "unsupported"):
            d[k] = list(getattr(self, k))
        d["tar_mib"] = round(self.tar_bytes / (1024 * 1024), 1)
        d["extract_mib"] = round(self.extract_bytes / (1024 * 1024), 1)
        d["ok"] = self.ok
        return d


# --------------------------------------------------------------------------- #
# Tar helpers
# --------------------------------------------------------------------------- #
def _member_names(tar_path: Path) -> set[str]:
    with tarfile.open(tar_path, "r:*") as t:
        return set(t.getnames())


def _has_member(tar_path: Path, rootfs_path: str) -> bool:
    return ("./" + rootfs_path.lstrip("/")) in _member_names(tar_path)


def _extract_bytes(tar_path: Path) -> int:
    total = 0
    with tarfile.open(tar_path, "r:*") as t:
        for m in t.getmembers():
            if m.isfile():
                total += m.size
    return total


def _read_member(tar_path: Path, rootfs_path: str) -> bytes | None:
    want = "./" + rootfs_path.lstrip("/")
    with tarfile.open(tar_path, "r:*") as t:
        try:
            m = t.getmember(want)
        except KeyError:
            return None
        if not m.isfile():
            return None
        f = t.extractfile(m)
        return f.read() if f else None


def _add_bytes(tar: tarfile.TarFile, rootfs_path: str, data: bytes, mode: int = 0o644) -> None:
    ti = tarfile.TarInfo("./" + rootfs_path.lstrip("/"))
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 0
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


# --------------------------------------------------------------------------- #
# Ozone / Wayland proof
# --------------------------------------------------------------------------- #
def binary_has_ozone_wayland(elf_bytes: bytes) -> bool:
    """True iff the binary contains the Wayland Ozone backend tokens (i.e. it is
    the full GUI browser with ``--ozone-platform=wayland`` support compiled in)."""
    return all(tok in elf_bytes for tok in OZONE_WAYLAND_TOKENS)


# --------------------------------------------------------------------------- #
# DT_NEEDED SONAME-major compat aliases
# --------------------------------------------------------------------------- #
def compat_alias_plan(
    needed: list[str],
    base_lib_names: set[str],
    overlay_real: dict[str, str],
) -> dict[str, str]:
    """Plan flat compat-alias copies for DT_NEEDED entries no provider matches by
    exact name, but the overlay ships under a *different major* of the same lib.

    The motivating case: chromium DT_NEEDED ``libopenh264.so.7`` while Debian's
    libopenh264 .deb is SONAME ``libopenh264.so.2`` and only carries a
    ``libopenh264.so.7 -> libopenh264.so.2.3.1`` compat symlink — which the
    flat-SONAME flattener drops (different soname group). The ALR loader resolves
    DT_NEEDED by exact filename on the flat LD_LIBRARY_PATH, so it needs a real
    ``libopenh264.so.7`` there. We satisfy it by copying the overlay's flat real
    file of the *same stem* (``libopenh264`` → ``libopenh264.so.2``) to the needed
    name.

    Parameters
    ----------
    needed:           the binary's DT_NEEDED soname list.
    base_lib_names:   library basenames the base rootfs already provides (exact).
    overlay_real:     ``{basename: rootfs_rel_path}`` of the overlay's real lib
                      files (flat, post-flatten).

    Returns ``{needed_soname: source_rootfs_rel_path}`` — copy source → flat dest
    ``{LIBDIR}/<needed_soname>`` for each plannable alias. A need already satisfied
    (exact name in base or overlay) yields no entry; a need with no same-stem
    overlay lib also yields no entry (it is a genuine miss the caller reports).
    """
    def stem_of(name: str) -> str | None:
        lib = parse_solib(name)
        return lib.stem if lib is not None else None

    # index overlay real libs by stem -> [(version_tuple, basename, rel)]
    by_stem: dict[str, list[tuple[tuple, str, str]]] = {}
    for bn, rel in overlay_real.items():
        lib = parse_solib(bn)
        if lib is None:
            continue
        by_stem.setdefault(lib.stem, []).append((lib.version, bn, rel))

    plan: dict[str, str] = {}
    for need in needed:
        if need in base_lib_names or need in overlay_real:
            continue  # already satisfied by exact name
        stem = stem_of(need)
        if stem is None:
            continue
        candidates = by_stem.get(stem)
        if not candidates:
            continue  # no same-stem overlay lib → genuine miss (not aliasable)
        # pick the highest-version same-stem real lib as the alias source
        candidates.sort(key=lambda t: t[0])
        plan[need] = candidates[-1][2]
    return plan


# --------------------------------------------------------------------------- #
# Addon staging (libpulsecommon flat + NSS dlopen plugins)
# --------------------------------------------------------------------------- #
def _fetch_nss_modules(cache: Path) -> dict[str, bytes]:
    """Fetch the bookworm libnss3 .deb and return {filename: bytes} for each NSS
    dlopen plugin module (+ its .chk sidecar when present)."""
    idx = parse_packages(fetch_packages_index(MIRROR, SUITE, ARCH, components=COMPONENTS))
    fields = idx.get(NSS_PACKAGE)
    if not fields or not fields.get("Filename"):
        raise SystemExit(f"{NSS_PACKAGE} not found in {SUITE} index")
    deb = _download_deb(MIRROR, fields["Filename"], cache)
    out: dict[str, bytes] = {}
    with tempfile.TemporaryDirectory() as td:
        root = extract_deb(deb, Path(td))
        for mod in NSS_MODULES:
            for p in Path(root).rglob(mod):
                if p.is_file():
                    out[mod] = p.read_bytes()
                    chk = p.with_suffix(".chk")
                    if chk.is_file():
                        out[chk.name] = chk.read_bytes()
                    break
    return out


def _repack_with_addons(
    base_tar: Path,
    out_tar: Path,
    nss_blobs: dict[str, bytes],
    alias_plan: dict[str, str] | None = None,
) -> tuple[str | None, tuple[str, ...], tuple[str, ...]]:
    """Copy ``base_tar`` to ``out_tar`` adding the flat libpulsecommon, the NSS
    dlopen plugins, and any DT_NEEDED-major compat aliases.
    Returns (pulsecommon_flat_path|None, nss_modules_added, compat_aliases_added).

    libpulsecommon: find the ``…/pulseaudio/libpulsecommon-*.so`` member already in
    the closure and add a copy at the flat ``{LIBDIR}/`` path (DT_NEEDED-resolvable
    via LD_LIBRARY_PATH). NSS: add each module flat + under ``{LIBDIR}/nss/`` (+ any
    .chk), skipping ones already present. ``alias_plan`` maps a needed soname to the
    overlay-relative real source to copy to ``{LIBDIR}/<needed>`` (e.g.
    libopenh264.so.7 ← libopenh264.so.2).
    """
    alias_plan = alias_plan or {}
    existing = _member_names(base_tar)
    pulse_flat: str | None = None
    nss_added: list[str] = []
    aliases_added: list[str] = []

    with tarfile.open(base_tar, "r:*") as src, tarfile.open(out_tar, "w") as dst:
        # 1) copy every member through verbatim
        for m in src.getmembers():
            if m.isfile():
                f = src.extractfile(m)
                dst.addfile(m, f)
            else:
                dst.addfile(m)

        # 2) libpulsecommon flat copy (read its bytes from the just-copied source)
        pulse_member = next(
            (n for n in existing
             if "/pulseaudio/libpulsecommon-" in n and n.endswith(".so")),
            None,
        )
        if pulse_member is not None:
            flat_rel = f"{LIBDIR}/{Path(pulse_member).name}"
            flat_arc = "./" + flat_rel
            if flat_arc not in existing:
                with tarfile.open(base_tar, "r:*") as s2:
                    blob = s2.extractfile(s2.getmember(pulse_member)).read()
                _add_bytes(dst, flat_rel, blob, mode=0o644)
                pulse_flat = flat_rel

        # 3) NSS dlopen plugins: flat + nss/ (skip if already in the closure)
        for fname, blob in sorted(nss_blobs.items()):
            wrote_any = False
            for rel in (f"{LIBDIR}/{fname}", f"{LIBDIR}/nss/{fname}"):
                if ("./" + rel) in existing:
                    continue
                _add_bytes(dst, rel, blob, mode=0o644 if fname.endswith(".chk") else 0o755)
                wrote_any = True
            if wrote_any and fname.endswith(".so"):
                nss_added.append(fname)

        # 4) DT_NEEDED-major compat aliases (copy overlay real lib → needed name)
        for need, src_rel in sorted(alias_plan.items()):
            dest_rel = f"{LIBDIR}/{need}"
            if ("./" + dest_rel) in existing:
                continue
            src_arc = "./" + src_rel.lstrip("/")
            if src_arc not in existing:
                continue  # source not in the overlay — cannot alias
            with tarfile.open(base_tar, "r:*") as s3:
                blob = s3.extractfile(s3.getmember(src_arc)).read()
            _add_bytes(dst, dest_rel, blob, mode=0o755)
            aliases_added.append(f"{need} <- {Path(src_rel).name}")

    return pulse_flat, tuple(sorted(nss_added)), tuple(sorted(aliases_added))


# --------------------------------------------------------------------------- #
# DT_NEEDED satisfaction over base ∪ overlay
# --------------------------------------------------------------------------- #
def _lib_basenames(tar_path: str | Path) -> set[str]:
    """Every library basename a tar provides as a real file (for exact-name match)."""
    out: set[str] = set()
    with tarfile.open(tar_path, "r:*") as t:
        for m in t.getmembers():
            if m.isfile():
                out.add(Path(m.name).name)
    return out


def _overlay_real_libs(tar_path: str | Path) -> dict[str, str]:
    """``{basename: rootfs_rel_path}`` for real lib files in the overlay (flat dir),
    used as compat-alias sources. Keeps only ``{LIBDIR}/<lib>`` (no nss/ subdir)."""
    out: dict[str, str] = {}
    with tarfile.open(tar_path, "r:*") as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            rel = m.name.lstrip("./")
            if rel.startswith(LIBDIR + "/") and "/" not in rel[len(LIBDIR) + 1:]:
                out[Path(rel).name] = rel
    return out


def _chromium_needed(tar_path: str | Path) -> list[str]:
    """The DT_NEEDED list of the chromium entrypoint binary inside the overlay."""
    from tools.elf_needed import read_elf_dynamic

    with tempfile.TemporaryDirectory() as td:
        want = "./" + ENTRYPOINT.lstrip("/")
        with tarfile.open(tar_path, "r:*") as t:
            try:
                m = t.getmember(want)
            except KeyError:
                return []
            t.extract(m, td, set_attrs=False)
        return list(read_elf_dynamic(Path(td) / ENTRYPOINT.lstrip("/")).needed)


def _plan_aliases_for(overlay_tar: str | Path, base: str | Path) -> dict[str, str]:
    """Compute the compat-alias plan for the chromium binary against base ∪ overlay."""
    needed = _chromium_needed(overlay_tar)
    if not needed:
        return {}
    base_names = _lib_basenames(base)
    overlay_real = _overlay_real_libs(overlay_tar)
    return compat_alias_plan(needed, base_names, overlay_real)


def _unsatisfied_needed(overlay_tar: str | Path, base: str | Path) -> tuple[str, ...]:
    """DT_NEEDED entries of the chromium binary NOT provided (by exact name) by
    base ∪ the produced overlay. The honest 'missing .so' gate. ``ld-linux`` is the
    dynamic loader (always provided by the runtime), excluded."""
    needed = _chromium_needed(overlay_tar)
    if not needed:
        return ()
    have = _lib_basenames(base) | _lib_basenames(overlay_tar)
    miss = [n for n in needed if n not in have and not n.startswith("ld-linux")]
    return tuple(sorted(miss))


# --------------------------------------------------------------------------- #
# Build
# --------------------------------------------------------------------------- #
def build_chromium_gui_stage(
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path = DEFAULT_CACHE,
    add_nss: bool = True,
) -> GuiStageBuild:
    """Build the full-browser chromium GUI stage-tar (NETWORK).

    1. ``deb_closure.build_minimal_overlay`` packs the leaf chromium files (full
       browser binary + data, headless-shell dropped) + the DT_NEEDED libs the
       base lacks, base-subtracted and §5-E flat.
    2. repack to add the flat libpulsecommon and (``add_nss``) the NSS dlopen
       plugin modules.
    3. validate: entrypoint present, §5-E conformant, no base downgrade, Wayland
       Ozone token present.
    """
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    cache = Path(cache_dir)
    cache.mkdir(parents=True, exist_ok=True)

    def _exclude_leaf(rel: str) -> bool:
        return rel in DROP_LEAF

    with tempfile.TemporaryDirectory() as td:
        raw_tar = Path(td) / "chromium-closure.tar"
        m = build_minimal_overlay(
            list(PACKAGES),
            base,
            raw_tar,
            mirror=mirror,
            suite=suite,
            arch=arch,
            components=components,
            cache_dir=cache,
            exclude_leaf=_exclude_leaf,
        )

        # Plan DT_NEEDED-major compat aliases: read the chromium binary's NEEDED
        # list, the base's lib names, and the overlay's flat real libs; any need
        # not matched by exact name but present under a different major (e.g.
        # libopenh264.so.7 vs the .deb's libopenh264.so.2) gets a flat alias copy.
        alias_plan = _plan_aliases_for(raw_tar, base)

        nss_blobs = _fetch_nss_modules(cache) if add_nss else {}
        pulse_flat, nss_added, aliases_added = _repack_with_addons(
            raw_tar, out_tar, nss_blobs, alias_plan
        )

    # entrypoint + ozone proof
    entrypoint_present = _has_member(out_tar, ENTRYPOINT)
    elf = _read_member(out_tar, ENTRYPOINT)
    ozone_ok = bool(elf) and binary_has_ozone_wayland(elf)

    # final DT_NEEDED satisfaction over base ∪ produced overlay (the real gate)
    missing_needed = _unsatisfied_needed(out_tar, base)

    # §5-E conformance + base downgrade
    from tools.stage_tar_spec import validate_stage_tar

    rep = validate_stage_tar(str(out_tar), base=base)

    return GuiStageBuild(
        out_tar=str(out_tar),
        file_count=sum(1 for _ in _member_names(out_tar)),
        tar_bytes=out_tar.stat().st_size,
        extract_bytes=_extract_bytes(out_tar),
        entrypoint=ENTRYPOINT,
        entrypoint_present=entrypoint_present,
        closure_pkgs=len(m["closure"]),
        reachable_libs=tuple(m["reachable_libs"]),
        missing_soname=tuple(m["missing_soname"]),
        unsatisfied_needed=missing_needed,
        pulsecommon_flattened=pulse_flat,
        nss_modules_added=nss_added,
        compat_aliases=aliases_added,
        ozone_wayland_ok=ozone_ok,
        conformant=rep.conformant,
        violations=tuple(rep.errors),
        unsupported=tuple(m.get("unsupported", ())),
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def _print_build(b: GuiStageBuild) -> None:
    print(f"chromium GUI stage: {b.out_tar}")
    print(f"  tar size:         {b.as_dict()['tar_mib']} MiB ({b.tar_bytes} bytes)")
    print(f"  extract estimate: {b.as_dict()['extract_mib']} MiB ({b.file_count} members)")
    print(f"  entrypoint:       {b.entrypoint}  "
          f"{'PRESENT' if b.entrypoint_present else 'MISSING'}")
    print(f"  ozone-wayland:    {'YES (full GUI browser)' if b.ozone_wayland_ok else 'NO'}"
          f"  → launch flag: --ozone-platform=wayland")
    print(f"  closure pkgs:     {b.closure_pkgs}")
    print(f"  reachable libs:   {len(b.reachable_libs)} (base-missing .so added)")
    print(f"  DT_NEEDED gate:   {len(b.unsatisfied_needed)} unsatisfied"
          + (f"  !! {list(b.unsatisfied_needed)}" if b.unsatisfied_needed
             else "  (full closure: base ∪ overlay)"))
    print(f"  libpulsecommon:   "
          + (f"flattened → {b.pulsecommon_flattened}" if b.pulsecommon_flattened
             else "NOT flattened"))
    print(f"  NSS dlopen mods:  {len(b.nss_modules_added)} {list(b.nss_modules_added)}")
    if b.compat_aliases:
        print(f"  compat aliases:   {list(b.compat_aliases)}")
    if b.unsupported:
        print(f"  unsupported pkgs: {len(b.unsupported)} (config/init debs w/ abs-symlinks; "
              f"no chromium .so → harmless)")
    print(f"  overlay_guard:    {'OK' if not b.violations else str(len(b.violations)) + ' violation(s)'}")
    for v in b.violations:
        print(f"    {v}")
    print(f"  stage_tar_spec:   {'CONFORMANT' if b.conformant else 'NON-CONFORMANT'}")
    print(f"  => {'DEMO-READY (full GUI browser, 0 missing .so)' if b.ok else 'NOT READY'}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="build_chromium_gui_stage",
        description="Build the full-browser chromium GUI stage-tar "
        "(chromium-gui-stage.tar): /usr/lib/chromium/chromium + DT_NEEDED closure "
        "+ libpulsecommon-flat + NSS dlopen plugins; ozone-wayland on-screen demo.",
    )
    ap.add_argument("--base", help="base rootfs tar|dir (soname/path subtract + guard)")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help=f"output stage tar (default {DEFAULT_OUT})")
    ap.add_argument("--mirror", default=MIRROR)
    ap.add_argument("--suite", default=SUITE)
    ap.add_argument("--cache", default=DEFAULT_CACHE)
    ap.add_argument("--no-nss", action="store_true",
                    help="do NOT fold in the NSS dlopen plugins (rely on the "
                    "separate nss-stage.tar overlay instead)")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--selftest", action="store_true",
                    help="run the OFFLINE deterministic selftest and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.base:
        ap.error("--base is required (or use --selftest)")

    b = build_chromium_gui_stage(
        args.base, args.out,
        mirror=args.mirror, suite=args.suite, cache_dir=args.cache,
        add_nss=not args.no_nss,
    )
    if args.json:
        print(json.dumps(b.as_dict(), indent=2))
    else:
        _print_build(b)
    return 0 if b.ok else 1


# --------------------------------------------------------------------------- #
# OFFLINE selftest (synthetic .deb-like tar; no network)
# --------------------------------------------------------------------------- #
def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- ozone token detector ---------------------------------------------- #
    check("ozone-wayland detector: positive on tokens",
          binary_has_ozone_wayland(b"...ozone_platform_wayland.cc...ozone-platform..."))
    check("ozone-wayland detector: negative without tokens",
          not binary_has_ozone_wayland(b"headless only, no backend tokens"))
    check("ozone-wayland detector: needs BOTH tokens",
          not binary_has_ozone_wayland(b"ozone-platform but no wayland cc"))

    # --- repack: libpulsecommon flat + NSS plugins ------------------------- #
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        # a synthetic closure tar that mimics the build_minimal_overlay output:
        #   chromium binary, a flat lib, and the non-flat pulseaudio/ lib.
        base_tar = tdp / "closure.tar"
        with tarfile.open(base_tar, "w") as t:
            _add_bytes(t, "usr/lib/chromium/chromium",
                       b"\x7fELF ozone_platform_wayland.cc ozone-platform", mode=0o755)
            _add_bytes(t, f"{LIBDIR}/libdav1d.so.6", b"DAV1D")
            _add_bytes(t, f"{LIBDIR}/pulseaudio/libpulsecommon-16.1.so", b"PULSECOMMON")

        nss_blobs = {
            "libsoftokn3.so": b"SOFTOKN",
            "libsoftokn3.chk": b"CHK",
            "libfreebl3.so": b"FREEBL",
            "libnssckbi.so": b"CKBI",
        }
        # compat alias: binary NEEDs libopenh264.so.7 but overlay ships .so.2
        with tarfile.open(base_tar, "a") as t:
            _add_bytes(t, f"{LIBDIR}/libopenh264.so.2", b"OPENH264v2")
        alias_plan = {"libopenh264.so.7": f"{LIBDIR}/libopenh264.so.2"}

        out_tar = tdp / "gui.tar"
        pulse_flat, nss_added, aliases = _repack_with_addons(
            base_tar, out_tar, nss_blobs, alias_plan
        )

        names = _member_names(out_tar)
        check("compat alias libopenh264.so.7 created flat",
              f"./{LIBDIR}/libopenh264.so.7" in names)
        check("compat alias has the source lib bytes",
              _read_member(out_tar, f"{LIBDIR}/libopenh264.so.7") == b"OPENH264v2")
        check("compat alias reported", aliases == ("libopenh264.so.7 <- libopenh264.so.2",))
        check("repack carried the chromium binary through",
              "./usr/lib/chromium/chromium" in names)
        check("repack carried the flat dav1d through",
              f"./{LIBDIR}/libdav1d.so.6" in names)
        check("repack kept the non-flat pulseaudio/ libpulsecommon",
              f"./{LIBDIR}/pulseaudio/libpulsecommon-16.1.so" in names)
        check("repack ADDED flat libpulsecommon",
              f"./{LIBDIR}/libpulsecommon-16.1.so" in names)
        check("pulsecommon_flattened path reported",
              pulse_flat == f"{LIBDIR}/libpulsecommon-16.1.so")
        # flat copy bytes == the pulseaudio/ original
        flat_bytes = _read_member(out_tar, f"{LIBDIR}/libpulsecommon-16.1.so")
        check("flat libpulsecommon has the original bytes", flat_bytes == b"PULSECOMMON")

        for mod in ("libsoftokn3.so", "libfreebl3.so", "libnssckbi.so"):
            check(f"NSS {mod} added flat", f"./{LIBDIR}/{mod}" in names)
            check(f"NSS {mod} added under nss/", f"./{LIBDIR}/nss/{mod}" in names)
        check("NSS .chk sidecar carried", f"./{LIBDIR}/libsoftokn3.chk" in names)
        check("nss_modules_added lists the .so modules (not .chk)",
              set(nss_added) == {"libsoftokn3.so", "libfreebl3.so", "libnssckbi.so"})

        # entrypoint + ozone proof on the produced tar
        check("entrypoint present in produced tar", _has_member(out_tar, ENTRYPOINT))
        elf = _read_member(out_tar, ENTRYPOINT)
        check("produced binary passes ozone-wayland proof",
              elf is not None and binary_has_ozone_wayland(elf))

        # §5-E conformance of the produced tar (structural, no base)
        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(out_tar))
        check("produced tar is §5-E conformant", rep.conformant)

        # idempotent skip: re-running repack when the addons already exist adds none
        out_tar2 = tdp / "gui2.tar"
        pf2, nss2, al2 = _repack_with_addons(out_tar, out_tar2, nss_blobs, alias_plan)
        check("re-repack does not duplicate already-present flat pulsecommon",
              pf2 is None)
        check("re-repack adds no NSS modules the second time", nss2 == ())
        check("re-repack adds no compat alias the second time", al2 == ())

        # --- compat_alias_plan unit: only aliases a same-stem different-major need #
        plan = compat_alias_plan(
            ["libopenh264.so.7", "libc.so.6", "libfoo.so.9"],
            {"libc.so.6"},                                   # base provides libc
            {"libopenh264.so.2": f"{LIBDIR}/libopenh264.so.2"},  # overlay has .so.2
        )
        check("plan aliases libopenh264.so.7 -> overlay .so.2",
              plan.get("libopenh264.so.7") == f"{LIBDIR}/libopenh264.so.2")
        check("plan does NOT alias a base-satisfied need (libc)", "libc.so.6" not in plan)
        check("plan does NOT alias a need with no same-stem overlay lib (libfoo)",
              "libfoo.so.9" not in plan)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
