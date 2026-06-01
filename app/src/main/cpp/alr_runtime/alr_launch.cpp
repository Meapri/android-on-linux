#include "alr_runtime/alr_launch.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "alr_runtime/alr_elf.hpp"
#include "alr_runtime/alr_trampoline.hpp"

namespace alr::runtime {
namespace {

std::string errno_message(const char* action) {
    std::ostringstream out;
    out << action << " failed errno=" << errno << " message=" << std::strerror(errno);
    return out.str();
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
            throw std::runtime_error(errno_message("read pipe"));
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> build_env_strings(const RuntimeConfig& config) {
    std::vector<std::string> env;
    env.reserve(config.env.size());
    for (const auto& [key, value] : config.env) {
        env.push_back(key + "=" + value);
    }
    return env;
}

std::string status_for_attempt(const LaunchAttemptResult& result) {
    if (result.policy_blocked) return "SKIP";
    if (!result.attempted) return "FAIL";
    return result.exit_code == 0 ? "PASS" : "FAIL";
}

std::string hello_status_for_attempt(std::string_view requested_program, const LaunchAttemptResult& result) {
    const bool is_hello = requested_program == "/bin/hello" || requested_program == "hello";
    if (!is_hello) return "SKIP";
    return status_for_attempt(result);
}

std::string build_report(std::string_view requested_program, const LaunchAttemptResult& result) {
    std::ostringstream out;
    const auto elf_plan = result.resolution.resolved && result.resolution.kind == ExecutableKind::Elf
        ? build_elf_load_plan(result.resolution.translation.host_path)
        : ElfLoadPlan{};
    const std::string elf_report = elf_plan.report.empty()
        ? std::string{
            "ALR ELF LOAD PLAN: SKIP\n"
            "ALR ELF STATIC HELLO CANDIDATE: SKIP\n"
            "alr elf class=none\n"
            "alr elf machine=none\n"
            "alr elf type=none\n"
            "alr elf status=unsupported\n"
            "alr elf entry=0x0\n"
            "alr elf min vaddr=0x0\n"
            "alr elf max vaddr=0x0\n"
            "alr elf interp=none\n"
            "alr elf load segments=0"}
        : elf_plan.report;
    const std::string trampoline_report = result.trampoline.report.empty()
        ? std::string{
            "ALR TRAMPOLINE AVAILABLE: SKIP\n"
            "ALR TRAMPOLINE CONFIG HANDOFF: SKIP\n"
            "ALR TRAMPOLINE POLICY PREFLIGHT: SKIP\n"
            "ALR STATIC HELLO VIA TRAMPOLINE: SKIP\n"
            "alr trampoline path=none\n"
            "alr trampoline exit=not-run\n"
            "alr trampoline stdout=\n"
            "alr trampoline stderr="}
        : std::string{
            result.trampoline.report};
    out << "ALR LAUNCH ATTEMPT: " << status_for_attempt(result);
    out << "\nALR LAUNCH MODE: " << (result.policy_blocked ? "policy-preflight" : "direct-host-exec");
    out << "\nALR LOW-OVERHEAD RUNTIME HELLO EXECUTION: " << hello_status_for_attempt(requested_program, result);
    out << "\n" << elf_report;
    out << "\n" << trampoline_report;
    if (!result.continuation.report.empty()) {
        out << "\n" << result.continuation.report;
    }
    out << "\nalr launch exit=" << result.exit_code;
    out << "\nalr launch stdout=" << result.stdout_text;
    out << "\nalr launch stderr=" << result.stderr_text;
    if (!result.error.empty()) {
        out << "\nalr launch error=" << result.error;
    }
    return out.str();
}

}  // namespace

LaunchAttemptResult attempt_guest_launch(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const std::vector<std::string>& arguments,
    LaunchAttemptPolicy policy) {
    LaunchAttemptResult result;
    result.resolution = resolve_guest_executable(config, requested_program);
    result.continuation = build_exec_continuation_plan(
        config,
        requested_program,
        arguments,
        ExecContinuationKind::Execve);
    if (!result.resolution.resolved || result.resolution.kind == ExecutableKind::Missing) {
        result.error = result.resolution.error.empty() ? "executable was not resolved" : result.resolution.error;
        result.report = build_report(requested_program, result);
        return result;
    }
    if (result.resolution.kind == ExecutableKind::Unsupported) {
        result.error = "resolved executable kind is unsupported";
        result.report = build_report(requested_program, result);
        return result;
    }
    if (!policy.allow_direct_host_exec) {
        result.policy_blocked = true;
        result.error = "direct rootfs host exec disabled by ALR policy";
        const auto elf_plan = result.resolution.kind == ExecutableKind::Elf
            ? build_elf_load_plan(result.resolution.translation.host_path)
            : ElfLoadPlan{};
        result.trampoline = attempt_packaged_trampoline(
            config,
            requested_program,
            result.resolution,
            elf_plan,
            policy.trampoline);
        result.report = build_report(requested_program, result);
        return result;
    }

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (::pipe(stdout_pipe) != 0) {
        result.error = errno_message("pipe stdout");
        result.report = build_report(requested_program, result);
        return result;
    }
    if (::pipe(stderr_pipe) != 0) {
        const int saved_errno = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        errno = saved_errno;
        result.error = errno_message("pipe stderr");
        result.report = build_report(requested_program, result);
        return result;
    }

    std::vector<std::string> argv_storage;
    argv_storage.push_back(result.resolution.translation.host_path);
    argv_storage.insert(argv_storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& value : argv_storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage = build_env_strings(config);
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
        ::execve(result.resolution.translation.host_path.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        errno = saved_errno;
        result.error = errno_message("fork");
        result.report = build_report(requested_program, result);
        return result;
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
            result.error = errno_message("waitpid");
            result.report = build_report(requested_program, result);
            return result;
        }
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = -1;
    }
    result.report = build_report(requested_program, result);
    return result;
}

}  // namespace alr::runtime
