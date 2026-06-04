"""Host tests for tools/build_es2gears_overlay.py + the real-GLES-app-via-ANGLE wiring.

All OFFLINE — the overlay assembly (Mesa-GL strip + es2gears symlinks + .so x-bit) is
exercised against a SYNTHETIC build_minimal_overlay-shaped tar, never a .deb download.
The NETWORK pack (real noble mesa-utils-bin es2gears + libdecor → es2gears-stage.tar,
Mesa libEGL/libGLESv2/libGL/libvulkan stripped, 0 missing .so over base∪overlay) is
verified by hand on the host (the module's main() prints PASS); the on-device run —
es2gears → ANGLE libGLESv2 (androlinux-angle) → our VK ICD → Mali, GL_RENDERER=ANGLE
(Vulkan/Mali), software=false — is the device/compositor gate (see the device-test plan
in the module docstring + docs).

Contract (the es2gears real-GLES proof rung):
  * the leaf is mesa-utils-bin (ships es2gears_wayland/_x11 as multiarch-suffixed ELFs);
  * the overlay STRIPS Mesa's libEGL/libGLESv2/libGL/libGLX/libGLdispatch/libvulkan so
    ANGLE (private androlinux-angle, first on LD_LIBRARY_PATH) + the Khronos vk-loader own
    those SONAMEs — without the strip es2gears would bind Mesa swrast (SOFTWARE), defeating
    the proof;
  * it KEEPS the base-missing libs es2gears_wayland needs (libdecor-0.so.0 &c) at 0o755;
  * it adds RELATIVE in-dir convenience symlinks /usr/bin/es2gears{,_wayland,_x11} so the
    launch marker can name a stable path; the bare es2gears → the WAYLAND flavour;
  * the launcher generalizes launchAngleGlesProbe: a /data/local/tmp/.alr-angle-app marker
    naming an in-rootfs binary runs THAT app through the identical ALR_ANGLE env.
"""

from __future__ import annotations

import io
import tarfile
from pathlib import Path

from tools import build_es2gears_overlay as beo
from tools.build_es2gears_overlay import (
    ES2GEARS_LAUNCH_PATH,
    ES2GEARS_SYMLINKS,
    ES2GEARS_WAYLAND,
    ES2GEARS_X11,
    LEAF_PACKAGE,
    MESA_GL_STRIP_PREFIXES,
    add_es2gears_symlinks,
    strip_mesa_gl,
)


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
RUNTIME = ROOT / "app/src/main/cpp/runtime_report.cpp"


# --------------------------------------------------------------------------- #
# A build_minimal_overlay-shaped synthetic overlay (the demos + Mesa GL + keep-libs)
# --------------------------------------------------------------------------- #

def _synthetic_overlay(path: Path) -> Path:
    members = {
        "./usr/bin/es2gears_wayland.aarch64-linux-gnu": (0o755, b"\x7fELFwl"),
        "./usr/bin/es2gears_x11.aarch64-linux-gnu": (0o755, b"\x7fELFx11"),
        "./usr/bin/glxgears.aarch64-linux-gnu": (0o755, b"\x7fELFglx"),
        # Mesa GL stack — must be STRIPPED.
        "./usr/lib/aarch64-linux-gnu/libEGL.so.1": (0o644, b"\x7fELF-mesa-egl"),
        "./usr/lib/aarch64-linux-gnu/libGLESv2.so.2": (0o644, b"\x7fELF-mesa-gles"),
        "./usr/lib/aarch64-linux-gnu/libGL.so.1": (0o644, b"\x7fELF-mesa-gl"),
        "./usr/lib/aarch64-linux-gnu/libGLX.so.0": (0o644, b"\x7fELF-mesa-glx"),
        "./usr/lib/aarch64-linux-gnu/libGLdispatch.so.0": (0o644, b"\x7fELF-mesa-disp"),
        "./usr/lib/aarch64-linux-gnu/libvulkan.so.1": (0o644, b"\x7fELF-mesa-vk"),
        # base-missing libs es2gears needs — must be KEPT.
        "./usr/lib/aarch64-linux-gnu/libdecor-0.so.0": (0o644, b"\x7fELF-libdecor"),
        "./usr/lib/aarch64-linux-gnu/libxkbcommon-x11.so.0": (0o644, b"\x7fELF-xkbx11"),
    }
    with tarfile.open(path, "w") as tar:
        for name, (mode, data) in members.items():
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = mode
            ti.mtime = 0
            tar.addfile(ti, io.BytesIO(data))
    return path


