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
guaranteed by apt's signature chain (see "Authenticated by default" below). We
therefore default the sources to
``http://`` (``--scheme http``). HTTPS is offered (``--scheme https``) for
environments that require transport encryption; it works via the ``/etc/hosts``
pin because the URL still carries the hostname (SNI/cert match — host-proven), but
it is strictly heavier than HTTP here.

  (host evidence, 2026-06-03, this builder's research:
     curl --resolve ports.ubuntu.com:80:172.66.152.176  http://…/InRelease     -> 200
     curl --resolve ports.ubuntu.com:443:172.66.152.176 https://…/InRelease    -> 200, ssl_verify=0
     curl                              https://172.66.152.176/…/InRelease       -> TLS handshake FAIL
   i.e. hostname-via-/etc/hosts works over both; raw-IP HTTPS fails the cert.)

Authenticated by default — gpgv via apt-key + a Signed-By keyring
----------------------------------------------------------------
Integrity is only real if apt **verifies the signature** on that ``InRelease``.
The overlay therefore ships an honest, authenticated path by default:

  * It stages the **Ubuntu archive signing key** into the rootfs at
    ``/usr/share/keyrings/ubuntu-archive-keyring.gpg`` (a dearmored binary
    OpenPGP keyring) and sets ``Signed-By: <that path>`` on the deb822 stanza.
    ``AllowUnauthenticated`` is OFF in this mode.
  * HOW noble apt verifies it (the device-diagnosed crux of GAP 1).
    ``ports``'s base is **noble**, whose apt is **2.7.14**. In that apt the
    ``gpgv`` *method* (``apt-pkg/contrib/gpgv.cc`` ``ExecGPGV``) does **NOT** run
    ``gpgv`` directly — even with a per-source ``Signed-By`` it ALWAYS exec()s
    ``Dir::Bin::apt-key`` (default ``/usr/bin/apt-key``), passing ``--keyring
    <Signed-By>`` (host-verified: the device method binary's strings carry
    "Unknown error executing apt-key"; the 2.7.14 source does
    ``Args.push_back(aptkey)`` unconditionally). The ``apt-key verify`` *script*
    then (a) resolves its verifier from ``Apt::Key::gpgvcommand`` or a bare-name
    ``gpgv`` PATH lookup and runs it against the keyring, and (b) shells out to a
    handful of **coreutils** (``mktemp``/``chmod``/``touch``/``rm``/``cat``/
    ``head`` …) for its temp gpg home. So "authenticated" needs the WHOLE apt-key
    happy path, not just gpgv — and the slim base ships **none** of it
    (host-verified: no ``/usr/bin/gpgv``, and of coreutils only ``/usr/bin/env``).
    That is exactly why the device died "Unknown error executing apt-key" → "E:
    The repository is not signed": apt-key was reached but could not run.
    The FIX is split across the two overlays:
      - THIS overlay's apt.conf pins ``Dir::Bin::apt-key`` → ``/usr/bin/apt-key``
        and ``Apt::Key::gpgvcommand`` → ``/usr/bin/gpgv`` (absolute, so the guest's
        PATH is irrelevant) — see :func:`build_apt_conf_body` concern (5);
      - the apt+dpkg overlay (``tools/build_apt_dpkg_overlay.py --self-contained``)
        STAGES ``apt-key`` + ``methods/gpgv`` (both ride in the ``apt`` package),
        ``gpgv``, and the coreutils apt-key shells out to.
    NB: a plain ``.gpg`` Signed-By keyring needs **no** ``gpg``/``gpgconf`` (the
    apt-key dearmor of a ``.gpg`` is a no-op, the merge is ``cat``, and cleanup's
    ``gpgconf`` is ``command_available``-guarded — script-traced), so we do not pull
    the heavy gnupg stack. RESIDUAL (if it still fails after both halves land):
    ``apt → apt-key → gpgv`` is a 2-deep fork/exec chain, so any remaining failure
    is a native exec-re-entry-depth issue for the loader track, NOT this tooling.
  * The key bytes are the **exact** ones the base rootfs already ships in
    ``etc/apt/trusted.gpg.d/`` — the **Ubuntu Archive Automatic Signing Key
    (2018)**, fingerprint ``F6ECB3762474EDA9D21B7022871920D1991BC93C`` (plus the
    2012 CD Image key, harmless to include). That 2018 key is the one that signs
    the noble ``InRelease`` on ports.ubuntu.com (host-proven: the InRelease
    signature's Issuer Fingerprint == ``F6EC…C93C``). ``ports.ubuntu.com`` uses
    the **same** Ubuntu archive key as ``archive.ubuntu.com`` — there is no
    separate "ports" key — so this keyring is correct for the arm64 archive.
    Bytes are embedded here (base64) for a deterministic offline build, with
    their sha256 asserted in the selftest; ``--keyring-from-rootfs <tar|dir>``
    re-extracts them from a live base rootfs instead, and ``--demo-trust`` falls
    back to the old unauthenticated ``Trusted: yes`` shim.
  * DEVICE PREREQ: stage ``tools/build_apt_dpkg_overlay.py --self-contained`` too.
    The base rootfs ships the apt ``gpgv`` *method* but **not** ``/usr/bin/gpgv``,
    nor ``apt-key``'s coreutils, so without that overlay authenticated
    ``apt-get update`` fails closed ("Unknown error executing apt-key"). This is
    the honest trade: authenticated mode refuses everything if the verifier is
    missing, which is exactly what a security boundary should do.

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
what ``/etc/hosts`` does for apt) and that the noble ``InRelease`` is clearsigned
by the staged key's fingerprint. The real in-app ``apt update`` (and the
gpgv-verifies-the-signature confirmation) is the integration/device gate (the
integration session stages the tar). The socket-passthrough claim is read from
the seccomp source, not a device run.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import socket
import ssl
import tarfile
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------- #
# Ubuntu archive signing keyring (authenticated mode)
# --------------------------------------------------------------------------- #
# apt verifies an InRelease against a keyring named by ``Signed-By:`` using gpgv
# (NOT apt-key). We stage the Ubuntu archive key into the rootfs and point the
# stanza at it. The bytes below are the EXACT keyring the base rootfs already
# ships at etc/apt/trusted.gpg.d/ubuntu-keyring-2018-archive.gpg (+ the 2012
# cdimage key), embedded here so the build is offline+deterministic. They are
# REUSE, not fabricated: --keyring-from-rootfs re-extracts them from a live base
# rootfs and they must byte-match. The 2018 key (fpr F6EC…C93C) is what signs the
# noble InRelease on ports.ubuntu.com (host-proven). ports uses the SAME archive
# key as archive.ubuntu.com — there is no separate ports key.
#
# Where the keyring lands in the rootfs. /usr/share/keyrings/ is the canonical
# location the ``ubuntu-keyring`` package installs and the path
# ``Signed-By:`` already referenced — but the base rootfs does NOT actually ship
# it there (it ships the same key under etc/apt/trusted.gpg.d/ instead), so the
# overlay MUST stage the file or Signed-By dangles. We stage it here.
ARCHIVE_KEYRING_PATH = "usr/share/keyrings/ubuntu-archive-keyring.gpg"

