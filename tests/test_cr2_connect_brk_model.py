"""CR-2 connect/TLS `brk` model — host regression of the prerequisite decision.

Host-side regression of tools/cr2_connect_brk_model.classify_connect, the
executable spec for chromium's https-fetch layers under ALR (untrusted_app).
Pins the device-confirmed root-cause chain so a dropped NSS plugin, a missing CA
bundle, a reverted setsockopt errno-massage, or a wrong DoH bootstrap turns red
on the host BEFORE any drain.

  docs/design/cr2-connect-brk.md
  docs/design/chromium-dns-doh.md
  tools/build_nss_overlay.py  (the L2 fix this model mirrors)

NO device, NO ptrace, NO network — only the layer-arithmetic. The substitution
EFFECTS (the actual dlopen succeeding, the TLS bytes flowing) are DEVICE-ONLY
(DEVICE-REQ: ALR-CR2-nss / ALR-CR2-doh / ALR-CR2-tls).
"""
from __future__ import annotations

import pytest

from tools.cr2_connect_brk_model import (
    FAIL_BRK,
    FAIL_HANG,
    FAIL_NONE,
    FAIL_SOFT_NET_ERROR,
    NSS_FIRST_DLOPEN,
    NSS_PLUGIN_MODULES,
    NSS_SOFTOKN_TRANSITIVE,
    ConnectVerdict,
    FetchRequest,
    NetStage,
    classify_connect,
    fully_staged,
)

# Stand-ins for the two probe URLs the device A/B used.
IP_LITERAL = FetchRequest(ip_literal=True)
HOSTNAME = FetchRequest(ip_literal=False)
HOSTNAME_DOH = FetchRequest(ip_literal=False, doh_flags_set=True)


# --------------------------------------------------------------------------- #
# THE headline result: the IP-literal ~6s brk is L2 NSS, and ONLY a complete NSS
# closure clears it. This is the whole diagnosis in one assertion.
# --------------------------------------------------------------------------- #
def test_ip_literal_brk_is_nss_when_plugins_missing():
    # The pre-v163 state: net overlay staged (CA bundle ok) but NO NSS plugins.
    stage = NetStage(nss_plugins_present=frozenset(), ca_bundle_present=True)
    v = classify_connect(stage, IP_LITERAL)
    assert v.layer == "L2-nss"
    assert v.failure == FAIL_BRK
    assert v.fatal
    assert "nss_error=-5925" in v.detail
    assert "build_nss_overlay" in (v.fix or "")


def test_ip_literal_passes_once_nss_complete():
    # v163 fix: every NSS plugin + the sqlite transitive staged, CA bundle present.
    v = classify_connect(fully_staged(), IP_LITERAL)
    assert v.passes
    assert v.failure == FAIL_NONE
    assert not v.fatal


# --------------------------------------------------------------------------- #
# The brk is FATAL even if only ONE plugin (the first-dlopen'd softoken) is
# missing — partial staging does not help. Mirrors the device evidence naming
# exactly libsoftokn3.so.
# --------------------------------------------------------------------------- #
def test_missing_only_softokn_still_brks():
    have = (set(NSS_PLUGIN_MODULES) | set(NSS_SOFTOKN_TRANSITIVE)) - {NSS_FIRST_DLOPEN}
    stage = NetStage(nss_plugins_present=frozenset(have), ca_bundle_present=True)
    v = classify_connect(stage, IP_LITERAL)
    assert v.failure == FAIL_BRK
    assert NSS_FIRST_DLOPEN in v.detail


def test_missing_only_sqlite_transitive_still_brks():
    # libsoftokn3 present but its DT_NEEDED libsqlite3.so.0 dropped -> still FATAL
    # (the device-confirmed "libsqlite3.so.0: cannot open shared object file").
    have = set(NSS_PLUGIN_MODULES)  # all 5 plugins, but NOT the sqlite transitive
    stage = NetStage(nss_plugins_present=frozenset(have), ca_bundle_present=True)
    v = classify_connect(stage, IP_LITERAL)
    assert v.failure == FAIL_BRK
    assert "libsqlite3.so.0" in v.detail


# --------------------------------------------------------------------------- #
# Layer ORDERING: NSS (L2, brk) is reached BEFORE TLS/cert (L3, soft). So with
# NSS missing the verdict is the brk regardless of the CA bundle — the brk masks
# any later cert gap. This is why the device saw a crash, not a cert error.
# --------------------------------------------------------------------------- #
def test_nss_brk_masks_missing_ca_bundle():
    stage = NetStage(nss_plugins_present=frozenset(), ca_bundle_present=False)
    v = classify_connect(stage, IP_LITERAL)
    assert v.layer == "L2-nss"        # NOT L3 — NSS crashes first
    assert v.failure == FAIL_BRK


