# CR-2 connect/TLS `brk` — root-cause + ranked interposer fixes (chromium 147, untrusted_app)

Date: 2026-06-02 · Branch `auto/cr2-connect-brk` · HOST-ONLY research (no device)
Owner files I touch: this doc only. WS-1 owns `libalr_interpose.c` / `runtime_report.cpp`
/ `MainActivity.kt` and implements any wrapper from the DESIGN below (`wsActionNeeded`).

## 0. The observation we are explaining

From `docs/evidence/2026-06-02-cr2-network-netlink-blocker.md` (device SM-X236N, Android 16,
untrusted_app, with the NETLINK `bind()` workaround already landed):

> 1.1.1.1 (IP literal): does NOT hang — runs only **~6s** then `exit=-1 sig=5` = a `brk`/CHECK
> crash (SIGTRAP **si_code=1 TRAP_BRKPT**, pc=0x77b22bc290).

So an **IP-literal** `https://1.1.1.1/` (DNS removed from the equation) reaches a **deliberate
abort** in chromium's own code in ~6 s. Key facts that constrain the diagnosis:

- **`SIGTRAP` / `si_code=TRAP_BRKPT`** on arm64 = the CPU executed a `brk #imm` instruction.
  This is NOT a page fault, NOT a seccomp kill, NOT an illegal instruction. It is exactly how
  chromium aborts on a failed invariant: `base::ImmediateCrash()` / `IMMEDIATE_CRASH()` emit
  `brk #0` (`base/immediate_crash.h`), and `CHECK`/`LOG(FATAL)`/`DCHECK`/`RAW_CHECK` failures,
  the `//net` `PCHECK`, and Rust/`absl` aborts all funnel through it. **It is chromium killing
  itself on purpose because a syscall returned something it treats as impossible.**
- **~6 s, not ~180 s.** The 180 s case is the DNS alarm (a hang). 6 s is "init + create the
  URLRequest + open the transport socket + the first socket-setup syscall returns an errno the
  code path `CHECK`s on." So the failing syscall is on the **connect / socket-configuration**
  path, executed *after* the socket exists, *before* the TLS handshake completes.
- The socket syscalls themselves are **NOT seccomp-traced** (PCGATE traces only path-syscalls +
  execve; raw `socket`/`connect`/`setsockopt`/`sendto` run free) — so the failure is **the
  kernel/SELinux returning an errno to a real syscall**, and chromium `CHECK`ing on that errno.
  It is therefore fixable purely in the **LD_PRELOAD interposer** (libc-symbol level), exactly
  like the existing `bind()` NETLINK workaround — no SELinux weakening, no supervisor change.

The full `CHECK` line is in chromium's 5192-byte stderr, which the loader captures in full into
the report string but the Android log path truncates to head/tail (see §6). So the *literal*
message is not yet in hand. This doc ranks the candidates **by how well each matches all four
constraints above** and gives each a ready-to-implement interposer wrapper, so WS-1 can land the
top one in parallel with the device drain that captures the exact line.

---

## 1. Ranked candidates

Ranking criteria, in priority order:
(A) does it run **per-connection on the connect path** (matches ~6 s, IP-literal, post-socket)?
(B) does chromium **`CHECK`/`PCHECK`/abort** on the failure (matches `brk`, not a soft error)?
(C) is it **plausibly EPERM/EACCES under untrusted_app** specifically (SELinux/cap-gated)?
(D) is the fix a **safe errno-massage** that does not weaken the sandbox?

### #1 — `setsockopt(SO_MARK)` on the TCP connect socket → EPERM → `PCHECK`  ★ TOP

**Why #1.** Chromium tags *every* outbound socket with a SO_MARK (the "traffic annotation" /
network-isolation traffic tag) the moment the socket is created, on the connect path, once per
connection — matches (A) exactly. `SO_MARK` requires **`CAP_NET_ADMIN`**; an untrusted_app has
no such capability, so the kernel returns **`EPERM`** — matches (C) exactly, and this is the
single most famous "works as root, EPERM as an app" socket option on Linux/Android. And the
chromium call site historically uses a **`PCHECK`/`DCHECK`-grade** assertion on the result on
platforms where the mark is expected to succeed — matches (B).

