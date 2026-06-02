"""Host model + unit tests for ADR-002 §3 (P2): the USER_NOTIF two-filter stack.

This file does NOT execute any seccomp BPF or any real syscall (the host is
darwin/arm64 and the filters target an on-device aarch64 guest; no kernel
NEW_LISTENER / SECCOMP_RET_USER_NOTIF is reachable here). Instead it models, as
pure logic, two things ADR-002 §2(2b)/§3(P2) need decided *before* the device
probe is even attempted:

  (A) When TWO seccomp filters are STACKED on a thread, which one's action
      wins per syscall? The kernel evaluates *every* installed filter and takes
      the action with the HIGHEST PRECEDENCE, which after masking with
      SECCOMP_RET_ACTION_FULL (0xffff0000) is the *numerically smallest*
      action value (man seccomp(2): "If multiple filters exist, they are all
      run, ... the first one that returns a value other than ALLOW ... actually
      the highest-precedence == smallest action value wins"). We model the two
      ALR filters and assert that path syscalls stay on the ptrace channel
      (RET_TRACE) while the hot non-path whitelist is absorbed by the notif
      channel (RET_USER_NOTIF) — i.e. the two channels coexist *purely by
      kernel precedence*, with no filter able to clobber the other's lane.

  (B) The seccomp(SET_MODE_FILTER, flags, ...) install-time branch table:
      TSYNC alone -> ok (no fd); NEW_LISTENER alone -> returns a listener fd;
      TSYNC|NEW_LISTENER -> EINVAL (the v5.7 conflict, kernel commit 51891498);
      TSYNC_ESRCH|NEW_LISTENER -> fd (the post-5.7 resolution). Modeled as a
      pure function, NOT a real syscall.

Why precedence == smallest-masked-value:
  SECCOMP_RET_ACTION_FULL is 0xffff0000. The kernel keeps, across all filters,
  the return whose masked value is smallest *as an unsigned compare*, EXCEPT
  that SECCOMP_RET_KILL_PROCESS (0x80000000) is special-cased to always win.
  Our two filters never emit KILL, so a plain unsigned-min over the masked
  actions reproduces the kernel's choice exactly. (KILL_THREAD == 0x0 would be
  the unsigned-min if present; we include it in the precedence test to prove the
  ordering, but neither ALR filter ever returns it.)

Pure-python, no external deps. Run with:
  uvx --with pytest pytest tests/test_user_notif_bpf_logic.py -q
"""

import pytest


# --------------------------------------------------------------------------- #
# SECCOMP_RET_* action values (uapi/linux/seccomp.h), masked form.
# These are the SECCOMP_RET_ACTION_FULL (0xffff0000) action bits; the data bits
# (low 16) are not part of precedence and are ignored here.
# --------------------------------------------------------------------------- #

SECCOMP_RET_ACTION_FULL = 0xFFFF0000

RET_KILL_PROCESS = 0x80000000
RET_KILL_THREAD  = 0x00000000   # historical SECCOMP_RET_KILL
RET_TRAP         = 0x00030000
RET_ERRNO        = 0x00050000
RET_USER_NOTIF   = 0x7FC00000
RET_TRACE        = 0x7FF00000
RET_LOG          = 0x7FFC0000
RET_ALLOW        = 0x7FFF0000

# Human-readable names for assertion messages / table dumps.
ACTION_NAME = {
    RET_KILL_PROCESS: "KILL_PROCESS",
    RET_KILL_THREAD:  "KILL_THREAD",
    RET_TRAP:         "TRAP",
    RET_ERRNO:        "ERRNO",
    RET_USER_NOTIF:   "USER_NOTIF",
    RET_TRACE:        "TRACE",
    RET_LOG:          "LOG",
    RET_ALLOW:        "ALLOW",
}

AUDIT_ARCH_AARCH64 = 0xC00000B7  # EM_AARCH64 | __AUDIT_ARCH_64BIT | LE


# --------------------------------------------------------------------------- #
# Syscall sets (aarch64 NR, stable kernel ABI).
# --------------------------------------------------------------------------- #

# The nine path-taking syscalls F_pcgate routes through ptrace path mediation.
PATH_SYSCALLS = {
    34:  "mkdirat",
    35:  "unlinkat",
    48:  "faccessat",
    56:  "openat",
    78:  "readlinkat",
    79:  "newfstatat",
    291: "statx",
    437: "openat2",
    439: "faccessat2",
}

