#!/usr/bin/env python3
"""Build the chromium **content_shell** stage-tar (content-shell-stage.tar).

Why content_shell (the lightest on-screen chromium that still opens a window)
----------------------------------------------------------------------------
The two prior chromium stagings sit at the wrong ends for an ALR on-screen demo:

  * ``chromium-headless-shell`` — the ``--headless`` render engine. **No window
    backend** (the wayland Ozone platform is *not* compiled in), so it can
    ``--dump-dom``/screenshot off-screen but never opens an on-screen window.
  * the full ``chromium`` browser — opens a real ``--ozone-platform=wayland``
    window, but is a **multi-process** browser (browser + GPU + renderer +
    utility). Under the ALR in-process re-map each child re-maps ~0.8-1.3 GB →
    the window never appears on a phone (OOM). See
    ``docs/research/chromium-window-demo-plan.md`` §2 and the memory note
    ``chromium-native-goal`` CR-4.

``content_shell`` is Chromium's *minimal embedder* of the content module: a tiny
shell window with **the full Ozone platform set compiled in** (so
``--ozone-platform=wayland`` selects the wayland backend the same way full chrome
does), yet small enough — and crucially **runnable single-process**
(``--single-process``) — that the ALR loader re-maps **one** address space, not
four. That is the memory-fit win this builder targets: *one* process opening a
*real* wayland window.

Where the binary comes from (it IS a distro .deb — settled, not inferred)
------------------------------------------------------------------------
content_shell is normally a Chromium build artifact, **not** packaged by most
distros — but **Debian packages it as ``chromium-shell``** (source package
``chromium``; description: "a minimal version of the chromium user interface (the
content shell)"). We use Debian **bookworm** ``chromium-shell``
147.0.7727.137 (glibc 2.36); a 2.36-linked binary runs on the noble 2.39 base
(glibc is backward-compatible — same forward-compat the headless/full chromium
stagings already rely on). The entrypoint ELF is
``/usr/lib/chromium/chromium-shell`` (the ``/usr/bin/chromium-shell`` is a 52-byte
shell wrapper, NOT the ELF the ALR loader maps). The ``chromium-common`` .deb adds
the shared resources (``icudtl.dat``, V8 snapshots, ``libGLESv2``/``libEGL``/
swiftshader, the locale ``.pak``s) the shell loads at runtime.

**ozone-wayland is host-verified present** in this exact binary: the
``ozone_platform_wayland.cc`` token is in ``/usr/lib/chromium/chromium-shell``
(``binary_has_ozone_wayland`` proves it at build time; the ``--selftest`` exercises
the detector). The headless token is *absent* (this is not the headless-shell).

What this builder produces
--------------------------
A §5-E ``./``-rooted overlay tar (the unit ``RootfsInstaller.extractOverlayTar``
lays over the base) carrying, with **zero unsatisfied DT_NEEDED** in the runtime
closure:

  1. The ``chromium-shell`` ELF + its ``content_shell.pak`` / ``shell_resources.pak``
     and ``libtest_trace_processor.so`` (the leaf .deb's own files, kept verbatim
     at ``/usr/lib/chromium/``).
  2. Every shared library the shell binary pulls via **DT_NEEDED** (transitively)
     that the **base rootfs does not already provide** — computed by
     ``deb_closure.build_minimal_overlay`` (flat-SONAME, base-subtracted, no
     base-library downgrade). Same engine the full-GUI builder uses.
  3. **libpulsecommon-<v>.so flattened** onto the flat ``LD_LIBRARY_PATH`` dir
     (libpulse DT_NEEDEDs it but only resolves it on its absolute RUNPATH, which
     the ALR loader does not honor — same fix the full-GUI and first-device runs
     needed).
  3b. **``$ORIGIN``-only DT_NEEDED .so flattened.** chromium-shell is linked with
     ``RUNPATH=$ORIGIN`` and DT_NEEDEDs ``libtest_trace_processor.so``, which the
     .deb installs ONLY next to the binary (``$ORIGIN`` = ``/usr/lib/chromium/``).
     The ALR loader resolves DT_NEEDED solely on the flat ``LD_LIBRARY_PATH`` and
     does NOT search ``$ORIGIN``, so the on-device load FATALs ``libtest_trace_
     processor.so: cannot open shared object file`` even though the file is in the
     tar. Fix (this builder, ``_flatten_origin_needed``): copy each such NEEDED .so
     to the flat libdir. Distinct from the full ``chromium`` binary, which has no
     RUNPATH and no ``$ORIGIN``-only NEEDED — this is content_shell-specific.
  4. **NSS dlopen plugin modules** (libsoftokn3/libfreebl3/…). content_shell
     initializes NSS during TLS; those modules are ``dlopen()``'d (NOT DT_NEEDED →
     the closure drops them, and NSS FATALs ``nss_error=-5925`` without them).
     Packed flat + under ``…/nss/``, exactly like the full-GUI builder.

This reuses ``build_chromium_gui_stage``'s repack/closure helpers verbatim (the
flattening, NSS folding, compat-alias planning, ozone proof, §5-E validation are
identical problems); only the package / entrypoint / output differ. The
content_shell leaf has **no** headless-shell binary to drop (unlike the full
``chromium`` .deb), so there is no ``DROP_LEAF``.

Honest scope
------------
HOST-ONLY. This builds + validates the tar host-side (closure resolution, §5-E
conformance, entrypoint present, ozone-wayland token check). It does NOT run
content_shell on a device — the on-screen ``--ozone-platform=wayland`` window and
the single-process memory-fit claim are the **device/compositor gate** (DEVICE-REQ
in ``docs/research/chromium-window-lightweight-options.md``). "0 missing .so" is
over the binary's **DT_NEEDED** graph resolved on the **flat ``LD_LIBRARY_PATH``**
(the loader's real search path — NOT basename-anywhere, which previously gave a
false 0 for the ``$ORIGIN`` ``libtest_trace_processor.so``), plus the two dlopen
sets added by hand (pulse, NSS); a real render may still want runtime config
(fonts, the net overlay for DNS/TLS) supplied by the other overlays.
"""

