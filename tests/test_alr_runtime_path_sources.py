from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "docs/alr-runtime-v1-clean-room-spec.md"
PATH_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_path.hpp"
PATH_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_path.cpp"
ENV_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_env.hpp"
ENV_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_env.cpp"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_path_test.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"


def test_clean_room_spec_defines_alr_runtime_scope_and_non_goals():
    text = SPEC.read_text()
    assert "Clean-room" in text
    assert "No proprietary PRoot/proot reverse engineering" in text
    assert "Rootfs path translation" in text
    assert "clamp at guest root" in text


def test_path_translation_api_is_declared_and_implemented():
    header = PATH_HPP.read_text()
    source = PATH_CPP.read_text()
    assert "struct PathTranslation" in header
    assert "normalize_guest_path" in header
    assert "translate_rootfs_path" in header
    assert "is_guest_absolute_path" in header
    assert "escaped_rootfs" in header
    assert "append_components" in source
    assert "component == \"..\"" in source
    assert "components.pop_back()" in source
    assert "rootfs + guest" in source


def test_guest_environment_skeleton_is_declared_and_implemented():
    header = ENV_HPP.read_text()
    source = ENV_CPP.read_text()
    assert "struct GuestEnvironmentInput" in header
    assert "build_guest_environment" in header
    assert "ALR_PACKAGE" in source
    assert "ALR_ROOTFS" in source
    assert "HOME" in source
    assert "TMPDIR" in source
    assert "PATH" in source


def test_native_core_script_builds_alr_runtime_path_test():
    text = NATIVE_SCRIPT.read_text()
    assert "native_alr_runtime_path_test.cpp" in text
    assert "alr_runtime/alr_path.cpp" in text
    assert "alr_runtime/alr_env.cpp" in text


def test_cmake_compiles_alr_runtime_sources_for_android_builds():
    text = CMAKE.read_text()
    assert "add_library(alr_runtime STATIC" in text
    assert "alr_runtime/alr_path.cpp" in text
    assert "alr_runtime/alr_env.cpp" in text
    assert "add_library(alr_runtime_launcher SHARED" in text
    assert "alr_runtime_launcher.cpp" in text
    assert "target_compile_features(alr_runtime PRIVATE cxx_std_20)" in text
    assert "target_compile_features(alr_runtime_launcher PRIVATE cxx_std_20)" in text


def test_native_path_test_covers_normalization_and_translation_cases():
    text = NATIVE_TEST.read_text()
    assert "normalizes absolute paths" in text
    assert "resolves relative paths against cwd" in text
    assert "clamps parent traversal at guest root" in text
    assert "relative rootfs paths are rejected" in text
