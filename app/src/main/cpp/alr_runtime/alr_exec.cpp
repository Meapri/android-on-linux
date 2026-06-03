#include "alr_runtime/alr_exec.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alr::runtime {
namespace {

constexpr std::size_t kProbeBytes = 256;

std::string errno_message(const char* action) {
    std::ostringstream out;
    out << action << " failed errno=" << errno << " message=" << std::strerror(errno);
    return out.str();
}

std::vector<std::string> split_colon_paths(std::string_view value) {
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t colon = value.find(':', start);
        const std::size_t end = colon == std::string_view::npos ? value.size() : colon;
        const std::string_view part = value.substr(start, end - start);
        if (!part.empty() && is_guest_absolute_path(part)) {
            paths.emplace_back(part);
        }
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    return paths;
}

std::string join_guest_path(const std::string& dir, std::string_view name) {
    if (dir == "/") {
        return "/" + std::string(name);
    }
    return dir + "/" + std::string(name);
}

bool is_plain_command_name(std::string_view value) {
    return !value.empty() && value.find('/') == std::string_view::npos;
}

bool requires_path_lookup(std::string_view requested, ExecContinuationKind kind) {
    return kind == ExecContinuationKind::Execvp && is_plain_command_name(requested);
}

std::string configured_trampoline_path(const RuntimeConfig& config) {
    const auto iter = config.env.find("ALR_TRAMPOLINE_PATH");
    return iter == config.env.end() ? "" : iter->second;
}

std::vector<std::string> candidate_guest_paths(const RuntimeConfig& config, std::string_view requested) {
    if (requested.empty()) {
        throw std::invalid_argument("requested program must not be empty");
    }
    if (is_guest_absolute_path(requested) || requested.find('/') != std::string_view::npos) {
        return {normalize_guest_path(requested, config.cwd)};
    }
    if (!is_plain_command_name(requested)) {
        throw std::invalid_argument("requested program must be an absolute path or PATH command name");
    }
    const auto path_iter = config.env.find("PATH");
    const std::string path_value = path_iter == config.env.end()
        ? "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
        : path_iter->second;
    std::vector<std::string> candidates;
    for (const auto& dir : split_colon_paths(path_value)) {
        candidates.push_back(normalize_guest_path(join_guest_path(dir, requested)));
    }
    return candidates;
}

std::vector<unsigned char> read_prefix(const std::string& host_path) {
    const int fd = ::open(host_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(errno_message("open executable"));
    }
    std::vector<unsigned char> bytes(kProbeBytes);
    const ssize_t count = ::read(fd, bytes.data(), bytes.size());
    const int saved_errno = errno;
    ::close(fd);
    if (count < 0) {
        errno = saved_errno;
        throw std::runtime_error(errno_message("read executable"));
    }
    bytes.resize(static_cast<std::size_t>(count));
    return bytes;
}

ShebangInfo parse_shebang(const std::vector<unsigned char>& bytes) {
    std::string line;
    for (std::size_t i = 2; i < bytes.size(); ++i) {
        const char c = static_cast<char>(bytes[i]);
        if (c == '\n' || c == '\r') {
            break;
        }
        line.push_back(c);
    }
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    std::size_t end = line.size();
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        --end;
    }
    const std::string trimmed = line.substr(start, end - start);
    const std::size_t split = trimmed.find_first_of(" \t");
    if (split == std::string::npos) {
        return ShebangInfo{.interpreter = trimmed, .argument = ""};
    }
    std::size_t arg_start = split;
    while (arg_start < trimmed.size() && (trimmed[arg_start] == ' ' || trimmed[arg_start] == '\t')) {
        ++arg_start;
    }
    return ShebangInfo{
        .interpreter = trimmed.substr(0, split),
        .argument = trimmed.substr(arg_start),
    };
}

