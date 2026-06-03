#!/usr/bin/env python3
"""build_vk_loader_overlay.py — assemble the ``vk-loader-stage.tar`` overlay carrying
the **upstream Khronos Vulkan-Loader** (``libvulkan.so.1``) into
``/usr/lib/androlinux`` + the ICD discovery manifest ``alr_icd.json`` that points the
loader at our (renamed) guest Mali ICD ``libalr_mali_icd.so``.

Why this overlay exists — the "Part B" ICD discovery redirect
-------------------------------------------------------------
Today an ANGLE / volk / dlopen("libvulkan.so.1") guest reaches the **host**
``/system/lib64/libvulkan.so`` (Android's own loader), which never consults our
manifest, so our Mali ICD is never loaded and ANGLE's Vulkan backend dies at
``VK_ERROR_INITIALIZATION_FAILED`` / error -3.

The fix is to put the REAL Khronos loader on the guest's library path under the
standard SONAME, plus a manifest it discovers via ``VK_DRIVER_FILES``:

    guest dlopen("libvulkan.so.1")  →  our staged Khronos libvulkan.so.1  (this overlay)
        → reads alr_icd.json (this overlay)  →  loads libalr_mali_icd.so (our ICD)
        → drives Mali-G615.

This script produces ONLY the loader artifact + the manifest. The companion wiring
(renaming our guest ICD's SONAME ``libvulkan.so.1`` → ``libalr_mali_icd.so`` so the
loader and ICD coexist, exporting ``VK_DRIVER_FILES`` in the launcher, and the
interposer dlopen-redirect) is SEPARATE later (wave-2) work — see the module
docstring's "Remaining wave-2 wiring" note and the report.

Where the Khronos loader bytes come from
----------------------------------------
EXTRACTED from the Ubuntu **noble** ``libvulkan1`` .deb (ports.ubuntu.com arm64,
component ``main``). That package's ``Source:`` is ``vulkan-loader`` — it IS the
upstream Khronos Vulkan-Loader, built for the same glibc as the ALR base (noble
2.39). Audit of the extracted real file ``libvulkan.so.1.3.275`` (the SONAME link
``libvulkan.so.1`` points at it):

  * ELF64 LSB **AArch64** (EM_AARCH64), SONAME ``libvulkan.so.1``;
  * loader-identity strings present: ``VK_DRIVER_FILES``, ``VK_ICD_FILENAMES``,
    ``VK_LOADER_DEBUG``, ``VK_ADD_DRIVER_FILES``,
    ``vk_icdNegotiateLoaderICDInterfaceVersion`` — i.e. it is the Khronos loader,
    NOT a vendor ICD;
  * DT_NEEDED: ``libm.so.6``, ``libc.so.6``, ``ld-linux-aarch64.so.1`` — ALL
    provided by the base rootfs as real files, so NOTHING extra ships flat
    (NEEDED-clean over the base);
  * max GLIBC symbol version **2.38** ≤ noble **2.39** (the runtime satisfies it).

The base rootfs ships NO ``libvulkan`` at all, so this overlay is a CLEAN
direct-SONAME drop with ZERO base conflict (overlay_guard: 0 BLOCK).

Overlay layout (mirrors the gpushim / vk-icd / angle staging)
-------------------------------------------------------------
``./``-rooted, relative paths only; the .so shipped as a single real file at the
bare SONAME ``libvulkan.so.1`` mode **0755** (the ALR file-backed PROT_EXEC
requirement under untrusted_app — §10.1); the unversioned dev symlink relative +
in-dir:

  ./usr/lib/androlinux/libvulkan.so.1   (real Khronos loader, 0755)
  ./usr/lib/androlinux/libvulkan.so     -> libvulkan.so.1
  ./usr/lib/androlinux/alr_icd.json     (ICD manifest, library_path = libalr_mali_icd.so)

ICD manifest — the discovery redirect
--------------------------------------
``alr_icd.json`` is the standard Khronos ICD manifest the loader reads (via
``VK_DRIVER_FILES`` / ``VK_ICD_FILENAMES``). Its ``library_path`` is the ABSOLUTE
path of our guest ICD UNDER THE NAME IT WILL BE RENAMED TO in the wave-2 wiring,
``/usr/lib/androlinux/libalr_mali_icd.so`` — distinct from the loader's own
``libvulkan.so.1`` so the two coexist in the same directory (a relative
``./libvulkan.so.1`` here would make the loader try to load ITSELF as the ICD).

  {"file_format_version":"1.0.1",
   "ICD":{"library_path":"/usr/lib/androlinux/libalr_mali_icd.so","api_version":"1.3.0"}}

Usage
-----
  # NETWORK: fetch the noble libvulkan1 .deb, extract the Khronos loader, stage it
  python -m tools.build_vk_loader_overlay --out out/v2-stage/vk-loader-stage.tar
  # OFFLINE: consume a pre-extracted Khronos libvulkan.so.1 (no .deb download)
  python -m tools.build_vk_loader_overlay --out vk-loader-stage.tar --so libvulkan.so.1
  python -m tools.build_vk_loader_overlay --selftest   # offline shape self-test
"""
from __future__ import annotations

