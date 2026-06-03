"""Host tests for tools/build_apt_mirror_overlay.py (apt-without-DNS mirror pin).

All OFFLINE — the overlay is pure config (no .deb download, no network). The
on-host reachability of the index/pool through the pinned IP is proven by hand
(curl --resolve / the builder's --verify-fetch, see
docs/research/apt-mirror-ip-no-dns.md); the real in-app `apt update` is the
integration/device gate.

Contract:
  * `ports` mirror is Ubuntu noble **arm64** (ubuntu-ports), NOT archive.ubuntu.com;
  * the pinned IPs are members of the CDN's published anycast ranges (durable);
  * /etc/hosts pins host -> each anycast IP (so glibc resolves it with no DNS);
  * the deb822 .sources name the HOSTNAME (never a bare IP) so HTTPS SNI/cert match;
  * apt is isolated to the ALR ports stanza via Dir::Etc (ignores base sources),
    and the base cloud-init ubuntu.sources is overwritten with a neutralizer;
  * by default the stanza is Trusted: yes + AllowUnauthenticated (DEMO GPG skip —
    the device apt-key is broken; real GPG is a follow-up); `trusted=False`
    restores Signed-By keyring verification (the original integrity-on behavior);
  * the packed overlay is §5-E ./-rooted and stage_tar_spec conformant.

These mirror the device evidence that drove the overlay's final shape: with this
overlay `apt-get update` reaches exit 0 with no E: errors on a DNS-blocked device
(the only residual is a NATIVE realpath edge on the in-rootfs dpkg status, dodged
via `--status-path` pointing at a status copy outside the rootfs).
"""

from __future__ import annotations

import ipaddress
import tarfile
from pathlib import Path

import pytest

from tools import build_apt_mirror_overlay as bam
from tools.build_apt_mirror_overlay import (
    APT_CONF_BODY,
    APT_CONF_PATH,
    HOSTS_PATH,
    MIRRORS,
    Mirror,
    build_apt_mirror_overlay,
    build_hosts_body,
    build_sources_body,
    resolve_mirror,
)
from tools.stage_tar_spec import validate_stage_tar


# --------------------------------------------------------------------------- #
# Builder exists; selftest is green
# --------------------------------------------------------------------------- #

def test_builder_module_file_exists():
    assert Path(bam.__file__).is_file()


def test_selftest_passes():
    assert bam._selftest() == 0


# --------------------------------------------------------------------------- #
# Mirror catalog — arch correctness + IP-range stability
# --------------------------------------------------------------------------- #

def test_ports_is_ubuntu_ports_arm64_not_archive():
    m = MIRRORS["ports"]
    assert m.host == "ports.ubuntu.com"
    # ubuntu-ports is the arm64 archive; archive.ubuntu.com is amd64/i386 only.
    assert m.archive_path == "ubuntu-ports"
    assert "archive.ubuntu.com" != m.host
    assert m.suite == "noble"


@pytest.mark.parametrize(
    "ip,ranges",
    [
        ("172.66.152.176", ("172.64.0.0/13",)),   # Cloudflare
        ("104.20.28.246", ("104.16.0.0/13",)),    # Cloudflare
        ("146.75.50.132", ("146.75.0.0/17",)),    # Fastly
        ("151.101.0.204", ("151.101.0.0/16",)),   # Fastly
    ],
)
def test_pinned_ips_are_in_published_anycast_ranges(ip, ranges):
    a = ipaddress.ip_address(ip)
    assert any(a in ipaddress.ip_network(c) for c in ranges)


def test_catalog_pins_match_their_published_ranges():
    # every catalog mirror's bootstrap IPs fall in its declared cdn_ranges
    for m in MIRRORS.values():
        nets = [ipaddress.ip_network(c) for c in m.cdn_ranges]
        for ip in m.bootstrap:
            a = ipaddress.ip_address(ip)
            assert any(a in n for n in nets), f"{ip} not in {m.cdn_ranges} for {m.key}"


def test_resolve_mirror_rejects_unknown():
    with pytest.raises(ValueError):
        resolve_mirror("nope")


# --------------------------------------------------------------------------- #
# /etc/hosts — host -> anycast IP, with no DNS needed
# --------------------------------------------------------------------------- #

