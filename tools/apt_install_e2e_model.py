"""Host model of the FULL ``apt install <pkg>`` chain, stage-by-stage, with each
stage's ALR gate tagged — T2 of the v2 apt pipeline.

What this is (and is NOT)
-------------------------
:mod:`tools.dpkg_unpack_model` (T2-earlier) proves ONE stage end-to-end on the
host: the per-member privileged-call sequence dpkg performs *in-process* while
laying files down (chown/chmod/stat/mknod) — the fakeroot (G2) slice. This module
zooms OUT to the WHOLE ``apt install`` chain and answers the orthogonal question:

    "For ``apt install <pkg>`` to succeed, what EXACTLY is required, stage by
    stage — and at each stage which ALR gate does it touch (G1 exec-re-entry /
    G2 fakeroot / pure), so which stages are already HOST-proven and which are
    PENDING-device (G1)?"

It is a **decision table over a dependency graph**, not a device run. Every stage
is a node; an edge ``A -> B`` means B cannot start until A succeeds. Each stage
carries:

  * the concrete dpkg/apt action,
  * whether it runs **in dpkg's own process** or **forks+execs a child**,
  * its ALR **gate** tag (the SSOT §6 taxonomy):

        PURE  — no ALR mediation (apt index fetch over the net; dpkg-db write).
        G2    — fakeroot: a privileged *meta* op satisfied IN-PROCESS by the
                LD_PRELOAD shim (getuid→0, chown no-op+DB, stat overlay). The
                dpkg_unpack_model already host-proves this slice.
        G1    — exec-re-entry: a ``fork + execvp`` of a guest child (``zstd``,
                ``/bin/sh`` …) whose kernel-execve fails under ALR (guest ld.so
                can't resolve) and must be re-mapped + re-injected by the loader.
                This is the MAIN session's wall; host can only MODEL it.

  * its **proof ceiling**: HOST (this host can prove it) or DEVICE (only the
    device drain, gated on G1, can prove it).

The honest crux (SSOT §6, reproduced as code): the *metadata* operations
(superuser gate, chown/chmod/stat) are G2 — host-ready and **G1-independent** —
but the terminal ``unpacked=true`` is NOT, because dpkg forks a compressor
(``zstd``/``xz``) to extract a compressed ``data.tar``, and that child is a G1
exec-re-entry. So "unpack is single-process, therefore G1-independent" is FALSE
for a real ``.zst`` package. This module makes that conclusion executable: it
classifies each stage, shows fakeroot-alone clears the meta stages, and shows the
extract/configure stages still require G1 — and it branches hello.deb
(maintainer-script-free → only the extract child needs G1) vs a script-bearing
package (an extra ``/bin/sh`` configure child → a second G1 stage).

Honest scope
------------
HOST-ONLY. No stage here is *executed* on a device. ``host_provable`` is a claim
about WHERE the proof can land (this module + dpkg_unpack_model for the G2/PURE
stages; the device drain for the G1 stages), not a device success claim. We never
promote a G1 stage to "passes" from the host (SSOT update rule).
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, field
from enum import Enum


# --------------------------------------------------------------------------- #
# Gate taxonomy (the SSOT §2/§6 three orthogonal gates, plus PURE)
# --------------------------------------------------------------------------- #
class Gate(str, Enum):
    """The ALR gate a stage touches. String-valued for clean JSON."""

    PURE = "pure"   # no ALR mediation at all (network fetch; in-proc DB write)
    G1 = "g1"       # exec-re-entry: fork+exec of a guest child the loader re-maps
    G2 = "g2"       # fakeroot: a privileged meta-op satisfied in-process by the shim
    G3 = "g3"       # staging: the runtime closure had to be present (a precondition,
                    # not a runtime stage — used only by the precondition nodes)


class Proc(str, Enum):
    """How the stage executes relative to dpkg's own process."""

    IN_PROC = "in_proc"       # inside dpkg/apt's own address space (libdpkg/libapt)
    FORK_EXEC = "fork_exec"   # fork(2) + execvp(2) of a separate guest binary
    NETWORK = "network"       # an out-of-process network operation (apt http method)


class Ceiling(str, Enum):
    """Where a stage's proof can actually land."""

    HOST = "host"       # this host (Darwin) can prove the mechanism today
    DEVICE = "device"   # only the on-device drain (gated on G1) can prove it


