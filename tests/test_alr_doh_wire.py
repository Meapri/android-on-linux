"""Host tests for the ALR DNS-over-HTTPS (DoH) name-resolution shim.

Two layers:

1. BEHAVIOURAL — compile and run the TLS-independent DoH wire-format core
   (app/src/main/cpp/alr_doh/alr_doh_wire.c) together with its native unit test
   (tests/native_alr_doh_wire_test.c) using a host cc. This exercises the DNS
   query encoder, base64url, and the response parser against golden messages.
   The host build box blocks outbound 443 (so a live DoH query is impossible),
   which is exactly why the byte-level encode/decode is what we pin here.

2. STRUCTURAL — assert the shim source, the wire header/impl, and the build
   script carry the load-bearing contract (getaddrinfo interposition, dlopen'd
   OpenSSL transport, RFC 8484 GET framing, CA-bundle verification, the rootfs
   install path), and that the device shim cross-compiles for arm64 with zig
   when zig is available.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
DOH_DIR = ROOT / "app/src/main/cpp/alr_doh"
WIRE_H = DOH_DIR / "alr_doh_wire.h"
WIRE_C = DOH_DIR / "alr_doh_wire.c"
SHIM_C = DOH_DIR / "libalr_doh.c"
NATIVE_TEST = ROOT / "tests/native_alr_doh_wire_test.c"
BUILD_SH = ROOT / "scripts/build-doh.sh"


def _cc():
    for c in ("cc", "clang", "gcc"):
        p = shutil.which(c)
        if p:
            return p
    return None


# ----------------------------- behavioural ----------------------------- #


def test_doh_wire_core_unit_tests_pass(tmp_path):
    """Compile + run the wire core's native unit tests; require exit 0."""
    cc = _cc()
    if not cc:
        pytest.skip("no host C compiler (cc/clang/gcc) available")
    bin_ = tmp_path / "alr-doh-wire-test"
    cmd = [
        cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-I", str(DOH_DIR),
        str(NATIVE_TEST), str(WIRE_C),
        "-o", str(bin_),
    ]
    cp = subprocess.run(cmd, capture_output=True, text=True)
    assert cp.returncode == 0, f"compile failed:\n{cp.stderr}"
    run = subprocess.run([str(bin_)], capture_output=True, text=True)
    assert run.returncode == 0, f"wire core tests failed:\n{run.stdout}\n{run.stderr}"
    assert "all tests passed" in run.stdout


# ----------------------------- structural ----------------------------- #


def test_wire_header_declares_core_api():
    h = WIRE_H.read_text()
    for sym in (
        "alr_doh_build_query",
        "alr_doh_base64url",
        "alr_doh_parse_response",
        "alr_doh_result",
        "ALR_DNS_T_A",
        "ALR_DNS_T_AAAA",
        "ALR_DNS_T_CNAME",
        "ALR_DNS_RCODE_NXDOMAIN",
    ):
        assert sym in h, f"missing {sym} in wire header"


def test_wire_impl_is_dependency_free_and_bounds_checked():
    c = WIRE_C.read_text()
    # The core must NOT pull sockets / OpenSSL / arpa-inet — it is the host-
    # testable, transport-free layer. Check actual #include lines (not prose in
    # comments, which legitimately mention these names).
    include_lines = [
        ln.strip()
        for ln in c.splitlines()
        if ln.lstrip().startswith("#include")
    ]
    for forbidden in ("<arpa/inet.h>", "<sys/socket.h>", "<openssl"):
        assert not any(forbidden in ln for ln in include_lines), \
            f"wire core must not #include {forbidden}"
    assert "dlopen" not in c, "wire core must not call dlopen"
    # compression-pointer loop guard must be present
    assert "MAX_JUMPS" in c
    assert "skip_name" in c


def test_shim_interposes_getaddrinfo_over_doh():
    s = SHIM_C.read_text()
    # interposes the glibc resolver entry points
    assert "int getaddrinfo(" in s
    assert "void freeaddrinfo(" in s
    assert 'dlsym(RTLD_NEXT, "getaddrinfo")' in s
    # transport is dlopen'd OpenSSL (no link-time dependency)
    assert "libssl.so.3" in s
    assert "libcrypto.so.3" in s
    assert "TLS_client_method" in s
    assert "SSL_connect" in s
    # RFC 8484 GET framing + dns-message accept
    assert "?dns=" in s
    assert "application/dns-message" in s
    # uses the wire core
    assert "alr_doh_build_query" in s
    assert "alr_doh_parse_response" in s


def test_shim_verifies_certificate_by_default():
    s = SHIM_C.read_text()
    # peer verification ON with the common-data CA bundle, gated off only by an
    # explicit ALR_DOH_INSECURE=1 debug switch.
    assert "SSL_CTX_load_verify_locations" in s
    assert "SSL_CTX_set_verify" in s
    assert "SSL_get_verify_result" in s
    assert "/etc/ssl/certs/ca-certificates.crt" in s
    assert "ALR_DOH_INSECURE" in s
    # SNI must be set so the right cert is served / verified
    assert "SET_TLSEXT_HOSTNAME" in s


def test_shim_falls_back_and_skips_numeric_hosts():
    s = SHIM_C.read_text()
    # must defer to the real resolver for numeric hosts / AI_NUMERICHOST and on
    # any DoH failure (never WORSE than baseline).
    assert "is_numeric_host" in s
    assert "AI_NUMERICHOST" in s
    assert "real_getaddrinfo" in s
    # default-on switch
    assert "ALR_DOH" in s


def test_shim_default_doh_server_is_ip_literal_no_bootstrap():
    s = SHIM_C.read_text()
    # the DoH server is pinned by IP so resolving the resolver needs no DNS.
    assert "1.1.1.1" in s          # Cloudflare default
    assert "ALR_DOH_SERVER" in s   # overridable


def test_build_script_targets_arm64_and_rootfs_path():
    sh = BUILD_SH.read_text()
    assert "aarch64-linux-gnu" in sh
    assert "zig cc" in sh.replace('"$ZIG"', "zig cc").replace("$ZIG", "zig") or "ZIG" in sh
    assert "./usr/lib/androlinux/libalr_doh.so" in sh
    assert "libalr_doh.c" in sh
    assert "alr_doh_wire.c" in sh


def test_device_shim_cross_compiles_with_zig(tmp_path):
    """If zig is present, the device .so must cross-compile clean for arm64."""
    zig = shutil.which("zig")
    if not zig:
        pytest.skip("zig not available for cross-compile check")
    out = tmp_path / "libalr_doh.so"
    cmd = [
        zig, "cc", "--target=aarch64-linux-gnu.2.36",
        "-shared", "-fPIC", "-O2", "-Wall", "-Werror",
        "-I", str(DOH_DIR),
        str(SHIM_C), str(WIRE_C),
        "-o", str(out),
    ]
    cp = subprocess.run(cmd, capture_output=True, text=True)
    assert cp.returncode == 0, f"zig cross-compile failed:\n{cp.stderr}"
    assert out.exists() and out.stat().st_size > 0
