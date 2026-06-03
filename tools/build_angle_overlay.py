#!/usr/bin/env python3
"""build_angle_overlay.py — assemble the ``angle-stage.tar`` overlay carrying a
SYSTEM **ANGLE** (libEGL.so.1 + libGLESv2.so.2, Vulkan backend) into
``/usr/lib/androlinux-angle`` (a PRIVATE dir, kept separate from the shared
``/usr/lib/androlinux`` so it never clobbers the device-proven gpushim GLES libs)
so GLES/EGL guests get GL → Vulkan → our guest VK ICD → Mali, instead of falling
back to software.

Why ANGLE, and why these exact bytes
-------------------------------------
DECIDED strategy: Vulkan-first marshalling + **ANGLE-for-GLES**. A glibc-arm64
ANGLE in the rootfs translates GL/GLES → Vulkan; ANGLE's Vulkan backend loads a
Vulkan ICD via the standard loader → our guest ``libvulkan.so.1`` (staged by
``tools/build_vk_icd_overlay.py``) → real Mali. The base rootfs ships **no GL
stack** (verified: no libGL/EGL/GLES/mesa/glvnd/libvulkan in tiny-rootfs.tar), so
a system ANGLE is a CLEAN direct-SONAME drop with ZERO base conflict.

ANGLE SOURCE — extracted from the Debian **chromium-common** .deb
-----------------------------------------------------------------
chromium's bundled ``libEGL.so`` / ``libGLESv2.so`` **ARE** ANGLE (Chrome builds
ANGLE as its GL implementation). For Debian bookworm chromium 147 they live in
the **chromium-common** .deb under ``/usr/lib/chromium/`` (NOT the main chromium
.deb, which on bookworm ships only the ``chromium`` ELF). Audit of the extracted
bytes (``readelf``/strings):

  libGLESv2.so : AArch64 ELF, contains ``third_party/angle/src/libANGLE/…`` and
                 ``…/common/vulkan/vulkan_icd.cpp`` → ANGLE **with the Vulkan
                 backend**; dlopens ``libvulkan.so.1`` (then ``libvulkan.so``) at
                 runtime (NOT DT_NEEDED). SONAME ``libGLESv2.so``. Max glibc
                 symver 2.34 (base noble is 2.39 → backward-compatible).
                 DT_NEEDED: libz, libX11, **libXNVCtrl.so.0**, libxcb, libm,
                 libgcc_s, libc, ld-linux — all base-provided EXCEPT libXNVCtrl.
  libEGL.so    : AArch64 ELF, SONAME ``libEGL.so``; DT_NEEDED libgcc_s/libc/
                 ld-linux (all base-provided). dlopens ``libGLESv2`` (→
                 ``libGLESv2.so.2`` on Linux) at runtime. Exports eglInitialize/
                 eglGetDisplay/eglCreateContext/eglGetPlatformDisplay (the
                 first-rung proof entrypoints).

The single base-missing DT_NEEDED, ``libXNVCtrl.so.0`` (NVIDIA-Control X
extension — never actually *called* on Mali, but DT_NEEDED is resolved eagerly by
ld.so), is the real 67 KiB library from the bookworm **libxnvctrl0** .deb,
shipped **flat** (so no base downgrade, no §5-E non-flat warning).

Overlay layout (mirrors the gpushim/vk-icd staging — README + build_vk_icd_overlay)
-----------------------------------------------------------------------------------
``./``-rooted, relative paths only; .so real files mode **0755** (the ALR
file-backed PROT_EXEC requirement under untrusted_app — §10.1); unversioned
symlinks relative + in-dir:

  ./usr/lib/androlinux-angle/libGLESv2.so.2   (real ANGLE GLESv2, 0755)
  ./usr/lib/androlinux-angle/libGLESv2.so     -> libGLESv2.so.2
  ./usr/lib/androlinux-angle/libEGL.so.1      (real ANGLE EGL, 0755)
  ./usr/lib/androlinux-angle/libEGL.so        -> libEGL.so.1
  ./usr/lib/androlinux-angle/libXNVCtrl.so.0  (real, flat, 0755 — base-missing DT_NEEDED)

WHY both versioned + unversioned names (load-bearing, not just dev convenience):
  * a GLES app links ``-lEGL -lGLESv2`` → DT_NEEDED records the standard Linux
    SONAMEs ``libEGL.so.1`` / ``libGLESv2.so.2``; ld.so resolves DT_NEEDED by the
    *filename* on LD_LIBRARY_PATH (the .so's own internal SONAME, here the
    unversioned ``libEGL.so``, is only used for the soname cache) — so placing
    the ANGLE bytes at ``libEGL.so.1`` / ``libGLESv2.so.2`` satisfies a normal
    GLES app's NEEDED.
  * ANGLE's libEGL dlopens GLESv2 by the platform name ``libGLESv2.so.2`` (real
    file ✓); ANGLE's Vulkan backend dlopens ``libvulkan.so.1`` then ``libvulkan.so``
    (provided by the vk-icd overlay). The unversioned ``libEGL.so``/``libGLESv2.so``
    symlinks cover any tool that asks for the bare names.

Loader wiring (see runtime_report.cpp)
--------------------------------------
The launcher opts a GLES/ANGLE guest into the Mali path with **ALR_ANGLE=1**
(prepends ``/usr/lib/androlinux-angle`` AHEAD of ``/usr/lib/androlinux`` on
LD_LIBRARY_PATH so ANGLE's libEGL.so.1/libGLESv2.so.2 win over gpushim's) **plus
ALR_VK_ICD=1** (attaches the VK request/reply rings AND sets ``VK_ICD_FILENAMES``
→ our ``alr_icd.json`` so ANGLE's Vulkan backend binds our guest
``libvulkan.so.1`` ICD, which lives in the shared ``/usr/lib/androlinux`` dir and
is found next on the path). ``ALR_GPU_ACCEL=1`` additionally attaches the GLES
Mali ring (only relevant to the gpushim path, harmless here). This overlay just
supplies the ANGLE bytes that wiring expects, in the private dir.

Usage
-----
  # NETWORK: fetch chromium-common + libxnvctrl0 from the bookworm mirror, extract
  python -m tools.build_angle_overlay --out out/v2-stage/angle-stage.tar
  # OFFLINE: consume pre-extracted bytes (no .deb download)
  python -m tools.build_angle_overlay --out angle-stage.tar \
      --so-egl libEGL.so --so-gles libGLESv2.so --so-nvctrl libXNVCtrl.so.0.0.0
  python -m tools.build_angle_overlay --selftest    # offline shape self-test
"""
from __future__ import annotations

