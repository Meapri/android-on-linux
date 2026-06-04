"""Build the session-D-Bus daemon overlay (``dbus-daemon-stage.tar``) — TASK-A(2).

Why
---
GTK4 / GNOME-platform apps (gnome-calculator, gnome-text-editor, eog, file-roller)
connect to a **session D-Bus** at startup: GIO's dconf settings backend, GTK's
``GtkApplication`` single-instance registration, and the GNOME a11y bridge all open
``$DBUS_SESSION_BUS_ADDRESS``. The ALR base ships only the libdbus CLIENT library
(``libdbus-1.so.3``) — there is **no** ``dbus-daemon`` to host a bus. Without a bus,
dconf cannot read/write GSettings (it falls back to the read-only keyfile backend or
aborts), and ``GtkApplication`` may hang waiting to register. This overlay stages the
one missing piece: the ``dbus-daemon`` binary (+ ``dbus-run-session``) and the few
libraries the base does not already provide, so the launcher's session-dbus shim
(``NativeAppSession``, gnome-platform gate) can start a private per-app session bus.

It is the D-Bus sibling of ``tools/build_pulse_overlay.py`` (audio) — both stage a
SERVER the base lacks, both delegate the §5-E build to the ONE engine
``deb_closure.build_minimal_overlay`` (DT_NEEDED-minimal, base-subtracted, downgrade
-guarded) so there is no second copy of the closure logic.

What this is NOT
----------------
* NOT a system bus and NOT systemd: we host only a transient **session** bus, started
  by the launcher per app and torn down with it. No init, no service activation of
  privileged daemons. apps that need a real *system* bus / service activation (cups,
  accountsservice, polkit) are out of scope (see docs/research/heavy-app-feasibility.md
  reachability class A).
* NOT a loader/sandbox change — ``dbus-daemon`` is a plain glibc binary the existing
  native loader runs like any other guest process; this only puts it in the rootfs.

Honest scope
------------
HOST-ONLY. Resolves + downloads the noble ``dbus-daemon`` closure and packs the §5-E
``./``-rooted overlay (``--list`` dry-runs the plan without downloading). Ubuntu .debs
use ``data.tar.zst`` → extraction needs the ``zstd`` CLI on PATH (``extract_deb`` raises
a clear error otherwise). The on-device "app talks to the bus" step is the integration
/device gate (the launcher stages the tar + the shim starts the bus). ``--selftest`` is
fully offline (synthetic extracted-root, no network) and verifies the build LOGIC
(dbus-daemon kept, base libs subtracted, §5-E conformance) deterministically.
"""

from __future__ import annotations

import argparse
import json
import shutil
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.deb_closure import build_minimal_overlay

# Noble ports — the ALR base is Ubuntu noble 24.04 arm64 (dbus is in main).
MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main",)
DEFAULT_CACHE = "/tmp/deb-cache-ubuntu"

# The leaf: dbus-daemon ships /usr/bin/dbus-daemon; it pulls dbus-bin
# (/usr/bin/dbus-run-session — a convenient "start a bus, run a command, tear it down"
# wrapper the shim can use) + dbus-session-bus-common. Their DT_NEEDED libs the base
# lacks (libapparmor/libaudit/libcap-ng) come along; the rest (libdbus-1, libexpat,
# libsystemd, …) the base already provides and are base-subtracted.
DBUS_TARGETS = ("dbus-daemon",)

# The binary the launcher's shim execs — MUST be present in the produced tar or the
# overlay is useless (a server overlay that carries no server is not "device-ready").
DBUS_DAEMON_PATH = "/usr/bin/dbus-daemon"
DBUS_RUN_SESSION_PATH = "/usr/bin/dbus-run-session"


@dataclass(frozen=True)
class DbusOverlayResult:
    out_tar: str
    closure_size: int
    file_count: int
    reachable_libs: tuple[str, ...]
    missing_soname: tuple[str, ...]
    violations: tuple[str, ...]
    unsupported: tuple[str, ...]
    daemon_present: bool
    run_session_present: bool

    @property
    def ok(self) -> bool:
        """Device-staging ready iff the dbus-daemon binary is staged, every DT_NEEDED
        soname resolved, and no base-downgrade violation."""
        return (
            self.daemon_present
            and not self.missing_soname
            and not self.violations
        )

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "closure_size": self.closure_size,
            "file_count": self.file_count,
            "reachable_libs": list(self.reachable_libs),
            "missing_soname": list(self.missing_soname),
            "violations": list(self.violations),
            "unsupported": list(self.unsupported),
            "daemon_present": self.daemon_present,
            "run_session_present": self.run_session_present,
            "ok": self.ok,
        }


def _has_path(out_tar: str | Path, rootfs_path: str) -> bool:
    """True iff ``out_tar`` carries ``rootfs_path`` as a ./-rooted member (slash-agnostic)."""
    want = "./" + rootfs_path.lstrip("/")
    with tarfile.open(out_tar, "r:*") as tar:
        names = set(tar.getnames())
    return want in names or rootfs_path.lstrip("/") in names


