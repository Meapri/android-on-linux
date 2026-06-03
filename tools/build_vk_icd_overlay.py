#!/usr/bin/env python3
"""build_vk_icd_overlay.py — assemble the §5-E `vk-icd-stage.tar` overlay carrying
the ALR guest Vulkan Mali ICD (libalr_mali_icd.so) into /usr/lib/androlinux.

ICD DISCOVERY REDIRECT (Part B) — why the SONAME is libalr_mali_icd.so, NOT libvulkan.so.1
-----------------------------------------------------------------------------------------
Our guest ICD was RENAMED from libvulkan.so.1 to **libalr_mali_icd.so** so it COEXISTS
with the real Khronos Vulkan-Loader, which now owns libvulkan.so.1 in the SAME directory
(/usr/lib/androlinux) via the companion vk-loader overlay (tools/build_vk_loader_overlay.py).
An ANGLE/volk guest does dlopen("libvulkan.so.1") → reaches the Khronos LOADER → the loader
reads the discovery manifest alr_icd.json (shipped by the vk-loader overlay, library_path =
/usr/lib/androlinux/libalr_mali_icd.so) → loads THIS file as the ICD → Mali-G615. If our ICD
kept the name libvulkan.so.1, the loader would try to load ITSELF as the ICD (fatal).

This is the staging counterpart of the GLES shim's gpushim layout (README:
/usr/lib/androlinux/libGLESv2.so.2 + libEGL.so.1 + unversioned symlinks). It produces a
conformant overlay tar (tools/STAGE_TAR_SPEC.md):
  - `./`-rooted, relative paths only;
  - the .so shipped as a single real file at the bare SONAME `libalr_mali_icd.so`, mode 0755
    (ALR/Android file-backed PROT_EXEC under untrusted_app rejects a non-exec .so — §10.1).

This overlay ships ONLY the renamed ICD .so. It deliberately does NOT ship:
  * a libvulkan.so / libvulkan.so.1 symlink — the Khronos loader (vk-loader overlay) owns
    those names; a libvulkan.so here would shadow the loader for a dlopen("libvulkan.so").
  * an alr_icd.json manifest — the vk-loader overlay supplies the authoritative manifest
    (its library_path is the ABSOLUTE /usr/lib/androlinux/libalr_mali_icd.so). The
    direct-SONAME guests (alr-vk-enum / alr-vk-tri, which DT_NEEDED libalr_mali_icd.so)
    bind the ICD directly and need no manifest at all.

Tar member layout:
  ./usr/lib/androlinux/libalr_mali_icd.so     (real ICD, 0755)

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
import os
import subprocess
import sys
import tarfile
import tempfile

ANDROLINUX_DIR = "usr/lib/androlinux"
# The ICD's RENAMED SONAME (was libvulkan.so.1). It coexists with the Khronos loader's
# libvulkan.so.1 in the same dir; the loader's alr_icd.json (vk-loader overlay) names it.
SONAME = "libalr_mali_icd.so"


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _build_icd_so(out_dir: str) -> str:
    """Run build-icd.sh and return the path to the produced libalr_mali_icd.so."""
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


# Guest client programs (VK-M3/M4 device tests). When --with-clients is passed they ride
# the overlay into the rootfs at /usr/bin so the loader can launch them directly (vs. the
# README's adb-push-to-/data/local/tmp route). alr-vk-tri is the VK-M4 PRESENT proof.
CLIENT_BIN_DIR = "usr/bin"
CLIENT_NAMES = ("alr-vk-enum", "alr-vk-tri")


def build_overlay(out_tar: str, so_path: str | None = None,
                  clients_dir: str | None = None) -> dict:
    """Write the vk-icd overlay tar. Returns a small summary dict. If `clients_dir` is
    given, also bundle the guest client programs (CLIENT_NAMES) from it under /usr/bin."""
    tmp = None
    if so_path is None:
        tmp = tempfile.mkdtemp(prefix="alr-vk-icd-")
        so_path = _build_icd_so(tmp)
        # The build script also produced the clients next to the .so.
        if clients_dir is None:
            clients_dir = tmp
    with open(so_path, "rb") as f:
        so_bytes = f.read()

    members = []
    client_bytes = 0
    with tarfile.open(out_tar, "w") as tar:
        def add_file(rel: str, data: bytes, mode: int):
            ti = tarfile.TarInfo("./" + rel)
            ti.size = len(data)
            ti.mode = mode
            ti.type = tarfile.REGTYPE
            tar.addfile(ti, io.BytesIO(data))
            members.append(("file", "./" + rel, mode))

        # The real ICD .so under its RENAMED SONAME — mode 0755 (ALR exec-bit, §10.1).
        # NO libvulkan.so symlink (the Khronos loader owns that name) and NO alr_icd.json
        # (the vk-loader overlay supplies the authoritative absolute-path manifest).
        add_file(f"{ANDROLINUX_DIR}/{SONAME}", so_bytes, 0o755)
        # Optional: the guest client programs under /usr/bin (mode 0755 exec bit).
        if clients_dir is not None:
            for name in CLIENT_NAMES:
                p = os.path.join(clients_dir, name)
                if os.path.isfile(p):
                    with open(p, "rb") as cf:
                        data = cf.read()
                    add_file(f"{CLIENT_BIN_DIR}/{name}", data, 0o755)
                    client_bytes += len(data)

    if tmp is not None:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)

    return {
        "out": out_tar,
        "so_bytes": len(so_bytes),
        "client_bytes": client_bytes,
        "members": members,
    }


def _selftest() -> int:
    """Build the overlay (from a fake .so so we need no toolchain), then validate its
    SHAPE: ./-rooted, the ICD .so is a single real file at the bare SONAME
    libalr_mali_icd.so mode 0755, and the overlay ships NEITHER a libvulkan.so symlink
    (the Khronos loader owns that name) NOR an alr_icd.json (the vk-loader overlay owns
    the manifest). (The full §5-E validation against a base runs via
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
            if so_member not in names:
                errors.append(f"missing {SONAME}")
            else:
                ti = tar.getmember(so_member)
                if not ti.isfile():
                    errors.append(f"{SONAME} is not a real file")
                if (ti.mode & 0o111) == 0:
                    errors.append(f"{SONAME} not executable (mode {oct(ti.mode)})")
            # The overlay must NOT carry the loader-owned libvulkan name or the manifest.
            for forbidden in (f"./{ANDROLINUX_DIR}/libvulkan.so",
                              f"./{ANDROLINUX_DIR}/libvulkan.so.1",
                              f"./{ANDROLINUX_DIR}/alr_icd.json"):
                if forbidden in names:
                    errors.append(f"overlay must not ship {forbidden} (owned by vk-loader overlay)")

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
    ap.add_argument("--so", help="pre-built libalr_mali_icd.so (else build-icd.sh runs)")
    ap.add_argument("--base", help="base rootfs (dir|tar) for an optional guard check")
    ap.add_argument("--with-clients", action="store_true",
                    help="also bundle the guest client programs (alr-vk-enum, alr-vk-tri) "
                         "under /usr/bin so the loader can launch them from the rootfs")
    ap.add_argument("--clients-dir",
                    help="dir holding the pre-built guest clients (default: next to --so, "
                         "or build-icd.sh's out dir)")
    ap.add_argument("--selftest", action="store_true", help="run the shape self-test")
    args = ap.parse_args()

    if args.selftest:
        return _selftest()
    if not args.out:
        ap.error("--out is required (or use --selftest)")

    clients_dir = None
    if args.with_clients:
        # Prefer an explicit dir, else infer from --so's directory (the build script puts
        # the clients next to libalr_mali_icd.so). If neither + --so is absent, build_overlay
        # falls back to the freshly-built temp out dir.
        clients_dir = args.clients_dir or (os.path.dirname(os.path.abspath(args.so))
                                           if args.so else None)
    summary = build_overlay(args.out, so_path=args.so, clients_dir=clients_dir)
    extra = (f", clients = {summary['client_bytes']} bytes"
             if summary.get("client_bytes") else "")
    print(f"wrote {summary['out']} ({SONAME} = {summary['so_bytes']} bytes{extra})")
    for kind, name, val in summary["members"]:
        print(f"  {kind:8} {name}  {val}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
