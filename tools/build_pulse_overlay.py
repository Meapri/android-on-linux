"""Build the ALR audio staging overlay (``pulse-stage.tar``) — design §3d.

Why
---
The ALR audio path (docs/design/android-audio-sink.md) runs an **in-app
PulseAudio-native-protocol server** that translates guest playback streams to an
Android AAudio sink. The guest's half is just the stock **libpulse client** plus a
one-line ``client.conf`` that keeps libpulse from autospawning a (non-existent)
real pulse daemon and forces the socket-copy PCM path. This builder stages exactly
that:

  1. The ``libpulse0`` (+ ``libpulse-mainloop-glib0``) runtime closure from Ubuntu
     **noble** (the ALR base — MEMORY ``base-is-ubuntu-noble``), base-subtracted so
     it can never downgrade a base lib (passes the WS-4 ``overlay_guard``; all
     paths are new), pruned of man/doc/locale payload.
  2. ``/etc/pulse/client.conf`` — **address-free** (design §3b recommendation: the
     authoritative server address is the ``PULSE_SERVER`` env WS-1 exports in
     runtime_report.cpp). It only sets ``autospawn=no`` (never fork a server) and
     ``enable-shm=no``/``enable-memfd=no`` (force the socket-copy PCM path our
     server reads with plain ``recv()`` — design §1b).
  3. (optional, ``--with-alsa``) the stock ALSA→pulse bridge
     ``libasound2-plugins`` closure + ``/etc/asound.conf`` so pure-ALSA apps
     (``snd_pcm_open("default")``) route transitively to our pulse server with zero
     custom code (design §3c/§7).

We stage only **client** libs — never the ``pulseaudio`` *server* package (WE are
the server). Output is the §5-E ``./``-rooted overlay tar consumed by the already
wired, guarded ``extractOverlayTar`` slot in MainActivity (the ``pulse`` entry in
the overlay-staging loop). No app change needed beyond that one-word list entry.

Honest scope
------------
HOST-ONLY. This resolves + downloads the noble closure and packs the overlay
host-side (or ``--list`` dry-run to print the plan without downloading). The actual
on-device "guest app plays sound" is the integration/device gate (WS-1 stages the
tar + starts the native sink). Ubuntu .debs use ``data.tar.zst``; extraction needs
the ``zstd`` CLI on PATH (``extract_deb`` raises a clear error otherwise).
"""

from __future__ import annotations

import argparse
import io
import json
import shutil
import tarfile
from dataclasses import dataclass
from pathlib import Path

from tools.deb_closure import (
    base_path_set,
    base_soname_set,
    build_provides_map,
    drop_base_paths,
    drop_base_sonames,
    fetch_packages_index,
    parse_packages,
    prune_paths,
    resolve_closure,
    _download_deb,
    DEFAULT_PRUNE_PREFIXES,
)
from tools.build_stage_tar import build_stage_tar, extract_deb
from tools.stage_tar_spec import validate_stage_tar
from tools.overlay_guard import scan_overlay_violations

# --------------------------------------------------------------------------- #
# Constants — Ubuntu noble (the ALR base), arm64.
# --------------------------------------------------------------------------- #

MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
SUITE = "noble"
ARCH = "arm64"
COMPONENTS = ("main", "universe")  # Ubuntu splits libs across main + universe

# The pulse CLIENT packages (NOT the `pulseaudio` server — we are the server).
PULSE_TARGETS = ("libpulse0", "libpulse-mainloop-glib0")
# Optional ALSA→pulse bridge (stock; lights up pure-ALSA apps for free, §3c/§7).
ALSA_BRIDGE_TARGETS = ("libasound2-plugins",)

# /etc/pulse/client.conf — ADDRESS-FREE (design §3b recommendation). The server
# address is driven by the PULSE_SERVER env (runtime_report.cpp), which overrides
# client.conf; here we only nail off autospawn + force the socket-copy PCM path.
CLIENT_CONF_PATH = "etc/pulse/client.conf"
CLIENT_CONF_BODY = (
    "# /etc/pulse/client.conf — staged by tools/build_pulse_overlay.py (ALR).\n"
    "# Address-free: the authoritative server address is the PULSE_SERVER env\n"
    "# (unix:$XDG_RUNTIME_DIR/pulse/native) exported by the ALR loader.\n"
    "autospawn = no\n"          # never try to fork a (non-existent) real pulse daemon
    "daemon-binary = /bin/true\n"  # belt-and-suspenders: no autospawn target
    "enable-shm = no\n"         # force socket-copy PCM (no memfd/shm) — design §1b
    "enable-memfd = no\n"
)