from __future__ import annotations

import argparse
import io
import json
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path

# Reuse the full-GUI builder's engine verbatim — the repack (flat libpulsecommon
# + NSS dlopen plugins + DT_NEEDED-major compat aliases), the ozone proof, and the
# DT_NEEDED-satisfaction gate are identical problems for content_shell.
from tools.build_chromium_gui_stage import (
    LIBDIR,
    NSS_MODULES,
    NSS_PACKAGE,
    OZONE_WAYLAND_TOKENS,
    binary_has_ozone_wayland,
    compat_alias_plan,
    _add_bytes,
    _extract_bytes,
    _has_member,
    _lib_basenames,
    _member_names,
    _overlay_real_libs,
    _read_member,
    _repack_with_addons,
)
from tools.deb_closure import build_minimal_overlay
from tools.build_stage_tar import extract_deb
from tools.deb_closure import fetch_packages_index, parse_packages, _download_deb

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

# Debian (Ubuntu noble ships chromium as a snap; Debian ships a real .deb, and is
# the only distro that packages content_shell — as ``chromium-shell``).
MIRROR = "http://deb.debian.org/debian"
SUITE = "bookworm"
ARCH = "arm64"
COMPONENTS = ("main",)

# Leaf packages: the content_shell binary + its shared resources/data.
PACKAGES = ("chromium-shell", "chromium-common")

# Rootfs-absolute entrypoint the device loader maps (the ELF, NOT the 52-byte
# /usr/bin/chromium-shell shell wrapper). build_minimal_overlay keeps the leaf
# .deb's own files, so this binary is carried verbatim.
ENTRYPOINT = "/usr/lib/chromium/chromium-shell"

ARCH_TRIPLET = "aarch64-linux-gnu"

# The entrypoint's own directory == its DT_RUNPATH ``$ORIGIN``. chromium-shell is
# linked with ``RUNPATH=$ORIGIN`` and DT_NEEDEDs ``libtest_trace_processor.so``,
# which the .deb installs ONLY here (next to the binary), NOT in the flat libdir.
# On a normal Linux system the loader finds it via $ORIGIN; the ALR loader resolves
# DT_NEEDED solely on the flat LD_LIBRARY_PATH (= LIBDIR) and does NOT honor
# $ORIGIN, so any such $ORIGIN-only NEEDED .so must be FLATTENED into LIBDIR or the
# ELF dies at load with "cannot open shared object file" (device-confirmed for
# libtest_trace_processor.so). Same RUNPATH-not-honored class as libpulsecommon.
ORIGIN_DIR = str(Path(ENTRYPOINT).parent).lstrip("/")  # "usr/lib/chromium"

