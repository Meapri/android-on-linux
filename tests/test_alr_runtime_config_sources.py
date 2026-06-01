from pathlib import Path

from tools.runtime_launch_plan import build_launch_plan, build_runtime_config, serialize_runtime_config


ROOT = Path(__file__).resolve().parents[1]
CONFIG_HPP = ROOT / "app/src/main/cpp/alr_runtime/alr_config.hpp"
CONFIG_CPP = ROOT / "app/src/main/cpp/alr_runtime/alr_config.cpp"
CMAKE = ROOT / "app/src/main/cpp/CMakeLists.txt"
NATIVE_SCRIPT = ROOT / "scripts/test-native-core.sh"
NATIVE_TEST = ROOT / "tests/native_alr_runtime_config_test.cpp"
PLAN_CPP = ROOT / "app/src/main/cpp/runtime_plan.cpp"
REPORT_CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"


def test_config_api_is_declared_and_implemented():
    header = CONFIG_HPP.read_text()
    source = CONFIG_CPP.read_text()
    assert "struct RuntimeConfig" in header
    assert "struct BindMount" in header
    assert "serialize_runtime_config" in header
    assert "parse_runtime_config" in header
    assert "runtime_config_checksum_hex" in header
    assert "alr-config-v1" in source
    assert "translate" not in source.lower()


def test_config_is_built_into_native_runtime_library_and_host_tests():
    cmake = CMAKE.read_text()
    script = NATIVE_SCRIPT.read_text()
    assert "alr_runtime/alr_config.cpp" in cmake
    assert "native_alr_runtime_config_test.cpp" in script
    assert "alr_runtime/alr_config.cpp" in script
    assert "alr-native-runtime-config-test" in script
    assert "alr runtime config native test ok" in NATIVE_TEST.read_text()


def test_runtime_report_includes_config_status_and_interposer_env():
    plan = PLAN_CPP.read_text()
    report = REPORT_CPP.read_text()
    assert "ALR CONFIG SERIALIZE: PASS" in plan
    assert "ALR CONFIG PARSE: " in plan
    assert "alr runtime config bytes=" in plan
    assert "alr runtime config checksum=" in plan
    assert "ALR_CONFIG_FORMAT" in plan
    assert "ALR_TRACE_PATH" in plan
    assert "ALR_TRACE_EXEC" in plan
    assert "ALR_INTERPOSER_PATH" in report


def test_python_launch_plan_serializes_matching_runtime_config():
    plan = build_launch_plan(
        package_name="dev.chanwoo.androlinux",
        native_library_dir="/data/app/~~token/dev.chanwoo.androlinux-abc/lib/arm64",
        app_files_dir="/data/user/0/dev.chanwoo.androlinux/files",
        rootfs_name="debian-arm64",
        program="/bin/hello",
        backend="alr-runtime",
    )
    config = build_runtime_config(plan)
    serialized = serialize_runtime_config(config)
    assert serialized.text.startswith("alr-config-v1\n")
    assert "field\tprogram\t/bin/hello\n" in serialized.text
    assert "field\tinterposer_path\t/data/app/" in serialized.text
    assert "env\tALR_INTERPOSER_PATH\t/data/app/" in serialized.text
    assert len(serialized.checksum_hex) == 16