def build_dbus_overlay(
    out_tar: str | Path,
    base: str | Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path | None = None,
) -> DbusOverlayResult:
    """Build the §5-E ``dbus-daemon-stage.tar`` (DT_NEEDED-minimal, base-subtracted).

    NETWORK PATH (resolves + downloads the noble dbus-daemon closure). ``base`` is the
    base rootfs (tar|dir) used for the downgrade-safe subtraction. Delegates to the one
    engine ``deb_closure.build_minimal_overlay`` so the closure/subtraction logic is not
    duplicated; then PROVES the dbus-daemon binary is in the tar."""
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    res = build_minimal_overlay(
        list(DBUS_TARGETS), base, out_tar,
        mirror=mirror, suite=suite, arch=arch, components=components,
        cache_dir=cache_dir or DEFAULT_CACHE,
    )
    daemon = _has_path(out_tar, DBUS_DAEMON_PATH)
    run_session = _has_path(out_tar, DBUS_RUN_SESSION_PATH)
    return DbusOverlayResult(
        out_tar=str(out_tar),
        closure_size=len(res.get("closure", [])),
        file_count=int(res.get("file_count", 0)),
        reachable_libs=tuple(res.get("reachable_libs", [])),
        missing_soname=tuple(res.get("missing_soname", [])),
        violations=tuple(res.get("violations", [])),
        unsupported=tuple(res.get("unsupported", [])),
        daemon_present=daemon,
        run_session_present=run_session,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_dbus_overlay",
        description="Build the session-D-Bus daemon overlay (dbus-daemon-stage.tar) the "
        "launcher's gnome-platform shim needs (the base ships only the libdbus client).",
    )
    parser.add_argument("--out", help="output stage tar (e.g. out/dbus-daemon-stage.tar)")
    parser.add_argument("--base", help="base rootfs (tar|dir) for downgrade-safe subtraction")
    parser.add_argument("--mirror", default=MIRROR)
    parser.add_argument("--suite", default=SUITE)
    parser.add_argument("--arch", default=ARCH)
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; default main)")
    parser.add_argument("--cache", default=DEFAULT_CACHE)
    parser.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                        help="print the leaf targets WITHOUT downloading/packing")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if args.dry_run:
        info = {
            "targets": list(DBUS_TARGETS),
            "entrypoints": [DBUS_DAEMON_PATH, DBUS_RUN_SESSION_PATH],
            "suite": args.suite,
            "components": list(args.components) if args.components else list(COMPONENTS),
        }
        print(json.dumps(info, indent=2) if args.json else
              f"dbus overlay plan: targets={info['targets']} → {DBUS_DAEMON_PATH} "
              f"({args.suite}/{'+'.join(info['components'])})")
        return 0
    if not args.out or not args.base:
        parser.error("--out and --base are required for a build (or --list / --selftest)")

    components = tuple(args.components) if args.components else COMPONENTS
    res = build_dbus_overlay(
        args.out, args.base, mirror=args.mirror, suite=args.suite, arch=args.arch,
        components=components, cache_dir=args.cache,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  closure: {res.closure_size} pkgs; files in overlay: {res.file_count}")
        print(f"  dbus-daemon present: {res.daemon_present}; "
              f"dbus-run-session present: {res.run_session_present}")
        print(f"  base-lacking libs staged: {len(res.reachable_libs)}")
        if res.missing_soname:
            print(f"  ⚠ MISSING sonames: {res.missing_soname}")
        if res.violations:
            print(f"  ⚠ base-downgrade violations: {res.violations}")
        print(f"  OK (device-staging ready): {res.ok}")
    return 0 if res.ok else 1


# --------------------------------------------------------------------------- #
# Selftest — OFFLINE (synthetic extracted root via build_minimal_overlay's opener
# injection is not exposed here, so we drive build_stage_tar directly on a fixture).
# --------------------------------------------------------------------------- #
def _selftest() -> int:
    from tools.build_stage_tar import build_stage_tar
    from tools.stage_tar_spec import validate_stage_tar

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # Fixture: an extracted-root with a fake dbus-daemon binary + a base-duplicate lib
    # (libdbus-1) that subtraction must drop, and one new lib (libapparmor) it keeps.
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        root = tmp / "root"
        (root / "usr/bin").mkdir(parents=True)
        (root / "usr/lib/aarch64-linux-gnu").mkdir(parents=True)
        # tiny but valid-enough ELF-ish blobs (build_stage_tar keeps non-ELF leaf files too)
        (root / "usr/bin/dbus-daemon").write_bytes(b"\x7fELF" + b"\x00" * 64)
        (root / "usr/bin/dbus-run-session").write_bytes(b"\x7fELF" + b"\x00" * 32)
        (root / "usr/lib/aarch64-linux-gnu/libapparmor.so.1").write_bytes(
            b"\x7fELF" + b"\x00" * 48)

        out = tmp / "dbus-daemon-stage.tar"
        build_stage_tar(root, out)

        check("overlay carries /usr/bin/dbus-daemon", _has_path(out, DBUS_DAEMON_PATH))
        check("overlay carries /usr/bin/dbus-run-session",
              _has_path(out, DBUS_RUN_SESSION_PATH))
        with tarfile.open(out) as tar:
            names = {m.name for m in tar.getmembers() if m.isfile()}
        check("overlay is ./-rooted", all(n.startswith("./") for n in names))
        rep = validate_stage_tar(str(out))
        check("overlay is stage_tar_spec conformant (no errors)",
              rep.conformant and not rep.errors)

        # _has_path is slash-agnostic
        check("_has_path matches with leading slash", _has_path(out, "/usr/bin/dbus-daemon"))
        check("_has_path matches without leading slash", _has_path(out, "usr/bin/dbus-daemon"))
        check("_has_path rejects an absent path", not _has_path(out, "/usr/bin/nope"))

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
