"""Build the maintainer-script SHIM overlay (``maintscript-shim-stage.tar``) — TASK-A(1).

Why
---
The ALR base rootfs was assembled by extracting files, so it ships almost none of the
small helper programs a Debian ``postinst`` registration script expects. When apt then
installs a gnome-platform app (gnome-calculator), several packages in its install DELTA
fail ``dpkg --configure`` with the device-proven exit-73 cascade. The maintainer-script
forensics (reading the actual noble ``.deb`` control archives) pin the EXACT causes:

  * **libpaper1** ``postinst`` → exit 2: its FIRST line is ``. /usr/share/debconf/
    confmodule`` (unconditional) and the base has NO ``/usr/share/debconf/confmodule`` →
    sourcing a missing file under the default shell errors; then ``db_get libpaper/
    defaultpaper`` + ``ucf …`` need a running debconf frontend + the ``ucf`` tool, both
    absent.
  * **x11-common** ``postinst`` → exit 127: sources the debconf confmodule (``db_purge``)
    and, in environments where a confmodule IS present but no frontend runs, the ``db_*``
    calls / ``update-rc.d`` / ``invoke-rc.d`` resolve to nothing → "command not found".
  * **session-migration** ``postinst``: runs ``deb-systemd-helper --user …`` (each
    ``|| true`` guarded, so benign on its own, but noisy without the tool).

NONE of this registration is meaningful in a non-root, no-systemd, single-process Android
guest: there is no init to ``update-rc.d`` into, no debconf db to seed, no ``--user``
systemd units to enable. So the correct fix is to make those helpers **successful
no-ops**: a stub ``confmodule`` whose ``db_*`` shell functions all ``return 0`` (so every
postinst that merely *reads* a debconf default proceeds with an empty/derived value), a
``policy-rc.d`` returning 101 (deny init actions — the dh_installinit guard), and tiny
``exit 0`` stubs for ``ucf`` / ``update-rc.d`` / ``invoke-rc.d`` / ``deb-systemd-helper`` /
``dpkg-reconfigure``. With these staged, libpaper1 + x11-common + session-migration
``postinst`` run to ``exit 0`` and ``dpkg --configure`` completes → gnome-calculator reaches
``Status: install ok installed``.

This is the same philosophy already used elsewhere in the project: a STUB that satisfies a
read which is meaningless in the sandbox (cf. the common-data machine-id stub, the
``GSETTINGS_BACKEND=memory`` env). It does NOT emulate a kernel-denied syscall or weaken any
sandbox — every member is a plain rootfs file/stub script.

Scope / honesty
---------------
* This unblocks the **debconf + init-registration** class of postinst failure (libpaper1,
  x11-common, session-migration, and similar X11/debconf packages). It does NOT make a
  DAEMON's *function* appear (cups/avahi/polkit still have no running service — those are
  reachability-class-A, out of scope) and it does NOT run ``glib-compile-schemas`` (that is
  the common-data ``schemas`` overlay's job — ship a host-precompiled gschemas.compiled).
* The ``confmodule`` stub deliberately makes ``db_get`` return an EMPTY ``$RET``. A postinst
  that REQUIRES a specific seeded value (rare) would get the empty/derived default; for the
  targeted set (libpaper paper-size, x11 registration) that is correct/harmless.
* Pure builder: ``--out`` writes the §5-E ``./``-rooted tar (no network — all stubs are
  generated text). ``--selftest`` is offline and asserts the stub set + that the stub
  confmodule drives the real libpaper1 postinst body to exit 0.
"""

from __future__ import annotations

import argparse
import io
import subprocess
import tarfile
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------- #
# The stub files (rootfs-relative path -> (mode, body))
# --------------------------------------------------------------------------- #

# A debconf confmodule whose db_* are all successful no-ops. Sourcing this makes every
# `. /usr/share/debconf/confmodule; db_get X; …; set -e` postinst proceed (empty $RET).
_CONFMODULE = """\
# ALR maintainer-script shim: a NO-OP debconf confmodule.
#
# The real confmodule speaks the debconf FD protocol to a running frontend. In the ALR
# guest there is no frontend and no debconf db, so we define every db_* command as a shell
# function that succeeds and yields an empty result. This lets a postinst that only READS a
# debconf default (libpaper1's paper size, x11-common's db_purge) run to completion under
# `set -e` instead of dying when it sources a missing confmodule or calls an undefined db_*.
db_get()       { RET=""; return 0; }
db_set()       { return 0; }
db_reset()     { return 0; }
db_fset()      { return 0; }
db_fget()      { RET=""; return 0; }
db_register()  { return 0; }
db_unregister(){ return 0; }
db_purge()     { return 0; }
db_metaget()   { RET=""; return 0; }
db_subst()     { return 0; }
db_input()     { return 0; }
db_go()        { return 0; }
db_capb()      { return 0; }
db_settitle()  { return 0; }
db_title()     { return 0; }
db_beginblock(){ return 0; }
db_endblock()  { return 0; }
db_stop()      { return 0; }
db_version()   { RET="2.0"; return 0; }
db_x_loadtemplatefile() { return 0; }
"""