# Ubuntu Archive Automatic Signing Key (2018) — signs the noble Release/InRelease.
UBUNTU_ARCHIVE_KEY_2018_FPR = "F6ECB3762474EDA9D21B7022871920D1991BC93C"
_KEY_2018_SHA256 = "5ebbeeb474034b1fa7e50abbe6f136e177fc826219e01f314b3623f7b3097e96"
_KEY_2018_B64 = (
    "mQINBFufwdoBEADv/Gxytx/LcSXYuM0MwKojbBye81s0G1nEx+lz6VAUpIUZnbkqdXBHC+dwrGS/"
    "CeeLuAjPRLU8AoxE/jjvZVp8xFGEWHYdklqXGZ/gJfP5d3fIUBtZHZEJl8B8m9pMHf/AQQdsC+Yz"
    "izSG5t5Mhnotw044LXtdEEkx2t6Jz0OGrh+5IoxqX7pZiq6Cv19BohaUioKMdp7ES6RYfN7ol6HS"
    "LFlrMXtVfh/ijpN9j3ZhVGVeRC8kKHQsJ5PkIbmvxBiUh7SJmfZUx0IQhNMaDHXfdZAGNtnhzzNR"
    "eb1FqNLSVkrS/PnsAQzMhG1BDm2VOSF64jebKXffFqM5LXRQTeqTLsjUbbrqR6s/GCO8UF7jfUj6"
    "I7taLygmsHO/JD4jpKRC0gbpUBfaiJyLvuepx3kWoqL3sN0LhlMI80+fA7GTvoOx4tpqVlzlE6Ta"
    "jYu+jfW3QpOFS5ewEMdL26hzxsZg/geZvTbArcP+OsJKRmhv4kNo6AydyHQ/3ZV/f3X9mT3/SPLb"
    "Jaumkgp3Yzd6t5PeBu+ZQk/mN5WNNuaihNEV7llb1ZhvY0Fxu9BVd/BNl0rzuxp3rIinB2TX2SCg"
    "7wE5xXkwXuQ/2eTDE0v0HlGntkuZjGowDZkxHZQSxZVOzdZCRVaX/WEFLpKa2AQpw5RJrQ4oZ/Of"
    "ifXyJzP27o03wQARAQABtEJVYnVudHUgQXJjaGl2ZSBBdXRvbWF0aWMgU2lnbmluZyBLZXkgKDIw"
    "MTgpIDxmdHBtYXN0ZXJAdWJ1bnR1LmNvbT6JAjgEEwEKACIFAlufwdoCGwMGCwkIBwMCBhUIAgkK"
    "CwQWAgMBAh4BAheAAAoJEIcZINGZG8k8LHMQAKS2cnxz/5WaoCOWArf5g6UHbeOCgc5DBm0hCuFD"
    "ZWWv427aGei3CPuLw0DGLCXZdyc5dqE8mvjMlOmmAKKlj1uGg3TYCbQWjWPeMnBPZbkFgkZoXJ7/"
    "6CB7bWRht1sHzpt1LTZ+SYDwOwJ68QRp7DRaZl9Y6QiUbeuhq2DUcTofVbBxbhrckN4ZteLvm+/n"
    "G9m/ciopc66LwRdkxqfJ32Cyq+1TS5VaIJDG7DWziG+Kbu6qCDM4QNlg3LH7p14CrRxAbc4lvohR"
    "gsV4eQqsIcdFkuVY5HPPj2K8TqpY6STe8Gh0aprG1RV8ZKay3KSMpnyV1fAKn4fM9byiLzQAovC0"
    "LZ9MMMsrAS/45AvC3IEKSShjLFn1X1dRCiO6/7jmZEoZtAp53hkf8SMBsi78hVNrBumZwfIdBA1v"
    "22+LY4xQK8q4XCoRcA9G+pvzU9YVW7cRnDZZGl0uwOw7z9PkQBF5KFKjWDz4fCk+K6+YtGpovGKe"
    "kGBb8I7EA6UpvPgqA/QdI0t1IBP0N06RQcs1fUaAQEtz6DGy5zkRhR4pGSZn+dFET7PdAjEK84y7"
    "BdY4t+U1jcSIvBj0F2B7LwRL7xGpSpIKi/ekAXLs117bvFHaCvmUYN7JVp1GMmVFxhIdx6CFm3fx"
    "G8QjNb5tere/YqK+uOgcXny1UlwtCUzlrSaP"
)
# Ubuntu CD Image Automatic Signing Key (2012) — signs CD images, not the apt
# archive; bundled for parity with the stock ubuntu-archive-keyring (harmless).
UBUNTU_CDIMAGE_KEY_2012_FPR = "843938DF228D22F7B3742BC0D94AA3F0EFE21092"
_KEY_2012_SHA256 = "192b3782ba2e00e05b6521371fbe67847efad3fdd1cfb87621882d833c8703fa"
_KEY_2012_B64 = (
    "mQINBE+tjmgBEAC7pKK78t89DW7mvMoSgiScLfPNF8/TSF380is0hFRL3dOmcXEfNsX26jtv8bdv"
    "vtkElB1fPwOntmqSAsrLOuURVQ6GSxH7IDU5QFfaTIsudtLR5YTlC3ZuOTOb1HWEK26fDRXuIWjh"
    "FDXJH3KLv+rSrq0+x7ZtH++CHq5XJWk7VUh/wWcGxZefs7+1HTivymhjXCOwQvqblzZ5MAec9i4Q"
    "IXxkqX1HY7ryxGVdjj9lApOnoU5EcSYr08cm7xQEgrdDLAZFQxDYBLDuV6E6jKEfAfwZINSEe4Oc"
    "m82vtCF5K0HiwhFU09ky2yogbMuTTi2f8ibN8SbbhZDJlDPd2ZkkpsKNfIALmOiPhHGvXGmtg6Fd"
    "zRUOSGirSm8tcakpS+d0/IElbD453sksxg6s3cTs7Q+PudaccyQ0BqatMnzmfxCVOotT65kVnmz2"
    "P+4Q0gRSQ/Zi9Inz+OrzWxtn6/Tdw+FMUwvBccxW1r88k6uVLz23jW/8jOuwnUp4JKmZta/U2UZK"
    "TyPyrvTYhp/zK332BEnxiRY4ZfQjA4Iwlw00l4pYBDLLc6TFJtLbDv859UCisXa8MtWYWrlM3YfG"
    "Fs9k1WemML8u79g2DK8g3VPkD94Q5anqufEGm74K/keOmss8cQoBX9VPFMpS1mFCT+2UdGP0UvMl"
    "ADct0aFnAwtb9QARAQABtEFVYnVudHUgQ0QgSW1hZ2UgQXV0b21hdGljIFNpZ25pbmcgS2V5ICgy"
    "MDEyKSA8Y2RpbWFnZUB1YnVudHUuY29tPokCNwQTAQoAIQUCT62OaAIbAwULCQgHAwUVCgkICwUW"
    "AgMBAAIeAQIXgAAKCRDZSqPw7+IQkkhAEACJjZZXuAabMrC49Z52HywVZipJgoV5ufMi2LQYMkyG"
    "KVQQ/E74lUjccMmbQ4j00ihTYB+F/i29AxfavJnlSpWgmwjPO4YY5jvooUiXQmVHX10oM1w3+Y9w"
    "ScmeUY3IhTtwiFaBJr6TZ7RvOTg/pbQ0GvzxNlkSobuqFCZ023mcl2Y7OkY1PZgxiLafD6Rx2O/g"
    "clQPs4YfHo8bKRA4o10702nE8YE+dixIgAQw67Txhq5idNxsWpudKq9J1fLgnEz7i9AJUOf12sg9"
    "X7ZvpXZ3QvMV5iOvLA4DRLv9HIxyz70XqeakS+uzfKXuCMzhdUTIb/tNACNB37+reIqdPsyUF3tx"
    "VyWaL1jMkRsv617yKAiYvPNwMDRvrbKiJ4Icnd4tPzmqz5HBFUyULns3JzJNjpgKCvLGhVq+lVsd"
    "pMlpQxEG5/bhzJgB1jrIbkcOSfnQ1y0Gv9CItel+1q0BHMn0dPVWaNfKYFGsz4igW+uj//C09/gt"
    "GMm78PQfjqEoR2j/Tam/tmucxSK331yfm5ag2CQYGC3bswfII+4EanX9dN/RG3/2dsSyYruWpTIQ"
    "G6Xa7+AZtYBDEXNYovgdJtXWyUtW0X7R6vIjh1HYer3dR6ivJ+q/bWGY45zHeNBNU33hlnlxEENi"
    "f3RZ/j/w3SjGrtSQK69maNR6onq492e+6w=="
)