# The hot NON-path syscalls F_notif whitelists for USER_NOTIF absorption.
# (ADR-002 §3 P2 example set; none overlaps PATH_SYSCALLS.)
NOTIF_WHITELIST = {
    98:  "futex",
    73:  "ppoll",
    63:  "read",
    64:  "write",
    222: "mmap",
    226: "mprotect",
    135: "rt_sigprocmask",
    113: "clock_gettime",
}

# A few hot non-path syscalls that are NOT whitelisted -> must stay ALLOW.
OTHER_NON_PATH = {
    94:  "exit_group",
    96:  "set_tid_address",
    260: "wait4",
    221: "execve",      # interposer does not gate execve; loader does (separate)
    281: "execveat",
}


# --------------------------------------------------------------------------- #
# Filter models (each returns one masked SECCOMP_RET_* action for one syscall).
# --------------------------------------------------------------------------- #

def f_pcgate(nr, arch=AUDIT_ARCH_AARCH64):
    """F_pcgate (current PC-gate path filter), modeled at the *un-gated* PC.

    foreign arch        -> ALLOW
    nr in PATH_SYSCALLS -> TRACE
    else                -> ALLOW

    (The PC-gate's trampoline-IP ALLOW arm is a fast path for already-rewritten
    path syscalls; for the stacking-precedence question that matters here we
    model the conservative un-gated case where a path syscall reaches TRACE,
    because that is the only case where F_pcgate's action could compete with
    F_notif's. A trampoline-gated path syscall is ALLOW in F_pcgate and is
    likewise ALLOW or absorbed by F_notif, never producing a different lane.)
    """
    if arch != AUDIT_ARCH_AARCH64:
        return RET_ALLOW
    if nr in PATH_SYSCALLS:
        return RET_TRACE
    return RET_ALLOW


def f_notif(nr, arch=AUDIT_ARCH_AARCH64):
    """F_notif (new NEW_LISTENER filter): whitelist hot non-path -> USER_NOTIF.

    foreign arch            -> ALLOW
    nr in NOTIF_WHITELIST   -> USER_NOTIF   (and these are all NON-path)
    else (incl. path nr)    -> ALLOW

    Crucially F_notif NEVER returns USER_NOTIF for a path syscall: the path nr
    set and the whitelist are disjoint, so path syscalls fall to ALLOW here and
    are left entirely to F_pcgate's TRACE lane.
    """
    if arch != AUDIT_ARCH_AARCH64:
        return RET_ALLOW
    if nr in NOTIF_WHITELIST:
        return RET_USER_NOTIF
    return RET_ALLOW


def kernel_combined_action(actions):
    """Model the kernel's multi-filter combination.

    The kernel runs all stacked filters and keeps the highest-precedence
    action == the numerically smallest masked action value (unsigned), with
    KILL_PROCESS special-cased to win outright. Our filters never emit KILL,
    but we honor the special case for completeness / the precedence test.
    """
    masked = [a & SECCOMP_RET_ACTION_FULL for a in actions]
    if RET_KILL_PROCESS in masked:
        return RET_KILL_PROCESS
    return min(masked)  # unsigned min == highest precedence among non-KILL


def stacked_decision(nr, arch=AUDIT_ARCH_AARCH64):
    """The action the kernel applies with BOTH ALR filters installed."""
    return kernel_combined_action([f_pcgate(nr, arch), f_notif(nr, arch)])


# --------------------------------------------------------------------------- #
# Disjointness sanity: path set and whitelist must not overlap, or a path
# syscall could be mis-routed to USER_NOTIF (TOCTOU-unsafe). This is a hard
# design invariant, asserted first.
# --------------------------------------------------------------------------- #

def test_path_and_whitelist_are_disjoint():
    assert PATH_SYSCALLS.keys().isdisjoint(NOTIF_WHITELIST.keys()), (
        "a path syscall is in the USER_NOTIF whitelist — would route a path "
        "syscall through CONTINUE (TOCTOU-unsafe). Must never happen."
    )
    # And the 'other non-path' control set is disjoint from both lanes.
    assert OTHER_NON_PATH.keys().isdisjoint(PATH_SYSCALLS.keys())
    assert OTHER_NON_PATH.keys().isdisjoint(NOTIF_WHITELIST.keys())


