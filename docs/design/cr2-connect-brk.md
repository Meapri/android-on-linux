# CR-2 connect/TLS `brk` — definitive root-cause + fix design

Date: 2026-06-03 · HOST-ONLY diagnosis/design (no device) · ALR integration session
Touches (docs/tools only): this doc · `tools/cr2_connect_brk_model.py` ·
`tests/test_cr2_connect_brk_model.py`. No 본체/stamp edits.

> **Bottom line (FACT, device-confirmed):** the IP-literal `https://1.1.1.1/` ~6 s
> `brk`/CHECK is **chromium's NSS initialization failing because the dlopen'd NSS
> PKCS#11/crypto plugin modules were missing from the rootfs** — NOT `setsockopt(SO_MARK)`.
> Full stderr (`crypto/nss_util.cc … nss_error=-5925 … libsoftokn3.so: cannot open shared
> object file`) was captured (v161 report-to-file), root-caused (v162), and **already
> fixed** (v163, `tools/build_nss_overlay.py` → `nss-stage.tar`). The guest now runs
> **past NSS into the actual TLS connect**. This doc is the SSOT that supersedes the
> earlier SO_MARK *prediction* (`docs/research/chromium-connect-brk-android.md`), records
> what each network layer needs, and ships a host model/test so a regression turns red
> before any drain.

---

## 0. Why this doc exists (the brief was based on stale evidence)