ExecutableKind classify_bytes(const std::vector<unsigned char>& bytes, ShebangInfo& shebang) {
    // ELF magic: 0x7f 'E' 'L' 'F'.
    if (bytes.size() >= 4 && bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F') {
        return ExecutableKind::Elf;
    }
    if (bytes.size() >= 2 && bytes[0] == '#' && bytes[1] == '!') {
        shebang = parse_shebang(bytes);
        return shebang.interpreter.empty() ? ExecutableKind::Unsupported : ExecutableKind::Shebang;
    }
    return ExecutableKind::Unsupported;
}

std::string build_report(const ExecutableResolution& result) {
    std::ostringstream out;
    out << "ALR EXEC RESOLVE: " << (result.resolved ? "PASS" : "FAIL");
    out << "\nALR EXEC CLASSIFY: " << (result.classified ? "PASS" : "FAIL");
    out << "\nALR EXEC STRATEGY: " << result.strategy;
    out << "\nalr exec guest path=" << result.translation.guest_path;
    out << "\nalr exec host path=" << result.translation.host_path;
    out << "\nalr exec kind=" << executable_kind_name(result.kind);
    if (!result.shebang.interpreter.empty()) {
        out << "\nalr exec shebang interpreter=" << result.shebang.interpreter;
    }
    if (!result.shebang.argument.empty()) {
        out << "\nalr exec shebang argument=" << result.shebang.argument;
    }
    if (!result.error.empty()) {
        out << "\nalr exec error=" << result.error;
    }
    return out.str();
}

RuntimeConfig child_config_for_continuation(
    const RuntimeConfig& config,
    const ExecutableResolution& resolution) {
    RuntimeConfig child = config;
    child.program = resolution.translation.guest_path;
    child.env["ALR_PROGRAM"] = resolution.translation.guest_path;
    child.env["ALR_CONTINUATION_TARGET_GUEST_PATH"] = resolution.translation.guest_path;
    return child;
}

std::string plan_status(const ExecContinuationPlan& plan) {
    return plan.planned ? "PASS" : "FAIL";
}

std::string kind_line_status(const ExecContinuationPlan& plan, ExecContinuationKind kind) {
    return plan.kind == kind ? plan_status(plan) : "SKIP";
}

std::string build_continuation_report(const ExecContinuationPlan& plan) {
    std::ostringstream out;
    out << "ALR EXEC CONTINUITY PLAN: " << plan_status(plan);
    out << "\nALR EXECVE CHILD CONTINUITY PLAN: " << kind_line_status(plan, ExecContinuationKind::Execve);
    out << "\nALR EXECVP PATH LOOKUP PLAN: " << kind_line_status(plan, ExecContinuationKind::Execvp);
    out << "\nALR POSIX_SPAWN CHILD PLAN: " << kind_line_status(plan, ExecContinuationKind::PosixSpawn);
    out << "\nALR EXEC CONTINUITY TRAMPOLINE: " << (plan.uses_packaged_trampoline ? "PASS" : "FAIL");
    out << "\nALR EXEC CONTINUITY CONFIG HANDOFF: " << (plan.config_handoff ? "PASS" : "FAIL");
    out << "\nalr exec continuation kind=" << exec_continuation_kind_name(plan.kind);
    out << "\nalr exec continuation path lookup=" << (plan.path_lookup ? "true" : "false");
    out << "\nalr exec continuation target guest path=" << plan.resolution.translation.guest_path;
    out << "\nalr exec continuation host path diagnostic=" << plan.resolution.translation.host_path;
    out << "\nalr exec continuation argv count=" << plan.argv.size();
    if (!plan.argv.empty()) {
        out << "\nalr exec continuation argv0=" << plan.argv.front();
    }
    out << "\nalr exec continuation env overrides=" << plan.env_overrides.size();
    if (!plan.error.empty()) {
        out << "\nalr exec continuation error=" << plan.error;
    }
    return out.str();
}

}  // namespace

