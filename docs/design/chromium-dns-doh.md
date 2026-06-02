# CR-2 DNS layer — DNS-over-HTTPS (DoH) so chromium can resolve hostnames on Android

Status: DESIGN + host-built overlay (branch `auto/cr2-doh`). DEVICE-PENDING — WS-1
wires the chromium flags into the MainActivity CR-2 probe and drains.
Owner of this lane: the CR-2-DoH worker (host-only). Loader/interposer/MainActivity
edits are WS-1's to make from the handoff in "WS-1 action" below.

## Problem (device-observed)

CR-1 (in-process headless render of a local page) is ACHIEVED + 3/3. CR-2 (load a
real `https://` URL) was isolated into two distinct blockers in
`docs/evidence/2026-06-02-cr2-network-netlink-blocker.md`:

1. **DNS** — a hostname URL (`https://example.com/`) hangs ~180–200s until
   chromium's navigation alarm fires. Root cause: a non-root `untrusted_app` is
   not permitted to send raw **UDP/53** to an arbitrary nameserver (8.8.8.8).
   Android funnels app DNS through the system resolver (`netd`); a glibc guest
   doing its own `sendto(:53)` just hangs. The `resolv.conf` + `nsswitch.conf`
   the overlay already ships are necessary but **not sufficient** on Android.
2. **connect/TLS** — even an IP-literal URL (`https://1.1.1.1/`, no DNS) runs
   ~6s then crashes at a `brk`/CHECK (SIGTRAP `TRAP_BRKPT`). A SECOND, distinct
   blocker. **Out of scope for this doc** (this lane is the DNS layer only); it
   needs full-stderr capture to root-cause.

This doc solves **(1) DNS**. The transport an `untrusted_app` *can* use is HTTPS
on **port 443** (chromium already opens TCP/443 fine — sockets are un-mediated by
the loader and permitted by SELinux for an app in group `AID_INET`/3003). So the
DNS layer must ride 443: **DNS-over-HTTPS (DoH)**.

## Why not the alternatives

- **Raw UDP-53 (status quo)** — blocked for apps; hangs. This is the bug.
- **Bind the Android system resolver (`netd`) / `getaddrinfo` via the NDK** — would
  require routing the guest's DNS into Android's resolver, i.e. an interposer that
  re-implements `res_*`/`getaddrinfo` against `android_res_nquery`. Much larger,
  Android-API-coupled, and chromium does its *own* resolution (it does not call
  glibc `getaddrinfo` for navigations), so a libc shim wouldn't even cover the hot
  path. Rejected for CR-2.
- **A localhost DNS proxy (DoH client → UDP-53 on 127.0.0.1)** — extra moving part,
  another process/thread under the ptrace supervisor. chromium has DoH built in;
  use it. Rejected.
- **chromium DoH (chosen)** — first-class, no extra process, all DNS over 443.

## Design — chromium secure-DoH + a two-layer bootstrap pin

### 1. Flags (the DNS transport)

    --dns-over-https-mode=secure
    --dns-over-https-templates=https://dns.google/dns-query

`secure` mode sends **all** name resolution over the DoH template (TCP 443) and
**never falls back** to plaintext UDP-53 — exactly right, since UDP-53 is the
blocked transport. (`automatic` mode would probe-then-fall-back to UDP-53 and
re-hang; do NOT use `automatic` here.)

### 2. The bootstrap problem and its fix

DoH is chicken-and-egg: to open the HTTPS connection to `dns.google`, chromium
must first resolve `dns.google` — but plaintext DNS is blocked. Break the cycle by
pinning the DoH server hostname to its well-known anycast IP, in **two** places
(belt-and-suspenders) so no plaintext lookup is ever attempted:

- **chromium-level** — `--host-resolver-rules="MAP dns.google 8.8.8.8,MAP dns.google 8.8.4.4"`.
  chromium consults host-resolver-rules *before* any resolver (including before
  DoH), so the template host resolves locally and the rule does NOT recurse into
  DoH. This is the canonical chromium DoH-bootstrap pattern.
- **glibc-level** — an `/etc/hosts` entry mapping the same host→IP, so any libc
  `getaddrinfo` chromium issues for the DoH host is also satisfied offline.
  `nsswitch.conf` already orders `hosts: files dns` → files (i.e. /etc/hosts) win.

