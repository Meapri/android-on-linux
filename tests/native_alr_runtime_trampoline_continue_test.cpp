#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../app/src/main/cpp/alr_runtime/alr_config.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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
        } else {
            throw std::runtime_error("read pipe failed");
        }
    }
    return out;
}

std::vector<char*> mutable_vector(std::vector<std::string>& values) {
    std::vector<char*> out;
    out.reserve(values.size() + 1);
    for (auto& value : values) {
        out.push_back(value.data());
    }
    out.push_back(nullptr);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected trampoline executable path");
    const std::string trampoline_path = argv[1];
    require(std::filesystem::is_regular_file(trampoline_path), "trampoline executable exists");

    const auto root = std::filesystem::temp_directory_path() /
        ("alr-trampoline-continue-rootfs-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "bin");

    const auto config = alr::runtime::RuntimeConfig{
        .package_name = "dev.chanwoo.androlinux",
        .rootfs_dir = root.string(),
        .cwd = "/",
        .program = "/bin/hello",
        .env = {
            {"ALR_BACKEND", "alr-runtime"},
            {"ALR_PROGRAM", "/bin/hello"},
            {"ALR_ROOTFS", root.string()},
            {"PATH", "/bin"},
        },
    };
    const auto serialized = alr::runtime::serialize_runtime_config(config);

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    require(::pipe(stdout_pipe) == 0, "stdout pipe");
    require(::pipe(stderr_pipe) == 0, "stderr pipe");

    std::vector<std::string> argv_storage{
        trampoline_path,
        "--continue-exec",
        "--kind",
        "execve",
        "--",
        "/bin/hello",
        "--smoke",
    };
    auto child_argv = mutable_vector(argv_storage);
    std::vector<std::string> env_storage{
        "ALR_CONFIG_TEXT=" + serialized.text,
        "ALR_CONFIG_CHECKSUM=" + serialized.checksum_hex,
        "ALR_CONTINUATION_KIND=execve",
        "ALR_CONTINUATION_TARGET_GUEST_PATH=/bin/hello",
        "ALR_CONTINUATION_ARGC=1",
        "ALR_CONTINUATION_PATH_LOOKUP=0",
    };
    auto child_env = mutable_vector(env_storage);

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::execve(trampoline_path.c_str(), child_argv.data(), child_env.data());
        _exit(127);
    }
    require(pid > 0, "fork");
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    const auto stdout_text = read_all_from_fd(stdout_pipe[0]);
    const auto stderr_text = read_all_from_fd(stderr_pipe[0]);
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    require(::waitpid(pid, &status, 0) == pid, "waitpid");
    require(WIFEXITED(status), "trampoline exited");
    require(WEXITSTATUS(status) == 0, "trampoline continue exit");
    require(stderr_text.empty(), "trampoline continue stderr empty");
    require(stdout_text.find("ALR TRAMPOLINE CONTINUE EXEC: PASS") != std::string::npos, "continue exec pass");
    require(stdout_text.find("ALR TRAMPOLINE CONTINUE CONFIG: PASS") != std::string::npos, "continue config pass");
    require(stdout_text.find("ALR TRAMPOLINE CONTINUE CHECKSUM: PASS") != std::string::npos, "continue checksum pass");
    require(stdout_text.find("alr trampoline continue mode=dry-run") != std::string::npos, "continue dry run");
    require(stdout_text.find("alr trampoline continue extra argc=1") != std::string::npos, "continue argc");

    std::filesystem::remove_all(root);
    std::cout << "alr runtime trampoline continue native test ok\n";
    return EXIT_SUCCESS;
}