const char* executable_kind_name(ExecutableKind kind) {
    switch (kind) {
        case ExecutableKind::Missing:
            return "missing";
        case ExecutableKind::Elf:
            return "elf";
        case ExecutableKind::Shebang:
            return "shebang";
        case ExecutableKind::Unsupported:
            return "unsupported";
    }
    return "unsupported";
}

const char* exec_continuation_kind_name(ExecContinuationKind kind) {
    switch (kind) {
        case ExecContinuationKind::Execve:
            return "execve";
        case ExecContinuationKind::Execvp:
            return "execvp";
        case ExecContinuationKind::PosixSpawn:
            return "posix_spawn";
    }
    return "execve";
}

namespace {

// Path-prefix containment with a component boundary: "/proc" matches "/proc"
// and "/proc/foo" but NOT "/process". Byte-identical to the supervisor's
// `under` lambda (runtime_report.cpp EVENT_SECCOMP handler) so the host-tested
// decision here and the on-device rewrite classify paths the same way.
bool path_under(std::string_view path, std::string_view dir) {
    if (dir.empty()) {
        return false;
    }
    if (path.size() < dir.size()) {
        return false;
    }
    if (path.compare(0, dir.size(), dir) != 0) {
        return false;
    }
    if (path.size() == dir.size()) {
        return true;
    }
    return path[dir.size()] == '/';
}

}  // namespace

ExecPathMediation decide_exec_path_mediation(
    std::string_view rootfs_dir,
    std::string_view guest_path) {
    ExecPathMediation out;
    out.guest_path.assign(guest_path);
    if (guest_path.empty()) {
        out.reason = "empty";
        return out;
    }
    // Relative paths resolve against the guest cwd; the supervisor leaves them
    // native (the path-family rewrite only mediates absolute guest paths).
    if (guest_path.front() != '/') {
        out.reason = "relative";
        return out;
    }
    // Kernel virtual filesystems have no rootfs backing — leaving them native
    // keeps /proc/self/exe and friends valid (ADR-003 §3, §4-가정-3). NOTE: this
    // means a guest that execs /proc/self/exe is intentionally NOT redirected
    // into the rootfs; that is a device-only correctness question (the loaded
    // image may then be the host binary), tracked as ADR-003 §4-가정-3.
    if (path_under(guest_path, "/proc") || path_under(guest_path, "/sys") ||
        path_under(guest_path, "/dev")) {
        out.reason = "sysdir";
        return out;
    }
    // Idempotency guard: a path the guest already presents as a rootfs host path
    // (e.g. learned from /proc/self/maps, or rewritten by the interposer) must
    // not be re-prefixed into <rootfs><rootfs>/… (the open would fail).
    if (path_under(guest_path, rootfs_dir)) {
        out.reason = "already-host";
        return out;
    }
    // Any other absolute guest path is mediated into the rootfs. translate_
    // rootfs_path is pure in (rootfs_dir, cwd, path); cwd is irrelevant for an
    // absolute path, so "/" is passed deterministically (matches the supervisor,
    // which fixes cwd="/" for the run).
    const auto t = translate_rootfs_path(rootfs_dir, "/", guest_path);
    out.host_path = t.host_path;
    out.should_rewrite = true;
    out.reason = "rewrite";
    return out;
}

namespace {

// Split a "KEY=VALUE" (or bare "KEY") env entry at the FIRST '='. A bare "KEY"
// (no '=') yields {key, ""} — matched as a present-but-empty var, which is the
// POSIX convention for environ entries. Mirrors how getenv() keys on the prefix
// up to the first '='.
std::pair<std::string_view, std::string_view> split_env_entry(std::string_view entry) {
    const std::size_t eq = entry.find('=');
    if (eq == std::string_view::npos) {
        return {entry, std::string_view{}};
    }
    return {entry.substr(0, eq), entry.substr(eq + 1)};
}

// True iff `needle` appears as a whole colon-delimited element of `list` (an
// LD_PRELOAD value). "/a/b.so" is contained in "/a/b.so:/c.so" but NOT in
// "/x/a/b.so.0" — element boundaries are the ':' separators (and string ends).
bool colon_list_contains(std::string_view list, std::string_view needle) {
    if (needle.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t colon = list.find(':', start);
        const std::size_t end = colon == std::string_view::npos ? list.size() : colon;
        if (list.substr(start, end - start) == needle) {
            return true;
        }
        if (colon == std::string_view::npos) {
            break;
        }
        start = colon + 1;
    }
    return false;
}

}  // namespace

