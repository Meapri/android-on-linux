"""Build the chromium NETWORK staging overlay (chromium-net-stage.tar) — CR-1/CR-2.

Why
---
chromium on ALR already starts (`chromium-headless-shell --version` runs in-process
over a ~186 MB + ~200 .so closure). The next two run milestones are:

  * **CR-1** — single-process headless render of a *local* page (no net, no GPU,
    no multiprocess): ``--single-process --no-zygote --no-sandbox --dump-dom
    file:///root/cr-test.html`` (or a ``data:`` URL).
  * **CR-2** — the same, but loading a *real* ``https://`` URL: DNS resolution +
    TLS certificate verification must work.

This builder produces the rootfs-absolute §5-E overlay that unblocks BOTH:

  1. ``/root/cr-test.html`` — a self-contained local page (``tools/chromium/cr-test.html``)
     so CR-1 has something to render with zero network.
  2. ``/etc/resolv.conf`` — ``nameserver 8.8.8.8`` + ``1.1.1.1`` so the guest glibc
     resolver has an upstream (the base rootfs ships no resolv.conf; without it
     ``getaddrinfo`` returns EAI_AGAIN and every https URL fails to resolve).
  3. ``/etc/nsswitch.conf`` — ``hosts: files dns`` so glibc consults
     ``/etc/hosts`` then DNS (the noble default; absent it, some glibc builds
     refuse DNS lookups entirely).
  4. ``/etc/ssl/certs/ca-certificates.crt`` — the Mozilla CA trust bundle, so
     chromium's (and curl/openssl's) TLS verification has a root store. This is
     assembled host-side by concatenating every enabled ``.crt`` the noble
     ``ca-certificates`` .deb ships under ``/usr/share/ca-certificates/mozilla``
     (see ANALYSIS below — the .deb does *not* ship the pre-built bundle; its
     postinst assembles it via ``update-ca-certificates``, which we reproduce).

ANALYSIS — ALR does NOT mediate sockets; network "just works" once staged
-------------------------------------------------------------------------
The ALR loader's seccomp design traces ONLY two syscall families (verified in
``app/src/main/cpp/runtime_report.cpp``):

  * ``alr_install_path_trace_filter`` — RET_TRACE for the 9 *path* syscalls
    (openat / openat2 / newfstatat / statx / faccessat{,2} / readlinkat /
    mkdirat / unlinkat); RET_ALLOW for everything else. (PCGATE A/B baseline.)
  * ``alr_install_execve_trace_filter`` — RET_TRACE for execve / execveat only;
    RET_ALLOW for everything else. (PCGATE default; the in-process interposer
    installs its own PC-gated *path* filter on top.)

Neither filter names ``socket``/``connect``/``bind``/``sendto``/``recvfrom``/
``sendmsg``/``recvmsg``/``getsockopt`` — and seccomp's default for an un-named
syscall in these filters is RET_ALLOW. So **chromium's TCP/UDP sockets pass
straight through to the Android kernel un-traced, at native speed** — there is no
socket mediation, no userspace proxy, no per-packet supervision. (untrusted_app
has INTERNET, so the kernel/SELinux side permits outbound sockets too.)

The ONLY thing the loader's path mediation touches that the network path needs is
the *file* reads of ``/etc/resolv.conf``, ``/etc/nsswitch.conf``,
``/etc/hosts`` and the CA bundle — which the path filter rewrites into the rootfs.
So the entire "network gap" is **name resolution config + TLS roots**, both of
which are plain files this overlay supplies. Once WS-1 stages
``chromium-net-stage.tar``, network should "just work":

  * DNS:  glibc reads ``/etc/resolv.conf`` (→ 8.8.8.8 / 1.1.1.1) + ``nsswitch.conf``
          (→ ``dns``), then opens a UDP socket to :53 — un-traced, native.
  * TLS:  chromium/openssl reads ``/etc/ssl/certs/ca-certificates.crt`` for the
          root store, then does the TLS handshake over a TCP socket — un-traced.

(If WS-1 prefers to inject these via env instead of rootfs files, the equivalents
are ``RES_OPTIONS``/a ``resolv.conf`` path and ``SSL_CERT_FILE=/etc/ssl/certs/
ca-certificates.crt`` / ``CURL_CA_BUNDLE`` — but shipping the files is the
zero-env, "looks like a normal rootfs" path and is what this overlay does.)

Honest scope
------------
HOST-ONLY. This builds + validates the overlay host-side (full pack, or
``--list`` dry-run if the ca-certs download is unwanted). The actual on-device
``file:///root/cr-test.html`` render (CR-1) and ``https://`` load (CR-2) are the
integration/device gate (WS-1 stages the tar). The socket-passthrough analysis is
read from the seccomp filter source; it is not a substitute for a device run.
"""

