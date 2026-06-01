"""Build an apt-config cleanup overlay (WS-4 §10c) — Ubuntu-noble-only apt.

The base ships correct Ubuntu noble sources (`etc/apt/sources.list.d/ubuntu.sources`,
noble ports) but ALSO leftover third-party lists — `github-cli.list` and
`tailscale.list` — whose signing keys aren't both present (tailscale's keyring is
missing). `apt update` then emits GPG/repo errors for those sources. They are noise
for the ALR rootfs.

This overlay neutralizes the third-party lists (ships them EMPTY, so the device
extractor overwrites the populated ones) and leaves the noble `ubuntu.sources`
untouched → a clean Ubuntu-noble-only `apt update`. Pairs with build_dpkg_db.py
(the reconstructed dpkg admin DB) to make in-app apt/dpkg usable.
"""

from __future__ import annotations

import argparse
import tarfile
from dataclasses import dataclass
from pathlib import Path

# third-party source lists to neutralize (ship empty → overwrites the base's)
DEFAULT_NEUTRALIZE = (
    "etc/apt/sources.list.d/github-cli.list",
    "etc/apt/sources.list.d/tailscale.list",
)


@dataclass(frozen=True)
class AptOverlayResult:
    out_tar: str
    neutralized: tuple[str, ...]


def build_apt_overlay(out_tar: str | Path, *, neutralize=DEFAULT_NEUTRALIZE) -> AptOverlayResult:
    """Write a §5-E overlay that empties each path in ``neutralize``."""
    out_tar = Path(out_tar)
    banner = b"# neutralized by ALR (WS-4 sec.10c): third-party repo disabled on the rootfs\n"
    with tarfile.open(out_tar, "w") as tar:
        for rel in neutralize:
            rel_n = rel.lstrip("./").lstrip("/")
            ti = tarfile.TarInfo("./" + rel_n)
            ti.size = len(banner)
            ti.mode = 0o644
            tar.addfile(ti, _bio(banner))
    return AptOverlayResult(out_tar=str(out_tar), neutralized=tuple(neutralize))


def _bio(data: bytes):
    import io

    return io.BytesIO(data)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="build_apt_overlay",
        description="Build an apt-config cleanup overlay (neutralize third-party sources).",
    )
    parser.add_argument("--out", help="output tar (e.g. /tmp/apt-config-stage.tar)")
    parser.add_argument("--base", help="base rootfs tar/dir to validate against")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.out:
        parser.error("--out is required (or use --selftest)")
    res = build_apt_overlay(args.out)
    print(f"built {res.out_tar}: neutralized {len(res.neutralized)} third-party source(s)")
    for n in res.neutralized:
        print(f"    - {n}")
    if args.base:
        from tools.stage_tar_spec import validate_stage_tar

        rep = validate_stage_tar(res.out_tar, base=args.base)
        print(f"stage_tar_spec: {'CONFORMANT' if rep.conformant else 'NON-CONFORMANT'} {rep.errors}")
        return 0 if rep.conformant else 1
    return 0


def _selftest() -> int:
    import tempfile

    failures = 0

    def check(label: str, cond: bool) -> None:
        nonlocal failures
        if not cond:
            failures += 1
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}")

    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "apt-config-stage.tar"
        res = build_apt_overlay(out)
        with tarfile.open(out) as t:
            members = {m.name: m for m in t.getmembers()}
            ghc = t.extractfile("./etc/apt/sources.list.d/github-cli.list").read()
        check("github-cli.list present + empty-of-repo", b"deb " not in ghc and b"neutralized" in ghc)
        check("tailscale.list present", "./etc/apt/sources.list.d/tailscale.list" in members)
        check("does NOT touch ubuntu.sources", not any("ubuntu.sources" in n for n in members))
        from tools.stage_tar_spec import validate_stage_tar

        rep = validate_stage_tar(str(out))
        check("stage_tar_spec conformant", rep.conformant)
    print(f"\nselftest: {'ALL PASS' if failures == 0 else str(failures) + ' FAILED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