# --------------------------------------------------------------------------- #
# A single stage of the apt-install chain
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class Stage:
    """One node of the ``apt install`` dependency graph.

    ``depends_on`` are the ids of stages that must SUCCEED before this one runs —
    the edges of the chain. A stage's proof ceiling is DEVICE iff its gate is G1
    (a guest child the host cannot exec), else HOST.
    """

    id: str
    title: str
    action: str                 # the concrete dpkg/apt action
    proc: Proc
    gate: Gate
    depends_on: tuple[str, ...] = ()
    optional: bool = False      # stage only fires for SOME packages (e.g. scripts)
    note: str = ""

    @property
    def ceiling(self) -> Ceiling:
        """G1 stages are DEVICE-only (the host can't exec a guest child); every
        other gate (PURE/G2) is host-provable mechanism."""
        return Ceiling.DEVICE if self.gate is Gate.G1 else Ceiling.HOST

    @property
    def host_provable(self) -> bool:
        return self.ceiling is Ceiling.HOST

    @property
    def cleared_by_fakeroot_alone(self) -> bool:
        """True iff fakeroot (G2) alone — with NO exec-re-entry — lets this stage
        pass. The PURE stages need nothing from ALR; the G2 stages need exactly
        fakeroot; the G1 stages are NOT cleared by fakeroot (they need the loader's
        re-map)."""
        return self.gate in (Gate.PURE, Gate.G2)

    def as_dict(self) -> dict:
        return {
            "id": self.id,
            "title": self.title,
            "action": self.action,
            "proc": self.proc.value,
            "gate": self.gate.value,
            "depends_on": list(self.depends_on),
            "optional": self.optional,
            "ceiling": self.ceiling.value,
            "host_provable": self.host_provable,
            "cleared_by_fakeroot_alone": self.cleared_by_fakeroot_alone,
            "note": self.note,
        }


# --------------------------------------------------------------------------- #
# The canonical apt-install chain (SSOT §6, expanded to apt's outer stages)
# --------------------------------------------------------------------------- #
# dpkg's data.tar of a package may or may not be compressed; if compressed, the
# extract stage forks a compressor child (G1). noble debs are ``.zst`` → the child
# fires. A package with maintainer scripts forks ``/bin/sh`` at configure (G1).
#
# ``has_compressed_data`` and ``has_maintainer_scripts`` are the two branch knobs
# that decide which OPTIONAL stages appear. hello.deb: compressed data (.zst),
# NO maintainer scripts → exactly ONE G1 stage (the extract child). A
# script-bearing package: an additional configure-script G1 stage.

class Entry(str, Enum):
    """How the install is invoked — this decides whether the apt OUTER stages
    (index/fetch/apt-forks-dpkg) are part of the chain.

    APT      — ``apt install <pkg>``: apt reads the index, fetches the .deb, then
               fork+execs dpkg (an EXTRA G1 stage: apt's dpkg child).
    DPKG_I   — ``dpkg -i <local.deb>``: dpkg is the entry process (the device drain
               form, SSOT §5-제안 diff B). No apt outer stages, so the apt→dpkg G1
               stage is absent; the dpkg-inner stages are identical.
    """

    APT = "apt"
    DPKG_I = "dpkg_i"


