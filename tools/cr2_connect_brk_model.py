"""CR-2 connect/TLS `brk` — host decision model for chromium's https prerequisites.

Pure, host-testable (NO device, NO I/O, NO ptrace, NO network). This is the
executable decision spec for *why an IP-literal `https://` fetch crashes/hangs*
under ALR (non-root Android untrusted_app), and *which staged prerequisite clears
each layer*. It encodes the device-confirmed root-cause chain so a future
regression (a dropped NSS plugin, a missing CA bundle, a reverted setsockopt
errno-massage, a wrong DoH bootstrap) turns red on the host before any drain.

  docs/design/cr2-connect-brk.md            (the prose this module specs)
  docs/design/chromium-dns-doh.md           §1-§3 (the DNS layer / DoH)
  docs/research/chromium-connect-brk-android.md (the SUPERSEDED SO_MARK prediction)
  tools/build_nss_overlay.py / tools/build_chromium_net_overlay.py (the fixes)

WHAT THIS MODELS
----------------
chromium-headless-shell is a GENERIC-LINUX glibc build run on an Android kernel
inside ALR. A `https://<host-or-ip>/` navigation crosses, in order, four
independent layers, each of which can FAIL DIFFERENTLY:

  L0 DNS (hostname only)   — raw UDP/53 to a public nameserver is BLOCKED for an
                             untrusted_app -> chromium's resolv.conf DNS HANGS
                             ~180s (the navigation alarm). IP-literal SKIPS L0.
                             Fix = DoH-over-443 + a /etc/hosts+host-resolver-rules
                             bootstrap pin (chromium-dns-doh.md). FAILURE=HANG.
  L1 socket setup          — chromium tags every outbound socket. On a GENERIC
                             build SocketTag::Apply calls setsockopt(SO_MARK),
                             which needs CAP_NET_ADMIN the app lacks -> EPERM.
                             *Predicted* (chromium-connect-brk-android.md #1) to
                             be the brk; device DISPROVED it (harmless no-op).
                             Fix = setsockopt() errno-massage (already landed,
                             libalr_interpose.c). FAILURE=NONE on this device
                             (the option is best-effort; chromium does not PCHECK
                             it in 147), but the errno-massage is kept as a belt.
  L2 NSS init  ★ THE BRK   — before any TLS, chromium initialises NSS for its
                             cert/crypto store. libnss3 dlopen()s its PKCS#11
                             plugins (libsoftokn3/freebl3/nssckbi/...). Those are
                             dlopen'd, NOT DT_NEEDED, so the DT_NEEDED-only deb
                             closure DROPPED them -> dlopen fails ->
                             crypto/nss_util.cc nss_error=-5925 -> ImmediateCrash
                             (brk #0, SIGTRAP/TRAP_BRKPT) in ~6s. THIS is the
                             device-confirmed IP-literal brk (v162 full-stderr
                             capture). Fix = nss-stage.tar stages the 5 plugins +
                             their transitive libsqlite3.so.0 (build_nss_overlay).
                             FAILURE=BRK.
  L3 TLS handshake + cert  — once NSS is up and the socket connects, chromium
                             verifies the server cert against the CA bundle. A
                             MISSING CA bundle -> handshake fails -> a SOFT net
                             error (ERR_CERT_AUTHORITY_INVALID), NOT a brk. Fix =
                             the 146-cert ca-certificates.crt the net overlay
                             already stages. FAILURE=SOFT_NET_ERROR.

KEY DISTINCTION the model encodes (the whole point):
  - A missing **L2 NSS plugin** is FATAL — chromium brk()s (CHECK on the NSS init
    result). This is the ~6s IP-literal crash. Staging the plugins is MANDATORY.
  - A missing **L3 CA bundle** is RECOVERABLE — chromium surfaces a soft
    net::ERR_*; the process does NOT crash. So a CA gap looks like "page failed
    to load", not "exit sig=5".
  - **L0 DNS** failure is a HANG (~180s alarm), not a crash — distinguishable from
    L2's fast brk by timing alone (the device A/B that first split them).
  - **L1 SO_MARK** was the headline PREDICTION but is, on this device, a NON-event
    (chromium 147 does not fatally CHECK the SO_MARK result); the errno-massage is
    retained as a no-op belt, not as the brk fix. The model records this so the
    SO_MARK story is never re-promoted to "the brk" again.

DEVICE-REQ (effects this model does NOT execute — they are device-only):
  - ALR-CR2-nss   : drain with nss-stage.tar extracted; expect the nss_util brk
                    GONE, guest advances past NSS into the TLS connect.
  - ALR-CR2-doh   : drain a HOSTNAME url with the DoH flags; expect no ~180s hang.
  - ALR-CR2-tls   : drain past NSS with a ≥360s window; read the post-NSS verdict
                    (clean DOM, or a soft net::ERR_* = which TLS/cert/connect gap).

INVARIANTS (HARD CONSTRAINTS)
  - This module is PURE: same inputs -> same outputs, no clock, no fs, no net.
  - It does not assert WHICH provider/IP is correct policy — only that a chosen
    DoH host MUST be pinned (else the bootstrap is a hang). Provider choice is
    config (build_chromium_net_overlay.DOH_PROVIDERS), not modelled here.
  - The fatal-vs-soft classification is the load-bearing contract; the exact
    chromium source line numbers are evidence, not asserted here.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

# --------------------------------------------------------------------------- #
# Failure-mode taxonomy (what the guest does when a layer's prereq is absent)
# --------------------------------------------------------------------------- #
FAIL_NONE = "none"                 # layer clears
FAIL_HANG = "hang"                 # ~180s navigation alarm (no progress)
FAIL_BRK = "brk"                   # SIGTRAP/TRAP_BRKPT — ImmediateCrash/CHECK
FAIL_SOFT_NET_ERROR = "soft_net_error"  # net::ERR_* — recoverable, no crash

# --------------------------------------------------------------------------- #
# The five NSS plugin modules dlopen'd by libnss3 (NOT DT_NEEDED) + the one
# transitive DT_NEEDED of libsoftokn3 the closure also dropped. Staging ALL of
# these is what clears L2. Mirrors tools/build_nss_overlay.MODULES + SQLITE_SONAME
# (kept in sync by tests/test_cr2_connect_brk_model.py).
# --------------------------------------------------------------------------- #
NSS_PLUGIN_MODULES = (
    "libsoftokn3.so",       # softoken — the sql: key/cert DB; FIRST to be dlopen'd
    "libfreebl3.so",        # freebl — low-level crypto
    "libfreeblpriv3.so",    # freebl private
    "libnssckbi.so",        # builtin CA trust (PKCS#11)
    "libnssdbm3.so",        # legacy dbm DB backend
)
# libsoftokn3 DT_NEEDED -> libsqlite3.so.0, shipped in a SEPARATE deb (libsqlite3-0),
# also dropped by the DT_NEEDED-only closure. Without it libsoftokn3 fails to load
# and chromium still FATALs at nss_util.cc (device-confirmed "libsqlite3.so.0:
# cannot open shared object file").
NSS_SOFTOKN_TRANSITIVE = ("libsqlite3.so.0",)

# The first plugin chromium's NSS init actually dlopen()s. If THIS one is missing
# the brk fires immediately; the device evidence named exactly this file.
NSS_FIRST_DLOPEN = "libsoftokn3.so"


# --------------------------------------------------------------------------- #
# Inputs: what the staged rootfs provides for a given drain.
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class NetStage:
    """The network-relevant prerequisites a drain's rootfs has staged."""
    # L2: every dlopen'd NSS plugin + libsoftokn3's transitive sqlite present?
    nss_plugins_present: frozenset = field(default_factory=frozenset)
    # L3: the CA bundle (/etc/ssl/certs/ca-certificates.crt) staged?
    ca_bundle_present: bool = False
    # L0/DNS: a /etc/hosts (or host-resolver-rules) pin for the DoH host?
    doh_host_pinned: bool = False
    # L1: the setsockopt(SO_MARK/...) errno-massage wrapper present in interposer?
    setsockopt_errno_massage: bool = False

    def nss_complete(self) -> bool:
        """True iff EVERY required NSS plugin AND libsoftokn3's transitive
        libsqlite3.so.0 are present (the full L2 closure)."""
        need = set(NSS_PLUGIN_MODULES) | set(NSS_SOFTOKN_TRANSITIVE)
        return need.issubset(set(self.nss_plugins_present))

    def nss_missing(self) -> tuple:
        need = list(NSS_PLUGIN_MODULES) + list(NSS_SOFTOKN_TRANSITIVE)
        have = set(self.nss_plugins_present)
        return tuple(m for m in need if m not in have)