The task brief points at `docs/evidence/2026-06-02-cr2-network-netlink-blocker.md`, which
ends with the connect/TLS `brk` **open** ("the specific CHECK is inside chromium's 5192-byte
stderr … needs better capture"). That was true at the moment it was written. Between then and
now, the CR-2 lane **closed that gap on device** in three commits the evidence doc predates:

| commit | what it established | the brk story |
|--------|---------------------|---------------|
| `b823a64` (v161) | full guest stderr → `filesDir/cr2-report.txt` + setsockopt(SO_MARK) errno-massage | SO_MARK *hypothesis* landed speculatively |
| `0522c3c` (v162) | CR-2 fast-path → reliable drain → **read the full 6953-byte stderr** | **brk REVEALED = `[FATAL:crypto/nss_util.cc:146] nss_error=-5925`** (NSS init, NOT SO_MARK) |
| `a20756a` (v163) | `tools/build_nss_overlay.py` → `nss-stage.tar` (5 NSS plugins + libsqlite3) | brk **GONE**; guest advances past NSS into the TLS connect (>150 s, window-cut) |

So the question the brief poses — *root-cause the connect/TLS brk* — is **answered and the
fix is landed**. This doc therefore does four things: (1) state the definitive diagnosis and
*explicitly retire* the SO_MARK prediction; (2) lay out the full https layer-stack so the
*next* failure (post-NSS) is pre-classified; (3) document the full-stderr capture method that
made it possible; (4) compare apt's (GnuTLS) network path to chromium's (NSS) so the apt
proving-target is analyzed too. The executable spec is `tools/cr2_connect_brk_model.py`.

---

## 1. The four-layer https stack (what an IP-literal fetch actually crosses)

chromium-headless-shell is a **generic-Linux glibc build** run on an Android kernel inside
ALR. A `https://<host|ip>/` navigation crosses four independent layers, **each of which fails
differently** — and the failure *mode* (crash vs hang vs soft error) is the diagnostic key:

```
  L0 DNS          hostname only · raw UDP/53 blocked for untrusted_app  → HANG (~180s alarm)
  L1 socket setup SO_MARK tag (CAP_NET_ADMIN) → EPERM                    → (NON-event on cr147)
  L2 NSS init  ★  dlopen libsoftokn3/freebl3/... (NOT DT_NEEDED) missing → BRK  (~6s)   ← THE crash
  L3 TLS + cert   verify server cert vs CA bundle; CA missing            → SOFT net::ERR_*
```

The device A/B that split DNS from connect used exactly this: a **hostname** URL HANGS ~180 s
(L0), an **IP-literal** URL does NOT hang but `brk`s in ~6 s (L2). Timing alone separates them
— a hang is a stalled alarm; a `brk` is a fast deliberate abort.

### Why `SIGTRAP`/`TRAP_BRKPT` ⇒ "chromium killed itself on a failed invariant"
On arm64, `si_code = TRAP_BRKPT` means the CPU executed a `brk #imm`. That is exactly how
`base::ImmediateCrash()` / `CHECK` / `LOG(FATAL)` / `PCHECK` abort (`base/immediate_crash.h`
emits `brk #0`). It is **not** a page fault, **not** a seccomp kill, **not** SIGILL. So the
~6 s IP-literal crash is chromium aborting *on purpose* because something returned a value it
treats as impossible. The question was only *which check*.

---

## 2. L2 = the brk (FACT). Root cause and the landed fix

**The captured stderr (device, v162, `filesDir/cr2-report.txt`):**
```
[…] crypto/nss_util.cc:256] Error initializing NSS with a persistent database
      (sql:/root/.local/share/pki/nssdb): libsoftokn3.so: cannot open shared
      object file: No such file or directory
[FATAL:crypto/nss_util.cc:146] nss_error=-5925, os_error=0
```
`nss_error = -5925` = `SEC_ERROR_LIBRARY_FAILURE` family / NSPR "cannot load" — NSS could not
bring up its softoken (the SQLite-backed cert/key DB). chromium `CHECK`s the NSS-init result
and `ImmediateCrash()`es → the `brk`.

**Why the libs were missing (the real mechanism):** `libnss3` does **not** link its crypto
plugins via `DT_NEEDED` — it **`dlopen()`s** `libsoftokn3.so` / `libfreebl3.so` /
`libnssckbi.so` / … at runtime. The rootfs staging closure (`deb_closure`, §5-E) follows
**`DT_NEEDED` only**, so it shipped `libnss3.so` + `libnssutil3.so` but **dropped every
dlopen'd plugin**. First TLS → `NSS_Init` → `dlopen("libsoftokn3.so")` → `ENOENT` → FATAL.
There is a second-order drop: `libsoftokn3` *does* have a `DT_NEEDED` on **`libsqlite3.so.0`**,
which lives in a *different* deb (`libsqlite3-0`) the closure also dropped — so even staging
the 5 plugins without sqlite still FATALs (`libsqlite3.so.0: cannot open shared object file`).

**The fix (LANDED, v163, `tools/build_nss_overlay.py` → `nss-stage.tar`):** fetch the noble
`libnss3` + `libsqlite3-0` debs and pack:
- `libsoftokn3.so`, `libfreebl3.so`, `libfreeblpriv3.so`, `libnssckbi.so`, `libnssdbm3.so`
- `libsqlite3.so.0` (the softoken transitive)

at **BOTH** `/usr/lib/aarch64-linux-gnu/` (flat — resolved by the loader's `LD_LIBRARY_PATH`
for a bare `dlopen("libsoftokn3.so")`) **and** `…/nss/` (the Debian/NSS compiled search path).
The MainActivity toolkit loop extracts `nss` (device: `extracted=10`). Device result:
**the nss_util brk is GONE; the guest runs past NSS into the actual TLS connect** (cut off by
the 150 s window — see §4 for what to look for next).

> This is the **general** lesson, not a chromium one-off: any glibc app that `dlopen()`s
> plugins by soname (NSS, GDK-pixbuf loaders, GIO TLS backends, GStreamer elements) is
> under-staged by a `DT_NEEDED`-only closure. `e79279c` already generalized this into
> `tools/build_plugin_overlay.py` (module **classes**: `nss` / `gdk-pixbuf` / `gio` / …).

---

## 3. L1 = SO_MARK: the headline PREDICTION that the device DISPROVED (retired)

`docs/research/chromium-connect-brk-android.md` (written host-only, pre-capture) ranked
`setsockopt(SO_MARK)` → EPERM → PCHECK as **#1 (HIGH/MED-HIGH)**, reasoning that a generic-Linux
build takes the non-Android `SocketTag::Apply` branch (`setsockopt(SO_MARK)`), `SO_MARK` needs
`CAP_NET_ADMIN` an app lacks → EPERM, and chromium `PCHECK`s it. The interposer wrapper landed
on that basis (`b823a64`, `libalr_interpose.c` `setsockopt()` returns 0 on EPERM/EACCES for
`SO_MARK`/`SO_BINDTODEVICE`/`SO_PRIORITY`).

**Device verdict: SO_MARK is NOT the brk.** The full stderr named `crypto/nss_util.cc`, not
any `net/socket/*`. The v162 commit states it plainly: *"The SO_MARK setsockopt workaround
(b823a64) is a harmless no-op but is not this blocker."* chromium 147's `SocketTag::Apply`
sets the mark **best-effort** and does not fatally `CHECK` the result for the default fetch.

**Disposition (ESTIMATE, low-risk):** **keep** the `setsockopt()` errno-massage as a no-op
belt. Rationale: it is a strict no-op wherever the option succeeds (cannot regress CR-1), it
costs nothing, and it guards against a future chromium/QUIC path that *does* `PCHECK` a
capability-gated option. But it must **never again be promoted to "the connect brk."** The
host model encodes this: `test_so_mark_is_not_the_brk` asserts that removing the massage does
NOT change an otherwise-complete IP-literal fetch from PASS.

---

## 4. L3 = the NEXT layer (post-NSS): what the cut-off >150 s drain is, and how to read it

After the NSS fix the guest "runs past NSS into the actual TLS connect (>150 s, cut off by the
window)" (v163). That window-cut is **not yet a verdict** — it could be any of:

| symptom (in `cr2-report.txt` / live tee) | layer | meaning | fix |
|---|---|---|---|
| clean `exit=0` + DOM (or a server 4xx/redirect body) | — | **CR-2 PASS** | none — transport works |
| `net::ERR_CERT_AUTHORITY_INVALID` / `_DATE_INVALID` | L3 cert | CA bundle gap or clock skew (IP-literal `1.1.1.1` cert is for the hostname → SNI/`ERR_CERT_COMMON_NAME_INVALID` is EXPECTED for a bare IP) | use a hostname-via-DoH target, or accept the cert error as transport-success |
| `net::ERR_CONNECTION_TIMED_OUT` after the alarm | L1/connect | the raw `connect()`/handshake never completed (network, or a residual socket-setup denial) | A/B the interposer socket wrappers; check SELinux egress |
| a NEW `brk` at a DIFFERENT pc | L2′/L1′ | a *second* fatal CHECK behind NSS (e.g. a QUIC/UDP `setsockopt` PCHECK) | the §3 IP-level `setsockopt` clause is pre-written in the research doc #4 |
| silent hang with the supervisor near-idle | chromium-internals | a post-NSS init wedge (futex/service-thread), like the CR-1 post-ICU note | longer window + the live tee; usually a missing data file, not a loader bug |

**Important nuance for the IP-literal probe specifically:** `https://1.1.1.1/`'s TLS cert is
issued for `cloudflare-dns.com` / `one.one.one.one`, **not** for the bare IP `1.1.1.1`. Once
NSS+CA are up, chromium will *correctly* reject the IP-literal with a cert-name error. That is
**transport success** (NSS, socket, connect, TLS handshake all worked — only the name check
failed, which is the right behavior). So the **definitive CR-2 PASS target is a hostname URL
resolved via DoH** (§5), where the cert name matches — *not* the IP-literal, which was only ever
a *diagnostic* to remove DNS from the equation. The host model returns `FAIL_NONE` (pass) for a
fully-staged IP-literal because it models the *transport* clearing; the cert-name mismatch is a
property of the chosen target, not of ALR, and is called out here so the drain isn't misread.

---

## 5. L0 = DNS (separate, already designed): DoH-over-443

Orthogonal to the brk, the **hostname** path (the real PASS target) needs DNS, and raw UDP/53
to a public nameserver is blocked for an untrusted_app → ~180 s hang. The fix is **fully
designed and host-built** in `docs/design/chromium-dns-doh.md` + `tools/build_chromium_net_overlay.py`:

- `--dns-over-https-mode=secure --dns-over-https-templates=https://dns.google/dns-query`
  (all resolution over TCP/443, never falls back to the blocked UDP-53).