def build_chain(
    *,
    has_compressed_data: bool = True,
    has_maintainer_scripts: bool = False,
    has_device_nodes: bool = False,
    entry: Entry = Entry.APT,
) -> tuple[Stage, ...]:
    """Build the ordered apt-install stage graph for a package profile.

    The default profile is the noble ``hello`` package: compressed (``.zst``)
    data, no maintainer scripts, no device nodes, invoked via ``apt install``.

    ``entry`` selects the invocation. ``Entry.APT`` prepends the apt outer stages
    (index → fetch → apt forks dpkg, the last being a G1 exec-re-entry).
    ``Entry.DPKG_I`` starts directly at dpkg (the device drain form) — no apt
    outer stages, so the apt→dpkg G1 stage is absent.
    """
    stages: list[Stage] = []

    # === apt outer stages (only when invoked via `apt install`) ============ #
    if entry is Entry.APT:
        stages.append(Stage(
            id="apt_index",
            title="apt: read package index",
            action="apt reads the cached Packages index, resolves <pkg>'s "
                   "dependency closure, and computes the install set",
            proc=Proc.IN_PROC, gate=Gate.PURE,
            note="pure in-proc libapt; no ALR mediation (index already on disk)",
        ))
        stages.append(Stage(
            id="apt_fetch",
            title="apt: fetch .deb(s) over the network",
            action="apt's http/https method downloads each .deb to "
                   "/var/cache/apt/archives/ (or uses an already-cached .deb)",
            proc=Proc.NETWORK, gate=Gate.PURE, depends_on=("apt_index",),
            note="network only — ALR un-involved. For the device drain the .deb "
                 "is pre-cached (offline), so this stage is a no-op there.",
        ))
        stages.append(Stage(
            id="apt_spawn_dpkg",
            title="apt: fork+exec dpkg",
            action="apt fork()s and execvp()s `dpkg --unpack` / `dpkg --configure` "
                   "for the fetched archives",
            proc=Proc.FORK_EXEC, gate=Gate.G1, depends_on=("apt_fetch",),
            note="an exec-re-entry UNIQUE to the apt entry: apt's dpkg child is a "
                 "guest binary the loader must re-map. `dpkg -i` skips it.",
        ))
        gate_dep: tuple[str, ...] = ("apt_spawn_dpkg",)
    else:
        gate_dep = ()

    # === dpkg unpack stages (identical for both entry modes) =============== #
    stages.append(Stage(
        id="dpkg_superuser_gate",
        title="dpkg: superuser gate",
        action="dpkg checks getuid()==0 (or --force-not-root) before unpacking; a "
               "bare non-root uid is refused here",
        proc=Proc.IN_PROC, gate=Gate.G2, depends_on=gate_dep,
        note="fakeroot getuid()->0 clears this in-process (the §7 mid-sub-gate)",
    ))
    stages.append(Stage(
        id="dpkg_read_control",
        title="dpkg: read control.tar",
        action="dpkg reads control.tar (control, md5sums, maintainer scripts) "
               "in-process via libdpkg",
        proc=Proc.IN_PROC, gate=Gate.PURE, depends_on=("dpkg_superuser_gate",),
        note="control.tar is small + (in noble) gzip, read by libdpkg in-proc; no "
             "child compressor for control",
    ))

    # --- data.tar extraction: the compressor child is the G1 crux --------- #
    if has_compressed_data:
        stages.append(Stage(
            id="dpkg_extract_data",
            title="dpkg: decompress data.tar (fork zstd/xz)",
            action="dpkg fork()s and execvp()s the data.tar compressor "
                   "(zstd/xz/gzip) to stream-decompress the payload",
            proc=Proc.FORK_EXEC, gate=Gate.G1,
            depends_on=("dpkg_read_control",),
            note="THE crux (SSOT §6 concl.2): a compressed data.tar means extract "
                 "itself forks a child → 'unpack is single-process' is FALSE. "
                 "noble hello = data.tar.zst → this stage fires.",
        ))
        extract_dep = "dpkg_extract_data"
    else:
        # uncompressed data.tar: libdpkg reads it in-proc, no child compressor.
        stages.append(Stage(
            id="dpkg_extract_data",
            title="dpkg: read uncompressed data.tar (in-proc)",
            action="data.tar is uncompressed; libdpkg reads members in-process, no "
                   "compressor child is forked",
            proc=Proc.IN_PROC, gate=Gate.PURE,
            depends_on=("dpkg_read_control",),
            note="the rare uncompressed-data case skips the G1 extract child; the "
                 "metadata stages below are then the ONLY non-pure work.",
        ))
        extract_dep = "dpkg_extract_data"

    # --- per-member metadata ops (the G2 fakeroot slice) ------------------ #
    stages.append(Stage(
        id="dpkg_lay_files",
        title="dpkg: write file bytes / dirs / symlinks",
        action="dpkg writes each regular file, makes dirs, creates symlinks "
               "(unprivileged, always works for a non-root uid)",
        proc=Proc.IN_PROC, gate=Gate.PURE, depends_on=(extract_dep,),
        note="unprivileged fs writes; the kernel creates them owned by the running "
             "non-root uid (fakeroot fixes ownership at stat time, below)",
    ))
    stages.append(Stage(
        id="dpkg_chown",
        title="dpkg: chown(root:root) each member",
        action="dpkg fchown()s each laid-down path to the .deb's recorded "
               "root:root ownership",
        proc=Proc.IN_PROC, gate=Gate.G2, depends_on=("dpkg_lay_files",),
        note="non-root chown EPERMs on the kernel; fakeroot makes it a no-op + "
             "records intent in the (dev,ino) DB (dpkg_unpack_model proves it)",
    ))
    stages.append(Stage(
        id="dpkg_chmod_stat",
        title="dpkg: chmod + stat verify",
        action="dpkg fchmod()s recorded perms and stat()s back to verify "
               "owner/mode == recorded",
        proc=Proc.IN_PROC, gate=Gate.G2, depends_on=("dpkg_chown",),
        note="fakeroot's stat overlay reports root:root + intended mode → dpkg's "
             "verify passes (the §7 stat==root:root sub-gate)",
    ))

    if has_device_nodes:
        stages.append(Stage(
            id="dpkg_mknod",
            title="dpkg: mknod device node(s)",
            action="dpkg mknod()s any char/block device members the package ships",
            proc=Proc.IN_PROC, gate=Gate.G2, depends_on=("dpkg_chmod_stat",),
            optional=True,
            note="THE one residual hard wall: the shipped C shim does NOT yet "
                 "intercept mknod, so non-root mknod(2) EPERMs. The model's "
                 "placeholder rule clears it; hello ships NO device node → absent.",
        ))
        meta_dep = "dpkg_mknod"
    else:
        meta_dep = "dpkg_chmod_stat"

    # === maintainer scripts (configure) — the SECOND G1 stage ============= #
    if has_maintainer_scripts:
        stages.append(Stage(
            id="dpkg_run_preinst",
            title="dpkg: run preinst (fork /bin/sh)",
            action="dpkg fork()s and execvp()s /bin/sh to run the package's "
                   "preinst maintainer script before unpack-completion",
            proc=Proc.FORK_EXEC, gate=Gate.G1, depends_on=(meta_dep,),
            optional=True,
            note="a SECOND exec-re-entry: the /bin/sh child + its envp need the "
                 "abs-rootfs LD_PRELOAD/ALR_ROOTFS re-injection (B-3).",
        ))
        unpack_complete_dep = "dpkg_run_preinst"
    else:
        unpack_complete_dep = meta_dep

    stages.append(Stage(
        id="dpkg_unpacked",
        title="dpkg: UNPACKED (files on disk)",
        action="dpkg records the package as Unpacked; all files are materialised "
               "and (faked-)owned root:root",
        proc=Proc.IN_PROC, gate=Gate.PURE, depends_on=(unpack_complete_dep,),
        note="the terminal `unpacked=true` marker. Reaching it required the G1 "
             "extract child (compressed data) — so it is NOT G1-independent.",
    ))

    # === configure (the install-completion stages) ======================== #
    if has_maintainer_scripts:
        stages.append(Stage(
            id="dpkg_run_postinst",
            title="dpkg: run postinst (fork /bin/sh)",
            action="dpkg fork()s and execvp()s /bin/sh to run the postinst "
                   "(`Setting up <pkg>`) configure script",
            proc=Proc.FORK_EXEC, gate=Gate.G1, depends_on=("dpkg_unpacked",),
            optional=True,
            note="the `Setting up` G1 stage; without it a script-bearing package "
                 "reaches Unpacked but never `install ok installed`.",
        ))
        configure_dep = "dpkg_run_postinst"
    else:
        # hello has no postinst → configure is a pure in-proc DB transition.
        stages.append(Stage(
            id="dpkg_configure_noscript",
            title="dpkg: configure (no script)",
            action="with no postinst, dpkg's configure is a pure in-process state "
                   "transition to Installed",
            proc=Proc.IN_PROC, gate=Gate.PURE, depends_on=("dpkg_unpacked",),
            note="hello has no maintainer scripts → configure needs no /bin/sh "
                 "child → no extra G1 stage beyond the extract child.",
        ))
        configure_dep = "dpkg_configure_noscript"

    stages.append(Stage(
        id="dpkg_update_db",
        title="dpkg: update /var/lib/dpkg/status",
        action="dpkg writes the package state (`install ok installed`) into the "
               "admindir status DB",
        proc=Proc.IN_PROC, gate=Gate.PURE, depends_on=(configure_dep,),
        note="pure in-proc DB write to the staged admindir (G3 provided the "
             "scaffold); the terminal `installed=true` marker.",
    ))

    return tuple(stages)