def test_hosts_pins_each_anycast_ip():
    m = MIRRORS["ports"]
    body = build_hosts_body(m)
    assert "127.0.0.1\tlocalhost" in body
    for ip in m.bootstrap:
        assert f"{ip}\t{m.host}" in body
    assert m.cdn in body  # the comment names the CDN for the reader


def test_hosts_extra_pin():
    m = MIRRORS["ports"]
    body = build_hosts_body(m, extra_hosts=(("security.ubuntu.com", ("1.2.3.4",)),))
    assert "1.2.3.4\tsecurity.ubuntu.com" in body


# --------------------------------------------------------------------------- #
# sources name the HOSTNAME (not a bare IP) — SNI/cert correctness
# --------------------------------------------------------------------------- #

def test_sources_name_hostname_never_ip_http():
    m = MIRRORS["ports"]
    body = build_sources_body(m, scheme="http")
    assert "URIs: http://ports.ubuntu.com/ubuntu-ports" in body
    for ip in m.bootstrap:
        assert ip not in body  # the URI must use the hostname, not a literal IP
    assert "Suites: noble noble-updates noble-security" in body
    assert "Types: deb" in body
    # default (device demo) is Trusted: yes (apt-key is broken on device); the
    # signed-keyring path is opt-in via trusted=False (see below).
    assert "Trusted: yes" in body
    assert "Signed-By:" not in body


def test_sources_trusted_false_restores_signed_by():
    m = MIRRORS["ports"]
    body = build_sources_body(m, scheme="http", trusted=False)
    # the original integrity-on behavior: GPG-signed Release via the archive keyring
    assert "Signed-By:" in body and "ubuntu-archive-keyring.gpg" in body
    assert "Trusted: yes" not in body


def test_sources_https_uses_hostname_for_sni():
    m = MIRRORS["ports"]
    body = build_sources_body(m, scheme="https")
    assert "URIs: https://ports.ubuntu.com/ubuntu-ports" in body


def test_debian_sources_shape():
    m = MIRRORS["debian"]
    # check the keyring on the trusted=False (signed) variant
    body = build_sources_body(m, scheme="http", trusted=False)
    assert "URIs: http://deb.debian.org/debian" in body
    assert "debian-archive-keyring.gpg" in body
    # Debian serves -security from a separate host, so the stanza omits it here.
    assert "noble-security" not in body
    assert "Suites: bookworm bookworm-updates" in body


def test_bad_scheme_rejected():
    with pytest.raises(ValueError):
        build_apt_mirror_overlay("/tmp/ignored.tar", scheme="ftp")


# --------------------------------------------------------------------------- #
# apt.conf keeps integrity ON
# --------------------------------------------------------------------------- #

def test_apt_conf_isolates_sources_and_demo_trusts():
    # round-trip trimming retained
    assert 'Acquire::Languages "none";' in APT_CONF_BODY
    assert 'Acquire::ForceIPv4 "true";' in APT_CONF_BODY
    # (1) Dir::Etc isolation: apt reads ONLY the ALR ports stanza, ignoring the base
    # cloud-init ubuntu.sources + github-cli/tailscale (device-proven necessary).
    assert 'Dir::Etc::sourcelist "/dev/null";' in APT_CONF_BODY
    assert "Dir::Etc::sourceparts" in APT_CONF_BODY and "sources.list.alr.d" in APT_CONF_BODY
    # (2) default trusted: AllowUnauthenticated (device apt-key is broken).
    assert 'APT::Get::AllowUnauthenticated "true";' in APT_CONF_BODY
    # (3) status pin to the absolute in-rootfs DB path (default).
    assert "Dir::State::status" in APT_CONF_BODY and bam.DEFAULT_ROOTFS_ABS in APT_CONF_BODY
    # (4) device-env hygiene: the PackageKit/c-n-f Post-Invoke hook lists are
    # #cleared (not blanked — assigning "" only appends an empty entry), run as root.
    assert "#clear APT::Update::Post-Invoke-Success;" in APT_CONF_BODY
    assert 'APT::Sandbox::User "root";' in APT_CONF_BODY