# Where the base rootfs actually ships the two keys (used by --keyring-from-rootfs).
_ROOTFS_KEY_MEMBERS = (
    "etc/apt/trusted.gpg.d/ubuntu-keyring-2018-archive.gpg",
    "etc/apt/trusted.gpg.d/ubuntu-keyring-2012-cdimage.gpg",
)


def archive_keyring_bytes() -> bytes:
    """The dearmored Ubuntu archive keyring (2018 archive key + 2012 cdimage key),
    concatenated — exactly the bytes the base rootfs ships. A binary OpenPGP
    keyring is just the concatenation of its key packets, so gpgv reads both."""
    blob = base64.b64decode(_KEY_2018_B64) + base64.b64decode(_KEY_2012_B64)
    return blob


def keyring_from_rootfs(base: str | Path) -> bytes:
    """Re-extract the archive keyring from a live base rootfs (tar OR dir) and
    return the same concatenated-keyring bytes. Lets the integration session
    rebuild the overlay from the canonical source instead of the embedded copy;
    selftest asserts this equals :func:`archive_keyring_bytes`."""
    base = Path(base)
    parts: list[bytes] = []
    if base.is_dir():
        for rel in _ROOTFS_KEY_MEMBERS:
            p = base / rel
            if not p.is_file():
                raise FileNotFoundError(f"{base}: missing rootfs key {rel}")
            parts.append(p.read_bytes())
    else:
        with tarfile.open(base, "r:*") as t:
            members = {m.name.lstrip("./").lstrip("/"): m for m in t.getmembers()}
            for rel in _ROOTFS_KEY_MEMBERS:
                m = members.get(rel)
                if m is None:
                    raise FileNotFoundError(f"{base}: missing rootfs key {rel}")
                fh = t.extractfile(m)
                if fh is None:
                    raise FileNotFoundError(f"{base}: {rel} is not a regular file")
                parts.append(fh.read())
    return b"".join(parts)


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