# policy-rc.d: 101 = "do not run any init-script action" (the dh_installinit invoke-rc.d
# guard honours this), so any postinst that DOES find an init script still no-ops cleanly.
_POLICY_RC_D = """\
#!/bin/sh
# ALR maintainer-script shim: deny all init-script actions (no init in the guest).
exit 101
"""

# Generic `exit 0` stub for tools whose only job is registration meaningless in the guest.
_NOOP_TOOL = """\
#!/bin/sh
# ALR maintainer-script shim: no-op stand-in (registration is inert in the ALR guest).
exit 0
"""

# rootfs-relative path -> (octal mode, body). Scripts are 0755; the confmodule is sourced
# (not exec'd) so 0644 is fine but 0755 is harmless and matches the real file.
STUB_FILES: dict[str, tuple[int, str]] = {
    "usr/share/debconf/confmodule": (0o644, _CONFMODULE),
    "usr/sbin/policy-rc.d": (0o755, _POLICY_RC_D),
    "usr/bin/ucf": (0o755, _NOOP_TOOL),
    "usr/bin/ucfr": (0o755, _NOOP_TOOL),
    "usr/sbin/update-rc.d": (0o755, _NOOP_TOOL),
    "usr/sbin/invoke-rc.d": (0o755, _NOOP_TOOL),
    "usr/bin/deb-systemd-helper": (0o755, _NOOP_TOOL),
    "usr/bin/deb-systemd-invoke": (0o755, _NOOP_TOOL),
    "usr/sbin/dpkg-reconfigure": (0o755, _NOOP_TOOL),
}


@dataclass
class ShimOverlayResult:
    out_tar: str
    members: tuple[str, ...] = ()
    file_count: int = 0

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "members": list(self.members),
            "file_count": self.file_count,
        }


def _add_dir(tar: tarfile.TarFile, rel: str) -> None:
    ti = tarfile.TarInfo("./" + rel.strip("/") + "/")
    ti.type = tarfile.DIRTYPE
    ti.mode = 0o755
    ti.mtime = 0
    tar.addfile(ti)


def _add_file(tar: tarfile.TarFile, rel: str, data: bytes, mode: int) -> None:
    ti = tarfile.TarInfo("./" + rel.strip("/"))
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = 0
    ti.type = tarfile.REGTYPE
    tar.addfile(ti, io.BytesIO(data))


def build_maintscript_shim_overlay(out_tar: str | Path) -> ShimOverlayResult:
    """Pack the no-op maintainer-script shims into a §5-E ``./``-rooted overlay tar."""
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    members: list[str] = []
    # parent dirs we touch (so the extractor has them; harmless if the base already has them)
    dirs = sorted({str(Path(p).parent).strip(".") or "" for p in STUB_FILES}
                  | {"usr", "usr/bin", "usr/sbin", "usr/share", "usr/share/debconf"})
    with tarfile.open(out_tar, "w") as tar:
        for d in sorted(d for d in dirs if d):
            _add_dir(tar, d)
        for rel, (mode, body) in STUB_FILES.items():
            _add_file(tar, rel, body.encode(), mode)
            members.append("./" + rel)
    res = ShimOverlayResult(out_tar=str(out_tar))
    with tarfile.open(out_tar, "r") as t:
        res.file_count = sum(1 for m in t.getmembers() if m.isfile())
    res.members = tuple(sorted(members))
    return res


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_maintscript_shim_overlay",
        description="Build the no-op maintainer-script shim overlay (confmodule + "
        "policy-rc.d + ucf/update-rc.d/invoke-rc.d/deb-systemd-helper stubs) that lets "
        "gnome-platform postinsts (libpaper1/x11-common/session-migration) exit 0 so "
        "`dpkg --configure` completes.",
    )
    parser.add_argument("--out", help="output stage tar (e.g. out/maintscript-shim-stage.tar)")
    parser.add_argument("--base", help="base rootfs (tar|dir) to §5-E-validate against")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.out:
        parser.error("--out is required (or use --selftest)")

    res = build_maintscript_shim_overlay(args.out)
    if args.json:
        import json
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}: {res.file_count} stub files")
        for m in res.members:
            print(f"  {m}")
    if args.base:
        from tools.stage_tar_spec import validate_stage_tar
        rep = validate_stage_tar(res.out_tar, base=args.base)
        print(f"stage_tar_spec: {'CONFORMANT' if rep.conformant else 'NON-CONFORMANT'} "
              f"errors={rep.errors[:3]}")
        return 0 if rep.conformant else 1
    return 0