# /etc/asound.conf — routes ALSA's default PCM/CTL to pulse (only with --with-alsa).
ASOUND_CONF_PATH = "etc/asound.conf"
ASOUND_CONF_BODY = (
    "# /etc/asound.conf — staged by tools/build_pulse_overlay.py (ALR, --with-alsa).\n"
    "# Routes ALSA's default device to our pulse server via the stock\n"
    "# libasound_module_pcm_pulse.so bridge (design §3c/§7).\n"
    "pcm.!default { type pulse }\n"
    "ctl.!default { type pulse }\n"
)


@dataclass
class PulseOverlayResult:
    out_tar: str
    closure: tuple[str, ...] = ()
    targets: tuple[str, ...] = ()
    skipped_base: tuple[str, ...] = ()
    unsupported: tuple[str, ...] = ()
    pruned_count: int = 0
    config_members: tuple[str, ...] = ()
    file_count: int = 0
    violations: tuple[str, ...] = ()
    with_alsa: bool = False

    def as_dict(self) -> dict:
        return {
            "out_tar": self.out_tar,
            "targets": list(self.targets),
            "closure": list(self.closure),
            "skipped_base": list(self.skipped_base),
            "unsupported": list(self.unsupported),
            "pruned_count": self.pruned_count,
            "config_members": list(self.config_members),
            "file_count": self.file_count,
            "violations": list(self.violations),
            "with_alsa": self.with_alsa,
        }


def _inject_config(merged_root: Path, with_alsa: bool) -> list[str]:
    """Write client.conf (+ optional asound.conf) into the merged root.

    Returns the rootfs-relative paths injected (so the caller can report them)."""
    members: list[str] = []
    conf = merged_root / CLIENT_CONF_PATH
    conf.parent.mkdir(parents=True, exist_ok=True)
    conf.write_text(CLIENT_CONF_BODY)
    members.append(CLIENT_CONF_PATH)
    if with_alsa:
        asound = merged_root / ASOUND_CONF_PATH
        asound.parent.mkdir(parents=True, exist_ok=True)
        asound.write_text(ASOUND_CONF_BODY)
        members.append(ASOUND_CONF_PATH)
    return members