# --------------------------------------------------------------------------- #
# Precedence ordering: assert the exact kernel ordering on the masked values.
# Smallest masked value == highest precedence (KILL_PROCESS special-cased).
# --------------------------------------------------------------------------- #

def test_action_precedence_is_unsigned_min_excluding_kill_process():
    # The non-KILL_PROCESS actions, ordered from highest to lowest precedence,
    # must be strictly increasing in masked value (so unsigned-min picks them
    # in this order). KILL_THREAD (0x0) is the strongest non-KILL_PROCESS.
    order = [
        RET_KILL_THREAD,   # 0x00000000
        RET_TRAP,          # 0x00030000
        RET_ERRNO,         # 0x00050000
        RET_USER_NOTIF,    # 0x7fc00000
        RET_TRACE,         # 0x7ff00000
        RET_LOG,           # 0x7ffc0000
        RET_ALLOW,         # 0x7fff0000
    ]
    for a, b in zip(order, order[1:]):
        assert (a & SECCOMP_RET_ACTION_FULL) < (b & SECCOMP_RET_ACTION_FULL), (
            f"{ACTION_NAME[a]} should out-precede {ACTION_NAME[b]}"
        )
    # The two lane actions we care about: USER_NOTIF out-precedes TRACE which
    # out-precedes ALLOW. This is the chain the whole coexistence rests on.
    assert RET_USER_NOTIF < RET_TRACE < RET_ALLOW


def test_kill_process_special_cased_wins_over_smaller_values():
    # KILL_PROCESS (0x80000000) is numerically the LARGEST masked value but the
    # kernel makes it win outright. Our combiner honors that even though ALR
    # never emits it. (Pure precedence guard; not exercised by the ALR filters.)
    assert kernel_combined_action([RET_KILL_PROCESS, RET_ALLOW]) == RET_KILL_PROCESS
    assert kernel_combined_action([RET_ALLOW, RET_KILL_PROCESS]) == RET_KILL_PROCESS
    # Without KILL_PROCESS, plain unsigned-min applies.
    assert kernel_combined_action([RET_TRACE, RET_ALLOW]) == RET_TRACE
    assert kernel_combined_action([RET_USER_NOTIF, RET_TRACE]) == RET_USER_NOTIF


# --------------------------------------------------------------------------- #
# THE CORE THEOREM (0-mismatch): with both filters stacked, every syscall lands
# in the *intended* lane. Build the full decision table and compare to the
# spec'd expected lane for every nr we model.
# --------------------------------------------------------------------------- #

def expected_lane(nr, arch=AUDIT_ARCH_AARCH64):
    """The lane ADR-002 §3(P2) says each syscall MUST end up in."""
    if arch != AUDIT_ARCH_AARCH64:
        return RET_ALLOW                      # foreign arch -> ALLOW (both filters)
    if nr in PATH_SYSCALLS:
        return RET_TRACE                      # ptrace lane stays with F_pcgate
    if nr in NOTIF_WHITELIST:
        return RET_USER_NOTIF                 # absorbed by F_notif
    return RET_ALLOW                          # everything else


@pytest.mark.parametrize(
    "nr",
    sorted(set(PATH_SYSCALLS) | set(NOTIF_WHITELIST) | set(OTHER_NON_PATH)),
)
def test_stacked_decision_matches_expected_lane_zero_mismatch(nr):
    got = stacked_decision(nr)
    want = expected_lane(nr)
    assert got == want, (
        f"nr={nr} ({_name(nr)}): stacked kernel action "
        f"{ACTION_NAME[got]} != expected {ACTION_NAME[want]}"
    )


def test_full_decision_table_zero_mismatch_aggregate():
    """One aggregate pass over the whole table — assert ZERO mismatches and
    that each lane is actually populated (no lane silently empty)."""
    lanes = {RET_TRACE: 0, RET_USER_NOTIF: 0, RET_ALLOW: 0}
    mismatches = []
    all_nrs = set(PATH_SYSCALLS) | set(NOTIF_WHITELIST) | set(OTHER_NON_PATH)
    for nr in all_nrs:
        got = stacked_decision(nr)
        want = expected_lane(nr)
        if got != want:
            mismatches.append((nr, ACTION_NAME[got], ACTION_NAME[want]))
        lanes[got] = lanes.get(got, 0) + 1
    assert mismatches == [], f"lane mismatches: {mismatches}"
    # Each lane is non-empty: ptrace keeps all 9 paths, notif absorbs all 8
    # whitelist entries, allow holds the rest.
    assert lanes[RET_TRACE] == len(PATH_SYSCALLS)
    assert lanes[RET_USER_NOTIF] == len(NOTIF_WHITELIST)
    assert lanes[RET_ALLOW] == len(OTHER_NON_PATH)


