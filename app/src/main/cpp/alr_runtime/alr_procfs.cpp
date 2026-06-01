#include "alr_runtime/alr_procfs.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "alr_runtime/alr_exec.hpp"
#include "alr_runtime/alr_path.hpp"

namespace alr::runtime {
namespace {

// Android host mount sources/paths that must never appear in the guest-visible
// /proc/mounts. The configured rootfs host path and any bind host paths are
// appended to this set per-plan so leaks of the real backing store are caught.
const std::vector<std::string>& generic_host_leak_tokens() {
    static const std::vector<std::string> tokens = {
        "/data/",
        "/system",
        "/vendor",
        "/apex",
        "/storage",
        "/mnt/",
        "/dev/block",
        "magisk",
    };
    return tokens;
}

std::string env_value(const RuntimeConfig& config, const char* key) {
    const auto iter = config.env.find(key);
    return iter == config.env.end() ? std::string{} : iter->second;
}

std::string basename_of(std::string_view guest_path) {
    if (guest_path.empty()) {
        return std::string{};
    }
    const std::size_t slash = guest_path.find_last_of('/');
    if (slash == std::string_view::npos) {
        return std::string(guest_path);
    }
    return std::string(guest_path.substr(slash + 1));
}

std::string join_csv(const std::vector<std::string>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += values[i];
    }
    return out;
}

std::string resolve_guest_view(const RuntimeConfig& config, std::string_view requested_program) {
    // Prefer the exec resolver so PATH command names map to a real guest path,
    // but fall back to lexical normalization so the plan still works before the
    // rootfs is extracted.
    try {
        const auto resolution = resolve_guest_executable(config, requested_program);
        if (!resolution.translation.guest_path.empty()) {
            return resolution.translation.guest_path;
        }
    } catch (const std::exception&) {
        // Fall through to lexical handling below.
    }
    try {
        if (!requested_program.empty() && requested_program.find('/') != std::string_view::npos) {
            return normalize_guest_path(requested_program, config.cwd);
        }
    } catch (const std::exception&) {
        // Leave empty; the self-exe view will be marked invalid.
    }
    return std::string{};
}

ProcfsSelfExeView build_self_exe_view(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const ProcfsHostFacts& host_facts) {
    ProcfsSelfExeView view;
    view.guest_view = resolve_guest_view(config, requested_program);
    view.host_truth = host_facts.self_exe_truth.empty()
        ? env_value(config, "ALR_TRAMPOLINE_PATH")
        : host_facts.self_exe_truth;
    view.interpose_paths = {"/proc/self/exe", "/proc/<pid>/exe"};
    view.interpose_syscalls = {"readlink", "readlinkat", "openat"};
    view.requires_virtualization = !view.host_truth.empty() && view.host_truth != view.guest_view;
    view.valid = is_guest_absolute_path(view.guest_view);
    return view;
}

ProcfsSelfStatusView build_self_status_view(
    const RuntimeConfig& config,
    const ProcfsSelfExeView& self_exe,
    const ProcfsHostFacts& host_facts) {
    ProcfsSelfStatusView view;
    view.name = basename_of(self_exe.guest_view);
    view.fakeroot = config.fake_root;
    if (config.fake_root) {
        view.uid = 0;
        view.gid = 0;
    } else if (host_facts.has_identity) {
        view.uid = host_facts.uid;
        view.gid = host_facts.gid;
    } else {
        view.uid = static_cast<long>(::getuid());
        view.gid = static_cast<long>(::getgid());
    }
    std::ostringstream uid_line;
    uid_line << "Uid:\t" << view.uid << "\t" << view.uid << "\t" << view.uid << "\t" << view.uid;
    std::ostringstream gid_line;
    gid_line << "Gid:\t" << view.gid << "\t" << view.gid << "\t" << view.gid << "\t" << view.gid;
    view.lines = {
        "Name:\t" + view.name,
        uid_line.str(),
        gid_line.str(),
    };
    view.valid = !view.name.empty() && view.uid >= 0 && view.gid >= 0;
    return view;
}

ProcfsMountsView build_mounts_view(const RuntimeConfig& config) {
    ProcfsMountsView view;
    view.entries.push_back({"alr_rootfs", "/", "overlay", "rw,relatime", config.rootfs_dir});
    view.entries.push_back({"proc", "/proc", "proc", "rw,nosuid,nodev,noexec,relatime", ""});
    view.entries.push_back({"sysfs", "/sys", "sysfs", "rw,nosuid,nodev,noexec,relatime", ""});
    view.entries.push_back({"tmpfs", "/tmp", "tmpfs", "rw,nosuid,nodev,relatime", ""});
    view.entries.push_back({"devpts", "/dev/pts", "devpts", "rw,nosuid,noexec,relatime", ""});
    view.entries.push_back({"tmpfs", "/dev/shm", "tmpfs", "rw,nosuid,nodev,relatime", ""});
    for (const auto& bind : config.binds) {
        std::string guest_target;
        try {
            guest_target = normalize_guest_path(bind.guest_path, config.cwd);
        } catch (const std::exception&) {
            guest_target = bind.guest_path;
        }
        // Source is a synthetic label so the real host backing never reaches the
        // guest view; the host path is retained only for diagnostics.
        view.entries.push_back({"alr_bind", guest_target, "none", "rw,bind,relatime", bind.host_path});
    }

    for (const auto& entry : view.entries) {
        if (entry.target == "/") {
            view.has_guest_root = true;
        }
        if (entry.target == "/proc") {
            view.has_proc = true;
        }
    }

    // Leak guard: scan only the source and options columns. Mount targets are
    // guest-namespace paths by construction (e.g. /mnt/share is a legitimate
    // guest bind point), so a host backing path can only leak through the
    // source or options, never the target.
    std::string guest_text;
    for (const auto& entry : view.entries) {
        guest_text += entry.source;
        guest_text += " ";
        guest_text += entry.options;
        guest_text += "\n";
    }
    std::vector<std::string> tokens = generic_host_leak_tokens();
    if (!config.rootfs_dir.empty()) {
        tokens.push_back(config.rootfs_dir);
    }
    for (const auto& bind : config.binds) {
        if (!bind.host_path.empty()) {
            tokens.push_back(bind.host_path);
        }
    }
    for (const auto& token : tokens) {
        if (!token.empty() && guest_text.find(token) != std::string::npos) {
            view.leaked_host_tokens.push_back(token);
        }
    }

    view.valid = view.has_guest_root && view.has_proc && !view.entries.empty();
    return view;
}

std::string pass_fail(bool value) {
    return value ? "PASS" : "FAIL";
}

bool self_exe_plan_ok(const ProcfsSelfExeView& view) {
    return view.valid && view.requires_virtualization &&
        !view.interpose_paths.empty() && !view.interpose_syscalls.empty();
}

std::string build_report(const ProcfsVirtualizationPlan& plan) {
    const bool exe_ok = self_exe_plan_ok(plan.self_exe);
    const bool status_ok = plan.self_status.valid;
    const bool mounts_ok = plan.mounts.valid;
    const bool leak_ok = plan.mounts.leaked_host_tokens.empty();

    std::ostringstream out;
    out << "ALR PROCFS PLAN: " << pass_fail(plan.planned);
    out << "\nALR PROCFS SELF EXE PLAN: " << pass_fail(exe_ok);
    out << "\nALR PROCFS SELF STATUS FAKEROOT PLAN: " << pass_fail(status_ok);
    out << "\nALR PROCFS MOUNTS PLAN: " << pass_fail(mounts_ok);
    out << "\nALR PROCFS HOST LEAK GUARD: " << pass_fail(leak_ok);
    out << "\nalr procfs self exe guest view=" << plan.self_exe.guest_view;
    out << "\nalr procfs self exe host truth=" << plan.self_exe.host_truth;
    out << "\nalr procfs self exe requires virtualization="
        << (plan.self_exe.requires_virtualization ? "true" : "false");
    out << "\nalr procfs self exe interpose paths=" << join_csv(plan.self_exe.interpose_paths);
    out << "\nalr procfs self exe interpose syscalls=" << join_csv(plan.self_exe.interpose_syscalls);
    out << "\nalr procfs self status name=" << plan.self_status.name;
    out << "\nalr procfs self status uid=" << plan.self_status.uid;
    out << "\nalr procfs self status gid=" << plan.self_status.gid;
    out << "\nalr procfs self status fakeroot=" << (plan.self_status.fakeroot ? "true" : "false");
    out << "\nalr procfs mounts entries=" << plan.mounts.entries.size();
    out << "\nalr procfs mounts has guest root=" << (plan.mounts.has_guest_root ? "true" : "false");
    out << "\nalr procfs mounts has proc=" << (plan.mounts.has_proc ? "true" : "false");
    out << "\nalr procfs mounts host leak tokens=" << plan.mounts.leaked_host_tokens.size();
    for (std::size_t i = 0; i < plan.mounts.entries.size(); ++i) {
        const auto& entry = plan.mounts.entries[i];
        out << "\nalr procfs mount[" << i << "]=" << entry.source << " " << entry.target
            << " " << entry.fstype << " " << entry.options;
    }
    if (!plan.error.empty()) {
        out << "\nalr procfs error=" << plan.error;
    }
    return out.str();
}

}  // namespace

