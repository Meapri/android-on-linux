#include "alr_runtime/alr_trampoline.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace alr::runtime {
namespace {

std::string errno_message(const char* action) {
    std::ostringstream out;
    out << action << " failed errno=" << errno << " message=" << std::strerror(errno);
    return out.str();
}

std::string configured_trampoline_path(const RuntimeConfig& config, const TrampolineAttemptPolicy& policy) {
    if (!policy.packaged_trampoline_path.empty()) {
        return policy.packaged_trampoline_path;
    }
    const auto iter = config.env.find("ALR_TRAMPOLINE_PATH");
    return iter == config.env.end() ? "" : iter->second;
}

bool is_static_hello_candidate(std::string_view requested_program, const ExecutableResolution& resolution, const ElfLoadPlan& elf_plan) {
    const bool is_hello = requested_program == "/bin/hello" || requested_program == "hello";
    return is_hello &&
        resolution.resolved &&
        resolution.kind == ExecutableKind::Elf &&
        elf_plan.valid &&
        elf_plan.status == ElfLoadPlanStatus::StaticExecutable;
}

std::string read_all_from_fd(int fd) {
    std::string out;
    char buffer[512];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            out.append(buffer, static_cast<std::size_t>(count));
        } else if (count == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error(errno_message("read trampoline pipe"));
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> build_trampoline_env(
    const RuntimeConfig& config,
    const SerializedRuntimeConfig& serialized_config,
    const ExecutableResolution& resolution,
    const ElfLoadPlan& elf_plan) {
    std::vector<std::string> env;
    env.reserve(config.env.size() + 8);
    for (const auto& [key, value] : config.env) {
        env.push_back(key + "=" + value);
    }
    env.push_back("ALR_CONFIG_TEXT=" + serialized_config.text);
    env.push_back("ALR_CONFIG_CHECKSUM=" + serialized_config.checksum_hex);
    env.push_back("ALR_TRAMPOLINE_TARGET_GUEST_PATH=" + resolution.translation.guest_path);
    env.push_back("ALR_TRAMPOLINE_TARGET_HOST_PATH=" + resolution.translation.host_path);
    env.push_back(std::string{"ALR_TRAMPOLINE_ELF_STATUS="} + elf_load_plan_status_name(elf_plan.status));
    env.push_back("ALR_TRAMPOLINE_ELF_ENTRY=" + std::to_string(elf_plan.entry));
    env.push_back("ALR_TRAMPOLINE_MODE=preflight");
    return env;
}

void execute_trampoline(
    TrampolineAttemptResult& result,
    const RuntimeConfig& config,
    const SerializedRuntimeConfig& serialized_config,
    const ExecutableResolution& resolution,
    const ElfLoadPlan& elf_plan) {
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (::pipe(stdout_pipe) != 0) {
        result.error = errno_message("pipe trampoline stdout");
        return;
    }
    if (::pipe(stderr_pipe) != 0) {
        const int saved_errno = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        errno = saved_errno;
        result.error = errno_message("pipe trampoline stderr");
        return;
    }

    std::vector<std::string> argv_storage{result.path, "--preflight"};
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& value : argv_storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    auto env_storage = build_trampoline_env(config, serialized_config, resolution, elf_plan);
    std::vector<char*> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto& value : env_storage) {
        envp.push_back(value.data());
    }
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::execve(result.path.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        errno = saved_errno;
        result.error = errno_message("fork trampoline");
        return;
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    result.attempted = true;
    try {
        result.stdout_text = read_all_from_fd(stdout_pipe[0]);
        result.stderr_text = read_all_from_fd(stderr_pipe[0]);
    } catch (const std::exception& exc) {
        result.error = exc.what();
    }
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            result.error = errno_message("waitpid trampoline");
            return;
        }
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
}

std::string build_report(const TrampolineAttemptResult& result) {
    std::ostringstream out;
    out << "ALR TRAMPOLINE AVAILABLE: " << (result.available ? "PASS" : "SKIP");
    out << "\nALR TRAMPOLINE CONFIG HANDOFF: " << (result.config_handoff ? "PASS" : "SKIP");
    out << "\nALR TRAMPOLINE POLICY PREFLIGHT: " << (result.policy_preflight ? "PASS" : "SKIP");
    out << "\nALR STATIC HELLO VIA TRAMPOLINE: " << (result.static_hello_executed ? "PASS" : "SKIP");
    out << "\nalr trampoline path=" << (result.path.empty() ? "none" : result.path);
    out << "\nalr trampoline exit=" << (result.attempted ? std::to_string(result.exit_code) : "not-run");
    out << "\nalr trampoline stdout=" << result.stdout_text;
    out << "\nalr trampoline stderr=" << result.stderr_text;
    if (!result.error.empty()) {
        out << "\nalr trampoline error=" << result.error;
    }
    return out.str();
}

}  // namespace

TrampolineAttemptResult attempt_packaged_trampoline(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const ExecutableResolution& resolution,
    const ElfLoadPlan& elf_plan,
    TrampolineAttemptPolicy policy) {
    TrampolineAttemptResult result;
    result.path = configured_trampoline_path(config, policy);
    result.available = !result.path.empty();
    if (!result.available) {
        result.report = build_report(result);
        return result;
    }

    SerializedRuntimeConfig serialized_config;
    try {
        serialized_config = serialize_runtime_config(config);
        const auto parsed = parse_runtime_config(serialized_config.text);
        result.config_handoff =
            parsed.rootfs_dir == config.rootfs_dir &&
            parsed.program == config.program &&
            runtime_config_checksum_hex(serialized_config.text) == serialized_config.checksum_hex;
    } catch (const std::exception& exc) {
        result.error = exc.what();
        result.report = build_report(result);
        return result;
    }

    const bool static_hello_candidate = is_static_hello_candidate(requested_program, resolution, elf_plan);
    result.policy_preflight = result.config_handoff && static_hello_candidate;
    if (!policy.allow_trampoline_exec) {
        result.report = build_report(result);
        return result;
    }

    execute_trampoline(result, config, serialized_config, resolution, elf_plan);
    result.static_hello_executed =
        static_hello_candidate &&
        result.attempted &&
        result.exit_code == 0 &&
        result.stdout_text.find("ALR TRAMPOLINE STATIC HELLO EXECUTION: PASS") != std::string::npos;
    result.report = build_report(result);
    return result;
}

}  // namespace alr::runtime