# --------------------------------------------------------------------------- #
# Lane-by-lane explanations (the precedence arithmetic stated operationally).
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("nr", sorted(PATH_SYSCALLS))
def test_path_syscall_trace_beats_notif_allow(nr):
    # F_pcgate says TRACE, F_notif says ALLOW (path nr not whitelisted).
    assert f_pcgate(nr) == RET_TRACE
    assert f_notif(nr) == RET_ALLOW
    # TRACE (0x7ff00000) < ALLOW (0x7fff0000) -> TRACE wins -> ptrace retained.
    assert stacked_decision(nr) == RET_TRACE


@pytest.mark.parametrize("nr", sorted(NOTIF_WHITELIST))
def test_whitelisted_non_path_user_notif_beats_pcgate_allow(nr):
    # F_notif says USER_NOTIF, F_pcgate says ALLOW (non-path).
    assert f_notif(nr) == RET_USER_NOTIF
    assert f_pcgate(nr) == RET_ALLOW
    # USER_NOTIF (0x7fc00000) < ALLOW (0x7fff0000) -> USER_NOTIF wins -> absorbed.
    assert stacked_decision(nr) == RET_USER_NOTIF


@pytest.mark.parametrize("nr", sorted(OTHER_NON_PATH))
def test_other_non_path_stays_allow(nr):
    # Neither filter claims it; both ALLOW -> ALLOW.
    assert f_pcgate(nr) == RET_ALLOW
    assert f_notif(nr) == RET_ALLOW
    assert stacked_decision(nr) == RET_ALLOW


# --------------------------------------------------------------------------- #
# Foreign arch: BOTH filters short-circuit to ALLOW before inspecting nr, so the
# combined action is ALLOW for *every* syscall, including path and whitelist.
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize(
    "nr",
    sorted(set(PATH_SYSCALLS) | set(NOTIF_WHITELIST) | set(OTHER_NON_PATH)),
)
def test_foreign_arch_always_allow_both_lanes(nr):
    foreign = 0x40000028  # AUDIT_ARCH_ARM (32-bit) — not our guest arch
    assert f_pcgate(nr, arch=foreign) == RET_ALLOW
    assert f_notif(nr, arch=foreign) == RET_ALLOW
    assert stacked_decision(nr, arch=foreign) == RET_ALLOW
    assert expected_lane(nr, arch=foreign) == RET_ALLOW


# --------------------------------------------------------------------------- #
# Channel-independence invariant: stacking F_notif must NOT change ANY decision
# F_pcgate made on its own lane, and must NOT pull any path syscall out of TRACE.
# This is what lets the notif channel be added without touching the ptrace one.
# --------------------------------------------------------------------------- #

def test_adding_notif_filter_never_disturbs_path_trace_lane():
    all_nrs = set(PATH_SYSCALLS) | set(NOTIF_WHITELIST) | set(OTHER_NON_PATH)
    for nr in all_nrs:
        before = f_pcgate(nr)                 # F_pcgate alone
        after = stacked_decision(nr)          # F_pcgate + F_notif
        if before == RET_TRACE:
            # A path syscall traced by F_pcgate is STILL traced after stacking.
            assert after == RET_TRACE, (
                f"nr={nr} ({_name(nr)}) left the ptrace lane after stacking "
                f"F_notif: {ACTION_NAME[after]}"
            )
        # F_notif may only *tighten* an ALLOW into USER_NOTIF, never loosen.
        assert (after & SECCOMP_RET_ACTION_FULL) <= (before & SECCOMP_RET_ACTION_FULL)


# --------------------------------------------------------------------------- #
# (B) Install-time branch decision table: seccomp(SET_MODE_FILTER, flags, ...)
# TSYNC vs NEW_LISTENER vs the v5.7 TSYNC_ESRCH resolution. Pure logic, no
# real syscall. Mirrors kernel/seccomp.c flag validation (man seccomp(2)).
# --------------------------------------------------------------------------- #