from __future__ import annotations

import argparse
import io
import json
import re
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.deb_closure import (
    _download_deb,
    fetch_packages_index,
    parse_packages,
)
from tools.build_apt_dpkg_overlay import _decompress_data_tar

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main",)  # ca-certificates lives in noble main

# The noble ca-certificates package. `all` arch (the CA bundle is arch-independent).
CA_CERTS = {
    "package": "ca-certificates",
    "note": "noble ca-certificates; ships per-CA .crt under usr/share/ca-certificates,"
            " bundle assembled host-side (postinst would run update-ca-certificates)",
}

# Where the noble ca-certificates .deb keeps the individual trusted-CA PEM files.
# update-ca-certificates concatenates the enabled ones into the bundle below; with
# no ca-certificates.conf present (the .deb doesn't ship it — debconf generates it
# with ALL mozilla CAs enabled by default) we reproduce the default = concat all.
CA_SRC_PREFIX = "usr/share/ca-certificates/"
CA_BUNDLE_PATH = "etc/ssl/certs/ca-certificates.crt"

# Rootfs-absolute (./-rooted in the tar) network config the base rootfs lacks.
RESOLV_CONF_PATH = "etc/resolv.conf"
RESOLV_CONF_BODY = (
    "# ALR chromium-net overlay — public resolvers (no systemd-resolved on guest).\n"
    "nameserver 8.8.8.8\n"
    "nameserver 1.1.1.1\n"
)

NSSWITCH_PATH = "etc/nsswitch.conf"
# Minimal noble-default hosts line: files (/etc/hosts) then DNS. Other databases
# resolve via files only (no NIS/LDAP/systemd on the guest) — keeps glibc happy.
NSSWITCH_BODY = (
    "# ALR chromium-net overlay — files then DNS for host name resolution.\n"
    "passwd:         files\n"
    "group:          files\n"
    "shadow:         files\n"
    "hosts:          files dns\n"
    "networks:       files\n"
    "protocols:      files\n"
    "services:       files\n"
    "ethers:         files\n"
    "rpc:            files\n"
)

# The local CR-1 test page, packed at /root/cr-test.html. Source of truth on disk:
TEST_PAGE_SRC = Path(__file__).resolve().parent / "chromium" / "cr-test.html"
TEST_PAGE_PATH = "root/cr-test.html"


@dataclass
class ChromiumNetOverlayResult:
    out_tar: str
    members: tuple[str, ...] = ()
    ca_cert_count: int = 0
    ca_bundle_bytes: int = 0
    ca_deb_filename: str = ""
    file_count: int = 0

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "members": list(self.members),
            "ca_cert_count": self.ca_cert_count,
            "ca_bundle_bytes": self.ca_bundle_bytes,
            "ca_bundle_kib": round(self.ca_bundle_bytes / 1024, 1),
            "ca_deb_filename": self.ca_deb_filename,
            "file_count": self.file_count,
        }


# --------------------------------------------------------------------------- #
# CR-1 page self-containment check (no external fetch)
# --------------------------------------------------------------------------- #

# Patterns that pull a remote resource at render time. We look for an absolute or
# protocol-relative URL inside a src=/href= attribute, a CSS `@import` of a URL,
# or a remote `@font-face` src — NOT the bare words (which may appear in prose/
# comments). A self-contained CR-1 page must match NONE of these.
_EXTERNAL_REF_PATTERNS = (
    re.compile(r"""\b(?:src|href)\s*=\s*['"]\s*(?:https?:)?//""", re.IGNORECASE),
    re.compile(r"""@import\s+(?:url\(\s*)?['"]?\s*(?:https?:)?//""", re.IGNORECASE),
    re.compile(r"""@font-face[^}]*url\(\s*['"]?\s*(?:https?:)?//""",
               re.IGNORECASE | re.DOTALL),
)


def page_external_resources(html: str) -> list[str]:
    """Return the matched fragments of any external-fetch directive in ``html``.

    Empty list == the page is self-contained (CR-1 can render it with zero
    network). Catches absolute/protocol-relative src/href, CSS ``@import`` of a
    URL, and remote ``@font-face`` src; ignores the bare words in prose/comments.
    """
    hits: list[str] = []
    for pat in _EXTERNAL_REF_PATTERNS:
        for m in pat.finditer(html):
            hits.append(m.group(0))
    return hits