DEFAULT_OUT = "out/v2-stage/content-shell-stage.tar"
DEFAULT_CACHE = "/tmp/deb-cache-bookworm"


# --------------------------------------------------------------------------- #
# Result
# --------------------------------------------------------------------------- #
@dataclass
class ContentShellStageBuild:
    out_tar: str
    file_count: int
    tar_bytes: int
    extract_bytes: int
    entrypoint: str
    entrypoint_present: bool
    entrypoint_bytes: int
    closure_pkgs: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    unsatisfied_needed: tuple[str, ...]
    pulsecommon_flattened: str | None
    origin_libs_flattened: tuple[str, ...]
    nss_modules_added: tuple[str, ...]
    compat_aliases: tuple[str, ...]
    ozone_wayland_ok: bool
    conformant: bool
    violations: tuple[str, ...]
    unsupported: tuple[str, ...] = ()

    @property
    def ok(self) -> bool:
        """Demo-ready iff: the content_shell entrypoint is present, EVERY DT_NEEDED
        is satisfied by base ∪ overlay (0 unsatisfied), NSS init won't FATAL
        (softokn+freebl present), libpulsecommon is flat, the Wayland Ozone backend
        is compiled in, §5-E conformant, no base downgrade."""
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
                  "origin_libs_flattened", "nss_modules_added", "compat_aliases",
                  "violations", "unsupported"):
            d[k] = list(getattr(self, k))
        d["tar_mib"] = round(self.tar_bytes / (1024 * 1024), 1)
        d["extract_mib"] = round(self.extract_bytes / (1024 * 1024), 1)
        d["entrypoint_mib"] = round(self.entrypoint_bytes / (1024 * 1024), 1)
        d["ok"] = self.ok
        return d


# --------------------------------------------------------------------------- #
# NSS dlopen plugins — fetch (same set/source as the full-GUI builder)
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


# --------------------------------------------------------------------------- #
# DT_NEEDED of the entrypoint over base ∪ overlay (parametrized on ENTRYPOINT)
# --------------------------------------------------------------------------- #
def _entry_needed(tar_path: str | Path) -> list[str]:
    """The DT_NEEDED list of the content_shell entrypoint binary inside the overlay."""
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
    """Compat-alias plan for the content_shell binary against base ∪ overlay
    (e.g. libopenh264.so.7 wanted vs the .deb's .so.2)."""
    needed = _entry_needed(overlay_tar)
    if not needed:
        return {}
    base_names = _lib_basenames(base)
    overlay_real = _overlay_real_libs(overlay_tar)
    return compat_alias_plan(needed, base_names, overlay_real)


# --------------------------------------------------------------------------- #
# Flat-load-path satisfaction (the REAL device criterion, not basename-anywhere)
# --------------------------------------------------------------------------- #
# A flat ARCH libdir the ALR loader puts on LD_LIBRARY_PATH. The base ships libs
# under BOTH /lib/<triplet> and /usr/lib/<triplet> (merged-usr); a real lib resolves
# only if it sits DIRECTLY in one of these (no deeper subdir like pulseaudio/ or
# nss/, and NOT in the entrypoint's $ORIGIN dir which the loader does not search).
_FLAT_LIBDIRS = (f"lib/{ARCH_TRIPLET}", f"usr/lib/{ARCH_TRIPLET}")


def _flat_lib_basenames(tar_path: str | Path) -> set[str]:
    """Library basenames a tar provides on the FLAT load path — i.e. files sitting
    DIRECTLY in a flat ARCH libdir (``lib/<triplet>`` or ``usr/lib/<triplet>``), the
    only place the ALR loader resolves DT_NEEDED. A .so buried in a subdir
    (``…/pulseaudio/``, ``…/nss/``) or at the entrypoint's ``$ORIGIN``
    (``/usr/lib/chromium/``) is NOT counted — matching what the loader can actually
    find, unlike ``_lib_basenames`` (which counts a basename anywhere in the tar and
    so falsely satisfied ``libtest_trace_processor.so`` from $ORIGIN)."""
    out: set[str] = set()
    with tarfile.open(tar_path, "r:*") as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            rel = m.name.lstrip("./")
            for d in _FLAT_LIBDIRS:
                if rel.startswith(d + "/") and "/" not in rel[len(d) + 1:]:
                    out.add(Path(rel).name)
                    break
    return out


