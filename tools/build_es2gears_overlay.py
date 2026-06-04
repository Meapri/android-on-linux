#!/usr/bin/env python3
"""build_es2gears_overlay.py — assemble the ``es2gears-stage.tar`` overlay carrying a
REAL upstream GLES2 demo (**es2gears**, the ES 2.0 spinning gears) + the one
base-missing runtime lib it needs (``libdecor-0.so.0``), so we can prove a REAL GLES
app — not a bespoke test client — runs HW-accelerated via ANGLE → our Vulkan ICD →
Mali-G615.

Why es2gears, and why these exact bytes
---------------------------------------
The ANGLE-on-Vulkan path is already PROVEN with a bespoke client (``alr-angle-vk``,
surfaceless FBO render). The next rung is a CANONICAL real app. ``es2gears`` is the
cleanest such app:

  * it is genuinely **GLES2** (ES 2.0 shaders) — ``glGetString(GL_RENDERER)`` reports
    the live GL backend, so on ANGLE→Vulkan→Mali it prints ``ANGLE (... Mali-G615 ...
    Vulkan ...)`` and software=false;
  * it is **tiny** (a single ~40 KiB ELF, no asset files) — far lighter than glmark2;
  * it **DT_NEEDED ``libEGL.so.1`` + ``libGLESv2.so.2``** — the exact SONAMEs our
    SYSTEM ANGLE ships at (``/usr/lib/androlinux-angle``, first on LD_LIBRARY_PATH
    under ALR_ANGLE), so at runtime es2gears binds ANGLE's EGL/GLES, NOT any other.

Source: the Ubuntu **noble** ``mesa-utils-bin`` .deb (Source: ``mesa-demos``,
``ports.ubuntu.com`` arm64, component ``universe``). Debian/Ubuntu suffix the demo
binaries with the multiarch triplet, so the .deb ships TWO es2gears flavours:

  usr/bin/es2gears_wayland.aarch64-linux-gnu
      DT_NEEDED: libGLESv2.so.2, libEGL.so.1, libwayland-egl.so.1,
                 libwayland-client.so.0, libxkbcommon.so.0, **libdecor-0.so.0**,
                 libm/libc/ld. Opens a WAYLAND window (wl_egl_window) → ANGLE's
                 DisplayVkWayland → VK swapchain on the in-app compositor. This is the
                 ON-SCREEN target under the ALR_ANGLE env (DISPLAY unset,
                 XDG_SESSION_TYPE=wayland → ANGLE selects the Wayland WSI backend).
  usr/bin/es2gears_x11.aarch64-linux-gnu
      DT_NEEDED: libGLESv2.so.2, libEGL.so.1, libX11.so.6, libm/libc/ld. Opens an X11
      window → needs Xwayland. Kept as the alternate display rung.

We stage BOTH real ELFs and ALSO add unsuffixed convenience symlinks
``/usr/bin/es2gears_wayland`` / ``/usr/bin/es2gears_x11`` / ``/usr/bin/es2gears``
(→ the wayland flavour) so the launch marker can name a stable path.

The Mesa-GL STRIP (load-bearing — without it ANGLE never wins)
-------------------------------------------------------------
``mesa-utils-bin`` Depends on ``libegl1`` / ``libgles2`` / ``libgl1`` (Mesa's libglvnd
EGL/GLES/GL) + ``libvulkan1`` (the Khronos loader), so the DT_NEEDED-minimal closure
pulls Mesa's ``libEGL.so.1`` / ``libGLESv2.so.2`` / ``libGL.so.1`` / ``libGLX.so.0`` /
``libGLdispatch.so.0`` + a second ``libvulkan.so.1`` into the merged root. If those
shipped in the overlay they would land in ``/usr/lib/aarch64-linux-gnu`` and — because
es2gears resolves DT_NEEDED by filename — could be picked up as the GL/EGL provider,
giving Mesa **software** GL (llvmpipe / swrast) and defeating the whole point. The base
rootfs ships NONE of these SONAMEs, so ``deb_closure``'s base-subtraction does NOT drop
them; we therefore STRIP them here explicitly (``MESA_GL_STRIP_PREFIXES``):

  * ``libEGL.so*`` / ``libGLESv2.so*``   — ANGLE owns these (androlinux-angle, FIRST
    on LD_LIBRARY_PATH); es2gears's NEEDED resolves to ANGLE.
  * ``libGL.so*`` / ``libGLX.so*`` / ``libGLdispatch.so*`` — desktop-GL glvnd; only the
    glx*/eglgears demos link them, NOT es2gears. Dropping them keeps Mesa swrast out.
  * ``libvulkan.so*`` — the Khronos Vulkan-Loader is owned by the **vk-loader** overlay
    (``/usr/lib/androlinux/libvulkan.so.1`` + ``alr_icd.json`` → our Mali ICD). A second
    Mesa loader here would shadow it.

What the overlay KEEPS (the genuinely new, non-software libs the base lacks):
  * ``libdecor-0.so.0``        — es2gears_wayland's window-decoration lib (NEEDED).
  * ``libxkbcommon-x11.so.0``  — keymap-from-X helper pulled by the demos; base lacks.
  * ``libxcb-xkb.so.1``        — its xcb dep; base lacks.
(All the rest — libwayland-*, libxkbcommon, libX11, libxcb, libm, libc, ld — the base
already ships, so base-subtraction drops them.)

§5-E conformance
----------------
``./``-rooted flat-SONAME tar built by ``deb_closure.build_minimal_overlay`` (leaf
files kept entirely + DT_NEEDED libs the base lacks, base-subtracted by SONAME *and*
path), then the Mesa-GL strip, then the unsuffixed es2gears symlinks are added, then
every ``.so`` is forced to 0o755 (ALR file-backed PROT_EXEC dlopen under untrusted_app
rejects a non-x .so — §10.1) and the tar is validated with overlay_guard +
stage_tar_spec. NETWORK build (ports.ubuntu.com noble main+universe); ``--selftest``
is OFFLINE.

This module reuses, and does NOT duplicate:
  * tools.deb_closure.build_minimal_overlay  — the DT_NEEDED-minimal §5-E engine
  * tools.build_gui_demo_overlays.force_so_executable / overlay_has_exec — the .so
    x-bit enforcement + the exec-present check (same contract as the GUI demos)
  * tools.overlay_guard / tools.stage_tar_spec — apply-time + structural gates

Usage
-----
  # NETWORK: fetch mesa-utils-bin + libdecor closure from noble, stage es2gears
  python -m tools.build_es2gears_overlay \
      --base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar \
      --out out/v2-stage/es2gears-stage.tar
  python -m tools.build_es2gears_overlay --selftest   # OFFLINE shape self-test
"""
from __future__ import annotations

