#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "alr_runtime/alr_config.hpp"

namespace alr::runtime {

// Clean-room planning for the procfs views a guest process must observe when it
// runs under ALR. None of this reads the host /proc; it synthesizes the
// guest-namespace truth and records which syscalls a future interposer hooks.
// This is plan-only: it does not virtualize procfs on a live device yet, and it
// must never be reported as device-proven acceleration/identity.

// What the guest's /proc/self/exe (and /proc/<pid>/exe) must resolve to. On
// Android the real symlink points at the packaged trampoline/loader host path;
// the guest must instead see its own guest-namespace executable path.
struct ProcfsSelfExeView {
    std::string guest_view;                       // path the guest symlink must resolve to
    std::string host_truth;                       // what Android procfs actually returns (diagnostic)
    bool requires_virtualization = false;         // host_truth differs from guest_view
    std::vector<std::string> interpose_paths;     // /proc/self/exe, /proc/<pid>/exe
    std::vector<std::string> interpose_syscalls;  // readlink, readlinkat, openat
    bool valid = false;
};

// Fakeroot identity the guest must observe in /proc/self/status. Under fakeroot
// the guest sees uid/gid 0 to match the PRoot `-r` baseline identity; without
// fakeroot it observes the real process identity.
struct ProcfsSelfStatusView {
    std::string name;                 // guest comm (executable basename)
    long uid = -1;                    // guest-visible effective uid (0 under fakeroot)
    long gid = -1;                    // guest-visible effective gid (0 under fakeroot)
    bool fakeroot = false;
    std::vector<std::string> lines;   // synthesized Name/Uid/Gid status lines
    bool valid = false;
};

struct ProcfsMountEntry {
    std::string source;
    std::string target;
    std::string fstype;
    std::string options;
    std::string host_backing;  // diagnostic only; never rendered into the guest view
};

// Synthesized guest /proc/mounts table. It presents the rootfs as `/` plus the
// pseudo-filesystems a guest expects, and must not leak Android host mount
// sources (the real rootfs host path, bind host paths, /data, /system, ...).
struct ProcfsMountsView {
    std::vector<ProcfsMountEntry> entries;
    std::vector<std::string> leaked_host_tokens;  // android host paths that escaped into the guest view
    bool has_guest_root = false;
    bool has_proc = false;
    bool valid = false;
};

// Host-side facts a caller can pin for deterministic planning. When unset the
// planner falls back to ALR_TRAMPOLINE_PATH for the self-exe truth and to the
// live getuid()/getgid() for the non-fakeroot identity.
struct ProcfsHostFacts {
    std::string self_exe_truth;   // real /proc/self/exe target (packaged trampoline/loader)
    bool has_identity = false;
    long uid = 0;
    long gid = 0;
};

struct ProcfsVirtualizationPlan {
    ProcfsSelfExeView self_exe;
    ProcfsSelfStatusView self_status;
    ProcfsMountsView mounts;
    bool planned = false;
    std::string error;
    std::string report;
};

// Render the guest-visible /proc/mounts text for a mounts view (one line per
// entry, host backing paths excluded). Exposed for interposer reuse and tests.
std::string render_guest_mounts(const ProcfsMountsView& mounts);

// Build the deterministic procfs virtualization plan ALR will hand to a future
// /proc interposer. Clean-room planning only; it does not exec the guest or
// mutate the host procfs.
ProcfsVirtualizationPlan build_procfs_virtualization_plan(
    const RuntimeConfig& config,
    std::string_view requested_program,
    const ProcfsHostFacts& host_facts = {});

}  // namespace alr::runtime