import argparse
import io
import sys
import tarfile
import tempfile
from pathlib import Path

# Reuse, do NOT duplicate, the .deb fetch/crack + §5-E validation primitives.
from tools.deb_closure import (
    _download_deb,
    fetch_packages_index,
    parse_packages,
)
from tools.build_stage_tar import extract_deb
from tools.stage_tar_spec import validate_stage_tar

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

# ANGLE ships into its OWN private dir, NOT the shared /usr/lib/androlinux, so it can
# NEVER clobber the device-proven gpushim libEGL.so.1/libGLESv2.so.2 (last-writer-wins
# was the old collision). The loader prepends THIS dir ahead of /usr/lib/androlinux on
# LD_LIBRARY_PATH only under ALR_ANGLE=1, so ANGLE's EGL/GLES resolve first while its
# runtime dlopen of libvulkan.so.1 still finds our VK ICD in /usr/lib/androlinux (next
# on the path). With ALR_ANGLE unset this dir is simply never on the path → gpushim is
# byte-for-byte the proven path (strict no-regression).
ANDROLINUX_DIR = "usr/lib/androlinux-angle"

# Versioned SONAMEs a normal GLES app's DT_NEEDED records (resolved by filename on
# LD_LIBRARY_PATH); the ANGLE bytes are placed here as the real files.
EGL_SONAME = "libEGL.so.1"
GLES_SONAME = "libGLESv2.so.2"
EGL_UNVERSIONED = "libEGL.so"
GLES_UNVERSIONED = "libGLESv2.so"

# The base-missing DT_NEEDED of ANGLE's libGLESv2 — shipped flat at the bare SONAME.
NVCTRL_SONAME = "libXNVCtrl.so.0"

# Where the ANGLE bytes come from: Debian bookworm chromium-common (ANGLE is
# chromium's bundled libEGL/libGLESv2). The base is Ubuntu noble (glibc 2.39) but
# Ubuntu ships chromium as a snap, so we take Debian's .deb; ANGLE's max glibc
# symver is 2.34, which the noble 2.39 runtime satisfies (newer libc, older lib).
MIRROR = "http://deb.debian.org/debian"
SUITE = "bookworm"
ARCH = "arm64"
COMPONENTS = ("main",)

ANGLE_PACKAGE = "chromium-common"
# Filenames of the ANGLE .so inside the chromium-common .deb (under /usr/lib/chromium/).
ANGLE_DEB_EGL = "libEGL.so"
ANGLE_DEB_GLES = "libGLESv2.so"

