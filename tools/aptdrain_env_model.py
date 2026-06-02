"""Host model of the §5 v2-apt-drain launch ENV contract — T2.

What this is (and is NOT)
-------------------------
This is the executable specification of the *environment* a dpkg/apt drain must
run under so that a non-root ``dpkg -i hello.deb`` re-enters BOTH ALR mediations
(fakeroot credentials + interpose path-rewrite) in-process, including across the
exec'd children (``zstd``/``sh``/``dpkg-deb``) that the unpack/configure stages
fork.

It is a PURE host model. It runs NO aarch64 ``.so``, installs NO seccomp/ptrace,
allocates NO executable memory, and touches NO device. Its job is to be the
single, testable source of truth for three decisions the loader
(``runtime_report.cpp``) must make when the drain is active:

  1. **LD_PRELOAD chain construction** — fakeroot FIRST, interpose KEPT, both as
     ABSOLUTE rootfs host paths, de-duplicated, preserving any guest preloads.
     This mirrors ``build_fakeroot_overlay.device_cmd`` and the loader's
     ``decide_exec_envp_injection`` (``alr_runtime/alr_exec.cpp``), EXTENDED with
     the fakeroot ``.so`` that the current classifier does not yet know about.
  2. **Drain env block** — the ``ALR_ROOTFS`` / ``FAKEROOTUID`` / ``FAKEROOTGID``
     (and the app-side ``ALR_FAKEROOT`` gate + ``ALR_REEXEC_INPROC`` enable)
     that pin the faked identity and turn on in-process exec re-entry for the
     children.
  3. **Marker → drain decision** — whether, given the on-device
     ``/data/local/tmp/.alr-aptdrain`` marker and the staged assets, the loader
     should activate the fakeroot chain at all (default-OFF, no regression for a
     normal guest launch).

Why a host model when the real run is device-only
-------------------------------------------------
The host is Darwin: it cannot run the glibc shims or dpkg, so "``unpacked=true``"
is a device claim (DEVICE-REQ ``ALR-V2-apt-unpack``). But the *env wiring* that
gets dpkg there is pure string/decision logic, and getting it wrong (interpose
dropped, fakeroot not first, ALR_ROOTFS missing from a child, marker not gating)
is exactly the class of bug that would silently make the device drain fail with
``requires superuser`` / EPERM / bare-Android-fs opens. This module makes that
logic testable on the host so the device drain only has to prove the mechanism,
not the bookkeeping.

Relationship to the loader source (read-only — NOT modified by this session)
----------------------------------------------------------------------------
* ``runtime_report.cpp`` (L1617-1636) builds the FIRST guest's env: it pushes
  ``LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_interpose.so`` and
  ``ALR_ROOTFS=<rootfs>`` but has NO fakeroot awareness today.
* ``alr_runtime/alr_exec.cpp`` ``decide_exec_envp_injection`` rebuilds each
  exec'd CHILD's envp (B-3) so the child re-enters mediation — but, again, only
  the interpose ``.so``. The inproc re-map redirect (L2584-2637) passes the
  child's envp register through the SAME B-3 block, so this one classifier is the
  single chokepoint for both the kernel-execve and the inproc-remap paths.

``chain_ld_preload`` / ``decide_drain_env`` below are the proposed *superset* of
that logic for the drain case. The proposed loader diff that adopts them lives in
``docs/design/runtime-report-aptdrain-contract.md`` (this session owns the doc +
this model; the loader edit is WS-1/main's).
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, field

# --------------------------------------------------------------------------- #
# Constants — kept in lockstep with tools/build_fakeroot_overlay.py and the
# loader's hardcoded interpose path (runtime_report.cpp L1633). If these move,
# move them in all three places (see memory: version-stamp-pin-sites style).
# --------------------------------------------------------------------------- #

# Rootfs-relative locations the device extractor lays the two shims at.
INTERPOSE_REL = "usr/lib/androlinux/libalr_interpose.so"
FAKEROOT_REL = "usr/lib/androlinux/libalr_fakeroot.so"

# The single device marker that gates the whole drain (same convention as the
# .alr-cr1 markers: absent => normal launch, no regression; present => the
# integration/device session has explicitly opted this run into the apt drain).
DRAIN_MARKER = "/data/local/tmp/.alr-aptdrain"

# App-process env var the loader reads (::getenv) to turn the fakeroot chain on.
# MainActivity sets it just before the drain probe and unsets it after, so the
# normal probe path never sees it (proposed diff A/B in the contract doc).
FAKEROOT_GATE_ENV = "ALR_FAKEROOT"

# App-process env var that enables the in-process exec re-entry (re-map+jump)
# for the children dpkg forks (zstd/sh/dpkg-deb). Default-OFF in the loader
# (runtime_report.cpp L2077); the drain must enable it or the extract/configure
# children hit the dead kernel-execve path. Premised on the static-startup fix.
REEXEC_GATE_ENV = "ALR_REEXEC_INPROC"

# Faked identity the fakeroot shim reports (FAKEROOTUID/GID env contract — see
# build_fakeroot_overlay device_cmd / libalr_fakeroot.c).
FAKEROOT_UID = "0"
FAKEROOT_GID = "0"


# --------------------------------------------------------------------------- #
# 1. LD_PRELOAD chain construction
# --------------------------------------------------------------------------- #

def _abs(rootfs_dir: str, rel: str) -> str:
    """Absolute on-device host path of a rootfs-relative shim. R3 finding: the
    preload MUST be the absolute ROOTFS host path (not a guest "/usr/..." path),
    because under PCGATE the loader no longer traces ld.so's open of the preload,
    so a guest path would hit the bare Android fs and the shim would fail to
    load. Normalised to a single '/' join, no trailing slash on rootfs_dir."""
    return rootfs_dir.rstrip("/") + "/" + rel.lstrip("/")


def chain_ld_preload(
    rootfs_dir: str,
    *,
    fakeroot: bool,
    existing: str = "",
) -> str:
    """Build the LD_PRELOAD value for a drain process / its exec'd children.

    Contract (mirrors build_fakeroot_overlay.device_cmd + the loader's B-3
    classifier, EXTENDED with fakeroot):

      * interpose ``.so`` is ALWAYS present (path mediation must never drop).
      * when ``fakeroot`` is True the fakeroot ``.so`` is prepended FIRST so its
        credential/stat wrappers run OUTERMOST (dlsym(RTLD_NEXT) then chains down
        into the interpose ``.so`` and finally libc).
      * any ``existing`` guest LD_PRELOAD entries are PRESERVED after ours (guest
        preloads must survive), but our two shims are de-duplicated out of the
        guest list first (idempotency across the dpkg→sh→dpkg-deb chain: a child
        whose parent we already injected must not accumulate duplicates).
      * every entry is the ABSOLUTE rootfs host path (R3).

    Order: ``[fakeroot?] interpose [guest preloads…]`` joined by ':'.
    Pure string logic; no fs access.
    """
    interpose = _abs(rootfs_dir, INTERPOSE_REL)
    fakeroot_so = _abs(rootfs_dir, FAKEROOT_REL)

    chain: list[str] = []
    if fakeroot:
        chain.append(fakeroot_so)  # FIRST — credential wrappers outermost
    chain.append(interpose)        # path mediation — never dropped

    # Preserve guest preloads, dropping any duplicate of our two shims (so a
    # re-injected child does not double-list them). Order within the guest tail
    # is preserved; first occurrence of each unique entry wins.
    ours = {fakeroot_so, interpose}
    seen = set(chain)
    for entry in existing.split(":"):
        entry = entry.strip()
        if not entry or entry in ours or entry in seen:
            continue
        seen.add(entry)
        chain.append(entry)

    return ":".join(chain)


# --------------------------------------------------------------------------- #
# 2. Drain env block
# --------------------------------------------------------------------------- #

@dataclass
class DrainEnv:
    """The fully-resolved environment a drain process runs under.

    ``ld_preload`` / ``alr_rootfs`` / ``fakeroot_uid`` / ``fakeroot_gid`` are the
    GUEST-process env (what dpkg and its children see). ``app_env`` is the small
    set the loader reads from its OWN (app) process env to decide behaviour
    (ALR_FAKEROOT gate + ALR_REEXEC_INPROC enable) — these are NOT passed to the
    guest, they steer the loader.
    """

    active: bool
    reason: str
    ld_preload: str = ""
    alr_rootfs: str = ""
    fakeroot_uid: str = ""
    fakeroot_gid: str = ""
    app_env: dict[str, str] = field(default_factory=dict)

    def guest_env(self) -> dict[str, str]:
        """The KEY=VALUE block the loader pushes into the guest's envp for a
        drain. Empty dict when the drain is inactive (normal launch — no
        regression)."""
        if not self.active:
            return {}
        return {
            "LD_PRELOAD": self.ld_preload,
            "ALR_ROOTFS": self.alr_rootfs,
            "FAKEROOTUID": self.fakeroot_uid,
            "FAKEROOTGID": self.fakeroot_gid,
        }

    def to_dict(self) -> dict:
        return {
            "active": self.active,
            "reason": self.reason,
            "guest_env": self.guest_env(),
            "app_env": dict(self.app_env),
        }


def decide_drain_env(
    rootfs_dir: str,
    *,
    fakeroot: bool = True,
    inproc_reexec: bool = True,
    existing_ld_preload: str = "",
) -> DrainEnv:
    """Resolve the full env for a drain launch.

    ``fakeroot``        — chain the fakeroot ``.so`` and set the ALR_FAKEROOT app
                          gate + FAKEROOTUID/GID identity (the dpkg/apt case).
    ``inproc_reexec``   — set the ALR_REEXEC_INPROC app gate so the children dpkg
                          forks (zstd/sh) re-enter in-process instead of dying on
                          the kernel-execve path. Premised on the static-startup
                          fix (PR#8); the contract notes that dependency.
    ``existing_ld_preload`` — any guest-set LD_PRELOAD to preserve underneath.

    A ``rootfs_dir`` of "" is a misconfiguration: the interposer self-disables
    with no ALR_ROOTFS, so there is nothing to mediate — return inactive.
    """
    if not rootfs_dir:
        return DrainEnv(active=False, reason="no-rootfs")

    app_env: dict[str, str] = {}
    if fakeroot:
        app_env[FAKEROOT_GATE_ENV] = "1"
    if inproc_reexec:
        app_env[REEXEC_GATE_ENV] = "1"

    return DrainEnv(
        active=True,
        reason="fakeroot-drain" if fakeroot else "interpose-only",
        ld_preload=chain_ld_preload(
            rootfs_dir, fakeroot=fakeroot, existing=existing_ld_preload
        ),
        alr_rootfs=rootfs_dir.rstrip("/"),
        fakeroot_uid=FAKEROOT_UID if fakeroot else "",
        fakeroot_gid=FAKEROOT_GID if fakeroot else "",
        app_env=app_env,
    )


# --------------------------------------------------------------------------- #
# 3. Marker → drain decision
# --------------------------------------------------------------------------- #

@dataclass
class DrainGate:
    """Whether the loader should activate the apt drain for this launch, and why.

    The decision is the AND of: the device marker present, the program being a
    package manager (dpkg/apt — fakeroot must NOT be forced on a normal app
    launch even under the marker), and the two shims being staged. Any missing
    input => inactive, with a precise reason so a device drain log can pinpoint
    the missing prerequisite.
    """

    active: bool
    reason: str


# Programs for which the fakeroot drain is meaningful. Matched on the basename of
# the first argv token so "/usr/bin/dpkg", "dpkg", "/usr/bin/apt-get" all hit.
_DRAIN_PROGRAMS = {
    "dpkg", "dpkg-deb", "dpkg-query", "dpkg-split",
    "apt", "apt-get", "apt-cache", "apt-config",
}


def _program_basename(program: str) -> str:
    """Basename of the program token (handles the newline-joined argv the JNI
    probe takes: program is "argv0\\n--flag\\n…" — only argv0 names the binary)."""
    argv0 = program.split("\n", 1)[0]
    return argv0.rsplit("/", 1)[-1]


def decide_drain_gate(
    *,
    program: str,
    marker_present: bool,
    fakeroot_so_staged: bool,
    interpose_so_staged: bool,
) -> DrainGate:
    """Decide whether the fakeroot apt drain is active for this launch.

    Default-OFF: every prerequisite must hold. The order of the checks is the
    order a device drain would want them reported (marker first — it is the
    intentional opt-in; then program; then the two staged shims).
    """
    if not marker_present:
        return DrainGate(active=False, reason="marker-absent")
    if _program_basename(program) not in _DRAIN_PROGRAMS:
        return DrainGate(active=False, reason="not-pkg-manager")
    if not interpose_so_staged:
        # Without the interpose .so even a normal launch loses path mediation;
        # for the drain it is mandatory. Report it distinctly from fakeroot.
        return DrainGate(active=False, reason="interpose-not-staged")
    if not fakeroot_so_staged:
        return DrainGate(active=False, reason="fakeroot-not-staged")
    return DrainGate(active=True, reason="drain-armed")


# --------------------------------------------------------------------------- #
# Convenience: full resolution from the gate inputs straight to a DrainEnv.
# --------------------------------------------------------------------------- #

def resolve(
    rootfs_dir: str,
    *,
    program: str,
    marker_present: bool,
    fakeroot_so_staged: bool,
    interpose_so_staged: bool,
    inproc_reexec: bool = True,
    existing_ld_preload: str = "",
) -> DrainEnv:
    """Gate THEN env: if the gate is closed return an inactive DrainEnv carrying
    the gate's reason; otherwise return the fully-resolved drain env."""
    gate = decide_drain_gate(
        program=program,
        marker_present=marker_present,
        fakeroot_so_staged=fakeroot_so_staged,
        interpose_so_staged=interpose_so_staged,
    )
    if not gate.active:
        return DrainEnv(active=False, reason=gate.reason)
    return decide_drain_env(
        rootfs_dir,
        fakeroot=True,
        inproc_reexec=inproc_reexec,
        existing_ld_preload=existing_ld_preload,
    )


# --------------------------------------------------------------------------- #
# CLI — emit the resolved env as JSON or as a copy-pasteable device block.
# --------------------------------------------------------------------------- #

def _render_device_block(env: DrainEnv) -> str:
    if not env.active:
        return f"# apt drain INACTIVE: {env.reason}"
    g = env.guest_env()
    lines = [
        f"# apt drain ARMED ({env.reason}) — app-process gate env (loader reads "
        f"these via getenv, NOT passed to guest):",
    ]
    for k, v in env.app_env.items():
        lines.append(f"#   {k}={v}")
    lines.append("# guest-process env (fakeroot FIRST, interpose KEPT):")
    lines.append(f"LD_PRELOAD={g['LD_PRELOAD']} \\")
    lines.append(
        f"  ALR_ROOTFS={g['ALR_ROOTFS']} "
        f"FAKEROOTUID={g['FAKEROOTUID']} FAKEROOTGID={g['FAKEROOTGID']} \\"
    )
    lines.append(
        "  dpkg --force-not-root --force-bad-path -i "
        "/var/cache/apt/archives/hello_2.10-3build1_arm64.deb"
    )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="aptdrain_env_model",
        description="Resolve the §5 v2-apt-drain launch env (LD_PRELOAD chain + "
        "ALR_ROOTFS/FAKEROOT* + marker gate). Host model — no device, no .so.",
    )
    p.add_argument("--rootfs", default="/data/data/dev.chanwoo.androlinux/files/rootfs",
                   help="on-device rootfs host dir (ALR config.rootfs_dir)")
    p.add_argument("--program", default="/usr/bin/dpkg",
                   help="program argv0 (drain only arms for dpkg/apt family)")
    p.add_argument("--no-marker", action="store_true",
                   help="simulate the .alr-aptdrain marker being ABSENT")
    p.add_argument("--no-fakeroot-staged", action="store_true",
                   help="simulate fakeroot .so NOT staged")
    p.add_argument("--no-interpose-staged", action="store_true",
                   help="simulate interpose .so NOT staged")
    p.add_argument("--no-inproc", action="store_true",
                   help="do NOT enable ALR_REEXEC_INPROC")
    p.add_argument("--existing-ld-preload", default="",
                   help="a guest-set LD_PRELOAD to preserve underneath ours")
    p.add_argument("--json", action="store_true", help="emit JSON")
    args = p.parse_args(argv)

    env = resolve(
        args.rootfs,
        program=args.program,
        marker_present=not args.no_marker,
        fakeroot_so_staged=not args.no_fakeroot_staged,
        interpose_so_staged=not args.no_interpose_staged,
        inproc_reexec=not args.no_inproc,
        existing_ld_preload=args.existing_ld_preload,
    )
    if args.json:
        print(json.dumps(env.to_dict(), indent=2))
    else:
        print(_render_device_block(env))
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
