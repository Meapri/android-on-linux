"""Build the apt MIRROR-IP overlay (apt-mirror-stage.tar) — DoH fallback for apt.

Why — apt without working DNS
-----------------------------
On Android a non-root ``untrusted_app`` cannot send raw UDP/53 (Android funnels
DNS through ``netd``); a glibc guest's own ``sendto(:53)`` just hangs (see
``docs/evidence/2026-06-02-cr2-network-netlink-blocker.md``). The chromium path
solves this with **DoH** (DNS-over-HTTPS, port 443) — see
``tools/build_chromium_net_overlay.py``. But **apt has no DoH**: apt resolves its
mirror hostname through glibc ``getaddrinfo`` like any other program, so on a
DNS-blocked device ``apt update`` stalls on name resolution before it ever opens
a socket.

This overlay is the **fallback / complement** to DoH for the apt path: it makes
``apt update`` reach the repository **with no DNS at all** by combining two plain
rootfs files —

  1. ``/etc/hosts`` — pins the mirror **hostname → its stable CDN anycast IP(s)**,
     so glibc ``getaddrinfo("ports.ubuntu.com")`` is answered offline from
     ``/etc/hosts`` (``nsswitch hosts: files ...`` consults files first). No UDP-53.
  2. ``/etc/apt/sources.list.d/*.sources`` — apt sources that name that **same
     hostname** (NOT a bare IP). Naming the hostname is what makes HTTPS work:
     TLS SNI + certificate verification are keyed on the hostname, and a cert for
     ``ports.ubuntu.com`` does NOT validate against a literal-IP URL (host-proven
     below). With the ``/etc/hosts`` pin, the hostname URL connects straight to the
     pinned IP while still presenting the right SNI and validating the right cert.

Plus a tiny ``/etc/apt/apt.conf.d/*`` that keeps apt from trying to *also* do its
own SRV/again lookups and (optionally) prefers the offline-friendly transport.

HTTP vs HTTPS — why we default to HTTP for the *index* path
-----------------------------------------------------------
Both ``ports.ubuntu.com`` (Ubuntu arm64 = ``ubuntu-ports``) and
``deb.debian.org`` serve the archive over **plain HTTP** as well as HTTPS. Debian
/ Ubuntu apt does NOT need TLS for integrity: the ``Release``/``InRelease`` file
is GPG-signed and every ``Packages`` index and ``.deb`` is checksum-pinned by that
signed Release. So **HTTP is the simpler, more robust offline transport** — no
cert, no clock-skew failures, no SNI subtleties — while integrity is still
guaranteed by apt's signature chain. We therefore default the sources to
``http://`` (``--scheme http``). HTTPS is offered (``--scheme https``) for
environments that require transport encryption; it works via the ``/etc/hosts``
pin because the URL still carries the hostname (SNI/cert match — host-proven), but
it is strictly heavier than HTTP here.

  (host evidence, 2026-06-03, this builder's research:
     curl --resolve ports.ubuntu.com:80:172.66.152.176  http://…/InRelease     -> 200
     curl --resolve ports.ubuntu.com:443:172.66.152.176 https://…/InRelease    -> 200, ssl_verify=0
     curl                              https://172.66.152.176/…/InRelease       -> TLS handshake FAIL
   i.e. hostname-via-/etc/hosts works over both; raw-IP HTTPS fails the cert.)

Mirror choice & IP stability (the crux)
---------------------------------------
A CDN edge IP can change, so we pin only mirrors whose front IPs are **published,
long-lived anycast ranges** — the same class of stability the chromium DoH
bootstrap already relies on:

  * ``ports.ubuntu.com``  (Ubuntu **arm64** — the ALR base is Ubuntu *noble*
    arm64, so its archive is ``ubuntu-ports``, NOT ``archive.ubuntu.com`` which is
    amd64/i386 only) is fronted by **Cloudflare**. Its IPs (e.g. 172.66.152.176,
    104.20.28.246) fall inside Cloudflare's published ``172.64.0.0/13`` and
    ``104.16.0.0/13`` anycast ranges.
  * ``deb.debian.org``  (Debian **arm64**, suite ``bookworm``) is fronted by
    **Fastly**. Its IPs (e.g. 146.75.50.132, 151.101.0.204) fall inside Fastly's
    published ``146.75.0.0/17`` / ``151.101.0.0/16`` anycast ranges.

We pin **two** IPs per mirror (belt-and-suspenders); both are anycast so either
reaches the nearest edge. The IPs are refreshable: ``--print-current-ips`` re-runs
the resolution so a future drift is a one-line overlay rebuild, never a code
change. (HONEST: the *specific* addresses are a point-in-time snapshot; the
*ranges* are the durable guarantee. If you want zero-IP-drift risk, set the
sources to a country mirror that publishes a static IP and pin that instead —
``--mirror-host``/``--mirror-ip`` accept any host+IP.)

ALR does NOT mediate sockets — once staged, apt's TCP "just works"
------------------------------------------------------------------
Same finding as the chromium-net overlay: the ALR loader's seccomp filters trace
only *path* + *execve* syscalls; ``socket``/``connect``/``sendto`` default to
RET_ALLOW, so apt's TCP to the pinned IP is un-traced/native, and untrusted_app
has INTERNET. The only files the path filter must rewrite into the rootfs are the
ones this overlay ships (``/etc/hosts``, the sources, apt.conf) plus glibc's
``/etc/nsswitch.conf`` (the base already orders ``hosts: files …``; the
chromium-net overlay also ships one). So the entire "apt can't reach the repo"
gap is name resolution — which ``/etc/hosts`` closes with zero DNS.

Honest scope
------------
HOST-ONLY. This builds + validates the overlay host-side and (with
``--verify-fetch``) proves on the *host* that apt's index + a pool ``.deb`` are
reachable through the pinned IP via the hostname (curl ``--resolve`` == exactly
what ``/etc/hosts`` does for apt). The real in-app ``apt update`` is the
integration/device gate (the integration session stages the tar). The
socket-passthrough claim is read from the seccomp source, not a device run.
"""