# --------------------------------------------------------------------------- #
# Package profiles — the two branches the task calls out
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class PackageProfile:
    """A package's shape + invocation, which decides which stages fire."""

    name: str
    has_compressed_data: bool
    has_maintainer_scripts: bool
    has_device_nodes: bool = False
    entry: Entry = Entry.APT
    note: str = ""

    def chain(self) -> tuple[Stage, ...]:
        return build_chain(
            has_compressed_data=self.has_compressed_data,
            has_maintainer_scripts=self.has_maintainer_scripts,
            has_device_nodes=self.has_device_nodes,
            entry=self.entry,
        )


# hello: the canonical maintainer-script-FREE target (noble hello ships a
# data.tar.zst, NO preinst/postinst, NO device node).
PROFILE_HELLO = PackageProfile(
    name="hello",
    has_compressed_data=True,
    has_maintainer_scripts=False,
    has_device_nodes=False,
    note="noble hello_2.10-3build1: data.tar.zst, no maintainer scripts, no device "
         "nodes. Via `apt install`: TWO G1 stages (apt→dpkg + the extract child).",
)

# hello invoked the device-drain way (`dpkg -i <local.deb>`, SSOT §5 diff B): the
# apt outer stages drop out, so the ONLY G1 stage is the extract child.
PROFILE_HELLO_DPKG_I = PackageProfile(
    name="hello (dpkg -i)",
    has_compressed_data=True,
    has_maintainer_scripts=False,
    has_device_nodes=False,
    entry=Entry.DPKG_I,
    note="the device drain form: `dpkg -i hello.deb` skips apt, so the apt→dpkg "
         "G1 stage is absent → exactly ONE G1 stage (the extract child).",
)