The overlay supplies the **/etc/hosts** pin (member #5, new). The **flags** go on
chromium's command line — emitted verbatim by the builder so WS-1 pastes them in.

### 3. Providers

`tools/build_chromium_net_overlay.py` ships a `DOH_PROVIDERS` table; default
`google`. Each entry = `host`, RFC 8484 `template`, and stable anycast `bootstrap`
IPs (the same addresses chromium ships internally for these providers):

| provider     | DoH host             | template                                  | bootstrap IPs            |
|--------------|----------------------|-------------------------------------------|--------------------------|
| `google`     | `dns.google`         | `https://dns.google/dns-query`            | 8.8.8.8, 8.8.4.4         |
| `cloudflare` | `cloudflare-dns.com` | `https://cloudflare-dns.com/dns-query`    | 1.1.1.1, 1.0.0.1         |
| `quad9`      | `dns.quad9.net`      | `https://dns.quad9.net/dns-query`         | 9.9.9.9, 149.112.112.112 |

Selecting `--doh-provider cloudflare` re-pins `/etc/hosts` to that provider AND
changes the emitted flag block — the two stay in lockstep (host test enforces it).

## What the overlay now ships (chromium-net-stage.tar, 5 files)

1. `/etc/resolv.conf` — `nameserver 8.8.8.8` / `1.1.1.1` (kept; harmless, and a
   non-DoH consumer of the rootfs still has an upstream).
2. `/etc/nsswitch.conf` — `hosts: files dns` (kept; "files" makes the /etc/hosts
   pin authoritative).
3. **`/etc/hosts`** (NEW) — loopback + DoH bootstrap pin(s) for the chosen provider.
4. `/etc/ssl/certs/ca-certificates.crt` — Mozilla CA bundle (kept; TLS roots, also
   needed for the DoH server's own cert).
5. `/root/cr-test.html` — CR-1 local page (kept).

All `./`-rooted, mtime=0, `stage_tar_spec` conformant (verified for every provider).

## WS-1 action — wire the DoH flags into the CR-2 probe

The CR-2 probe in `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt`
(gated on `/data/local/tmp/.alr-cr2`) currently runs:

    /usr/lib/chromium/chromium-headless-shell
    --no-sandbox --single-process --no-zygote --disable-gpu --disable-dev-shm-usage
    --user-data-dir=/tmp/cr2-profile --no-first-run --no-default-browser-check
    --disable-crash-reporter --enable-logging=stderr --v=1 --dump-dom
    https://example.com/

Add these THREE args (between the existing flags and the URL) — get them verbatim
from `python3 -m tools.build_chromium_net_overlay --doh-flags` (default google):

    --dns-over-https-mode=secure
    --dns-over-https-templates=https://dns.google/dns-query
    --host-resolver-rules=MAP dns.google 8.8.8.8,MAP dns.google 8.8.4.4

i.e. the `\n`-joined arg string gains:

    "\n--dns-over-https-mode=secure" +
    "\n--dns-over-https-templates=https://dns.google/dns-query" +
    "\n--host-resolver-rules=MAP dns.google 8.8.8.8,MAP dns.google 8.8.4.4" +

Keep the target `https://example.com/` (a hostname URL — proves DNS now works).
The overlay's new `/etc/hosts` pin is staged automatically (the toolkit loop
already extracts `chromium-net`). No loader/interposer/seccomp change is required:
DoH rides ordinary TCP/443 sockets, which are already un-traced and permitted.

Expected result if DNS was the only remaining hostname blocker: the navigation no
longer hangs ~180s; chromium resolves `example.com` via DoH/443 and proceeds to
connect/TLS (where the *separate* `brk`/CHECK from blocker #2 may still bite — that
is the next, distinct lane, needing full-stderr capture).

## Honest scope / limits

- HOST-ONLY. The overlay is built + validated host-side (`--selftest`, `--list`,
  pytest). The on-device "hostname resolves via DoH" result is the WS-1 device gate.
- This addresses **DNS only**. The IP-literal `brk`/CHECK (blocker #2) is NOT
  touched here; if it also fires on the hostname path post-DNS, CR-2 still won't
  fully complete until that is root-caused (full guest stderr to a file).
- `secure` DoH means: if the DoH server itself is unreachable (e.g. 443 also
  blocked on some network), there is intentionally NO UDP-53 fallback — resolution
  fails fast rather than hanging. That is the correct trade for this sandbox.
- The bootstrap IPs are public anycast and stable, but are config, not code; if a
  provider ever renumbers, bump `DOH_PROVIDERS` and rebuild the overlay.

## Host gate

    python3 -m tools.build_chromium_net_overlay --selftest      # 37 checks PASS
    python3 -m tools.build_chromium_net_overlay --list           # shows DoH plan + members
    python3 -m tools.build_chromium_net_overlay --doh-flags       # the WS-1 flag block
    uvx pytest -q tests/test_build_chromium_net_overlay.py        # 24 passed