# Absolute rootfs paths of the two binaries apt's signature verification execs.
# noble apt 2.7.14's `methods/gpgv` ALWAYS shells out to `Dir::Bin::apt-key`, and
# that apt-key script resolves its verifier from `Apt::Key::gpgvcommand` (else a
# bare-name PATH lookup). In-guest PATH is not guaranteed, so we PIN both to their
# absolute rootfs paths. The binaries are staged by build_apt_dpkg_overlay
# --self-contained (apt-key + methods/gpgv ride in `apt`; gpgv in `gpgv`); without
# that overlay these point at absent files and authenticated update fails closed —
# the honest behaviour for a missing verifier.
GPGV_BIN_PATH = "/usr/bin/gpgv"
APT_KEY_BIN_PATH = "/usr/bin/apt-key"

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
    trusted: bool = False,
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
    2. **Signature trust (AUTHENTICATED default).** By default this is
       authenticated: the sources stanza carries ``Signed-By: <staged keyring>``
       and apt verifies the Release with gpgv — so NO ``AllowUnauthenticated``
       knob is emitted (apt fails closed on a bad/missing signature, as it
       should). The DEMO fallback (``--demo-trust`` → ``trusted=True``) instead
       skips verification: it emits ``APT::Get::AllowUnauthenticated`` as the
       belt-and-suspenders global to the stanza's ``Trusted: yes``. That demo
       path matches the device's broken ``apt-key`` ("Unknown error executing
       apt-key") but is UNAUTHENTICATED — only use it where gpgv is unavailable.
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

    5. **Verifier path pins (AUTHENTICATED mode — gpgv + apt-key).** noble apt is
       2.7.14, whose ``methods/gpgv`` (``apt-pkg/contrib/gpgv.cc`` ``ExecGPGV``)
       ALWAYS exec()s ``Dir::Bin::apt-key`` to check an ``InRelease`` — it never
       calls ``gpgv`` directly, even with a per-source ``Signed-By`` (host-verified
       from the device binary's strings + the 2.7.14 source: ``ExecGPGV`` does
       ``Args.push_back(aptkey)`` unconditionally and passes ``--keyring
       <Signed-By>``). The ``apt-key verify`` script then resolves its verifier from
       ``Apt::Key::gpgvcommand`` (falling back to a **bare-name PATH** lookup of
       ``gpgv``). In the guest, neither ``apt-key`` nor ``gpgv`` is guaranteed on
       ``PATH``, so we PIN both absolutely: ``Dir::Bin::apt-key`` →
       ``/usr/bin/apt-key`` and ``Apt::Key::gpgvcommand`` → ``/usr/bin/gpgv``. With
       these (and the apt+dpkg ``--self-contained`` overlay that actually stages
       ``apt-key``/``methods/gpgv``/``gpgv`` + the coreutils apt-key shells out to),
       authenticated ``apt-get update`` runs the verifier against the staged
       ``Signed-By`` keyring with NO ``apt-key`` "Unknown error" and NO ``gpg``
       dependency. Emitted ONLY in authenticated mode (``--demo-trust`` skips
       verification, so the verifier path is irrelevant there). This is the
       eliminate-the-apt-key-failure half of GAP 1; the staging half is the
       overlay's. If a RESIDUAL failure persists after both pins + staging, it is a
       native exec-re-entry issue (apt→apt-key→gpgv is a 2-deep fork/exec chain) —
       documented for the loader track, NOT patched here.

    ``Languages "none"`` + ``ForceIPv4`` are retained (trim round-trips; avoid a
    dead IPv6 path on the pinned IPv4 anycast).
    """
    lines = [
        "// ALR apt mirror-IP overlay (DoH fallback): robust single-mirror apt without DNS.",
        "// (1) Dir::Etc isolates apt to the ports stanza in sources.list.alr.d (ignores the",
        "//     base cloud-init ubuntu.sources + github-cli/tailscale). (2) Signature: AUTHENTICATED",
        "//     by default (stanza Signed-By + gpgv verify); the unauthenticated skip is opt-in",
        "//     via --demo-trust. (3) Dir::State::status pins the dpkg DB to its absolute rootfs path.",
        "//     (4) Blank the PackageKit/c-n-f Post-Invoke hooks + run as root (no _apt user):",
        "//     those hooks exec gdbus/dbus that this headless rootfs lacks, which otherwise",
        "//     fails apt-get update AFTER a clean fetch. (5) AUTHENTICATED only: pin",
        "//     Dir::Bin::apt-key + Apt::Key::gpgvcommand to absolute rootfs paths (noble apt",
        "//     2.7.14 always shells out to apt-key, which then PATH-looks-up gpgv).",
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
    else:
        # (5) AUTHENTICATED verifier pins. noble apt's gpgv method always execs
        # apt-key, which PATH-resolves gpgv; pin both to absolute rootfs paths so a
        # bare-PATH miss in the guest can't surface as "Unknown error executing
        # apt-key" / "The repository is not signed". (Staging of these binaries is
        # the apt+dpkg --self-contained overlay's job; these are just the paths.)
        lines.append(f'Dir::Bin::apt-key "{APT_KEY_BIN_PATH}";')
        lines.append(f'Apt::Key::gpgvcommand "{GPGV_BIN_PATH}";')
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
    trusted: bool = False,
) -> str:
    """A deb822 ``.sources`` stanza naming the **hostname** (not a bare IP).

    deb822 is the noble default (the base ships ``ubuntu.sources`` in this form).
    Naming the hostname (resolved offline via /etc/hosts) is what lets HTTPS keep
    a valid SNI + cert; over HTTP it is simply the clean canonical URI.

    AUTHENTICATED by default (``trusted=False``): emits ``Signed-By: <keyring>``
    so apt verifies the Release signature with gpgv against the archive keyring
    this overlay stages at ``/<ARCHIVE_KEYRING_PATH>`` (the modern keyring path —
    NOT apt-key). For Ubuntu that keyring is the 2018 archive key (fpr
    ``F6EC…C93C``) which signs the noble InRelease (host-proven).

    ``trusted=True`` is the DEMO fallback (``--demo-trust``): emits
    ``Trusted: yes`` to SKIP signature verification, for the case where gpgv is
    unavailable on the device (the base ships the apt gpgv *method* but not the
    ``/usr/bin/gpgv`` binary unless the apt+dpkg overlay is also staged). This
    matches the device's broken ``apt-key`` workaround but is UNAUTHENTICATED —
    any MITM on the HTTP mirror could inject packages. Prefer the default.
    """
    suite = suite or mirror.suite
    comps = components or mirror.components
    uri = mirror.base_uri(scheme)
    # Debian ships security as a SEPARATE suite (`<suite>-security` on
    # security.debian.org), so a deb.debian.org stanza carries only
    # `<suite>`+`<suite>-updates`; Ubuntu serves `-security` from the same host.
    if mirror.host.endswith("debian.org"):
        suites = f"{suite} {suite}-updates"
        # Debian's stock keyring path (the base/closure must provide it; for the
        # Ubuntu default path we STAGE the keyring, see ARCHIVE_KEYRING_PATH).
        keyring = "/usr/share/keyrings/debian-archive-keyring.gpg"
    else:
        suites = f"{suite} {suite}-updates {suite}-security"
        # The keyring THIS overlay stages (Ubuntu archive key). Leading "/" =
        # rootfs-absolute, matching where _add_file writes ARCHIVE_KEYRING_PATH.
        keyring = "/" + ARCHIVE_KEYRING_PATH
    trust_line = (
        "Trusted: yes\n" if trusted else f"Signed-By: {keyring}\n"
    )
    trust_note = (
        "# Trusted: yes = DEMO signature SKIP (--demo-trust; UNAUTHENTICATED, MITM-able).\n"
        if trusted
        else "# Integrity = gpgv-verified GPG-signed Release via the staged archive keyring.\n"
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
    trusted: bool = False
    rootfs_abs: str | None = None
    keyring_path: str | None = None       # staged keyring (authenticated mode), else None
    keyring_fpr: str | None = None        # archive signing-key fingerprint

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
            "authenticated": not self.trusted,
            "trusted_demo_skip": self.trusted,
            "rootfs_abs": self.rootfs_abs,
            "keyring_path": self.keyring_path,
            "keyring_fpr": self.keyring_fpr,
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
    trusted: bool = False,
    rootfs_abs: str | None = DEFAULT_ROOTFS_ABS,
    status_path: str | None = None,
    keyring_bytes: bytes | None = None,
) -> AptMirrorOverlayResult:
    """Pack the §5-E ``apt-mirror-stage.tar`` (OFFLINE — no network).

    Writes, all ./-rooted at their rootfs-absolute paths:

      * ``/etc/hosts`` — mirror host -> anycast IP pin (no-DNS resolution).
      * ``/etc/apt/sources.list.alr.d/alr-<key>.sources`` — the ONLY stanza apt
        reads (apt.conf repoints Dir::Etc::sourceparts here). Carries
        ``Signed-By`` (authenticated, default) or ``Trusted: yes`` (--demo-trust).
      * ``/etc/apt/sources.list.d/alr-<key>.sources`` — same stanza, kept for
        humans / any tool that reads the conventional dir directly. (Inert for the
        ``apt-get update`` path since Dir::Etc::sourceparts points elsewhere.)
      * ``/etc/apt/sources.list.d/ubuntu.sources`` — OVERWRITES the base cloud-init
        file with a neutralizer comment (kills the unpinned clouds.ports host +
        noble-backports). Overlay extraction atomically replaces non-library files.
      * ``/usr/share/keyrings/ubuntu-archive-keyring.gpg`` — the Ubuntu archive
        signing key (authenticated mode only, Ubuntu mirrors). ``Signed-By`` points
        here so gpgv can verify the Release. NOT staged in --demo-trust mode or for
        the Debian mirror (which relies on its own base/closure keyring path).
      * ``/etc/apt/apt.conf.d/99alr-mirror-ip`` — Dir::Etc isolation + (only in
        ``--demo-trust``) AllowUnauthenticated + Dir::State::status absolute-path pin.

    ``bootstrap_ips`` overrides the catalog pins. ``rootfs_abs`` is the on-device
    rootfs path baked into the absolute Dir::Etc::sourceparts / Dir::State::status
    (None => relative sourceparts + no status pin). ``trusted`` toggles the DEMO
    signature skip (default False = authenticated; see ``build_sources_body`` /
    ``build_apt_conf_body``). ``keyring_bytes`` overrides the embedded archive
    keyring (e.g. re-extracted via :func:`keyring_from_rootfs`).
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

    # Stage the Ubuntu archive keyring in authenticated mode (Ubuntu mirrors only;
    # Debian's Signed-By names the stock debian-archive-keyring path the base owns).
    stage_keyring = (not trusted) and (not m.host.endswith("debian.org"))
    keyring_path = None
    keyring_fpr = None
    if stage_keyring:
        keyring_path = "/" + ARCHIVE_KEYRING_PATH
        keyring_fpr = UBUNTU_ARCHIVE_KEY_2018_FPR

    members: list[str] = []
    with tarfile.open(out_tar, "w") as tar:
        dirs = [
            "etc", "etc/apt",
            "etc/apt/sources.list.d",
            ALR_SOURCES_DIR,
            "etc/apt/apt.conf.d",
        ]
        if stage_keyring:
            dirs += ["usr", "usr/share", "usr/share/keyrings"]
        for d in dirs:
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
        # The archive keyring the Signed-By stanza points at (authenticated mode).
        if stage_keyring:
            kr = keyring_bytes if keyring_bytes is not None else archive_keyring_bytes()
            _add_file(tar, ARCHIVE_KEYRING_PATH, kr)
            members.append("./" + ARCHIVE_KEYRING_PATH)
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
        keyring_path=keyring_path,
        keyring_fpr=keyring_fpr,
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