# The base-missing DT_NEEDED lib comes from libxnvctrl0; we ship its real .so flat.
NVCTRL_PACKAGE = "libxnvctrl0"
NVCTRL_DEB_REAL = "libXNVCtrl.so.0.0.0"  # the versioned real file inside the .deb

DEFAULT_OUT = "out/v2-stage/angle-stage.tar"
DEFAULT_CACHE = "/tmp/deb-cache-bookworm"

# tiny-rootfs (the base) for the optional downgrade guard.
DEFAULT_BASE = "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"


# --------------------------------------------------------------------------- #
# Fetch (network) — extract the ANGLE + libXNVCtrl bytes from bookworm .debs
# --------------------------------------------------------------------------- #

def _deb_filename(pkg: str, *, opener=None) -> str:
    """Look up a package's pool Filename in the bookworm arm64 index."""
    kw = {} if opener is None else {"opener": opener}
    idx = parse_packages(
        fetch_packages_index(MIRROR, SUITE, ARCH, components=COMPONENTS, **kw)
    )
    fields = idx.get(pkg)
    if not fields or not fields.get("Filename"):
        raise SystemExit(f"{pkg} not found in {SUITE}/{ARCH} index")
    return fields["Filename"]


def _extract_named(deb: Path, names: dict[str, str], dest: Path) -> dict[str, bytes]:
    """Crack ``deb`` and return {key: bytes} for each requested basename in ``names``
    ({key: basename}). The first matching real file (not symlink) wins."""
    out: dict[str, bytes] = {}
    root = extract_deb(deb, dest)
    for key, basename in names.items():
        for p in Path(root).rglob(basename):
            if p.is_file() and not p.is_symlink():
                out[key] = p.read_bytes()
                break
        if key not in out:
            raise SystemExit(f"{basename} not found in {deb.name}")
    return out


def fetch_angle_bytes(cache: Path, *, opener=None) -> dict[str, bytes]:
    """NETWORK: download chromium-common + libxnvctrl0 and return the three blobs
    keyed 'egl', 'gles', 'nvctrl'."""
    cache.mkdir(parents=True, exist_ok=True)
    dl = {} if opener is None else {"opener": opener}

    angle_deb = _download_deb(MIRROR, _deb_filename(ANGLE_PACKAGE, opener=opener), cache, **dl)
    nvctrl_deb = _download_deb(MIRROR, _deb_filename(NVCTRL_PACKAGE, opener=opener), cache, **dl)

    blobs: dict[str, bytes] = {}
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        blobs.update(
            _extract_named(angle_deb, {"egl": ANGLE_DEB_EGL, "gles": ANGLE_DEB_GLES}, tdp / "angle")
        )
        blobs.update(
            _extract_named(nvctrl_deb, {"nvctrl": NVCTRL_DEB_REAL}, tdp / "nvctrl")
        )
    return blobs


# --------------------------------------------------------------------------- #
# Overlay assembly
# --------------------------------------------------------------------------- #