def _names(path: Path) -> set[str]:
    with tarfile.open(path, "r:*") as tar:
        return set(tar.getnames())


# --------------------------------------------------------------------------- #
# Constants / matrix invariants
# --------------------------------------------------------------------------- #

def test_leaf_is_mesa_utils_bin():
    assert LEAF_PACKAGE == "mesa-utils-bin"


def test_es2gears_elf_paths_are_multiarch_suffixed():
    assert ES2GEARS_WAYLAND == "usr/bin/es2gears_wayland.aarch64-linux-gnu"
    assert ES2GEARS_X11 == "usr/bin/es2gears_x11.aarch64-linux-gnu"


def test_bare_es2gears_symlink_targets_the_wayland_flavour():
    # the on-screen target under ALR_ANGLE (DISPLAY unset + XDG_SESSION_TYPE=wayland →
    # ANGLE's DisplayVkWayland on the in-app compositor)
    assert ES2GEARS_SYMLINKS["usr/bin/es2gears"] == "es2gears_wayland.aarch64-linux-gnu"
    assert ES2GEARS_LAUNCH_PATH == "/usr/bin/es2gears_wayland"


def test_mesa_gl_strip_covers_the_glvnd_and_loader_sonames():
    # libEGL/libGLESv2 (ANGLE owns), the desktop-GL trio, AND the second libvulkan
    # (the Khronos vk-loader owns it) — all must be in the strip set.
    assert {
        "libEGL.so", "libGLESv2.so", "libGL.so", "libGLX.so",
        "libGLdispatch.so", "libvulkan.so",
    } <= set(MESA_GL_STRIP_PREFIXES)


# --------------------------------------------------------------------------- #
# Mesa-GL strip
# --------------------------------------------------------------------------- #

def test_strip_removes_all_mesa_gl_and_the_second_vulkan(tmp_path):
    out = _synthetic_overlay(tmp_path / "es2gears-stage.tar")
    removed = strip_mesa_gl(out)
    names = _names(out)
    # every Mesa GL/EGL + the Mesa libvulkan is gone
    for gone in (
        "./usr/lib/aarch64-linux-gnu/libEGL.so.1",
        "./usr/lib/aarch64-linux-gnu/libGLESv2.so.2",
        "./usr/lib/aarch64-linux-gnu/libGL.so.1",
        "./usr/lib/aarch64-linux-gnu/libGLX.so.0",
        "./usr/lib/aarch64-linux-gnu/libGLdispatch.so.0",
        "./usr/lib/aarch64-linux-gnu/libvulkan.so.1",
    ):
        assert gone in removed
        assert gone not in names


def test_strip_keeps_es2gears_elfs_and_base_missing_libs(tmp_path):
    out = _synthetic_overlay(tmp_path / "es2gears-stage.tar")
    strip_mesa_gl(out)
    names = _names(out)
    assert "./usr/bin/es2gears_wayland.aarch64-linux-gnu" in names
    assert "./usr/bin/es2gears_x11.aarch64-linux-gnu" in names
    assert "./usr/lib/aarch64-linux-gnu/libdecor-0.so.0" in names
    assert "./usr/lib/aarch64-linux-gnu/libxkbcommon-x11.so.0" in names