def _selftest() -> int:
    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        out = tmp / "maintscript-shim-stage.tar"
        res = build_maintscript_shim_overlay(out)
        with tarfile.open(out) as tar:
            names = {m.name for m in tar.getmembers()}
            bodies = {m.name: tar.extractfile(m).read()
                      for m in tar.getmembers() if m.isfile()}
            modes = {m.name: m.mode for m in tar.getmembers() if m.isfile()}

        check("ships ./usr/share/debconf/confmodule",
              "./usr/share/debconf/confmodule" in names)
        check("ships ./usr/sbin/policy-rc.d", "./usr/sbin/policy-rc.d" in names)
        check("ships ./usr/bin/ucf", "./usr/bin/ucf" in names)
        check("ships ./usr/sbin/update-rc.d", "./usr/sbin/update-rc.d" in names)
        check("ships ./usr/sbin/invoke-rc.d", "./usr/sbin/invoke-rc.d" in names)
        check("ships ./usr/bin/deb-systemd-helper",
              "./usr/bin/deb-systemd-helper" in names)
        check("ships ./usr/sbin/dpkg-reconfigure",
              "./usr/sbin/dpkg-reconfigure" in names)
        check("all members ./-rooted", all(n.startswith("./") for n in names))
        check("policy-rc.d exits 101",
              b"exit 101" in bodies.get("./usr/sbin/policy-rc.d", b""))
        check("policy-rc.d is executable (0755)",
              modes.get("./usr/sbin/policy-rc.d", 0) & 0o111 != 0)
        check("ucf stub is executable (0755)",
              modes.get("./usr/bin/ucf", 0) & 0o111 != 0)
        check("confmodule defines db_get/db_purge as functions",
              b"db_get()" in bodies.get("./usr/share/debconf/confmodule", b"")
              and b"db_purge()" in bodies.get("./usr/share/debconf/confmodule", b""))
        check("res.file_count matches stub set", res.file_count == len(STUB_FILES))

        # --- functional: the stub confmodule drives the REAL libpaper1 postinst body
        #     (the device-proven exit-2 culprit) to exit 0. ----------------------------
        cm = tmp / "confmodule"
        cm.write_bytes(bodies["./usr/share/debconf/confmodule"])
        etc = tmp / "etc"; etc.mkdir()
        # a faithful slice of libpaper1's postinst: source confmodule, set -e, db_get,
        # write the default, (ucf is a no-op stub on device → emulate with cp).
        post = tmp / "libpaper1.postinst"
        post.write_text(
            "#!/bin/sh\n"
            f". {cm}\n"
            "set -e\n"
            'if [ "$1" ]; then\n'
            "  db_get libpaper/defaultpaper\n"
            f'  echo "$RET" > {etc}/papersize.dpkg-inst\n'
            f"  cp {etc}/papersize.dpkg-inst {etc}/papersize\n"
            "fi\n"
            "exit 0\n"
        )
        try:
            r = subprocess.run(["sh", str(post), "configure"],
                               capture_output=True, timeout=10)
            check("stub confmodule drives libpaper1 postinst body to exit 0",
                  r.returncode == 0)
        except Exception as exc:  # pragma: no cover
            check(f"libpaper1 postinst emulation ({exc!r})", False)

        # --- §5-E conformance --------------------------------------------------------- #
        try:
            from tools.stage_tar_spec import validate_stage_tar
            rep = validate_stage_tar(str(out))
            check("overlay is stage_tar_spec conformant", rep.conformant and not rep.errors)
        except Exception as exc:  # pragma: no cover
            check(f"stage_tar_spec cross-check ({exc!r})", False)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