def _fetch_full(url: str, host: str, ip: str, *, timeout: float = 30.0, cap: int = 600_000) -> bytes:
    """Fetch the FULL body of a small text resource (InRelease) through a pinned IP
    while presenting ``host`` for Host/SNI. Returns the decoded body bytes (no
    chunked-encoding handling needed: the archive serves InRelease with a fixed
    Content-Length over plain HTTP)."""
    from urllib.parse import urlsplit

    parts = urlsplit(url)
    scheme = parts.scheme
    port = parts.port or (443 if scheme == "https" else 80)
    path = parts.path or "/"
    if scheme == "https":
        ctx = ssl.create_default_context()
        sock = ctx.wrap_socket(socket.create_connection((ip, port), timeout=timeout),
                               server_hostname=host)
    else:
        sock = socket.create_connection((ip, port), timeout=timeout)
    try:
        sock.sendall(
            (f"GET {path} HTTP/1.1\r\nHost: {host}\r\n"
             "User-Agent: Debian APT-HTTP/1.3 (ALR apt-mirror-overlay verify)\r\n"
             "Connection: close\r\n\r\n").encode()
        )
        buf = b""
        while len(buf) < cap:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    finally:
        sock.close()
    return buf.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in buf else b""


def inrelease_issuer_fprs(body: bytes) -> set[str]:
    """Parse a clearsigned ``InRelease`` and return the set of issuer
    fingerprints + 64-bit key IDs found in its OpenPGP signature packet.

    This does NOT verify the signature (no gpg on the build host) — it confirms
    the document is clearsigned by the key we stage (the fpr matches), so the
    on-device gpgv has the right key to succeed against. Returns an empty set if
    the body is not clearsigned / has no parseable issuer."""
    import re as _re
    import struct as _struct

    m = _re.search(
        rb"-----BEGIN PGP SIGNATURE-----\r?\n(.*?)\r?\n-----END PGP SIGNATURE-----",
        body, _re.S,
    )
    if not m:
        return set()
    armored = m.group(1)
    b64_lines = [
        ln for ln in armored.split(b"\n")
        if ln and not ln.startswith(b"=") and b":" not in ln
    ]
    try:
        sig = base64.b64decode(b"".join(b64_lines))
    except Exception:
        return set()
    if not sig:
        return set()
    # First packet = signature (tag 2); read its length (old or new format).
    tag = sig[0]
    i = 1
    if tag & 0x40:  # new format
        l = sig[i]
        if l < 192:
            length = l; i += 1
        elif l < 224:
            length = ((l - 192) << 8) + sig[i + 1] + 192; i += 2
        else:
            length = _struct.unpack(">I", sig[i + 1:i + 5])[0]; i += 5
    else:  # old format
        ltype = tag & 0x03
        if ltype == 0:
            length = sig[i]; i += 1
        elif ltype == 1:
            length = _struct.unpack(">H", sig[i:i + 2])[0]; i += 2
        elif ltype == 2:
            length = _struct.unpack(">I", sig[i:i + 4])[0]; i += 4
        else:
            length = len(sig) - i
    body_pkt = sig[i:i + length]
    out: set[str] = set()
    if not body_pkt or body_pkt[0] != 4:  # only v4 sigs carry subpackets
        return out
    p = 4  # ver, sigtype, pkalgo, hashalgo
    for _region in range(2):  # hashed, then unhashed subpacket areas
        if p + 2 > len(body_pkt):
            break
        sublen = _struct.unpack(">H", body_pkt[p:p + 2])[0]; p += 2
        end = p + sublen
        while p < end and p < len(body_pkt):
            sl = body_pkt[p]; p += 1
            if sl == 0 or p >= len(body_pkt):
                break
            stype = body_pkt[p]
            val = body_pkt[p + 1:p + sl]
            if stype == 16 and len(val) == 8:        # Issuer key ID
                out.add(val.hex().upper())
            elif stype == 33 and len(val) >= 21:     # Issuer Fingerprint (ver byte + 20)
                out.add(val[1:].hex().upper())
            p += sl
    return out