# A script-bearing package (the general case): adds a /bin/sh configure child.
PROFILE_WITH_SCRIPTS = PackageProfile(
    name="scripted-pkg",
    has_compressed_data=True,
    has_maintainer_scripts=True,
    has_device_nodes=False,
    note="a typical package with preinst/postinst (e.g. one registering an "
         "alternative or a service) → adds the /bin/sh configure G1 stage on top "
         "of the extract child (and, via apt, the apt→dpkg child).",
)

# A device-node-bearing package (the mknod hard-wall vehicle), for completeness.
PROFILE_WITH_DEVNODE = PackageProfile(
    name="devnode-pkg",
    has_compressed_data=True,
    has_maintainer_scripts=False,
    has_device_nodes=True,
    note="a (rare) package shipping a device node → exercises the mknod hard wall "
         "the C shim does not yet intercept.",
)


# --------------------------------------------------------------------------- #
# Decision report — the stage gate map + the per-gate verdict
# --------------------------------------------------------------------------- #
@dataclass(frozen=True)
class GateDecision:
    """The per-stage decision table + the rolled-up verdict for a profile."""

    profile: str
    stages: tuple[Stage, ...]

    # roll-ups
    pure_stages: tuple[str, ...]
    g2_stages: tuple[str, ...]            # fakeroot — host-proven mechanism
    g1_stages: tuple[str, ...]            # exec-re-entry — DEVICE-pending
    host_provable_stages: tuple[str, ...]
    device_pending_stages: tuple[str, ...]

    # the two terminal markers and what they require
    unpacked_requires_g1: bool            # does `unpacked=true` need a G1 stage?
    installed_requires_g1: bool           # does `installed=true` need a G1 stage?
    g1_stage_count: int

    def as_dict(self) -> dict:
        return {
            "profile": self.profile,
            "stages": [s.as_dict() for s in self.stages],
            "pure_stages": list(self.pure_stages),
            "g2_stages": list(self.g2_stages),
            "g1_stages": list(self.g1_stages),
            "host_provable_stages": list(self.host_provable_stages),
            "device_pending_stages": list(self.device_pending_stages),
            "unpacked_requires_g1": self.unpacked_requires_g1,
            "installed_requires_g1": self.installed_requires_g1,
            "g1_stage_count": self.g1_stage_count,
        }


