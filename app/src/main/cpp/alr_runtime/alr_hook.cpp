#include "alr_runtime/alr_hook.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace alr::runtime {
namespace {

std::string errno_message(const char* action) {
    std::ostringstream out;
    out << action << " failed errno=" << errno << " message=" << std::strerror(errno);
    return out.str();
}

std::string sanitize_first_bytes(const std::vector<char>& bytes, ssize_t count) {
    std::string out;
    for (ssize_t i = 0; i < count; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]);
        if (c == '\n' || c == '\r' || c == '\t') {
            out.push_back(' ');
        } else if (c >= 0x20 && c < 0x7f) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('.');
        }
    }
    return out;
}

}  // namespace

PathHookSmokeResult run_path_hook_smoke(
    std::string_view rootfs_dir,
    std::string_view cwd,
    std::string_view path,
    std::size_t max_read_bytes) {
    PathHookSmokeResult result;
    result.translation = translate_rootfs_path(rootfs_dir, cwd, path);

    struct stat st {};
    if (::stat(result.translation.host_path.c_str(), &st) == 0) {
        result.stated = true;
        result.size_bytes = static_cast<long long>(st.st_size);
    } else {
        result.error = errno_message("stat");
    }

    const int fd = ::open(result.translation.host_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        result.opened = true;
        std::vector<char> buffer(max_read_bytes == 0 ? 1 : max_read_bytes);
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count >= 0) {
            result.first_bytes = sanitize_first_bytes(buffer, count);
        } else if (result.error.empty()) {
            result.error = errno_message("read");
        }
        ::close(fd);
    } else if (result.error.empty()) {
        result.error = errno_message("open");
    }

    std::ostringstream report;
    // Was an unconditional "PASS" printed directly above code that had
    // already computed the outcome. A verdict line that cannot say FAIL is
    // not a verdict; it is a label wearing one, and the reader cannot tell
    // the difference. This repo has shipped that shape in several files.
    report << "ALR HOOK LOAD: "
           << ((result.opened && result.stated && result.error.empty()) ? "PASS" : "FAIL");
    if (!result.error.empty()) report << "\nalr hook error=" << result.error;
    report << "\nALR HOOK MODE: host-path-smoke";
    report << "\nALR HOOK GUEST PATH: " << result.translation.guest_path;
    report << "\nALR HOOK HOST PATH: " << result.translation.host_path;
    report << "\nALR STAT ROOTFS FILE: " << (result.stated ? "PASS" : "FAIL");
    report << "\nALR OPEN ROOTFS FILE: " << (result.opened ? "PASS" : "FAIL");
    report << "\nALR HOOK FILE SIZE: " << result.size_bytes;
    report << "\nALR HOOK FIRST BYTES: " << result.first_bytes;
    if (!result.error.empty()) {
        report << "\nALR HOOK ERROR: " << result.error;
    }
    result.report = report.str();
    return result;
}

}  // namespace alr::runtime