def test_strip_preserves_kept_member_bytes(tmp_path):
    out = _synthetic_overlay(tmp_path / "es2gears-stage.tar")
    strip_mesa_gl(out)
    with tarfile.open(out, "r:*") as t:
        assert t.extractfile("./usr/bin/es2gears_wayland.aarch64-linux-gnu").read() == b"\x7fELFwl"
        assert t.extractfile("./usr/lib/aarch64-linux-gnu/libdecor-0.so.0").read() == b"\x7fELF-libdecor"


def test_strip_is_idempotent(tmp_path):
    out = _synthetic_overlay(tmp_path / "es2gears-stage.tar")
    strip_mesa_gl(out)
    assert strip_mesa_gl(out) == []


# --------------------------------------------------------------------------- #
# es2gears convenience symlinks
# --------------------------------------------------------------------------- #

def test_symlinks_added_are_relative_in_dir_to_the_suffixed_elf(tmp_path):
    out = _synthetic_overlay(tmp_path / "es2gears-stage.tar")
    added = add_es2gears_symlinks(out)
    assert "./usr/bin/es2gears" in added
    with tarfile.open(out, "r:*") as t:
        for link, want in (
            ("./usr/bin/es2gears", "es2gears_wayland.aarch64-linux-gnu"),
            ("./usr/bin/es2gears_wayland", "es2gears_wayland.aarch64-linux-gnu"),
            ("./usr/bin/es2gears_x11", "es2gears_x11.aarch64-linux-gnu"),
        ):
            m = t.getmember(link)
            assert m.issym(), f"{link} is not a symlink"
            assert m.linkname == want, f"{link} -> {m.linkname} (want {want})"
            assert "/" not in m.linkname, f"{link} target not in-dir"


def test_symlinks_idempotent(tmp_path):
    out = _synthetic_overlay(tmp_path / "es2gears-stage.tar")
    add_es2gears_symlinks(out)
    assert add_es2gears_symlinks(out) == []


# --------------------------------------------------------------------------- #
# The module's own offline selftest (exhaustive shape coverage)
# --------------------------------------------------------------------------- #

def test_module_selftest_passes():
    assert beo._selftest() == 0


# --------------------------------------------------------------------------- #
# Launch wiring — source-invariant guards on MainActivity + runtime_report
# --------------------------------------------------------------------------- #

def test_main_activity_stages_es2gears_under_angle_gate():
    text = MAIN.read_text()
    # the es2gears overlay rides the same .alr-angle opt-in as ANGLE (no-regression)
    assert "es2gears-stage.tar" in text
    assert "/data/local/tmp/.alr-angle" in text
    # launchAngleGlesProbe also stages it (so the marker can name it)
    assert 'stageOverlay("es2gears")' in text


def test_main_activity_runs_arbitrary_app_from_marker():
    text = MAIN.read_text()
    # the generalization: a second marker names an arbitrary in-rootfs app…
    assert "/data/local/tmp/.alr-angle-app" in text
    # …default es2gears_wayland (the on-screen flavour)…
    assert "/usr/bin/es2gears_wayland" in text
    # …run through the SAME runAngle() helper that drives the bespoke clients…
    assert 'runAngle("angle-app:' in text
    # …guarded so a stale/typo'd path is a logged skip, never an unintended exec.
    assert "absent in the rootfs (skipped)" in text


def test_angle_probe_readiness_accepts_es2gears_as_a_run_target():
    text = MAIN.read_text()
    # the readiness gate must proceed when the marker's app exists even if neither
    # bespoke client (alr-angle-vk / alr-gles-cube) shipped.
    assert "haveRunTarget()" in text
    assert "appBin?.exists() == true" in text


def test_runtime_report_angle_env_is_program_agnostic():
    text = RUNTIME.read_text()
    # ALR_ANGLE drives the env purely from the env var (not the program name), so a
    # real app gets the identical ANGLE-on-Wayland config (androlinux-angle first +
    # XDG_SESSION_TYPE=wayland + DISPLAY unset) the bespoke clients use.
    assert "angle_requested" in text
    assert "/usr/lib/androlinux-angle" in text
    assert "XDG_SESSION_TYPE=wayland" in text
