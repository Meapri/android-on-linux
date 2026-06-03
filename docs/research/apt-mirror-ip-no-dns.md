# apt without DNS — mirror-IP `/etc/hosts` pin (the DoH fallback for apt)

Status: HOST-VERIFIED (2026-06-03). Device `apt update` = integration/device gate.
Builder: `tools/build_apt_mirror_overlay.py` → `apt-mirror-stage.tar`.
Pairs with / complements: `tools/build_chromium_net_overlay.py` (DoH for chromium).

## Problem (recap)

On Android a non-root `untrusted_app` cannot send raw UDP/53 — Android funnels app
DNS through `netd`, and a glibc guest's own `sendto(:53)` just hangs
(`docs/evidence/2026-06-02-cr2-network-netlink-blocker.md`). chromium routes around
this with **DoH** (DNS-over-HTTPS on 443). **apt has no DoH**: it resolves its
mirror hostname through glibc `getaddrinfo` like any program, so on a DNS-blocked
device `apt update` stalls on name resolution before it ever opens a socket.

What *does* work on the device: **direct IP HTTP/HTTPS** (port 80/443 sockets are
un-mediated by the ALR loader — only path + execve syscalls are traced — and
`untrusted_app` has INTERNET). So if apt can be made to resolve its mirror host
*without DNS*, the rest "just works".

## Fix

Two plain rootfs files (plus a tiny apt.conf), shipped as a §5-E overlay:

1. **`/etc/hosts`** pins the mirror **hostname → stable CDN anycast IP(s)**.
   `nsswitch hosts: files …` (base default) makes glibc answer
   `getaddrinfo("ports.ubuntu.com")` from `/etc/hosts` first → **zero DNS**.
2. **`/etc/apt/sources.list.d/alr-<mirror>.sources`** (deb822, the noble default)
   names that **same hostname** — NOT a bare IP. Naming the hostname is what keeps
   HTTPS valid (SNI + cert are keyed on the hostname); over HTTP it's just the
   clean canonical URI.
3. **`/etc/apt/apt.conf.d/99alr-mirror-ip`** — `Acquire::Languages "none"` (skip
   Translation-* round-trips) + `Acquire::ForceIPv4 "true"` (the pins are IPv4
   anycast; never wait on a dead v6 route). **Signature checking stays ON.**

## Mirror choice & IP stability (the crux)

The ALR base is **Ubuntu noble arm64**, so its archive is **`ubuntu-ports`**, NOT
`archive.ubuntu.com` (which is amd64/i386 only and would 404 every arm64 path).

| mirror key | host | arch/suite | CDN | published anycast ranges the IPs live in | pinned IPs (snapshot) |
|---|---|---|---|---|---|
| `ports` (default) | `ports.ubuntu.com` | Ubuntu noble **arm64** (`ubuntu-ports`) | **Cloudflare** | `172.64.0.0/13`, `104.16.0.0/13` | `172.66.152.176`, `104.20.28.246` |
| `debian` | `deb.debian.org` | Debian bookworm **arm64** | **Fastly** | `146.75.0.0/17`, `151.101.0.0/16` | `146.75.50.132`, `151.101.0.204` |

- **FACT (host-verified 2026-06-03):** the snapshot IPs are members of the
  published CDN ranges above (verified by `ipaddress` membership against
  `cloudflare.com/ips-v4` and `api.fastly.com/public-ip-list`; also asserted in
  the builder selftest).
- **The durable guarantee is the RANGE, not the address.** A CDN edge IP can
  rotate within its anycast block. Two IPs are pinned per mirror (belt-and-
  suspenders; both anycast → either reaches the nearest edge).
- **Refreshable with no code change:** `--print-current-ips` shows the host's
  current addresses; `--refresh-ips` re-resolves and pins the fresh set at build
  time. A future drift is a one-command overlay rebuild.
- **ESTIMATE / caveat:** if you need *zero* IP-drift risk, point the sources at a
  country mirror that publishes a static IP and pin that via
  `--mirror-host`/`--mirror-ip`. (Most fast mirrors are themselves on a CDN, so the
  anycast-range pin is the pragmatic stable choice.)

## HTTP vs HTTPS — default is HTTP for the index path

Debian/Ubuntu apt does **not** need TLS for integrity: the `Release`/`InRelease`
is GPG-signed and every `Packages` index and `.deb` is checksum-pinned by that
signed Release. So:

- **HTTP (default, `--scheme http`)** — simpler & more robust offline: no cert, no
  clock-skew failure, no SNI subtlety. Integrity still guaranteed by apt's
  signature chain. The `/etc/hosts` pin alone makes it reach the repo.
