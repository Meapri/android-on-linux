from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WX_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_wx.hpp"
WX_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_wx.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_wx_test.cpp"


def test_wx_strategy_api_is_declared_and_implemented():
    header = WX_HPP.read_text()
    source = WX_CPP.read_text()
    assert "enum class GuestLoadMethod" in header
    assert "enum class RejectedExecMethod" in header
    assert "struct WxSafeExecStrategy" in header
    assert "build_wx_safe_exec_strategy" in header
    # Plan-only honesty: the strategy is not device-proven.
    assert "clean-room strategy plan" in header
    assert "ALR WX-SAFE EXEC STRATEGY: " in source
    assert "ALR WX-SAFE ENTRYPOINT IS PACKAGED: " in source
    assert "ALR WX-SAFE DIRECT ROOTFS EXEC REJECTED: " in source
    assert "ALR WX-SAFE FILE-BACKED MMAP EXEC REJECTED: " in source
    assert "ALR WX-SAFE PROOT BASELINE AVAILABLE: " in source


def test_wx_strategy_rejects_wx_violations_and_keeps_packaged_entrypoint():
    source = WX_CPP.read_text()
    # The packaged trampoline entrypoint must not be inside the writable rootfs.
    assert "ALR_TRAMPOLINE_PATH" in source
    assert "path_is_inside" in source
    # Both W^X-violating shortcuts must be explicitly rejected with reasons.
    assert "direct-rootfs-execve" in source
    assert "file-backed-mmap-exec" in source
    assert "app_data_file" in source
    assert "W^X" in source


def test_wx_strategy_ranks_native_methods_with_proot_fallback():
    source = WX_CPP.read_text()
    assert "memfd-execveat" in source
    assert "anon-mmap-loader" in source
    assert "proot-baseline" in source
    # PRoot is only a fallback: a viable plan needs a native-exec primary.
    assert "primary != GuestLoadMethod::ProotBaseline" in source
    # The domain decides, and it is READ rather than inferred from targetSdk.
    assert "domain_allows_app_data_exec" in source
    assert "/proc/self/attr/current" in source
    # The native mechanisms still require on-device SELinux verification.
    assert "execmem" in source
    assert "requires_device_selinux_check" in WX_HPP.read_text()


def test_wx_strategy_is_built_and_host_tested():
    cmake = CMAKE.read_text()
    script = NATIVE_SCRIPT.read_text()
    native = NATIVE_TEST.read_text()
    assert "alr_runtime/alr_wx.cpp" in cmake
    assert "native_alr_runtime_wx_test.cpp" in script
    assert "app/src/main/cpp/alr_runtime/alr_wx.cpp" in script
    assert "alr-native-runtime-wx-test" in script
    assert "alr runtime wx native test ok" in native


def test_wx_native_test_covers_packaged_and_rejected_entrypoints():
    native = NATIVE_TEST.read_text()
    # Not a single pinned method any more: the primary depends on the SELinux
    # domain the process is in, and pinning one answer is what let the model
    # contradict the device for a whole release.
    assert "primary is direct-rootfs-execve or anon-mmap-loader" in native
    assert "rootfs-internal entrypoint refused" in native
    assert "prefix-sibling entrypoint is packaged" in native