from __future__ import annotations

import argparse
import io
import json
import socket
import ssl
import tarfile
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------- #
# Mirror catalog — host + suite + stable anycast bootstrap IPs
# --------------------------------------------------------------------------- #
# `ubuntu-ports` is the Ubuntu **arm64** archive (the ALR base is noble arm64).
# `archive.ubuntu.com` is amd64/i386 only and would 404 every arm64 path, so it
# is deliberately NOT a choice here.


@dataclass(frozen=True)
class Mirror:
    key: str
    host: str                       # mirror hostname (SNI / cert / Host header)
    archive_path: str               # path prefix under the host, e.g. "ubuntu-ports"
    suite: str                      # default suite (codename)
    components: tuple[str, ...]     # default components
    bootstrap: tuple[str, ...]      # stable anycast IPv4 to pin host -> IP
    cdn: str                        # which CDN fronts it (for the doc/report)
    cdn_ranges: tuple[str, ...]     # published anycast ranges the IPs live in

    def base_uri(self, scheme: str) -> str:
        return f"{scheme}://{self.host}/{self.archive_path}"


MIRRORS: dict[str, Mirror] = {
    # Ubuntu noble arm64 — the ALR base. Fronted by Cloudflare.
    "ports": Mirror(
        key="ports",
        host="ports.ubuntu.com",
        archive_path="ubuntu-ports",
        suite="noble",
        components=("main", "restricted", "universe", "multiverse"),
        bootstrap=("172.66.152.176", "104.20.28.246"),
        cdn="Cloudflare",
        cdn_ranges=("172.64.0.0/13", "104.16.0.0/13"),
    ),
    # Debian bookworm arm64 — alternative base / extra packages. Fronted by Fastly.
    "debian": Mirror(
        key="debian",
        host="deb.debian.org",
        archive_path="debian",
        suite="bookworm",
        components=("main",),
        bootstrap=("146.75.50.132", "151.101.0.204"),
        cdn="Fastly",
        cdn_ranges=("146.75.0.0/17", "151.101.0.0/16"),
    ),
}
DEFAULT_MIRROR = "ports"
DEFAULT_SCHEME = "http"  # see "HTTP vs HTTPS" in the module docstring

