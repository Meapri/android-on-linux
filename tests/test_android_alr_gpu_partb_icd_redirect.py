"""Source-invariant guards for GPU Part B — the ICD discovery redirect.

Design (memory alr-gpu-native-track; the headline milestone): ANGLE/volk
dlopen("libvulkan.so.1") and on Android the host /system/lib64/libvulkan.so wins,
so our Mali ICD is never loaded and ANGLE dies at "Internal Vulkan error -3". The
fix ships the REAL Khronos Vulkan-Loader as /usr/lib/androlinux/libvulkan.so.1 +
our (loader-conformant) ICD RENAMED to libalr_mali_icd.so + alr_icd.json, and makes
ANGLE's dlopen reach the staged loader instead of /system.

This file asserts the WIRING is present and correctly shaped (the on-device
ANGLE→our-ICD→Mali run is the integration gate). The four wiring pieces:

  (i)   the guest ICD is RENAMED libvulkan.so.1 → libalr_mali_icd.so
        (build-icd.sh DT_SONAME + the overlay builder + the manifest target);
  (ii)  runtime_report.cpp exports VK_DRIVER_FILES (+ keeps VK_ICD_FILENAMES) →
        the rootfs alr_icd.json so the Khronos loader discovers our ICD;
  (iii) the interposer wraps dlopen and redirects a bare "libvulkan.so.1" /
        "libvulkan.so" to the ABSOLUTE rootfs Khronos loader (beats /system);
  (iv)  the app stages the vk-loader overlay (Khronos loader) alongside vk-icd
        under the .alr-angle gate, and waits for BOTH files before the proof.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
SESSION = ROOT / "app/src/main/java/dev/chanwoo/androlinux/runtime/NativeAppSession.kt"
INTERPOSE = ROOT / "app/src/main/cpp/alr_interpose/libalr_interpose.c"
RUNTIME = ROOT / "app/src/main/cpp/runtime_report.cpp"
BUILD_ICD = ROOT / "app/src/main/cpp/alr_gpu/guest_icd/build-icd.sh"

ICD_SONAME = "libalr_mali_icd.so"
LOADER_SONAME = "libvulkan.so.1"


# --------------------------------------------------------------------------- #
# (i) the guest ICD is renamed libvulkan.so.1 → libalr_mali_icd.so
# --------------------------------------------------------------------------- #

def test_build_icd_soname_is_renamed_to_mali_icd():
    text = BUILD_ICD.read_text()
    # the link step sets the new SONAME and emits the new file name
    assert f"-Wl,-soname,{ICD_SONAME}" in text
    assert f"-o \"$OUT/{ICD_SONAME}\"" in text
    # it must NOT ship its own libvulkan.so.1 (that's the loader's name now)
    assert '-Wl,-soname,libvulkan.so.1' not in text
    assert '-o "$OUT/libvulkan.so.1"' not in text


def test_build_icd_overlay_ships_only_renamed_icd_no_loader_name_no_manifest():
    from tools import build_vk_icd_overlay as bvi
    assert bvi.SONAME == ICD_SONAME
    # the overlay module must no longer define a libvulkan.so symlink or a manifest
    assert not hasattr(bvi, "UNVERSIONED") or bvi.UNVERSIONED != "libvulkan.so"
    # its selftest enforces the no-symlink / no-manifest contract
    assert bvi._selftest() == 0


def test_loader_overlay_manifest_points_at_the_renamed_icd():
    from tools import build_vk_loader_overlay as bvl
    # the Khronos loader owns libvulkan.so.1; its manifest names OUR renamed ICD
    assert bvl.SONAME == LOADER_SONAME
    assert bvl.ICD_BASENAME == ICD_SONAME
    assert bvl.ICD_LIBRARY_PATH == f"/usr/lib/androlinux/{ICD_SONAME}"
    # the manifest must NOT point at the loader's own libvulkan.so.1 (would self-load)
    assert LOADER_SONAME not in bvl.ICD_LIBRARY_PATH


# --------------------------------------------------------------------------- #
# (ii) runtime_report exports VK_DRIVER_FILES → the rootfs alr_icd.json
# --------------------------------------------------------------------------- #

def test_runtime_report_exports_vk_driver_files_and_keeps_icd_filenames():
    text = RUNTIME.read_text()
    # the modern loader selector (overrides the system ICD search)…
    assert 'VK_DRIVER_FILES=' in text
    # …and the legacy alias, both pointing at the rootfs manifest
    assert 'VK_ICD_FILENAMES=' in text
    assert '/usr/lib/androlinux/alr_icd.json' in text
    # both are pushed inside the ALR_VK_ICD guard (so a non-VK guest is unaffected)
    assert 'vk_icd_requested' in text


# --------------------------------------------------------------------------- #
# (iii) the interposer dlopen redirect (libvulkan.so.1 → rootfs Khronos loader)
# --------------------------------------------------------------------------- #

def test_interpose_wraps_dlopen():
    text = INTERPOSE.read_text()
    # a real dlopen wrapper is defined (exported symbol the dynamic linker overrides)
    assert "void *dlopen(const char *filename, int flag)" in text
    # it resolves the real dlopen via the cached RTLD_NEXT slot
    assert 'ALR_REAL(real, void *(*)(const char *, int), "dlopen")' in text


def test_interpose_dlopen_redirects_vulkan_to_absolute_rootfs_loader():
    text = INTERPOSE.read_text()
    # the redirect targets the rootfs Khronos loader by ABSOLUTE path
    assert "/usr/lib/androlinux/libvulkan.so.1" in text
    # it matches BOTH the versioned soname and the unversioned one
    assert 'alr_basename_is(filename, "libvulkan.so.1")' in text
    assert 'alr_basename_is(filename, "libvulkan.so")' in text
    # it only fires for a BARE soname (no slash) — an explicit path is honored verbatim
    assert "has_slash" in text
    # fail-safe: take the redirect only if the staged loader file actually EXISTS
    assert "real_faccessat" in text


def test_interpose_dlopen_redirect_is_rootfs_gated():
    """The redirect must no-op when the interposer is disabled (no ALR_ROOTFS)."""
    text = INTERPOSE.read_text()
    # the dlopen body is guarded on g_rootfs_len != 0 (interposer enabled)
    dl = text[text.index("void *dlopen("):]
    dl = dl[: dl.index("\n}\n") + 3]
    assert "g_rootfs_len != 0" in dl


# --------------------------------------------------------------------------- #
# (iv) the app stages vk-loader alongside vk-icd and waits for BOTH
# --------------------------------------------------------------------------- #

def test_main_activity_stages_vk_loader_under_angle_gate():
    text = MAIN.read_text()
    assert 'vk-loader-stage.tar' in text
    # rides the same opt-in marker as ANGLE (no-regression on a normal cold start)
    assert '/data/local/tmp/.alr-angle' in text


def test_angle_probe_waits_for_both_loader_and_renamed_icd():
    text = MAIN.read_text()
    # the readiness check must require BOTH the loader and our renamed ICD on disk
    assert 'usr/lib/androlinux/libvulkan.so.1' in text          # Khronos loader
    assert f'usr/lib/androlinux/{ICD_SONAME}' in text           # our renamed ICD
    assert 'stageOverlay("vk-loader")' in text
    assert 'stageOverlay("vk-icd")' in text


def test_session_overlay_set_includes_vk_loader():
    text = SESSION.read_text()
    # the always-on best-effort overlay set carries both vk-icd and vk-loader
    assert '"vk-icd"' in text
    assert '"vk-loader"' in text