SECCOMP_FILTER_FLAG_TSYNC        = 1 << 0
SECCOMP_FILTER_FLAG_LOG          = 1 << 1
SECCOMP_FILTER_FLAG_SPEC_ALLOW   = 1 << 2
SECCOMP_FILTER_FLAG_NEW_LISTENER = 1 << 3
SECCOMP_FILTER_FLAG_TSYNC_ESRCH  = 1 << 4

# All flags this kernel model recognizes; an unknown bit -> EINVAL.
_KNOWN_FLAGS = (
    SECCOMP_FILTER_FLAG_TSYNC
    | SECCOMP_FILTER_FLAG_LOG
    | SECCOMP_FILTER_FLAG_SPEC_ALLOW
    | SECCOMP_FILTER_FLAG_NEW_LISTENER
    | SECCOMP_FILTER_FLAG_TSYNC_ESRCH
)


class SeccompInstallResult:
    """Outcome of a modeled seccomp(SET_MODE_FILTER, flags, ...) call."""

    def __init__(self, ok, returns_fd=False, errno=None):
        self.ok = ok
        self.returns_fd = returns_fd      # True => return value is a listener fd
        self.errno = errno                # e.g. "EINVAL" on rejection

    def __repr__(self):
        if not self.ok:
            return f"<install errno={self.errno}>"
        return f"<install ok fd={self.returns_fd}>"


def seccomp_install(flags, *, kernel_has_tsync_esrch=True):
    """Model the kernel's flag validation for SECCOMP_SET_MODE_FILTER.

    Rules (man seccomp(2), kernel/seccomp.c, commit 51891498f039 for 5.7):
      - Unknown flag bit                                   -> EINVAL.
      - NEW_LISTENER together with TSYNC (plain)           -> EINVAL
        (a listener fd cannot be meaningfully returned when TSYNC fans the
         filter across all threads and returns the thread id on partial fail).
      - NEW_LISTENER together with TSYNC_ESRCH             -> ok, returns fd
        (5.7 resolution: ESRCH variant makes the pairing well-defined).
        On a pre-5.7 kernel TSYNC_ESRCH is unknown -> EINVAL.
      - NEW_LISTENER (with or without TSYNC_ESRCH)         -> ok, returns fd.
      - otherwise (incl. TSYNC alone, LOG, 0)              -> ok, no fd.
    """
    # TSYNC_ESRCH only exists on >= 5.7; older kernels reject the bit.
    known = _KNOWN_FLAGS
    if not kernel_has_tsync_esrch:
        known &= ~SECCOMP_FILTER_FLAG_TSYNC_ESRCH
    if flags & ~known:
        return SeccompInstallResult(ok=False, errno="EINVAL")

    has_tsync   = bool(flags & SECCOMP_FILTER_FLAG_TSYNC)
    has_listen  = bool(flags & SECCOMP_FILTER_FLAG_NEW_LISTENER)
    has_esrch   = bool(flags & SECCOMP_FILTER_FLAG_TSYNC_ESRCH)

    if has_listen and has_tsync:
        # plain TSYNC + NEW_LISTENER is the rejected combination.
        return SeccompInstallResult(ok=False, errno="EINVAL")

    if has_listen:
        # NEW_LISTENER alone, or NEW_LISTENER|TSYNC_ESRCH -> fd.
        return SeccompInstallResult(ok=True, returns_fd=True)

    # No listener requested: TSYNC alone / TSYNC_ESRCH alone / LOG / 0 -> ok.
    # (TSYNC_ESRCH without NEW_LISTENER is benign: it only changes TSYNC's
    #  partial-failure return convention, which is moot with no fd.)
    return SeccompInstallResult(ok=True, returns_fd=False)


def test_install_tsync_alone_ok_no_fd():
    r = seccomp_install(SECCOMP_FILTER_FLAG_TSYNC)
    assert r.ok and not r.returns_fd and r.errno is None


def test_install_new_listener_alone_returns_fd():
    r = seccomp_install(SECCOMP_FILTER_FLAG_NEW_LISTENER)
    assert r.ok and r.returns_fd and r.errno is None


def test_install_tsync_plus_new_listener_einval():
    flags = SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_NEW_LISTENER
    r = seccomp_install(flags)
    assert not r.ok and r.errno == "EINVAL" and not r.returns_fd


