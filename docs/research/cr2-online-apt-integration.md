# CR-2 online-apt — converged single device path (DoH + hosts-pin + connect-brk)

Status: HOST-converged design (2026-06-03), no device. ALR integration session.
Touches (docs only): this file. No 본체/stamp edit here — the body wiring it
specifies is the DEVICE-REQ at the end.

This document merges the three CR-2 online-apt host tracks into **one** path an
integration/device session can apply and drain without re-deriving the layering:

| track | commit | artifact | role in the converged path |
|---|---|---|---|
| (1) DoH `getaddrinfo` shim | `23e59b2` | `app/src/main/cpp/alr_doh/*` + `scripts/build-doh.sh` → `usr/lib/androlinux/libalr_doh.so` | **general** DNS for every glibc app over TCP/443 |
| (2) mirror-IP `/etc/hosts` pin | `b671e48` | `tools/build_apt_mirror_overlay.py` → `apt-mirror-stage.tar` | **authoritative** no-DNS pin of the apt mirror host + deb822 sources |
| (3) connect/TLS `brk` SSOT+fix | `baf2c51` | `docs/design/cr2-connect-brk.md` + `tools/build_nss_overlay.py` → `nss-stage.tar` | chromium-only TLS-init fix; **NOT on apt's path** (apt = GnuTLS) |

