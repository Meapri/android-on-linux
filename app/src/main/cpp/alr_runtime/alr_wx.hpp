#pragma once

#include <string>
#include <vector>

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"

namespace alr::runtime {

// How ALR gets the guest program running under Android's exec rules.
//
// THE RULE IS PER-DOMAIN, NOT UNIVERSAL. targetSdk selects the SELinux domain,
// and only the legacy ones are granted app_data_file:file execute_no_trans --
// from AOSP system/sepolicy/private/untrusted_app_27.te:
//     allow untrusted_app_27 app_data_file:file execute_no_trans;
// with no such rule for untrusted_app_29/30/32 or untrusted_app, enforced by a
// neverallow in private/app_neverallows.te that names untrusted_app_25,
// untrusted_app_27 and runas_app as the only exemptions.
//
// This remains a clean-room strategy plan: it picks the entrypoint, ranks the
// guest-load mechanisms for the domain we are in, and names what is ruled out
// and why. It does NOT exec anything.
//
// This app targets 28, so it runs as untrusted_app_27 and MAY execve a rootfs
// binary. MEASURED on SM-X236N / Android 16: ALR DIRECT APP-DATA EXECVE: PASS,
// and a stock Ubuntu 24.04 glibc userland boots and networks through it.
//
// The earlier version of this file asserted the opposite unconditionally --
// "W^X denies it for targetSdk>=29" with direct-rootfs-execve hardcoded into
// the rejected list -- which steered the runtime away from the one mechanism
// that works here and toward memfd-execveat, which the same device DENIES
// (EACCES). The ranking is now derived from the domain we are actually in.

enum class GuestLoadMethod {
    DirectRootfsExecve, // plain execve() of the rootfs binary; needs execute_no_trans
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
    // Whether this process's SELinux domain grants app_data_file execute.
    // Read from /proc/self/attr/current, not assumed from a version number.
    bool app_data_exec_allowed = false;
    std::string selinux_domain;
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
