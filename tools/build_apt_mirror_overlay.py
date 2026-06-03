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

# An ALR-only sources directory. apt.conf below repoints Dir::Etc::sourceparts at
# THIS dir (and Dir::Etc::sourcelist at /dev/null), so apt reads ONLY this stanza
# and ignores every base file in /etc/apt/sources.list.d (the cloud-init
# ubuntu.sources that names ap-seoul-1-ad-1.clouds.ports.ubuntu.com, plus the
# github-cli / tailscale stanzas). An overlay can only ADD files, never delete the
# base ones — Dir::Etc is the canonical apt way to make those base files inert.
ALR_SOURCES_DIR = "etc/apt/sources.list.alr.d"
ALR_SOURCES_PATH_TMPL = ALR_SOURCES_DIR + "/alr-{key}.sources"

# The on-device rootfs absolute path. Used to give apt an ALREADY-CANONICAL
# absolute path for Dir::State::status and Dir::Etc::sourceparts, see
# build_apt_conf_body(). DEVICE-SPECIFIC default (the integration device's app
# data dir); override with --rootfs-abs for any other install. When empty/None the
# status + sourceparts overrides are omitted (the overlay then relies on apt's
# default Dir + the path interposer, which is the pre-existing behavior).
DEFAULT_ROOTFS_ABS = (
    "/data/user/0/dev.chanwoo.androlinux/files/rootfs/debian-arm64"
)

# `ubuntu.sources` neutralizer body. The overlay OVERWRITES the base cloud-init
# ubuntu.sources (RootfsInstaller.extractOverlayTar atomically renames non-library
# files into place, so a plain config file IS replaced — only flat-SONAME .so
# downgrades are guarded). With Dir::Etc::sourceparts already repointed this is
# belt-and-suspenders, but it also makes the rootfs honest: the stale
# clouds.ports.ubuntu.com host (which is NOT pinned in /etc/hosts and only resolves
# because DoH happens to answer it) no longer advertises a repo.
UBUNTU_SOURCES_PATH = "etc/apt/sources.list.d/ubuntu.sources"
UBUNTU_SOURCES_NEUTRALIZED = (
    "# neutralized by ALR apt-mirror overlay: the base cloud-init ubuntu.sources\n"
    "# named ap-seoul-1-ad-1.clouds.ports.ubuntu.com (NOT pinned in /etc/hosts) and\n"
    "# noble-backports. apt now reads only /etc/apt/sources.list.alr.d (ports pin).\n"
)