import argparse
import io
import sys
import tarfile
from pathlib import Path, PurePosixPath

from tools.deb_closure import build_minimal_overlay
from tools.build_gui_demo_overlays import force_so_executable, overlay_has_exec

DEFAULT_MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
DEFAULT_SUITE = "noble"
DEFAULT_COMPONENTS = ("main", "universe")
DEFAULT_CACHE = "/tmp/deb-cache-ubuntu"
DEFAULT_OUT = "out/v2-stage/es2gears-stage.tar"
DEFAULT_BASE = "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"

# The leaf package that ships the es2gears ELF(s) (mesa-demos' native binaries).
LEAF_PACKAGE = "mesa-utils-bin"

# The Debian/Ubuntu multiarch-suffixed es2gears ELFs inside the .deb.
ES2GEARS_WAYLAND = "usr/bin/es2gears_wayland.aarch64-linux-gnu"
ES2GEARS_X11 = "usr/bin/es2gears_x11.aarch64-linux-gnu"

# Unsuffixed convenience names we add as in-dir symlinks so the launch marker can use a
# stable path. The bare ``es2gears`` points at the WAYLAND flavour (the on-screen target
# under the ALR_ANGLE env: DISPLAY unset + XDG_SESSION_TYPE=wayland → ANGLE's Wayland WSI).
ES2GEARS_SYMLINKS = {
    "usr/bin/es2gears_wayland": "es2gears_wayland.aarch64-linux-gnu",
    "usr/bin/es2gears_x11": "es2gears_x11.aarch64-linux-gnu",
    "usr/bin/es2gears": "es2gears_wayland.aarch64-linux-gnu",
}