def _stages_on_path_to(target_id: str, stages: tuple[Stage, ...]) -> set[str]:
    """The set of stage ids that must SUCCEED before ``target_id`` (the transitive
    dependency closure of the target node, inclusive). This is how we ask 'does
    reaching <marker> require a G1 stage?' precisely from the graph."""
    by_id = {s.id: s for s in stages}
    if target_id not in by_id:
        return set()
    seen: set[str] = set()
    stack = [target_id]
    while stack:
        sid = stack.pop()
        if sid in seen:
            continue
        seen.add(sid)
        stack.extend(by_id[sid].depends_on)
    return seen


def decide(profile: PackageProfile) -> GateDecision:
    """Classify every stage of ``profile``'s chain and compute the verdict:
    which stages are pure / G2(fakeroot) / G1(exec-re-entry), which are
    host-provable vs device-pending, and whether the `unpacked`/`installed`
    terminal markers transitively require a G1 stage."""
    stages = profile.chain()

    pure = tuple(s.id for s in stages if s.gate is Gate.PURE)
    g2 = tuple(s.id for s in stages if s.gate is Gate.G2)
    g1 = tuple(s.id for s in stages if s.gate is Gate.G1)
    host_ok = tuple(s.id for s in stages if s.host_provable)
    device_pending = tuple(s.id for s in stages if not s.host_provable)

    g1_ids = set(g1)
    unpacked_path = _stages_on_path_to("dpkg_unpacked", stages)
    installed_path = _stages_on_path_to("dpkg_update_db", stages)

    return GateDecision(
        profile=profile.name,
        stages=stages,
        pure_stages=pure,
        g2_stages=g2,
        g1_stages=g1,
        host_provable_stages=host_ok,
        device_pending_stages=device_pending,
        unpacked_requires_g1=bool(unpacked_path & g1_ids),
        installed_requires_g1=bool(installed_path & g1_ids),
        g1_stage_count=len(g1),
    )


# --------------------------------------------------------------------------- #
# Cross-link to dpkg_unpack_model — the G2 meta-stages are HOST-PROVEN there
# --------------------------------------------------------------------------- #
def metadata_stages_host_proof() -> dict:
    """Run :mod:`tools.dpkg_unpack_model` to HOST-prove the G2 meta-stages
    (superuser gate, chown, chmod/stat) this chain tags — closing the loop that
    the host-provable stages of the e2e graph are actually proven, not just
    asserted. Returns the without/with-fakeroot contrast for the synthetic deb."""
    from tools.dpkg_unpack_model import prove

    rep = prove()
    return {
        "without_fakeroot_unpacked": rep.without_fakeroot.unpacked,
        "without_fakeroot_wall": rep.without_fakeroot.failure,
        "with_fakeroot_unpacked": rep.with_fakeroot.unpacked,
        "with_fakeroot_self_consistent": rep.with_fakeroot.self_consistent,
    }