import argparse
import io
import json
import sys
import tarfile
import tempfile
from pathlib import Path

# Reuse, do NOT duplicate, the .deb fetch/crack + §5-E validation primitives
# (identical sources to tools/build_angle_overlay.py).
from tools.deb_closure import _download_deb, fetch_packages_index, parse_packages
from tools.build_stage_tar import extract_deb
from tools.stage_tar_spec import validate_stage_tar

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

# The Khronos loader ships into the SHARED /usr/lib/androlinux dir (NOT a private
# one): a guest's dlopen("libvulkan.so.1") must resolve THIS loader, and the loader
# in turn finds our ICD (libalr_mali_icd.so) next to its own manifest in the same
# dir. The base ships no libvulkan, so there is nothing to clobber.
ANDROLINUX_DIR = "usr/lib/androlinux"

# The standard Linux Vulkan SONAME a guest's DT_NEEDED / dlopen records; the Khronos
# loader bytes are placed here as the real file (ld.so resolves NEEDED by filename).
SONAME = "libvulkan.so.1"
UNVERSIONED = "libvulkan.so"
MANIFEST = "alr_icd.json"

# The NAME our guest Mali ICD is renamed to in the wave-2 wiring (build_vk_icd_overlay
# + build-icd.sh + the DT_SONAME). The manifest's library_path is the ABSOLUTE path so
# the loader loads it regardless of cwd, and so it never collides with the loader's own
# libvulkan.so.1 in the same directory. THIS overlay does not ship the ICD — it only
# names it; the vk-icd overlay (wave-2 renamed) supplies the bytes.
ICD_BASENAME = "libalr_mali_icd.so"
ICD_LIBRARY_PATH = f"/{ANDROLINUX_DIR}/{ICD_BASENAME}"

# Khronos ICD manifest. file_format_version 1.0.1 is the format the modern loader
# (1.3.x) reads; library_path is ABSOLUTE (see above); api_version is the Vulkan
# version our ICD advertises (Mali-G615 is 1.3).
ICD_MANIFEST = {
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": ICD_LIBRARY_PATH,
        "api_version": "1.3.0",
    },
}

# Where the Khronos loader bytes come from: the Ubuntu noble libvulkan1 .deb
# (Source: vulkan-loader = upstream Khronos Vulkan-Loader), arm64, component main.
# The ALR base is Ubuntu noble (glibc 2.39); this .deb is built for the same glibc,
# and its max symver (2.38) is satisfied. NEEDED-clean over the base (libm/libc/ld).
MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main",)

LOADER_PACKAGE = "libvulkan1"
# The real loader file inside the .deb is the fully-versioned name; the .deb's own
# libvulkan.so.1 is a SONAME symlink to it. We resolve the SONAME symlink to its real
# target and ship THOSE bytes flat at libvulkan.so.1.
LOADER_SONAME_LINK = "libvulkan.so.1"

DEFAULT_OUT = "out/v2-stage/vk-loader-stage.tar"
DEFAULT_CACHE = "/tmp/deb-cache-noble"

# tiny-rootfs (the base) for the optional downgrade guard.
DEFAULT_BASE = "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"


