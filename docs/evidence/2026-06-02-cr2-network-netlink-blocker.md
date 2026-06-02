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

## Status
- CR-1 (engine + DOM render in-process): **ACHIEVED + reproduced 2/2** (v159).
- CR-2 (network fetch): NETLINK connectivity blocker **CLEARED** (interposer bind fix);
  chromium now issues the request; remaining = DNS/connect layer (isolation in flight).
