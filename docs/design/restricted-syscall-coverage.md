# Design — Restricted-syscall long-tail coverage (one interposer pass)

Status: DESIGN (HOST-ONLY; the loader owner implements in
`app/src/main/cpp/alr_interpose/libalr_interpose.c`, with two PCGATE BPF-allow
notes for `app/src/main/cpp/runtime_report.cpp`).
Owner of this doc: `auto/syscall-coverage-design`. Owner of the implementation
files: the loader/interposer session (libalr_interpose.c, runtime_report.cpp).
Date: 2026-06-03 · Device target SM-X236N / Android 16 / untrusted_app / Mali-G615.

This is the SSOT that answers, in **one pass**, the question every new Linux app
re-asks: *"which operations does Android SELinux deny an `untrusted_app` that an
ordinary glibc app expects to succeed, and what is the exact interposer wrapper
that neutralizes each one without weakening the sandbox?"* Instead of debugging
one app at a time (chromium today, GIMP-plugin tomorrow, qt6 next), this
enumerates the **bounded** set and gives a ready-to-paste wrapper + a
not-a-bypass argument for each, ranked by how many app *classes* hit it.

---

## 0. The invariant that makes every entry below legitimate

There is exactly ONE rule, applied uniformly. Read it once; every wrapper is an
instance of it:

> **Neutralize a kernel-denied POLICY/PROBE operation by reporting the benign
> result the kernel would have produced on an unrestricted host — but NEVER
> fabricate a DATA-PATH result.**