ExecEnvpInjection decide_exec_envp_injection(
    std::string_view rootfs_dir,
    const std::vector<std::string>& env_entries,
    bool fakeroot) {
    ExecEnvpInjection out;
    // The interposer self-disables (pure passthrough) when ALR_ROOTFS is unset, so
    // with no rootfs there is nothing to mediate — leave the child's envp alone.
    if (rootfs_dir.empty()) {
        out.reason = "already";
        return out;
    }
    // Derive the target values purely from rootfs_dir. These are the SAME strings
    // the parent loader pushes into the first guest's env (runtime_report env-setup):
    // LD_PRELOAD must be the ABSOLUTE ROOTFS host path (R3 finding) and ALR_ROOTFS is
    // the rootfs dir itself. Under fakeroot the fakeroot .so chains FIRST.
    out.rootfs_value.assign(rootfs_dir);
    out.interpose_so = std::string(rootfs_dir) + "/usr/lib/androlinux/libalr_interpose.so";
    const std::string fakeroot_so =
        std::string(rootfs_dir) + "/usr/lib/androlinux/libalr_fakeroot.so";

    // The desired leading chain (our shims), in order. Mirrors chain_ld_preload()
    // in tools/aptdrain_env_model.py: [fakeroot?] interpose. interpose is never
    // dropped; fakeroot is FIRST so its credential wrappers run outermost.
    std::vector<std::string> desired;
    if (fakeroot) {
        desired.push_back(fakeroot_so);
    }
    desired.push_back(out.interpose_so);

    // Scan the child's existing envp for the two vars (first occurrence wins, as
    // libc's environ lookup does). We capture the existing LD_PRELOAD value so we
    // can PREPEND our shims while preserving the guest's own preloads.
    bool have_ld_preload = false;
    std::string_view existing_ld_preload;
    bool have_rootfs = false;
    for (const auto& entry : env_entries) {
        const auto [key, value] = split_env_entry(entry);
        if (!have_ld_preload && key == "LD_PRELOAD") {
            have_ld_preload = true;
            existing_ld_preload = value;
        } else if (!have_rootfs && key == "ALR_ROOTFS") {
            have_rootfs = true;
        }
    }

    // LD_PRELOAD is satisfied iff EVERY desired shim already appears as a whole
    // colon-element (idempotent re-exec of a child the parent already injected).
    bool ld_preload_satisfied = have_ld_preload;
    for (const auto& so : desired) {
        if (!colon_list_contains(existing_ld_preload, so)) {
            ld_preload_satisfied = false;
            break;
        }
    }

    const bool need_ld = !ld_preload_satisfied;       // absent OR missing a desired shim
    const bool need_rootfs = !have_rootfs;

    if (need_ld) {
        if (have_ld_preload) {
            // Guest set its own LD_PRELOAD (or a parent injected only part of our
            // chain): rebuild as [desired shims FIRST] + [guest preloads minus our
            // shims], de-duped, order preserved within the guest tail. The
            // supervisor DROPs the old LD_PRELOAD entry and appends this combined one.
            out.replace_ld_preload = true;
            std::string built;
            std::vector<std::string_view> seen;
            const auto append = [&](std::string_view v) {
                if (v.empty()) {
                    return;
                }
                for (const auto& s : seen) {
                    if (s == v) {
                        return;
                    }
                }
                seen.push_back(v);
                if (!built.empty()) {
                    built += ':';
                }
                built.append(v);
            };
            for (const auto& so : desired) {
                append(so);
            }
            // Preserve guest preloads after ours, dropping duplicates of our shims.
            std::size_t start = 0;
            const std::string_view list = existing_ld_preload;
            while (start <= list.size()) {
                const std::size_t colon = list.find(':', start);
                const std::size_t end =
                    colon == std::string_view::npos ? list.size() : colon;
                append(list.substr(start, end - start));
                if (colon == std::string_view::npos) {
                    break;
                }
                start = colon + 1;
            }
            out.ld_preload_value = std::move(built);
        } else {
            // No guest LD_PRELOAD: a fresh append of just our shim chain.
            std::string built;
            for (const auto& so : desired) {
                if (!built.empty()) {
                    built += ':';
                }
                built += so;
            }
            out.ld_preload_value = std::move(built);
        }
        out.add_entries.push_back("LD_PRELOAD=" + out.ld_preload_value);
    }
    if (need_rootfs) {
        out.add_entries.push_back("ALR_ROOTFS=" + out.rootfs_value);
    }

    out.should_inject = !out.add_entries.empty() || out.replace_ld_preload;
    if (!out.should_inject) {
        out.reason = "already";
    } else if (out.replace_ld_preload) {
        out.reason = fakeroot ? "prepend-fakeroot" : "prepend-ld";
    } else if (need_ld && need_rootfs) {
        out.reason = "inject-both";
    } else if (need_ld) {
        out.reason = "inject-ld";
    } else {
        out.reason = "inject-rootfs";
    }
    return out;
}

