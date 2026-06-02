# Design — Interposer NETLINK recvmsg short-circuit (CR-2 robustness)

Status: DESIGN (host-only; WS-1 implements in `app/src/main/cpp/alr_interpose/libalr_interpose.c`)
Owner of this doc: auto/netlink-recv. Owner of the implementation file: WS-1.
Date: 2026-06-02 · Device target SM-X236N / Android 16 / untrusted_app / Mali-G615

---

## 1. Problem (what the bind() workaround left open)

CR-1 (chromium-headless-shell `--dump-dom` of a `data:` URL) is ACHIEVED in-process.
CR-2 (network) is the frontier. The first network blocker was NETLINK: chromium's
`net::AddressTrackerLinux` opens `socket(AF_NETLINK, SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,
NETLINK_ROUTE)` and `bind()`s it to the `RTMGRP_LINK | RTMGRP_IPV4_IFADDR |
RTMGRP_IPV6_IFADDR` multicast groups to watch interface/route changes for
`NetworkChangeNotifier`. Android SELinux denies `untrusted_app` that multicast bind
(EACCES, `address_tracker_linux.cc:243 "Could not bind NETLINK socket: Permission
denied"`), and with connectivity unknown chromium stalled every request to its 180 s
alarm.

The shipped fix (WS-1, `libalr_interpose.c` `bind()` wrapper) makes a netlink bind that
fails EACCES/EPERM return success. That advanced chromium PAST the bind: it now issues the
request (`NetworkDelegate::NotifyBeforeURLRequest`). But it is **necessary-but-insufficient**:

After the (now faked-success) bind, `AddressTrackerLinux::Init()` does a *synchronous*
initial enumeration on that same socket:

```
sendmsg(fd, RTM_GETLINK  dump request)  ; then loop recvmsg until NLMSG_DONE   -> builds the iface set
sendmsg(fd, RTM_GETADDR  dump request)  ; then loop recvmsg until NLMSG_DONE   -> builds the addr set
```

(In chromium this is `AddressTrackerLinux::DumpInitialAddressesAndWatch()` calling the
private `Request`/`HandleMessage` helpers; the read side is `ReadMessages()` over the
socket via `recvmsg`.) If SELinux *also* blocks the netlink send/recv on this socket — not
just the multicast bind — then:

* the `sendmsg` may fail, OR succeed but produce no reply, and
* the **`recvmsg` blocks** (or, on a NONBLOCK socket, spins/EAGAIN-loops via the message
  loop) waiting for a dump reply that never arrives — an **intermittent hang**.

Device-observed symptom of exactly this: with the bind fix in place, chromium `--version`
(reliably ~1 s exit-0 across dozens of prior drains) **once took 41 s + exit=-1**
(`docs/evidence/2026-06-02-cr2-network-netlink-blocker.md` §"Caveat found"). That flakiness
is the netlink read loop occasionally wedging on the empty/blocked dump.

**Goal of this design:** make the netlink socket's *read* side deterministically complete
the enumeration by **synthesizing a minimal, valid, in-process netlink reply** when a real
`recvmsg`/`recv`/`recvfrom` on a tracked AF_NETLINK fd would block or error. This removes
the init-hang probability entirely and does NOT touch the SELinux sandbox — we only stop a
kernel-denied connectivity *probe* from wedging the read loop.

---

## 2. Two reply strategies

We can synthesize either:

### Strategy A — bare `NLMSG_DONE` (empty enumeration)
A single `NLMSG_DONE` message ⇒ chromium sees **0 interfaces, 0 addresses**. The dump
completes immediately. `AddressTrackerLinux` finishes Init with an empty connection-type /
empty address map. `NetworkChangeNotifier` then reports `CONNECTION_NONE` /
`CONNECTION_UNKNOWN`.

* PRO: trivial, 20 bytes total, impossible to mis-parse, zero attribute encoding.
* CON: chromium may conclude it is **offline** (no usable interface) and *still* refuse to
  start requests, re-introducing the original 180 s stall through a different door.
  `AddressTrackerLinux::GetCurrentConnectionType()` returns `CONNECTION_NONE` when it has no
  online non-loopback interface.

### Strategy B — minimal "one interface UP" reply (RECOMMENDED)
A multipart dump that makes chromium believe the host is online:

For the **RTM_GETLINK** dump reply: one `RTM_NEWLINK` for `lo` (loopback, UP) **plus** one
`RTM_NEWLINK` for a synthetic non-loopback iface `alr0` (UP|RUNNING|LOWER_UP), then
`NLMSG_DONE`.

For the **RTM_GETADDR** dump reply: one `RTM_NEWADDR` (IPv4) on the `alr0` iface giving it a
private address (e.g. `10.0.2.15`, the classic emulated-NAT guest IP), then `NLMSG_DONE`.

* PRO: chromium's `AddressTrackerLinux` finds an online, non-loopback interface with an
  address ⇒ `GetCurrentConnectionType()` ≠ `CONNECTION_NONE` ⇒ it proceeds to actually
  open the AF_INET data socket (which is permitted; the app UID is in AID_INET/3003). This
  is the path that lets CR-2's *real* connect/DNS work get exercised.
* CON: ~3× the bytes, must encode `ifinfomsg`/`ifaddrmsg` + a couple of rtattrs exactly.

### Decision: implement BOTH, default to B, env-selectable.
`A` is the safe fallback if `B` ever destabilizes chromium's tracker; `B` is what actually
unblocks CR-2 (an online interface). Gate with an env var so WS-1/device drains can A/B it
without a rebuild:

```
ALR_NL_EMU = "0"  -> disabled: pass real recvmsg through unchanged (current behavior)
ALR_NL_EMU = "1"  -> Strategy A (bare NLMSG_DONE)
ALR_NL_EMU = "2"  -> Strategy B (lo + alr0 UP + one IPv4 addr)   [DEFAULT when unset]
```

Reading the env in the constructor mirrors the existing `g_pcgate` pattern (read once,
store in a static int — see §6).

> **Why B is "safer" in the CR-2 sense:** A is byte-safer but *behaviorally* riskier (can
> re-stall as offline). B carries the (small, bounded) risk of a malformed attribute, which
> this doc removes by giving the exact byte layout. Net: B is the recommended default
> because it advances the actual goal; A is the conservative escape hatch.

---

## 3. Exact wire layout (arm64, little-endian; all the numbers WS-1 needs)

All values verified against the aarch64 UAPI headers
(`linux/netlink.h`, `linux/rtnetlink.h`, `linux/if_link.h`, `linux/if_addr.h`,
`linux/if.h`, `linux/if_arp.h`) on the build sysroot.

### 3.1 Constants (hard-code; do not rely on chromium's copies)
```c
/* nlmsg_type */
#define ALR_NLMSG_NOOP   0x1
#define ALR_NLMSG_ERROR  0x2
#define ALR_NLMSG_DONE   0x3
#define ALR_RTM_NEWLINK  16
#define ALR_RTM_NEWADDR  20
/* nlmsg_flags */
#define ALR_NLM_F_MULTI  0x02   /* set on every dump element EXCEPT it's also set on DONE */
/* ifi_flags / IFF_* (bit positions) */
#define ALR_IFF_UP        (1u<<0)
#define ALR_IFF_LOOPBACK  (1u<<3)
#define ALR_IFF_RUNNING   (1u<<6)
#define ALR_IFF_LOWER_UP  (1u<<16)
#define ALR_IFF_MULTICAST (1u<<12)
/* ifi_type / ARPHRD_* */
#define ALR_ARPHRD_LOOPBACK 772
#define ALR_ARPHRD_ETHER    1
/* rtattr types */
#define ALR_IFLA_ADDRESS  1
#define ALR_IFLA_IFNAME   3
#define ALR_IFA_ADDRESS   1
#define ALR_IFA_LOCAL     2
#define ALR_IFA_LABEL     3
/* address families */
#define ALR_AF_INET   2
#define ALR_AF_UNSPEC 0
/* alignment */
#define ALR_NLMSG_ALIGNTO 4u
#define ALR_RTA_ALIGNTO   4u
#define ALR_NLA(n) (((n)+ALR_NLMSG_ALIGNTO-1) & ~(ALR_NLMSG_ALIGNTO-1))
#define ALR_RTA(n) (((n)+ALR_RTA_ALIGNTO-1)   & ~(ALR_RTA_ALIGNTO-1))
```

