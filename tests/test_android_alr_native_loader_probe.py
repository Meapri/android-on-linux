from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_runtime_report_has_userspace_elf_loader():
    text = CPP.read_text()
    assert "nativeAlrNativeLoaderProbe" in text
    assert "build_native_loader_probe" in text
    assert "ALR NATIVE LOADER GUEST EXEC: " in text
    # A real userspace ELF loader: parse PT_LOAD, map into anon execmem, build a
    # SysV stack with auxv, jump to entry.
    assert "Elf64_Ehdr" in text
    assert "PT_LOAD" in text
    assert "AT_PHDR" in text
    assert "AT_ENTRY" in text
    assert "AT_RANDOM" in text
    assert "MAP_FIXED" in text
    assert "__builtin___clear_cache" in text
    # PASS only when the guest's own output appears (no PRoot, no ptrace).
    assert "hello from static arm64 rootfs" in text


def test_native_runtime_report_has_freestanding_selftest():
    text = CPP.read_text()
    assert "nativeAlrNativeLoaderSelftest" in text
    assert "build_native_loader_selftest" in text
    assert "ALR NATIVE LOADER SELFTEST EXEC: " in text
    # The freestanding stub: mov x0,#42; mov x8,#93(exit); svc #0.
    assert "0xd2800540u" in text
    assert "alr_enter_guest" in text


def test_main_activity_reports_native_loader_gate():
    text = MAIN.read_text()
    assert "nativeAlrNativeLoaderProbe" in text
    assert "alrNativeLoaderProbe" in text
    assert "alrNativeLoaderGuestExecPassed" in text
    assert "ALR NATIVE LOADER MECHANISM (freestanding): " in text
    assert "ALR NATIVE LOADER GUEST EXEC (glibc static): " in text
    assert "nativeAlrNativeLoaderSelftest" in text
    assert "ALR native ELF loader (anon-mmap-loader) probe:" in text
    assert "build: 0.4.127-cp1-gui-baseline-v127" in text