- **HTTPS (`--scheme https`)** — works *because the URL still names the hostname*,
  so the `/etc/hosts`-pinned IP is contacted with the correct SNI and the cert
  validates. Strictly heavier than HTTP here; offered for transport-encryption
  requirements.
- **Why not raw-IP URLs?** A `https://<IP>/…` URL fails cert verification (the cert
  is for the hostname). The hostname-in-URL + `/etc/hosts` pin is what gives you
  *both* no-DNS *and* a valid cert. (host-proven below.)

## Host evidence (2026-06-03)

`curl --resolve` and the builder's own `--verify-fetch` (urllib forcing the
connection to the pinned IP while sending Host + TLS SNI = hostname — *exactly*
what `/etc/hosts` does for apt) both confirm:

```
# index reachable through the pin, HTTP and HTTPS, both Cloudflare IPs:
$ python3 -m tools.build_apt_mirror_overlay --mirror ports --scheme http  --verify-fetch
  [OK] 172.66.152.176 -> HTTP 200, 66244+ bytes  (…/dists/noble/InRelease)
  [OK] 104.20.28.246  -> HTTP 200, 66244+ bytes  (…/dists/noble/InRelease)
$ python3 -m tools.build_apt_mirror_overlay --mirror ports --scheme https --verify-fetch
  [OK] 172.66.152.176 -> HTTP 200, 68057+ bytes  (https …/dists/noble/InRelease)   # cert OK via SNI
  [OK] 104.20.28.246  -> HTTP 200, 68057+ bytes  (https …/dists/noble/InRelease)

# raw curl cross-checks:
curl --resolve ports.ubuntu.com:80:172.66.152.176  http://…/InRelease   -> 200
curl --resolve ports.ubuntu.com:443:172.66.152.176 https://…/InRelease  -> 200, ssl_verify=0
curl                              https://172.66.152.176/…/InRelease     -> TLS handshake FAIL  # raw-IP cert mismatch
# full apt path (not just the signed index):
http://ports.ubuntu.com/ubuntu-ports/dists/noble/main/binary-arm64/Packages.gz  -> 200, 1.77 MB
http://ports.ubuntu.com/ubuntu-ports/pool/main/h/hello/hello_2.10-3build1_arm64.deb -> 200, 25 KB
```

So apt's **signed index, the arm64 `Packages.gz`, and a real `pool/` `.deb`** are
all reachable through the pinned IP via the hostname — over plain HTTP, no DNS.

## DoH vs mirror-IP — trade-offs

| | DoH (chromium-net overlay) | mirror-IP `/etc/hosts` (this overlay) |
|---|---|---|
| Resolves **arbitrary** hostnames | Yes (full resolver over 443) | No — only the hostname(s) pinned |
| Needs app DoH support | Yes (chromium has it; **apt does not**) | No — works for any glibc program |
| DNS transport | HTTPS/443 (encrypted) | none (answered from a local file) |
| Freshness if mirror IP rotates | self-heals (DoH re-resolves) | needs `--refresh-ips` rebuild |
| Best for | the browser, dynamic hosts | **apt / fixed-set tooling** (this) |

They are complementary: DoH is the general path; the mirror-IP pin is the apt-shaped
fallback (and a belt-and-suspenders pin even when DoH is up). Both ship the same
`hosts: files …` ordering the base already provides.

## ALR loader interaction

Same as chromium-net: the loader's seccomp filters trace only **path** + **execve**
syscalls; `socket`/`connect`/`sendto` default to RET_ALLOW, so apt's TCP to the
pinned IP is un-traced/native. The only files the path filter rewrites into the
rootfs are the ones this overlay ships (`/etc/hosts`, the `.sources`, the apt.conf)
plus `/etc/nsswitch.conf` (base default; chromium-net also ships one). So the whole
"apt can't reach the repo" gap is name resolution — closed here with zero DNS.

## DEVICE-REQ (integration / device gate)

1. Stage `apt-mirror-stage.tar` onto the base rootfs (same extractor path as the
   other §5-E overlays).
2. Ensure `/etc/nsswitch.conf` orders `hosts: files …` (base default; the
   chromium-net overlay already ships one — don't let a later overlay drop it).
3. In-app: `am force-stop` first (per the device-test rule), then run
   `apt-get update` and confirm it fetches `InRelease` + `Packages` through the
   pin (no DNS). A follow-up `apt-get install --download-only <pkg>` confirms the
   `pool/` `.deb` path. **HONEST:** host cannot run apt (Darwin, no apt/docker), so
   the actual `apt update` exit-0 is device-only; the host proof is the
   index/pool reachability above + the resolution-behavior reproduction in
   `--verify-fetch`.
4. If the device clock is wrong and you chose `--scheme https`, expect cert-time
   failures — prefer the default HTTP (signed Release still guarantees integrity).