namespace {

// Basename of a path view: the substring after the last '/'. "a/b/chromium" ->
// "chromium"; "chromium" (no slash) -> "chromium"; "a/b/" -> "" (trailing slash).
std::string_view path_basename(std::string_view p) {
    const std::size_t slash = p.find_last_of('/');
    if (slash == std::string_view::npos) {
        return p;
    }
    return p.substr(slash + 1);
}

}  // namespace

ChromiumChildArgv decide_chromium_child_argv(
    std::string_view program_path,
    const std::vector<std::string>& argv) {
    ChromiumChildArgv out;
    // Only the full chromium browser binary forks children that drop our sandbox
    // flags. content_shell ("chromium-shell") is --single-process and never does, and
    // chrome_crashpad_handler is neutered separately — match the exact basenames so a
    // helper like "chromium-shell" or "chrome_crashpad_handler" is NOT treated as a
    // chromium browser child here.
    const std::string_view base = path_basename(program_path);
    if (base != "chromium" && base != "chrome") {
        out.reason = "not-chromium";
        return out;
    }
    // The sandbox-disable switches every chromium child must carry inside an
    // untrusted_app domain (no usable sandbox transport exists there). Order matches
    // the launch argv so a device log reads consistently.
    static const char* const kSandboxFlags[] = {
        "--no-sandbox",
        "--disable-seccomp-filter-sandbox",
        "--disable-setuid-sandbox",
        "--disable-namespace-sandbox",
        "--disable-gpu-sandbox",
    };
    for (const char* flag : kSandboxFlags) {
        bool present = false;
        for (const std::string& a : argv) {
            if (a == flag) {
                present = true;
                break;
            }
        }
        if (!present) {
            out.add_flags.emplace_back(flag);
        }
    }
    if (out.add_flags.empty()) {
        out.reason = "already";  // chromium child, all flags already present
        return out;
    }
    out.should_inject = true;
    out.reason = "inject";
    return out;
}