std::string render_guest_mounts(const ProcfsMountsView& mounts) {
    std::ostringstream out;
    for (const auto& entry : mounts.entries) {
        out << entry.source << " " << entry.target << " " << entry.fstype << " "
            << entry.options << " 0 0\n";
    }
    return out.str();
}

ProcfsVirtualizationPlan build_procfs_virtualization_plan(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const ProcfsHostFacts& host_facts) {
    ProcfsVirtualizationPlan plan;
    try {
        plan.self_exe = build_self_exe_view(config, requested_program, host_facts);
        plan.self_status = build_self_status_view(config, plan.self_exe, host_facts);
        plan.mounts = build_mounts_view(config);
        plan.planned = self_exe_plan_ok(plan.self_exe) &&
            plan.self_status.valid &&
            plan.mounts.valid &&
            plan.mounts.leaked_host_tokens.empty();
        if (!plan.planned && plan.error.empty()) {
            if (!plan.self_exe.valid) {
                plan.error = "self-exe guest view could not be resolved";
            } else if (!plan.self_exe.requires_virtualization) {
                plan.error = "self-exe host truth is unknown so virtualization gap is undefined";
            } else if (!plan.self_status.valid) {
                plan.error = "self-status identity is incomplete";
            } else if (!plan.mounts.valid) {
                plan.error = "synthesized mount table is missing the guest root or /proc";
            } else if (!plan.mounts.leaked_host_tokens.empty()) {
                plan.error = "synthesized mount table leaks an android host path";
            }
        }
    } catch (const std::exception& exc) {
        plan.error = exc.what();
    }
    plan.report = build_report(plan);
    return plan;
}

}  // namespace alr::runtime
