#include <fstream>
#include "alr_runtime/alr_wx.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace alr::runtime {
namespace {

std::string env_value(const RuntimeConfig& config, const char* key) {
    const auto iter = config.env.find(key);
    return iter == config.env.end() ? std::string{} : iter->second;
}

bool path_is_inside(const std::string& path, const std::string& prefix) {
    if (prefix.empty() || path.size() < prefix.size()) {
        return false;
    }
    if (path.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    // Require a path boundary so "/data/rootfsX" is not treated as inside
    // "/data/rootfs".
    return path.size() == prefix.size() || path[prefix.size()] == '/' ||
        (!prefix.empty() && prefix.back() == '/');
}

// The domain we are in, e.g. "u:r:untrusted_app_27:s0:c28,...". Read, not
// inferred: a build-time targetSdk tells you what was requested, and this
// tells you what the kernel actually gave us.
std::string current_selinux_domain() {
    std::ifstream f("/proc/self/attr/current");
    std::string s;
    if (f) std::getline(f, s);
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n')) s.pop_back();
    return s;
}

// Only untrusted_app_25 and untrusted_app_27 (and runas_app, which is for
// debugging) carry app_data_file:file execute_no_trans. Everywhere else the
// W^X rule applies and a writable app-data file can never be an exec target.
bool domain_allows_app_data_exec(const std::string& domain) {
    return domain.find(":untrusted_app_27:") != std::string::npos ||
           domain.find(":untrusted_app_25:") != std::string::npos ||
           domain.find(":runas_app:") != std::string::npos;
}

std::vector<GuestLoadCandidate> ranked_guest_load_methods(bool app_data_exec_allowed) {
    std::vector<GuestLoadCandidate> out;
    if (app_data_exec_allowed) {
        // First, because it is the only one MEASURED to work here, and because
        // it is the cheapest by a wide margin: the kernel just runs the file.
        out.push_back({
            GuestLoadMethod::DirectRootfsExecve,
            "direct-rootfs-execve",
            true,
            "app_data_file:file execute_no_trans for this domain",
            "the domain grants execute on app-private files, so the guest loader "
            "can be exec'd directly -- no in-memory copy, no userspace loader",
        });
    }
    // anon-mmap-loader BEFORE memfd-execveat: on SM-X236N / Android 16 the
    // memfd path is denied (EACCES -- SELinux refuses to execute a file-backed
    // memfd) while the anonymous RW->RX page works, so the original ranking had
    // them backwards against the device.
    for (auto& c : std::vector<GuestLoadCandidate>{
        {
            GuestLoadMethod::AnonMmapLoader,
            "anon-mmap-loader",
            true,
            "execmem for the app domain",
            "a userspace loader copies segments into anonymous RW->RX memory, so no "
            "writable file ever carries execute permission",
        },
        {
            GuestLoadMethod::MemfdExecveat,
            "memfd-execveat",
            true,
            "execute on the anonymous memfd/tmpfs label for the app domain",
            "copies the guest ELF into an anonymous in-memory fd; MEASURED DENIED "
            "on this device (EACCES) so it ranks below the anon-mmap loader",
        },
        {
            GuestLoadMethod::ProotBaseline,
            "proot-baseline",
            false,
            "none (ptrace-mediated, no native exec page is created)",
            "known-working fallback with the highest overhead; keeps a guaranteed path "
            "when the native mechanisms are blocked by device policy",
        },
    }) out.push_back(c);
    return out;
}

std::vector<RejectedExecCandidate> rejected_exec_methods(const std::string& target_host_path,
                                                        bool app_data_exec_allowed) {
    // Nothing is rejected when the domain permits it. Listing direct exec as
    // "rejected" while the device runs it is how this file came to contradict
    // its own report -- ALR WX-SAFE DIRECT ROOTFS EXEC REJECTED: PASS printed
    // seventy lines above ALR DIRECT APP-DATA EXECVE: PASS.
    if (app_data_exec_allowed) return {};
    return {
        {
            RejectedExecMethod::DirectRootfsExecve,
            "direct-rootfs-execve",
            "execve() needs execute permission on " + target_host_path +
                " which is an app_data_file; this domain lacks execute_no_trans",
        },
        {
            RejectedExecMethod::FileBackedMmapExec,
            "file-backed-mmap-exec",
            "mmap PROT_EXEC of " + target_host_path +
                " needs file execute permission, also denied in this domain",
        },
    };
}

std::string pass_fail(bool value) {
    return value ? "PASS" : "FAIL";
}

std::string fallback_chain(const std::vector<GuestLoadCandidate>& methods) {
    std::string out;
    for (std::size_t i = 0; i < methods.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += methods[i].name;
    }
    return out;
}

bool file_backed_mmap_rejected(const std::vector<RejectedExecCandidate>& rejected) {
    for (const auto& candidate : rejected) {
        if (candidate.method == RejectedExecMethod::FileBackedMmapExec) {
            return true;
        }
    }
    return false;
}

std::string build_report(const WxSafeExecStrategy& strategy) {
    std::ostringstream out;
    out << "ALR WX-SAFE EXEC STRATEGY: " << pass_fail(strategy.planned);
    out << "\nALR WX-SAFE ENTRYPOINT IS PACKAGED: " << pass_fail(strategy.entrypoint_is_packaged);
    out << "\nalr wx selinux domain=" << (strategy.selinux_domain.empty()
            ? std::string("(unknown)") : strategy.selinux_domain);
    out << "\nalr wx app-data exec allowed=" << (strategy.app_data_exec_allowed ? "yes" : "no");
    out << "\nALR WX-SAFE DIRECT ROOTFS EXEC REJECTED: "
        << (strategy.app_data_exec_allowed ? "N/A (this domain permits it)"
                                           : pass_fail(strategy.rejects_direct_rootfs_exec));
    out << "\nALR WX-SAFE FILE-BACKED MMAP EXEC REJECTED: "
        << (strategy.app_data_exec_allowed
                ? std::string("N/A (this domain permits file execute)")
                : pass_fail(file_backed_mmap_rejected(strategy.rejected_methods)));
    out << "\nALR WX-SAFE PROOT BASELINE AVAILABLE: " << pass_fail(strategy.proot_baseline_available);
    out << "\nalr wx entrypoint=" << strategy.entrypoint;
    out << "\nalr wx target rootfs host path diagnostic=" << strategy.target_host_path;
    out << "\nalr wx primary method=" << guest_load_method_name(strategy.primary);
    out << "\nalr wx fallback chain=" << fallback_chain(strategy.ranked_methods);
    for (std::size_t i = 0; i < strategy.ranked_methods.size(); ++i) {
        const auto& method = strategy.ranked_methods[i];
        out << "\nalr wx method[" << i << "]=" << method.name
            << " device-selinux-check=" << (method.requires_device_selinux_check ? "true" : "false")
            << " leans-on=" << method.selinux_lean;
    }
    for (std::size_t i = 0; i < strategy.rejected_methods.size(); ++i) {
        const auto& rejected = strategy.rejected_methods[i];
        out << "\nalr wx rejected[" << i << "]=" << rejected.name << " reason=" << rejected.reason;
    }
    if (!strategy.error.empty()) {
        out << "\nalr wx error=" << strategy.error;
    }
    return out.str();
}

}  // namespace

const char* guest_load_method_name(GuestLoadMethod method) {
    switch (method) {
        case GuestLoadMethod::DirectRootfsExecve:
            return "direct-rootfs-execve";
        case GuestLoadMethod::MemfdExecveat:
            return "memfd-execveat";
        case GuestLoadMethod::AnonMmapLoader:
            return "anon-mmap-loader";
        case GuestLoadMethod::ProotBaseline:
            return "proot-baseline";
    }
    return "proot-baseline";
}

WxSafeExecStrategy build_wx_safe_exec_strategy(
    const RuntimeConfig& config,
    const ExecutableResolution& resolution) {
    WxSafeExecStrategy strategy;
    try {
        strategy.entrypoint = env_value(config, "ALR_TRAMPOLINE_PATH");
        strategy.target_host_path = resolution.translation.host_path;
        // The entrypoint must be the read-only packaged trampoline, never a file
        // inside the writable rootfs.
        strategy.entrypoint_is_packaged =
            !strategy.entrypoint.empty() && !path_is_inside(strategy.entrypoint, config.rootfs_dir);
        strategy.selinux_domain = current_selinux_domain();
        strategy.app_data_exec_allowed = domain_allows_app_data_exec(strategy.selinux_domain);
        strategy.ranked_methods = ranked_guest_load_methods(strategy.app_data_exec_allowed);
        strategy.rejected_methods =
            rejected_exec_methods(strategy.target_host_path, strategy.app_data_exec_allowed);
        // A fact now, not a constant. When the domain permits direct exec there
        // is nothing to reject, and saying otherwise was the bug.
        strategy.rejects_direct_rootfs_exec = !strategy.app_data_exec_allowed;
        strategy.primary = strategy.ranked_methods.empty()
            ? GuestLoadMethod::ProotBaseline
            : strategy.ranked_methods.front().method;
        for (const auto& method : strategy.ranked_methods) {
            if (method.method == GuestLoadMethod::ProotBaseline) {
                strategy.proot_baseline_available = true;
            }
        }
        // A viable strategy needs a packaged entrypoint, a native-exec primary
        // (PRoot is only a fallback), and a guaranteed PRoot baseline behind it.
        // Viability no longer requires rejecting direct exec -- in a domain
        // that allows it, direct exec IS the plan, and it is the only method
        // measured to work on the reference device. The packaged-entrypoint
        // requirement also only applies when we must avoid exec'ing from the
        // rootfs at all.
        strategy.planned = !strategy.ranked_methods.empty() &&
            strategy.primary != GuestLoadMethod::ProotBaseline &&
            strategy.proot_baseline_available &&
            (strategy.app_data_exec_allowed ||
             (strategy.entrypoint_is_packaged &&
              strategy.rejects_direct_rootfs_exec &&
              file_backed_mmap_rejected(strategy.rejected_methods)));
        if (!strategy.planned && strategy.error.empty()) {
            if (!strategy.entrypoint_is_packaged) {
                strategy.error = "entrypoint is missing or inside the writable rootfs";
            } else if (strategy.primary == GuestLoadMethod::ProotBaseline) {
                strategy.error = "no native-exec primary method is available";
            } else if (!strategy.proot_baseline_available) {
                strategy.error = "proot baseline fallback is unavailable";
            }
        }
    } catch (const std::exception& exc) {
        strategy.error = exc.what();
    }
    strategy.report = build_report(strategy);
    return strategy;
}

}  // namespace alr::runtime