# The canonical app path the marker / launcher names (the bare wayland symlink).
ES2GEARS_LAUNCH_PATH = "/usr/bin/es2gears_wayland"

# Mesa GL/EGL/Vulkan libs to STRIP from the overlay so ANGLE (androlinux-angle) + the
# Khronos vk-loader own those SONAMEs. Matched on the member BASENAME by these prefixes
# (covers ``libEGL.so``, ``libEGL.so.1``, ``libEGL.so.1.1.0`` — real, SONAME-link and
# dev-link spellings). es2gears links only libEGL/libGLESv2 (→ ANGLE); the desktop-GL
# glvnd trio + the second libvulkan are pulled by sibling demos / Depends, never es2gears.
MESA_GL_STRIP_PREFIXES = (
    "libEGL.so",
    "libGLESv2.so",
    "libGL.so",
    "libGLX.so",
    "libGLdispatch.so",
    "libOpenGL.so",
    "libvulkan.so",
)


# --------------------------------------------------------------------------- #
# Mesa-GL strip + es2gears symlinks (applied to the produced tar)
# --------------------------------------------------------------------------- #

def _basename_is_stripped(name: str) -> bool:
    """True if the tar member ``name``'s basename is a Mesa GL/EGL/Vulkan lib to drop."""
    base = PurePosixPath(name).name
    return any(base.startswith(p) for p in MESA_GL_STRIP_PREFIXES)


def strip_mesa_gl(tar_path: str | Path) -> list[str]:
    """Rewrite ``tar_path`` in place, REMOVING every Mesa GL/EGL/Vulkan lib member
    (``MESA_GL_STRIP_PREFIXES``) so ANGLE's libEGL/libGLESv2 (private androlinux-angle
    dir, first on LD_LIBRARY_PATH) and the Khronos vk-loader's libvulkan win at runtime.

    Returns the sorted list of removed ``./``-rooted member names (deterministic).
    """
    tar_path = Path(tar_path)
    keep: list[tuple[tarfile.TarInfo, bytes | None]] = []
    removed: list[str] = []
    with tarfile.open(tar_path, "r:*") as tar:
        for ti in tar.getmembers():
            if (ti.isreg() or ti.issym() or ti.islnk()) and _basename_is_stripped(ti.name):
                removed.append(ti.name)
                continue
            data = tar.extractfile(ti).read() if ti.isreg() else None
            keep.append((ti, data))
    with tarfile.open(tar_path, "w") as tar:
        for ti, data in keep:
            if data is not None:
                tar.addfile(ti, io.BytesIO(data))
            else:
                tar.addfile(ti)
    removed.sort()
    return removed


def add_es2gears_symlinks(tar_path: str | Path) -> list[str]:
    """Append the unsuffixed es2gears convenience symlinks to ``tar_path`` if absent.

    Each is a RELATIVE, in-dir symlink (§5-E safe-symlink rule) to the multiarch-suffixed
    real ELF in the SAME ``/usr/bin`` directory, so the launch marker can name a stable
    path. No-op for any symlink already present (idempotent). Returns the members added.
    """
    tar_path = Path(tar_path)
    with tarfile.open(tar_path, "r:*") as tar:
        have = set(tar.getnames())
    to_add = {
        rel: tgt
        for rel, tgt in ES2GEARS_SYMLINKS.items()
        if ("./" + rel) not in have
    }
    if not to_add:
        return []
    added: list[str] = []
    with tarfile.open(tar_path, "a") as tar:
        for rel, tgt in to_add.items():
            ti = tarfile.TarInfo("./" + rel)
            ti.type = tarfile.SYMTYPE
            ti.linkname = tgt  # RELATIVE, in-dir
            ti.mode = 0o777
            ti.mtime = 0
            tar.addfile(ti)
            added.append("./" + rel)
    return added