# --------------------------------------------------------------------------- #
# tar helpers (§5-E ./-rooted, mtime=0, deterministic)
# --------------------------------------------------------------------------- #

def _add_dir(tar: tarfile.TarFile, rel: str, mode: int = 0o755) -> None:
    ti = tarfile.TarInfo("./" + rel.strip("/"))
    ti.type = tarfile.DIRTYPE
    ti.mode = mode
    ti.mtime = 0
    tar.addfile(ti)


def _add_file(tar: tarfile.TarFile, rel: str, data: bytes, mode: int = 0o644) -> None:
    ti = tarfile.TarInfo("./" + rel.strip("/"))
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 0
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


# --------------------------------------------------------------------------- #
# CA bundle assembly (from the noble ca-certificates .deb)
# --------------------------------------------------------------------------- #

def resolve_ca_certs_filename(
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    index: dict[str, dict] | None = None,
) -> str:
    """Look up the ca-certificates .deb ``Filename`` in the noble Packages index.

    NETWORK unless ``index`` is supplied (offline/selftest)."""
    if index is None:
        index = parse_packages(
            fetch_packages_index(mirror, suite, arch, components=components)
        )
    fields = index.get(CA_CERTS["package"], {})
    filename = fields.get("Filename")
    if not filename:
        raise RuntimeError(
            f"{CA_CERTS['package']} not found in {suite}/{'+'.join(components)} index"
        )
    return filename


def assemble_ca_bundle(deb: Path) -> tuple[bytes, int]:
    """Concatenate every PEM CA in the ca-certificates .deb into one bundle.

    Reproduces ``update-ca-certificates`` with the default (all-enabled) config:
    pulls every ``usr/share/ca-certificates/**/*.crt`` regular-file member out of
    the .deb's data.tar (in-memory; tolerant of absolute-symlink members), checks
    each is PEM, and concatenates them sorted-by-path (deterministic), each cert
    newline-terminated. Returns ``(bundle_bytes, cert_count)``."""
    raw = _decompress_data_tar(deb)
    pems: list[tuple[str, bytes]] = []
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as t:
        for m in t.getmembers():
            rel = m.name
            while rel.startswith("./"):
                rel = rel[2:]
            rel = rel.lstrip("/")
            if rel.startswith(CA_SRC_PREFIX) and rel.endswith(".crt") and m.isfile():
                fh = t.extractfile(m)
                if fh is None:
                    continue
                body = fh.read()
                if b"-----BEGIN CERTIFICATE-----" not in body:
                    continue  # skip anything that isn't a PEM cert
                pems.append((rel, body))
    pems.sort(key=lambda kv: kv[0])
    chunks: list[bytes] = []
    for _, body in pems:
        chunks.append(body if body.endswith(b"\n") else body + b"\n")
    return b"".join(chunks), len(pems)


# --------------------------------------------------------------------------- #
# Full build
# --------------------------------------------------------------------------- #

