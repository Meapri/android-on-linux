#include "alr_runtime/alr_interposer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "alr_runtime/alr_procfs.hpp"

namespace alr::runtime {
namespace {

bool is_all_digits(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// Match "/proc/self/<suffix>" or "/proc/<pid>/<suffix>" on an already-normalized
// guest path.
bool matches_proc_suffix(const std::string& normalized, std::string_view suffix) {
    constexpr std::string_view kProc = "/proc/";
    if (normalized.compare(0, kProc.size(), kProc) != 0) {
        return false;
    }
    const std::string_view rest = std::string_view(normalized).substr(kProc.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    const std::string_view owner = rest.substr(0, slash);
    const std::string_view tail = rest.substr(slash + 1);
    if (owner != "self" && !is_all_digits(owner)) {
        return false;
    }
    return tail == suffix;
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

InterposedPathResult run_interposer_path_smoke(
    const InterposerConfig& config,
    std::string_view path,
    std::size_t max_read_bytes) {
    InterposedPathResult result;
    result.translation = translate_rootfs_path(config.rootfs_dir, config.cwd, path);

    struct stat st {};
    if (::stat(result.translation.host_path.c_str(), &st) == 0) {
        result.stated = true;
        result.size_bytes = static_cast<long long>(st.st_size);
    } else {
        result.stat_errno = errno;
    }

    const int fd = ::open(result.translation.host_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        result.opened = true;
        std::vector<char> buffer(max_read_bytes == 0 ? 1 : max_read_bytes);
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count >= 0) {
            result.first_bytes = sanitize_first_bytes(buffer, count);
        }
        ::close(fd);
    } else {
        result.open_errno = errno;
    }

    std::ostringstream report;
    // Was an unconditional "PASS" printed directly above code that had
    // already computed the outcome. A verdict line that cannot say FAIL is
    // not a verdict; it is a label wearing one, and the reader cannot tell
    // the difference. This repo has shipped that shape in several files.
    report << "ALR INTERPOSER LOAD: "
           << ((result.opened && result.stated) ? "PASS" : "FAIL");
    if (!result.opened) report << "\nalr interposer open errno=" << result.open_errno;
    if (!result.stated) report << "\nalr interposer stat errno=" << result.stat_errno;
    report << "\nALR INTERPOSER MODE: translated-open-stat-smoke";
    report << "\nALR INTERPOSER GUEST PATH: " << result.translation.guest_path;
    report << "\nALR INTERPOSER HOST PATH: " << result.translation.host_path;
    report << "\nALR INTERPOSER STAT PATH: " << (result.stated ? "PASS" : "FAIL");
    report << "\nALR INTERPOSER OPEN PATH: " << (result.opened ? "PASS" : "FAIL");
    report << "\nALR INTERPOSER FILE SIZE: " << result.size_bytes;
    report << "\nALR INTERPOSER FIRST BYTES: " << result.first_bytes;
    if (!result.stated) {
        report << "\nALR INTERPOSER STAT ERRNO: " << result.stat_errno;
    }
    if (!result.opened) {
        report << "\nALR INTERPOSER OPEN ERRNO: " << result.open_errno;
    }
    result.report = report.str();
    return result;
}

const char* interposed_kind_name(InterposedKind kind) {
    switch (kind) {
        case InterposedKind::RootfsPath:
            return "rootfs-path";
        case InterposedKind::ProcSelfExe:
            return "proc-self-exe";
        case InterposedKind::ProcSelfStatus:
            return "proc-self-status";
        case InterposedKind::ProcMounts:
            return "proc-mounts";
    }
    return "rootfs-path";
}

InterposedResolution resolve_interposed_access(
    const RuntimeConfig& config,
    std::string_view requested_program,
    std::string_view access_path) {
    InterposedResolution result;
    result.guest_path = normalize_guest_path(access_path, config.cwd);

    if (matches_proc_suffix(result.guest_path, "exe")) {
        result.kind = InterposedKind::ProcSelfExe;
    } else if (matches_proc_suffix(result.guest_path, "status")) {
        result.kind = InterposedKind::ProcSelfStatus;
    } else if (result.guest_path == "/proc/mounts" || matches_proc_suffix(result.guest_path, "mounts")) {
        result.kind = InterposedKind::ProcMounts;
    } else {
        result.kind = InterposedKind::RootfsPath;
    }

    if (result.kind != InterposedKind::RootfsPath) {
        const auto plan = build_procfs_virtualization_plan(config, requested_program);
        switch (result.kind) {
            case InterposedKind::ProcSelfExe:
                result.synthetic_link_target = plan.self_exe.guest_view;
                result.virtualized = plan.self_exe.valid;
                break;
            case InterposedKind::ProcSelfStatus: {
                std::string content;
                for (const auto& line : plan.self_status.lines) {
                    content += line;
                    content += "\n";
                }
                result.synthetic_content = content;
                result.virtualized = plan.self_status.valid;
                break;
            }
            case InterposedKind::ProcMounts:
                result.synthetic_content = render_guest_mounts(plan.mounts);
                result.virtualized = plan.mounts.valid;
                break;
            case InterposedKind::RootfsPath:
                break;
        }
    } else {
        const auto translation = translate_rootfs_path(config.rootfs_dir, config.cwd, access_path);
        result.host_path = translation.host_path;
        result.virtualized = false;
    }

    std::ostringstream report;
    // Was an unconditional "PASS" printed directly above code that had
    // already computed the outcome. A verdict line that cannot say FAIL is
    // not a verdict; it is a label wearing one, and the reader cannot tell
    // the difference. This repo has shipped that shape in several files.
    // Resolution succeeded iff it produced something to act on: a translated
    // host path, or a synthesized answer for a /proc entry.
    report << "ALR INTERPOSE RESOLVE: "
           << ((!result.host_path.empty() || !result.synthetic_link_target.empty() ||
                !result.synthetic_content.empty()) ? "PASS" : "FAIL");
    report << "\nALR INTERPOSE KIND: " << interposed_kind_name(result.kind);
    report << "\nALR INTERPOSE VIRTUALIZED: " << (result.virtualized ? "PASS" : "SKIP");
    report << "\nalr interpose guest path=" << result.guest_path;
    report << "\nalr interpose host path=" << (result.host_path.empty() ? "synthetic" : result.host_path);
    if (result.kind == InterposedKind::ProcSelfExe) {
        report << "\nalr interpose synthetic link target=" << result.synthetic_link_target;
    }
    if (result.kind == InterposedKind::ProcSelfStatus || result.kind == InterposedKind::ProcMounts) {
        report << "\nalr interpose synthetic content bytes=" << result.synthetic_content.size();
    }
    result.report = report.str();
    return result;
}

}  // namespace alr::runtime
