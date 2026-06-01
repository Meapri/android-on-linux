#pragma once

#include <string>
#include <vector>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"

namespace alr::runtime {

// How ALR will get the guest program running without violating Android's W^X
// rule: on targetSdk >= 29 the OS denies execute permission on writable
// app-data files, so a rootfs binary can never be the host exec target. This is
// a clean-room strategy plan. It picks the read-only packaged entrypoint, ranks
// the W^X-safe guest-load mechanisms, and explicitly rejects the W^X-violating
// shortcuts. It does NOT exec anything; the chosen mechanisms still require
// on-device SELinux verification and must not be reported as device-proven.

enum class GuestLoadMethod {
    MemfdExecveat,    // copy guest ELF into an anonymous memfd, execveat(AT_EMPTY_PATH)
    AnonMmapLoader,   // userspace loader copies segments into anonymous PROT_EXEC memory
    ProotBaseline,    // ptrace-based PRoot fallback (no native exec page)
};

enum class RejectedExecMethod {
    DirectRootfsExecve,   // execve() of a writable rootfs file
    FileBackedMmapExec,   // mmap PROT_EXEC of a writable rootfs file
};

struct GuestLoadCandidate {
    GuestLoadMethod method;
    std::string name;
    bool requires_device_selinux_check = true;
    std::string selinux_lean;   // the SELinux permission this mechanism leans on
    std::string rationale;      // why it stays W^X-safe
};

struct RejectedExecCandidate {
    RejectedExecMethod method;
    std::string name;
    std::string reason;         // why it violates W^X
};

struct WxSafeExecStrategy {
    std::string entrypoint;                 // packaged trampoline host path (read-only APK lib area)
    std::string target_host_path;           // writable rootfs guest binary (diagnostic; never an exec target)
    bool entrypoint_is_packaged = false;    // entrypoint is not inside the writable rootfs
    bool rejects_direct_rootfs_exec = false;
    std::vector<GuestLoadCandidate> ranked_methods;
    std::vector<RejectedExecCandidate> rejected_methods;
    GuestLoadMethod primary = GuestLoadMethod::ProotBaseline;
    bool proot_baseline_available = false;
    bool planned = false;
    std::string error;
    std::string report;
};

const char* guest_load_method_name(GuestLoadMethod method);

// Build the W^X-safe exec strategy ALR will follow for a resolved guest binary.
// Clean-room planning only.
WxSafeExecStrategy build_wx_safe_exec_strategy(
    const RuntimeConfig& config,
    const ExecutableResolution& resolution);

}  // namespace alr::runtime