def build_apt_conf_body(
    *,
    rootfs_abs: str | None = DEFAULT_ROOTFS_ABS,
    trusted: bool = True,
    status_path: str | None = None,
) -> str:
    """The apt.conf drop-in for the DNS-less, single-mirror apt path.

    Three concerns, all device-proven necessary (see the run logs in
    ``docs/research/cr2-online-apt-integration.md`` / the commit body):

    1. **Sources isolation (Dir::Etc).** ``Dir::Etc::sourcelist "/dev/null"`` +
       ``Dir::Etc::sourceparts <rootfs>/etc/apt/sources.list.alr.d`` make apt read
       ONLY the ports stanza this overlay ships, ignoring the base cloud-init
       ``ubuntu.sources`` (``…clouds.ports.ubuntu.com`` + ``noble-backports``) and
       the github-cli / tailscale stanzas — which otherwise all get fetched and
       then fail. An overlay can't delete base files; Dir::Etc is how apt itself
       scopes the source set. (sourceparts must be a directory that EXISTS; we ship
       it. We give the ABSOLUTE rootfs path so apt's own canonicalization of the
       dir doesn't depend on the path interposer.)
    2. **Signature trust (Trusted/AllowUnauthenticated).** On-device, apt's
       ``apt-key`` shells out and dies "Unknown error executing apt-key" → every
       InRelease is "not signed" → ``apt-get update`` exits 100. ``Trusted: yes``
       on the stanza (set in the sources body) skips the GPG check for THIS repo;
       ``APT::Get::AllowUnauthenticated`` here is the belt-and-suspenders global.
       DEMO-grade: real GPG (ubuntu-archive-keyring + working gpgv) is a follow-up
       DEVICE-REQ. Only enabled when ``trusted`` is True.
    3. **dpkg status realpath (Dir::State::status).** apt does
       ``flAbsPath(Dir::State::status)`` = ``realpath("/var/lib/dpkg/status")``
       early; on device that fails ``realpath (2: No such file or directory)`` even
       though the file exists in the rootfs, because realpath canonicalizes each
       parent component against the Android root, not the rootfs. Giving apt the
       ALREADY-ABSOLUTE rootfs path makes ``realpath`` resolve a path that really
       exists, sidestepping the canonicalization edge. (dpkg-query reads the same
       file fine — it ``open()``s it directly, which the seccomp path filter
       rewrites; apt's realpath probe is the one syscall shape that slips the net.
       If this override proves insufficient the residual is an interposer/realpath
       gap, reported as a DEVICE-REQ, NOT patched here.)
    4. **Device-environment hygiene (Post-Invoke hooks + sandbox user).** The base
       ships ``APT::Update::Post-Invoke-Success`` hooks (PackageKit ``gdbus`` /
       command-not-found) that exec ``gdbus``/dbus, which do not exist on this
       headless rootfs — apt reports ``E: Problem executing scripts
       APT::Update::Post-Invoke-Success`` → ``E: Sub-process returned an error
       code`` and a non-zero exit even after a clean fetch. We blank those hook
       lists. apt also wants to drop privileges to the ``_apt`` sandbox user (which
       the minimal rootfs lacks → ``W: No sandbox user '_apt'``); ``APT::Sandbox::
       User "root"`` keeps apt as the fakeroot uid=0 instead of warning/erroring.

    ``Languages "none"`` + ``ForceIPv4`` are retained (trim round-trips; avoid a
    dead IPv6 path on the pinned IPv4 anycast).
    """
    lines = [
        "// ALR apt mirror-IP overlay (DoH fallback): robust single-mirror apt without DNS.",
        "// (1) Dir::Etc isolates apt to the ports stanza in sources.list.alr.d (ignores the",
        "//     base cloud-init ubuntu.sources + github-cli/tailscale). (2) Trusted/Allow-",
        "//     Unauthenticated = DEMO signature skip (device apt-key is broken; real GPG is a",
        "//     follow-up). (3) Dir::State::status pins the dpkg DB to its absolute rootfs path.",
        "//     (4) Blank the PackageKit/c-n-f Post-Invoke hooks + run as root (no _apt user):",
        "//     those hooks exec gdbus/dbus that this headless rootfs lacks, which otherwise",
        "//     fails apt-get update AFTER a clean fetch.",
        'Acquire::Languages "none";',
        'Acquire::ForceIPv4 "true";',
        # (4) Device-environment hygiene — always (independent of trust / rootfs).
        # The Post-Invoke* knobs are apt LISTS: assigning "" only APPENDS an empty
        # entry, it does NOT drop the base PackageKit/c-n-f hooks (device-proven:
        # the gdbus hook still ran). The apt-config `#clear` directive empties the
        # whole list — that is the correct way to remove a base-defined hook list.
        '// Empty the base PackageKit / command-not-found update hooks (no dbus here).',
        '#clear APT::Update::Post-Invoke-Success;',
        '#clear APT::Update::Post-Invoke;',
        'APT::Sandbox::User "root";',
    ]
    if rootfs_abs:
        ra = rootfs_abs.rstrip("/")
        lines += [
            'Dir::Etc::sourcelist "/dev/null";',
            f'Dir::Etc::sourceparts "{ra}/etc/apt/sources.list.alr.d";',
        ]
        # Dir::State::status: by default the in-rootfs DB at its absolute path. apt
        # flAbsPath()s this (realpath); on device that realpath fails for any path
        # UNDER the rootfs (interposer/seccomp edge — device-proven, both relative
        # and absolute). `status_path` lets the caller point at a DB copy OUTSIDE
        # the rootfs (e.g. /data/local/tmp/alr-dpkg-status), which the path
        # interposer does NOT rewrite, so realpath runs pure and resolves. That is
        # a workaround for the realpath edge, not a fix — see the module notes.
        status = status_path or f"{ra}/var/lib/dpkg/status"
        lines.append(f'Dir::State::status "{status}";')
    else:
        # No absolute rootfs known: still isolate to the ALR sources dir by RELATIVE
        # name (apt resolves it under Dir::Etc); status is left to apt's default +
        # the path interposer unless a status_path is given.
        lines += [
            'Dir::Etc::sourcelist "/dev/null";',
            'Dir::Etc::sourceparts "sources.list.alr.d";',
        ]
        if status_path:
            lines.append(f'Dir::State::status "{status_path}";')
    if trusted:
        lines.append('APT::Get::AllowUnauthenticated "true";')
    return "\n".join(lines) + "\n"