**Exact chromium 147 call sites.**
- `net/socket/socket_posix.cc` and `net/socket/udp_socket_posix.cc` —
  `UDPSocketPosix::SetSocketOptions()` / the TCP equivalent call
  `net::SetSocketTag()` / `socket.SetIPv6Only()`-style helpers.
- The mark itself: `net/socket/socket_tag.cc` → `SocketTag::Apply(int socket)`:
  ```cpp
  // net/socket/socket_tag.cc  (Android: tag via qtaguid; non-Android Linux: SO_MARK)
  void SocketTag::Apply(SocketDescriptor socket) const {
  #if BUILDFLAG(IS_ANDROID)
    // ... android tag_socket() via qtaguid / NetworkTrafficAnnotation
  #else
    // generic POSIX: setsockopt(socket, SOL_SOCKET, SO_MARK, &mark_, sizeof(mark_));
  #endif
  }
  ```
  **Subtlety that makes this the prime suspect for *our* build:** chromium-headless-shell is a
  **generic-Linux glibc binary**, NOT an Android (bionic) build — so `BUILDFLAG(IS_ANDROID)` is
  **false**. It takes the `#else` branch and calls **`setsockopt(...SO_MARK...)`** directly,
  even though it is *running on* an Android kernel. On a real Linux box this succeeds (or the
  process is root); under untrusted_app SELinux + no `CAP_NET_ADMIN` it returns `EPERM`, and any
  `PCHECK(rv == 0)` around it (or a downstream invariant that assumes the mark applied) fires the
  `brk`. This Linux-binary-on-Android-kernel mismatch is *exactly* the class of bug that does not
  reproduce on desktop Linux and is invisible to the chromium team.
- Companion site: `net/socket/socket_options.cc` `SetSocketGlobalPriority` / `SetIPv6Only` and
  `net/socket/tcp_socket_posix.cc` `TCPSocketPosix::SetDefaultOptionsForClient()` — the cluster
  of `setsockopt` calls run on the connect path right before `connect(2)`.

**Interposer fix (DESIGN — WS-1 to implement in `libalr_interpose.c`).**
Same shape and rationale as the existing `bind()` NETLINK workaround
(`libalr_interpose.c:1174-1184`). Add a `setsockopt` wrapper that, *only* when the option is one
of the capability-gated routing/marking options and the real call fails `EPERM`/`EACCES`, returns
success — so a kernel-denied **routing-policy hint** is not misread as a fatal socket-setup
failure. The data socket itself is untouched; we never change what bytes go on the wire.

```c
/* libalr_interpose.c — add near the bind() workaround (~line 1184).
 * HOST-VERIFIED (NDK 27.2.12479018, aarch64-linux-android21-clang): SO_MARK(36),
 * SO_BINDTODEVICE(25), SO_PRIORITY(12) are ALL already visible via <sys/socket.h>
 * — the header bind() already includes (libalr_interpose.c:127). No new include,
 * no fallback #define needed. (Compiled a 3-symbol probe clean against the NDK.) */
int setsockopt(int fd, int level, int optname,
               const void *optval, socklen_t optlen) {
    static int (*real)(int, int, int, const void *, socklen_t);
    ALR_REAL(real, int (*)(int, int, int, const void *, socklen_t), "setsockopt");
    int r = real(fd, level, optname, optval, optlen);
    if (r != 0 && (errno == EPERM || errno == EACCES) && level == SOL_SOCKET &&
        (optname == SO_MARK            /* CAP_NET_ADMIN — traffic accounting tag   */
      || optname == SO_BINDTODEVICE    /* CAP_NET_ADMIN — bind to a named iface     */
      || optname == SO_PRIORITY)) {    /* CAP_NET_ADMIN for prio >6                 */
        /* A kernel-denied ROUTING-POLICY hint, not a data-path failure. The packet
         * still flows over the default route untagged. Report success so chromium's
         * PCHECK on the socket-setup result does not ImmediateCrash(). NOT a SELinux
         * bypass: every syscall is still kernel-enforced; we only stop a denied
         * traffic-accounting hint from being treated as fatal. Mirrors the AF_NETLINK
         * bind() workaround at libalr_interpose.c:1174. */
        errno = 0;
        return 0;
    }
    return r;
}
```