def build_chromium_net_overlay(
    out_tar: str | Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path | None = None,
    test_page_src: str | Path = TEST_PAGE_SRC,
    ca_bundle: bytes | None = None,
    ca_cert_count: int | None = None,
    ca_deb_filename: str = "",
) -> ChromiumNetOverlayResult:
    """Pack the §5-E ``chromium-net-stage.tar``.

    NETWORK PATH unless ``ca_bundle`` is supplied (offline/selftest): fetches the
    noble ca-certificates .deb, assembles the CA bundle, then writes resolv.conf +
    nsswitch.conf + the CA bundle + the local CR-1 test page into one ./-rooted
    overlay tar at their rootfs-absolute paths.
    """
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    cache = Path(cache_dir) if cache_dir is not None else Path("/tmp/deb-cache-ubuntu")

    if ca_bundle is None:
        filename = resolve_ca_certs_filename(
            mirror=mirror, suite=suite, arch=arch, components=components
        )
        ca_deb_filename = filename
        deb = _download_deb(mirror, filename, cache)
        ca_bundle, ca_cert_count = assemble_ca_bundle(deb)
    elif ca_cert_count is None:
        ca_cert_count = ca_bundle.count(b"-----BEGIN CERTIFICATE-----")

    page_src = Path(test_page_src)
    page_bytes = page_src.read_bytes()

    members: list[str] = []
    with tarfile.open(out_tar, "w") as tar:
        # parent dirs (so a raw extractor that doesn't auto-mkdir is happy)
        for d in ("etc", "etc/ssl", "etc/ssl/certs", "root"):
            _add_dir(tar, d)
            members.append("./" + d)
        _add_file(tar, RESOLV_CONF_PATH, RESOLV_CONF_BODY.encode())
        members.append("./" + RESOLV_CONF_PATH)
        _add_file(tar, NSSWITCH_PATH, NSSWITCH_BODY.encode())
        members.append("./" + NSSWITCH_PATH)
        _add_file(tar, CA_BUNDLE_PATH, ca_bundle)
        members.append("./" + CA_BUNDLE_PATH)
        _add_file(tar, TEST_PAGE_PATH, page_bytes)
        members.append("./" + TEST_PAGE_PATH)

    with tarfile.open(out_tar, "r") as t:
        file_count = sum(1 for m in t.getmembers() if m.isfile())

    return ChromiumNetOverlayResult(
        out_tar=str(out_tar),
        members=tuple(sorted(members)),
        ca_cert_count=ca_cert_count,
        ca_bundle_bytes=len(ca_bundle),
        ca_deb_filename=ca_deb_filename,
        file_count=file_count,
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_chromium_net_overlay",
        description="Build the chromium NETWORK staging overlay (chromium-net-stage.tar): "
        "resolv.conf + nsswitch.conf + CA bundle + the CR-1 local test page.",
    )
    parser.add_argument("--out", help="output stage tar (e.g. /tmp/chromium-net-stage.tar)")
    parser.add_argument("--suite", default=SUITE)
    parser.add_argument("--mirror", default=MIRROR)
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; default main)")
    parser.add_argument("--cache", default="/tmp/deb-cache-ubuntu")
    parser.add_argument("--test-page", default=str(TEST_PAGE_SRC),
                        help="path to the CR-1 local HTML page to pack at /root/cr-test.html")
    parser.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                        help="resolve the ca-certificates .deb + print the planned overlay "
                        "members WITHOUT downloading/packing")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    components = tuple(args.components) if args.components else COMPONENTS

    if args.dry_run:
        filename = resolve_ca_certs_filename(
            mirror=args.mirror, suite=args.suite, arch=ARCH, components=components
        )
        planned = sorted([
            "./" + RESOLV_CONF_PATH,
            "./" + NSSWITCH_PATH,
            "./" + CA_BUNDLE_PATH,
            "./" + TEST_PAGE_PATH,
        ])
        info = {
            "ca_deb_filename": filename,
            "planned_members": planned,
            "test_page_src": args.test_page,
            "test_page_exists": Path(args.test_page).is_file(),
        }
        if args.json:
            print(json.dumps(info, indent=2))
        else:
            print(f"chromium-net overlay plan ({args.suite}/{'+'.join(components)}):")
            print(f"  ca-certificates .deb: {filename}")
            print(f"  test page src: {args.test_page} "
                  f"(exists={info['test_page_exists']})")
            print("  planned members:")
            for m in planned:
                print(f"    {m}")
        return 0

    if not args.out:
        parser.error("--out is required for a full build (or use --list / --selftest)")

    res = build_chromium_net_overlay(
        args.out, mirror=args.mirror, suite=args.suite, arch=ARCH,
        components=components, cache_dir=args.cache, test_page_src=args.test_page,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  files in overlay: {res.file_count}")
        print(f"  CA bundle: {res.ca_cert_count} certs, "
              f"{round(res.ca_bundle_bytes / 1024, 1)} KiB "
              f"(from {res.ca_deb_filename})")
        print("  members:")
        for m in res.members:
            print(f"    {m}")
    return 0


# --------------------------------------------------------------------------- #
# Selftest (OFFLINE — synthetic CA bundle + a real on-disk test page)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- the CR-1 test page exists + is self-contained (no external refs) --- #
    check("cr-test.html exists on disk", TEST_PAGE_SRC.is_file())
    page = TEST_PAGE_SRC.read_text(encoding="utf-8") if TEST_PAGE_SRC.is_file() else ""
    check("page has an <h1>", "<h1" in page)
    check("page has a CSS-colored div (#swatch)", "#swatch" in page and "background:" in page)
    check("page has an inline <svg>", "<svg" in page)
    # No external fetch: no absolute-URL src/href, no CSS @import of a URL, no
    # remote @font-face src. (Checks target real fetch directives, not prose.)
    no_ext = page_external_resources(page)
    check("page has no external resource (src/href/@import/@font-face URL)",
          no_ext == [])
    if no_ext:
        for hit in no_ext:
            print(f"        external ref: {hit!r}")

    # --- offline build with a synthetic 2-cert bundle ----------------------- #
    fake_cert = (
        b"-----BEGIN CERTIFICATE-----\nMIIBfakeAAA\n-----END CERTIFICATE-----\n"
    )
    bundle = fake_cert * 2
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "chromium-net-stage.tar"
        res = build_chromium_net_overlay(
            out, ca_bundle=bundle, ca_cert_count=2, ca_deb_filename="(synthetic)"
        )
        with tarfile.open(out) as t:
            names = {m.name: m for m in t.getmembers()}
            bodies = {}
            for n, m in names.items():
                if m.isfile():
                    bodies[n] = t.extractfile(m).read()
        check("overlay ships ./etc/resolv.conf", "./etc/resolv.conf" in names)
        check("overlay ships ./etc/nsswitch.conf", "./etc/nsswitch.conf" in names)
        check("overlay ships ./etc/ssl/certs/ca-certificates.crt",
              "./etc/ssl/certs/ca-certificates.crt" in names)
        check("overlay ships ./root/cr-test.html", "./root/cr-test.html" in names)
        check("all members ./-rooted", all(n.startswith("./") for n in names))
        check("resolv.conf has 8.8.8.8",
              b"nameserver 8.8.8.8" in bodies.get("./etc/resolv.conf", b""))
        check("resolv.conf has 1.1.1.1",
              b"nameserver 1.1.1.1" in bodies.get("./etc/resolv.conf", b""))
        check("nsswitch hosts: files dns",
              b"hosts:" in bodies.get("./etc/nsswitch.conf", b"")
              and b"dns" in bodies.get("./etc/nsswitch.conf", b""))
        check("CA bundle is the assembled PEM",
              bodies.get("./etc/ssl/certs/ca-certificates.crt") == bundle)
        check("packed page == on-disk page",
              bodies.get("./root/cr-test.html") == TEST_PAGE_SRC.read_bytes())
        check("result reports 2 certs", res.ca_cert_count == 2)
        check("result file_count == 4", res.file_count == 4)

        # §5-E structural conformance
        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(out))
        check("overlay is stage_tar_spec conformant", rep.conformant)

    # --- assemble_ca_bundle over a synthetic ca-certificates-like .deb ------ #
    import shutil
    import subprocess
    if shutil.which("ar"):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            root = tmp / "dataroot"
            (root / "usr/share/ca-certificates/mozilla").mkdir(parents=True)
            (root / "usr/share/ca-certificates/mozilla/A.crt").write_bytes(
                b"-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n")
            (root / "usr/share/ca-certificates/mozilla/B.crt").write_bytes(
                b"-----BEGIN CERTIFICATE-----\nBBBB\n-----END CERTIFICATE-----")  # no trailing \n
            # a non-cert file under the prefix must be ignored
            (root / "usr/share/ca-certificates/mozilla/README").write_bytes(b"not a cert\n")
            data_tar = tmp / "data.tar"
            with tarfile.open(data_tar, "w") as dt:
                dt.add(root / "usr/share/ca-certificates/mozilla/A.crt",
                       arcname="./usr/share/ca-certificates/mozilla/A.crt")
                dt.add(root / "usr/share/ca-certificates/mozilla/B.crt",
                       arcname="./usr/share/ca-certificates/mozilla/B.crt")
                dt.add(root / "usr/share/ca-certificates/mozilla/README",
                       arcname="./usr/share/ca-certificates/mozilla/README")
            (tmp / "debian-binary").write_text("2.0\n")
            (tmp / "control.tar").write_bytes(b"")
            subprocess.run(
                ["ar", "qcS", "synthetic-cacerts.deb", "debian-binary",
                 "control.tar", "data.tar"],
                check=True, capture_output=True, cwd=str(tmp),
            )
            b2, n2 = assemble_ca_bundle(tmp / "synthetic-cacerts.deb")
            check("assemble_ca_bundle finds 2 certs", n2 == 2)
            check("assemble_ca_bundle ignores non-cert README",
                  b"not a cert" not in b2)
            check("assemble_ca_bundle newline-terminates each cert (B got \\n)",
                  b2.endswith(b"-----END CERTIFICATE-----\n"))
            check("assemble_ca_bundle sorts by path (A before B)",
                  b2.index(b"AAAA") < b2.index(b"BBBB"))
    else:
        check("ar available for assemble_ca_bundle test (skipped)", True)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