- Bootstrap pin (break the chicken-and-egg of resolving the DoH host itself), in **two**
  places: chromium `--host-resolver-rules="MAP dns.google 8.8.8.8,…"` **and** an `/etc/hosts`
  entry (member #5 of the net overlay) — so the DoH server resolves with zero plaintext DNS.

DoH rides ordinary TCP/443 sockets, which are **un-traced** by the loader (PCGATE traces only
path-syscalls + execve) and **permitted** by SELinux for an app in group `AID_INET`/3003. **No
loader/interposer/seccomp change is needed for DNS.** This is purely a chromium-flag +
overlay-data change.

---

## 6. apt vs chromium — the TLS-stack difference (the apt proving-target)

The brief asks for an apt-based analysis because apt's network path is *simpler*. It is also
**structurally different from chromium's at exactly the layer that crashed**:

| layer | chromium-headless-shell | apt / `apt-get update` over https |
|---|---|---|
| who does DNS | chromium's **own** resolver (does NOT call glibc `getaddrinfo` for navigations) → needs **DoH** | apt's https method → libcurl/its own → **glibc `getaddrinfo`** (`/etc/resolv.conf`, nsswitch `files dns`) |
| TLS library | **NSS** (`libnss3` + dlopen'd `libsoftokn3`/`freebl3`/`nssckbi`) — **the L2 brk** | **GnuTLS** (`libgnutls`, pulled transitively by the apt closure) — **no NSS, no softoken** |
| CA trust | NSS builtin (`libnssckbi`) + `/etc/ssl/certs` | GnuTLS reads `/etc/ssl/certs/ca-certificates.crt` directly |
| failure if TLS lib under-staged | **`brk`** (CHECK on NSS init) | a clean apt **error string** ("Certificate verification failed" / "Could not resolve host"), **not** a crash |
| socket tagging | `setsockopt(SO_MARK)` best-effort | none (apt does not tag sockets) |

**Consequences for using apt as the proving target:**
- apt **does not hit the NSS brk** — its TLS is GnuTLS, which links cleanly via `DT_NEEDED`
  (no dlopen'd softoken plugin to drop). So the *L2 crash is chromium-specific*. ✔ FACT
  (the deb closure dependency model + `tools/build_apt_dpkg_overlay.py` comment: "…gnutls …
  are pulled transitively by the resolver").
- apt **does hit the L0 DNS wall** — `getaddrinfo("deb.debian.org")` → glibc → `/etc/resolv.conf`
  → raw UDP/53 → **blocked for the app** → "Could not resolve host." So apt's DNS is the SAME
  Android-sandbox blocker as chromium's, but apt has **no DoH** of its own. apt's DNS fix is
  therefore a *different* lane than chromium's DoH: it needs either (a) an `/etc/hosts` pin of
  the mirror host (works for a fixed mirror; ESTIMATE: simplest), or (b) an interposer
  `getaddrinfo`/`res_*` shim that routes name resolution over a DoH/TCP helper or the Android
  resolver (`android_res_nquery`) — a larger, glibc-API-coupled change. **(b) is the general
  fix; (a) unblocks an apt e2e for a pinned mirror today.** ESTIMATE.
- apt's failures are **soft strings, not `brk`s** — so apt is a *better* first network probe
  for the DNS/CA layers precisely because it degrades gracefully and prints a legible reason,
  whereas chromium aborts. **Recommendation (ESTIMATE):** add an `apt-get update`-over-https
  probe (mirror host pinned in `/etc/hosts`, CA bundle staged) as the cheap network smoke test;
  it isolates L0(DNS)+L3(CA) from chromium's L2(NSS) entirely.

> Net: the **connect/TLS brk is a chromium-NSS-staging bug**, not a generic ALR network wall.
> apt shares only the **DNS** layer with chromium, and even there degrades to a string, not a
> crash. The single most reusable network-layer fix is the DNS one (DoH for chromium; `/etc/hosts`
> pin or a `getaddrinfo` shim for glibc apps like apt).

---

## 7. Full-stderr capture — the method that made the diagnosis possible

The brk reason lived in the **middle** of chromium's 5–7 KiB stderr, which the Android log path
truncates head/tail. Two complementary, already-implemented capture paths surfaced it:

1. **Report-to-file (the one that read the NSS line).** The CR-2 probe in
   `app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt` writes the **full** report
   (including chromium's complete stderr) to **`filesDir/cr2-report.txt`** (`MainActivity.kt:168`),
   then `adb pull` / `run-as cat` it and `grep -i 'fatal\|check\|nss\|error='`. The loader fills
   the full stderr into the report string (`runtime_report.cpp:261-292`
   `stderr_text = read_all_from_fd(...)`); writing it to a file dodges the logcat truncation.
   *This is what captured `nss_util.cc … nss_error=-5925`.*
2. **Live line-tee (`ALR_TEE_GUEST_STDOUT=1`).** When set, the loader streams the guest's
   stdout/stderr + the trampoline diag pipe to logcat **line-by-line as they arrive**
   (`runtime_report.cpp:2140-2186`, tags `alr_cr_out` / `alr_cr_diag`), so you can watch a
   *wedged* guest's last lines in real time even if the report never returns (a hung guest
   never lets the probe return). Gated (chromium `--v=1` is very verbose) so normal runs are
   quiet; byte-identical buffering so the final report is unchanged.

**Which log gates the root cause:** look in `cr2-report.txt` (or the `alr_cr_out` tee) for the
**`[FATAL:...]` / `Check failed:` line and its `errno`/`nss_error`**. The *file:line* on the
FATAL line is the decider: `crypto/nss_util.cc` ⇒ L2 (this case); `net/socket/*` + "Operation
not permitted (1)" ⇒ would have been L1 (SO_MARK); a soft `net::ERR_*` (no `[FATAL]`) ⇒ L3,
not a crash at all.

For the **next** drain, also enable the live tee and use a ≥360 s window (the brief's note),
so the post-NSS verdict (§4) is actually reached instead of window-cut.

---

## 8. Fix design summary — where each change lives (본체 = device track)

Everything load-bearing for the brk is **already landed**; the remaining items are the
post-NSS verdict (drain) and the apt-DNS lane. None of the *brk* fixes need new interposer
code — they are overlay-data (`nss-stage.tar`) and a flag block (DoH).

| layer | fix | kind | location | status |
|---|---|---|---|---|
| L2 NSS (THE brk) | stage the 5 dlopen'd plugins + `libsqlite3.so.0` | overlay data | `tools/build_nss_overlay.py` → `nss-stage.tar` | **LANDED v163, device-confirmed brk-gone** |
| L1 SO_MARK | errno-massage on EPERM/EACCES for `SO_MARK`/`SO_BINDTODEVICE`/`SO_PRIORITY` | interposer | `app/src/main/cpp/alr_interpose/libalr_interpose.c` `setsockopt()` (~L1373) | **LANDED v161; KEEP as no-op belt** (not the brk) |
| L0 DNS | secure-DoH flags + 2-place bootstrap pin | chromium flags + overlay data | `tools/build_chromium_net_overlay.py` + `docs/design/chromium-dns-doh.md` | designed + host-built; **DEVICE-REQ: ALR-CR2-doh** |
| L3 cert | 146-cert CA bundle | overlay data | `build_chromium_net_overlay` (`/etc/ssl/certs/ca-certificates.crt`) | staged |
| capture | report-to-file + live tee | loader + MainActivity | `runtime_report.cpp` / `MainActivity.kt` | **LANDED** |

**If a post-NSS drain reveals a NEW interposer need** (e.g. a QUIC/UDP-socket `setsockopt`
PCHECK at §4 row "NEW brk at a different pc"), the location is the **same** `setsockopt()`
wrapper in `libalr_interpose.c`, extended with the IP-level clause already drafted in
`docs/research/chromium-connect-brk-android.md` §1 #4 (`IP_MTU_DISCOVER`/`IP_RECVTOS`/…). That
is a 본체 change → device track, out of scope for this host doc; flagged here as the only
plausible remaining interposer edit.

---

## 9. Host model + test (this doc's executable spec)

`tools/cr2_connect_brk_model.py` — pure, host-testable layer-decision model
(`classify_connect(NetStage, FetchRequest) → ConnectVerdict`). It encodes the §1 layer order,
the §2 NSS-complete closure (mirrors `build_nss_overlay.MODULES` + `SQLITE_SONAME`), the
fatal-vs-soft contract (§4), and the SO_MARK-is-not-the-brk fact (§3). DEVICE-REQ markers note
the effects it does not execute (the dlopen actually succeeding, the TLS bytes flowing).

`tests/test_cr2_connect_brk_model.py` — 16 tests, **all green**. Highlights:
- `test_ip_literal_brk_is_nss_when_plugins_missing` — the headline: pre-v163 (CA present, NSS
  absent) ⇒ `L2-nss` / `FAIL_BRK` / `nss_error=-5925`.
- `test_ip_literal_passes_once_nss_complete` — v163 fix ⇒ PASS.
- `test_missing_only_softokn_still_brks` / `…sqlite_transitive…` — partial staging still
  `brk`s (names `libsoftokn3.so` / `libsqlite3.so.0`, matching device evidence).
- `test_nss_brk_masks_missing_ca_bundle` — NSS (L2) crashes BEFORE the cert layer (L3) → the
  brk masks any CA gap (why the device saw a crash, not a cert error).
- `test_missing_ca_bundle_is_soft_not_brk` — once NSS is up, a CA gap is `FAIL_SOFT_NET_ERROR`,
  not fatal (the fatal-vs-soft contract).
- `test_so_mark_is_not_the_brk` — removing the SO_MARK massage does NOT brk a complete fetch
  (guards against re-promoting the disproved prediction).
- `test_model_plugin_set_matches_nss_overlay_builder` — the model's plugin list **must** equal
  the real builder's, so the prediction can't silently drift from what's staged.

Run:
```
uvx pytest -q tests/test_cr2_connect_brk_model.py     # 16 passed
uvx pytest -q tests/test_build_chromium_net_overlay.py # 24 passed (baseline, unchanged)
```

---

## 10. DEVICE-REQ (the only things this host pass cannot settle)

- **ALR-CR2-nss** *(largely done)* — confirm on a fresh drain that with `nss-stage.tar`
  extracted, the `crypto/nss_util.cc` brk is absent and the guest reaches the TLS connect.
  (v163 already device-confirmed "brk gone, past NSS"; re-confirm with the live tee on.)
- **ALR-CR2-tls** — drain past NSS with a **≥360 s window** and the **live tee on**; read the
  §4 verdict from `cr2-report.txt`. **Use a hostname-via-DoH target** for the definitive PASS
  (the IP-literal will correctly cert-name-fail — that is transport success, not a bug).
- **ALR-CR2-doh** — drain a hostname URL with the §5 DoH flag block + the `/etc/hosts` pin;
  expect no ~180 s hang (DNS rides 443).
- **ALR-APT-net** *(ESTIMATE, recommended)* — `apt-get update` over https against a mirror host
  pinned in `/etc/hosts`, CA bundle staged; expect a clean fetch or a *string* error, never a
  `brk` — isolating the DNS/CA layers from chromium's NSS.

---

## 11. Fact vs estimate (honesty ledger)

- **FACT (device-confirmed):** the IP-literal ~6 s brk = NSS init failure (`nss_util.cc`,
  `nss_error=-5925`, missing `libsoftokn3.so`); the fix = stage the dlopen'd NSS plugins +
  `libsqlite3.so.0` (`nss-stage.tar`, v163); the brk is gone and the guest advances past NSS.
- **FACT:** SO_MARK is NOT the brk (v162 full-stderr capture + commit text); the wrapper is a
  no-op belt. FACT: the apt TLS stack is GnuTLS (closure dependency), distinct from chromium's
  NSS; apt shares only the DNS layer and degrades to strings, not crashes.
- **FACT:** the full-stderr capture mechanism (report-to-file `cr2-report.txt` + live tee
  `ALR_TEE_GUEST_STDOUT`) exists and is what surfaced the NSS line.
- **ESTIMATE:** the post-NSS (>150 s window-cut) outcome is most likely a cert-name reject for
  the *IP-literal* (expected, not a bug) and a clean fetch for a *hostname-via-DoH* target; the
  exact verdict is the open DEVICE-REQ (ALR-CR2-tls). ESTIMATE: keeping the SO_MARK massage is
  harmless and worth retaining. ESTIMATE: apt's DNS is best unblocked first via an `/etc/hosts`
  mirror pin (simple), with a `getaddrinfo`/`res_*` DoH shim as the general fix.
- **ESTIMATE:** the only plausible *remaining* interposer edit for CR-2 is the pre-drafted
  IP-level `setsockopt` clause, and only if a post-NSS drain shows a new `brk` at a different pc.
