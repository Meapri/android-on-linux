#!/usr/bin/env python3
"""build_vk_icd_overlay.py — assemble the §5-E `vk-icd-stage.tar` overlay carrying
the ALR guest Vulkan ICD (libvulkan.so.1) into /usr/lib/androlinux.

This is the staging counterpart of the GLES shim's gpushim layout (README:
/usr/lib/androlinux/libGLESv2.so.2 + libEGL.so.1 + unversioned symlinks). It produces a
conformant overlay tar (tools/STAGE_TAR_SPEC.md):
  - `./`-rooted, relative paths only;
  - the .so shipped as a single real file at the bare SONAME `libvulkan.so.1`, mode 0755
    (ALR/Android file-backed PROT_EXEC under untrusted_app rejects a non-exec .so — §10.1);
  - an unversioned `libvulkan.so -> libvulkan.so.1` relative symlink (dev/link convenience);
  - the Khronos ICD manifest `alr_icd.json` (only consulted on the LOADER route, via
    VK_ICD_FILENAMES; harmless on the direct-SONAME route).

Tar member layout:
  ./usr/lib/androlinux/libvulkan.so.1     (real ICD, 0755)
  ./usr/lib/androlinux/libvulkan.so       (-> libvulkan.so.1)
  ./usr/lib/androlinux/alr_icd.json       (ICD manifest, library_path = ./libvulkan.so.1)

The .so is built by app/src/main/cpp/alr_gpu/guest_icd/build-icd.sh (zig cc, glibc-2.34
NEEDED-clean). This script either consumes a pre-built .so (--so PATH) or runs build-icd.sh.

Usage:
  python -m tools.build_vk_icd_overlay --out /tmp/vk-icd-stage.tar [--so PATH] \
      [--base app/src/main/assets/rootfs/payloads/tiny-rootfs.tar]
  python -m tools.build_vk_icd_overlay --selftest
"""
from __future__ import annotations

import argparse
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile

ANDROLINUX_DIR = "usr/lib/androlinux"
SONAME = "libvulkan.so.1"
UNVERSIONED = "libvulkan.so"
MANIFEST = "alr_icd.json"

# Khronos ICD manifest. library_path is RELATIVE to the manifest's own directory, so the
# loader resolves it next to alr_icd.json (both land in /usr/lib/androlinux). api_version
# is the Vulkan version the ICD advertises (Mali-G615 is 1.3; the ENUM rung surfaces it).
ICD_MANIFEST = {
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./" + SONAME,
        "api_version": "1.3.0",
    },
}


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _build_icd_so(out_dir: str) -> str:
    """Run build-icd.sh and return the path to the produced libvulkan.so.1."""
    script = os.path.join(
        _repo_root(), "app", "src", "main", "cpp", "alr_gpu", "guest_icd", "build-icd.sh"
    )
    if not os.path.isfile(script):
        raise FileNotFoundError(f"build-icd.sh not found at {script}")
    env = dict(os.environ)
    env["OUT"] = out_dir
    subprocess.run(["bash", script], check=True, env=env)
    so = os.path.join(out_dir, SONAME)
    if not os.path.isfile(so):
        raise FileNotFoundError(f"build-icd.sh did not produce {so}")
    return so


def build_overlay(out_tar: str, so_path: str | None = None) -> dict:
    """Write the vk-icd overlay tar. Returns a small summary dict."""
    tmp = None
    if so_path is None:
        tmp = tempfile.mkdtemp(prefix="alr-vk-icd-")
        so_path = _build_icd_so(tmp)
    with open(so_path, "rb") as f:
        so_bytes = f.read()

    manifest_bytes = (json.dumps(ICD_MANIFEST, indent=2) + "\n").encode("utf-8")

    members = []
    with tarfile.open(out_tar, "w") as tar:
        def add_file(rel: str, data: bytes, mode: int):
            ti = tarfile.TarInfo("./" + rel)
            ti.size = len(data)
            ti.mode = mode
            ti.type = tarfile.REGTYPE
            tar.addfile(ti, io.BytesIO(data))
            members.append(("file", "./" + rel, mode))

        def add_symlink(rel: str, target: str):
            ti = tarfile.TarInfo("./" + rel)
            ti.type = tarfile.SYMTYPE
            ti.linkname = target  # RELATIVE (in-dir) target per §5-E safe-symlink rule
            ti.mode = 0o777
            tar.addfile(ti)
            members.append(("symlink", "./" + rel, target))

        # The real ICD .so — mode 0755 (the ALR exec-bit requirement, §10.1).
        add_file(f"{ANDROLINUX_DIR}/{SONAME}", so_bytes, 0o755)
        # Unversioned dev symlink (relative, in-dir).
        add_symlink(f"{ANDROLINUX_DIR}/{UNVERSIONED}", SONAME)
        # ICD manifest (loader route only).
        add_file(f"{ANDROLINUX_DIR}/{MANIFEST}", manifest_bytes, 0o644)

    if tmp is not None:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)

    return {
        "out": out_tar,
        "so_bytes": len(so_bytes),
        "members": members,
    }


def _selftest() -> int:
    """Build the overlay (from a fake .so so we need no toolchain), then validate its
    SHAPE: ./-rooted, the .so is a single real file at the bare SONAME mode 0755, the
    unversioned symlink is relative+in-dir, and the manifest parses with a relative
    library_path. (The full §5-E validation against a base runs via
    `python -m tools.stage_tar_spec --overlay <tar>` in the build flow.)"""
    with tempfile.TemporaryDirectory() as d:
        fake_so = os.path.join(d, SONAME)
        with open(fake_so, "wb") as f:
            f.write(b"\x7fELF" + b"\x00" * 64)  # not a real ELF, shape test only
        out = os.path.join(d, "vk-icd-stage.tar")
        summary = build_overlay(out, so_path=fake_so)

        errors = []
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
                data = tar.extractfile(man_member).read()
                man = json.loads(data)
                lib = man.get("ICD", {}).get("library_path", "")
                if not lib.startswith("./"):
                    errors.append(f"manifest library_path not relative: {lib}")

        if errors:
            print("SELFTEST FAIL:")
            for e in errors:
                print("  -", e)
            return 1
        print("SELFTEST PASS: vk-icd overlay shape conformant")
        print(f"  members: {[m[1] for m in summary['members']]}")
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the vk-icd-stage.tar overlay.")
    ap.add_argument("--out", help="output overlay tar path")
    ap.add_argument("--so", help="pre-built libvulkan.so.1 (else build-icd.sh runs)")
    ap.add_argument("--base", help="base rootfs (dir|tar) for an optional guard check")
    ap.add_argument("--selftest", action="store_true", help="run the shape self-test")
    args = ap.parse_args()

    if args.selftest:
        return _selftest()
    if not args.out:
        ap.error("--out is required (or use --selftest)")

    summary = build_overlay(args.out, so_path=args.so)
    print(f"wrote {summary['out']} (libvulkan.so.1 = {summary['so_bytes']} bytes)")
    for kind, name, extra in summary["members"]:
        print(f"  {kind:8} {name}  {extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