def _origin_needed_to_flatten(overlay_tar: str | Path) -> dict[str, str]:
    """Plan flat copies for entrypoint DT_NEEDED .so files that exist in the overlay
    ONLY at the entrypoint's ``$ORIGIN`` dir (``/usr/lib/chromium/``) and NOT on the
    flat load path.

    Returns ``{soname: origin_rel_source}`` (e.g.
    ``libtest_trace_processor.so -> usr/lib/chromium/libtest_trace_processor.so``).
    A NEEDED already on the flat path (base or overlay) yields no entry; a NEEDED with
    no $ORIGIN copy either yields no entry (it is a genuine miss the gate reports)."""
    needed = _entry_needed(overlay_tar)
    if not needed:
        return {}
    flat = _flat_lib_basenames(overlay_tar)
    origin_files = {
        Path(n.lstrip("./")).name: n.lstrip("./")
        for n in _member_names(overlay_tar)
        if n.lstrip("./").startswith(ORIGIN_DIR + "/")
        and "/" not in n.lstrip("./")[len(ORIGIN_DIR) + 1:]
    }
    plan: dict[str, str] = {}
    for soname in needed:
        if soname.startswith("ld-linux") or soname in flat:
            continue
        src = origin_files.get(soname)
        if src is not None:
            plan[soname] = src
    return plan


def _flatten_origin_needed(tar_path: Path, plan: dict[str, str]) -> tuple[str, ...]:
    """Append a flat ``{LIBDIR}/<soname>`` copy of each $ORIGIN-only NEEDED .so to an
    existing overlay tar (in place). Idempotent: skips any flat dest already present.
    Returns the sorted list of sonames flattened."""
    if not plan:
        return ()
    existing = _member_names(tar_path)
    flattened: list[str] = []
    todo = {
        soname: src for soname, src in plan.items()
        if ("./" + f"{LIBDIR}/{soname}") not in existing
    }
    if not todo:
        return ()
    with tarfile.open(tar_path, "r:*") as src_t:
        blobs = {
            soname: src_t.extractfile(src_t.getmember("./" + src)).read()
            for soname, src in todo.items()
        }
    with tarfile.open(tar_path, "a") as dst:
        for soname in sorted(blobs):
            _add_bytes(dst, f"{LIBDIR}/{soname}", blobs[soname], mode=0o755)
            flattened.append(soname)
    return tuple(flattened)


def _unsatisfied_needed(overlay_tar: str | Path, base: str | Path) -> tuple[str, ...]:
    """DT_NEEDED entries of the content_shell binary NOT resolvable on the FLAT load
    path (base ∪ overlay flat ARCH libdirs). ``ld-linux`` (the dynamic loader, always
    provided by the runtime) is excluded.

    This is the HONEST device gate: it counts only libs the ALR loader can actually
    find on LD_LIBRARY_PATH. The earlier basename-anywhere check counted the
    ``$ORIGIN`` copy of ``libtest_trace_processor.so`` and so reported 0 unsatisfied
    while the device FATALed — that false-negative is what this flat-aware gate fixes.
    """
    needed = _entry_needed(overlay_tar)
    if not needed:
        return ()
    have = _flat_lib_basenames(base) | _flat_lib_basenames(overlay_tar)
    miss = [n for n in needed if n not in have and not n.startswith("ld-linux")]
    return tuple(sorted(miss))