def overlay_member_basenames(tar_path: str | Path) -> set[str]:
    """The set of member basenames in ``tar_path`` (for strip/keep assertions)."""
    with tarfile.open(tar_path, "r:*") as tar:
        return {PurePosixPath(n).name for n in tar.getnames()}


# --------------------------------------------------------------------------- #
# Build (network)
# --------------------------------------------------------------------------- #

def build_es2gears_overlay(
    base: str | Path,
    out_tar: str | Path,
    *,
    mirror: str = DEFAULT_MIRROR,
    suite: str = DEFAULT_SUITE,
    arch: str = "arm64",
    components=DEFAULT_COMPONENTS,
    cache_dir: str | Path | None = DEFAULT_CACHE,
) -> dict:
    """Build the §5-E DT_NEEDED-minimal es2gears overlay (NETWORK).

    1. ``build_minimal_overlay([mesa-utils-bin])`` — keep the demo ELFs entirely + the
       DT_NEEDED libs the base lacks, base-subtracted and flattened.
    2. STRIP Mesa's libEGL/libGLESv2/libGL/libGLX/libGLdispatch/libvulkan so ANGLE +
       the Khronos vk-loader own those SONAMEs (the software-GL defeater).
    3. add the unsuffixed es2gears symlinks (stable launch path).
    4. force every ``.so`` to 0o755 (ALR PROT_EXEC dlopen) and validate.

    Returns a summary dict (out_tar, file_count, kept/stripped libs, exec-present,
    overlay_guard + stage_tar_spec verdicts).
    """
    m = build_minimal_overlay(
        [LEAF_PACKAGE],
        base,
        out_tar,
        mirror=mirror,
        suite=suite,
        arch=arch,
        components=components,
        cache_dir=cache_dir,
    )
    out = m["out_tar"]

    stripped = strip_mesa_gl(out)
    symlinks = add_es2gears_symlinks(out)
    # Force every remaining dlopen'd/linked .so (libdecor, libxkbcommon-x11, …) to 0o755.
    so_made_exec = force_so_executable(out)

    from tools.overlay_guard import scan_overlay_violations, BLOCK
    from tools.stage_tar_spec import validate_stage_tar

    blocks = [
        v.render()
        for v in scan_overlay_violations(str(base), out)
        if v.severity == BLOCK
    ]
    rep = validate_stage_tar(out, base=base)

    basenames = overlay_member_basenames(out)
    names = _names(out)
    # Post-strip assertions: no Mesa GL/EGL/Vulkan member survived; the wayland ELF + its
    # base-missing libdecor are present; the bare launch symlink resolves.
    mesa_leftover = sorted(b for b in basenames if any(b.startswith(p) for p in MESA_GL_STRIP_PREFIXES))
    have_wayland_elf = overlay_has_exec(out, ES2GEARS_WAYLAND)
    have_x11_elf = overlay_has_exec(out, ES2GEARS_X11)
    have_libdecor = any(b.startswith("libdecor-0.so") for b in basenames)
    have_launch_symlink = ("./" + ES2GEARS_LAUNCH_PATH.lstrip("/")) in names

    return {
        "out_tar": out,
        "file_count": m["file_count"],
        "reachable_libs": m["reachable_libs"],
        "missing_soname": m["missing_soname"],
        "stripped_mesa_gl": stripped,
        "symlinks_added": symlinks,
        "so_made_exec": so_made_exec,
        "mesa_leftover": mesa_leftover,
        "have_wayland_elf": have_wayland_elf,
        "have_x11_elf": have_x11_elf,
        "have_libdecor": have_libdecor,
        "have_launch_symlink": have_launch_symlink,
        "violations": list(m["violations"]) + blocks,
        "conformant": rep.conformant,
    }