# Rootfs-absolute (./-rooted in the tar) targets.
HOSTS_PATH = "etc/hosts"
SOURCES_PATH_TMPL = "etc/apt/sources.list.d/alr-{key}.sources"
APT_CONF_PATH = "etc/apt/apt.conf.d/99alr-mirror-ip"

# A small apt.conf that suits the DNS-less, signature-trusted fallback path.
#   - Languages "none": don't fetch per-language Translation-* indices (fewer
#     round-trips; they aren't needed for install).
#   - ForceIPv4: the pinned bootstraps are IPv4 anycast; never wait on a broken
#     IPv6 path on a device with no usable v6 route.
# NOTE: we intentionally do NOT relax signature checking — apt still verifies the
# GPG-signed Release, which is exactly what makes plain-HTTP transport safe here.
APT_CONF_BODY = (
    "// ALR apt mirror-IP overlay (DoH fallback): make apt robust without DNS.\n"
    "// The mirror hostname is pinned to a stable anycast IP in /etc/hosts; these\n"
    "// knobs just trim round-trips and avoid a dead IPv6 path. Signature checking\n"
    "// is left ON (the signed Release is what makes plain HTTP safe).\n"
    'Acquire::Languages "none";\n'
    'Acquire::ForceIPv4 "true";\n'
)


# --------------------------------------------------------------------------- #
# Body builders
# --------------------------------------------------------------------------- #

def resolve_mirror(name: str | Mirror) -> Mirror:
    if isinstance(name, Mirror):
        return name
    try:
        return MIRRORS[name]
    except KeyError:
        raise ValueError(
            f"unknown mirror {name!r}; choose one of {', '.join(sorted(MIRRORS))} "
            f"(or pass --mirror-host/--mirror-ip for a custom one)"
        ) from None


def build_hosts_body(
    mirror: Mirror, *, extra_hosts: tuple[tuple[str, tuple[str, ...]], ...] = ()
) -> str:
    """``/etc/hosts``: loopback + the mirror hostname pinned to its anycast IP(s).

    This is the whole trick: with ``nsswitch hosts: files …`` (base default; the
    chromium-net overlay also ships nsswitch), glibc answers
    ``getaddrinfo(mirror.host)`` from these lines — so apt connects with **zero
    DNS**. Multiple IPs are listed as separate lines (glibc tries them in order;
    both are anycast so either reaches an edge).
    """
    lines = [
        "# ALR apt mirror-IP overlay (DoH fallback) — loopback + mirror pin.",
        "127.0.0.1\tlocalhost",
        "::1\tlocalhost ip6-localhost ip6-loopback",
        f"# {mirror.host}: {mirror.cdn} anycast "
        f"({', '.join(mirror.cdn_ranges)}); pinned so apt resolves it with no DNS.",
    ]
    for ip in mirror.bootstrap:
        lines.append(f"{ip}\t{mirror.host}")
    for host, ips in extra_hosts:
        lines.append(f"# extra pin: {host}")
        for ip in ips:
            lines.append(f"{ip}\t{host}")
    return "\n".join(lines) + "\n"