def build_overlay(
    out_tar: str | Path,
    *,
    egl_bytes: bytes,
    gles_bytes: bytes,
    nvctrl_bytes: bytes,
    cube_bytes: bytes | None = None,
    vk_client_bytes: bytes | None = None,
) -> dict:
    """Write the angle overlay tar from the three provided blobs. Returns a summary.

    ``cube_bytes`` (optional): the ``alr-gles-cube`` test client (an ordinary glibc
    GLES2 binary, DT_NEEDED libEGL.so/libGLESv2.so). Staged at ``/usr/bin/alr-gles-cube``
    so the loader can run it under ALR_ANGLE=1 as the minimal ANGLE init+clear+present
    proof (eglInitialize → eglCreateContext → glClear → eglSwapBuffers). When the cube
    runs, ANGLE's EGL/GLES resolve from androlinux-angle (first on LD_LIBRARY_PATH).

    ``vk_client_bytes`` (optional): the ``alr-angle-vk`` client (alr-angle-vk.c) which
    EXPLICITLY drives ANGLE's Vulkan backend via eglGetPlatformDisplayEXT(VULKAN_ANGLE) +
    a headless pbuffer (no X/Wayland), so ANGLE → our libvulkan.so.1 ICD → Mali. Staged at
    ``/usr/bin/alr-angle-vk``. This is the actual ANGLE-on-Vulkan device proof (the cube's
    eglGetDisplay path makes ANGLE pick X11 → "Could not open the default X display")."""
    members: list[tuple[str, str, object]] = []
    with tarfile.open(out_tar, "w") as tar:

        def add_file(rel: str, data: bytes, mode: int) -> None:
            ti = tarfile.TarInfo("./" + rel)
            ti.size = len(data)
            ti.mode = mode
            ti.type = tarfile.REGTYPE
            tar.addfile(ti, io.BytesIO(data))
            members.append(("file", "./" + rel, mode))

        def add_symlink(rel: str, target: str) -> None:
            ti = tarfile.TarInfo("./" + rel)
            ti.type = tarfile.SYMTYPE
            ti.linkname = target  # RELATIVE, in-dir (§5-E safe-symlink rule)
            ti.mode = 0o777
            tar.addfile(ti)
            members.append(("symlink", "./" + rel, target))

        # ANGLE real .so at the versioned SONAMEs, mode 0755 (ALR exec-bit, §10.1).
        add_file(f"{ANDROLINUX_DIR}/{GLES_SONAME}", gles_bytes, 0o755)
        add_file(f"{ANDROLINUX_DIR}/{EGL_SONAME}", egl_bytes, 0o755)
        # Unversioned dev/runtime symlinks (relative, in-dir).
        add_symlink(f"{ANDROLINUX_DIR}/{GLES_UNVERSIONED}", GLES_SONAME)
        add_symlink(f"{ANDROLINUX_DIR}/{EGL_UNVERSIONED}", EGL_SONAME)
        # The base-missing DT_NEEDED lib, shipped flat at the bare SONAME (0755).
        add_file(f"{ANDROLINUX_DIR}/{NVCTRL_SONAME}", nvctrl_bytes, 0o755)
        # The optional ANGLE init+clear+present test clients at /usr/bin (0755, exec-bit).
        if cube_bytes is not None:
            add_file("usr/bin/alr-gles-cube", cube_bytes, 0o755)
        if vk_client_bytes is not None:
            add_file("usr/bin/alr-angle-vk", vk_client_bytes, 0o755)

    return {
        "out": str(out_tar),
        "egl_bytes": len(egl_bytes),
        "gles_bytes": len(gles_bytes),
        "nvctrl_bytes": len(nvctrl_bytes),
        "cube_bytes": (len(cube_bytes) if cube_bytes is not None else 0),
        "vk_client_bytes": (len(vk_client_bytes) if vk_client_bytes is not None else 0),
        "members": members,
    }


def build_from_mirror(out_tar: str | Path, cache: Path, *, opener=None,
                      cube_bytes: bytes | None = None,
                      vk_client_bytes: bytes | None = None) -> dict:
    """NETWORK path: fetch the three blobs, then assemble the overlay."""
    blobs = fetch_angle_bytes(cache, opener=opener)
    return build_overlay(
        out_tar,
        egl_bytes=blobs["egl"],
        gles_bytes=blobs["gles"],
        nvctrl_bytes=blobs["nvctrl"],
        cube_bytes=cube_bytes,
        vk_client_bytes=vk_client_bytes,
    )


# --------------------------------------------------------------------------- #
# Validation
# --------------------------------------------------------------------------- #

def validate(out_tar: str | Path, base: str | Path | None) -> bool:
    """Run the §5-E structural + downgrade-guard validation; print the verdict."""
    report = validate_stage_tar(out_tar, base)
    for e in report.errors:
        print(f"[ERROR]   {e}")
    for w in report.warnings:
        print(f"[WARNING] {w}")
    print(
        f"{out_tar}: {len(report.errors)} error(s), {len(report.warnings)} warning(s) "
        f"— {'CONFORMANT' if report.conformant else 'NON-CONFORMANT'}"
    )
    return report.conformant


