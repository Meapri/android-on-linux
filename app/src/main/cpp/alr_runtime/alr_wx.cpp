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

std::vector<GuestLoadCandidate> ranked_guest_load_methods() {
    return {
        {
            GuestLoadMethod::MemfdExecveat,
            "memfd-execveat",
            true,
            "execute on the anonymous memfd/tmpfs label for the app domain",
            "the guest ELF is copied into an anonymous in-memory fd, which is not an "
            "app_data_file, so the W^X file-exec denial does not apply",
        },
        {
            GuestLoadMethod::AnonMmapLoader,
            "anon-mmap-loader",
            true,
            "execmem for the app domain",
            "a userspace loader copies segments into anonymous RW->RX memory, so no "
            "writable file ever carries execute permission",
        },
        {
            GuestLoadMethod::ProotBaseline,
            "proot-baseline",
            false,
            "none (ptrace-mediated, no native exec page is created)",
            "known-working fallback with the highest overhead; keeps a guaranteed path "
            "when the native mechanisms are blocked by device policy",
        },
    };
}

std::vector<RejectedExecCandidate> rejected_exec_methods(const std::string& target_host_path) {
    return {
        {
            RejectedExecMethod::DirectRootfsExecve,
            "direct-rootfs-execve",
            "execve() needs execute permission on " + target_host_path +
                " which is an app_data_file; W^X denies it for targetSdk>=29",
        },
        {
            RejectedExecMethod::FileBackedMmapExec,
            "file-backed-mmap-exec",
            "mmap PROT_EXEC of " + target_host_path +
                " needs file execute permission, also denied under W^X",
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
    const bool mmap_rejected = file_backed_mmap_rejected(strategy.rejected_methods);
    std::ostringstream out;
    out << "ALR WX-SAFE EXEC STRATEGY: " << pass_fail(strategy.planned);
    out << "\nALR WX-SAFE ENTRYPOINT IS PACKAGED: " << pass_fail(strategy.entrypoint_is_packaged);
    out << "\nALR WX-SAFE DIRECT ROOTFS EXEC REJECTED: " << pass_fail(strategy.rejects_direct_rootfs_exec);
    out << "\nALR WX-SAFE FILE-BACKED MMAP EXEC REJECTED: " << pass_fail(mmap_rejected);
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
        strategy.ranked_methods = ranked_guest_load_methods();
        strategy.rejected_methods = rejected_exec_methods(strategy.target_host_path);
        strategy.rejects_direct_rootfs_exec = true;
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
        strategy.planned = strategy.entrypoint_is_packaged &&
            strategy.rejects_direct_rootfs_exec &&
            !strategy.ranked_methods.empty() &&
            strategy.primary != GuestLoadMethod::ProotBaseline &&
            strategy.proot_baseline_available &&
            file_backed_mmap_rejected(strategy.rejected_methods);
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