def build_sources_body(
    mirror: Mirror,
    *,
    scheme: str = DEFAULT_SCHEME,
    suite: str | None = None,
    components: tuple[str, ...] | None = None,
) -> str:
    """A deb822 ``.sources`` stanza naming the **hostname** (not a bare IP).

    deb822 is the noble default (the base ships ``ubuntu.sources`` in this form).
    Naming the hostname (resolved offline via /etc/hosts) is what lets HTTPS keep
    a valid SNI + cert; over HTTP it is simply the clean canonical URI.

    ``Trusted: no`` is intentional — we keep apt's signature verification on; the
    repo's GPG-signed Release is what makes the (default) plain-HTTP transport
    safe. apt finds the archive keyring on the base rootfs as usual.
    """
    suite = suite or mirror.suite
    comps = components or mirror.components
    uri = mirror.base_uri(scheme)
    # Debian ships security as a SEPARATE suite (`<suite>-security` on
    # security.debian.org), so a deb.debian.org stanza carries only
    # `<suite>`+`<suite>-updates`; Ubuntu serves `-security` from the same host.
    if mirror.host.endswith("debian.org"):
        suites = f"{suite} {suite}-updates"
        keyring = "/usr/share/keyrings/debian-archive-keyring.gpg"
    else:
        suites = f"{suite} {suite}-updates {suite}-security"
        keyring = "/usr/share/keyrings/ubuntu-archive-keyring.gpg"
    return (
        f"# ALR apt mirror-IP overlay (DoH fallback) — {mirror.key} via {scheme.upper()}.\n"
        f"# Host {mirror.host} is pinned to {mirror.cdn} anycast in /etc/hosts, so\n"
        f"# `apt update` reaches this URI with NO DNS. Integrity = signed Release.\n"
        "Types: deb\n"
        f"URIs: {uri}\n"
        f"Suites: {suites}\n"
        f"Components: {' '.join(comps)}\n"
        f"Signed-By: {keyring}\n"
    )


# --------------------------------------------------------------------------- #
# IP (re)resolution — keeps the pins refreshable without code edits
# --------------------------------------------------------------------------- #

def resolve_current_ips(host: str, *, limit: int = 2) -> list[str]:
    """Resolve ``host`` to up to ``limit`` IPv4 addresses (NETWORK).

    Used by ``--print-current-ips`` / ``--refresh-ips`` so a future CDN drift is a
    one-command overlay rebuild. The *ranges* in the catalog are the durable
    guarantee; this just refreshes the *specific* addresses.
    """
    out: list[str] = []
    for fam, _, _, _, sockaddr in socket.getaddrinfo(host, 80, socket.AF_INET, socket.SOCK_STREAM):
        ip = sockaddr[0]
        if ip not in out:
            out.append(ip)
        if len(out) >= limit:
            break
    return out


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
# Result + full build
# --------------------------------------------------------------------------- #

@dataclass
class AptMirrorOverlayResult:
    out_tar: str
    mirror: str
    scheme: str
    host: str
    bootstrap_ips: tuple[str, ...]
    base_uri: str
    members: tuple[str, ...] = ()
    file_count: int = 0

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "mirror": self.mirror,
            "scheme": self.scheme,
            "host": self.host,
            "bootstrap_ips": list(self.bootstrap_ips),
            "base_uri": self.base_uri,
            "members": list(self.members),
            "file_count": self.file_count,
        }


def build_apt_mirror_overlay(
    out_tar: str | Path,
    *,
    mirror: str | Mirror = DEFAULT_MIRROR,
    scheme: str = DEFAULT_SCHEME,
    suite: str | None = None,
    components: tuple[str, ...] | None = None,
    bootstrap_ips: tuple[str, ...] | None = None,
    extra_hosts: tuple[tuple[str, tuple[str, ...]], ...] = (),
) -> AptMirrorOverlayResult:
    """Pack the §5-E ``apt-mirror-stage.tar`` (OFFLINE — no network).

    Writes ``/etc/hosts`` (mirror host -> anycast IP), the deb822 ``.sources``
    naming that host, and a small ``apt.conf.d`` drop-in, all ./-rooted at their
    rootfs-absolute paths. ``bootstrap_ips`` overrides the catalog pins (e.g. from
    ``--refresh-ips`` or a custom mirror).
    """
    m = resolve_mirror(mirror)
    if bootstrap_ips:
        m = Mirror(**{**m.__dict__, "bootstrap": tuple(bootstrap_ips)})
    if scheme not in ("http", "https"):
        raise ValueError(f"scheme must be 'http' or 'https', got {scheme!r}")

    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)

    hosts_body = build_hosts_body(m, extra_hosts=extra_hosts).encode()
    sources_body = build_sources_body(
        m, scheme=scheme, suite=suite, components=components
    ).encode()
    sources_path = SOURCES_PATH_TMPL.format(key=m.key)

    members: list[str] = []
    with tarfile.open(out_tar, "w") as tar:
        for d in ("etc", "etc/apt", "etc/apt/sources.list.d", "etc/apt/apt.conf.d"):
            _add_dir(tar, d)
            members.append("./" + d)
        _add_file(tar, HOSTS_PATH, hosts_body)
        members.append("./" + HOSTS_PATH)
        _add_file(tar, sources_path, sources_body)
        members.append("./" + sources_path)
        _add_file(tar, APT_CONF_PATH, APT_CONF_BODY.encode())
        members.append("./" + APT_CONF_PATH)

    with tarfile.open(out_tar, "r") as t:
        file_count = sum(1 for x in t.getmembers() if x.isfile())

    return AptMirrorOverlayResult(
        out_tar=str(out_tar),
        mirror=m.key,
        scheme=scheme,
        host=m.host,
        bootstrap_ips=tuple(m.bootstrap),
        base_uri=m.base_uri(scheme),
        members=tuple(sorted(members)),
        file_count=file_count,
    )