# --------------------------------------------------------------------------- #
# Fetch (network) — extract the Khronos loader bytes from the noble .deb
# --------------------------------------------------------------------------- #

def _deb_filename(pkg: str, *, opener=None) -> str:
    """Look up a package's pool Filename in the noble arm64 main index."""
    kw = {} if opener is None else {"opener": opener}
    idx = parse_packages(
        fetch_packages_index(MIRROR, SUITE, ARCH, components=COMPONENTS, **kw)
    )
    fields = idx.get(pkg)
    if not fields or not fields.get("Filename"):
        raise SystemExit(f"{pkg} not found in {SUITE}/{ARCH} index")
    return fields["Filename"]


def _resolve_loader_real(root: Path) -> Path:
    """Return the REAL Khronos loader file inside an extracted libvulkan1 root.

    The .deb ships ``libvulkan.so.1 -> libvulkan.so.1.3.275`` (SONAME symlink to the
    versioned real file). We find the SONAME link, resolve it to its real target, and
    return that path (so the staged bytes are the actual loader, not a dangling link)."""
    # Prefer resolving the canonical SONAME symlink to its real target.
    for p in Path(root).rglob(LOADER_SONAME_LINK):
        if p.is_symlink():
            tgt = (p.parent / p.readlink()).resolve()
            if tgt.is_file():
                return tgt
        elif p.is_file():
            return p
    # Fallback: the highest-versioned real libvulkan.so.1.* file.
    cands = sorted(
        q for q in Path(root).rglob("libvulkan.so.1.*")
        if q.is_file() and not q.is_symlink()
    )
    if cands:
        return cands[-1]
    raise SystemExit(f"no real Khronos loader (libvulkan.so.1*) found under {root}")


def fetch_loader_bytes(cache: Path, *, opener=None) -> bytes:
    """NETWORK: download the noble libvulkan1 .deb and return the Khronos loader bytes."""
    cache.mkdir(parents=True, exist_ok=True)
    dl = {} if opener is None else {"opener": opener}
    deb = _download_deb(MIRROR, _deb_filename(LOADER_PACKAGE, opener=opener), cache, **dl)
    with tempfile.TemporaryDirectory() as td:
        root = extract_deb(deb, Path(td))
        return _resolve_loader_real(Path(root)).read_bytes()


# --------------------------------------------------------------------------- #
# Overlay assembly
# --------------------------------------------------------------------------- #

def build_overlay(out_tar: str | Path, *, loader_bytes: bytes) -> dict:
    """Write the vk-loader overlay tar from the provided Khronos loader bytes.

    Members: the real loader at the bare SONAME ``libvulkan.so.1`` (0755), an
    unversioned ``libvulkan.so`` relative in-dir symlink, and the ``alr_icd.json``
    manifest pointing at the (wave-2 renamed) ICD ``libalr_mali_icd.so``.
    Returns a small summary dict."""
    manifest_bytes = (json.dumps(ICD_MANIFEST, indent=2) + "\n").encode("utf-8")

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

        # The real Khronos loader at the bare SONAME — mode 0755 (ALR exec-bit, §10.1).
        add_file(f"{ANDROLINUX_DIR}/{SONAME}", loader_bytes, 0o755)
        # Unversioned dev symlink (relative, in-dir).
        add_symlink(f"{ANDROLINUX_DIR}/{UNVERSIONED}", SONAME)
        # ICD discovery manifest (read by the loader via VK_DRIVER_FILES).
        add_file(f"{ANDROLINUX_DIR}/{MANIFEST}", manifest_bytes, 0o644)

    return {
        "out": str(out_tar),
        "loader_bytes": len(loader_bytes),
        "manifest": ICD_MANIFEST,
        "members": members,
    }


