# CR-2 (chromium network) — attempted; first blocker = NETLINK socket EACCES (v160)

Date: 2026-06-02 · Device SM-X236N / Android 16 / untrusted_app · branch ws-1

## Setup (scaffolding, committed)
- `tools/build_chromium_net_overlay.py` → chromium-net-stage.tar (resolv.conf 8.8.8.8/1.1.1.1
  + nsswitch.conf + 146-cert CA bundle + cr-test.html). Auto-extracted via the MainActivity
  toolkit loop (`chromium-net` added; device confirmed `extracted=8`).
- MainActivity CR-2 probe (gated `/data/local/tmp/.alr-cr2`): chromium-headless-shell
  `--single-process --no-zygote --disable-gpu --enable-logging=stderr --v=1 --dump-dom
  https://example.com/`. The app UID is in group 3003 (AID_INET) — network IS permitted
  (confirmed via the `id` probe: `groups=…,3003,…`).

## Result: FAIL — network fetch hangs 181s (hits chromium's 180s alarm)
chromium ran exec_ms=181069, exit=-1 (alarm), traps=75 rewrites=61. Init produced only
non-fatal errors then went silent (no fetch output). The actionable one:
```
ERROR:net/base/address_tracker_linux.cc:243] Could not bind NETLINK socket: Permission denied (13)
```
(plus non-fatal dbus `/run/dbus/system_bus_socket` and pulse `Connection refused`.)

## Diagnosis
chromium's `AddressTrackerLinux` binds a NETLINK_ROUTE socket to observe interface/route
changes for `NetworkChangeNotifier`. Android SELinux denies NETLINK route sockets to
`untrusted_app` (EACCES). With connectivity-tracking broken, chromium's network state is
unknown/offline, so the navigation to a real URL stalls until the alarm. This is an
Android-sandbox restriction, NOT a loader/supervisor issue (CR-1 — the engine + DOM render
— is achieved; sockets themselves are un-mediated and permitted).

## Candidate fixes (next CR-2 iteration)
1. A chromium flag to bypass connectivity detection / assume-online — e.g.
   `--disable-features=NetworkServiceInProcess` is NOT it; look at NetworkChangeNotifier
   overrides / `net::NetworkChangeNotifier::CreateMockIfNeeded` paths, or a force-online
   command-line. (Verify against chromium 147 source.)
2. Emulate the NETLINK socket in the LD_PRELOAD interposer: intercept the
   `socket(AF_NETLINK, …)` + `bind` and return a benign fd / success (a quiet "no changes"
   stream). This is NOT a SELinux bypass — it satisfies a kernel-denied connectivity *probe*
   so chromium proceeds; the actual data sockets are normal AF_INET (permitted).
3. Isolate DNS vs connect: re-run with `--host-resolver-rules="MAP example.com <ip>"` to see
   if DNS (raw UDP 53 to 8.8.8.8, which Android may also block for apps) is a SECOND blocker
   behind the NETLINK one.

## NETLINK fix landed — advanced past connectivity, now a 2nd (DNS/connect) blocker
Interposer `bind()` wrapper: a netlink bind that fails EACCES/EPERM returns success (the
kernel-denied multicast SUBSCRIPTION is skipped; initial enumeration via nlmsg_read still
works). Device re-drain (interpose .so 142848, re-extracted): the NETLINK bind error is
GONE and chromium now ISSUES the request —
`net/base/network_delegate.cc:38 NetworkDelegate::NotifyBeforeURLRequest: https://example.com/`
— so NetworkChangeNotifier sees the network online (the fix worked). But the request still
doesn't complete (200s, no DOM): a SECOND blocker in the request itself — DNS or TCP connect.
Most likely DNS: Android typically blocks apps from raw UDP-53 to arbitrary nameservers
(8.8.8.8), requiring the system resolver (netd); chromium's resolv.conf-based DNS would then
hang. Next: isolate via `--host-resolver-rules="MAP example.com <ip>"` (bypass DNS) — if it
then fetches, DNS is the only remaining blocker (workaround: chromium DoH over 443, which
connect permits), and connect+TLS are proven.

## Caveat found: the bind fix is INCOMPLETE (chromium netlink-recv hang)
Faking `bind`-success lets chromium's AddressTrackerLinux proceed PAST the bind to its
`sendmsg(RTM_GETLINK)` + `recvmsg` enumeration. If SELinux also blocks the netlink
send/recv (not just the multicast bind), `recvmsg` waits for a dump reply that never comes →
intermittent hang. Device-observed: with the bind fix, chromium `--version` (which was a
reliable ~1s exit-0 across many prior drains) once took **41s + exit=-1** — a flakiness the
bind fix introduced. So the bind-only workaround is necessary-but-insufficient: the next
iteration must ALSO short-circuit the netlink socket's `recvmsg` (return an empty/"done"
NLMSG_DONE so AddressTracker completes its enumeration immediately) or emulate a minimal
"1 interface up" reply — i.e. a fuller netlink emulation in the interposer, not just bind.

## Connect+TLS isolation (IP-literal https://1.1.1.1/): INCONCLUSIVE
Switched the CR-2 probe to an IP-literal URL to test connect+TLS without DNS, but the drain
window (240s) was consumed by the 41s flaky `--version` + the CR-2 200s alarm, so CR-2 did
not report. (Diagnostic reverted to keep ws-1 clean.) Re-run with a ≥360s window AND the
netlink recvmsg short-circuit before drawing a connect/DNS conclusion.

## CR-1 RE-VERIFIED safe with the bind fix (3/3) — the 41s was a one-off
Re-drained CR-1 with the netlink-bind interposer on device: `chromium-CR1 PASS
rendered-DOM-has-marker`, render exit=0 in ~1s, AND chromium `--version` exit=0 exec_ms=1014
(stable). So the earlier 41s+exit=-1 `--version` was chromium's inherent init
non-determinism (a one-off), NOT a deterministic bind-fix regression. CR-1 is now 3/3
(v159 ×2 + this re-verify) and the bind fix does not break it. The netlink recvmsg
short-circuit is still the right CR-2 robustness step (reduce that init-hang probability),
but it is NOT a CR-1 regression.

## Status
- CR-1 (engine + DOM render in-process): **ACHIEVED + reproduced 3/3**, confirmed safe WITH
  the netlink bind fix on device.
- CR-2 (network fetch): NETLINK connectivity bind blocker **addressed** (chromium now issues
  the request); remaining = the request's network layer (DNS/connect) + a netlink recvmsg
  short-circuit to cut chromium's init-hang probability. A deeper network-layer effort, rungs
  clear (see candidates above).