def verify_fetch(
    mirror: Mirror, scheme: str, *, suite: str | None = None, check_key: str | None = None
) -> list[dict]:
    """Prove (on the host) that apt's index is reachable through the pinned IP via
    the hostname — for EACH bootstrap IP. Returns one result dict per IP. When
    ``check_key`` (a fingerprint) is given, also fetch the InRelease in full and
    confirm it is clearsigned by that key (so the staged keyring is the right one
    for the on-device gpgv)."""
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
            if rec["ok"] and check_key:
                body = _fetch_full(idx, mirror.host, ip)
                fprs = inrelease_issuer_fprs(body)
                want = check_key.upper()
                signed = any(f == want or want.endswith(f) for f in fprs)
                rec["clearsigned_by"] = sorted(fprs)
                rec["signed_by_expected_key"] = signed
                rec["ok"] = rec["ok"] and signed
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
                        "pinned IP via the hostname (curl --resolve equivalent; NETWORK). In "
                        "authenticated mode also confirms the InRelease is clearsigned by the "
                        "staged archive key's fingerprint")
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
    # Signature mode. DEFAULT = authenticated (Signed-By + staged keyring + gpgv).
    # --demo-trust falls back to the UNAUTHENTICATED Trusted:yes shim; --no-trusted
    # is kept as a back-compat alias for "authenticated" (it always meant GPG-on).
    parser.add_argument("--demo-trust", dest="trusted", action="store_true",
                        help="UNAUTHENTICATED demo fallback: Trusted:yes + AllowUnauthenticated "
                        "(skips signature verification). Use ONLY where gpgv is unavailable on "
                        "the device — any MITM on the HTTP mirror could inject packages. The "
                        "default is authenticated (Signed-By staged keyring + gpgv)")
    parser.add_argument("--no-trusted", dest="trusted", action="store_false",
                        help="(back-compat alias for the authenticated default) keep apt GPG "
                        "signature verification ON via the staged Signed-By keyring")
    parser.set_defaults(trusted=False)
    parser.add_argument("--keyring-from-rootfs", metavar="TAR|DIR",
                        help="re-extract the Ubuntu archive keyring from a live base rootfs "
                        "(tar or dir) instead of the embedded copy; must byte-match the "
                        "embedded 2018+2012 keys")
    parser.add_argument("--print-keyring-fpr", action="store_true",
                        help="print the staged archive keyring's signing-key fingerprint(s) "
                        "+ sha256 and exit")
    parser.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                        help="print the planned overlay members + bodies WITHOUT packing")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    rootfs_abs = args.rootfs_abs or None

    if args.selftest:
        return _selftest()

    if args.print_keyring_fpr:
        kr = archive_keyring_bytes()
        info = {
            "keyring_path": "/" + ARCHIVE_KEYRING_PATH,
            "bytes": len(kr),
            "sha256": hashlib.sha256(kr).hexdigest(),
            "signing_keys": [
                {"name": "Ubuntu Archive Automatic Signing Key (2018)",
                 "fingerprint": UBUNTU_ARCHIVE_KEY_2018_FPR,
                 "role": "signs noble Release/InRelease on ports.ubuntu.com"},
                {"name": "Ubuntu CD Image Automatic Signing Key (2012)",
                 "fingerprint": UBUNTU_CDIMAGE_KEY_2012_FPR,
                 "role": "CD images (bundled for parity; not used by apt-get update)"},
            ],
        }
        if args.json:
            print(json.dumps(info, indent=2))
        else:
            print(f"staged keyring: /{ARCHIVE_KEYRING_PATH} "
                  f"({info['bytes']} bytes, sha256 {info['sha256']})")
            for k in info["signing_keys"]:
                print(f"  {k['fingerprint']}  {k['name']}")
                print(f"      -> {k['role']}")
        return 0

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

    # Optionally re-extract the archive keyring from a live base rootfs (else the
    # embedded copy is used). Validate it byte-matches so a wrong/corrupt rootfs
    # can't silently stage a bad key that would make apt refuse everything.
    keyring_bytes = None
    if args.keyring_from_rootfs:
        keyring_bytes = keyring_from_rootfs(args.keyring_from_rootfs)
        if keyring_bytes != archive_keyring_bytes():
            parser.error(
                f"--keyring-from-rootfs {args.keyring_from_rootfs}: extracted keyring "
                "does not byte-match the known Ubuntu archive keyring (2018+2012). "
                "Refusing to stage an unverified key."
            )

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
        # In authenticated mode (and for an Ubuntu mirror) also confirm the
        # InRelease is clearsigned by the key we stage.
        check_key = None
        if not args.trusted and not m.host.endswith("debian.org"):
            check_key = UBUNTU_ARCHIVE_KEY_2018_FPR
        res = verify_fetch(m, args.scheme, suite=args.suite, check_key=check_key)
        if args.json:
            print(json.dumps({"host": m.host, "scheme": args.scheme,
                              "check_key": check_key, "results": res}, indent=2))
        else:
            print(f"verify-fetch {m.host} via {args.scheme} (hostname pinned to each IP"
                  + (f"; expect signer {check_key})" if check_key else ")") + ":")
            for r in res:
                if r.get("ok"):
                    sig = ""
                    if check_key:
                        sig = "  signed-by-2018-key=OK"
                    print(f"  [OK]   {r['ip']} -> HTTP {r['status']}, {r['bytes']}+ bytes{sig}  ({r['url']})")
                else:
                    why = r.get("error")
                    if not why and check_key and r.get("signed_by_expected_key") is False:
                        why = f"InRelease NOT signed by {check_key} (saw {r.get('clearsigned_by')})"
                    print(f"  [FAIL] {r['ip']} -> {why or ('HTTP ' + str(r.get('status')))}")
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
        stage_keyring = (not args.trusted) and (not m.host.endswith("debian.org"))
        planned = [
            "./" + HOSTS_PATH, "./" + alr_sources_path, "./" + sources_path,
            "./" + UBUNTU_SOURCES_PATH, "./" + APT_CONF_PATH,
        ]
        if stage_keyring:
            planned.append("./" + ARCHIVE_KEYRING_PATH)
        planned = sorted(planned)
        if args.json:
            print(json.dumps({
                "mirror": m.key, "host": m.host, "scheme": args.scheme,
                "authenticated": not args.trusted, "trusted_demo_skip": args.trusted,
                "rootfs_abs": rootfs_abs,
                "keyring_path": ("/" + ARCHIVE_KEYRING_PATH) if stage_keyring else None,
                "keyring_fpr": UBUNTU_ARCHIVE_KEY_2018_FPR if stage_keyring else None,
                "bootstrap_ips": list(m.bootstrap), "base_uri": m.base_uri(args.scheme),
                "planned_members": planned,
                "hosts_body": hosts_body, "sources_body": sources_body,
                "apt_conf_body": apt_conf_body,
                "ubuntu_sources_neutralized": UBUNTU_SOURCES_NEUTRALIZED,
            }, indent=2))
        else:
            print(f"apt mirror-IP overlay plan ({m.key} via {args.scheme}, "
                  f"authenticated={not args.trusted}, rootfs_abs={rootfs_abs}):")
            print(f"  base URI: {m.base_uri(args.scheme)}")
            print(f"  pinned IPs: {', '.join(m.bootstrap)} ({m.cdn} {', '.join(m.cdn_ranges)})")
            if stage_keyring:
                print(f"  staged keyring: /{ARCHIVE_KEYRING_PATH} (fpr {UBUNTU_ARCHIVE_KEY_2018_FPR})")
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
                     "--print-current-ips / --print-keyring-fpr / --selftest)")

    res = build_apt_mirror_overlay(
        args.out, mirror=m, scheme=args.scheme, suite=args.suite,
        components=tuple(args.components) if args.components else None,
        bootstrap_ips=tuple(m.bootstrap),
        trusted=args.trusted, rootfs_abs=rootfs_abs, status_path=args.status_path,
        keyring_bytes=keyring_bytes,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  mirror: {res.mirror} ({res.host}) via {res.scheme}")
        print(f"  base URI: {res.base_uri}")
        print(f"  pinned IPs: {', '.join(res.bootstrap_ips)}")
        if res.trusted:
            print("  signature: DEMO-TRUST (UNAUTHENTICATED — Trusted:yes + AllowUnauthenticated)")
        else:
            print(f"  signature: AUTHENTICATED (Signed-By gpgv keyring)")
            if res.keyring_path:
                print(f"    staged keyring: {res.keyring_path} (fpr {res.keyring_fpr})")
                print("    DEVICE PREREQ: stage the apt+dpkg overlay too (it provides /usr/bin/gpgv)")
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
    # Default is AUTHENTICATED (trusted=False): Signed-By staged keyring, NO Trusted.
    sb_http = build_sources_body(ports, scheme="http")
    check("http sources URI names the hostname (not an IP)",
          "URIs: http://ports.ubuntu.com/ubuntu-ports" in sb_http)
    check("http sources URI has NO literal IP",
          "172.66.152.176" not in sb_http and "104.20.28.246" not in sb_http)
    check("sources include noble + -updates + -security suites",
          "Suites: noble noble-updates noble-security" in sb_http)
    check("DEFAULT sources are AUTHENTICATED: Signed-By the staged archive keyring",
          f"Signed-By: /{ARCHIVE_KEYRING_PATH}" in sb_http)
    check("default sources DROP Trusted: yes (no unauthenticated skip)",
          "Trusted: yes" not in sb_http)
    check("sources are deb822 (Types: deb)", sb_http.startswith("#") and "Types: deb" in sb_http)
    # --demo-trust (trusted=True) falls back to the UNAUTHENTICATED Trusted:yes shim.
    sb_demo = build_sources_body(ports, scheme="http", trusted=True)
    check("demo-trust sources carry Trusted: yes (UNAUTHENTICATED skip)",
          "Trusted: yes" in sb_demo)
    check("demo-trust sources drop Signed-By", "Signed-By:" not in sb_demo)
    sb_https = build_sources_body(ports, scheme="https")
    check("https sources URI names the hostname (SNI/cert match)",
          "URIs: https://ports.ubuntu.com/ubuntu-ports" in sb_https)

    # debian sources differ (no -security suffix the same way; Fastly host). The
    # Debian Signed-By names the stock debian keyring (we don't stage that one).
    sb_deb = build_sources_body(deb, scheme="http")
    check("debian sources name deb.debian.org",
          "URIs: http://deb.debian.org/debian" in sb_deb)
    check("debian sources use the debian keyring path",
          "debian-archive-keyring.gpg" in sb_deb)

    # --- apt.conf: Dir::Etc isolation + AUTHENTICATED default + status pin --- #
    check("apt.conf sets Languages none", 'Acquire::Languages "none";' in APT_CONF_BODY)
    check("apt.conf forces IPv4", 'Acquire::ForceIPv4 "true";' in APT_CONF_BODY)
    check("apt.conf isolates sourcelist to /dev/null (ignore base sources.list)",
          'Dir::Etc::sourcelist "/dev/null";' in APT_CONF_BODY)
    check("apt.conf repoints sourceparts at sources.list.alr.d",
          "sources.list.alr.d" in APT_CONF_BODY and "Dir::Etc::sourceparts" in APT_CONF_BODY)
    check("apt.conf (authenticated default) does NOT set AllowUnauthenticated",
          'APT::Get::AllowUnauthenticated' not in APT_CONF_BODY)
    # (5) authenticated mode pins the apt-key + gpgv verifier paths absolutely
    # (noble apt 2.7.14 always shells to apt-key, which PATH-resolves gpgv).
    check("apt.conf (authenticated) pins Dir::Bin::apt-key to the absolute rootfs path",
          f'Dir::Bin::apt-key "{APT_KEY_BIN_PATH}";' in APT_CONF_BODY)
    check("apt.conf (authenticated) pins Apt::Key::gpgvcommand to the absolute gpgv",
          f'Apt::Key::gpgvcommand "{GPGV_BIN_PATH}";' in APT_CONF_BODY)
    check("apt.conf default bakes the device rootfs status pin (Dir::State::status)",
          "Dir::State::status" in APT_CONF_BODY and DEFAULT_ROOTFS_ABS in APT_CONF_BODY)
    check("apt.conf #clears the PackageKit Post-Invoke-Success hook list",
          "#clear APT::Update::Post-Invoke-Success;" in APT_CONF_BODY)
    check("apt.conf #clears Post-Invoke too",
          "#clear APT::Update::Post-Invoke;" in APT_CONF_BODY)
    check("apt.conf runs as root (no _apt sandbox user on the rootfs)",
          'APT::Sandbox::User "root";' in APT_CONF_BODY)
    # --demo-trust (trusted=True) ADDS AllowUnauthenticated back.
    conf_demo = build_apt_conf_body(trusted=True)
    check("apt.conf demo-trust sets AllowUnauthenticated",
          'APT::Get::AllowUnauthenticated "true";' in conf_demo)
    # demo-trust SKIPS verification entirely, so it must NOT EMIT the verifier
    # directives (the header comment still DESCRIBES concern (5), so match the real
    # `<knob> "<path>";` directive line, not the comment prose).
    check("apt.conf demo-trust does NOT pin apt-key/gpgv (verification skipped)",
          f'Dir::Bin::apt-key "{APT_KEY_BIN_PATH}";' not in conf_demo
          and f'Apt::Key::gpgvcommand "{GPGV_BIN_PATH}";' not in conf_demo)
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
        check("overlay STAGES the archive keyring ./usr/share/keyrings/ubuntu-archive-keyring.gpg",
              "./" + ARCHIVE_KEYRING_PATH in names)
        check("all members ./-rooted", all(n.startswith("./") for n in names))
        check("packed hosts pins the mirror IP",
              b"172.66.152.176\tports.ubuntu.com" in bodies["./etc/hosts"])
        check("packed authoritative sources name the hostname + Signed-By (authenticated)",
              b"http://ports.ubuntu.com/ubuntu-ports" in
              bodies["./etc/apt/sources.list.alr.d/alr-ports.sources"]
              and b"Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg" in
              bodies["./etc/apt/sources.list.alr.d/alr-ports.sources"]
              and b"Trusted: yes" not in
              bodies["./etc/apt/sources.list.alr.d/alr-ports.sources"])
        check("packed ubuntu.sources is neutralized (no live URIs)",
              b"Types: deb" not in bodies["./etc/apt/sources.list.d/ubuntu.sources"])
        check("packed apt.conf carries the absolute status pin",
              DEFAULT_ROOTFS_ABS.encode() in bodies["./etc/apt/apt.conf.d/99alr-mirror-ip"])
        check("packed apt.conf (authenticated) has NO AllowUnauthenticated directive",
              b"AllowUnauthenticated" not in bodies["./etc/apt/apt.conf.d/99alr-mirror-ip"])
        # staged keyring byte-matches the embedded archive keyring (2018+2012)
        check("packed keyring == embedded archive keyring bytes",
              bodies["./" + ARCHIVE_KEYRING_PATH] == archive_keyring_bytes())
        check("packed keyring starts with an OpenPGP public-key packet (0x99/0x98)",
              bodies["./" + ARCHIVE_KEYRING_PATH][:1] in (b"\x99", b"\x98"))
        check("result.file_count == 6 (hosts + 2 sources + ubuntu.sources + apt.conf + keyring)",
              res.file_count == 6)
        check("result is authenticated (trusted False) + reports keyring + rootfs_abs",
              res.trusted is False and res.rootfs_abs == DEFAULT_ROOTFS_ABS
              and res.keyring_path == "/" + ARCHIVE_KEYRING_PATH
              and res.keyring_fpr == UBUNTU_ARCHIVE_KEY_2018_FPR)
        check("result base_uri is the hostname URI",
              res.base_uri == "http://ports.ubuntu.com/ubuntu-ports")

        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(str(out))
        check("authenticated overlay is stage_tar_spec conformant", rep.conformant)
        check("authenticated overlay has no stage_tar warnings", rep.warnings == [])

    # --- archive keyring identity (the bytes we stage are the RIGHT key) ----- #
    kr = archive_keyring_bytes()
    check("embedded keyring sha256 == base rootfs 2018 key + 2012 key concat",
          hashlib.sha256(kr).hexdigest() ==
          hashlib.sha256(base64.b64decode(_KEY_2018_B64)
                         + base64.b64decode(_KEY_2012_B64)).hexdigest())
    check("embedded 2018 archive key sha256 matches base rootfs",
          hashlib.sha256(base64.b64decode(_KEY_2018_B64)).hexdigest() == _KEY_2018_SHA256)
    check("embedded 2012 cdimage key sha256 matches base rootfs",
          hashlib.sha256(base64.b64decode(_KEY_2012_B64)).hexdigest() == _KEY_2012_SHA256)
    fprs_2018 = _pgp_primary_fprs(base64.b64decode(_KEY_2018_B64))
    check("embedded 2018 key fingerprint == F6EC…C93C (Ubuntu Archive 2018 signing key)",
          UBUNTU_ARCHIVE_KEY_2018_FPR in fprs_2018)
    fprs_2012 = _pgp_primary_fprs(base64.b64decode(_KEY_2012_B64))
    check("embedded 2012 key fingerprint == Ubuntu CD Image 2012 key",
          UBUNTU_CDIMAGE_KEY_2012_FPR in fprs_2012)

    # --- --demo-trust pack: NO keyring, Trusted:yes, file_count 5 ------------- #
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "demo.tar"
        rd = build_apt_mirror_overlay(out, mirror="ports", scheme="http", trusted=True)
        with tarfile.open(out) as t:
            dnames = {m.name for m in t.getmembers()}
            dsrc = t.extractfile("./etc/apt/sources.list.alr.d/alr-ports.sources").read()
        check("demo-trust does NOT stage the keyring",
              "./" + ARCHIVE_KEYRING_PATH not in dnames)
        check("demo-trust sources carry Trusted: yes", b"Trusted: yes" in dsrc)
        check("demo-trust sources drop Signed-By", b"Signed-By:" not in dsrc)
        check("demo-trust file_count == 5 (no keyring)", rd.file_count == 5)
        check("demo-trust result is trusted + no keyring_path",
              rd.trusted is True and rd.keyring_path is None)
        rep_d = validate_stage_tar(str(out))
        check("demo-trust overlay is stage_tar_spec conformant", rep_d.conformant)

    # --- keyring_from_rootfs round-trips against the live base rootfs --------- #
    _rootfs = Path(__file__).resolve().parent.parent / "rootfs" / "tiny-rootfs.tar"
    if _rootfs.is_file():
        extracted = keyring_from_rootfs(_rootfs)
        check("keyring_from_rootfs(tiny-rootfs.tar) byte-matches the embedded keyring",
              extracted == archive_keyring_bytes())
    else:
        check("keyring_from_rootfs skipped (no rootfs/tiny-rootfs.tar handy)", True)

    # --- debian mirror: authenticated but keyring NOT staged (base owns it) -- #
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "debian.tar"
        rdeb = build_apt_mirror_overlay(out, mirror="debian", scheme="http")
        with tarfile.open(out) as t:
            dnames = {m.name for m in t.getmembers()}
        check("debian authenticated mode does NOT stage an Ubuntu keyring",
              "./" + ARCHIVE_KEYRING_PATH not in dnames)
        check("debian result has no keyring_path (uses base debian keyring)",
              rdeb.keyring_path is None and rdeb.trusted is False)

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


def _pgp_primary_fprs(blob: bytes) -> set[str]:
    """Return the v4 fingerprints of the primary public-key packets in a binary
    OpenPGP keyring blob (SHA-1 over 0x99 || 2-byte-len || packet body). Used by
    the selftest to prove the embedded key bytes ARE the expected archive key."""
    import struct as _struct

    out: set[str] = set()
    i, n = 0, len(blob)
    while i < n:
        tag = blob[i]
        if not (tag & 0x80):
            break
        if tag & 0x40:  # new format
            ptag = tag & 0x3f
            i += 1
            l = blob[i]
            if l < 192:
                length = l; i += 1
            elif l < 224:
                length = ((l - 192) << 8) + blob[i + 1] + 192; i += 2
            elif l == 255:
                length = _struct.unpack(">I", blob[i + 1:i + 5])[0]; i += 5
            else:
                length = 1 << (l & 0x1f); i += 1
        else:  # old format
            ptag = (tag >> 2) & 0x0f
            ltype = tag & 0x03
            i += 1
            if ltype == 0:
                length = blob[i]; i += 1
            elif ltype == 1:
                length = _struct.unpack(">H", blob[i:i + 2])[0]; i += 2
            elif ltype == 2:
                length = _struct.unpack(">I", blob[i:i + 4])[0]; i += 4
            else:
                length = n - i
        body = blob[i:i + length]
        if ptag == 6 and body and body[0] == 4:  # primary public key, v4
            pref = b"\x99" + _struct.pack(">H", len(body)) + body
            out.add(hashlib.sha1(pref).hexdigest().upper())
        i += length
    return out


if __name__ == "__main__":
    raise SystemExit(main())