def test_install_tsync_esrch_plus_new_listener_returns_fd():
    flags = SECCOMP_FILTER_FLAG_TSYNC_ESRCH | SECCOMP_FILTER_FLAG_NEW_LISTENER
    r = seccomp_install(flags, kernel_has_tsync_esrch=True)
    assert r.ok and r.returns_fd and r.errno is None


def test_install_tsync_esrch_plus_new_listener_einval_on_pre_5_7():
    # On a kernel without the ESRCH bit, the flag is unknown -> EINVAL. This is
    # the failure mode the ALR probe must distinguish (fall back, don't claim
    # NEW_LISTENER unavailable when it's really the ESRCH pairing that's absent).
    flags = SECCOMP_FILTER_FLAG_TSYNC_ESRCH | SECCOMP_FILTER_FLAG_NEW_LISTENER
    r = seccomp_install(flags, kernel_has_tsync_esrch=False)
    assert not r.ok and r.errno == "EINVAL"


def test_install_unknown_flag_einval():
    r = seccomp_install(1 << 20)  # bogus high bit
    assert not r.ok and r.errno == "EINVAL"


def test_install_zero_flags_ok_no_fd():
    r = seccomp_install(0)
    assert r.ok and not r.returns_fd


def test_install_branch_table_full():
    """The complete install decision table ADR-002 §2(2b) relies on, asserted
    as a single mapping so any kernel-model drift is caught in one place."""
    TS = SECCOMP_FILTER_FLAG_TSYNC
    NL = SECCOMP_FILTER_FLAG_NEW_LISTENER
    ES = SECCOMP_FILTER_FLAG_TSYNC_ESRCH

    # (flags, kernel_has_tsync_esrch) -> (ok, returns_fd, errno)
    table = {
        (0, True):           (True, False, None),
        (TS, True):          (True, False, None),
        (NL, True):          (True, True, None),
        (TS | NL, True):     (False, False, "EINVAL"),
        (ES | NL, True):     (True, True, None),
        (ES | NL, False):    (False, False, "EINVAL"),
        (ES, True):          (True, False, None),   # ESRCH w/o listener: benign
    }
    for (flags, has_esrch), (ok, fd, errno) in table.items():
        r = seccomp_install(flags, kernel_has_tsync_esrch=has_esrch)
        assert (r.ok, r.returns_fd, r.errno) == (ok, fd, errno), (
            f"flags={flags:#x} esrch={has_esrch}: "
            f"got {(r.ok, r.returns_fd, r.errno)} want {(ok, fd, errno)}"
        )


def test_pcgate_tsync_and_notif_new_listener_are_separately_installable():
    """ADR-002 §2(2b) key consequence: because TSYNC|NEW_LISTENER is EINVAL, the
    notif filter must be a SEPARATE install. Model exactly that sequence and
    assert both installs succeed independently and the listener install yields a
    fd to hand to the supervisor."""
    # 1) F_pcgate installed with TSYNC (current loader; covers pre-existing
    #    guest threads). ok, no fd.
    r_pcgate = seccomp_install(SECCOMP_FILTER_FLAG_TSYNC)
    assert r_pcgate.ok and not r_pcgate.returns_fd

    # 2) F_notif installed SEPARATELY with NEW_LISTENER (not OR-ed with TSYNC,
    #    which would EINVAL). Prefer flag 0 form, here exercised as the ESRCH
    #    pairing the loader falls back to. Either way: ok + fd.
    r_notif_plain = seccomp_install(SECCOMP_FILTER_FLAG_NEW_LISTENER)
    assert r_notif_plain.ok and r_notif_plain.returns_fd

    r_notif_esrch = seccomp_install(
        SECCOMP_FILTER_FLAG_TSYNC_ESRCH | SECCOMP_FILTER_FLAG_NEW_LISTENER
    )
    assert r_notif_esrch.ok and r_notif_esrch.returns_fd

    # 3) The combined single-install the loader must NOT attempt.
    r_combined = seccomp_install(
        SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_NEW_LISTENER
    )
    assert not r_combined.ok and r_combined.errno == "EINVAL"


# --------------------------------------------------------------------------- #
# Small helper for assertion messages.
# --------------------------------------------------------------------------- #

def _name(nr):
    return (PATH_SYSCALLS.get(nr)
            or NOTIF_WHITELIST.get(nr)
            or OTHER_NON_PATH.get(nr)
            or f"nr{nr}")