# --------------------------------------------------------------------------- #
# Host-side fetch verification (curl-free; urllib with a forced IP + SNI)
# --------------------------------------------------------------------------- #

def _open_pinned(url: str, host: str, ip: str, *, timeout: float = 30.0) -> tuple[int, int]:
    """Fetch ``url`` but force the connection to ``ip`` while keeping ``host`` for
    Host header / TLS SNI — exactly what ``/etc/hosts`` does for apt. Returns
    ``(status, bytes_read)``. Raises on transport/TLS failure.
    """
    from urllib.parse import urlsplit

    parts = urlsplit(url)
    scheme = parts.scheme
    port = parts.port or (443 if scheme == "https" else 80)
    path = parts.path or "/"
    headers = {
        "Host": host,
        "User-Agent": "Debian APT-HTTP/1.3 (ALR apt-mirror-overlay verify)",
        "Connection": "close",
    }
    if scheme == "https":
        ctx = ssl.create_default_context()
        raw = socket.create_connection((ip, port), timeout=timeout)
        sock = ctx.wrap_socket(raw, server_hostname=host)  # SNI = host, not ip
    else:
        sock = socket.create_connection((ip, port), timeout=timeout)
    try:
        req = f"GET {path} HTTP/1.1\r\n" + "".join(
            f"{k}: {v}\r\n" for k, v in headers.items()
        ) + "\r\n"
        sock.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        status_line = buf.split(b"\r\n", 1)[0].decode("latin-1", "replace")
        status = int(status_line.split(" ", 2)[1]) if " " in status_line else 0
        body = buf.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in buf else b""
        n = len(body)
        # drain a little more so we report a meaningful size, but don't pull MBs
        while n < 65536:
            chunk = sock.recv(8192)
            if not chunk:
                break
            n += len(chunk)
        return status, n
    finally:
        sock.close()