### 3.2 Fixed structs (sizes on arm64)
```c
struct nlmsghdr  { u32 nlmsg_len; u16 nlmsg_type; u16 nlmsg_flags; u32 nlmsg_seq; u32 nlmsg_pid; }; /* 16 bytes */
struct ifinfomsg { u8 ifi_family; u8 __pad; u16 ifi_type; s32 ifi_index; u32 ifi_flags; u32 ifi_change; }; /* 16 bytes */
struct ifaddrmsg { u8 ifa_family; u8 ifa_prefixlen; u8 ifa_flags; u8 ifa_scope; u32 ifa_index; }; /* 8 bytes */
struct rtattr    { u16 rta_len; u16 rta_type; }; /* 4 bytes; payload follows, padded to 4 */
```
`NLMSG_HDRLEN = NLMSG_ALIGN(16) = 16`. `RTA_LENGTH(n) = RTA_ALIGN(4) + n = 4 + n`;
`rta_len` is set to `4 + payload` (UNALIGNED, per kernel convention), but the *next*
attribute starts at `RTA_ALIGN(rta_len)`.

### 3.3 Strategy A bytes — bare DONE (one message, 20 bytes)
A `NLMSG_DONE` carries a 4-byte `int` payload (the dump error/terminator code, 0):
```
nlmsghdr {
  nlmsg_len   = 20            ; 16 hdr + 4 payload
  nlmsg_type  = NLMSG_DONE(3)
  nlmsg_flags = NLM_F_MULTI(2)
  nlmsg_seq   = <echo request seq>   ; see §4 — copy the seq from the request
  nlmsg_pid   = <echo nl_pid>        ; the socket's auto-bound pid (see §4)
}
int payload = 0                ; 4 bytes
```
chromium accepts a DONE with or without the trailing int; emit the int (kernel always does).

### 3.4 Strategy B — RTM_GETLINK reply (lo + alr0 + DONE)

**Message 1: RTM_NEWLINK for `lo`**
```
nlmsghdr  nlmsg_len = 16 + 16 + RTA(IFLA_IFNAME="lo\0")        = 16+16+ (4+3 ->RTA8) = 40
          nlmsg_type=RTM_NEWLINK(16) flags=NLM_F_MULTI(2) seq=<req seq> pid=<nl_pid>
ifinfomsg ifi_family=AF_UNSPEC(0) __pad=0 ifi_type=ARPHRD_LOOPBACK(772)
          ifi_index=1 ifi_flags=IFF_UP|IFF_LOOPBACK|IFF_RUNNING|IFF_LOWER_UP (0x10049)
          ifi_change=0
rtattr    rta_len=7 rta_type=IFLA_IFNAME(3)  data="lo\0"  (3 bytes) + 1 pad byte = 8 on wire
```
ifi_flags value: `IFF_UP(1) | IFF_LOOPBACK(8) | IFF_RUNNING(64) | IFF_LOWER_UP(65536) =
0x10049`.

**Message 2: RTM_NEWLINK for `alr0` (the online non-loopback iface)**
```
nlmsghdr  nlmsg_len = 16 + 16 + RTA(IFLA_ADDRESS, 6) + RTA(IFLA_IFNAME, "alr0\0"=5)
          = 16 + 16 + (4+6 ->RTA12) + (4+5 ->RTA12) = 56
          nlmsg_type=RTM_NEWLINK(16) flags=NLM_F_MULTI(2) seq=<req seq> pid=<nl_pid>
ifinfomsg ifi_family=AF_UNSPEC(0) ifi_type=ARPHRD_ETHER(1)
          ifi_index=2 ifi_flags=IFF_UP|IFF_RUNNING|IFF_LOWER_UP|IFF_MULTICAST (0x11041)
          ifi_change=0
rtattr#1  rta_len=10 rta_type=IFLA_ADDRESS(1) data=02:00:00:00:00:01 (6 MAC bytes) +2 pad =12
rtattr#2  rta_len=9  rta_type=IFLA_IFNAME(3)  data="alr0\0" (5 bytes) +3 pad =12
```
ifi_flags: `IFF_UP(1)|IFF_RUNNING(64)|IFF_LOWER_UP(65536)|IFF_MULTICAST(4096) = 0x11041`.
The MAC `02:00:00:00:00:01` is a locally-administered unicast address (bit 1 of first
octet set, bit 0 clear) — valid and inert. IFLA_ADDRESS is optional for chromium's link
parse; include it because some chromium versions read the hardware address for the
connection-type heuristic and a missing one is treated as "unknown" rather than online.