# --------------------------------------------------------------------------- #
# CLI / selftest
# --------------------------------------------------------------------------- #
def _render_table(dec: GateDecision) -> str:
    rows = [
        f"apt install <{dec.profile}> — stage gate map",
        "",
        "| # | stage | proc | gate | ceiling | fakeroot-alone? | depends_on |",
        "|--:|-------|------|:----:|:-------:|:---------------:|------------|",
    ]
    for i, s in enumerate(dec.stages, 1):
        fr = "yes" if s.cleared_by_fakeroot_alone else "NO (G1)"
        opt = " (opt)" if s.optional else ""
        rows.append(
            f"| {i} | {s.title}{opt} | {s.proc.value} | {s.gate.value} | "
            f"{s.ceiling.value} | {fr} | {', '.join(s.depends_on) or '-'} |"
        )
    rows += [
        "",
        f"G1 (exec-re-entry, DEVICE-pending) stages: {len(dec.g1_stages)} "
        f"-> {', '.join(dec.g1_stages) or 'none'}",
        f"G2 (fakeroot, HOST-proven) stages: {len(dec.g2_stages)} "
        f"-> {', '.join(dec.g2_stages)}",
        f"`unpacked=true` requires a G1 stage: {dec.unpacked_requires_g1}",
        f"`installed=true` requires a G1 stage: {dec.installed_requires_g1}",
    ]
    return "\n".join(rows)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="apt_install_e2e_model",
        description="Host model of the full `apt install <pkg>` chain with each "
                    "stage's ALR gate (PURE/G2-fakeroot/G1-exec-re-entry) tagged, "
                    "and the host-proven vs device-pending decision per stage.",
    )
    p.add_argument("--profile",
                   choices=["hello", "hello-dpkg-i", "scripted", "devnode"],
                   default="hello", help="package shape + invocation to model")
    p.add_argument("--json", action="store_true")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest:
        return _selftest()

    profile = {
        "hello": PROFILE_HELLO,
        "hello-dpkg-i": PROFILE_HELLO_DPKG_I,
        "scripted": PROFILE_WITH_SCRIPTS,
        "devnode": PROFILE_WITH_DEVNODE,
    }[args.profile]
    dec = decide(profile)

    if args.json:
        out = dec.as_dict()
        out["metadata_stages_host_proof"] = metadata_stages_host_proof()
        print(json.dumps(out, indent=2))
    else:
        print(_render_table(dec))
        print()
        proof = metadata_stages_host_proof()
        print("G2 meta-stages host proof (via dpkg_unpack_model):")
        print(f"  without fakeroot: unpacked={proof['without_fakeroot_unpacked']} "
              f"(wall: {proof['without_fakeroot_wall'][:60]}…)")
        print(f"  with fakeroot:    unpacked={proof['with_fakeroot_unpacked']} "
              f"self_consistent={proof['with_fakeroot_self_consistent']}")
    return 0


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    hello = decide(PROFILE_HELLO)              # via `apt install`
    hello_di = decide(PROFILE_HELLO_DPKG_I)    # via `dpkg -i` (device drain form)
    scripted = decide(PROFILE_WITH_SCRIPTS)

    # --- the chain is a well-formed DAG (every dep id exists, no self-loop) - #
    for dec in (hello, hello_di, scripted):
        ids = {s.id for s in dec.stages}
        for s in dec.stages:
            for d in s.depends_on:
                if d not in ids or d == s.id:
                    check(f"{dec.profile}: bad edge {s.id}->{d}", False)
    check("chains are well-formed DAGs (edges resolve)", True)

    # --- the central honest conclusion (SSOT §6 concl.2) ------------------- #
    check("hello: `unpacked=true` REQUIRES a G1 stage (compressed-data child)",
          hello.unpacked_requires_g1 is True)
    # via apt: TWO G1 stages (apt forks dpkg + the extract child).
    check("hello via apt: TWO G1 stages (apt->dpkg + extract child)",
          hello.g1_stage_count == 2
          and set(hello.g1_stages) == {"apt_spawn_dpkg", "dpkg_extract_data"})
    # via `dpkg -i` (device drain): the apt outer stages drop → ONE G1 stage.
    check("hello via `dpkg -i`: exactly ONE G1 stage (the extract child)",
          hello_di.g1_stage_count == 1
          and hello_di.g1_stages == ("dpkg_extract_data",))
    check("hello via `dpkg -i`: `unpacked=true` still requires that G1 stage",
          hello_di.unpacked_requires_g1 is True)

    # --- fakeroot-alone clears the meta stages but NOT the terminal --------- #
    g2_only = [s for s in hello.stages if s.gate is Gate.G2]
    check("hello: every G2 stage is cleared by fakeroot alone",
          all(s.cleared_by_fakeroot_alone for s in g2_only))
    check("hello: superuser gate is a G2 (fakeroot) stage",
          any(s.id == "dpkg_superuser_gate" and s.gate is Gate.G2 for s in hello.stages))
    check("hello: chown is a G2 (fakeroot) stage",
          any(s.id == "dpkg_chown" and s.gate is Gate.G2 for s in hello.stages))
    g1_stage = next(s for s in hello.stages if s.gate is Gate.G1)
    check("hello: the G1 stage is NOT cleared by fakeroot alone",
          g1_stage.cleared_by_fakeroot_alone is False)

    # --- hello vs scripted branch: scripts add a SECOND G1 stage ------------ #
    check("scripted has MORE G1 stages than hello (the /bin/sh configure child)",
          scripted.g1_stage_count > hello.g1_stage_count)
    check("scripted: `installed=true` requires a G1 stage (postinst /bin/sh)",
          scripted.installed_requires_g1 is True)
    check("scripted: a preinst stage forks /bin/sh",
          any(s.id == "dpkg_run_preinst" and s.proc is Proc.FORK_EXEC
              for s in scripted.stages))
    check("scripted: a postinst stage forks /bin/sh",
          any(s.id == "dpkg_run_postinst" and s.proc is Proc.FORK_EXEC
              for s in scripted.stages))
    check("hello: NO maintainer-script stages",
          not any(s.id in ("dpkg_run_preinst", "dpkg_run_postinst")
                  for s in hello.stages))
    check("hello: configure is a pure in-proc transition (no /bin/sh)",
          any(s.id == "dpkg_configure_noscript" and s.gate is Gate.PURE
              for s in hello.stages))

    # --- uncompressed-data branch: extract is then PURE, no G1 from it ------ #
    # Use the `dpkg -i` entry so the apt->dpkg G1 stage is absent and we can
    # isolate the claim that an UNCOMPRESSED data.tar contributes NO G1 stage.
    uncompressed = decide(PackageProfile(
        "uncompressed", has_compressed_data=False, has_maintainer_scripts=False,
        entry=Entry.DPKG_I))
    check("uncompressed-data: the extract stage is PURE (no compressor child)",
          any(s.id == "dpkg_extract_data" and s.gate is Gate.PURE
              for s in uncompressed.stages))
    check("uncompressed + no-scripts via `dpkg -i`: `unpacked=true` needs NO G1",
          uncompressed.unpacked_requires_g1 is False)
    check("uncompressed + no-scripts via `dpkg -i`: ZERO G1 stages",
          uncompressed.g1_stage_count == 0)

    # --- device-node branch: the mknod hard-wall stage ---------------------- #
    devnode = decide(PROFILE_WITH_DEVNODE)
    check("devnode: a mknod stage appears (the hard wall vehicle)",
          any(s.id == "dpkg_mknod" for s in devnode.stages))
    check("devnode: hello has NO mknod stage",
          not any(s.id == "dpkg_mknod" for s in hello.stages))

    # --- host-provable vs device-pending partition is exactly G1 ------------ #
    for dec in (hello, scripted):
        host_set = set(dec.host_provable_stages)
        dev_set = set(dec.device_pending_stages)
        all_ids = {s.id for s in dec.stages}
        check(f"{dec.profile}: host/device partition is a clean split",
              host_set | dev_set == all_ids and not (host_set & dev_set))
        check(f"{dec.profile}: device-pending stages == G1 stages exactly",
              dev_set == set(dec.g1_stages))

    # --- the apt outer stages: index/fetch are PURE, dpkg-spawn is G1 ------- #
    check("apt index read is PURE",
          any(s.id == "apt_index" and s.gate is Gate.PURE for s in hello.stages))
    check("apt .deb fetch is PURE/network",
          any(s.id == "apt_fetch" and s.gate is Gate.PURE
              and s.proc is Proc.NETWORK for s in hello.stages))
    check("apt forking dpkg is a G1 exec-re-entry stage",
          any(s.id == "apt_spawn_dpkg" and s.gate is Gate.G1 for s in hello.stages))

    # --- cross-link: the G2 meta-stages are actually HOST-PROVEN ------------ #
    proof = metadata_stages_host_proof()
    check("G2 host proof: without fakeroot the unpack hits the wall",
          proof["without_fakeroot_unpacked"] is False)
    check("G2 host proof: with fakeroot the meta-sequence passes + consistent",
          proof["with_fakeroot_unpacked"] is True
          and proof["with_fakeroot_self_consistent"] is True)

    # --- JSON round-trips --------------------------------------------------- #
    d = hello.as_dict()
    check("as_dict carries every stage with gate/ceiling",
          len(d["stages"]) == len(hello.stages)
          and all("gate" in st and "ceiling" in st for st in d["stages"]))
    check("as_dict is JSON-serialisable", json.dumps(d) is not None)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