# --------------------------------------------------------------------------- #
# Build
# --------------------------------------------------------------------------- #
def build_content_shell_stage(
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path = DEFAULT_CACHE,
    add_nss: bool = True,
) -> ContentShellStageBuild:
    """Build the content_shell stage-tar (NETWORK).

    1. ``deb_closure.build_minimal_overlay`` packs the leaf chromium-shell files
       (content_shell ELF + paks + libtest_trace_processor) + the DT_NEEDED libs
       the base lacks, base-subtracted and §5-E flat.
    2. repack to add the flat libpulsecommon, the NSS dlopen plugins, and any
       DT_NEEDED-major compat alias.
    3. flatten every $ORIGIN-only DT_NEEDED .so (libtest_trace_processor.so) into the
       flat libdir — the leaf .deb installs it next to the binary ($ORIGIN RUNPATH),
       which the ALR loader does not search (device-confirmed FATAL otherwise).
    4. validate: entrypoint present, §5-E conformant, no base downgrade, Wayland
       Ozone token present, 0 DT_NEEDED unsatisfied on the FLAT load path.
    """
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    cache = Path(cache_dir)
    cache.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        raw_tar = Path(td) / "content-shell-closure.tar"
        m = build_minimal_overlay(
            list(PACKAGES),
            base,
            raw_tar,
            mirror=mirror,
            suite=suite,
            arch=arch,
            components=components,
            cache_dir=cache,
            # No exclude_leaf: the chromium-shell .deb ships ONLY the content_shell
            # binary (no headless-shell to drop, unlike the full chromium .deb).
        )

        alias_plan = _plan_aliases_for(raw_tar, base)
        nss_blobs = _fetch_nss_modules(cache) if add_nss else {}
        pulse_flat, nss_added, aliases_added = _repack_with_addons(
            raw_tar, out_tar, nss_blobs, alias_plan
        )

    # Flatten $ORIGIN-only DT_NEEDED .so files (libtest_trace_processor.so) into the
    # flat libdir so the ALR loader (which honors only LD_LIBRARY_PATH, not $ORIGIN)
    # can resolve them. The $ORIGIN copy at /usr/lib/chromium/ is kept too (harmless).
    origin_plan = _origin_needed_to_flatten(out_tar)
    origin_flattened = _flatten_origin_needed(out_tar, origin_plan)

    entrypoint_present = _has_member(out_tar, ENTRYPOINT)
    elf = _read_member(out_tar, ENTRYPOINT)
    ozone_ok = bool(elf) and binary_has_ozone_wayland(elf)
    entry_bytes = len(elf) if elf else 0

    missing_needed = _unsatisfied_needed(out_tar, base)

    from tools.stage_tar_spec import validate_stage_tar

    rep = validate_stage_tar(str(out_tar), base=base)

    return ContentShellStageBuild(
        out_tar=str(out_tar),
        file_count=sum(1 for _ in _member_names(out_tar)),
        tar_bytes=out_tar.stat().st_size,
        extract_bytes=_extract_bytes(out_tar),
        entrypoint=ENTRYPOINT,
        entrypoint_present=entrypoint_present,
        entrypoint_bytes=entry_bytes,
        closure_pkgs=len(m["closure"]),
        reachable_libs=tuple(m["reachable_libs"]),
        missing_soname=tuple(m["missing_soname"]),
        unsatisfied_needed=missing_needed,
        pulsecommon_flattened=pulse_flat,
        origin_libs_flattened=origin_flattened,
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
def _print_build(b: ContentShellStageBuild) -> None:
    d = b.as_dict()
    print(f"content_shell stage: {b.out_tar}")
    print(f"  tar size:         {d['tar_mib']} MiB ({b.tar_bytes} bytes)")
    print(f"  extract estimate: {d['extract_mib']} MiB ({b.file_count} members)")
    print(f"  entrypoint:       {b.entrypoint}  "
          f"{'PRESENT' if b.entrypoint_present else 'MISSING'}  "
          f"({d['entrypoint_mib']} MiB ELF)")
    print(f"  ozone-wayland:    {'YES (window-capable shell)' if b.ozone_wayland_ok else 'NO'}"
          f"  → launch flag: --ozone-platform=wayland")
    print(f"  process model:    single-process capable (--single-process) "
          f"→ ONE re-mapped address space")
    print(f"  closure pkgs:     {b.closure_pkgs}")
    print(f"  reachable libs:   {len(b.reachable_libs)} (base-missing .so added)")
    print(f"  DT_NEEDED gate:   {len(b.unsatisfied_needed)} unsatisfied on FLAT load path"
          + (f"  !! {list(b.unsatisfied_needed)}" if b.unsatisfied_needed
             else "  (full closure: base ∪ overlay flat libdir)"))
    print(f"  libpulsecommon:   "
          + (f"flattened → {b.pulsecommon_flattened}" if b.pulsecommon_flattened
             else "NOT flattened"))
    print(f"  $ORIGIN .so flat: "
          + (f"{list(b.origin_libs_flattened)} (RUNPATH=$ORIGIN, loader-invisible → flattened)"
             if b.origin_libs_flattened else "none needed"))
    print(f"  NSS dlopen mods:  {len(b.nss_modules_added)} {list(b.nss_modules_added)}")
    if b.compat_aliases:
        print(f"  compat aliases:   {list(b.compat_aliases)}")
    if b.unsupported:
        print(f"  unsupported pkgs: {len(b.unsupported)} (config/init debs w/ abs-symlinks; "
              f"no shell .so → harmless)")
    print(f"  overlay_guard:    {'OK' if not b.violations else str(len(b.violations)) + ' violation(s)'}")
    for v in b.violations:
        print(f"    {v}")
    print(f"  stage_tar_spec:   {'CONFORMANT' if b.conformant else 'NON-CONFORMANT'}")
    print(f"  => {'DEMO-READY (single-process window-capable content_shell, 0 missing .so)' if b.ok else 'NOT READY'}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="build_content_shell_stage",
        description="Build the chromium content_shell stage-tar "
        "(content-shell-stage.tar): /usr/lib/chromium/chromium-shell + DT_NEEDED "
        "closure + libpulsecommon-flat + NSS dlopen plugins; a single-process, "
        "ozone-wayland, memory-fit on-screen shell (lighter than full chromium).",
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

    b = build_content_shell_stage(
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
# Synthetic ELF builder (offline tests) — a minimal valid ELF64-LE AArch64 ET_DYN
# carrying a real .dynamic section so read_elf_dynamic returns the given DT_NEEDED
# (+ optional DT_RUNPATH). Lets the $ORIGIN-flatten path be exercised with no .deb.
# --------------------------------------------------------------------------- #
def _synthetic_elf(needed: tuple[str, ...], runpath: str | None = None,
                   extra_tokens: bytes = b"") -> bytes:
    import struct

    strs = b"\x00"
    off: dict[str, int] = {}
    for n in list(needed) + ([runpath] if runpath else []):
        if n not in off:
            off[n] = len(strs)
            strs += n.encode() + b"\x00"

    DT_NEEDED, DT_STRTAB, DT_STRSZ, DT_RUNPATH, DT_NULL = 1, 5, 10, 29, 0
    ehdr_sz, phdr_sz, nph = 64, 56, 2
    strtab_off = ehdr_sz + phdr_sz * nph + len(extra_tokens)
    dyn_off = strtab_off + len(strs)

    dyn = b"".join(struct.pack("<qQ", DT_NEEDED, off[n]) for n in needed)
    if runpath:
        dyn += struct.pack("<qQ", DT_RUNPATH, off[runpath])
    dyn += struct.pack("<qQ", DT_STRTAB, strtab_off)   # vaddr == fileoff (identity)
    dyn += struct.pack("<qQ", DT_STRSZ, len(strs))
    dyn += struct.pack("<qQ", DT_NULL, 0)
    total = dyn_off + len(dyn)

    e_ident = b"\x7fELF" + bytes([2, 1, 1]) + b"\x00" * 9
    ehdr = e_ident + struct.pack(
        "<HHIQQQIHHHHHH",
        3, 183, 1, 0, ehdr_sz, 0, 0, ehdr_sz, phdr_sz, nph, 0, 0, 0,
    )
    PT_LOAD, PT_DYNAMIC = 1, 2
    ph0 = struct.pack("<IIQQQQQQ", PT_LOAD, 5, 0, 0, 0, total, total, 0x1000)
    ph1 = struct.pack("<IIQQQQQQ", PT_DYNAMIC, 6, dyn_off, dyn_off, 0, len(dyn), len(dyn), 8)
    return ehdr + ph0 + ph1 + extra_tokens + strs + dyn


# --------------------------------------------------------------------------- #
# OFFLINE selftest (synthetic .deb-like tar; no network)
# --------------------------------------------------------------------------- #
def _selftest() -> int:
    """Offline checks of the content_shell-specific wiring. The repack/closure/
    ozone-detector internals are reused from build_chromium_gui_stage and are
    covered by ITS selftest + tests; here we assert THIS module's contract:
    entrypoint path, package set, and that the produced tar is conformant +
    ozone-proven + DT_NEEDED-satisfied over a synthetic base."""
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- module contract --------------------------------------------------- #
    check("entrypoint is the content_shell ELF (not the /usr/bin wrapper)",
          ENTRYPOINT == "/usr/lib/chromium/chromium-shell")
    check("leaf packages = chromium-shell + chromium-common",
          PACKAGES == ("chromium-shell", "chromium-common"))
    check("ozone detector reused: positive on tokens",
          binary_has_ozone_wayland(b"x ozone_platform_wayland.cc y ozone-platform z"))
    check("ozone detector reused: needs the wayland src token",
          not binary_has_ozone_wayland(b"only ozone-platform here"))

    # --- synthetic end-to-end over a fake base + fake closure -------------- #
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)

        # synthetic base providing libc + a couple common libs the shell NEEDs
        base_tar = tdp / "base.tar"
        with tarfile.open(base_tar, "w") as t:
            for ln in ("libc.so.6", "libdl.so.2", "libglib-2.0.so.0"):
                _add_bytes(t, f"lib/{ARCH_TRIPLET}/{ln}", b"BASE-" + ln.encode())

        # synthetic closure tar mimicking build_minimal_overlay output for the
        # content_shell leaf: the ELF (with ozone tokens + a DT_NEEDED we satisfy
        # in-overlay is not modeled here — we model only the structural repack),
        # the paks, a flat overlay lib, and the non-flat pulseaudio/ lib.
        closure_tar = tdp / "closure.tar"
        with tarfile.open(closure_tar, "w") as t:
            _add_bytes(t, "usr/lib/chromium/chromium-shell",
                       b"\x7fELF ozone_platform_wayland.cc ozone-platform", mode=0o755)
            _add_bytes(t, "usr/lib/chromium/content_shell.pak", b"PAK")
            _add_bytes(t, "usr/lib/chromium/shell_resources.pak", b"PAK2")
            _add_bytes(t, f"usr/lib/{ARCH_TRIPLET}/libdav1d.so.6", b"DAV1D")
            _add_bytes(t, f"usr/lib/{ARCH_TRIPLET}/pulseaudio/libpulsecommon-16.1.so",
                       b"PULSECOMMON")
            _add_bytes(t, f"usr/lib/{ARCH_TRIPLET}/libopenh264.so.2", b"OPENH264v2")

        nss_blobs = {
            "libsoftokn3.so": b"SOFTOKN", "libsoftokn3.chk": b"CHK",
            "libfreebl3.so": b"FREEBL", "libnssckbi.so": b"CKBI",
        }
        alias_plan = {"libopenh264.so.7": f"usr/lib/{ARCH_TRIPLET}/libopenh264.so.2"}

        out_tar = tdp / "content-shell.tar"
        pulse_flat, nss_added, aliases = _repack_with_addons(
            closure_tar, out_tar, nss_blobs, alias_plan
        )

        names = _member_names(out_tar)
        check("repack carried the content_shell ELF through",
              "./usr/lib/chromium/chromium-shell" in names)
        check("repack carried content_shell.pak through",
              "./usr/lib/chromium/content_shell.pak" in names)
        check("repack ADDED flat libpulsecommon",
              f"./usr/lib/{ARCH_TRIPLET}/libpulsecommon-16.1.so" in names)
        check("pulsecommon_flattened reported",
              pulse_flat == f"usr/lib/{ARCH_TRIPLET}/libpulsecommon-16.1.so")
        check("compat alias libopenh264.so.7 created flat",
              f"./usr/lib/{ARCH_TRIPLET}/libopenh264.so.7" in names)
        for mod in ("libsoftokn3.so", "libfreebl3.so", "libnssckbi.so"):
            check(f"NSS {mod} added flat + nss/",
                  f"./usr/lib/{ARCH_TRIPLET}/{mod}" in names
                  and f"./usr/lib/{ARCH_TRIPLET}/nss/{mod}" in names)
        check("nss_modules_added lists the .so modules (not .chk)",
              set(nss_added) == {"libsoftokn3.so", "libfreebl3.so", "libnssckbi.so"})

        # entrypoint + ozone proof on the produced tar
        check("entrypoint present in produced tar", _has_member(out_tar, ENTRYPOINT))
        elf = _read_member(out_tar, ENTRYPOINT)
        check("produced content_shell passes ozone-wayland proof",
              elf is not None and binary_has_ozone_wayland(elf))

        # DT_NEEDED satisfaction gate over the synthetic base ∪ overlay: the
        # synthetic ELF has no real DT_NEEDED section, so _entry_needed → [] →
        # 0 unsatisfied (the gate is exercised for real by the network build).
        miss = _unsatisfied_needed(out_tar, base_tar)
        check("DT_NEEDED gate runs (synthetic ELF → 0 unsatisfied)", miss == ())

        # §5-E conformance of the produced tar
        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(out_tar))
        check("produced tar is §5-E conformant", rep.conformant)
        with tarfile.open(out_tar) as t:
            check("every member is ./-rooted",
                  all(n.startswith("./") for n in t.getnames()))
            check("no symlink members", not any(m.issym() for m in t.getmembers()))

        # idempotent re-repack adds nothing
        out2 = tdp / "content-shell2.tar"
        pf2, nss2, al2 = _repack_with_addons(out_tar, out2, nss_blobs, alias_plan)
        check("re-repack adds nothing (idempotent)",
              pf2 is None and nss2 == () and al2 == ())

    # --- $ORIGIN-only DT_NEEDED flatten (the libtest_trace_processor.so fix) ---- #
    # A REAL synthetic ELF: RUNPATH=$ORIGIN, DT_NEEDED libtest_trace_processor.so
    # (the leaf installs it ONLY at $ORIGIN /usr/lib/chromium/) + libc.so.6 (base).
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        base_tar = tdp / "base.tar"
        with tarfile.open(base_tar, "w") as t:
            _add_bytes(t, f"lib/{ARCH_TRIPLET}/libc.so.6", b"BASE-libc")

        ov = tdp / "ov.tar"
        with tarfile.open(ov, "w") as t:
            _add_bytes(t, "usr/lib/chromium/chromium-shell",
                       _synthetic_elf(("libtest_trace_processor.so", "libc.so.6"),
                                      runpath="$ORIGIN",
                                      extra_tokens=b"ozone_platform_wayland.cc ozone-platform"),
                       mode=0o755)
            # the leaf .deb ships the lib ONLY next to the binary ($ORIGIN), NOT flat
            _add_bytes(t, "usr/lib/chromium/libtest_trace_processor.so", b"TTP")

        # BEFORE flatten: the flat-aware gate must FLAG it (the device-true state) —
        # proving the old basename-anywhere gate's 0 was a false negative.
        check("entrypoint NEEDs libtest_trace_processor.so",
              "libtest_trace_processor.so" in _entry_needed(ov))
        miss_before = _unsatisfied_needed(ov, base_tar)
        check("flat-aware gate FLAGS $ORIGIN-only lib before flatten",
              miss_before == ("libtest_trace_processor.so",))

        # plan + flatten in place
        plan = _origin_needed_to_flatten(ov)
        check("flatten plan targets libtest_trace_processor.so from $ORIGIN",
              plan == {"libtest_trace_processor.so": "usr/lib/chromium/libtest_trace_processor.so"})
        flattened = _flatten_origin_needed(ov, plan)
        check("flatten reports libtest_trace_processor.so", flattened == ("libtest_trace_processor.so",))

        names = _member_names(ov)
        check("flat copy added to LIBDIR",
              f"./usr/lib/{ARCH_TRIPLET}/libtest_trace_processor.so" in names)
        check("flat copy carries the $ORIGIN bytes",
              _read_member(ov, f"usr/lib/{ARCH_TRIPLET}/libtest_trace_processor.so") == b"TTP")
        check("$ORIGIN copy still kept (harmless)",
              "./usr/lib/chromium/libtest_trace_processor.so" in names)

        # AFTER flatten: 0 unsatisfied on the flat load path (the real device gate)
        check("flat-aware gate: 0 unsatisfied after flatten",
              _unsatisfied_needed(ov, base_tar) == ())
        # idempotent: re-planning finds nothing to do
        check("flatten is idempotent (nothing to re-add)",
              _flatten_origin_needed(ov, _origin_needed_to_flatten(ov)) == ())
        # a NEEDED with no $ORIGIN copy is NOT invented (stays a genuine miss)
        ov2 = tdp / "ov2.tar"
        with tarfile.open(ov2, "w") as t:
            _add_bytes(t, "usr/lib/chromium/chromium-shell",
                       _synthetic_elf(("libabsent.so.9",)), mode=0o755)
        check("no $ORIGIN copy → no phantom flatten",
              _origin_needed_to_flatten(ov2) == {})

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