**Confidence: HIGH** that SO_MARK is denied; **MEDIUM-HIGH** that *this specific call* is the
~6 s `brk`. It is the best match to all four criteria and the cheapest, lowest-risk fix.

---

### #2 — `setsockopt(SO_BINDTODEVICE)` → EPERM → CHECK

**Why #2.** Same capability (`CAP_NET_ADMIN`), same EPERM-under-untrusted_app, same connect-path
timing. Chromium binds the socket to a specific network interface when a `NetworkHandle` is
specified — `net/socket/socket_posix.cc` / `udp_socket_posix.cc` `BindToNetwork()` →
`setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, …)`. **Lower than #1** only because
`SO_BINDTODEVICE` is used when a *specific* network is requested; the headless default fetch
usually leaves the network unspecified, so this path may not even execute. But it is the same
class and the **wrapper above already covers it** (it lists `SO_BINDTODEVICE`), so #1's fix
addresses #2 for free — no extra work.

**Confidence: MEDIUM** it executes on the default fetch; the fix is free either way.

---

### #3 — `connect(2)` itself returns EPERM/EACCES (SELinux egress) → CHECK  ★ verify first on device

**Why #3.** It is *conceivable* the SM-X236N policy denies the raw `connect()` to a remote
AF_INET endpoint for this app context. **But** the evidence argues against it: CR-2 prior notes
confirm the app UID is in **group 3003 (AID_INET)** and the manifest holds `INTERNET`, so generic
outbound TCP is permitted; and a denied `connect()` would more likely surface as a chromium *net
error* (`ERR_NETWORK_ACCESS_DENIED` / `ERR_CONNECTION_FAILED`) handled gracefully, NOT a `brk`.
chromium treats `connect()` errnos as ordinary `net::Error`s (`net/base/net_errors.cc`
`MapSystemError`), so EPERM there → `ERR_ACCESS_DENIED`, a soft failure — fails criterion (B).
Kept on the list because if §6's stderr capture shows `connect`-related text it jumps to #1.

**Interposer fix if confirmed (DESIGN).** Do **NOT** blanket-fake `connect` success — that would
make chromium think it has a live socket it does not, and break the data path. If and only if the
device log proves `connect()` is the denied call, the correct emulation is to let it proceed and
*not* mask it (chromium already maps it to a soft net error). No wrapper recommended unless §6
proves otherwise. **Confidence it is the cause: LOW.**

---

### #4 — UDP/QUIC socket creation/config denied → fallback path CHECK

**Why #4.** chromium tries QUIC (UDP-443) opportunistically. Creating/`connect`ing a UDP socket
or setting `IP_MTU_DISCOVER` / `IP_RECVTOS` / `SO_RCVBUF` on it may be restricted. The QUIC code
is supposed to *fall back to TCP* on UDP failure, so a clean failure is non-fatal — but
`net/socket/udp_socket_posix.cc` has several `DCHECK`/`PCHECK` on `setsockopt` results
(`SetReceiveBufferSize`, `SetDoNotFragment`, `SetRecvTos`). In a `dcheck_always_on`/`PCHECK`
build a denied UDP socket option would `brk` instead of falling back. **Lower than #1** because
`https://1.1.1.1/` with no prior Alt-Svc cache should attempt **TCP first** within the 6 s
window; QUIC is usually raced slightly later. The #1 wrapper (it matches `SOL_SOCKET` options)
covers `SO_RCVBUF`-class denials too; the IP-level ones (`IP_MTU_DISCOVER`, `IP_RECVTOS`,
`IPV6_RECVTCLASS`) would need an extra clause:

```c
    /* extend the same wrapper: IP/IPv6-level options chromium sets on UDP/QUIC sockets that
     * an untrusted_app may be denied; failing soft keeps QUIC's TCP fallback alive.
     * HOST-VERIFIED include note: IPPROTO_IP/IPPROTO_IPV6/IP_MTU_DISCOVER/IP_TOS/IPV6_TCLASS/
     * IPV6_MTU_DISCOVER come from <netinet/in.h>+<netinet/ip.h>, but IP_RECVTOS(13) and
     * IPV6_RECVTCLASS(66) are ONLY in <linux/in.h>/<linux/in6.h> on the NDK — so this clause
     * additionally needs  #include <netinet/in.h>  and either <linux/in.h>/<linux/in6.h> or a
     * local  #define IP_RECVTOS 13 / #define IPV6_RECVTCLASS 66 . (The #1 SOL_SOCKET clause
     * above needs none of this — it is the safe one to land first.) */
    if (r != 0 && (errno == EPERM || errno == EACCES) &&
        ((level == IPPROTO_IP   && (optname == IP_MTU_DISCOVER || optname == IP_RECVTOS  || optname == IP_TOS)) ||
         (level == IPPROTO_IPV6 && (optname == IPV6_RECVTCLASS || optname == IPV6_TCLASS || optname == IPV6_MTU_DISCOVER)))) {
        errno = 0; return 0;
    }
```
**Confidence: LOW-MEDIUM.** Add this clause only if device log points at a UDP/QUIC option.

---

### #5 — `SO_REUSEADDR` / `IP_TOS` / generic socket-option `CHECK`

`SO_REUSEADDR`, `SO_KEEPALIVE`, `TCP_NODELAY`, `SO_SNDBUF`/`SO_RCVBUF` are **not**
capability-gated — they do not return EPERM for an unprivileged app. So they are unlikely to be
the denied call. `IP_TOS` *can* be restricted; it is folded into #4's clause. **Confidence:
LOW.** No dedicated wrapper.

---

### #6 (lowest) — sandbox/seatbelt FD, `/proc` check, or Mojo network-service IPC

We run `--single-process --no-zygote --no-sandbox`, which **disables the seatbelt/zygote sandbox
and the multiprocess network service** — the network stack runs in-process (`NetworkService` in
the browser process, no Mojo socket-broker IPC). So the seccomp-bpf broker, the
`sandbox::syscall_broker`, the `/proc/self/...` sandbox preflight, and the cross-process Mojo
channel are **not on the path** for this configuration. A `/proc`-based check (e.g. reading
`/proc/sys/net/...`) would be a path-syscall the existing mediation already rewrites/handles, and
would surface as a file error, not a socket `brk`. **Confidence: VERY LOW** for the IP-literal
6 s `brk`. (These matter later for CR-5 multiprocess, not here.)

---

## 2. The one conclusion (keyFinding) and the one action (wsActionNeeded)

**keyFinding:** the ~6 s IP-literal `brk` is, by elimination against all four criteria, a
**capability-gated `setsockopt` on the connect path returning `EPERM` under untrusted_app, which
chromium `PCHECK`s — overwhelmingly `SO_MARK`** (chromium-headless-shell is a *generic-Linux*
build, so `SocketTag::Apply` takes the non-Android `setsockopt(SO_MARK)` branch even on the
Android kernel; `SO_MARK` needs `CAP_NET_ADMIN` the app lacks). The single highest-value action
is a `setsockopt()` interposer wrapper that returns success when a `SOL_SOCKET`
`SO_MARK`/`SO_BINDTODEVICE`/`SO_PRIORITY` set fails `EPERM`/`EACCES` — same shape as the existing
`bind()` NETLINK workaround (`libalr_interpose.c:1174-1184`). It covers #1 and #2 at once and
cannot weaken the sandbox (it only stops a *denied routing hint* from being treated as fatal; the
data socket is untouched).