def build_pulse_overlay(
    out_tar: str | Path,
    base: str | Path,
    *,
    mirror: str = MIRROR,
    suite: str = SUITE,
    arch: str = ARCH,
    components=COMPONENTS,
    cache_dir: str | Path | None = None,
    with_alsa: bool = False,
    prune=DEFAULT_PRUNE_PREFIXES,
    merged_root_override: str | Path | None = None,
    index_override: dict | None = None,
) -> PulseOverlayResult:
    """Resolve + base-subtract the libpulse client closure, inject client.conf,
    pack the §5-E ``pulse-stage.tar``.

    NETWORK PATH unless ``merged_root_override`` is supplied (offline/selftest):
    when given, that directory is used as the already-extracted closure root and no
    network/download happens — only base-subtraction, config injection and packing.

    ``base`` is the base rootfs (dir or tar) used for the downgrade-safe subtraction
    (every staged lib whose SONAME the base already owns is dropped).
    """
    out_tar = Path(out_tar)
    out_tar.parent.mkdir(parents=True, exist_ok=True)
    targets = list(PULSE_TARGETS) + (list(ALSA_BRIDGE_TARGETS) if with_alsa else [])

    closure: list[str] = []
    unsupported: list[str] = []

    if merged_root_override is not None:
        merged_root = Path(merged_root_override)
    else:
        cache = Path(cache_dir) if cache_dir is not None else Path("/tmp/deb-cache-ubuntu")
        cache.mkdir(parents=True, exist_ok=True)
        index = index_override or parse_packages(
            fetch_packages_index(mirror, suite, arch, components=components)
        )
        provides_map = build_provides_map(index)
        log: list[str] = []
        closure = resolve_closure(targets, index, provides_map=provides_map, log=log)

        merged_root = cache / "_pulse_merged_root"
        if merged_root.exists():
            shutil.rmtree(merged_root)
        merged_root.mkdir(parents=True)

        for name in closure:
            fields = index.get(name, {})
            filename = fields.get("Filename")
            if not filename:
                unsupported.append(f"{name} (no Filename in index)")
                continue
            try:
                deb_path = _download_deb(mirror, filename, cache)
            except Exception as exc:  # network / IO
                unsupported.append(f"{name} (download failed: {exc})")
                continue
            try:
                extract_deb(deb_path, merged_root)
            except NotImplementedError:
                unsupported.append(f"{name} (zstd .deb — need `zstd` CLI on PATH)")
            except Exception as exc:
                unsupported.append(f"{name} (extract failed: {exc})")

    # Base subtraction: drop any lib whose SONAME the base already owns (downgrade
    # protection), then any remaining base-duplicate path, then prune man/doc/etc.
    base_sonames = base_soname_set(base)
    skipped_sonames = drop_base_sonames(merged_root, base_sonames)
    skipped_paths = drop_base_paths(merged_root, base_path_set(base))
    skipped_base = sorted(set(skipped_sonames) | set(skipped_paths))
    pruned = prune_paths(merged_root, prune)

    # Inject our config AFTER subtraction/prune (so it's never dropped).
    config_members = _inject_config(merged_root, with_alsa)

    result = build_stage_tar(merged_root, out_tar)
    violations = scan_overlay_violations(base, out_tar)

    return PulseOverlayResult(
        out_tar=str(out_tar),
        closure=tuple(closure),
        targets=tuple(targets),
        skipped_base=tuple(skipped_base),
        unsupported=tuple(unsupported),
        pruned_count=len(pruned),
        config_members=tuple("./" + m for m in config_members),
        file_count=result.file_count,
        violations=tuple(v.render() for v in violations),
        with_alsa=with_alsa,
    )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_pulse_overlay",
        description="Build the ALR audio staging overlay (pulse-stage.tar): the "
        "stock libpulse client closure + an address-free /etc/pulse/client.conf.",
    )
    parser.add_argument("--out", help="output stage tar (e.g. /tmp/pulse-stage.tar)")
    parser.add_argument("--base", help="base rootfs (dir|tar) for downgrade-safe subtraction")
    parser.add_argument("--suite", default=SUITE)
    parser.add_argument("--mirror", default=MIRROR)
    parser.add_argument("--component", action="append", dest="components",
                        help="repo component (repeatable; default main+universe)")
    parser.add_argument("--cache", default="/tmp/deb-cache-ubuntu")
    parser.add_argument("--with-alsa", action="store_true",
                        help="also stage the stock ALSA->pulse bridge "
                        "(libasound2-plugins) + /etc/asound.conf (design §3c/§7)")
    parser.add_argument("--list", "--dry-run", action="store_true", dest="dry_run",
                        help="resolve the closure + print the plan WITHOUT downloading/packing")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    components = tuple(args.components) if args.components else COMPONENTS
    targets = list(PULSE_TARGETS) + (list(ALSA_BRIDGE_TARGETS) if args.with_alsa else [])

    if args.dry_run:
        index = parse_packages(
            fetch_packages_index(args.mirror, args.suite, ARCH, components=components)
        )
        provides_map = build_provides_map(index)
        log: list[str] = []
        closure = resolve_closure(targets, index, provides_map=provides_map, log=log)
        config = ["./" + CLIENT_CONF_PATH] + (
            ["./" + ASOUND_CONF_PATH] if args.with_alsa else [])
        info = {
            "targets": targets,
            "closure": closure,
            "missing": log,
            "config_members": config,
            "with_alsa": args.with_alsa,
            "suite": args.suite,
            "components": list(components),
        }
        if args.json:
            print(json.dumps(info, indent=2))
        else:
            print(f"pulse overlay plan ({args.suite}/{'+'.join(components)}):")
            print(f"  targets: {', '.join(targets)}")
            print(f"  closure ({len(closure)} pkgs): {', '.join(closure)}")
            print(f"  config members: {', '.join(config)}")
            if log:
                print(f"  unsatisfied (skipped): {len(log)}")
        return 0

    if not args.out:
        parser.error("--out is required for a full build (or use --list / --selftest)")
    if not args.base:
        parser.error("--base (base rootfs) is required for downgrade-safe subtraction")

    res = build_pulse_overlay(
        args.out, args.base, mirror=args.mirror, suite=args.suite, arch=ARCH,
        components=components, cache_dir=args.cache, with_alsa=args.with_alsa,
    )
    if args.json:
        print(json.dumps(res.as_dict(), indent=2))
    else:
        print(f"built {res.out_tar}")
        print(f"  targets: {', '.join(res.targets)}")
        print(f"  closure: {len(res.closure)} pkgs")
        print(f"  files in overlay: {res.file_count}")
        print(f"  config members: {', '.join(res.config_members)}")
        print(f"  base-subtracted (no downgrade): {len(res.skipped_base)}")
        print(f"  pruned (man/doc/locale): {res.pruned_count}")
        if res.unsupported:
            print(f"  UNSUPPORTED (not staged): {len(res.unsupported)}")
            for u in res.unsupported:
                print(f"    - {u}")
        if res.violations:
            print(f"  GUARD VIOLATIONS: {len(res.violations)}")
            for v in res.violations:
                print(f"    - {v}")
    return 1 if res.violations else 0


# --------------------------------------------------------------------------- #
# Selftest (OFFLINE — synthetic merged-root + base; no network)
# --------------------------------------------------------------------------- #