# --------------------------------------------------------------------------- #
# L3 cert gap is RECOVERABLE: once NSS is complete, a missing CA bundle is a SOFT
# net error, not a crash. The key fatal-vs-soft contract.
# --------------------------------------------------------------------------- #
def test_missing_ca_bundle_is_soft_not_brk():
    stage = NetStage(
        nss_plugins_present=frozenset(NSS_PLUGIN_MODULES) | frozenset(NSS_SOFTOKN_TRANSITIVE),
        ca_bundle_present=False,
    )
    v = classify_connect(stage, IP_LITERAL)
    assert v.layer == "L3-tls"
    assert v.failure == FAIL_SOFT_NET_ERROR
    assert not v.fatal                # the process LIVES; page just fails to load


# --------------------------------------------------------------------------- #
# L0 DNS: a HOSTNAME hangs (~180s) unless DoH flags AND the bootstrap pin are
# both present. IP-literal never enters L0. This is the timing split that first
# separated the DNS hang from the NSS brk on device.
# --------------------------------------------------------------------------- #
def test_hostname_without_doh_hangs():
    v = classify_connect(fully_staged(), HOSTNAME)  # fully staged BUT no DoH flag
    # fully_staged() pins the DoH host, but the REQUEST lacks the DoH flags.
    assert v.layer == "L0-dns"
    assert v.failure == FAIL_HANG
    assert v.fatal                    # a 180s hang never completes


def test_hostname_with_doh_and_pin_clears_dns():
    # DoH flags set AND the host pinned -> L0 clears; with full NSS+CA it passes.
    v = classify_connect(fully_staged(), HOSTNAME_DOH)
    assert v.passes


def test_hostname_doh_flags_but_no_pin_still_hangs():
    # DoH flags set but the bootstrap host NOT pinned -> DoH can't resolve its own
    # server -> still a hang (the chicken-and-egg the /etc/hosts pin breaks).
    stage = NetStage(
        nss_plugins_present=frozenset(NSS_PLUGIN_MODULES) | frozenset(NSS_SOFTOKN_TRANSITIVE),
        ca_bundle_present=True,
        doh_host_pinned=False,
    )
    v = classify_connect(stage, HOSTNAME_DOH)
    assert v.layer == "L0-dns"
    assert v.failure == FAIL_HANG


def test_ip_literal_ignores_doh_state():
    # IP-literal never needs DNS; DoH/pin state is irrelevant to its verdict.
    no_doh = NetStage(
        nss_plugins_present=frozenset(NSS_PLUGIN_MODULES) | frozenset(NSS_SOFTOKN_TRANSITIVE),
        ca_bundle_present=True,
        doh_host_pinned=False,
    )
    v = classify_connect(no_doh, IP_LITERAL)
    assert v.passes                   # NSS+CA complete -> IP-literal succeeds


# --------------------------------------------------------------------------- #
# L1 SO_MARK is NOT the brk (the disproved prediction). Even with NO errno-massage
# AND every other layer satisfied, the IP-literal fetch PASSES — i.e. SO_MARK
# never decides the verdict on this device. This guards against re-promoting the
# SO_MARK story to "the brk".
# --------------------------------------------------------------------------- #
def test_so_mark_is_not_the_brk():
    no_massage = NetStage(
        nss_plugins_present=frozenset(NSS_PLUGIN_MODULES) | frozenset(NSS_SOFTOKN_TRANSITIVE),
        ca_bundle_present=True,
        setsockopt_errno_massage=False,   # the wrapper absent
    )
    v = classify_connect(no_massage, IP_LITERAL)
    assert v.passes                   # absence of the SO_MARK massage does NOT brk
    assert v.layer != "L1-socket"


# --------------------------------------------------------------------------- #
# Model <-> overlay-builder sync: the plugin set this model reasons about MUST
# equal what tools/build_nss_overlay actually stages. If the builder's list and
# the model drift, the host test that "predicts" CR-2 would be lying.
# --------------------------------------------------------------------------- #
def test_model_plugin_set_matches_nss_overlay_builder():
    from tools import build_nss_overlay as nss
    assert set(NSS_PLUGIN_MODULES) == set(nss.MODULES)
    assert NSS_SOFTOKN_TRANSITIVE == (nss.SQLITE_SONAME,)
    assert NSS_FIRST_DLOPEN in nss.MODULES


# --------------------------------------------------------------------------- #
# Verdict helpers behave (fatal/passes are mutually consistent).
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize(
    "failure,fatal,passes",
    [
        (FAIL_NONE, False, True),
        (FAIL_HANG, True, False),
        (FAIL_BRK, True, False),
        (FAIL_SOFT_NET_ERROR, False, False),
    ],
)
def test_verdict_flags(failure, fatal, passes):
    v = ConnectVerdict(layer="x", failure=failure, detail="", fix=None)
    assert v.fatal is fatal
    assert v.passes is passes