**Message 3: NLMSG_DONE** — exactly as §3.3 (len 20).

### 3.5 Strategy B — RTM_GETADDR reply (one IPv4 addr on alr0 + DONE)

**Message 1: RTM_NEWADDR (IPv4 on alr0)**
```
nlmsghdr  nlmsg_len = 16 + 8 + RTA(IFA_ADDRESS,4) + RTA(IFA_LOCAL,4) + RTA(IFA_LABEL,"alr0\0"=5)
          = 16 + 8 + (4+4->RTA8) + (4+4->RTA8) + (4+5->RTA12) = 52
          nlmsg_type=RTM_NEWADDR(20) flags=NLM_F_MULTI(2) seq=<req seq> pid=<nl_pid>
ifaddrmsg ifa_family=AF_INET(2) ifa_prefixlen=24 ifa_flags=0 ifa_scope=0 (RT_SCOPE_UNIVERSE)
          ifa_index=2   ; matches alr0
rtattr#1  rta_len=8 rta_type=IFA_ADDRESS(1) data=0x0A00020F (10.0.2.15, network order: 0A 00 02 0F)
rtattr#2  rta_len=8 rta_type=IFA_LOCAL(2)   data=0x0A00020F (same; IFA_LOCAL is the local addr)
rtattr#3  rta_len=9 rta_type=IFA_LABEL(3)   data="alr0\0" (5) +3 pad =12
```
The IPv4 bytes go on the wire in network byte order: `10.0.2.15` = `0A 00 02 0F`. chromium
reads BOTH `IFA_ADDRESS` and `IFA_LOCAL`; provide them identical (correct for a non-p2p
iface). `ifa_scope=0` = `RT_SCOPE_UNIVERSE` (global) — required, a loopback/host scope would
make chromium discard it as non-global. Use a non-loopback, non-link-local, non-deprecated
address so `AddressTrackerLinux` counts it (it filters out tentative/deprecated/loopback).

> Optionally also emit an IPv6 `RTM_NEWADDR` (`ifa_family=AF_INET6(10)`, 16-byte address,
> `ifa_scope=0`). NOT required — one global IPv4 is enough for chromium to be "online".
> Keep the reply minimal; add IPv6 only if a drain shows chromium still treats v4-only as
> unusable (unlikely).

**Message 2: NLMSG_DONE** — as §3.3.

### 3.6 Sequencing of the dump on a NONBLOCK socket
chromium's socket is `SOCK_NONBLOCK`. Its read loop issues `recvmsg` repeatedly and
processes whatever bytes come back, treating EAGAIN as "no more right now" and DONE as "dump
finished". We therefore deliver the **entire** synthetic dump for a given request type in a
**single** `recvmsg` return (all messages concatenated in the caller's buffer, total ≤ ~140
bytes — far under any realistic buffer). One DONE per dump. This avoids any need to model
multi-syscall partial reads.

---

## 4. The req-matching subtlety (seq / pid)

Netlink dump replies echo the **`nlmsg_seq`** of the triggering request and carry the
socket's **`nl_pid`** (the kernel-assigned port id, usually the tid for an auto-bound
socket). chromium's `AddressTrackerLinux` is *lenient* — its `ReadMessages` does NOT
strictly reject a mismatched seq/pid in the headless path (it processes NEWLINK/NEWADDR/DONE
by type) — but to be byte-faithful and robust across chromium versions we should echo them:

* **seq:** record the `nlmsg_seq` from the last `sendmsg`/`send` we saw on that fd (we
  observe the outbound request — see §5 — and stash its first `nlmsghdr.nlmsg_seq`). Two
  dumps (GETLINK then GETADDR) use two seqs; tracking "last seq per fd" suffices because the
  dumps are strictly serialized (chromium finishes one dump's DONE before sending the next).
* **pid:** echo the fd's bound `nl_pid`. We can get it with a real `getsockname(fd)` into a
  `struct sockaddr_nl` (an allowed syscall; if it fails, use 0 — chromium accepts pid 0,
  which is what the kernel uses for multicast/broadcast-origin messages). Cache it per fd at
  socket()/first-send time.

Also remember the **request type** so we return the matching dump: inspect the outbound
buffer's first `nlmsghdr.nlmsg_type` (`RTM_GETLINK=18` ⇒ reply with the LINK dump;
`RTM_GETADDR=22` ⇒ reply with the ADDR dump). `RTM_GETLINK=18`, `RTM_GETADDR=22` (RTM_GETx =
RTM_NEWx+2). If we cannot determine the type (no send observed), default to replying GETLINK
first then GETADDR on the next read — but type-tracking from the send is cleaner and is the
recommended path.

---

## 5. fd-tracking design (which fds are "ours")

We must only synthesize for the **specific** AF_NETLINK sockets chromium opens for the route
tracker — never touch AF_INET data sockets or any other fd.

### 5.1 Intercept `socket()` to record AF_NETLINK fds
Add a `socket()` wrapper (NOT a path syscall; runs as a raw syscall today, so we add a thin
libc-level interposer exactly like `bind()`):

```c
int socket(int domain, int type, int protocol) {
    static int (*real)(int,int,int);
    ALR_REAL(real, int(*)(int,int,int), "socket");
    int fd = real(domain, type, protocol);
    if (fd >= 0 && domain == AF_NETLINK /* 16 */ && protocol == NETLINK_ROUTE /* 0 */)
        alr_nl_track(fd);          /* record fd in the netlink-fd table */
    return fd;
}
```
`NETLINK_ROUTE == 0`. Track only `NETLINK_ROUTE`; chromium's other netlink uses (none in the
headless path) stay untouched. `type` may carry `SOCK_CLOEXEC|SOCK_NONBLOCK` bits — mask to
`SOCK_RAW`/`SOCK_DGRAM` only if you want to be selective; not necessary, domain+protocol is
enough.