def build_from_mirror(out_tar: str | Path, cache: Path, *, opener=None) -> dict:
    """NETWORK path: fetch the Khronos loader bytes, then assemble the overlay."""
    return build_overlay(out_tar, loader_bytes=fetch_loader_bytes(cache, opener=opener))


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
    """Build the overlay from fake loader bytes and validate its SHAPE: ./-rooted,
    the loader is a real file at the bare SONAME mode 0755, the unversioned symlink is
    relative+in-dir, the manifest parses with an ABSOLUTE library_path that names the
    (renamed) ICD (NOT the loader's own libvulkan.so.1), and the whole tar passes
    stage_tar_spec (no base) CONFORMANT with no warnings (flat real .so at the bare
    SONAME → no non-flat-SONAME warning)."""
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "vk-loader-stage.tar"
        fake = b"\x7fELF" + b"L" * 4096  # valid magic; not a loadable loader
        summary = build_overlay(out, loader_bytes=fake)

        errors: list[str] = []
        with tarfile.open(out, "r") as tar:
            names = tar.getnames()
            for n in names:
                if not n.startswith("./"):
                    errors.append(f"member not ./-rooted: {n}")
                if ".." in n.split("/"):
                    errors.append(f"member escapes: {n}")

            so_member = f"./{ANDROLINUX_DIR}/{SONAME}"
            link_member = f"./{ANDROLINUX_DIR}/{UNVERSIONED}"
            man_member = f"./{ANDROLINUX_DIR}/{MANIFEST}"

            if so_member not in names:
                errors.append("missing libvulkan.so.1")
            else:
                ti = tar.getmember(so_member)
                if not ti.isfile():
                    errors.append("libvulkan.so.1 is not a real file")
                if (ti.mode & 0o111) == 0:
                    errors.append(f"libvulkan.so.1 not executable (mode {oct(ti.mode)})")
                if ti.size != len(fake):
                    errors.append("libvulkan.so.1 size mismatch")

            if link_member not in names:
                errors.append("missing libvulkan.so symlink")
            else:
                ti = tar.getmember(link_member)
                if not ti.issym():
                    errors.append("libvulkan.so is not a symlink")
                elif ti.linkname != SONAME or "/" in ti.linkname:
                    errors.append(f"libvulkan.so target not relative/in-dir: {ti.linkname}")

            if man_member not in names:
                errors.append("missing alr_icd.json")
            else:
                man = json.loads(tar.extractfile(man_member).read())
                lib = man.get("ICD", {}).get("library_path", "")
                if lib != ICD_LIBRARY_PATH:
                    errors.append(f"manifest library_path != {ICD_LIBRARY_PATH}: {lib}")
                if SONAME in lib:
                    errors.append(
                        f"manifest library_path points at the loader itself ({lib}); "
                        f"must point at the renamed ICD {ICD_BASENAME}"
                    )
                if man.get("file_format_version") != "1.0.1":
                    errors.append(
                        f"manifest file_format_version != 1.0.1: "
                        f"{man.get('file_format_version')}"
                    )

        # §5-E structural conformance (no base): CONFORMANT with NO warnings.
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
        print("SELFTEST PASS: vk-loader overlay shape conformant (0 warnings)")
        print(f"  members: {[m[1] for m in summary['members']]}")
        print(f"  manifest library_path: {ICD_LIBRARY_PATH}")
        return 0


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Build the vk-loader-stage.tar overlay (upstream Khronos Vulkan-Loader)."
    )
    ap.add_argument("--out", default=DEFAULT_OUT, help=f"output overlay tar (default {DEFAULT_OUT})")
    ap.add_argument("--cache", default=DEFAULT_CACHE, help=f".deb cache dir (default {DEFAULT_CACHE})")
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help="base rootfs (dir|tar) for the downgrade guard; '' to skip")
    # OFFLINE byte source (skip the network entirely): a pre-extracted Khronos loader.
    ap.add_argument("--so", help="pre-extracted Khronos libvulkan.so.1 bytes (offline)")
    ap.add_argument("--selftest", action="store_true", help="run the offline shape self-test")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    base = args.base if args.base else None

    if args.so:
        summary = build_overlay(out, loader_bytes=Path(args.so).read_bytes())
    else:
        summary = build_from_mirror(out, Path(args.cache))

    print(f"wrote {summary['out']}")
    print(f"  libvulkan.so.1  = {summary['loader_bytes']} bytes (Khronos Vulkan-Loader)")
    print(f"  alr_icd.json    -> {ICD_LIBRARY_PATH}")
    for kind, name, extra in summary["members"]:
        print(f"  {kind:8} {name}  {extra}")

    ok = validate(out, base)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
