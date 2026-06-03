"""HOST-ONLY guard: the restricted-syscall-coverage SSOT doc must enumerate, in
ONE pass, the bounded set of operations Android SELinux denies an untrusted_app
that ordinary glibc apps expect — each with a paste-ready interposer wrapper and
a not-a-bypass argument.

`docs/design/restricted-syscall-coverage.md` is the single place that answers
"which advisory/policy/probe operations does the Android sandbox deny, and what is
the exact libalr_interpose.c wrapper that neutralizes each WITHOUT weakening the
sandbox". It mirrors the shape of the already-shipped bind()/setsockopt() no-ops
and cross-references the sibling netlink-recvmsg-emulation.md (does not duplicate
its wire layout).

These lenient checks pin the contract the loader/interposer owner implements:
  - the doc exists, is HOST-ONLY, and declares itself the coverage SSOT,
  - the core invariant (neutralize a POLICY/PROBE denial; NEVER fabricate a
    DATA-PATH result) is stated explicitly,
  - every delta entry §4..§13 of the bounded class is present with its syscall,
  - each carries a ready-to-paste wrapper (a real C function signature) AND a
    not-a-bypass argument,
  - it reuses the shipped wrappers' shape (ALR_REAL / RTLD_NEXT / bind / setsockopt)
    and does NOT claim to edit runtime_report.cpp for the no-op class,
  - the whole class is env-gated for an A/B drain,
  - the netlink cross-reference resolves to a real on-disk doc (no dangling cites).
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "design" / "restricted-syscall-coverage.md"
DESIGN_DIR = ROOT / "docs" / "design"
INTERPOSE_C = ROOT / "app" / "src" / "main" / "cpp" / "alr_interpose" / "libalr_interpose.c"


def _text() -> str:
    assert DOC.is_file(), f"restricted-syscall-coverage SSOT doc missing: {DOC}"
    return DOC.read_text(encoding="utf-8")


def test_doc_exists_host_only_and_is_ssot():
    text = _text()
    assert text.strip(), "restricted-syscall-coverage doc is empty"
    assert "HOST-ONLY" in text, "doc must declare it is HOST-ONLY"
    assert "SSOT" in text, "doc must declare itself the coverage SSOT"
    # It is a design the loader/interposer owner implements, not this worker.
    assert "libalr_interpose.c" in text, (
        "doc must name the implementation file (libalr_interpose.c)"
    )


def test_core_invariant_no_datapath_fabrication():
    """The load-bearing rule: neutralize POLICY/PROBE denials, never fake DATA."""
    text = _text()
    low = text.lower()
    assert "policy" in low and "probe" in low, (
        "doc must frame the class as policy/probe denials"
    )
    assert "data-path" in low or "data path" in low, (
        "doc must distinguish the forbidden DATA-PATH case"
    )
    # It must explicitly forbid fabricating a data-path result.
    assert "never" in low and ("fabricate" in low or "synthes" in low), (
        "doc must explicitly forbid fabricating a data-path result"
    )


def test_not_a_bypass_argument_present():
    text = _text()
    assert "bypass" in text.lower(), "doc must carry the not-a-bypass argument"
    # The argument hinges on calling the REAL syscall first + W^X cleanliness.
    assert "REAL" in text or "real syscall" in text.lower(), (
        "not-a-bypass argument must rest on calling the real syscall first"
    )
    assert "W^X" in text or "WX" in text, "doc must assert W^X-cleanliness"


def test_mirrors_shipped_wrapper_shape():
    """The new wrappers must reuse the shipped bind()/setsockopt() idiom."""
    text = _text()
    assert "ALR_REAL" in text, "wrappers must reuse the ALR_REAL RTLD_NEXT cache macro"
    assert "RTLD_NEXT" in text
    assert "bind(" in text and "setsockopt" in text, (
        "doc must reference the shipped bind()/setsockopt() wrappers it mirrors"
    )
    # And the sanity-check that the shipped wrappers really exist in the file.
    assert INTERPOSE_C.is_file(), "interposer source must exist to mirror"
    src = INTERPOSE_C.read_text(encoding="utf-8")
    assert "int bind(" in src and "int setsockopt(" in src, (
        "shipped bind()/setsockopt() wrappers must exist in libalr_interpose.c"
    )


# The bounded delta class (§4..§13): each token is the syscall/operation the
# entry must specify, by libc name. The doc must name every one.
DELTA_SYSCALLS = (
    "sched_setaffinity",
    "sched_setscheduler",
    "setpriority",
    "nice",
    "prctl",
    "mlock",
    "mlockall",
    "statfs",
    "ioctl",
    "shmget",
    "SO_REUSEPORT",
    "/proc/sys",
    "prlimit64",
)


def test_all_delta_syscalls_enumerated():
    text = _text()
    missing = [s for s in DELTA_SYSCALLS if s not in text]
    assert not missing, (
        f"coverage doc must enumerate the whole bounded class; missing: {missing}"
    )


def test_each_entry_has_pasteable_wrapper_signature():
    """Every advisory-denial entry needs a real, paste-ready C wrapper signature."""
    text = _text()
    # A function definition/signature for each headline wrapper (loose: name + '(').
    for fn in (
        "sched_setaffinity(",
        "setpriority(",
        "mlock(",
        "mlockall(",
        "statfs(",
        "ioctl(",
        "shmget(",
        "prctl(",
        "prlimit64(",
    ):
        assert fn in text, f"doc must give a paste-ready wrapper for {fn})"
    # The verbatim-signatures block must exist (so the owner can copy them).
    assert "signatures" in text.lower(), (
        "doc must include a verbatim function-signatures block for the owner"
    )


def test_errnos_named_for_the_class():
    text = _text()
    # The denial errnos the class neutralizes must be named.
    for e in ("EPERM", "EACCES"):
        assert e in text, f"doc must name the {e} denial errno"


def test_class_is_env_gated_for_ab_drain():
    """The whole no-op class must be disable-able via one env var (A/B drain)."""
    text = _text()
    assert "ALR_POLICY_EMU" in text, (
        "doc must define a single env gate (ALR_POLICY_EMU) for the whole class"
    )
    # Mirror the existing g_pcgate read-once pattern.
    assert "g_policy_emu" in text or "g_pcgate" in text, (
        "doc must mirror the g_pcgate read-once env pattern"
    )


def test_selective_wrappers_are_flagged_selective():
    """prctl / ioctl / capset must be SELECTIVE, not blanket no-ops."""
    text = _text()
    # prctl and ioctl must be explicitly marked selective (not blanket).
    assert "SELECTIVE" in text or "selective" in text.lower(), (
        "prctl/ioctl/rlimit wrappers must be flagged SELECTIVE (no blanket no-op)"
    )
    # capset/setns/unshare must be explicitly NOT no-op'd (security-relevant).
    assert "capset" in text and "setns" in text, (
        "doc must address capset/setns and explicitly NOT no-op them"
    )


def test_does_not_claim_runtime_report_edit_for_noop_class():
    """The advisory no-op class is interposer-only; no runtime_report.cpp edit."""
    text = _text()
    # The doc must state no runtime_report.cpp change is needed for the no-op class.
    assert "runtime_report.cpp" in text, (
        "doc must address the supervisor (runtime_report.cpp) interaction"
    )
    assert re.search(r"[Nn]o[\s_-]*`?runtime_report\.cpp`?\s*change", text) or \
        "No runtime_report.cpp change" in text or \
        "no runtime_report.cpp change" in text.lower(), (
        "doc must state no runtime_report.cpp change is needed for §4..§13"
    )


def test_pcgate_interaction_addressed():
    """None of the new syscalls are in the 9 traced path syscalls => PC gate clean."""
    text = _text()
    assert "PC gate" in text or "PCGATE" in text or "PC-gate" in text, (
        "doc must address the PC-gate (seccomp BPF) interaction"
    )
    assert "9 traced path syscalls" in text or "nine" in text.lower(), (
        "doc must note these syscalls are NOT in the 9 traced path syscalls"
    )


def test_netlink_crossref_resolves_no_dangling_cites():
    """The netlink recvmsg piece is cross-referenced, not duplicated; cite resolves."""
    text = _text()
    assert "netlink-recvmsg-emulation.md" in text, (
        "doc must cross-reference the sibling netlink-recvmsg-emulation.md"
    )
    # Every .md path token in the doc must resolve on disk (no dangling cites).
    for tok in sorted(set(re.findall(r"[\w./-]+\.md", text))):
        if "/" not in tok:
            assert (DESIGN_DIR / tok).is_file() or (ROOT / tok).is_file(), (
                f"doc references {tok!r} but no such file exists under docs/design/"
            )
            continue
        if "design/" in tok or "research/" in tok or "evidence/" in tok:
            assert (ROOT / tok).is_file(), (
                f"doc references {tok!r} but no such file exists"
            )


def test_ranks_by_app_class_breadth():
    """The entries must be ranked by how many app classes each unblocks."""
    text = _text()
    low = text.lower()
    assert "rank" in low or "breadth" in low, (
        "doc must rank entries by app-class breadth"
    )
    # The headline broad classes must be named as beneficiaries.
    assert "chromium" in low and "qt" in low, (
        "doc must name the broad app classes (chromium/Qt thread-pools) it unblocks"
    )
    assert "crypto" in low or "openssl" in low.lower() or "gnutls" in low, (
        "doc must name crypto/TLS (mlock) as an unblocked class"
    )