def _names(tar_path: str | Path) -> set[str]:
    with tarfile.open(tar_path, "r:*") as tar:
        return set(tar.getnames())


def _ok(summary: dict) -> bool:
    """Overall PASS: exec present, libdecor kept, no Mesa-GL leftover, no missing soname,
    launch symlink resolves, no guard violation, §5-E conformant."""
    return (
        summary["have_wayland_elf"]
        and summary["have_x11_elf"]
        and summary["have_libdecor"]
        and not summary["mesa_leftover"]
        and not summary["missing_soname"]
        and summary["have_launch_symlink"]
        and not summary["violations"]
        and summary["conformant"]
    )


# --------------------------------------------------------------------------- #
# Offline self-test (synthetic tar; no toolchain, no network)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- constants ---------------------------------------------------------
    check("leaf package is mesa-utils-bin", LEAF_PACKAGE == "mesa-utils-bin")
    check("es2gears wayland ELF path is the multiarch-suffixed binary",
          ES2GEARS_WAYLAND.endswith("es2gears_wayland.aarch64-linux-gnu"))
    check("bare es2gears symlink targets the WAYLAND flavour",
          ES2GEARS_SYMLINKS["usr/bin/es2gears"] == "es2gears_wayland.aarch64-linux-gnu")
    check("launch path is the bare wayland symlink",
          ES2GEARS_LAUNCH_PATH == "/usr/bin/es2gears_wayland")
    check("Mesa-GL strip covers libEGL/libGLESv2/libGL/libGLX/libGLdispatch/libvulkan",
          {"libEGL.so", "libGLESv2.so", "libGL.so", "libGLX.so",
           "libGLdispatch.so", "libvulkan.so"} <= set(MESA_GL_STRIP_PREFIXES))

    # --- _basename_is_stripped ---------------------------------------------
    for name, want in (
        ("./usr/lib/aarch64-linux-gnu/libEGL.so.1", True),
        ("./usr/lib/aarch64-linux-gnu/libEGL.so.1.1.0", True),
        ("./usr/lib/aarch64-linux-gnu/libGLESv2.so.2", True),
        ("./usr/lib/aarch64-linux-gnu/libGL.so.1", True),
        ("./usr/lib/aarch64-linux-gnu/libGLX.so.0", True),
        ("./usr/lib/aarch64-linux-gnu/libGLdispatch.so.0", True),
        ("./usr/lib/aarch64-linux-gnu/libvulkan.so.1", True),
        ("./usr/lib/aarch64-linux-gnu/libvulkan.so.1.3.275", True),
        ("./usr/lib/aarch64-linux-gnu/libdecor-0.so.0", False),   # KEEP
        ("./usr/lib/aarch64-linux-gnu/libxkbcommon-x11.so.0", False),  # KEEP
        ("./usr/bin/es2gears_wayland.aarch64-linux-gnu", False),  # KEEP
    ):
        check(f"strip predicate: {PurePosixPath(name).name} -> {'STRIP' if want else 'keep'}",
              _basename_is_stripped(name) is want)

    # --- strip_mesa_gl + add_es2gears_symlinks + force_so_executable on a synthetic tar
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "es2gears-stage.tar"
        # Synthesize a build_minimal_overlay-shaped overlay: the demo ELFs (suffixed),
        # the Mesa GL trio (to be stripped), and the base-missing libs to keep.
        members = {
            "./usr/bin/es2gears_wayland.aarch64-linux-gnu": (0o755, b"\x7fELFwl"),
            "./usr/bin/es2gears_x11.aarch64-linux-gnu": (0o755, b"\x7fELFx11"),
            "./usr/bin/glxgears.aarch64-linux-gnu": (0o755, b"\x7fELFglx"),
            "./usr/lib/aarch64-linux-gnu/libEGL.so.1": (0o644, b"\x7fELF-mesa-egl"),
            "./usr/lib/aarch64-linux-gnu/libGLESv2.so.2": (0o644, b"\x7fELF-mesa-gles"),
            "./usr/lib/aarch64-linux-gnu/libGL.so.1": (0o644, b"\x7fELF-mesa-gl"),
            "./usr/lib/aarch64-linux-gnu/libGLX.so.0": (0o644, b"\x7fELF-mesa-glx"),
            "./usr/lib/aarch64-linux-gnu/libGLdispatch.so.0": (0o644, b"\x7fELF-mesa-disp"),
            "./usr/lib/aarch64-linux-gnu/libvulkan.so.1": (0o644, b"\x7fELF-mesa-vk"),
            "./usr/lib/aarch64-linux-gnu/libdecor-0.so.0": (0o644, b"\x7fELF-libdecor"),
            "./usr/lib/aarch64-linux-gnu/libxkbcommon-x11.so.0": (0o644, b"\x7fELF-xkbx11"),
        }
        with tarfile.open(out, "w") as tar:
            for name, (mode, data) in members.items():
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                ti.mode = mode
                ti.mtime = 0
                tar.addfile(ti, io.BytesIO(data))

        stripped = strip_mesa_gl(out)
        names_after = _names(out)
        check("strip removed Mesa libEGL.so.1",
              "./usr/lib/aarch64-linux-gnu/libEGL.so.1" in stripped
              and "./usr/lib/aarch64-linux-gnu/libEGL.so.1" not in names_after)
        check("strip removed Mesa libGLESv2.so.2",
              "./usr/lib/aarch64-linux-gnu/libGLESv2.so.2" not in names_after)
        check("strip removed Mesa libGL/libGLX/libGLdispatch",
              not any(b in names_after for b in (
                  "./usr/lib/aarch64-linux-gnu/libGL.so.1",
                  "./usr/lib/aarch64-linux-gnu/libGLX.so.0",
                  "./usr/lib/aarch64-linux-gnu/libGLdispatch.so.0")))
        check("strip removed the second (Mesa) libvulkan.so.1",
              "./usr/lib/aarch64-linux-gnu/libvulkan.so.1" not in names_after)
        check("strip KEPT the es2gears wayland ELF",
              "./usr/bin/es2gears_wayland.aarch64-linux-gnu" in names_after)
        check("strip KEPT the es2gears x11 ELF",
              "./usr/bin/es2gears_x11.aarch64-linux-gnu" in names_after)
        check("strip KEPT base-missing libdecor-0.so.0",
              "./usr/lib/aarch64-linux-gnu/libdecor-0.so.0" in names_after)
        check("strip KEPT base-missing libxkbcommon-x11.so.0",
              "./usr/lib/aarch64-linux-gnu/libxkbcommon-x11.so.0" in names_after)
        check("strip preserved es2gears ELF bytes",
              _read(out, "./usr/bin/es2gears_wayland.aarch64-linux-gnu") == b"\x7fELFwl")

        added = add_es2gears_symlinks(out)
        names_sym = _names(out)
        check("symlink ./usr/bin/es2gears added (bare convenience name)",
              "./usr/bin/es2gears" in added and "./usr/bin/es2gears" in names_sym)
        check("symlink ./usr/bin/es2gears_wayland added",
              "./usr/bin/es2gears_wayland" in names_sym)
        with tarfile.open(out, "r:*") as t:
            sl = t.getmember("./usr/bin/es2gears")
            check("bare es2gears is a RELATIVE in-dir symlink to the wayland ELF",
                  sl.issym() and sl.linkname == "es2gears_wayland.aarch64-linux-gnu"
                  and "/" not in sl.linkname)
            slw = t.getmember("./usr/bin/es2gears_wayland")
            check("es2gears_wayland symlink targets the suffixed wayland ELF",
                  slw.issym() and slw.linkname == "es2gears_wayland.aarch64-linux-gnu")
        check("add_es2gears_symlinks idempotent (no dup on 2nd pass)",
              add_es2gears_symlinks(out) == [])

        made = force_so_executable(out)
        with tarfile.open(out, "r:*") as t:
            modes = {m.name: (m.mode & 0o777) for m in t.getmembers() if m.isreg()}
        check("force_so_executable raised libdecor + libxkbcommon-x11 (2)", made == 2)
        check("libdecor-0.so.0 now 0o755",
              modes["./usr/lib/aarch64-linux-gnu/libdecor-0.so.0"] == 0o755)
        check("es2gears ELF stays executable",
              modes["./usr/bin/es2gears_wayland.aarch64-linux-gnu"] == 0o755)

        # No Mesa-GL leftover after the full pipeline.
        basenames = overlay_member_basenames(out)
        check("no Mesa GL/EGL/Vulkan member survives the pipeline",
              not any(any(b.startswith(p) for p in MESA_GL_STRIP_PREFIXES) for b in basenames))
        check("overlay_has_exec finds the suffixed wayland ELF",
              overlay_has_exec(out, ES2GEARS_WAYLAND))

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def _read(tar_path: str | Path, member: str) -> bytes:
    with tarfile.open(tar_path, "r:*") as tar:
        return tar.extractfile(member).read()


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Build the es2gears-stage.tar overlay (real GLES2 demo for the "
        "ANGLE→Vulkan→Mali proof) from Ubuntu noble."
    )
    ap.add_argument("--out", default=DEFAULT_OUT, help=f"output overlay tar (default {DEFAULT_OUT})")
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help="base rootfs (dir|tar) for soname/path subtract + the guard")
    ap.add_argument("--mirror", default=DEFAULT_MIRROR)
    ap.add_argument("--suite", default=DEFAULT_SUITE)
    ap.add_argument("--arch", default="arm64")
    ap.add_argument("--component", action="append", dest="components")
    ap.add_argument("--cache", default=DEFAULT_CACHE)
    ap.add_argument("--selftest", action="store_true", help="run the OFFLINE shape self-test")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    components = tuple(args.components) if args.components else DEFAULT_COMPONENTS
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    s = build_es2gears_overlay(
        args.base, out,
        mirror=args.mirror, suite=args.suite, arch=args.arch,
        components=components, cache_dir=args.cache,
    )
    print(f"wrote {s['out_tar']}")
    print(f"  files:            {s['file_count']}")
    print(f"  es2gears wayland: {'YES' if s['have_wayland_elf'] else 'NO — MISSING'} ({ES2GEARS_WAYLAND})")
    print(f"  es2gears x11:     {'YES' if s['have_x11_elf'] else 'NO — MISSING'}")
    print(f"  launch symlink:   {'YES ' + ES2GEARS_LAUNCH_PATH if s['have_launch_symlink'] else 'NO — MISSING'}")
    print(f"  libdecor kept:    {'YES' if s['have_libdecor'] else 'NO — MISSING (es2gears_wayland needs it)'}")
    print(f"  reachable libs:   {len(s['reachable_libs'])} {list(s['reachable_libs'])}")
    print(f"  Mesa GL stripped: {len(s['stripped_mesa_gl'])} {s['stripped_mesa_gl']}")
    print(f"  symlinks added:   {s['symlinks_added']}")
    print(f"  .so forced 0o755: {s['so_made_exec']}")
    if s["mesa_leftover"]:
        print(f"  !! Mesa GL LEFTOVER: {s['mesa_leftover']}")
    if s["missing_soname"]:
        print(f"  MISSING sonames:  {s['missing_soname']}")
    print(f"  overlay_guard:    {'OK' if not s['violations'] else str(len(s['violations'])) + ' violation(s)'}")
    for v in s["violations"]:
        print(f"    {v}")
    print(f"  stage_tar_spec:   {'CONFORMANT' if s['conformant'] else 'NON-CONFORMANT'}")
    ok = _ok(s)
    print(f"  => {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