def test_apt_conf_trusted_false_drops_allow_unauth():
    body = bam.build_apt_conf_body(trusted=False)
    assert "AllowUnauthenticated" not in body
    # isolation + hygiene still apply regardless of trust
    assert 'Dir::Etc::sourcelist "/dev/null";' in body
    assert "#clear APT::Update::Post-Invoke-Success;" in body


def test_apt_conf_status_path_overrides_to_outside_rootfs():
    # the realpath-edge workaround: point Dir::State::status at a DB copy that is
    # NOT under the rootfs (so the path interposer leaves realpath alone).
    body = bam.build_apt_conf_body(status_path="/data/local/tmp/alr-dpkg-status")
    assert 'Dir::State::status "/data/local/tmp/alr-dpkg-status";' in body
    assert f'Dir::State::status "{bam.DEFAULT_ROOTFS_ABS}/var/lib/dpkg/status";' not in body


# --------------------------------------------------------------------------- #
# Full pack — §5-E conformance + contents
# --------------------------------------------------------------------------- #

def test_full_pack_is_conformant_and_complete(tmp_path):
    out = tmp_path / "apt-mirror-stage.tar"
    res = build_apt_mirror_overlay(out, mirror="ports", scheme="http")

    with tarfile.open(out) as t:
        names = {m.name: m for m in t.getmembers()}
        bodies = {n: t.extractfile(m).read() for n, m in names.items() if m.isfile()}

    assert "./" + HOSTS_PATH in names
    # the AUTHORITATIVE stanza apt actually reads (Dir::Etc::sourceparts -> here)
    assert "./etc/apt/sources.list.alr.d/alr-ports.sources" in names
    # the conventional-dir copy is still shipped (humans / direct tools)
    assert "./etc/apt/sources.list.d/alr-ports.sources" in names
    # the base cloud-init ubuntu.sources is OVERWRITTEN with a neutralizer
    assert "./etc/apt/sources.list.d/ubuntu.sources" in names
    assert "./" + APT_CONF_PATH in names
    assert all(n.startswith("./") for n in names)

    assert b"172.66.152.176\tports.ubuntu.com" in bodies["./" + HOSTS_PATH]
    assert (b"http://ports.ubuntu.com/ubuntu-ports"
            in bodies["./etc/apt/sources.list.alr.d/alr-ports.sources"])
    assert b"Trusted: yes" in bodies["./etc/apt/sources.list.alr.d/alr-ports.sources"]
    # the neutralized ubuntu.sources carries no live repo stanza
    assert b"Types: deb" not in bodies["./etc/apt/sources.list.d/ubuntu.sources"]

    # hosts + 2x alr sources (alr.d + list.d) + ubuntu.sources + apt.conf = 5 files
    assert res.file_count == 5
    assert res.base_uri == "http://ports.ubuntu.com/ubuntu-ports"

    rep = validate_stage_tar(str(out))
    assert rep.conformant, rep.errors
    assert rep.warnings == []


def test_bootstrap_override(tmp_path):
    out = tmp_path / "custom.tar"
    res = build_apt_mirror_overlay(
        out, mirror="ports", scheme="http", bootstrap_ips=("203.0.113.7",)
    )
    with tarfile.open(out) as t:
        hosts = t.extractfile("./" + HOSTS_PATH).read()
    assert b"203.0.113.7\tports.ubuntu.com" in hosts
    assert b"172.66.152.176" not in hosts  # catalog IP dropped
    assert res.bootstrap_ips == ("203.0.113.7",)


def test_custom_mirror_object_roundtrips(tmp_path):
    m = Mirror(
        key="kr",
        host="mirror.example.kr",
        archive_path="ubuntu-ports",
        suite="noble",
        components=("main", "universe"),
        bootstrap=("198.51.100.9",),
        cdn="static",
        cdn_ranges=(),
    )
    out = tmp_path / "kr.tar"
    res = build_apt_mirror_overlay(out, mirror=m, scheme="http")
    with tarfile.open(out) as t:
        hosts = t.extractfile("./" + HOSTS_PATH).read()
        src = t.extractfile("./etc/apt/sources.list.d/alr-kr.sources").read()
    assert b"198.51.100.9\tmirror.example.kr" in hosts
    assert b"URIs: http://mirror.example.kr/ubuntu-ports" in src
    assert res.host == "mirror.example.kr"