ExecutableResolution resolve_guest_executable(
    const RuntimeConfig& config,
    std::string_view requested_program) {
    ExecutableResolution result;
    const auto candidates = candidate_guest_paths(config, requested_program);
    for (const auto& guest_path : candidates) {
        result.translation = translate_rootfs_path(config.rootfs_dir, config.cwd, guest_path);
        struct stat st {};
        if (::stat(result.translation.host_path.c_str(), &st) != 0) {
            result.error = errno_message("stat executable");
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            result.error = "resolved executable is not a regular file";
            result.kind = ExecutableKind::Unsupported;
            result.resolved = true;
            result.classified = true;
            result.report = build_report(result);
            return result;
        }
        result.resolved = true;
        try {
            auto bytes = read_prefix(result.translation.host_path);
            result.kind = classify_bytes(bytes, result.shebang);
            result.classified = true;
            result.error.clear();
        } catch (const std::exception& exc) {
            result.kind = ExecutableKind::Unsupported;
            result.error = exc.what();
        }
        result.report = build_report(result);
        return result;
    }
    if (result.translation.guest_path.empty() && !candidates.empty()) {
        result.translation = translate_rootfs_path(config.rootfs_dir, config.cwd, candidates.front());
    }
    result.kind = ExecutableKind::Missing;
    result.resolved = false;
    result.classified = false;
    result.report = build_report(result);
    return result;
}

ExecContinuationPlan build_exec_continuation_plan(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const std::vector<std::string>& arguments,
    ExecContinuationKind kind) {
    ExecContinuationPlan plan;
    plan.kind = kind;
    plan.path_lookup = requires_path_lookup(requested_program, kind);
    try {
        plan.resolution = resolve_guest_executable(config, requested_program);
        if (!plan.resolution.resolved || plan.resolution.kind == ExecutableKind::Missing) {
            plan.error = plan.resolution.error.empty() ? "child executable was not resolved" : plan.resolution.error;
            plan.report = build_continuation_report(plan);
            return plan;
        }
        if (plan.resolution.kind == ExecutableKind::Unsupported) {
            plan.error = "child executable kind is unsupported";
            plan.report = build_continuation_report(plan);
            return plan;
        }

        const std::string trampoline_path = configured_trampoline_path(config);
        plan.uses_packaged_trampoline = !trampoline_path.empty();
        if (!plan.uses_packaged_trampoline) {
            plan.error = "ALR_TRAMPOLINE_PATH is not configured";
            plan.report = build_continuation_report(plan);
            return plan;
        }

        const auto child_config = child_config_for_continuation(config, plan.resolution);
        const auto serialized = serialize_runtime_config(child_config);
        const auto parsed = parse_runtime_config(serialized.text);
        plan.config_handoff =
            parsed.program == plan.resolution.translation.guest_path &&
            parsed.rootfs_dir == config.rootfs_dir &&
            runtime_config_checksum_hex(serialized.text) == serialized.checksum_hex;

        plan.argv = {
            trampoline_path,
            "--continue-exec",
            "--kind",
            exec_continuation_kind_name(kind),
            "--",
            plan.resolution.translation.guest_path,
        };
        plan.argv.insert(plan.argv.end(), arguments.begin(), arguments.end());
        plan.env_overrides = {
            "ALR_CONFIG_TEXT=" + serialized.text,
            "ALR_CONFIG_CHECKSUM=" + serialized.checksum_hex,
            std::string{"ALR_CONTINUATION_KIND="} + exec_continuation_kind_name(kind),
            "ALR_CONTINUATION_TARGET_GUEST_PATH=" + plan.resolution.translation.guest_path,
            "ALR_CONTINUATION_ARGC=" + std::to_string(arguments.size()),
            std::string{"ALR_CONTINUATION_PATH_LOOKUP="} + (plan.path_lookup ? "1" : "0"),
        };
        plan.planned = plan.uses_packaged_trampoline && plan.config_handoff;
    } catch (const std::exception& exc) {
        plan.error = exc.what();
    }
    plan.report = build_continuation_report(plan);
    return plan;
}

}  // namespace alr::runtime
