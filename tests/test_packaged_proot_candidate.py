from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "app/build.gradle.kts"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"


def test_gradle_packages_proot_candidate_as_jni_lib_named_so():
    text = BUILD.read_text()
    assert "packageProotCandidate" in text
    assert "libalr_proot.so" in text
    assert "mergeDebugJniLibFolders" in text


def test_cmake_builds_proot_candidate_executable():
    text = CMAKE.read_text()
    assert "proot_candidate.cpp" in text
    assert "alr-proot-candidate" in text