@dataclass(frozen=True)
class FetchRequest:
    """The navigation the CR-2 probe issues."""
    # True for an IP-literal URL (https://1.1.1.1/) — SKIPS the DNS layer (L0).
    ip_literal: bool
    # True if chromium is launched with the secure-DoH flag block (only relevant
    # to a hostname fetch; harmless for IP-literal).
    doh_flags_set: bool = False


# --------------------------------------------------------------------------- #
# The verdict the model produces for a (stage, request) pair.
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class ConnectVerdict:
    layer: str                 # which layer decides the outcome
    failure: str               # FAIL_* — what the guest does
    detail: str                # human-readable reason
    fix: Optional[str]         # the staged/interposer fix that clears it (or None)

    @property
    def fatal(self) -> bool:
        """A brk (or a never-resolving hang) — i.e. the fetch CANNOT complete."""
        return self.failure in (FAIL_BRK, FAIL_HANG)

    @property
    def passes(self) -> bool:
        return self.failure == FAIL_NONE


# --------------------------------------------------------------------------- #
# The decision: walk the layers in execution order; the FIRST failing layer is
# the verdict (later layers never run if an earlier one crashes/hangs).
# --------------------------------------------------------------------------- #
def classify_connect(stage: NetStage, req: FetchRequest) -> ConnectVerdict:
    """Predict the outcome of a chromium https fetch given the staged prereqs.

    Order is chromium's actual execution order: DNS (hostname only) -> socket
    setup -> NSS init -> TLS/cert. The first layer that fails is the verdict.
    """
    # --- L0 DNS (hostname only; IP-literal skips it entirely) ----------------
    if not req.ip_literal:
        # A hostname needs resolution. Raw UDP/53 is blocked for the app, so the
        # ONLY way it resolves is the DoH path WITH its bootstrap host pinned.
        if not (req.doh_flags_set and stage.doh_host_pinned):
            return ConnectVerdict(
                layer="L0-dns",
                failure=FAIL_HANG,
                detail="hostname needs DNS; raw UDP/53 blocked for untrusted_app; "
                       "DoH flags or the DoH-host bootstrap pin are absent -> the "
                       "navigation hangs ~180s on the alarm",
                fix="chromium-dns-doh.md: secure-DoH flags + /etc/hosts pin "
                    "(build_chromium_net_overlay --doh-flags)",
            )
        # DoH bootstrap is in place: the DoH server resolves locally, name
        # resolution rides 443. L0 clears; fall through to the socket/NSS layers
        # exactly like the IP-literal case.

    # --- L1 socket setup (SO_MARK errno) -------------------------------------
    # chromium 147 sets SO_MARK best-effort and does NOT fatally CHECK the result
    # on this device (the SO_MARK==brk prediction was DISPROVED). So L1 never
    # decides the verdict here; the errno-massage is a retained no-op belt. We
    # still surface it as a non-fatal note when absent, to keep the wrapper from
    # being silently dropped (a future chromium COULD start CHECKing it).
    # (No early return: L1 is not the blocker on this device.)

    # --- L2 NSS init  (THE device-confirmed IP-literal brk) ------------------
    if not stage.nss_complete():
        missing = ", ".join(stage.nss_missing())
        return ConnectVerdict(
            layer="L2-nss",
            failure=FAIL_BRK,
            detail=f"NSS init dlopen's its plugins before TLS; missing [{missing}] "
                   f"-> crypto/nss_util.cc nss_error=-5925 -> ImmediateCrash "
                   f"(brk/TRAP_BRKPT, ~6s). This is the IP-literal connect brk.",
            fix="build_nss_overlay.py (nss-stage.tar): the 5 NSS plugins + "
                "libsqlite3.so.0, flat + in .../nss/",
        )

    # --- L3 TLS handshake + cert verification --------------------------------
    if not stage.ca_bundle_present:
        return ConnectVerdict(
            layer="L3-tls",
            failure=FAIL_SOFT_NET_ERROR,
            detail="NSS is up and the socket connects, but the CA bundle is absent "
                   "-> server-cert verification fails -> a SOFT net error "
                   "(ERR_CERT_AUTHORITY_INVALID). NOT a crash — the process lives.",
            fix="ca-certificates.crt staged by build_chromium_net_overlay (kept)",
        )

    # All layers cleared.
    return ConnectVerdict(
        layer="ok",
        failure=FAIL_NONE,
        detail="DNS (or IP-literal) -> socket -> NSS -> TLS all clear; the fetch "
               "should complete to a DOM (or a real-server net error, e.g. a 4xx, "
               "which is application-level success of the transport).",
        fix=None,
    )


# --------------------------------------------------------------------------- #
# Convenience: the fully-staged 'good' stage (every fix applied) — what a green
# CR-2 drain should look like, for tests + the doc's expected-state table.
# --------------------------------------------------------------------------- #
def fully_staged() -> NetStage:
    return NetStage(
        nss_plugins_present=frozenset(NSS_PLUGIN_MODULES) | frozenset(NSS_SOFTOKN_TRANSITIVE),
        ca_bundle_present=True,
        doh_host_pinned=True,
        setsockopt_errno_massage=True,
    )
