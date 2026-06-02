#!/usr/bin/env python3
"""Build the NSS plugin-module overlay (nss-stage.tar) for chromium TLS.

chromium 147 (generic-Linux build) initializes NSS for its certificate store
during TLS setup. NSS's libnss3 dlopen()s its PKCS#11 / crypto PLUGIN modules at
runtime — libsoftokn3.so (softoken), libfreebl3.so (freebl crypto), libnssckbi.so
(builtin CA trust), libnssdbm3.so, libfreeblpriv3.so. Because those are dlopen'd
(NOT DT_NEEDED), the §5-E deb-closure staging (which follows DT_NEEDED only) DROPS
them — the base rootfs ends up with libnss3.so + libnssutil3.so but none of the
plugins. Result, device-confirmed:

    crypto/nss_util.cc:256] Error initializing NSS with a persistent database
      (sql:/root/.local/share/pki/nssdb): libsoftokn3.so: cannot open shared
      object file: No such file or directory
    crypto/nss_util.cc:146] FATAL nss_error=-5925   → chromium ImmediateCrash (brk)

This builder fetches the noble libnss3 .deb and packs JUST those plugin modules
into an overlay tar, at BOTH /usr/lib/aarch64-linux-gnu/ (flat — found via the
loader's LD_LIBRARY_PATH for a bare dlopen("libsoftokn3.so")) and
/usr/lib/aarch64-linux-gnu/nss/ (the Debian/NSS compiled search path). Extracted on
device by the MainActivity toolkit loop (name "nss").
"""
from __future__ import annotations

import argparse
import sys
import tarfile
import tempfile
from pathlib import Path

from tools.deb_closure import fetch_packages_index, parse_packages, _download_deb
from tools.build_stage_tar import extract_deb

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main", "universe")
PACKAGE = "libnss3"
LIBDIR = "usr/lib/aarch64-linux-gnu"
# The dlopen'd NSS plugin modules dropped by the DT_NEEDED-only closure.
MODULES = (
    "libsoftokn3.so",
    "libfreebl3.so",
    "libfreeblpriv3.so",
    "libnssckbi.so",
    "libnssdbm3.so",
)


def _find_modules(root: Path) -> dict[str, Path]:
    """Locate each NSS plugin module anywhere under the extracted deb tree."""
    found: dict[str, Path] = {}
    for mod in MODULES:
        for p in root.rglob(mod):
            if p.is_file():
                found[mod] = p
                break
    return found


def build(out: Path, cache: Path) -> dict:
    idx = fetch_packages_index(MIRROR, SUITE, ARCH, components=COMPONENTS)
    pkgs = parse_packages(idx)
    if PACKAGE not in pkgs:
        raise SystemExit(f"{PACKAGE} not in noble index")
    filename = pkgs[PACKAGE].get("Filename")
    if not filename:
        raise SystemExit(f"{PACKAGE} has no Filename in index")
    deb = _download_deb(MIRROR, filename, cache)
    with tempfile.TemporaryDirectory() as td:
        root = extract_deb(deb, Path(td))
        mods = _find_modules(root)
        missing = [m for m in MODULES if m not in mods]
        # libsoftokn3 + libfreebl3 are load-bearing; the rest are best-effort.
        for hard in ("libsoftokn3.so", "libfreebl3.so"):
            if hard not in mods:
                raise SystemExit(f"required {hard} not found in {PACKAGE} .deb")
        with tarfile.open(out, "w") as tar:
            for mod, src in mods.items():
                # Flat (dlopen-by-soname via LD_LIBRARY_PATH) AND nss/ (NSS search path).
                for arc in (f"./{LIBDIR}/{mod}", f"./{LIBDIR}/nss/{mod}"):
                    ti = tar.gettarinfo(str(src), arcname=arc)
                    ti.uid = ti.gid = 0
                    ti.uname = ti.gname = ""
                    ti.mode = 0o755
                    with open(src, "rb") as fh:
                        tar.addfile(ti, fh)
        return {
            "out": str(out),
            "package": PACKAGE,
            "deb": deb.name,
            "modules": sorted(mods),
            "missing": missing,
            "members": len(mods) * 2,
        }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="build_nss_overlay", description=__doc__)
    ap.add_argument("--out", default="/tmp/nss-stage.tar")
    ap.add_argument("--cache", default="/tmp/alr-deb-cache")
    ap.add_argument("--list", action="store_true",
                    help="print the planned modules + the resolved .deb without packing")
    args = ap.parse_args(argv)
    cache = Path(args.cache)
    if args.list:
        idx = fetch_packages_index(MIRROR, SUITE, ARCH, components=COMPONENTS)
        pkgs = parse_packages(idx)
        print(f"package={PACKAGE} filename={pkgs.get(PACKAGE, {}).get('Filename')}")
        for m in MODULES:
            print(f"  module {m} -> {LIBDIR}/{m} (+ {LIBDIR}/nss/{m})")
        return 0
    info = build(Path(args.out), cache)
    print(f"built {info['out']}")
    print(f"  package: {info['package']} ({info['deb']})")
    print(f"  modules: {len(info['modules'])} ({', '.join(info['modules'])})")
    if info["missing"]:
        print(f"  NOTE missing (best-effort): {', '.join(info['missing'])}")
    print(f"  members: {info['members']} (flat + nss/ each)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