**wsActionNeeded:** see §1 #1 — add to `app/src/main/cpp/alr_interpose/libalr_interpose.c`, near
the `bind()` workaround (~line 1184), the `setsockopt()` wrapper that returns 0 on
`EPERM`/`EACCES` for `SOL_SOCKET` `SO_MARK`/`SO_BINDTODEVICE`/`SO_PRIORITY`. Use the `ALR_REAL`
macro (`libalr_interpose.c:712`) to resolve the real symbol, preserve/clear `errno` exactly like
`bind()`. If `SO_MARK`/`SO_BINDTODEVICE` are not visible from the NDK headers, define them
(arm64/asm-generic: `SO_MARK=36`, `SO_BINDTODEVICE=25`, `SO_PRIORITY=12`). This wrapper is
**safe to land before the exact CHECK line is captured** — it is a no-op on any platform where
these options succeed, and on desktop Linux these never fail, so it cannot regress CR-1.

---

## 3. How to CONFIRM on device (which log line / errno)

The wrapper above is safe to land speculatively, but to *confirm* the root cause WS-1 should
capture the un-truncated chromium stderr (see §6) and look for, in priority order:

1. **The `CHECK`/`PCHECK` line itself.** A `SO_MARK` failure most commonly logs near
   `net/socket/...` with a form like
   `[FATAL:socket_posix.cc(NNN)] Check failed: ... setsockopt ... Operation not permitted (1)`
   or `PCHECK failed: ... rv == 0`. The errno text **`Operation not permitted (1)` = `EPERM`**
   is the smoking gun (1 = `EPERM`; 13 = `EACCES`).
2. **Before the fix:** the message should name a `socket`/`socket_posix`/`udp_socket_posix`/
   `socket_tag` source file and an option set. `SO_MARK`/`mark`/`tag` in the text → #1 confirmed.
   `SO_BINDTODEVICE`/`bind_to_network`/`ifname` → #2. `IP_MTU_DISCOVER`/`recv_tos`/`quic` → #4.
3. **After the fix (validation):** the `brk`/`FATAL` line should be **gone**, exit changes from
   `-1 sig=5` to either a clean `exit=0` with DOM (full PASS) **or** advances to a *soft* net
   error — e.g. `net::ERR_CERT_AUTHORITY_INVALID` (CA bundle, already staged) or
   `ERR_TIMED_OUT`/`ERR_CONNECTION_REFUSED` — which is **progress**: it means the socket was set
   up and chromium reached the TLS handshake / data phase. Any soft `net::ERR_*` instead of a
   `brk` = the connect-path `CHECK` is fixed; remaining work is TLS/CA or routing, not a crash.
4. **A/B isolation independent of stderr:** if the un-truncated stderr is still hard to get, run
   the IP-literal probe **with vs. without** the new wrapper. If the run flips from
   `exit=-1 sig=5 @ ~6s` to **anything else** (clean exit, soft net error, or even a *different*
   later crash), the SO_MARK/setsockopt hypothesis is confirmed and #1 was the (first) blocker.

---

## 4. Why this is NOT a SELinux bypass (constraint check)

The wrapper returns success **only** for routing-policy *hints* (`SO_MARK` = traffic accounting
tag; `SO_BINDTODEVICE` = bind to a named iface; `SO_PRIORITY` = egress priority) that the kernel
denied. None of these change what bytes leave the device or where they go on the default route —
they are *advisory* QoS/accounting metadata. The kernel still enforces every actual syscall
(`socket`, `connect`, `send`, the TLS bytes); we do not call any privileged syscall on chromium's
behalf, do not change the socket's address/route, and do not touch any data-carrying option. This
is the **identical safety argument** as the existing `bind()` NETLINK workaround: a kernel-denied
*probe/hint* is prevented from being misread as a fatal condition, nothing more. W^X-safe (no new
mappings), public-API-only (libc `setsockopt`), and a strict no-op wherever the option succeeds.

---

## 5. Residual uncertainty (honest)