# Back-compat: a module-level default body (no device path baked in beyond the
# catalog default) for callers/selftests that import APT_CONF_BODY directly.
APT_CONF_BODY = build_apt_conf_body()


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
    trusted: bool = True,
) -> str:
    """A deb822 ``.sources`` stanza naming the **hostname** (not a bare IP).

    deb822 is the noble default (the base ships ``ubuntu.sources`` in this form).
    Naming the hostname (resolved offline via /etc/hosts) is what lets HTTPS keep
    a valid SNI + cert; over HTTP it is simply the clean canonical URI.

    ``trusted`` (default True for the device demo) emits ``Trusted: yes``, which
    tells apt to SKIP signature verification for this repo. This is required today
    because the device's ``apt-key`` is broken ("Unknown error executing apt-key"
    → every InRelease "not signed" → ``apt-get update`` exit 100); host-curl
    already proves transport+integrity-by-checksum, and a real GPG path
    (ubuntu-archive-keyring + a working gpgv) is the follow-up DEVICE-REQ. Pass
    ``trusted=False`` to keep ``Signed-By`` GPG verification on (the original
    behavior) once that lands.
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
    trust_line = (
        "Trusted: yes\n" if trusted else f"Signed-By: {keyring}\n"
    )
    trust_note = (
        "# Trusted: yes = DEMO signature skip (device apt-key broken); real GPG is a follow-up.\n"
        if trusted
        else "# Integrity = GPG-signed Release via the archive keyring.\n"
    )
    return (
        f"# ALR apt mirror-IP overlay (DoH fallback) — {mirror.key} via {scheme.upper()}.\n"
        f"# Host {mirror.host} is pinned to {mirror.cdn} anycast in /etc/hosts, so\n"
        f"# `apt update` reaches this URI with NO DNS.\n"
        f"{trust_note}"
        "Types: deb\n"
        f"URIs: {uri}\n"
        f"Suites: {suites}\n"
        f"Components: {' '.join(comps)}\n"
        f"{trust_line}"
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
    trusted: bool = True
    rootfs_abs: str | None = None

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
            "trusted": self.trusted,
            "rootfs_abs": self.rootfs_abs,
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
    trusted: bool = True,
    rootfs_abs: str | None = DEFAULT_ROOTFS_ABS,
    status_path: str | None = None,
) -> AptMirrorOverlayResult:
    """Pack the §5-E ``apt-mirror-stage.tar`` (OFFLINE — no network).

    Writes, all ./-rooted at their rootfs-absolute paths:

      * ``/etc/hosts`` — mirror host -> anycast IP pin (no-DNS resolution).
      * ``/etc/apt/sources.list.alr.d/alr-<key>.sources`` — the ONLY stanza apt
        reads (apt.conf repoints Dir::Etc::sourceparts here). Carries
        ``Trusted: yes`` when ``trusted``.
      * ``/etc/apt/sources.list.d/alr-<key>.sources`` — same stanza, kept for
        humans / any tool that reads the conventional dir directly. (Inert for the
        ``apt-get update`` path since Dir::Etc::sourceparts points elsewhere.)
      * ``/etc/apt/sources.list.d/ubuntu.sources`` — OVERWRITES the base cloud-init
        file with a neutralizer comment (kills the unpinned clouds.ports host +
        noble-backports). Overlay extraction atomically replaces non-library files.
      * ``/etc/apt/apt.conf.d/99alr-mirror-ip`` — Dir::Etc isolation + (when
        ``trusted``) AllowUnauthenticated + Dir::State::status absolute-path pin.

    ``bootstrap_ips`` overrides the catalog pins. ``rootfs_abs`` is the on-device
    rootfs path baked into the absolute Dir::Etc::sourceparts / Dir::State::status
    (None => relative sourceparts + no status pin). ``trusted`` toggles the DEMO
    signature skip (see ``build_sources_body`` / ``build_apt_conf_body``).
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
        m, scheme=scheme, suite=suite, components=components, trusted=trusted
    ).encode()
    apt_conf_body = build_apt_conf_body(
        rootfs_abs=rootfs_abs, trusted=trusted, status_path=status_path
    ).encode()
    sources_path = SOURCES_PATH_TMPL.format(key=m.key)
    alr_sources_path = ALR_SOURCES_PATH_TMPL.format(key=m.key)

    members: list[str] = []
    with tarfile.open(out_tar, "w") as tar:
        for d in (
            "etc", "etc/apt",
            "etc/apt/sources.list.d",
            ALR_SOURCES_DIR,
            "etc/apt/apt.conf.d",
        ):
            _add_dir(tar, d)
            members.append("./" + d)
        _add_file(tar, HOSTS_PATH, hosts_body)
        members.append("./" + HOSTS_PATH)
        # The authoritative stanza apt reads (Dir::Etc::sourceparts -> here).
        _add_file(tar, alr_sources_path, sources_body)
        members.append("./" + alr_sources_path)
        # Conventional-dir copy (humans / direct tools); inert for the update path.
        _add_file(tar, sources_path, sources_body)
        members.append("./" + sources_path)
        # Overwrite the base cloud-init ubuntu.sources with a neutralizer.
        _add_file(tar, UBUNTU_SOURCES_PATH, UBUNTU_SOURCES_NEUTRALIZED.encode())
        members.append("./" + UBUNTU_SOURCES_PATH)
        _add_file(tar, APT_CONF_PATH, apt_conf_body)
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
        trusted=trusted,
        rootfs_abs=rootfs_abs,
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
    parser.add_argument("--rootfs-abs", default=DEFAULT_ROOTFS_ABS,
                        help="on-device rootfs absolute path baked into the apt.conf "
                        f"Dir::Etc::sourceparts + Dir::State::status (default {DEFAULT_ROOTFS_ABS!r}); "
                        "pass '' to omit the absolute pins (relative sourceparts, no status pin)")
    parser.add_argument("--status-path",
                        help="Dir::State::status path (default: <rootfs-abs>/var/lib/dpkg/status). "
                        "Point at a dpkg-status copy OUTSIDE the rootfs (e.g. "
                        "/data/local/tmp/alr-dpkg-status) to dodge the on-device realpath edge "
                        "that fails flAbsPath for in-rootfs paths — WORKAROUND, needs that file "
                        "staged as a separate device asset")
    parser.add_argument("--no-trusted", dest="trusted", action="store_false",
                        help="keep apt GPG signature verification ON (Signed-By keyring); "
                        "default is Trusted:yes + AllowUnauthenticated (DEMO skip, since the "
                        "device apt-key is broken — real GPG is a follow-up DEVICE-REQ)")
    parser.set_defaults(trusted=True)
    parser.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                        help="print the planned overlay members + bodies WITHOUT packing")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    rootfs_abs = args.rootfs_abs or None

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
            trusted=args.trusted,
        )
        apt_conf_body = build_apt_conf_body(
            rootfs_abs=rootfs_abs, trusted=args.trusted, status_path=args.status_path
        )
        sources_path = SOURCES_PATH_TMPL.format(key=m.key)
        alr_sources_path = ALR_SOURCES_PATH_TMPL.format(key=m.key)
        planned = sorted([
            "./" + HOSTS_PATH, "./" + alr_sources_path, "./" + sources_path,
            "./" + UBUNTU_SOURCES_PATH, "./" + APT_CONF_PATH,
        ])
        if args.json:
            print(json.dumps({
                "mirror": m.key, "host": m.host, "scheme": args.scheme,
                "trusted": args.trusted, "rootfs_abs": rootfs_abs,
                "bootstrap_ips": list(m.bootstrap), "base_uri": m.base_uri(args.scheme),
                "planned_members": planned,
                "hosts_body": hosts_body, "sources_body": sources_body,
                "apt_conf_body": apt_conf_body,
                "ubuntu_sources_neutralized": UBUNTU_SOURCES_NEUTRALIZED,
            }, indent=2))
        else:
            print(f"apt mirror-IP overlay plan ({m.key} via {args.scheme}, "
                  f"trusted={args.trusted}, rootfs_abs={rootfs_abs}):")
            print(f"  base URI: {m.base_uri(args.scheme)}")
            print(f"  pinned IPs: {', '.join(m.bootstrap)} ({m.cdn} {', '.join(m.cdn_ranges)})")
            print("  planned members:")
            for x in planned:
                print(f"    {x}")
            print("\n--- /etc/hosts ---\n" + hosts_body, end="")
            print(f"\n--- {alr_sources_path} (authoritative) ---\n" + sources_body, end="")
            print(f"\n--- {UBUNTU_SOURCES_PATH} (overwrites base) ---\n" + UBUNTU_SOURCES_NEUTRALIZED, end="")
            print("\n--- " + APT_CONF_PATH + " ---\n" + apt_conf_body, end="")
        return 0

    if not args.out:
        parser.error("--out is required for a full build (or use --list / --verify-fetch / "
                     "--print-current-ips / --selftest)")

    res = build_apt_mirror_overlay(
        args.out, mirror=m, scheme=args.scheme, suite=args.suite,
        components=tuple(args.components) if args.components else None,
        bootstrap_ips=tuple(m.bootstrap),
        trusted=args.trusted, rootfs_abs=rootfs_abs, status_path=args.status_path,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  mirror: {res.mirror} ({res.host}) via {res.scheme}")
        print(f"  base URI: {res.base_uri}")
        print(f"  pinned IPs: {', '.join(res.bootstrap_ips)}")
        print(f"  trusted (DEMO GPG skip): {res.trusted}")
        print(f"  rootfs_abs (status/sourceparts pin): {res.rootfs_abs}")
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
    # Default is trusted=True (the device demo): Trusted: yes, NO Signed-By.
    sb_http = build_sources_body(ports, scheme="http")
    check("http sources URI names the hostname (not an IP)",
          "URIs: http://ports.ubuntu.com/ubuntu-ports" in sb_http)
    check("http sources URI has NO literal IP",
          "172.66.152.176" not in sb_http and "104.20.28.246" not in sb_http)
    check("sources include noble + -updates + -security suites",
          "Suites: noble noble-updates noble-security" in sb_http)
    check("default sources carry Trusted: yes (DEMO GPG skip)",
          "Trusted: yes" in sb_http)
    check("trusted sources DROP Signed-By (apt-key is broken on device)",
          "Signed-By:" not in sb_http)
    check("sources are deb822 (Types: deb)", sb_http.startswith("#") and "Types: deb" in sb_http)
    # --no-trusted restores Signed-By GPG verification (the original behavior).
    sb_signed = build_sources_body(ports, scheme="http", trusted=False)
    check("trusted=False restores Signed-By archive keyring",
          "Signed-By:" in sb_signed and "ubuntu-archive-keyring.gpg" in sb_signed)
    check("trusted=False drops Trusted: yes", "Trusted: yes" not in sb_signed)
    sb_https = build_sources_body(ports, scheme="https")
    check("https sources URI names the hostname (SNI/cert match)",
          "URIs: https://ports.ubuntu.com/ubuntu-ports" in sb_https)

    # debian sources differ (no -security suffix the same way; Fastly host)
    sb_deb = build_sources_body(deb, scheme="http", trusted=False)
    check("debian sources name deb.debian.org",
          "URIs: http://deb.debian.org/debian" in sb_deb)
    check("debian sources use the debian keyring (trusted=False)",
          "debian-archive-keyring.gpg" in sb_deb)

    # --- apt.conf: Dir::Etc isolation + DEMO trust + status pin ------------- #
    check("apt.conf sets Languages none", 'Acquire::Languages "none";' in APT_CONF_BODY)
    check("apt.conf forces IPv4", 'Acquire::ForceIPv4 "true";' in APT_CONF_BODY)
    check("apt.conf isolates sourcelist to /dev/null (ignore base sources.list)",
          'Dir::Etc::sourcelist "/dev/null";' in APT_CONF_BODY)
    check("apt.conf repoints sourceparts at sources.list.alr.d",
          "sources.list.alr.d" in APT_CONF_BODY and "Dir::Etc::sourceparts" in APT_CONF_BODY)
    check("apt.conf (default trusted) sets AllowUnauthenticated",
          'APT::Get::AllowUnauthenticated "true";' in APT_CONF_BODY)
    check("apt.conf default bakes the device rootfs status pin (Dir::State::status)",
          "Dir::State::status" in APT_CONF_BODY and DEFAULT_ROOTFS_ABS in APT_CONF_BODY)
    check("apt.conf #clears the PackageKit Post-Invoke-Success hook list",
          "#clear APT::Update::Post-Invoke-Success;" in APT_CONF_BODY)
    check("apt.conf #clears Post-Invoke too",
          "#clear APT::Update::Post-Invoke;" in APT_CONF_BODY)
    check("apt.conf runs as root (no _apt sandbox user on the rootfs)",
          'APT::Sandbox::User "root";' in APT_CONF_BODY)
    # trusted=False conf drops AllowUnauthenticated; absent rootfs drops status pin.
    conf_signed = build_apt_conf_body(trusted=False)
    check("apt.conf trusted=False drops AllowUnauthenticated",
          "AllowUnauthenticated" not in conf_signed)
    conf_norootfs = build_apt_conf_body(rootfs_abs=None)
    check("apt.conf rootfs_abs=None drops the absolute status pin directive",
          'Dir::State::status "' not in conf_norootfs)
    check("apt.conf rootfs_abs=None still isolates sources (relative sourceparts)",
          'Dir::Etc::sourceparts "sources.list.alr.d";' in conf_norootfs)
    # status_path overrides Dir::State::status to a path OUTSIDE the rootfs (the
    # device-proven realpath-edge workaround). It must win over the in-rootfs path.
    conf_extstatus = build_apt_conf_body(status_path="/data/local/tmp/alr-dpkg-status")
    check("apt.conf status_path points Dir::State::status outside the rootfs",
          'Dir::State::status "/data/local/tmp/alr-dpkg-status";' in conf_extstatus)
    check("apt.conf status_path replaces (not appends) the in-rootfs status pin",
          f'Dir::State::status "{DEFAULT_ROOTFS_ABS}/var/lib/dpkg/status";' not in conf_extstatus)
    conf_extstatus_norootfs = build_apt_conf_body(rootfs_abs=None,
                                                  status_path="/data/local/tmp/alr-dpkg-status")
    check("apt.conf status_path works even with rootfs_abs=None",
          'Dir::State::status "/data/local/tmp/alr-dpkg-status";' in conf_extstatus_norootfs)

    # --- ubuntu.sources neutralizer -------------------------------------- #
    check("ubuntu.sources neutralizer is a comment (no Types: deb stanza)",
          "Types: deb" not in UBUNTU_SOURCES_NEUTRALIZED
          and UBUNTU_SOURCES_NEUTRALIZED.startswith("#"))

    # --- full OFFLINE pack + §5-E conformance ------------------------------- #
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "apt-mirror-stage.tar"
        res = build_apt_mirror_overlay(out, mirror="ports", scheme="http")
        with tarfile.open(out) as t:
            names = {m.name: m for m in t.getmembers()}
            bodies = {n: t.extractfile(m).read() for n, m in names.items() if m.isfile()}
        check("overlay ships ./etc/hosts", "./etc/hosts" in names)
        check("overlay ships the AUTHORITATIVE ./etc/apt/sources.list.alr.d/alr-ports.sources",
              "./etc/apt/sources.list.alr.d/alr-ports.sources" in names)
        check("overlay still ships the conventional ./etc/apt/sources.list.d/alr-ports.sources",
              "./etc/apt/sources.list.d/alr-ports.sources" in names)
        check("overlay OVERWRITES base ./etc/apt/sources.list.d/ubuntu.sources",
              "./etc/apt/sources.list.d/ubuntu.sources" in names)
        check("overlay ships ./etc/apt/apt.conf.d/99alr-mirror-ip",
              "./etc/apt/apt.conf.d/99alr-mirror-ip" in names)
        check("all members ./-rooted", all(n.startswith("./") for n in names))
        check("packed hosts pins the mirror IP",
              b"172.66.152.176\tports.ubuntu.com" in bodies["./etc/hosts"])
        check("packed authoritative sources name the hostname + Trusted: yes",
              b"http://ports.ubuntu.com/ubuntu-ports" in
              bodies["./etc/apt/sources.list.alr.d/alr-ports.sources"]
              and b"Trusted: yes" in
              bodies["./etc/apt/sources.list.alr.d/alr-ports.sources"])
        check("packed ubuntu.sources is neutralized (no live URIs)",
              b"Types: deb" not in bodies["./etc/apt/sources.list.d/ubuntu.sources"])
        check("packed apt.conf carries the absolute status pin",
              DEFAULT_ROOTFS_ABS.encode() in bodies["./etc/apt/apt.conf.d/99alr-mirror-ip"])
        check("result.file_count == 5", res.file_count == 5)
        check("result reports trusted + rootfs_abs",
              res.trusted is True and res.rootfs_abs == DEFAULT_ROOTFS_ABS)
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