### 5.2 The fd table
Tiny fixed-size open-addressed table (no heap, matching the file's "zero heap" rule). Netlink
sockets are few (chromium opens 1, occasionally a 2nd). 16 slots is ample:

```c
#define ALR_NL_MAX 16
typedef struct {
    int      fd;        /* -1 = empty */
    uint32_t last_seq;  /* echoed into replies */
    uint16_t last_type; /* RTM_GETLINK / RTM_GETADDR of the last observed request */
    uint32_t nl_pid;    /* bound port id (from getsockname), 0 if unknown */
    uint8_t  did_link;  /* 1 once we've answered a GETLINK dump (for the type-less fallback) */
} alr_nl_slot;
static alr_nl_slot g_nl[ALR_NL_MAX];   /* zero-initialized => all .fd == 0, so init to -1 in ctor */
```
* `alr_nl_track(fd)`: find an empty slot (fd==-1) or evict by fd, set `{fd, last_seq=0,
  last_type=0, nl_pid=0, did_link=0}`.
* `alr_nl_find(fd) -> slot* | NULL`.
* `alr_nl_untrack(fd)`: on `close(fd)` clear the slot (add a `close()` wrapper, OR — cheaper
  — just let a future `socket()` returning the same fd number overwrite the slot via evict;
  but a `close()` wrapper is the correct, leak-free choice and `close` is already trivially
  interposable). **Recommend the `close()` wrapper** so a reused fd number for a non-netlink
  socket is never mistaken for a tracked one.

Concurrency: chromium's tracker runs on a single dedicated thread, but be defensive — guard
the table with a tiny spinlock OR rely on the fact that each fd is touched by exactly one
thread (the tracker thread) and `socket`/`close`/`recvmsg` for a given fd are serialized by
the caller. The pointer stores are word-sized/atomic on aarch64. **Recommendation:** no lock;
document the single-owner-thread invariant. If WS-1 prefers belt-and-suspenders, a
`__atomic` compare on `fd` is enough.

### 5.3 Observe the outbound request (`sendmsg`/`send`/`write`)
chromium uses `sendmsg` (it builds an `iovec` of `{nlmsghdr, ifinfomsg/ifaddrmsg}`). Add a
`sendmsg()` wrapper that, for a tracked fd, peeks the first `nlmsghdr` in `msg->msg_iov[0]`
to capture `nlmsg_seq` and `nlmsg_type` into the slot, then forwards to the real `sendmsg`:

```c
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    static ssize_t (*real)(int, const struct msghdr*, int);
    ALR_REAL(real, ssize_t(*)(int,const struct msghdr*,int), "sendmsg");
    alr_nl_slot *s = alr_nl_find(fd);
    if (s && msg && msg->msg_iov && msg->msg_iovlen >= 1 &&
        msg->msg_iov[0].iov_len >= sizeof(struct nlmsghdr)) {
        const struct nlmsghdr *h = msg->msg_iov[0].iov_base;
        s->last_seq  = h->nlmsg_seq;
        s->last_type = h->nlmsg_type;     /* RTM_GETLINK(18) or RTM_GETADDR(22) */
        if (s->nl_pid == 0) alr_nl_capture_pid(fd, s);  /* lazy getsockname */
    }
    ssize_t r = real(fd, msg, flags);
    /* If the real send was DENIED by SELinux (EACCES/EPERM), we STILL have the
       seq/type captured above, so the recvmsg short-circuit can answer. Returning
       the real error here is fine — chromium proceeds to recvmsg regardless. */
    return r;
}
```
Note: even if the real `sendmsg` fails EACCES, we have already captured what we need; we do
NOT fake the send success (no need — chromium's read loop runs anyway, and faking it risks
chromium thinking more is pending). Capturing on send is the clean trigger.

> If chromium ever uses `send`/`write` instead of `sendmsg` on this socket, add the same
> capture to those wrappers. Current chromium uses `sendmsg`; `send`/`write` wrappers are a
> cheap insurance addition.

---

## 6. The recvmsg / recv / recvfrom short-circuit

### 6.1 Policy
For a tracked netlink fd, intercept the read. Decision tree:

1. If `ALR_NL_EMU == 0` (disabled): call the real recvmsg unchanged. (escape hatch)
2. Else, **first** try the real recvmsg ONCE (non-destructively — see below). If it returns
   `> 0` with real netlink data, **pass it through** (the kernel actually answered; never
   override a real reply). The only reason to synthesize is when the kernel did NOT answer.
3. If the real recvmsg returns `-1` with `EAGAIN/EWOULDBLOCK` (NONBLOCK socket, nothing
   there), OR `-1` with `EACCES/EPERM/ENOBUFS`, OR `0` (EOF-like), **synthesize**: write the
   appropriate dump (per the slot's `last_type`) into the caller's buffer and return its
   length. After delivering a DONE for a given dump, mark the slot so a subsequent read for
   the *same* type returns EAGAIN (real) — i.e. don't loop the same dump forever.

> **Why "try real first":** it preserves correctness if the kernel ever DOES allow the
> dump (e.g. a future Android relaxation) — we never fabricate over a real answer. It also
> means on a blocking socket we must NOT block forever: chromium's socket is NONBLOCK so the
> real call returns EAGAIN promptly; for the (unexpected) blocking case, see §6.4.

### 6.2 What "synthesize into the caller's buffer" means for recvmsg
`recvmsg(fd, struct msghdr *msg, flags)` scatters into `msg->msg_iov[]`. We must copy our
synthetic bytes across the iovecs (chromium typically uses a single large iovec, but copy
generically across `msg_iov[0..iovlen)` and stop when our bytes are exhausted). Set
`msg->msg_namelen` appropriately if `msg_name` is non-NULL (fill a `struct sockaddr_nl`
with `nl_family=AF_NETLINK, nl_pid=0, nl_groups=0`), clear `msg->msg_flags`
(no `MSG_TRUNC`), and return the total bytes written.

```c
ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    static ssize_t (*real)(int, struct msghdr*, int);
    ALR_REAL(real, ssize_t(*)(int,struct msghdr*,int), "recvmsg");
    alr_nl_slot *s = (g_nl_emu ? alr_nl_find(fd) : NULL);
    if (!s) return real(fd, msg, flags);          /* not ours -> untouched */

    ssize_t r = real(fd, msg, flags);
    if (r > 0) return r;                          /* real reply -> never override */
    if (r == 0 || (r < 0 && (errno==EAGAIN||errno==EWOULDBLOCK||
                             errno==EACCES||errno==EPERM||errno==ENOBUFS))) {
        return alr_nl_synth_into_msg(s, msg);     /* fabricate the dump or EAGAIN-after-DONE */
    }
    return r;                                     /* other errors: surface them */
}
```

`recv`/`recvfrom` map to the same `alr_nl_synth` with a flat buffer instead of iovecs:
```c
ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *src, socklen_t *srclen) { ...same gate; synth into flat buf... }
ssize_t recv(int fd, void *buf, size_t n, int flags) { return recvfrom(fd,buf,n,flags,NULL,NULL); }
```
chromium uses `recvmsg`; provide `recv`/`recvfrom` for completeness/robustness.

### 6.3 The synth state machine (per slot, per dump)
```c
static ssize_t alr_nl_synth_into_msg(alr_nl_slot *s, struct msghdr *msg) {
    if (g_nl_emu == 1) {                 /* Strategy A: only ever a bare DONE */
        if (s->done_a) { errno = EAGAIN; return -1; }
        s->done_a = 1;
        return alr_emit_done(s, msg);
    }
    /* Strategy B (default): answer by last request type, then DONE, then EAGAIN. */
    uint16_t t = s->last_type;           /* RTM_GETLINK(18) / RTM_GETADDR(22) */
    if (t == RTM_GETLINK && !s->done_link) { s->done_link = 1; return alr_emit_link_dump(s, msg); }
    if (t == RTM_GETADDR && !s->done_addr) { s->done_addr = 1; return alr_emit_addr_dump(s, msg); }
    errno = EAGAIN; return -1;           /* dump for this type already delivered */
}
```
Where `alr_emit_link_dump` writes [NEWLINK(lo) + NEWLINK(alr0) + DONE] and
`alr_emit_addr_dump` writes [NEWADDR(alr0) + DONE], each as one contiguous blob copied into
the msg, echoing `s->last_seq` and `s->nl_pid` into every header. Reset `done_link/done_addr`
if a NEW request of that type arrives (i.e. clear the flag in the `sendmsg` capture when
`last_type` is (re)set), so a tracker that re-dumps later still gets answered.

> IMPORTANT ordering: the `done_*` flags are *per request*, reset on each matching `sendmsg`.
> AddressTrackerLinux issues GETLINK once and GETADDR once at Init; re-dumps happen only on
> change events (which we never deliver, since the multicast bind was faked — so there are
> no async events). So in practice each dump is answered exactly once.

### 6.4 The blocking-socket guard (defensive)
If the socket is somehow BLOCKING (not chromium's case, but a guest could clear NONBLOCK),
the "try real first" recvmsg would block forever on an empty SELinux-denied socket. Guard:
before the real recvmsg, if the fd is tracked AND `ALR_NL_EMU != 0` AND we have a pending
un-answered dump for `last_type`, **skip the real call and synthesize immediately**. This
trades a one-syscall correctness probe for guaranteed non-blocking. Recommended
implementation: check `fcntl(fd, F_GETFL) & O_NONBLOCK`; if NOT set and we have a pending
dump, synthesize without calling real recvmsg. This keeps §6.1-step-2 ("never override a
real reply") for the normal NONBLOCK path while never hanging on a blocking socket.

---

## 7. Why this is W^X-safe and not a SELinux bypass

* No codegen, no new executable mappings: all synthesis is plain data writes into a
  caller-provided buffer from `.text`/`.data`. W^X-clean exactly like the rest of the
  interposer.
* No syscall is weakened: we call the REAL `socket`/`sendmsg`/`recvmsg` first; the kernel
  still enforces every one. SELinux still denies the multicast bind and (if it does) the
  send/recv. We only stop chromium from *wedging* on a kernel-denied connectivity *probe* by
  handing it the minimal "no changes / one iface up" enumeration the kernel would have given
  on an unrestricted host. The real data path (AF_INET connect to 1.1.1.1, DNS) is wholly
  unaffected — those sockets are never tracked or faked.
* Bounded, allocation-free, fixed 16-slot table; no heap; reentrancy via single-owner-thread
  invariant (documented).

---

## 8. Exactly what WS-1 implements in `libalr_interpose.c`

1. **Includes** (add near the existing `<sys/socket.h>`):
   `<linux/netlink.h>`, `<linux/rtnetlink.h>`, `<linux/if_link.h>`, `<linux/if_addr.h>`,
   `<linux/if.h>`, `<linux/if_arp.h>`. If any are absent on a future sysroot, fall back to
   the `#define`/`struct` shims in §3 (same `__has_include` guard pattern the file already
   uses for `linux/openat2.h`).
2. **Globals**: `static int g_nl_emu;` (read in ctor), the `g_nl[ALR_NL_MAX]` table, init all
   `.fd=-1` in `alr_init()`/constructor.
3. **Ctor read**: `const char *e = getenv("ALR_NL_EMU"); g_nl_emu = e ? (e[0]-'0') : 2;`
   (clamp to 0/1/2). Mirror the `g_pcgate` read site (libalr_interpose.c around the
   constructor, near line 679).
4. **Helpers** (file-static, no heap, no interposed libc calls inside — use the byte helpers
   `a_len` etc. and raw struct writes): `alr_nl_track/find/untrack`, `alr_nl_capture_pid`
   (a real `getsockname`), `alr_emit_done/alr_emit_link_dump/alr_emit_addr_dump`,
   `alr_nl_synth_into_msg`, and a small `alr_copy_to_iov(msg, buf, n)`.
5. **Wrappers** (libc-level, RTLD_NEXT, same shape as the existing `bind()` at line 1174):
   `socket`, `close` (untrack), `sendmsg` (capture seq/type/pid), `recvmsg`, `recv`,
   `recvfrom`. Place them in the same "NOT a path syscall" section as `bind()`.
6. **No loader/supervisor change is required.** These are all raw, un-traced syscalls
   (sockets are not PCGATE-traced — see the task brief), so the PC gate and ptrace
   supervisor are untouched. No `runtime_report.cpp` edit. The constructor already runs the
   interposer; these wrappers slot in beside `bind()`.

### Function signatures WS-1 adds (verbatim)
```c
int     socket(int domain, int type, int protocol);
int     close(int fd);                                  /* untrack; forward to real */
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
```
(`<sys/socket.h>` already included declares `msghdr`, `sockaddr`, `socklen_t`; add
`<sys/types.h>` is already present for `ssize_t`.)

> CAUTION for WS-1 — `close()` is hot and on many paths. The `close` wrapper must be a
> thin `{ alr_nl_untrack(fd); return real_close(fd); }`. `alr_nl_untrack` is an O(16) scan of
> a tiny array with an early-out (almost always "fd not tracked"), so the per-close cost is a
> handful of int compares — negligible. If WS-1 would rather not add a global `close`
> interposer, the alternative is eviction-on-reuse in `socket()` (see §5.2), accepting the
> small risk that a recycled fd number briefly looks tracked until its next `socket`/`send`.
> Recommended: add the `close` wrapper; it is the correct, leak-free design.

---

## 9. Recommendation summary

* **Default = Strategy B** (lo + one UP non-loopback iface `alr0` + one global IPv4
  `10.0.2.15/24`): it makes chromium see an online interface, which is what actually unblocks
  CR-2's real connect/DNS work. Byte layout is fully specified in §3.4–3.5 (no ambiguity).
* **Keep Strategy A** (bare DONE) behind `ALR_NL_EMU=1` as the conservative fallback if B
  ever destabilizes the tracker.
* **fd-tracking via a `socket()`-recorded 16-slot table**, captured request seq/type on
  `sendmsg`, freed on `close`. Synthesize only on a tracked fd when the real recvmsg would
  block/deny; never override a real reply.
* **No loader/supervisor/MainActivity change** — pure interposer addition next to the
  existing `bind()` workaround.

## 10. Open items the device drain must confirm (host cannot)
* Whether chromium with Strategy B proceeds to the AF_INET connect (expected) — if it still
  reports offline, check `ifa_scope`/global-address filtering and consider adding the IPv6
  `RTM_NEWADDR`.
* Whether chromium's headless `ReadMessages` truly ignores seq/pid mismatch (we echo them
  anyway, so this should be moot).
* That the 41 s `--version` flakiness disappears with the short-circuit (the core success
  criterion).
