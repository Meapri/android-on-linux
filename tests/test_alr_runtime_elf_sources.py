from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ELF_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_elf.hpp"
ELF_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_elf.cpp"
LAUNCH_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_launch.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_elf_test.cpp"


def test_elf_load_plan_api_is_declared_and_implemented():
    header = ELF_HPP.read_text()
    source = ELF_CPP.read_text()
    assert "enum class ElfLoadPlanStatus" in header
    assert "struct ElfLoadPlan" in header
    assert "build_elf_load_plan" in header
    assert "Elf64_Ehdr" in source
    assert "Elf64_Phdr" in source
    assert "PT_LOAD" in source
    assert "PT_INTERP" in source
    assert "EM_AARCH64" in source
    assert "ALR ELF LOAD PLAN: " in source
    assert "ALR ELF STATIC HELLO CANDIDATE: " in source


def test_elf_load_plan_is_built_host_tested_and_reported():
    cmake = CMAKE.read_text()
    script = NATIVE_SCRIPT.read_text()
    launch = LAUNCH_CPP.read_text()
    assert "alr_runtime/alr_elf.cpp" in cmake
    assert "native_alr_runtime_elf_test.cpp" in script
    assert "alr_runtime/alr_elf.cpp" in script
    assert "alr-native-runtime-elf-test" in script
    assert "alr runtime elf native test ok" in NATIVE_TEST.read_text()
    assert "build_elf_load_plan" in launch
    assert "elf_report" in launch