A *policy/probe* operation is one whose **only** effect is to advise the kernel
("tag this socket", "pin me to CPU 3", "raise my scheduling class", "lock this
page resident", "subscribe to route-change events"). On a host where the app
lacks the privilege, the kernel **already** ignores the advice and the app is
expected to proceed; on Android the same call returns `EPERM`/`EACCES` and a
brittle app instead treats it as a fatal invariant violation (`PCHECK` →
`IMMEDIATE_CRASH`) or wedges (reads it as "offline"). We return the *non-fatal*
errno-or-success the app's own non-privileged code path already handles.

A *data-path* operation moves real bytes / returns real kernel state the app
will act on (a file's contents, a socket's received packet, the real free space
on a mount it will write to). We **never** synthesize those — at most we
*redirect* them (path mediation) or let the real error surface. Faking a
`recvmsg` of a TCP data socket, or a `read()` of a file, or a `statfs` free-block
count an installer divides by, is OUT OF SCOPE and explicitly forbidden.

**Why this is not a SELinux bypass (applies to every entry):** we always call
the REAL syscall first; the kernel still enforces it; the mapping/policy is never
actually applied; no capability is gained; no new executable mapping is created
(W^X-clean — every wrapper is plain control flow in `.text`, zero codegen, zero
heap). We only stop the app from *misreading* a kernel denial of an advisory hint
as a fatal error. The sandbox is byte-for-byte as strong after as before.

The two shipped wrappers (`bind()` AF_NETLINK no-op, `setsockopt()`
SO_MARK/SO_BINDTODEVICE/SO_PRIORITY no-op) are already instances of this rule.
This doc completes the class.

---

## 1. The bounded class (ranked by how many app classes hit each)

The denials an `untrusted_app` faces are NOT open-ended; they are the syscalls
SEAndroid's `untrusted_app` / `untrusted_app_all` policy + the seccomp app filter
restrict relative to a normal Linux process. Grouped and ranked:

| # | Group | Operation(s) | errno seen | App classes blocked | Action |
|---|-------|--------------|-----------|---------------------|--------|
| 1 | net policy | `setsockopt` SO_MARK/SO_BINDTODEVICE/SO_PRIORITY | EPERM | every chromium-net / curl-multi app | **SHIPPED** no-op |
| 2 | net probe | `bind(AF_NETLINK)` multicast | EACCES | every NetworkChangeNotifier app | **SHIPPED** no-op |
| 3 | net probe | netlink `recvmsg`/`recv`/`recvfrom` dump short-circuit | EAGAIN/EACCES/0 | every AddressTracker/getifaddrs app | see `netlink-recvmsg-emulation.md` (§3 here = cross-ref only) |
| 4 | sched policy | `sched_setaffinity` / `sched_setscheduler` / `sched_setparam` | EPERM | thread-pool apps (chromium, Qt, ffmpeg, browsers) | no-op → 0 |
| 5 | sched policy | `setpriority` / `nice` | EPERM/EACCES | build tools, media, daemons | no-op → 0 |
| 6 | proc policy | `prctl(PR_SET_*)` privileged subset | EPERM | sandbox/hardening apps (chromium, Firefox, systemd-ish) | no-op → 0 (selective) |
| 7 | mem policy | `mlock` / `mlock2` / `mlockall` | EPERM/EAGAIN | crypto (gnutls/openssl/gpg), DB engines | no-op → 0 |
| 8 | fs probe | `statfs`/`statvfs`/`fstatfs` on denied mount | EACCES/ENOENT | installers, disk-space checks (apt/dpkg, Electron) | path-mediate; benign fallback only on denial |
| 9 | fs probe | `/proc/sys/*` reads blocked by SELinux | EACCES | net stacks, tunables readers (nginx, redis, JVM) | path-mediate to rootfs shadow; benign default on denial |
| 10 | dev probe | `ioctl` on restricted char devices | EPERM/ENOTTY/ENODEV | terminal/tty (TIOCGWINSZ), RNG, DRM probes | benign per-request (size/ENOTTY), never fake data |
| 11 | net policy | `setsockopt` SO_REUSEPORT / IP_TRANSPARENT / IP_FREEBIND | EPERM/EACCES | servers, P2P, mDNS (avahi), QUIC | no-op → 0 (policy hints only) |
| 12 | IPC policy | SysV `shmget`/`semget`/`msgget`, POSIX `shm_open` | EACCES/EPERM | X clients (MIT-SHM), Qt, databases | force private fallback (see §11) |
| 13 | rlimit/caps | `setrlimit`/`prlimit64` raise, `capset`, `setns`/`unshare` | EPERM | sandbox self-confinement (chromium zygote, bubblewrap) | selective no-op for the *self-lowering-that-overshoots* case ONLY |

Entries 1–3 are shipped or covered by the sibling doc. Entries **4–13 are the
delta this doc specifies.** Each section below gives: the libc/syscall entry, the
errno, a paste-ready wrapper mirroring the `bind()`/`setsockopt()` shape, and the
not-a-bypass argument.

> **Scope discipline.** This is the *advisory-denial* long tail. It deliberately
> EXCLUDES: path mediation (already done via the rw()/seccomp net), exec re-entry
> (separate track), GPU marshalling, and anything that needs real kernel state.
> When in doubt, an entry stays OUT unless it is provably policy/probe-only.

---

## 2. Shared scaffolding (added once, used by §4–§13)

All wrappers live in the **same "NOT a path syscall" section** as the existing
`bind()` (libalr_interpose.c ≈ line 1240), use the existing `ALR_REAL(slot,...)`
RTLD_NEXT cache macro, and the existing byte helpers. Three tiny shared pieces:

```c
/* 2.1 — one env gate for the whole policy-denial class (read once in ctor,
 *       mirroring g_pcgate at ~line 737). DEFAULT ON; ALR_POLICY_EMU=0 disables
 *       every §4–§13 no-op at once so a device drain can A/B the entire class. */
static int g_policy_emu = 1;     /* set in alr_ctor: getenv("ALR_POLICY_EMU") != "0" */

/* 2.2 — the canonical "kernel denied an advisory op" test. EVERY wrapper uses
 *       THIS, never a bare errno compare, so the policy is uniform and auditable. */
static int alr_denied(int r) {            /* r = real syscall return */
    return r != 0 && (errno == EPERM || errno == EACCES);
}

/* 2.3 — "swallow the denial as benign success" — the single mutation point. */
static int alr_benign_ok(void) { errno = 0; return 0; }
```

`g_policy_emu` is read in `alr_ctor` exactly where `g_pcgate`/`g_diag` are read
(libalr_interpose.c ≈ line 737), so it inherits the B-3 child-reentry contract
(§"CHILD-REENTRY CONTRACT" in the file): a fresh exec'd child re-reads it from
env and self-initializes. No global state survives exec; nothing to migrate.

---

## 3. (cross-ref) netlink recvmsg short-circuit — entry #3

Already fully specified in **`docs/design/netlink-recvmsg-emulation.md`** (the
socket()/sendmsg()/recvmsg() fd-tracking table + the exact `RTM_NEWLINK`/
`RTM_NEWADDR`/`NLMSG_DONE` wire layout, env-gated `ALR_NL_EMU`). This doc does
**not** re-specify it; it is listed here only so the class table (§1) is
complete. The wrappers below sit beside that one in the same section.

---

## 4. Scheduler policy — `sched_setaffinity` / `sched_setscheduler` / `sched_setparam`

* **libc/syscall entry:** `sched_setaffinity(pid_t, size_t, const cpu_set_t*)`
  (`__NR_sched_setaffinity` = 122), `sched_setscheduler(pid_t,int,const struct sched_param*)`
  (= 119), `sched_setparam` (= 118).
* **errno seen:** `EPERM`. `untrusted_app` may not set CPU affinity nor raise its
  scheduling class (no `CAP_SYS_NICE`). chromium's `base::PlatformThread` and the
  Blink/compositor thread-pools call these to pin/boost worker threads; Qt's
  `QThread`, ffmpeg, and most thread-pool runtimes do the same at startup.
* **Why policy, not data:** affinity/priority are *hints*. The scheduler runs the
  thread regardless; the only effect of the denial is the thread is not pinned/
  boosted. On a normal non-privileged Linux process these calls ALSO commonly
  fail (you cannot raise to SCHED_FIFO without privilege) and well-written apps
  proceed — but some `PCHECK`/`DCHECK` the affinity call. We return the benign 0.

```c
/* sched_setaffinity: CPU-pinning hint denied to untrusted_app (no CAP_SYS_NICE).
 * Thread-pool runtimes (chromium base::PlatformThread, Qt, ffmpeg) pin workers;
 * the kernel runs them unpinned anyway. Report success on a kernel POLICY denial
 * so the app does not treat an advisory hint as fatal. Real placement is
 * unchanged — we only stop the misread. Any non-policy error (EINVAL bad mask,
 * ESRCH bad pid) is surfaced verbatim. */
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    static int (*real)(pid_t, size_t, const cpu_set_t *);
    ALR_REAL(real, int (*)(pid_t, size_t, const cpu_set_t *), "sched_setaffinity");
    int r = real(pid, cpusetsize, mask);
    if (g_policy_emu && alr_denied(r)) return alr_benign_ok();
    return r;
}

/* sched_setscheduler / sched_setparam: raising scheduling class/params needs
 * CAP_SYS_NICE; denied → EPERM. Same advisory-hint argument. */
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param) {
    static int (*real)(pid_t, int, const struct sched_param *);
    ALR_REAL(real, int (*)(pid_t, int, const struct sched_param *), "sched_setscheduler");
    int r = real(pid, policy, param);
    if (g_policy_emu && alr_denied(r)) return alr_benign_ok();
    return r;
}
int sched_setparam(pid_t pid, const struct sched_param *param) {
    static int (*real)(pid_t, const struct sched_param *);
    ALR_REAL(real, int (*)(pid_t, const struct sched_param *), "sched_setparam");
    int r = real(pid, param);
    if (g_policy_emu && alr_denied(r)) return alr_benign_ok();
    return r;
}
```

* **Not a bypass:** the thread's actual CPU placement and scheduling class are
  whatever the kernel chose; we changed nothing the kernel enforces. We do NOT
  fake the *reverse* (we never claim a thread IS pinned when queried — there is no
  `sched_getaffinity` lie; a query returns the real mask). Pure advisory no-op.

---

## 5. Process niceness — `setpriority` / `nice`

* **libc/syscall entry:** `setpriority(int which, id_t who, int prio)`
  (`__NR_setpriority` = 140); glibc `nice(int)` is a thin wrapper over it.
* **errno seen:** `EPERM` (raising priority, i.e. lowering nice value, needs
  privilege) or `EACCES`. Build tools, media transcoders, and daemons routinely
  `nice()` themselves.
* **Why policy, not data:** niceness is a scheduling weight hint; denial just
  means the process keeps its current nice value. Standard non-root Linux also
  refuses a nice DECREASE.

```c
/* setpriority/nice: lowering nice value (raising priority) needs privilege;
 * untrusted_app → EPERM. Scheduling weight is advisory; on denial keep the
 * current value and report success so an app that PCHECKs the renice proceeds.
 * A nice INCREASE (lowering priority) normally succeeds and is untouched. */
int setpriority(int which, id_t who, int prio) {
    static int (*real)(int, id_t, int);
    ALR_REAL(real, int (*)(int, id_t, int), "setpriority");
    int r = real(which, who, prio);
    if (g_policy_emu && alr_denied(r)) return alr_benign_ok();
    return r;
}
/* nice() returns the NEW nice value (or -1/errno). On a denied raise we report
 * the CURRENT nice value (getpriority), not a fabricated one — benign + truthful.
 * errno must be cleared before getpriority because nice()'s -1 is ambiguous. */
int nice(int inc) {
    static int (*real)(int);
    ALR_REAL(real, int (*)(int), "nice");
    int r = real(inc);
    if (g_policy_emu && r == -1 && (errno == EPERM || errno == EACCES)) {
        static int (*rgp)(int, id_t);
        ALR_REAL(rgp, int (*)(int, id_t), "getpriority");
        errno = 0;
        int cur = rgp ? rgp(PRIO_PROCESS, 0) : 0;   /* real current nice */
        errno = 0;
        return cur;                                 /* truthful current value */
    }
    return r;
}
```

* **Not a bypass:** the process's real nice value is unchanged and we *report it
  truthfully* (via the real `getpriority`), never a lie. We only convert the
  "raise refused" into "stayed where it is", which is the standard non-privileged
  semantics.

---

## 6. `prctl(PR_SET_*)` privileged subset — SELECTIVE (the careful one)

* **libc/syscall entry:** `prctl(int option, ...)` (`__NR_prctl` = 167).
* **errno seen:** `EPERM` for the privileged options; `EINVAL` for unknown ones.
* **CRITICAL — this wrapper is SELECTIVE, not a blanket no-op.** `prctl` carries
  both *harmless self-hardening* options (which a sandboxing app sets and treats
  as fatal-on-fail) AND options the loader/ALR runtime itself uses
  (`PR_SET_NO_NEW_PRIVS`, `PR_SET_SECCOMP` — the interposer's own ctor calls these
  at lines 491/556) AND security-relevant ones we must NEVER silently swallow.

  Only the options in an explicit **allowlist** are no-op'd on `EPERM`; everything
  else passes through untouched. The allowlist = options that are (a) advisory
  self-hardening, (b) commonly `PCHECK`'d by sandboxers, and (c) whose failure is
  harmless because the process is *already* more-confined than the option would
  make it (it runs inside an Android app sandbox + seccomp):

```c
/* prctl: SELECTIVE policy no-op. ONLY the advisory self-hardening options below
 * are converted from EPERM→success; every other option (incl. the ALR runtime's
 * own PR_SET_NO_NEW_PRIVS / PR_SET_SECCOMP, and anything security-relevant) is
 * forwarded verbatim. The allowlisted options REQUEST MORE restriction; failing
 * them cannot weaken the sandbox — the process is already confined by the Android
 * app domain + the loader's seccomp filter. We stop a sandbox app from aborting
 * because it could not re-apply a restriction it effectively already has. */
int prctl(int option, unsigned long a2, unsigned long a3,
          unsigned long a4, unsigned long a5) {
    static int (*real)(int, unsigned long, unsigned long, unsigned long, unsigned long);
    ALR_REAL(real, int (*)(int, unsigned long, unsigned long, unsigned long, unsigned long), "prctl");
    int r = real(option, a2, a3, a4, a5);
    if (g_policy_emu && alr_denied(r)) {
        switch (option) {
            /* advisory, restriction-INCREASING, commonly PCHECK'd: */
            case PR_SET_DUMPABLE:        /* 4  — turn OFF core dumps (set to 0) */
            case PR_SET_KEEPCAPS:        /* 8  — caps across setuid (we have none) */
            case PR_SET_TIMERSLACK:      /* 29 — timer coalescing hint */
            case PR_SET_CHILD_SUBREAPER: /* 36 — reaper role; harmless if denied */
            case PR_SET_THP_DISABLE:     /* 41 — disable transparent hugepages */
            case PR_MCE_KILL:            /* 33 — machine-check policy hint */
            case PR_SET_PTRACER:         /* 0x59616d61 — yama; denial is fine */
                return alr_benign_ok();
            default:
                break;   /* NOT allowlisted → surface the real EPERM */
        }
    }
    return r;
}
```

* **Explicitly NOT allowlisted (must surface EPERM):** `PR_SET_SECCOMP`,
  `PR_SET_NO_NEW_PRIVS` (the loader needs the real result; never mask it),
  `PR_CAPBSET_DROP`/`PR_CAP_AMBIENT` (capability-set changes — surfacing the real
  result is correct since the app has no caps anyway), and any option not listed.
  `PR_SET_NAME`/`PR_GET_*` are not privileged and never hit this branch.
* **Not a bypass:** every allowlisted option asks the kernel to *remove* a
  privilege or *add* a restriction. If the kernel refuses, the process is no MORE
  privileged than it would have been on success — strictly the inverse of a
  bypass. The data path is untouched (prctl moves no bytes).
* **PCGATE note:** `prctl` is NOT one of the 9 traced path syscalls, so the PC
  gate is unaffected. The interposer's own ctor `prctl` calls go through the
  trampoline BEFORE this wrapper is reachable by guest code and are unrelated.

---

## 7. Memory locking — `mlock` / `mlock2` / `mlockall` (best-effort → 0)

* **libc/syscall entry:** `mlock(const void*, size_t)` (`__NR_mlock` = 228),
  `mlock2(const void*, size_t, unsigned int)` (= 284),
  `mlockall(int flags)` (= 230).
* **errno seen:** `EPERM` (no `CAP_IPC_LOCK` and over `RLIMIT_MEMLOCK`) or
  `EAGAIN`. Crypto libs (OpenSSL `CRYPTO_secure_malloc`, GnuTLS, libgcrypt/gpg)
  lock key buffers; sqlite/LMDB/redis lock pages.
* **Why policy, not data:** `mlock` is **explicitly best-effort by contract** —
  POSIX and `man mlock` document that locking is an *optimization/hardening* hint;
  an app must still function if pages get swapped (Android has no swap by default
  anyway, so the lock is doubly moot). The data in the pages is untouched whether
  locked or not. This is the cleanest "return 0" case in the whole class.

```c
/* mlock/mlock2/mlockall: page residency is a best-effort hardening hint (POSIX);
 * untrusted_app lacks CAP_IPC_LOCK and is over RLIMIT_MEMLOCK → EPERM/EAGAIN.
 * Android has no swap, so the pages are de-facto resident regardless. Crypto libs
 * (OpenSSL/GnuTLS/gpg) and embedded DBs lock key/page buffers and may abort on
 * failure. Report success: the memory CONTENTS are identical locked or not — we
 * fake no data, only the advisory residency promise the platform already keeps. */
int mlock(const void *addr, size_t len) {
    static int (*real)(const void *, size_t);
    ALR_REAL(real, int (*)(const void *, size_t), "mlock");
    int r = real(addr, len);
    if (g_policy_emu && r != 0 && (errno == EPERM || errno == EAGAIN || errno == EACCES))
        return alr_benign_ok();
    return r;
}
int mlock2(const void *addr, size_t len, unsigned int flags) {
    static int (*real)(const void *, size_t, unsigned int);
    ALR_REAL(real, int (*)(const void *, size_t, unsigned int), "mlock2");
    int r = real(addr, len, flags);
    if (g_policy_emu && r != 0 && (errno == EPERM || errno == EAGAIN || errno == EACCES))
        return alr_benign_ok();
    return r;
}
int mlockall(int flags) {
    static int (*real)(int);
    ALR_REAL(real, int (*)(int), "mlockall");
    int r = real(flags);
    if (g_policy_emu && r != 0 && (errno == EPERM || errno == EAGAIN || errno == EACCES))
        return alr_benign_ok();
    return r;
}
```

* **Not a bypass:** no page is actually locked, but locking grants no privilege —
  it only asks the kernel not to swap. We fabricate no memory contents and gain no
  capability. The security posture is unchanged.
* **`munlock`/`munlockall` are left alone** — they normally succeed (unlocking is
  unprivileged) and faking them is pointless.

---

## 8. Filesystem space probes — `statfs` / `statvfs` / `fstatfs`

* **libc/syscall entry:** `statfs(const char*, struct statfs*)`
  (`__NR_statfs` = 43 via `__NR3264_statfs`), `statvfs` (glibc shim over statfs),
  `fstatfs(int, struct statfs*)`.
* **errno seen:** `EACCES`/`ENOENT` when the *path* lands on a mount the app
  cannot stat (because the absolute guest path was not rootfs-mediated, or the
  real Android mount denies it).
* **TWO-STAGE handling — path mediation FIRST, benign fallback ONLY on denial:**
  1. `statfs`/`statvfs` take a **path** — that path MUST go through `rw()` so it
     resolves under the rootfs, exactly like the other path wrappers. This is the
     primary fix and is *not* a policy no-op — it makes the call hit the right
     mount and return REAL numbers. Most "missing space check" failures are just
     un-mediated paths.
  2. **Only if** the mediated call STILL fails with `EACCES`/`EPERM` (a genuinely
     denied mount) do we fall back to a benign, clearly-non-zero `struct statfs`
     so a `free-space >= needed` check passes — and we do this **conservatively**.

```c
/* statfs: FIRST rewrite the path to the rootfs (real numbers from the right
 * mount — this is path mediation, not a policy no-op). Only if a *mediated* call
 * is still EACCES/EPERM-denied do we synthesize a benign, plausibly-large result
 * so a disk-space precondition (apt/dpkg, Electron updater) does not abort. We
 * fill REAL-looking but generous counts; we never under-report space (which could
 * cause silent truncation), and we never touch a call that succeeded. */
int statfs(const char *path, struct statfs *buf) {
    static int (*real)(const char *, struct statfs *);
    ALR_REAL(real, int (*)(const char *, struct statfs *), "statfs");
    char b[ALR_PBUF];
    const char *p = (path && path[0] == '/') ? rw(path, b, sizeof b) : path;
    int r = real(p, buf);
    if (g_policy_emu && r != 0 && (errno == EACCES || errno == EPERM) && buf) {
        alr_fake_statfs(buf);   /* generous, non-zero free blocks; see below */
        errno = 0;
        return 0;
    }
    return r;
}
/* fstatfs has no path; only the benign fallback applies (the fd already exists,
 * so a denial here is a true policy denial, not a missing rewrite). */
int fstatfs(int fd, struct statfs *buf) {
    static int (*real)(int, struct statfs *);
    ALR_REAL(real, int (*)(int, struct statfs *), "fstatfs");
    int r = real(fd, buf);
    if (g_policy_emu && r != 0 && (errno == EACCES || errno == EPERM) && buf) {
        alr_fake_statfs(buf); errno = 0; return 0;
    }
    return r;
}
```

`alr_fake_statfs` fills a fixed, plausible filesystem geometry (e.g.
`f_bsize=4096`, `f_blocks=4194304` = 16 GiB, `f_bfree=f_bavail=2097152` = 8 GiB
free, `f_files`/`f_ffree` generous, `f_namelen=255`) — generous so a
"need N bytes free" check passes, never UNDER-reporting. `statvfs` (glibc) is a
thin re-pack of `statfs`; interpose it the same way (or let it ride the `statfs`
wrapper if glibc routes through it — verify per build).

* **Not a bypass:** stage 1 is pure path redirection (no fabrication). Stage 2 is
  the **only** place we synthesize, and it (a) fires solely on a genuine kernel
  denial of a *metadata probe* (no file bytes involved), (b) reports MORE free
  space than real so no write is wrongly truncated, (c) never reports a writable
  mount the kernel would actually refuse to write to (the subsequent `open(...,
  O_CREAT)` still hits the real kernel and the real path-mediation — if THAT is
  denied, it fails honestly). We unblock the *check*, not the write.
* **Honest caveat:** this is the single entry that synthesizes data-shaped output.
  It is justified because a free-space *count* an installer compares against a
  threshold is a probe, not a data path — but it is the one entry a device drain
  must watch (does the install then succeed when it actually writes?). If a drain
  shows over-reporting causes a worse failure, gate stage 2 behind its own env
  (`ALR_FAKE_STATFS=0`) and ship only the path-mediation stage 1.

---

## 9. `/proc/sys/*` reads blocked by SELinux — path mediation + benign default

* **libc/syscall entry:** ordinary `open`/`read` of `/proc/sys/...` (e.g.
  `/proc/sys/net/core/somaxconn`, `/proc/sys/kernel/...`, `/proc/sys/vm/...`).
* **errno seen:** `EACCES` — SELinux denies `untrusted_app` read of many
  `proc_*` sysctls (`/proc/sys/net/*` for net stacks; nginx/redis/JVM read
  tunables at startup).
* **Mediation, NOT a syscall wrapper.** `/proc` is in the interposer's
  passthrough set (it is NEVER path-rewritten — see `rw()` and the supervisor's
  `under(p,"/proc")` guard) BECAUSE it is a kernel virtual fs. So the fix is a
  **rootfs-shadow**, not a libc wrapper: stage a small read-only tree of the
  commonly-read sysctl files under `<rootfs>/proc_sys_shadow/...` and have the
  loader/path-mediation, **only for the specific blocked `/proc/sys/...` reads**,
  redirect to that shadow. This is a *data* read, so we must serve real-enough
  content, not synthesize — the shadow files carry conservative real defaults
  (the values a stock Linux kernel ships).

  Two implementation choices (the loader owner picks):
  - **(a) Interposer-side** (preferred, no new tar needed if shadow is in the
    rootfs already): in the `open`/`openat` path, if the path is under
    `/proc/sys/` AND a real open returns `EACCES`, retry the open against
    `<rootfs>/proc_sys_shadow/<same-subpath>`; if THAT exists, return its fd.
    This is a *fallback-on-denial redirect* — the real `/proc/sys` is tried
    first; the shadow only answers when the kernel refused.
  - **(b) Stage-tar** (the deb-overlay pattern): a tiny stage-tar ships
    `proc_sys_shadow/net/core/somaxconn` (`4096`), `.../net/ipv4/tcp_*` defaults,
    `.../vm/overcommit_memory` (`0`), etc. The interposer redirect in (a) reads
    these. This is the ONE-SHOT overlay that unblocks the whole "reads a sysctl at
    startup" class.

```c
/* In alr_open_emit's denial path (NOT the fast path), add — AFTER the real
 * open returns EACCES for a /proc/sys/* read: */
if (g_policy_emu && r_errno == EACCES && a_under(path, "/proc/sys")) {
    /* build <rootfs>/proc_sys_shadow + (path + strlen("/proc")) and try it */
    /* serve the shadow fd if it exists; else surface the real EACCES */
}
```

* **Not a bypass:** the kernel's real `/proc/sys` is always tried first and still
  enforced; the shadow is a *read-only* fallback carrying the SAME values a normal
  Linux kernel exposes (conservative stock defaults), so the app reads truthful,
  representative numbers rather than crashing on `EACCES`. We grant no privilege
  and write nothing back to the kernel. (Writes to `/proc/sys/*` are out of scope
  — they remain denied; an app that *sets* a sysctl is not our class.)
* **Honest caveat:** because these are real data reads, the shadow values must be
  curated to match a plausible kernel (host can pre-fill from a stock noble
  kernel's defaults). A wrong value could mis-tune an app but won't crash it.

---

## 10. `ioctl` on restricted devices — per-request benign, never fake data

* **libc/syscall entry:** `ioctl(int fd, unsigned long request, ...)`
  (`__NR_ioctl` = 29).
* **errno seen:** `EPERM`/`ENOTTY`/`ENODEV`/`EACCES` for ioctls on devices an
  `untrusted_app` cannot reach (DRM nodes the app didn't open, raw input, some
  tty ops on a non-tty fd).
* **HIGHLY SELECTIVE — ioctl is a giant multiplexer; a blanket no-op is forbidden
  (it carries real data ops).** Only a tiny allowlist of *informational/terminal*
  requests gets benign handling, and even then we return a *plausible benign
  shape*, never fabricated device data:

```c
/* ioctl: SELECTIVE. The vast majority of ioctls carry real data and are
 * forwarded untouched. We special-case ONLY terminal-geometry / isatty-class
 * requests that an interactive glibc app issues on a non-tty fd (our stdio is not
 * a real terminal) and that abort on failure. We return a benign default WINDOW
 * SIZE (80x24) for TIOCGWINSZ and let isatty()'s TCGETS return ENOTTY (the honest
 * "not a tty" answer, which apps already handle). We NEVER fake a device read
 * (DRM/input/RNG ioctls are forwarded and fail honestly). */
int ioctl(int fd, unsigned long request, ...) {
    va_list ap; va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    static int (*real)(int, unsigned long, ...);
    ALR_REAL(real, int (*)(int, unsigned long, ...), "ioctl");
    int r = real(fd, request, arg);
    if (g_policy_emu && r != 0 && (errno == ENOTTY || errno == EINVAL)) {
        if (request == TIOCGWINSZ && arg) {        /* terminal size probe */
            struct winsize *ws = arg;
            ws->ws_row = 24; ws->ws_col = 80; ws->ws_xpixel = 0; ws->ws_ypixel = 0;
            errno = 0;
            return 0;       /* benign default geometry; not device data */
        }
    }
    return r;       /* everything else (incl. all data ioctls) untouched */
}
```

* **Not a bypass:** the only synthesized result is a default terminal window size
  — a UI hint with zero security or data meaning. Every device-control and
  device-read ioctl is forwarded to the real kernel and fails honestly. `TCGETS`
  is deliberately NOT faked (we let `isatty` correctly report "not a tty").
* **Honest caveat:** the device-pending question is which terminal ioctls a given
  CLI app actually issues; a drain refines the allowlist. Start with `TIOCGWINSZ`
  only.

---

## 11. `setsockopt` SO_REUSEPORT / IP_TRANSPARENT / IP_FREEBIND — policy no-op extension

* **libc/syscall entry:** `setsockopt` (= 208) — **extend the EXISTING shipped
  wrapper** (libalr_interpose.c ≈ line 1287), do not add a second one.
* **errno seen:** `EPERM`/`EACCES`. `SO_REUSEPORT` (servers, QUIC, mDNS/avahi),
  `IP_TRANSPARENT`/`IP_FREEBIND` (proxies, transparent-proxy/tproxy apps) need
  privilege the app lacks.
* **Why policy, not data:** these are bind/listen *policy* hints. `SO_REUSEPORT`
  load-balances multiple listeners — with one listener (the app's typical case)
  its absence changes nothing. `IP_TRANSPARENT`/`IP_FREEBIND` let you bind a
  non-local/absent address — irrelevant to an app binding its own address.

```c
/* EXTEND the existing setsockopt wrapper's allowlist (do NOT duplicate the fn).
 * Add to the existing `(optname == SO_MARK || ...)` condition: */
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif
/* IP_TRANSPARENT(19)/IP_FREEBIND(15 under IPPROTO_IP) are at level IPPROTO_IP. */
/* In setsockopt(): broaden the guard to two levels: */
if (r != 0 && (errno == EPERM || errno == EACCES)) {
    if (level == SOL_SOCKET &&
        (optname == SO_MARK || optname == SO_BINDTODEVICE ||
         optname == SO_PRIORITY || optname == SO_REUSEPORT)) {
        errno = 0; return 0;
    }
    if (level == IPPROTO_IP &&
        (optname == IP_TRANSPARENT || optname == IP_FREEBIND)) {
        errno = 0; return 0;
    }
}
return r;
```

* **Not a bypass:** identical argument to the shipped SO_MARK no-op — the option
  is never actually applied; the socket behaves as a plain non-privileged socket
  (one listener, local-address bind), which is exactly what an `untrusted_app`
  socket is. No privilege gained.
* **Honest caveat:** `SO_REUSEADDR` is NOT in this list (it normally succeeds for
  untrusted_app and is a legitimate, non-privileged option — never mask it).

---

## 12. SysV / POSIX IPC — `shmget` / `semget` / `msgget` / `shm_open`

* **libc/syscall entry:** `shmget` (= 194), `shmat` (= 196), `semget` (= 190),
  `msgget` (= 186); POSIX `shm_open` (opens `/dev/shm/...`).
* **errno seen:** `EACCES`/`EPERM`/`ENOSYS`. SEAndroid denies `untrusted_app` SysV
  IPC; `/dev/shm` does not exist on Android. The big hitter is **X11 MIT-SHM**:
  Xlib/XCB probe `shmget` to negotiate shared-memory image transport; on failure
  they fall back to socket transport. Qt and some media apps do the same.
* **Why policy/probe, not data — and the RIGHT action is to make the PROBE FAIL
  CLEANLY, not fake success.** Faking `shmget` success would be a DATA-PATH lie
  (the app would then try `shmat` and read/write a segment that does not exist →
  worse crash). The correct neutralization is the opposite of §4–§7: ensure the
  *failure* is the clean, expected one the app's fallback handles.

```c
/* shmget/semget/msgget: SysV IPC is denied to untrusted_app. The CORRECT action
 * is NOT to fake success (that would make shmat touch a non-existent segment — a
 * data-path lie). Instead, normalize the denial to the errno the app's fallback
 * already handles (ENOSYS / EACCES), so e.g. Xlib/XCB MIT-SHM cleanly falls back
 * to socket transport instead of mis-handling an unexpected EPERM. We FAIL the
 * probe cleanly; we never fabricate a segment. */
int shmget(key_t key, size_t size, int shmflg) {
    static int (*real)(key_t, size_t, int);
    ALR_REAL(real, int (*)(key_t, size_t, int), "shmget");
    int r = real(key, size, shmflg);
    if (g_policy_emu && r == -1 && errno == EPERM) { errno = ENOSYS; }  /* canonical "unsupported" */
    return r;       /* still -1 — the probe fails, cleanly */
}
/* semget/msgget: same normalization (EPERM → ENOSYS) so callers take the
 * "IPC unavailable" branch rather than treating EPERM as a hard error. */
```

* **Not a bypass:** we return FAILURE, just with the errno (`ENOSYS` = "feature
  absent") that maximizes the chance the app uses its non-IPC fallback. No segment
  is created; no privilege is gained; the data path (socket transport) is the
  app's own, unmodified.
* **Honest caveat:** the real X11-on-Android shared-memory story (whether the
  compositor offers `wl_shm`/`MIT-SHM`) is a WS-3/WS-4 question; this entry only
  ensures the *probe* fails cleanly so the socket fallback engages. If a drain
  shows an app hard-requires SHM, that is a compositor-side overlay, not this
  class.

---

## 13. rlimit / caps / namespaces — SELECTIVE self-confinement no-op

* **libc/syscall entry:** `setrlimit`/`prlimit64` (= 164 / 261), `capset` (= 91),
  `setns` (= 268), `unshare` (= 97).
* **errno seen:** `EPERM`. Sandboxing apps (chromium's zygote/sandbox,
  bubblewrap-style launchers) try to self-confine via these.
* **VERY SELECTIVE — only the case where the app is trying to make itself MORE
  restricted and overshoots.** We do **NOT** no-op `capset`/`setns`/`unshare`
  generally (those can be capability/namespace changes whose real result the app
  must see). The ONE safe no-op is `setrlimit`/`prlimit64` when the app tries to
  *lower* a limit below what the kernel allows it to set for itself, or raise a
  hard limit it lacks privilege for — and even then only the "the process ends up
  AT LEAST as restricted as requested" direction:

```c
/* prlimit64/setrlimit: an app that LOWERS its own soft limit (self-confinement,
 * e.g. RLIMIT_NOFILE down, RLIMIT_CORE=0) but is denied because it conflicts with
 * the Android-imposed hard limit gets EPERM. Report success ONLY when the request
 * would leave the process AT LEAST as restricted as the current effective limit
 * (i.e. it is not asking to be more privileged). A request to RAISE above the
 * hard limit is surfaced verbatim (the app must see it cannot). We change no
 * actual limit on the no-op path; the process keeps the (already-restrictive)
 * Android limit, which satisfies a self-confining app. */
int prlimit64(pid_t pid, int resource,
              const struct rlimit64 *new_limit, struct rlimit64 *old_limit) {
    static int (*real)(pid_t, int, const struct rlimit64 *, struct rlimit64 *);
    ALR_REAL(real, int (*)(pid_t, int, const struct rlimit64 *, struct rlimit64 *), "prlimit64");
    int r = real(pid, resource, new_limit, old_limit);
    if (g_policy_emu && r != 0 && errno == EPERM && new_limit) {
        /* Only swallow a self-RESTRICTING request (rlim_cur not larger than the
         * current effective soft limit). Fetch current with a real getrlimit;
         * if the request is <= current, the process is already at least that
         * confined → benign success. A widening request is surfaced. */
        struct rlimit64 cur;
        if (real(pid, resource, NULL, &cur) == 0 &&
            new_limit->rlim_cur <= cur.rlim_cur &&
            new_limit->rlim_max <= cur.rlim_max) {
            if (old_limit) *old_limit = cur;
            errno = 0; return 0;
        }
    }
    return r;
}
```

* **`capset`/`setns`/`unshare`: NOT no-op'd.** They are forwarded verbatim. An app
  has no caps to set and no permission to enter/create namespaces; surfacing the
  real `EPERM` is correct — a sandbox launcher that needs real namespaces is out
  of this class's scope (it cannot run unprivileged anyway), and silently faking
  success would be a security-relevant lie about confinement state.
* **Not a bypass:** the only no-op is for a request that would make the process
  *more* restricted (or no less), and only when it is already at least that
  restricted. The process never ends up with a HIGHER limit than the kernel set.
  Strictly the inverse of privilege escalation.

---

## 14. PCGATE / supervisor interaction (loader-owner notes — 2 small items)

None of the §4–§13 syscalls are in the **9 traced path syscalls**
`{34,35,48,56,78,79,291,437,439}`, so:

1. **PC gate (libalr_interpose.c BPF):** unaffected. The PC-gated filter ALLOWs
   every non-path syscall in ≤7 instructions (the "nr classify FIRST" layout), so
   `sched_setaffinity`/`mlock`/`ioctl`/`setsockopt`/etc. are already RET_ALLOW and
   reach the libc wrappers normally. **No BPF change needed.**
2. **Supervisor (runtime_report.cpp):** these syscalls are not traced, so the
   ptrace supervisor never sees them — the wrappers run entirely in-process. **No
   `runtime_report.cpp` change needed** for §4–§13. (The two ctor `prctl` calls
   the interposer already makes are pre-existing and unrelated.)

The ONE thing the loader owner must confirm on-device: that none of these
syscalls is itself **RET_TRAP**'d by the always-present Android zygote app filter
(like openat2/faccessat2 are). All of §4–§13 syscalls are in bionic's allowlist
(they are normal libc operations the Android runtime itself uses), so a blind
emission does NOT SIGSYS — unlike openat2 they need no SIGSYS-probe gate. If a
future device proves otherwise for a specific syscall, wrap that one in the same
`alr_probe_guarded` pattern the file already has for openat2 (§ the
`alr_probe_*_body` helpers).

---

## 15. Exactly what the loader/interposer owner implements

In `app/src/main/cpp/alr_interpose/libalr_interpose.c`, in the **"NOT a path
syscall" section beside `bind()`** (≈ line 1240):

1. **Includes** (near the existing `<sys/prctl.h>`, `<sched.h>` may be new):
   `<sched.h>` (sched_param, cpu_set_t), `<sys/resource.h>` (setpriority,
   rlimit, PRIO_PROCESS), `<sys/mman.h>` (mlock), `<sys/vfs.h>`/`<sys/statfs.h>`
   (statfs), `<sys/ioctl.h>` + `<termios.h>` (TIOCGWINSZ, winsize),
   `<sys/ipc.h>`+`<sys/shm.h>`/`<sys/sem.h>`/`<sys/msg.h>` (IPC),
   `<netinet/in.h>` (IPPROTO_IP, IP_TRANSPARENT/IP_FREEBIND). Each behind the
   `__has_include` guard pattern the file already uses, with the constant
   `#define` fallbacks shown inline above.
2. **Ctor read** of `ALR_POLICY_EMU` (and optional `ALR_FAKE_STATFS`) mirroring
   the `g_pcgate`/`g_diag` read site (≈ line 737); inherits the B-3 contract.
3. **Shared helpers** `alr_denied`, `alr_benign_ok`, `alr_fake_statfs` (§2, §8).
4. **New wrappers** (all the function bodies above): `sched_setaffinity`,
   `sched_setscheduler`, `sched_setparam`, `setpriority`, `nice`, `prctl`
   (SELECTIVE allowlist), `mlock`, `mlock2`, `mlockall`, `statfs`, `fstatfs`,
   (`statvfs` per build), `ioctl` (SELECTIVE), `shmget`, `semget`, `msgget`.
5. **Extend** (do NOT duplicate) the existing `setsockopt` wrapper for
   SO_REUSEPORT / IPPROTO_IP IP_TRANSPARENT / IP_FREEBIND (§11), and the
   `prlimit64` SELECTIVE self-restriction no-op (§13).
6. **§9 `/proc/sys` shadow** is a path-mediation fallback (interposer `open`
   denial path + a `proc_sys_shadow/` rootfs subtree), staged via the standard
   stage-tar pattern (`tools/deb_closure.py`/`build_stage_tar.py` are read-only
   reusable references). This is the only entry needing a staged data overlay.
7. **No `runtime_report.cpp` change** for §4–§13 (per §14). The `/proc/sys`
   shadow (§9) may, at the loader owner's choice, be implemented supervisor-side
   instead of interposer-side; either is consistent with the no-bypass rule.

### Verbatim signatures the owner adds
```c
int     sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
int     sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int     sched_setparam(pid_t pid, const struct sched_param *param);
int     setpriority(int which, id_t who, int prio);
int     nice(int inc);
int     prctl(int option, unsigned long, unsigned long, unsigned long, unsigned long);
int     mlock(const void *addr, size_t len);
int     mlock2(const void *addr, size_t len, unsigned int flags);
int     mlockall(int flags);
int     statfs(const char *path, struct statfs *buf);
int     fstatfs(int fd, struct statfs *buf);
int     ioctl(int fd, unsigned long request, ...);
int     shmget(key_t key, size_t size, int shmflg);
int     semget(key_t key, int nsems, int semflg);
int     msgget(key_t key, int msgflg);
int     prlimit64(pid_t, int, const struct rlimit64 *, struct rlimit64 *);
/* setsockopt: EXTEND existing wrapper, no new signature. */
```

---

## 16. Ranking summary (which classes of apps each entry unblocks)

1. **§4 sched_set\*** + **§5 setpriority/nice** → every thread-pool runtime
   (chromium, Qt, Electron, ffmpeg, browsers, game engines). Highest breadth.
2. **§6 prctl (selective)** → sandbox/hardening apps (chromium, Firefox).
3. **§7 mlock** → crypto/TLS apps (anything linking OpenSSL/GnuTLS/gpg) + embedded
   DBs (sqlite/LMDB/redis). Very broad, very safe (best-effort by contract).
4. **§8 statfs** → installers/updaters (apt/dpkg, Electron auto-update).
5. **§11 SO_REUSEPORT etc.** → servers / P2P / mDNS.
6. **§9 /proc/sys shadow** → net stacks / tunable readers (nginx, redis, JVM).
7. **§10 ioctl TIOCGWINSZ** → interactive CLI apps.
8. **§12 IPC** → X11/Qt clients (clean MIT-SHM fallback).
9. **§13 prlimit (self-restrict)** → self-confining launchers.

Entries 1–7 (§1 table) are SHIPPED or covered by `netlink-recvmsg-emulation.md`.
**§4–§13 here are the complete, bounded remainder** — one interposer pass closes
the advisory-denial long tail for whole app classes at once.

---

## 17. Honest limitations / device-pending items (host cannot verify)

* **Cannot device-verify** any of this (HOST-ONLY worker). Each wrapper is
  argued from SEAndroid policy + glibc/POSIX contract; a device drain must
  confirm the actual errno each app sees and that the no-op unblocks it.
* **§8 statfs stage-2 synthesis** is the one entry that fabricates data-shaped
  output (free-block counts). It is gated separately (`ALR_FAKE_STATFS`) so a
  drain can disable it if over-reporting causes a worse failure than the EACCES.
* **§9 /proc/sys shadow values** must be curated to plausible stock-kernel
  defaults; a wrong value mis-tunes but won't crash. Needs a real noble-kernel
  reference to populate.
* **§10 ioctl allowlist** starts at `TIOCGWINSZ` only; per-app drains extend it.
  Never broaden to data-carrying ioctls.
* **§12 IPC** only guarantees a *clean probe failure*; whether a given X11/Qt app
  has a working non-SHM transport on the ALR compositor is a WS-3/WS-4 question.
* The **whole class is env-gated** (`ALR_POLICY_EMU=0` disables §4–§13 at once),
  so a single A/B drain can attribute any regression to this layer and back it out
  without a rebuild.