> **One-line verdict.** For the **apt** online proving target, the converged path
> is **`/etc/hosts` pin (DNS) → CA bundle (TLS) → signed `apt-get update`/`install`**.
> The DoH shim is the *general* DNS fallback (and the only DNS for non-pinned hosts
> / chromium-DoH-less glibc apps), the hosts-pin is *first* and authoritative for the
> mirror, and the connect-`brk` fix is **orthogonal to apt** (it unblocks chromium's
> NSS, which apt's GnuTLS never touches). All three compose with **zero conflict**
> once two body items land: append `libalr_doh.so` to the guest `LD_PRELOAD`, and
> merge the two overlays' `/etc/hosts` into one file (or order their extraction).

---

## (a) DNS: who is first — `/etc/hosts` pin, or DoH? — DECISION + rationale

**Decision: `/etc/hosts` pin is FIRST/authoritative; DoH is the general fallback.**
This is not a toggle we pick — it is exactly how the two pieces already compose,
and it is the *correct* ordering for both robustness and the apt target:

1. **They sit at different layers and already nest correctly.** The DoH shim
   interposes `getaddrinfo()` (`libalr_doh.c:426`). Its very first deferral clause
   sends a request to the real glibc resolver for: shim-disabled, no-node, numeric
   host, `AI_NUMERICHOST`, or a non-INET family (`libalr_doh.c:442`). The real
   glibc resolver consults nsswitch `hosts: files dns` → **`/etc/hosts` first**. So
   even with the shim loaded, a host that is pinned in `/etc/hosts` is resolved from
   the file by the kernel/glibc path the shim *defers to on its fallback*. **But** —
   the shim's hot path does NOT pre-consult `/etc/hosts`; for a *non-numeric* name
   it goes straight to DoH/443. That means for the apt mirror hostname we must make
   the **hosts-pin win before the shim even tries DoH**. Two mechanisms guarantee
   that, in order of preference:

   - **Primary (chosen): keep the mirror reachable WITHOUT the shim at all.** apt's
     own resolution of `ports.ubuntu.com` is a glibc `getaddrinfo`. With the
     `/etc/hosts` pin present and the **DoH shim NOT in apt's `LD_PRELOAD` for the
     `apt-get update` step** (see (b)), apt resolves the mirror straight from
     `/etc/hosts` — zero DNS, zero TLS-for-DNS, no shim round trip. This is the
     simplest, most robust apt path and matches track (2)'s host proof exactly
     (`--verify-fetch`: index+Packages.gz+pool .deb all reachable through the pin).

   - **Belt (when the shim IS loaded globally): the shim must honor `/etc/hosts`
     before DoH.** If a future build LD_PRELOADs `libalr_doh.so` for *all* guests
     (so apt inherits it), the shim would DoH-resolve `ports.ubuntu.com` over 443
     instead of reading the pin. That still *works* (DoH returns the real anycast
     IP, apt connects), but it (i) needlessly spends a 443 round trip per apt run,
     and (ii) defeats the offline guarantee of the pin. **Converged fix (small,
     deferred to the shim's owner): have the shim try the real resolver FIRST for
     names and only DoH on its `EAI_NONAME`/hang** — i.e. invert to "files dns, then
     DoH-as-last-resort". Until that lands, prefer the Primary mechanism (don't put
     the shim on apt's update step). Recorded as a RISK below, not a blocker.

2. **Why the pin is authoritative (not DoH) for the mirror specifically:** the pin
   is a *local file answer* — no network, no clock, no cert, no CDN dependency for
   resolution. apt's integrity is the **GPG-signed Release**, not TLS, so plain-HTTP
   through the pinned IP is safe (track (2) §"HTTP vs HTTPS"). DoH for the mirror
   would add a 443 dependency to a step that needs none. **The pin is strictly
   cheaper and strictly more offline for a fixed mirror.**

3. **Why DoH is still required (not dropped):** the pin only resolves the host(s) it
   lists. The moment the guest needs an **arbitrary** name — `apt-get install` that
   pulls from a redirect to a *different* host, `python -m pip`, `curl`/`wget`/`git`
   to any URL, a maintainer script that phones home — only DoH resolves it (full
   resolver over 443). DoH is the **general** capability; the pin is the **apt-shaped
   shortcut**. Track (1) and (2)'s own trade-off table says exactly this; we adopt it.

**Net (a):** order is **hosts-pin → (real resolver) → DoH**. For the apt drain the
pin alone closes DNS; DoH is loaded for the *general* glibc surface and as the
fallback for any non-pinned host. They never fight because the shim's fallback path
*is* the `/etc/hosts` path, and we keep the shim off the one step (apt update) where
a redundant DoH round trip would only cost time.

---

## (b) Combination order with the connect-`brk` fix

The connect-`brk` (track 3) is **chromium-NSS-specific** and **not on apt's path** —
this is FACT from `docs/design/cr2-connect-brk.md` §6: apt's TLS is **GnuTLS**
(linked via `DT_NEEDED`, no dlopen'd softoken to drop), so apt never hits the
`nss_util.cc` `brk`; it degrades to a *string* ("Could not resolve host" /
"Certificate verification failed"), never a crash. Therefore:

**For the apt online target, the only layers that matter are L0 (DNS) and L3 (CA):**

```
  L0 DNS   = /etc/hosts pin   (track 2)   ── REQUIRED, and sufficient for the mirror
  L3 TLS   = CA bundle        (common-data / chromium-net overlay ships
                               /etc/ssl/certs/ca-certificates.crt) ── only if --scheme https
  L2 NSS brk (track 3)        = NOT on apt's path (GnuTLS) ── no-op for apt
```

So the **combination order for the apt drain** is simply:

1. **DNS first** — stage `apt-mirror-stage.tar` (the `/etc/hosts` pin + deb822
   sources + apt.conf). This is the load-bearing step; without it `apt-get update`
   hangs on `getaddrinfo` before opening any socket.
2. **TLS only if you chose HTTPS** — ensure the CA bundle is staged
   (`/etc/ssl/certs/ca-certificates.crt`, shipped by the common-data and/or
   chromium-net overlay). With the **default HTTP scheme this is not needed** (signed
   Release guarantees integrity). Prefer HTTP for the first drain.
3. **connect-`brk` fix** — **irrelevant to apt; do not gate the apt drain on it.**
   `nss-stage.tar` belongs to the *chromium* CR-2 lane only. (Stage it when the same
   device session also drains chromium-over-network; it is inert for apt.)

**For the chromium-over-network target** (the other half of CR-2, not the apt focus
here) the order is the full four-layer stack from the SSOT: DoH flags + `/etc/hosts`
DoH-bootstrap pin (L0) → `nss-stage.tar` (L2, the `brk` fix) → CA bundle (L3) →
hostname-via-DoH target for the definitive PASS (the IP-literal correctly
cert-name-fails = transport success). That lane is already specified in
`docs/design/cr2-connect-brk.md` and `docs/design/chromium-dns-doh.md`; this doc does
not duplicate it beyond noting where it diverges from apt (it adds L2+DoH; apt needs
neither).

**One shared-file caveat that couples the two lanes — the `/etc/hosts` collision.**
Both `chromium-net-stage.tar` (pins `dns.google`→8.8.8.8 for chromium's DoH
bootstrap) and `apt-mirror-stage.tar` (pins `ports.ubuntu.com`→Cloudflare) ship a
**full `/etc/hosts`**. `extractOverlayTar` overwrites, so **whichever extracts last
wins and the other's pin is lost.** Converged fix (no code, build-time):

- Build the apt-mirror overlay with **both** pins merged into its one `/etc/hosts`
  using the `extra_hosts=` Python kwarg of `build_apt_mirror_overlay()`
  (`tools/build_apt_mirror_overlay.py:341` kwarg, `:215` emit — add
  `dns.google → 8.8.8.8,8.8.4.4`; **no CLI flag exists today** — the call form is in
  Step 0), and **extract it LAST** so its merged file is the final state. OR
- If only apt is being driven this session, the chromium-net overlay need not be
  staged at all, and the collision does not arise.

This is the single real interaction between the apt path and the chromium-brk path:
they share *one file* (`/etc/hosts`), nothing else.

---

## (c) Device test procedure (exact push/extract/env/drain)

> All host-side; the integration session pushes to a device. `am force-stop` FIRST
> every relaunch (auto-memory rule: onCreate overlays+probes are skipped otherwise).
> Build env: `JAVA_HOME=openjdk@17` or the build silently keeps a stale APK.

### Step 0 — build the two new artifacts (host)

```
# (1) DoH shim .so for the rootfs (zig cross-build → build/doh/libalr_doh.so)
ZIG=/path/to/zig scripts/build-doh.sh
# pack it INTO the rootfs tar (or a doh-stage.tar) at the guest-visible path:
#   ./usr/lib/androlinux/libalr_doh.so   (0755)

# (2) apt mirror-IP overlay, default mirror=ports (noble arm64), default scheme=http:
python3 -m tools.build_apt_mirror_overlay \
        --mirror ports --scheme http \
        --out /tmp/apt-mirror-stage.tar
#   (optional freshness: add --refresh-ips to re-pin live anycast IPs at build time)
#
# To ALSO carry the chromium DoH-bootstrap pin in the SAME /etc/hosts (avoids the
# (b) collision) there is currently NO CLI flag — `extra_hosts` is a Python-API kwarg
# of build_apt_mirror_overlay() (build_apt_mirror_overlay.py:332 def, :341 kwarg,
# :215 emit). Reach it with a short call (or, cleaner, the builder owner adds a
# `--extra-host H=ip[,ip]` flag — see (b)):
#   python3 - <<'PY'
#   from tools.build_apt_mirror_overlay import build_apt_mirror_overlay
#   build_apt_mirror_overlay("/tmp/apt-mirror-stage.tar", mirror="ports", scheme="http",
#                            extra_hosts=(("dns.google", ("8.8.8.8", "8.8.4.4")),))
#   PY
# (out_tar is the first positional; mirror/scheme/extra_hosts are keyword-only. The
#  plain `--out` CLI path above is the verified-green one and omits the extra pin.)
```

### Step 1 — push stage tars + arm markers

```
adb push /tmp/apt-mirror-stage.tar        /data/local/tmp/apt-mirror-stage.tar
# the apt machinery + admindir + dpkg/apt binaries + a populated status:
adb push /tmp/apt-dpkg-stage.tar          /data/local/tmp/apt-dpkg-stage.tar
adb push /tmp/dpkg-db-stage.tar           /data/local/tmp/dpkg-db-stage.tar
adb push /tmp/fakeroot-stage.tar          /data/local/tmp/fakeroot-stage.tar   # non-root dpkg
adb push /tmp/interpose-stage.tar         /data/local/tmp/interpose-stage.tar  # if rebuilt
# DoH .so: ships in the rootfs tar (Step 0) OR a doh-stage.tar pushed here.

# arm the ONLINE apt drain (marker content = "update" mode, see DEVICE-REQ wiring):
adb shell 'echo "online:hello" > /data/local/tmp/.alr-aptdrain-online'
```

Note: today's `launchAptDrainProbe` runs **offline `dpkg -i`** against a pre-staged
`.deb` — it does NOT run `apt-get update`. The online drain below is the new branch
the DEVICE-REQ wires (a sibling marker `.alr-aptdrain-online` so the existing offline
drain is untouched / strict no-regression).

### Step 2 — env the loader applies (per the LD_PRELOAD wiring, runtime_report.cpp ~L1678)

The JNI entry is a fixed 6-arg signature; the loader reads gates from the **host
(app) process env** via `::getenv` and decides the guest env itself. For the online
apt drain the loader/Activity must set, around the probe only (restore after):

| host env | value | why |
|---|---|---|
| `ALR_FAKEROOT` | `1` | apt/dpkg run as fake uid/gid 0 (chain `libalr_fakeroot.so` first) |
| `ALR_REEXEC_INPROC` | `1` | apt forks `http`/`gpgv`/`dpkg`/`tar`/`zstd` children → in-process re-map |
| `ALR_PCGATE` | `1` (default) | sockets stay un-traced/native (apt's TCP to the pinned IP is native) |
| `ALR_DOH` | `0` for the `apt-get update` sub-step | keep the shim OFF the mirror resolve (pin is authoritative; avoids a redundant 443 round trip) — see (a) Primary. Leave default-ON for general guests. |
| `ALR_DOH_DIAG` | `1` (optional) | one stderr line per DoH lookup when the shim IS used (general installs) |

Guest env the loader injects (already wired): `ALR_ROOTFS`, and
`LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_fakeroot.so:<rootfs>/usr/lib/androlinux/libalr_interpose.so`.
**DEVICE-REQ adds** `:<rootfs>/usr/lib/androlinux/libalr_doh.so` to that chain for
the *general* guest case (see DEVICE-REQ wiring item 1). For the apt-update sub-step
specifically, `ALR_DOH=0` neutralizes the shim regardless of whether it is on the
chain.

### Step 3 — in-guest drain (the exact sequence the online probe runs)

Each line is one `nativeAlrNativeLoaderProbe(...)` guest exec (argv `\n`-joined),
through the loader, with the env from Step 2:

1. **Extract overlays** (guarded `extractOverlayTar`, marker-keyed by tar size):
   `fakeroot` → `apt-dpkg` → `dpkg-db` (this ORDER: apt-dpkg lays the admindir
   scaffold incl. `var/lib/dpkg/updates/`, dpkg-db overwrites status last) →
   **`apt-mirror`** (lays `/etc/hosts` + `/etc/apt/sources.list.d/alr-ports.sources`
   + `/etc/apt/apt.conf.d/99alr-mirror-ip`). Wait (bounded) for
   `usr/bin/apt`, `usr/bin/dpkg`, `etc/apt/sources.list.d/alr-ports.sources`,
   `libalr_fakeroot.so`, and the interpose-stage marker (the .so settled, not
   mid-rewrite — the v2 concurrent-rewrite root cause).

2. **`apt-get update`** — the network DNS+fetch proof:
   ```
   /usr/bin/apt-get  update
   ```
   Expect in the report/stderr: `Get:/Hit: http://ports.ubuntu.com/ubuntu-ports
   noble InRelease`, then `Packages` for `main … arm64`, and `Reading package
   lists... Done`. **Success = it fetched `InRelease` + a `Packages` index with NO
   `Could not resolve host` and NO ~180 s `getaddrinfo` hang** (the pin answered).
   This is the headline CR-2-online assertion.

3. **`apt-get install` of a light package** — the pool fetch + install proof.
   Pick a tiny, dependency-light package so the download is small and the dpkg
   unpack is fast. `hello` (~25 KB, zero deps) is the canonical smoke; `tree` or
   `sl` are similar. Download-only first (isolates the network from the install), then
   the real install:
   ```
   /usr/bin/apt-get  install  -y  --download-only  hello
   /usr/bin/apt-get  install  -y  hello
   ```
   Expect: `Get: … pool/main/h/hello/hello_*_arm64.deb` (pool path through the pin),
   then `Unpacking hello (…)`, `Setting up hello (…)`. Success markers (mirror the
   offline drain's): `Unpacking hello` + `Setting up hello`, then
   ```
   /usr/bin/dpkg  --status  hello   →  "Status: install ok installed"
   ```
   and, if you want an exec proof, `/usr/bin/hello` printing `Hello, world!`.

4. **Drain capture** — write the full probe report to `filesDir/aptonline-report.txt`
   (the apt fetch/dpkg output lands in the MIDDLE of the stream that logcat
   truncates), then `run-as <pkg> cat files/aptonline-report.txt` /
   `adb pull`. Optionally `ALR_TEE_GUEST_STDOUT=1` for a live line-tee of a wedged
   fetch. Use a **≥360 s window** so a slow CDN fetch isn't cut off mid-`Packages`.

### Step 4 — read the verdict

| in `aptonline-report.txt` | meaning | layer |
|---|---|---|
| `InRelease`+`Packages` fetched, `apt-get update` exit 0 | **CR-2-online DNS PASS** | L0 closed by the pin |
| pool `.deb` `Get:` + `Setting up hello` + `install ok installed` | **CR-2-online install PASS** | L0+pool+dpkg |
| `Could not resolve host ports.ubuntu.com` | pin not staged / `/etc/hosts` overwritten by a later overlay | L0 — fix (b) collision |
| `Certificate verification failed` (only if `--scheme https`) | CA bundle missing/clock skew | L3 — stage CA or use HTTP |
| ~180 s hang in `getaddrinfo`, no socket | neither pin nor DoH resolved the host | L0 — pin absent AND shim off/disabled |

---

## (d) DEVICE-REQ `CR2-online-apt` — what the integration session executes

**Goal:** on a DNS-blocked Android device, a glibc guest runs `apt-get update`
against the pinned Ubuntu-ports mirror (no DNS) and `apt-get install hello` pulling
the `.deb` from the mirror pool over the network, ending in
`dpkg --status hello = install ok installed`.

**Body wiring this requires (the only non-doc work; WS-1/loader + Activity):**

1. **Loader — append the DoH shim to the guest `LD_PRELOAD`.**
   `runtime_report.cpp` ~L1678 currently builds
   `LD_PRELOAD=<rootfs>/…/libalr_fakeroot.so:<rootfs>/…/libalr_interpose.so`. Append
   `:<rootfs>/usr/lib/androlinux/libalr_doh.so` (absolute ROOTFS host path, R3 — same
   rule as the interpose .so) **after** the interposer, for the general guest case.
   Gate it so a guest that sets `ALR_DOH=0` (the apt-update sub-step) still gets a
   no-op shim. (FACT: the DoH commit `23e59b2` explicitly lists this as its DEVICE-REQ
   — "loader must append the .so to the guest LD_PRELOAD … after libalr_interpose.so";
   it is **not yet landed**.)

2. **Rootfs packer — stage `libalr_doh.so`.** Pack `build/doh/libalr_doh.so` into the
   rootfs tar at `./usr/lib/androlinux/libalr_doh.so` (0755), OR ship a
   `doh-stage.tar` and add `"doh"` to a stage-extract loop. (FACT: nothing stages it
   today — only `scripts/build-doh.sh` exists.)

3. **Activity — add the apt-mirror overlay to a stage-extract list and add an ONLINE
   apt drain branch.** Add `"apt-mirror"` to the aptdrain `stageNames` (it currently
   stages `fakeroot`,`apt-dpkg`,`dpkg-db`,+target). Add a sibling marker
   `.alr-aptdrain-online` whose presence runs the Step-3 sequence (`apt-get update`
   → `apt-get install -y hello` → `dpkg --status hello`) with `ALR_DOH=0` set around
   the `update` exec, leaving the existing offline `dpkg -i` drain
   (`.alr-aptdrain`) byte-identical (strict no-regression).

4. **(b) `/etc/hosts` merge** — build `apt-mirror-stage.tar` with the chromium
   DoH-bootstrap pin merged in (via the `extra_hosts=` Python kwarg shown in Step 0,
   or add a `--extra-host H=ip[,ip]` CLI flag to the builder — no flag exists today)
   and extract it LAST among the `/etc/hosts`-shipping overlays, so chromium's DoH
   bootstrap pin and the apt mirror pin coexist in one file.

**Pass criteria (device):**
- `apt-get update` fetches `InRelease`+`Packages` from `ports.ubuntu.com` via the
  pin, **no `Could not resolve host`, no ~180 s hang**.
- `apt-get install -y hello` fetches `pool/main/h/hello/hello_*_arm64.deb` and
  `dpkg --status hello` shows `install ok installed`.
- (general) a DoH-resolved fetch to a **non-pinned** host (e.g. `curl https://example.com`
  with the shim on, `ALR_DOH_DIAG=1` showing `ALR-DOH example.com ok A=…`) proves the
  general resolver, independent of the pin.

**Out of scope for this DEVICE-REQ:** the chromium-over-network `brk`/TLS lane
(`nss-stage.tar` + DoH flags + hostname target) — tracked separately by
`docs/design/cr2-connect-brk.md` (ALR-CR2-tls / ALR-CR2-doh). It shares only the
`/etc/hosts` file with this path (handled by item 4).

---

## Risks & fallbacks

- **R1 — `/etc/hosts` collision (chromium-net vs apt-mirror).** Both ship a full
  `/etc/hosts`; last-extracted wins. *Fallback:* merge via the `extra_hosts=` kwarg
  (item 4 / Step 0) and extract apt-mirror last; or don't stage chromium-net when only
  apt is driven.
  Severity: medium (silent — apt would `Could not resolve host` if its pin is clobbered).
- **R2 — DoH shim shadows the pin for the mirror.** If `libalr_doh.so` is on apt's
  `LD_PRELOAD` and `ALR_DOH` is left ON, apt DoH-resolves `ports.ubuntu.com` over 443
  instead of reading the pin (works, but spends a round trip and breaks the offline
  guarantee). *Fallback (chosen):* set `ALR_DOH=0` around the `apt-get update` exec
  (Step 2). *Durable fix (deferred to shim owner):* make the shim try the real
  resolver first and DoH only on its failure (invert to "files dns, then DoH"). Low
  severity (functional either way).
- **R3 — CDN IP drift.** The pinned anycast IPs can rotate within their published
  ranges. *Fallback:* `--refresh-ips` rebuild (one command, no code), or point at a
  static country mirror via `--mirror-host/--mirror-ip`. Two IPs are pinned per
  mirror as belt-and-suspenders. Low severity.
- **R4 — HTTPS cert/time on a skewed device clock** (only if `--scheme https`).
  *Fallback:* use the default **HTTP** scheme — the GPG-signed Release still
  guarantees integrity; the pin alone reaches the repo. Low severity (HTTP is default).
- **R5 — DoH shim can't load TLS** (`libssl.so.3` absent) → it self-disables to pure
  passthrough, and a non-pinned host then hangs on Android (no worse than baseline).
  *Fallback:* the noble base / common-data overlay ships OpenSSL + the CA bundle;
  verify `libssl.so.3` + `/etc/ssl/certs/ca-certificates.crt` are in the rootfs. For
  the apt path this is moot (the pin needs no shim). Low severity for apt.
- **R6 — DoH server reachability** (default `1.1.1.1:443` literal). If 443 egress to
  that IP is blocked, DoH fails and the shim falls back (hang for non-pinned hosts).
  *Fallback:* `ALR_DOH_SERVER=8.8.8.8 ALR_DOH_HOST=dns.google` (Google), or rely on
  the pin for the mirror. Low severity for apt (pin-only).
- **R7 — concurrent overlay re-write of `libalr_doh.so`/`libalr_interpose.so`** during
  a guest LD_PRELOAD (the v2 device-confirmed root cause: a broken preload is silently
  skipped → no mediation). *Fallback:* the existing aptdrain already waits on the
  interpose-stage marker; the online branch must wait on the DoH-stage marker too
  before the first guest exec. Medium severity (silent loss of resolution).

---

## Fact vs estimate (honesty ledger)

- **FACT:** apt's TLS is GnuTLS, NOT NSS; apt never hits the chromium `nss_util.cc`
  `brk` and degrades to strings (`docs/design/cr2-connect-brk.md` §6). So track (3)
  is orthogonal to the apt path.
- **FACT:** the DoH shim defers to the real glibc resolver (→ `/etc/hosts` first via
  nsswitch `files dns`) for numeric/`AI_NUMERICHOST`/non-INET/disabled, and on ANY
  DoH failure (`libalr_doh.c:442`, `:514-519`) — so it is never *worse* than baseline
  and composes under the pin.
- **FACT:** track (2)'s host proof — `InRelease`, arm64 `Packages.gz` (1.77 MB), and a
  real pool `.deb` reachable through the pinned Cloudflare IP via the hostname, HTTP
  and (cert-valid) HTTPS; raw-IP HTTPS correctly cert-fails (`apt-mirror-ip-no-dns.md`).
- **FACT:** neither the DoH `.so` staging nor its `LD_PRELOAD` append is landed yet
  (only `scripts/build-doh.sh` exists; `runtime_report.cpp` ~L1678 chains only
  fakeroot+interpose); the current `launchAptDrainProbe` is offline `dpkg -i`, not
  `apt-get update`. These are the DEVICE-REQ body items.
- **ESTIMATE:** the converged apt drain will PASS on device with HTTP + the pin alone
  (DNS closed by `/etc/hosts`, integrity by the signed Release); the actual `apt-get
  update` exit-0 and the pool fetch are device-only (host can't run apt on Darwin).
- **ESTIMATE:** `ALR_DOH=0` on the update sub-step is the simplest way to keep the pin
  authoritative; the durable "files dns then DoH" inversion in the shim is the cleaner
  long-term shape but is the shim owner's change, not required for this drain.

---

## (e) Two follow-on gaps — AUTHENTICATED `apt-get update` + MULTI-DEP install

The (a)–(d) path was device-proven with **DEMO-TRUST** (`--demo-trust`: `Trusted: yes`,
verification SKIPPED) for single-leaf packages (`tree`, `galculator`). Two gaps remained;
both are now closed host-side (this section = root cause + the exact device-test plan).

### (e.1) GAP 1 — AUTHENTICATED apt: "Unknown error executing apt-key" — ROOT CAUSE

**Symptom (device):** with the authenticated apt-mirror overlay (`Signed-By` keyring +
gpgv, the builder default), `apt-get update` failed
`W: … Unknown error executing apt-key` / `E: The repository is not signed`. The gpgv
*method* was reached (worker target=`…/methods/gpgv`).

**Root cause (host-verified from the device binaries + apt 2.7.14 source):**
1. The base is **Ubuntu noble**, whose apt is **2.7.14**. In that apt the gpgv *method*
   (`apt-pkg/contrib/gpgv.cc` `ExecGPGV`) does **NOT** run `gpgv` directly even when the
   source pins `Signed-By` — it **ALWAYS** `exec()`s `Dir::Bin::apt-key` (default
   `/usr/bin/apt-key`), passing `--keyring <Signed-By>`. *(Evidence: the device
   `usr/lib/apt/methods/gpgv` ELF's strings carry `Unknown error executing apt-key` +
   `Could not execute 'apt-key'`; the 2.7.14 source does `Args.push_back(aptkey)`
   unconditionally.)* The newer apt (`main`) DID drop this, but noble has not.
2. `apt-key verify --keyring X.gpg` (a shell script) then (a) resolves its verifier from
   `Apt::Key::gpgvcommand` or a **bare-name `gpgv` PATH** lookup and runs it, and
   (b) shells out to a handful of **coreutils** (`mktemp`+`chmod` for `create_gpg_home`,
   `touch` for `create_new_keyring`, `head` for the dearmor sniff, `rm`/`cat` for
   cleanup) for its temp gpg home.
3. The **slim base ships NONE of that machinery**: no `/usr/bin/gpgv` (only the apt
   *method* `usr/lib/apt/methods/gpgv`), and of coreutils only `/usr/bin/env` (no
   `mktemp`/`chmod`/`touch`/`head`…). *(Host-verified by listing `rootfs/tiny-rootfs.tar`.)*
   So apt-key was reached but immediately failed (at `mktemp`, or at the bare-name `gpgv`
   lookup) → the method reported "Unknown error executing apt-key" → "not signed".

   The prior overlay docstring's claim that authenticated mode "does NOT touch apt-key"
   was the misconception that left this open: for noble apt it ALWAYS touches apt-key.

**NOT a native exec-re-entry bug per se** — the chain is reached and runs; it failed on
**missing files + an unpinned PATH**, both fixable in tooling. (The residual re-entry
caveat is below.)

**Fix (tooling, host-green):**
- `tools/build_apt_dpkg_overlay.py` — extend `SELF_CONTAINED_BINS` so `--self-contained`
  stages the WHOLE apt-key happy path: `usr/bin/apt-key` + `usr/lib/apt/methods/gpgv`
  (both ride in the `apt` pkg), `usr/bin/gpgv`, `usr/bin/apt-config`, and the coreutils
  apt-key shells out to (`mktemp`/`chmod`/`touch`/`rm`/`cat`/`head`/`readlink`/`cut`/
  `sort`/`comm`/`cp`/`mv`/`base64`/`id`/`expr`). `coreutils` is already a DEFAULT_TARGET,
  so the closure ships them too — this is the belt-and-suspenders 0o755 guarantee.
  `gpg`/`gpgconf` are **deliberately NOT** pulled: a plain `.gpg` keyring needs neither
  (dearmor of a `.gpg` is a no-op, the merge is `cat`, cleanup's `gpgconf` is
  `command_available`-guarded — script-traced). New `APT_KEY_VERIFY_DEPS` + selftest
  assert the happy-path set stays present.
- `tools/build_apt_mirror_overlay.py` — in AUTHENTICATED mode the apt.conf now pins both
  `Dir::Bin::apt-key "/usr/bin/apt-key";` and `Apt::Key::gpgvcommand "/usr/bin/gpgv";`
  (absolute → the guest PATH is irrelevant). Emitted only when authenticated
  (`--demo-trust` skips verification, so the pins are omitted there).

**Device-test plan — authenticated `apt-get update`:**
1. Build BOTH overlays authenticated:
   ```sh
   python3 -m tools.build_apt_dpkg_overlay --out /tmp/apt-dpkg-stage.tar \
       --base rootfs/tiny-rootfs.tar --self-contained     # ships apt-key+gpgv+coreutils
   python3 -m tools.build_apt_mirror_overlay --out /tmp/apt-mirror-stage.tar  # authenticated default
   # confirm the InRelease is signed by the staged key (host, NETWORK):
   python3 -m tools.build_apt_mirror_overlay --verify-fetch    # expects signer F6EC…C93C
   ```
2. Push both stage tars to `/data/local/tmp/` (plus fakeroot/dpkg-db/interpose as in
   Step 1) and arm the online apt drain.
3. Drain `apt-get -o Acquire::ForceIPv4=true update` (NO `--demo-trust`,
   `AllowUnauthenticated` OFF).
4. **PASS:** `apt-get update` exits 0, fetches `InRelease`+`Packages`, and logcat shows
   the gpgv method + `apt-key succeeded` (or simply no `apt-key` error and no
   `repository is not signed`). `apt-key`'s temp-home is created (mktemp present) and
   `gpgv` verifies against `/usr/share/keyrings/ubuntu-archive-keyring.gpg`.
5. **Confirm the security boundary:** flip ONE byte of the staged keyring (or point
   `Signed-By` at a wrong key) and re-run — `apt-get update` MUST fail closed
   (`NO_PUBKEY` / `not signed`), proving verification is real, not bypassed.

**Residual (DOCUMENTED for the loader track, not patched in tooling):** authenticated
verification adds depth to the exec-re-entry tree — `apt → apt-key (dash) → gpgv`, and
`apt-key` itself forks `mktemp`/`chmod`/`head`/… children. That is a **deeper** fork/exec
chain than the `--demo-trust` path (which skips apt-key entirely). If, with both overlays
staged + the pins, `apt-get update` STILL fails — but now with a re-entry signature
(`GUEST EXEC FAIL`, a re-map storm, or a child SIGKILLed mid-verify) rather than
"Unknown error executing apt-key" — then the residual is the SAME re-entry-DEPTH edge the
multi-supervisor wall / `dpkg→dpkg-split` exit-71 edge exposes, and belongs to the
exec-re-entry/supervisor owner (`runtime_report.cpp`), NOT here. Diagnostic to attach:
`ALR_TEE_GUEST_STDOUT=1` + `ALR_INTERPOSE_DIAG=1`, and grep logcat for
`ALR-INPROC: worker target=…/apt-key` and `…/gpgv` (proves how deep the re-map got).

### (e.2) GAP 2 — MULTI-DEP install: configure the whole closure outside apt

**Gap:** apt's in-line nested unpack (`apt → dpkg → dpkg-split`) hits a re-entry-DEPTH edge
(exit 71), so the shared `AptInstaller` finishes via a top-level `dpkg` instead. That was
proven for a **single leaf** (`tree`/`galculator` = one `.deb`); a package whose deps are
not already in the base needs **several** `.deb`s unpacked+configured **in dependency
order**, which the single-`.deb` fallback did not cover.

**Fix (`app/src/main/java/dev/chanwoo/androlinux/runtime/AptInstaller.kt`):** after
`apt-get install` downloads the whole closure into `var/cache/apt/archives`, the completion
fallback now hands dpkg **every** archived `.deb` on ONE command line
(`dpkg -i deb1 deb2 …`) and then runs `dpkg --configure -a`. dpkg itself unpacks all first,
then configures in **topological dependency order** (it defers a `Setting up` until that
package's intra-closure `Depends` are configured); `--configure -a` flushes any deferred
configure. So a multi-dep app installs+configures end-to-end without relying on apt's
blocked in-line unpack. A single leaf is byte-identical to the old behavior (the set is
just one `.deb`). The fallback also logs any dep left unconfigured (honest partial-closure
visibility). The strategy is modeled+tested host-side in
`tools/apt_install_e2e_model.py` (`complete_closure_via_dpkg`,
`DEMO_MULTIDEP_CLOSURE`): topological configure order, single-leaf base case,
base-provided-dep no-block, and the cyclic-deps honest edge.

**Device-test plan — a multi-dep GUI app:**
1. Pick a small GTK/X11 app whose runtime libs are NOT all in the base — e.g. **`xcalc`**
   (pulls `libxaw7`, `libxmu6`, `libxt6`, `libxpm4` … several not-in-base X libs) or a
   themed GTK tool. Confirm the closure shape host-side first:
   ```sh
   python3 -m tools.build_apt_dpkg_overlay --list --target xcalc   # see the dep count
   ```
   (`galculator` stays the single-leaf control; `xcalc` is the multi-dep vehicle.)
2. Stage the overlays (authenticated or demo-trust — GAP 2 is orthogonal to GAP 1) and
   run the in-app install of the chosen app through `AptInstaller.install(...)`.
3. Watch logcat: `aptinstall: completing via top-level dpkg -i on N apt-downloaded .deb(s)`
   with `N > 1`, then `dpkg -i (set) … settingUp=true`, then
   `dpkg --configure -a (flush)`, then `aptinstall: full closure configured (… deps + target)`.
4. **PASS:** `dpkg --status <app>` = `Status: install ok installed`, every dep also
   `install ok installed` (the fallback logs any that are not), the app's binary is present
   under `usr/bin/`, and (bonus) it launches under the compositor.
5. **Residual (same caveat as GAP 1):** if the **leaf** `dpkg -i <set>` itself hits the
   re-entry-depth wall while unpacking many `.deb`s (a deeper fork tree than one `.deb`),
   that is again the exec-re-entry owner's edge — attach the same `ALR-INPROC: worker
   target=…/dpkg-split` diagnostic. The Kotlin strategy is correct; only the loader's
   re-entry depth could gate it.