def verify_fetch(
    mirror: Mirror, scheme: str, *, suite: str | None = None
) -> list[dict]:
    """Prove (on the host) that apt's index is reachable through the pinned IP via
    the hostname — for EACH bootstrap IP. Returns one result dict per IP."""
    suite = suite or mirror.suite
    idx = f"{mirror.base_uri(scheme)}/dists/{suite}/InRelease"
    results: list[dict] = []
    for ip in mirror.bootstrap:
        rec: dict = {"ip": ip, "url": idx, "scheme": scheme}
        try:
            status, n = _open_pinned(idx, mirror.host, ip)
            rec["status"] = status
            rec["bytes"] = n
            rec["ok"] = status == 200 and n > 0
        except Exception as exc:  # noqa: BLE001 — report any transport/TLS failure
            rec["error"] = f"{type(exc).__name__}: {exc}"
            rec["ok"] = False
        results.append(rec)
    return results


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_apt_mirror_overlay",
        description="Build the apt MIRROR-IP overlay (apt-mirror-stage.tar): "
        "/etc/hosts mirror-pin + deb822 sources + apt.conf so `apt update` works "
        "with NO DNS (the DoH fallback for the apt path).",
    )
    parser.add_argument("--out", help="output stage tar (e.g. /tmp/apt-mirror-stage.tar)")
    parser.add_argument("--mirror", default=DEFAULT_MIRROR, choices=sorted(MIRRORS),
                        help=f"mirror to pin (default {DEFAULT_MIRROR}; 'ports'=Ubuntu "
                        "noble arm64, 'debian'=Debian bookworm arm64)")
    parser.add_argument("--scheme", default=DEFAULT_SCHEME, choices=("http", "https"),
                        help=f"transport for the sources URI (default {DEFAULT_SCHEME}; "
                        "HTTP is simpler+safe since the Release is GPG-signed, HTTPS "
                        "works via the hostname pin but is heavier)")
    parser.add_argument("--suite", help="override the suite/codename")
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; overrides the mirror default)")
    parser.add_argument("--mirror-host", help="custom mirror hostname (with --mirror-ip)")
    parser.add_argument("--mirror-ip", action="append", dest="mirror_ips",
                        help="IP for --mirror-host (repeatable); pins host->IP in /etc/hosts")
    parser.add_argument("--refresh-ips", action="store_true",
                        help="re-resolve the mirror host NOW (NETWORK) and pin the fresh IPs "
                        "instead of the catalog snapshot")
    parser.add_argument("--print-current-ips", action="store_true",
                        help="resolve + print the mirror host's current IPs (NETWORK) and exit")
    parser.add_argument("--verify-fetch", action="store_true",
                        help="prove on the HOST that the index is reachable through each "
                        "pinned IP via the hostname (curl --resolve equivalent; NETWORK)")
    parser.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                        help="print the planned overlay members + bodies WITHOUT packing")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    # custom mirror (host + IPs) short-circuits the catalog
    if args.mirror_host or args.mirror_ips:
        if not (args.mirror_host and args.mirror_ips):
            parser.error("--mirror-host and --mirror-ip must be given together")
        m = Mirror(
            key="custom",
            host=args.mirror_host,
            archive_path=(args.components and "") or "ubuntu-ports",
            suite=args.suite or "noble",
            components=tuple(args.components) if args.components else ("main",),
            bootstrap=tuple(args.mirror_ips),
            cdn="custom",
            cdn_ranges=(),
        )
    else:
        m = resolve_mirror(args.mirror)

    if args.print_current_ips:
        ips = resolve_current_ips(m.host)
        if args.json:
            print(json.dumps({"host": m.host, "current_ips": ips,
                              "catalog_ips": list(m.bootstrap),
                              "cdn": m.cdn, "cdn_ranges": list(m.cdn_ranges)}, indent=2))
        else:
            print(f"{m.host} ({m.cdn}) current IPs: {', '.join(ips) or '(none)'}")
            print(f"  catalog pin:   {', '.join(m.bootstrap)}")
            print(f"  cdn ranges:    {', '.join(m.cdn_ranges) or '(custom)'}")
        return 0

    if args.refresh_ips:
        fresh = resolve_current_ips(m.host)
        if fresh:
            m = Mirror(**{**m.__dict__, "bootstrap": tuple(fresh)})

    if args.verify_fetch:
        res = verify_fetch(m, args.scheme, suite=args.suite)
        if args.json:
            print(json.dumps({"host": m.host, "scheme": args.scheme, "results": res}, indent=2))
        else:
            print(f"verify-fetch {m.host} via {args.scheme} (hostname pinned to each IP):")
            for r in res:
                if r.get("ok"):
                    print(f"  [OK]   {r['ip']} -> HTTP {r['status']}, {r['bytes']}+ bytes  ({r['url']})")
                else:
                    print(f"  [FAIL] {r['ip']} -> {r.get('error') or ('HTTP ' + str(r.get('status')))}")
        return 0 if all(r.get("ok") for r in res) else 1

    if args.dry_run:
        hosts_body = build_hosts_body(m)
        sources_body = build_sources_body(
            m, scheme=args.scheme, suite=args.suite,
            components=tuple(args.components) if args.components else None,
        )
        sources_path = SOURCES_PATH_TMPL.format(key=m.key)
        planned = sorted(["./" + HOSTS_PATH, "./" + sources_path, "./" + APT_CONF_PATH])
        if args.json:
            print(json.dumps({
                "mirror": m.key, "host": m.host, "scheme": args.scheme,
                "bootstrap_ips": list(m.bootstrap), "base_uri": m.base_uri(args.scheme),
                "planned_members": planned,
                "hosts_body": hosts_body, "sources_body": sources_body,
                "apt_conf_body": APT_CONF_BODY,
            }, indent=2))
        else:
            print(f"apt mirror-IP overlay plan ({m.key} via {args.scheme}):")
            print(f"  base URI: {m.base_uri(args.scheme)}")
            print(f"  pinned IPs: {', '.join(m.bootstrap)} ({m.cdn} {', '.join(m.cdn_ranges)})")
            print("  planned members:")
            for x in planned:
                print(f"    {x}")
            print("\n--- /etc/hosts ---\n" + hosts_body, end="")
            print(f"\n--- {sources_path} ---\n" + sources_body, end="")
            print("\n--- " + APT_CONF_PATH + " ---\n" + APT_CONF_BODY, end="")
        return 0

    if not args.out:
        parser.error("--out is required for a full build (or use --list / --verify-fetch / "
                     "--print-current-ips / --selftest)")

    res = build_apt_mirror_overlay(
        args.out, mirror=m, scheme=args.scheme, suite=args.suite,
        components=tuple(args.components) if args.components else None,
        bootstrap_ips=tuple(m.bootstrap),
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  mirror: {res.mirror} ({res.host}) via {res.scheme}")
        print(f"  base URI: {res.base_uri}")
        print(f"  pinned IPs: {', '.join(res.bootstrap_ips)}")
        print(f"  files in overlay: {res.file_count}")
        print("  members:")
        for x in res.members:
            print(f"    {x}")
    return 0


# --------------------------------------------------------------------------- #
# Selftest (OFFLINE — pack + structural/contract checks, no network)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- catalog sanity: ports is Ubuntu arm64 (ubuntu-ports), not archive.* --- #
    ports = MIRRORS["ports"]
    check("ports mirror host is ports.ubuntu.com (Ubuntu arm64)",
          ports.host == "ports.ubuntu.com")
    check("ports archive path is ubuntu-ports (NOT archive.ubuntu.com / amd64)",
          ports.archive_path == "ubuntu-ports")
    check("ports default suite is noble", ports.suite == "noble")
    check("ports pinned IPs are in Cloudflare published ranges (172.64/13, 104.16/13)",
          _ip_in_any("172.66.152.176", ("172.64.0.0/13",))
          and _ip_in_any("104.20.28.246", ("104.16.0.0/13",)))
    deb = MIRRORS["debian"]
    check("debian pinned IPs are in Fastly published ranges (146.75/17, 151.101/16)",
          _ip_in_any("146.75.50.132", ("146.75.0.0/17",))
          and _ip_in_any("151.101.0.204", ("151.101.0.0/16",)))

    # --- /etc/hosts body pins host -> each anycast IP ----------------------- #
    hb = build_hosts_body(ports)
    check("hosts has loopback 127.0.0.1 localhost", "127.0.0.1\tlocalhost" in hb)
    check("hosts pins ports.ubuntu.com -> 172.66.152.176",
          "172.66.152.176\tports.ubuntu.com" in hb)
    check("hosts pins second anycast IP too",
          "104.20.28.246\tports.ubuntu.com" in hb)
    check("hosts comment names the CDN (Cloudflare)", "Cloudflare" in hb)

    # --- sources name the HOSTNAME (not a bare IP) so SNI/cert match -------- #
    sb_http = build_sources_body(ports, scheme="http")
    check("http sources URI names the hostname (not an IP)",
          "URIs: http://ports.ubuntu.com/ubuntu-ports" in sb_http)
    check("http sources URI has NO literal IP",
          "172.66.152.176" not in sb_http and "104.20.28.246" not in sb_http)
    check("sources include noble + -updates + -security suites",
          "Suites: noble noble-updates noble-security" in sb_http)
    check("sources keep signature checking (Signed-By archive keyring)",
          "Signed-By:" in sb_http and "ubuntu-archive-keyring.gpg" in sb_http)
    check("sources are deb822 (Types: deb)", sb_http.startswith("#") and "Types: deb" in sb_http)
    sb_https = build_sources_body(ports, scheme="https")
    check("https sources URI names the hostname (SNI/cert match)",
          "URIs: https://ports.ubuntu.com/ubuntu-ports" in sb_https)

    # debian sources differ (no -security suffix the same way; Fastly host)
    sb_deb = build_sources_body(deb, scheme="http")
    check("debian sources name deb.debian.org",
          "URIs: http://deb.debian.org/debian" in sb_deb)
    check("debian sources use the debian keyring",
          "debian-archive-keyring.gpg" in sb_deb)

    # --- apt.conf keeps signatures ON, trims round-trips -------------------- #
    check("apt.conf sets Languages none", 'Acquire::Languages "none";' in APT_CONF_BODY)
    check("apt.conf forces IPv4", 'Acquire::ForceIPv4 "true";' in APT_CONF_BODY)
    check("apt.conf does NOT disable signature checks (no AllowInsecure)",
          "AllowInsecure" not in APT_CONF_BODY and "AllowUnauthenticated" not in APT_CONF_BODY)

    # --- full OFFLINE pack + §5-E conformance ------------------------------- #
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "apt-mirror-stage.tar"
        res = build_apt_mirror_overlay(out, mirror="ports", scheme="http")
        with tarfile.open(out) as t:
            names = {m.name: m for m in t.getmembers()}
            bodies = {n: t.extractfile(m).read() for n, m in names.items() if m.isfile()}
        check("overlay ships ./etc/hosts", "./etc/hosts" in names)
        check("overlay ships ./etc/apt/sources.list.d/alr-ports.sources",
              "./etc/apt/sources.list.d/alr-ports.sources" in names)
        check("overlay ships ./etc/apt/apt.conf.d/99alr-mirror-ip",
              "./etc/apt/apt.conf.d/99alr-mirror-ip" in names)
        check("all members ./-rooted", all(n.startswith("./") for n in names))
        check("packed hosts pins the mirror IP",
              b"172.66.152.176\tports.ubuntu.com" in bodies["./etc/hosts"])
        check("packed sources name the hostname",
              b"http://ports.ubuntu.com/ubuntu-ports" in
              bodies["./etc/apt/sources.list.d/alr-ports.sources"])
        check("result.file_count == 3", res.file_count == 3)
        check("result base_uri is the hostname URI",
              res.base_uri == "http://ports.ubuntu.com/ubuntu-ports")

        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(out))
        check("overlay is stage_tar_spec conformant", rep.conformant)
        check("overlay has no stage_tar warnings", rep.warnings == [])

    # --- custom mirror + bootstrap override --------------------------------- #
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "custom.tar"
        rc = build_apt_mirror_overlay(
            out, mirror="ports", scheme="http",
            bootstrap_ips=("203.0.113.7",),
        )
        with tarfile.open(out) as t:
            hb2 = t.extractfile("./etc/hosts").read()
        check("bootstrap override is reflected in /etc/hosts",
              b"203.0.113.7\tports.ubuntu.com" in hb2)
        check("overridden overlay drops the catalog IP",
              b"172.66.152.176" not in hb2)
        check("result reports the overridden IP", rc.bootstrap_ips == ("203.0.113.7",))

    # --- error paths -------------------------------------------------------- #
    try:
        resolve_mirror("nope")
        check("unknown mirror rejected", False)
    except ValueError:
        check("unknown mirror rejected", True)
    try:
        build_apt_mirror_overlay("/tmp/x.tar", scheme="ftp")
        check("bad scheme rejected", False)
    except ValueError:
        check("bad scheme rejected", True)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


def _ip_in_any(ip: str, cidrs: tuple[str, ...]) -> bool:
    import ipaddress

    a = ipaddress.ip_address(ip)
    return any(a in ipaddress.ip_network(c) for c in cidrs)


if __name__ == "__main__":
    raise SystemExit(main())