def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    # --- config bodies are correct (address-free; the nail-off lines present) --- #
    check("client.conf is address-free (no default-server line)",
          "default-server" not in CLIENT_CONF_BODY)
    check("client.conf disables autospawn", "autospawn = no" in CLIENT_CONF_BODY)
    check("client.conf disables shm", "enable-shm = no" in CLIENT_CONF_BODY)
    check("client.conf disables memfd", "enable-memfd = no" in CLIENT_CONF_BODY)
    check("asound.conf routes default to pulse", "type pulse" in ASOUND_CONF_BODY)
    check("targets are CLIENT libs only (no `pulseaudio` server pkg)",
          "pulseaudio" not in PULSE_TARGETS)
    check("libpulse0 is a target", "libpulse0" in PULSE_TARGETS)
    check("glib mainloop binding is a target",
          "libpulse-mainloop-glib0" in PULSE_TARGETS)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)

        # synthetic extracted closure root: a libpulse.so.0 + a base-owned lib that
        # MUST be subtracted (libc.so.6) + a doc file that MUST be pruned.
        root = tmp / "merged"
        libdir = root / "usr/lib/aarch64-linux-gnu"
        libdir.mkdir(parents=True)
        # minimal ELF magic so the soname classifier treats these as libs.
        elf = b"\x7fELF" + b"\x00" * 60
        (libdir / "libpulse.so.0").write_bytes(elf)
        (libdir / "libpulsecommon-16.1.so").write_bytes(elf)
        (libdir / "libc.so.6").write_bytes(elf)  # base-owned -> dropped
        docdir = root / "usr/share/doc/libpulse0"
        docdir.mkdir(parents=True)
        (docdir / "copyright").write_text("doc -> pruned\n")

        # synthetic base that already owns libc.so.6 (so it gets subtracted).
        base_dir = tmp / "base"
        base_libdir = base_dir / "usr/lib/aarch64-linux-gnu"
        base_libdir.mkdir(parents=True)
        (base_libdir / "libc.so.6").write_bytes(elf)

        out = tmp / "pulse-stage.tar"
        res = build_pulse_overlay(
            out, base_dir, with_alsa=False, merged_root_override=root,
        )

        with tarfile.open(out) as t:
            names = {m.name for m in t.getmembers()}
            bodies = {}
            for m in t.getmembers():
                if m.isfile():
                    bodies[m.name] = t.extractfile(m).read()

        check("overlay ships ./etc/pulse/client.conf", "./etc/pulse/client.conf" in names)
        check("overlay does NOT ship asound.conf (no --with-alsa)",
              "./etc/asound.conf" not in names)
        check("client.conf body is address-free in the tar",
              b"default-server" not in bodies.get("./etc/pulse/client.conf", b""))
        check("client.conf autospawn=no in the tar",
              b"autospawn = no" in bodies.get("./etc/pulse/client.conf", b""))
        # libpulse.so.0 staged; flat-soname classifier may place it under /usr/lib
        # — accept any path ending in the soname.
        check("overlay stages libpulse.so.0",
              any(n.endswith("libpulse.so.0") for n in names))
        check("overlay subtracts the base-owned libc.so.6 (no downgrade)",
              not any(n.endswith("/libc.so.6") for n in names))
        check("overlay prunes usr/share/doc",
              not any("/usr/share/doc/" in n for n in names))
        check("all members ./-rooted", all(n.startswith("./") for n in names))
        check("result reports libc.so.6 subtracted",
              any("libc.so.6" in s for s in res.skipped_base))
        check("result config_members lists client.conf",
              "./etc/pulse/client.conf" in res.config_members)
        check("no overlay-guard violations", res.violations == ())

        # §5-E structural conformance (+ no base downgrade).
        rep = validate_stage_tar(str(out), base=base_dir)
        check("overlay is stage_tar_spec conformant against the base", rep.conformant)

        # --with-alsa adds /etc/asound.conf.
        out2 = tmp / "pulse-alsa-stage.tar"
        # re-create the merged root (build mutated/pruned the first one in place).
        root2 = tmp / "merged2"
        (root2 / "usr/lib/aarch64-linux-gnu").mkdir(parents=True)
        (root2 / "usr/lib/aarch64-linux-gnu/libpulse.so.0").write_bytes(elf)
        res2 = build_pulse_overlay(
            out2, base_dir, with_alsa=True, merged_root_override=root2,
        )
        with tarfile.open(out2) as t:
            names2 = {m.name for m in t.getmembers()}
        check("--with-alsa overlay ships ./etc/asound.conf",
              "./etc/asound.conf" in names2)
        check("--with-alsa result flags with_alsa", res2.with_alsa is True)
        check("--with-alsa config_members lists asound.conf",
              "./etc/asound.conf" in res2.config_members)

    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