# --------------------------------------------------------------------------- #
# Offline self-test (synthetic bytes; no toolchain, no network)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    """Build the overlay from fake .so bytes and validate its SHAPE: ./-rooted, the
    two ANGLE libs are real files at the versioned SONAMEs mode 0755, the
    unversioned symlinks are relative+in-dir, libXNVCtrl is a flat real 0755 file,
    and the whole tar passes stage_tar_spec (no base) CONFORMANT with no warnings."""
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "angle-stage.tar"
        # Shape-only fakes (valid-enough ELF magic; not loadable).
        egl = b"\x7fELF" + b"E" * 256
        gles = b"\x7fELF" + b"G" * 4096
        nvctrl = b"\x7fELF" + b"N" * 256
        summary = build_overlay(out, egl_bytes=egl, gles_bytes=gles, nvctrl_bytes=nvctrl)

        errors: list[str] = []
        with tarfile.open(out, "r") as tar:
            names = tar.getnames()
            for n in names:
                if not n.startswith("./"):
                    errors.append(f"member not ./-rooted: {n}")
                if ".." in n.split("/"):
                    errors.append(f"member escapes: {n}")

            def member(rel: str):
                full = f"./{ANDROLINUX_DIR}/{rel}"
                return tar.getmember(full) if full in names else None

            for so in (EGL_SONAME, GLES_SONAME, NVCTRL_SONAME):
                ti = member(so)
                if ti is None:
                    errors.append(f"missing real file {so}")
                elif not ti.isfile():
                    errors.append(f"{so} is not a real file")
                elif (ti.mode & 0o111) == 0:
                    errors.append(f"{so} not executable (mode {oct(ti.mode)})")

            for link, want in ((EGL_UNVERSIONED, EGL_SONAME), (GLES_UNVERSIONED, GLES_SONAME)):
                ti = member(link)
                if ti is None:
                    errors.append(f"missing symlink {link}")
                elif not ti.issym():
                    errors.append(f"{link} is not a symlink")
                elif ti.linkname != want or "/" in ti.linkname:
                    errors.append(f"{link} target not relative/in-dir: {ti.linkname}")

        # §5-E structural conformance (no base): must be CONFORMANT with NO warnings
        # (flat real .so at bare SONAMEs → no non-flat-SONAME warning).
        report = validate_stage_tar(out)
        if not report.conformant:
            errors.append(f"stage_tar_spec NON-CONFORMANT: {report.errors}")
        if report.warnings:
            errors.append(f"unexpected §5-E warnings: {report.warnings}")

        if errors:
            print("SELFTEST FAIL:")
            for e in errors:
                print("  -", e)
            return 1
        print("SELFTEST PASS: angle overlay shape conformant (0 warnings)")
        print(f"  members: {[m[1] for m in summary['members']]}")
        return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build the angle-stage.tar overlay (system ANGLE).")
    ap.add_argument("--out", default=DEFAULT_OUT, help=f"output overlay tar (default {DEFAULT_OUT})")
    ap.add_argument("--cache", default=DEFAULT_CACHE, help=f".deb cache dir (default {DEFAULT_CACHE})")
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help="base rootfs (dir|tar) for the downgrade guard; '' to skip")
    # OFFLINE byte sources (skip the network entirely).
    ap.add_argument("--so-egl", help="pre-extracted ANGLE libEGL.so bytes (offline)")
    ap.add_argument("--so-gles", help="pre-extracted ANGLE libGLESv2.so bytes (offline)")
    ap.add_argument("--so-nvctrl", help="pre-extracted libXNVCtrl.so.0(.0.0) bytes (offline)")
    ap.add_argument("--cube", help="optional alr-gles-cube test client to stage at "
                                   "/usr/bin/alr-gles-cube (the ANGLE init+clear+present probe)")
    ap.add_argument("--vk-client", help="optional alr-angle-vk client to stage at "
                                        "/usr/bin/alr-angle-vk (forces ANGLE's Vulkan backend, "
                                        "headless pbuffer — the real ANGLE-on-Vulkan device proof)")
    ap.add_argument("--selftest", action="store_true", help="run the offline shape self-test")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    base = args.base if args.base else None
    cube_bytes = Path(args.cube).read_bytes() if args.cube else None
    vk_client_bytes = Path(args.vk_client).read_bytes() if args.vk_client else None

    offline = bool(args.so_egl or args.so_gles or args.so_nvctrl)
    if offline:
        missing = [n for n, v in (("--so-egl", args.so_egl), ("--so-gles", args.so_gles),
                                  ("--so-nvctrl", args.so_nvctrl)) if not v]
        if missing:
            ap.error(f"offline mode needs all of --so-egl/--so-gles/--so-nvctrl (missing {missing})")
        summary = build_overlay(
            out,
            egl_bytes=Path(args.so_egl).read_bytes(),
            gles_bytes=Path(args.so_gles).read_bytes(),
            nvctrl_bytes=Path(args.so_nvctrl).read_bytes(),
            cube_bytes=cube_bytes,
            vk_client_bytes=vk_client_bytes,
        )
    else:
        summary = build_from_mirror(out, Path(args.cache), cube_bytes=cube_bytes,
                                    vk_client_bytes=vk_client_bytes)

    print(f"wrote {summary['out']}")
    print(f"  libEGL.so.1     = {summary['egl_bytes']} bytes (ANGLE)")
    print(f"  libGLESv2.so.2  = {summary['gles_bytes']} bytes (ANGLE, Vulkan backend)")
    print(f"  libXNVCtrl.so.0 = {summary['nvctrl_bytes']} bytes (base-missing DT_NEEDED, flat)")
    for kind, name, extra in summary["members"]:
        print(f"  {kind:8} {name}  {extra}")

    ok = validate(out, base)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
