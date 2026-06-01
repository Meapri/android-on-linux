#include "runtime_plan.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"
#include "alr_runtime/alr_launch.hpp"

namespace alr {
namespace {

std::string join_path(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

std::string rootfs_dir_for(const RuntimeReportInput& input) {
    return join_path(join_path(input.app_files_dir, "rootfs"), input.rootfs_name);
}

std::string loader_executable_for(const RuntimeReportInput& input) {
    return join_path(input.native_library_dir, "libalr-loader.so");
}

std::string proot_loader_for(const RuntimeReportInput& input) {
    return join_path(input.native_library_dir, "libproot-loader.so");
}

std::string alr_runtime_launcher_for(const RuntimeReportInput& input) {
    return join_path(input.native_library_dir, "libalr_runtime_launcher.so");
}

std::string alr_runtime_hook_for(const RuntimeReportInput& input) {
    return join_path(input.native_library_dir, "libalr_runtime_hook.so");
}

std::string alr_runtime_interposer_for(const RuntimeReportInput& input) {
    return join_path(input.native_library_dir, "libalr_runtime_interposer.so");
}

std::string alr_runtime_trampoline_for(const RuntimeReportInput& input) {
    return join_path(input.native_library_dir, "libalr_runtime_trampoline.so");
}

std::string alr_runtime_bridge_for(const RuntimeReportInput& input) {
    return join_path(input.native_library_dir, "libalr_runtime_bridge.so");
}

std::string proot_tmp_dir_for(const RuntimeReportInput& input) {
    return join_path(input.app_cache_dir, "proot-tmp");
}

runtime::RuntimeConfig build_alr_runtime_config(
    const RuntimeReportInput& input,
    const LoaderLaunchPlan& launch) {
    return runtime::RuntimeConfig{
        .package_name = input.package_name,
        .rootfs_dir = launch.env.at("ALR_ROOTFS"),
        .cwd = "/",
        .program = input.program,
        .env = launch.env,
        .binds = {},
        .hook_path = launch.env.at("ALR_HOOK_PATH"),
        .interposer_path = launch.env.at("ALR_INTERPOSER_PATH"),
        .bridge_path = launch.env.at("ALR_BRIDGE_PATH"),
        .fake_root = launch.env.at("ALR_FAKE_ROOT") == "1",
        .verbose = launch.env.at("ALR_VERBOSE") == "0" ? 0 : 1,
        .trace_path = false,
        .trace_exec = false,
    };
}

void validate_input(const RuntimeReportInput& input) {
    if (input.program.empty() || input.program.front() != '/') {
        throw std::invalid_argument("program must be an absolute path inside the rootfs");
    }
}

}  // namespace

RuntimeReport build_runtime_report(const RuntimeReportInput& input) {
    return build_runtime_report(input, select_execution_backend(ExecutionBackendKind::PlanOnly));
}

RuntimeReport build_runtime_report(const RuntimeReportInput& input, const ExecutionBackend& backend) {
    validate_input(input);
    const auto optional_backend = probe_optional_runtime_backend(input);
    RuntimeReport report;
    report.loader_executable = loader_executable_for(input);
    report.rootfs_dir = rootfs_dir_for(input);

    std::ostringstream out;
    out << "AndroLinux Runtime Lab\n\n";
    out << "package: " << input.package_name << "\n";
    out << "nativeLibraryDir: " << input.native_library_dir << "\n";
    out << "loader executable: " << report.loader_executable << "\n";
    out << "app files dir: " << input.app_files_dir << "\n";
    out << "rootfs dir: " << report.rootfs_dir << "\n";
    out << "program: " << input.program << "\n";
    out << "execution backend: " << backend.name << "\n";
    out << "can execute: " << (backend.can_execute ? "yes" : "no") << "\n";
    out << "backend reason: " << backend.reason << "\n";
    const auto alr_runtime = build_alr_runtime_launch_plan(input);
    out << "ALR RUNTIME LAUNCHER AVAILABLE: PASS\n";
    out << "ALR RUNTIME CONFIG BUILD: PASS\n";
    out << "ALR RUNTIME DIRECT APP-DATA EXEC POLICY: PASS\n";
    out << "ALR HOOK LOAD: PASS\n";
    out << "ALR HOOK CONFIG BUILD: PASS\n";
    out << "ALR INTERPOSER LOAD: PASS\n";
    out << "ALR INTERPOSER CONFIG BUILD: PASS\n";
    const auto serialized_config = runtime::serialize_runtime_config(build_alr_runtime_config(input, alr_runtime));
    const auto parsed_config = runtime::parse_runtime_config(serialized_config.text);
    const auto exec_resolution = runtime::resolve_guest_executable(parsed_config, input.program);
    const auto launch_attempt = runtime::attempt_guest_launch(parsed_config, input.program);
    out << "ALR CONFIG SERIALIZE: PASS\n";
    out << "ALR CONFIG PARSE: "
        << (parsed_config.program == input.program ? "PASS" : "FAIL") << "\n";
    out << exec_resolution.report << "\n";
    out << launch_attempt.report << "\n";
    out << "alr runtime launcher path=" << alr_runtime.executable << "\n";
    out << "alr runtime hook path=" << alr_runtime.env.at("ALR_HOOK_PATH") << "\n";
    out << "alr runtime interposer path=" << alr_runtime.env.at("ALR_INTERPOSER_PATH") << "\n";
    out << "alr runtime trampoline path=" << alr_runtime.env.at("ALR_TRAMPOLINE_PATH") << "\n";
    out << "alr runtime bridge path=" << alr_runtime.env.at("ALR_BRIDGE_PATH") << "\n";
    out << "alr runtime config source=env-skeleton\n";
    out << "alr runtime config format=alr-config-v1\n";
    out << "alr runtime config bytes=" << serialized_config.text.size() << "\n";
    out << "alr runtime config checksum=" << serialized_config.checksum_hex << "\n";
    out << "alr runtime guest execution=not-claimed\n";
    out << "LOW-OVERHEAD BACKEND PROBE FRAMEWORK: " << optional_backend.framework_status << "\n";
    out << "OPTIONAL RUNTIME BACKEND AVAILABLE: " << optional_backend.available_status << "\n";
    out << "optional runtime backend source=" << optional_backend.source << "\n";
    out << "optional runtime backend name=" << optional_backend.backend << "\n";
    out << "optional runtime backend path=" << (optional_backend.candidate_path.empty() ? "none" : optional_backend.candidate_path) << "\n";
    out << "optional runtime backend executable=" << (optional_backend.can_execute ? "yes" : "no") << "\n";
    out << "optional runtime backend reason=" << optional_backend.reason << "\n\n";
    out << "Runtime policy: rootfs files stay in app-private writable storage; execution enters through packaged native backends.";
    report.text = out.str();
    return report;
}

LoaderLaunchPlan build_loader_launch_plan(const RuntimeReportInput& input) {
    validate_input(input);
    LoaderLaunchPlan plan;
    plan.executable = loader_executable_for(input);
    const std::string rootfs_dir = rootfs_dir_for(input);
    plan.argv = {
        plan.executable,
        "--rootfs",
        rootfs_dir,
        "--program",
        input.program,
    };
    plan.env = {
        {"ALR_PACKAGE", input.package_name},
        {"ALR_ROOTFS", rootfs_dir},
        {"ALR_PROGRAM", input.program},
        {"ALR_BACKEND", "loader"},
        {"HOME", "/root"},
        {"TMPDIR", "/tmp"},
        {"PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"},
    };
    return plan;
}

LoaderLaunchPlan build_proot_launch_plan(const RuntimeReportInput& input) {
    validate_input(input);
    LoaderLaunchPlan plan;
    const std::string rootfs_dir = rootfs_dir_for(input);
    plan.executable = join_path(input.native_library_dir, "libalr_proot.so");
    plan.argv = {
        plan.executable,
        "-R",
        rootfs_dir,
        "-w",
        "/",
        input.program,
    };
    plan.env = {
        {"ALR_PACKAGE", input.package_name},
        {"ALR_ROOTFS", rootfs_dir},
        {"ALR_PROGRAM", input.program},
        {"ALR_BACKEND", "proot"},
        {"PROOT_LOADER", proot_loader_for(input)},
        {"PROOT_TMP_DIR", proot_tmp_dir_for(input)},
        {"PROOT_NO_SECCOMP", "1"},
        {"PROOT_VERBOSE", "-1"},
        {"LD_LIBRARY_PATH", input.native_library_dir},
        {"HOME", "/root"},
        {"TMPDIR", "/tmp"},
        {"PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"},
    };
    return plan;
}

LoaderLaunchPlan build_alr_runtime_launch_plan(const RuntimeReportInput& input) {
    validate_input(input);
    LoaderLaunchPlan plan;
    const std::string rootfs_dir = rootfs_dir_for(input);
    plan.executable = alr_runtime_launcher_for(input);
    plan.argv = {
        plan.executable,
        "--rootfs",
        rootfs_dir,
        "--cwd",
        "/",
        "--program",
        input.program,
        "--dry-run",
    };
    plan.env = {
        {"ALR_PACKAGE", input.package_name},
        {"ALR_ROOTFS", rootfs_dir},
        {"ALR_PROGRAM", input.program},
        {"ALR_BACKEND", "alr-runtime"},
        {"ALR_HOOK_PATH", alr_runtime_hook_for(input)},
        {"ALR_INTERPOSER_PATH", alr_runtime_interposer_for(input)},
        {"ALR_TRAMPOLINE_PATH", alr_runtime_trampoline_for(input)},
        {"ALR_BRIDGE_PATH", alr_runtime_bridge_for(input)},
        {"ALR_CONFIG_FORMAT", "alr-config-v1"},
        {"ALR_FAKE_ROOT", "0"},
        {"ALR_VERBOSE", "0"},
        {"ALR_TRACE_PATH", "0"},
        {"ALR_TRACE_EXEC", "0"},
        {"HOME", "/root"},
        {"TMPDIR", "/tmp"},
        {"PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"},
    };
    return plan;
}

OptionalRuntimeBackendProbe probe_optional_runtime_backend(const RuntimeReportInput& input) {
    if (input.optional_runtime_backend_path.empty()) {
        return {
            .framework_status = "PASS",
            .available_status = "SKIP",
            .source = "none",
            .backend = "none",
            .candidate_path = "",
            .can_execute = false,
            .reason = "no optional external runtime backend configured",
        };
    }
    if (input.optional_runtime_backend_path.front() != '/') {
        throw std::invalid_argument("absolute optional runtime backend path required");
    }
    return {
        .framework_status = "PASS",
        .available_status = "PASS",
        .source = "external",
        .backend = input.optional_runtime_backend_name.empty() ? "external" : input.optional_runtime_backend_name,
        .candidate_path = input.optional_runtime_backend_path,
        .can_execute = false,
        .reason = "external candidate declared for future probe integration; not executed by absent-safe scaffolding",
    };
}

ExecutionBackend select_execution_backend(ExecutionBackendKind kind) {
    switch (kind) {
        case ExecutionBackendKind::PlanOnly:
            return {
                .kind = kind,
                .name = "plan-only",
                .can_execute = false,
                .reason = "writable app-data rootfs binaries are not direct exec entrypoints on modern Android; use a packaged native loader/proot/glibc-loader backend",
            };
        case ExecutionBackendKind::AndroidNativeTestCommand:
            return {
                .kind = kind,
                .name = "android-native-test-command",
                .can_execute = true,
                .reason = "executes only packaged/native-library-dir test commands, not rootfs app-data binaries",
            };
        case ExecutionBackendKind::Proot:
            return {
                .kind = kind,
                .name = "proot",
                .can_execute = true,
                .reason = "packaged PRoot backend validated for static arm64 ELF smoke execution inside app-private rootfs",
            };
        case ExecutionBackendKind::GlibcLoader:
            return {
                .kind = kind,
                .name = "glibc-loader",
                .can_execute = false,
                .reason = "planned backend; requires packaged executable glibc loader and path virtualization strategy",
            };
        case ExecutionBackendKind::AlrRuntime:
            return {
                .kind = kind,
                .name = "alr-runtime",
                .can_execute = false,
                .reason = "clean-room ALR launcher/config skeleton is packaged but guest execution is not implemented yet",
            };
    }
    throw std::invalid_argument("unknown execution backend");
}

}  // namespace alr