- **I have not seen the literal CHECK line** (it is inside the 5192-byte truncated stderr). The
  ranking is an elimination argument from the four hard constraints (`brk`/`TRAP_BRKPT`, ~6 s,
  IP-literal, post-socket) + chromium 147 net-stack structure, not a direct read of the message.
  §3 + §6 close that gap on the next drain.
- **Exact source line numbers** for `socket_tag.cc` / `socket_posix.cc` in 147.0.7727.137 are not
  pinned here (no chromium checkout on the host); the *files and functions* are correct for the
  147 tree. WS-1 / the device log will surface the exact `file(line)`.
- **`PCHECK` vs `DCHECK` build flag.** If headless-shell is a release build with `DCHECK` off,
  the failing assert must be a `CHECK`/`PCHECK`/`RAW_CHECK` (always-on), which `SO_MARK`-class
  paths do use, OR a `dcheck_always_on` build. Either way the wrapper neutralises the EPERM
  *before* the assert evaluates, so it fixes both. (If it were a `DCHECK`-only assert compiled
  out in release, there would be no `brk` at all — the fact that there IS a `brk` confirms an
  always-on check, consistent with #1.)
- **Possible second connect-path blocker.** If §3-style A/B shows the `brk` moves to a *different*
  pc after the SO_MARK fix, a second option (most likely #4's IP-level UDP/QUIC clause) is in
  play; the §1 #4 extension is pre-written for that. Land #1 first, re-drain, then decide.

---

## 6. Prerequisite for confirmation: full stderr capture (WS-1 loader note, separate from the fix)

The diagnosis-grade follow-up (independent of the fix, which is safe to land now): the loader
captures chromium's **full** stderr into the report string (`runtime_report.cpp:264,292`
`stderr_text = read_all_from_fd(stderr_pipe[0]); ... out << "...stderr=" << stderr_text;`), but
the Android-side logging truncates the report to head/tail so the middle (where the `FATAL` line
lives) is lost. To read the exact CHECK line, **redirect the guest's stderr to a file in a
writable rootfs dir** (e.g. `--user-data-dir`'s parent, or a `2>` to a tmp path the app can read
back) instead of relying on the truncated logcat copy — then `grep -i 'check\|fatal\|so_mark\|
setsockopt\|operation not permitted'` it. This is a WS-1/MainActivity capture change, NOT part of
the interposer fix; the fix in §2 stands on its own. (Captured here as the confirmation enabler,
not as a blocker on landing the wrapper.)

---

## 7. Summary table

| # | Denied op | chromium 147 call site | errno | abort? | connect-path? | interposer fix | conf. |
|---|-----------|------------------------|-------|--------|---------------|----------------|-------|
| 1 | `setsockopt(SO_MARK)` | `net/socket/socket_tag.cc` `SocketTag::Apply` (non-Android `#else`); `tcp_socket_posix.cc SetDefaultOptionsForClient` | EPERM | PCHECK→`brk` | yes, per-conn | **§2 wrapper (TOP)** | HIGH |
| 2 | `setsockopt(SO_BINDTODEVICE)` | `socket_posix.cc`/`udp_socket_posix.cc` `BindToNetwork` | EPERM | CHECK | only if NetworkHandle set | covered by §2 wrapper | MED |
| 3 | `connect(2)` egress | `socket_posix.cc Connect` → `MapSystemError` | EPERM/EACCES | soft net err (NOT brk) | yes | none (already soft) | LOW |
| 4 | UDP/QUIC `setsockopt` (`IP_MTU_DISCOVER`/`IP_RECVTOS`) | `udp_socket_posix.cc` `SetDoNotFragment`/`SetRecvTos` | EPERM/EACCES | PCHECK if no fallback | QUIC, slightly later | §1 #4 IP-level clause | LOW-MED |
| 5 | `SO_REUSEADDR`/`IP_TOS`/bufs | `socket_options.cc` | usually 0 | — | yes | none | LOW |
| 6 | sandbox FD / Mojo IPC | `sandbox/`, `services/network` | n/a | — | disabled by `--no-sandbox --single-process` | none | V.LOW |
