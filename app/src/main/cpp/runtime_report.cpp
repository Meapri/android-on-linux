#include <jni.h>

#include <android/native_window_jni.h>
#include <android/log.h>   // parent-side loader probe diagnostics (alr_loader tag)

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef NT_ARM_SYSTEM_CALL
// Writing -1 to this regset at a syscall-entry/seccomp stop CANCELS the pending
// syscall (the kernel skips it, returns -ENOSYS). ADR-003-v3 uses this to neuter a
// guest execve so we can PC-redirect the tracee into an in-process re-map instead of
// a (W^X-forbidden) kernel execve.
#define NT_ARM_SYSTEM_CALL 0x404
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032  // (1027 is R_AARCH64_RELATIVE; IRELATIVE is 1032)
#endif

#include <chrono>
#include <atomic>
#include <thread>
#include <dirent.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>   // ::getenv for the ALR_DISABLE_INTERPOSE A/B gate
#include <cstring>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>  // per-tid /proc/<tid>/mem fd cache + translate result cache
#include <unordered_set>  // known-tid set: new-clone-child initial-stop vs group-stop
#include <mutex>
#include <sys/resource.h>  // M-R2 (ADR-002): getrusage(RUSAGE_CHILDREN) guest CPU/ctxt

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"
#include "alr_runtime/alr_interposer.hpp"
#include "alr_runtime/alr_path.hpp"
#include "alr_runtime/alr_perf.hpp"
#include "alr_runtime/alr_procfs.hpp"
#include "alr_runtime/alr_wx.hpp"
#include "alr_inproc_reexec.h"
#include "runtime_plan.hpp"

#ifdef ALR_HAVE_WAYLAND
#include "alr_wayland/alr_compositor.hpp"
#endif

// GPU-native app track (Phase 4): host-side GLES command-stream decoder + probes.
// Header-only and self-contained (owns its own EGL pbuffer context), so it adds no
// link deps beyond EGL/GLESv2 (already linked). alr_gpu_fbo.hpp adds the M4
// AHB-backed render-target probe (guest draws -> AHB -> zero-copy sample).
#include "alr_gpu/alr_gpu_probe.hpp"
#include "alr_gpu/alr_gpu_fbo.hpp"
// alr_gpu_host_service.hpp adds the M4 LIVE-INTEGRATION backbone: a host executor
// thread owning the Mali GLES2 ctx + AHB-FBO, fed by a guest over the SPSC ring,
// presenting per frame (req_seq/reply_seq handshake). run_live_integration_probe()
// is the in-process two-thread device self-test of that whole loop.
#include "alr_gpu/alr_gpu_host_service.hpp"
// alr_gpu_screen.hpp: STEP B-1 on-screen present — the executor renders the cube
// into an AHB, then presents it to an ANativeWindow via external-OES (a SECOND EGL
// context, dodging the v117 same-context black-AHB hazard). run_screen_cube_demo()
// streams a spinning textured cube to the SurfaceView (in-process, no fork/rootfs).
#include "alr_gpu/alr_gpu_screen.hpp"
#include "alr_gpu/alr_gpu_ring_hook.hpp"  // WS-1↔WS-2 CP-0 §5-A: GPU ring hook (GLES guest → Mali)
// §VK-M2 device path: real vendor Mali libvulkan behind the guest-Vulkan marshalling.
// ALR_VK_DECODE_REAL pulls <vulkan/vulkan.h> (NDK) + selects run_vk_marshal_mali_probe().
#define ALR_VK_DECODE_REAL 1
#include "alr_gpu/alr_gpu_vk_marshal_probe.hpp"
// alr_jit_probe.hpp: V8-style iterative W^X (RW<->RX) executable-memory cycle probe —
// decides whether Chromium (V8/SwiftShader JIT) can run WITHOUT --jitless on this
// untrusted_app domain. Pure anonymous mmap/mprotect; no memfd-exec (that's EACCES).
#include "alr_jit/alr_jit_probe.hpp"

namespace {

std::string jstring_to_string(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars == nullptr ? std::string{} : std::string{chars};
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(value, chars);
    }
    return result;
}

std::string join_path(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

std::string errno_message(const char* action) {
    std::ostringstream out;
    out << action << " failed errno=" << errno << " message=" << std::strerror(errno);
    return out.str();
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
        } else if (errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error(errno_message("read pipe"));
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

std::vector<char*> mutable_c_string_vector(std::vector<std::string>& values) {
    std::vector<char*> out;
    out.reserve(values.size() + 1);
    for (auto& value : values) {
        out.push_back(value.data());
    }
    out.push_back(nullptr);
    return out;
}

alr::runtime::RuntimeConfig alr_runtime_config_from_input(
    const alr::RuntimeReportInput& input,
    const alr::LoaderLaunchPlan& launch) {
    return alr::runtime::RuntimeConfig{
        .package_name = input.package_name,
        .rootfs_dir = launch.env.at("ALR_ROOTFS"),
        .cwd = "/",
        .program = input.program,
        .env = launch.env,
        .binds = {},
        .hook_path = launch.env.at("ALR_HOOK_PATH"),
        .interposer_path = launch.env.at("ALR_INTERPOSER_PATH"),
        .bridge_path = launch.env.at("ALR_BRIDGE_PATH"),
        .fake_root = launch.env.at("ALR_FAKE_ROOT") == "1",
        .verbose = launch.env.at("ALR_VERBOSE") == "0" ? 0 : 1,
        .trace_path = launch.env.at("ALR_TRACE_PATH") == "1",
        .trace_exec = launch.env.at("ALR_TRACE_EXEC") == "1",
    };
}

std::string run_packaged_trampoline_continue_probe(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR PACKAGED TRAMPOLINE CONTINUE PROBE: android-native-exec";
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr_runtime_config_from_input(input, launch);
    const auto continuation = alr::runtime::build_exec_continuation_plan(
        config,
        input.program,
        {"--android-dry-run"},
        alr::runtime::ExecContinuationKind::Execve);
    out << "\n" << continuation.report;
    if (!continuation.planned) {
        out << "\nALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: FAIL";
        out << "\nalr packaged trampoline continue reason=continuation-plan-not-ready";
        return out.str();
    }

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (::pipe(stdout_pipe) != 0) {
        out << "\nALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: FAIL";
        out << "\nalr packaged trampoline continue error=" << errno_message("pipe stdout");
        return out.str();
    }
    if (::pipe(stderr_pipe) != 0) {
        const int saved_errno = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        errno = saved_errno;
        out << "\nALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: FAIL";
        out << "\nalr packaged trampoline continue error=" << errno_message("pipe stderr");
        return out.str();
    }

    auto argv_storage = continuation.argv;
    auto argv = mutable_c_string_vector(argv_storage);
    auto env_storage = continuation.env_overrides;
    auto envp = mutable_c_string_vector(env_storage);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        ::execve(argv_storage.front().c_str(), argv.data(), envp.data());
        _exit(127);
    }
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        errno = saved_errno;
        out << "\nALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: FAIL";
        out << "\nalr packaged trampoline continue error=" << errno_message("fork");
        return out.str();
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    std::string stdout_text;
    std::string stderr_text;
    try {
        stdout_text = read_all_from_fd(stdout_pipe[0]);
        stderr_text = read_all_from_fd(stderr_pipe[0]);
    } catch (const std::exception& exc) {
        stderr_text = exc.what();
    }
    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            out << "\nALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: FAIL";
            out << "\nalr packaged trampoline continue error=" << errno_message("waitpid");
            return out.str();
        }
    }
    int exit_code = -1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }
    const bool passed = exit_code == 0 &&
        stdout_text.find("ALR TRAMPOLINE CONTINUE EXEC: PASS") != std::string::npos &&
        stdout_text.find("ALR TRAMPOLINE CONTINUE CHECKSUM: PASS") != std::string::npos;
    out << "\nALR PACKAGED TRAMPOLINE CONTINUE EXECUTION: " << (passed ? "PASS" : "FAIL");
    out << "\nalr packaged trampoline continue exit=" << exit_code;
    out << "\nalr packaged trampoline continue path=" << argv_storage.front();
    out << "\nalr packaged trampoline continue stdout=" << stdout_text;
    out << "\nalr packaged trampoline continue stderr=" << stderr_text;
    return out.str();
}

std::string build_procfs_virtualization_report(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR PROCFS VIRTUALIZATION PROBE: android-native-plan";
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr_runtime_config_from_input(input, launch);
    // The packaged trampoline host path is what Android's real /proc/self/exe
    // would resolve to once it becomes the W^X-safe entrypoint, so the guest view
    // must virtualize away from it. Live identity is captured directly (own uid),
    // never scraped from the host procfs.
    alr::runtime::ProcfsHostFacts host_facts;
    host_facts.self_exe_truth = launch.env.at("ALR_TRAMPOLINE_PATH");
    host_facts.has_identity = true;
    host_facts.uid = static_cast<long>(::getuid());
    host_facts.gid = static_cast<long>(::getgid());
    const auto plan = alr::runtime::build_procfs_virtualization_plan(config, input.program, host_facts);
    out << "\n" << plan.report;
    return out.str();
}

std::string build_wx_safe_exec_report(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR WX-SAFE EXEC PROBE: android-native-plan";
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr_runtime_config_from_input(input, launch);
    const auto resolution = alr::runtime::resolve_guest_executable(config, input.program);
    const auto strategy = alr::runtime::build_wx_safe_exec_strategy(config, resolution);
    out << "\n" << strategy.report;
    return out.str();
}

std::string build_perf_comparison_report(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR PERF PROBE: android-native-measure";
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr_runtime_config_from_input(input, launch);
    const auto comparison = alr::runtime::run_perf_comparison(config, 100000);
    out << "\n" << comparison.report;
    // WS-1 M2: surface the CPU-overhead microbench in logcat (path-xlate vs syscall-
    // roundtrip ns) so the near-zero-overhead claim is device-measurable, not just on
    // the report screen. Pairs with the per-guest traps/rewrites counters (=0 for the
    // in-process interposer path) to quantify ALR's CPU mediation cost.
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "perf-overhead:\n%s",
                        comparison.report.c_str());
    return out.str();
}

std::string build_interposer_procfs_report(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR INTERPOSE PROBE: android-native-mechanism";
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr_runtime_config_from_input(input, launch);
    const auto self_exe = alr::runtime::resolve_interposed_access(config, input.program, "/proc/self/exe");
    const auto mounts = alr::runtime::resolve_interposed_access(config, input.program, "/proc/mounts");

    const bool self_exe_ok = self_exe.virtualized && self_exe.host_path.empty() &&
        !self_exe.synthetic_link_target.empty();
    const bool self_exe_no_host = self_exe.host_path.empty();
    const bool mounts_ok = mounts.virtualized && !mounts.synthetic_content.empty();
    const bool mounts_no_leak = config.rootfs_dir.empty() ||
        mounts.synthetic_content.find(config.rootfs_dir) == std::string::npos;

    out << "\nALR INTERPOSE SELF EXE VIRTUALIZED: " << (self_exe_ok ? "PASS" : "FAIL");
    out << "\nALR INTERPOSE MOUNTS VIRTUALIZED: " << (mounts_ok ? "PASS" : "FAIL");
    out << "\nALR INTERPOSE SELF EXE NO HOST PROC: " << (self_exe_no_host ? "PASS" : "FAIL");
    out << "\nALR INTERPOSE MOUNTS NO HOST LEAK: " << (mounts_no_leak ? "PASS" : "FAIL");
    out << "\nalr interpose self exe target=" << self_exe.synthetic_link_target;
    out << "\nalr interpose mounts bytes=" << mounts.synthetic_content.size();
    out << "\n--- self exe ---\n" << self_exe.report;
    out << "\n--- mounts ---\n" << mounts.report;
    return out.str();
}

// Directly characterize which syscalls the Android app sandbox (untrusted_app)
// permits. The app seccomp filter returns ENOSYS for non-allowlisted syscalls,
// so each probe invokes the syscall with harmless/invalid arguments: ENOSYS
// means seccomp-blocked, any other errno means the syscall is allowed (it just
// failed on the bogus arguments). This pins the root cause of guest failures
// that proot inherits into Debian userland.
std::string build_syscall_capability_probe() {
    constexpr int kAtEmptyPath = 0x1000;
    struct Probe {
        const char* name;
        long result;
        int saved_errno;
    };
    std::vector<Probe> probes;
    auto run = [&](const char* name, long result) {
        probes.push_back({name, result, errno});
    };

    // arm64 is a "generic" syscall ABI: the legacy non-*at calls (symlink, link,
    // mknod) do not exist, so they are probed only where the kernel defines them.
    errno = 0; run("execveat", ::syscall(__NR_execveat, -1, "", nullptr, nullptr, kAtEmptyPath));
#ifdef __NR_symlink
    errno = 0; run("symlink", ::syscall(__NR_symlink, "t", ""));
#endif
    errno = 0; run("symlinkat", ::syscall(__NR_symlinkat, "t", -1, ""));
#ifdef __NR_link
    errno = 0; run("link", ::syscall(__NR_link, "", ""));
#endif
    errno = 0; run("linkat", ::syscall(__NR_linkat, -1, "", -1, "", 0));
#ifdef __NR_mknod
    errno = 0; run("mknod", ::syscall(__NR_mknod, "", 0, 0));
#endif
    errno = 0; run("mknodat", ::syscall(__NR_mknodat, -1, "", 0, 0));
    errno = 0; run("statx", ::syscall(__NR_statx, -1, "", 0, 0, nullptr));
#ifdef __NR_clone3
    errno = 0; run("clone3", ::syscall(__NR_clone3, nullptr, 0));
#endif
    errno = 0; run("unshare", ::syscall(__NR_unshare, 0));

    std::ostringstream out;
    out << "ALR SYSCALL SANDBOX PROBE: PASS";
    int blocked = 0;
    for (const auto& p : probes) {
        const bool enosys = p.result < 0 && p.saved_errno == ENOSYS;
        if (enosys) {
            ++blocked;
        }
        out << "\nalr syscall " << p.name << "=" << (enosys ? "BLOCKED_ENOSYS" : "ALLOWED")
            << " errno=" << (p.result < 0 ? p.saved_errno : 0);
    }
    out << "\nalr syscall blocked count=" << blocked;
    return out.str();
}

// KEYSTONE probe for low-overhead rootfs path mediation (the blocker for running
// real programs like dash/ls under the in-process ALR loader): can an
// untrusted_app STACK a second seccomp filter that SECCOMP_RET_TRACEs only the
// path-taking syscalls, so the parent supervisor can rewrite a guest path
// (/etc/foo) to its rootfs location BEFORE the kernel dereferences it — without
// PRoot's trap-every-syscall overhead?
//
// This validates the whole mechanism end to end on real hardware:
//   1. the child installs a 2nd seccomp filter (seccomp(2) or prctl fallback) —
//      tests whether the zygote filter lets us add one at all (NO_NEW_PRIVS is
//      already set, so the kernel privilege gate should be satisfied);
//   2. the filter returns SECCOMP_RET_TRACE for openat -> PTRACE_EVENT_SECCOMP;
//   3. the child openat()s a DECOY path that does NOT exist;
//   4. the parent catches the seccomp-stop, reads the path from /proc/<tid>/mem,
//      confirms it is the decoy, and rewrites it IN PLACE to a REAL file (shorter
//      string, so it fits) that exists and holds a known marker;
//   5. PTRACE_CONT runs the syscall ONCE against the rewritten path (RET_TRACE
//      does not re-enter the filter), so the child's open SUCCEEDS and reads the
//      marker -> proof the rewrite took effect.
// A clean negative (filter install blocked) is equally decisive: it would mean
// seccomp-selective path mediation is not viable here and we'd fall back to
// ptrace-all. Done in a forked child so any failure can't take down the app.
std::string build_seccomp_pathtrap_probe(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR SECCOMP PATH-TRAP PROBE: PASS";

    const std::string base_dir = !input.app_files_dir.empty() ? input.app_files_dir
                                 : (!input.app_cache_dir.empty() ? input.app_cache_dir : "/data/local/tmp");
    const std::string real_path = base_dir + "/alr-sc-real.txt";
    const std::string decoy_path = base_dir + "/alr-sc-decoy-DOES-NOT-EXIST.txt";
    const char* kMarker = "ALR_REWRITE_OK";

    // Stage the real target file with a known marker. The decoy is never created.
    {
        const int fd = ::open(real_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            ::write(fd, kMarker, ::strlen(kMarker));
            ::close(fd);
        }
    }
    ::unlink(decoy_path.c_str());  // ensure the decoy truly does not exist

    // The in-place rewrite requires the real path be no longer than the decoy.
    const bool fits = real_path.size() <= decoy_path.size();
    out << "\nalr sc real_path=" << real_path;
    out << "\nalr sc decoy_path=" << decoy_path;
    out << "\nalr sc rewrite_fits=" << (fits ? "yes" : "no");

    int diag_pipe[2] = {-1, -1};
    if (::pipe(diag_pipe) != 0) {
        out << "\nalr sc result=PROBE_ERROR pipe";
        return out.str();
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(diag_pipe[0]);
        const int dg = diag_pipe[1];

        // Sync: let the parent attach and set PTRACE_O_TRACESECCOMP before we
        // install the filter and issue the traced openat.
        ::ptrace(PTRACE_TRACEME, 0, 0, 0);
        ::syscall(__NR_kill, ::getpid(), SIGSTOP);

        // Install a 2nd seccomp filter: TRACE openat, ALLOW everything else.
        // NO_NEW_PRIVS is already set by zygote; set it again (harmless) so the
        // privilege gate is satisfied even if that assumption is ever wrong.
        ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        struct sock_filter filter[] = {
            // Reject foreign arches (don't trap a compat syscall by number).
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 0, 3),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = {
            static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
            filter,
        };
        long fr = ::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0u, &prog);
        const int seccomp_errno = errno;
        const char* how = "seccomp";
        if (fr != 0) {
            // Fall back to the older prctl interface.
            errno = 0;
            fr = ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
            how = "prctl";
        }
        if (fr != 0) {
            ::write(dg, "FILTER_FAIL how=", 16);
            ::write(dg, how, ::strlen(how));
            char eb[32];
            int n = ::snprintf(eb, sizeof(eb), " errno=%d/%d;", seccomp_errno, errno);
            if (n > 0) ::write(dg, eb, static_cast<size_t>(n));
            ::_exit(20);
        }
        ::write(dg, "FILTER_OK how=", 14);
        ::write(dg, how, ::strlen(how));
        ::write(dg, ";", 1);

        // The traced syscall: open the DECOY (which does not exist). Use the raw
        // syscall so no libc wrapper does an unrelated openat first.
        const long rc = ::syscall(__NR_openat, AT_FDCWD, decoy_path.c_str(), O_RDONLY, 0);
        if (rc < 0) {
            ::write(dg, "OPEN_FAIL errno=", 16);
            char eb[16];
            int n = ::snprintf(eb, sizeof(eb), "%d;", errno);
            if (n > 0) ::write(dg, eb, static_cast<size_t>(n));
            ::_exit(0);
        }
        char buf[64] = {0};
        const long rd = ::read(static_cast<int>(rc), buf, sizeof(buf) - 1);
        ::write(dg, "OPEN_OK content=", 16);
        if (rd > 0) ::write(dg, buf, static_cast<size_t>(rd));
        ::write(dg, ";", 1);
        ::syscall(__NR_close, rc);
        ::_exit(0);
    }

    if (pid < 0) {
        ::close(diag_pipe[0]);
        ::close(diag_pipe[1]);
        out << "\nalr sc result=PROBE_ERROR fork";
        return out.str();
    }
    ::close(diag_pipe[1]);

    int mem_fd_rw = -1;
    int mem_fd_ro = -1;
    int traps = 0;
    int rewrites = 0;
    bool options_set = false;
    std::string observed_path;
    int child_code = -1;
    std::string dbg;  // first-trap pinpoint diagnostics

    while (true) {
        int status = 0;
        const pid_t w = ::waitpid(pid, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (WIFEXITED(status)) {
            child_code = WEXITSTATUS(status);
            break;
        }
        if (WIFSIGNALED(status)) {
            break;
        }
        if (!WIFSTOPPED(status)) {
            continue;
        }
        const int stopsig = WSTOPSIG(status);
        const int event = status >> 16;
        if (event == PTRACE_EVENT_SECCOMP) {
            ++traps;
            uint64_t regs[34] = {0};
            struct iovec io{regs, sizeof(regs)};
            const bool got_regs =
                ::ptrace(PTRACE_GETREGSET, w, reinterpret_cast<void*>(NT_PRSTATUS), &io) == 0;
            const uintptr_t path_addr = static_cast<uintptr_t>(regs[1]);
            if (got_regs && regs[8] == static_cast<uint64_t>(__NR_openat)) {
                // Open mem handles lazily: O_RDWR for the rewrite, O_RDONLY as a
                // read fallback if write is denied by SELinux.
                if (mem_fd_rw < 0 && mem_fd_ro < 0) {
                    const std::string mp = "/proc/" + std::to_string(pid) + "/mem";
                    mem_fd_rw = ::open(mp.c_str(), O_RDWR);
                    const int rwerr = errno;
                    mem_fd_ro = ::open(mp.c_str(), O_RDONLY);
                    const int roerr = errno;
                    char b[96];
                    int n = ::snprintf(b, sizeof(b), "mem_rw=%s/%d mem_ro=%s/%d ",
                                       mem_fd_rw >= 0 ? "ok" : "fail", rwerr,
                                       mem_fd_ro >= 0 ? "ok" : "fail", roerr);
                    if (n > 0) dbg.append(b, static_cast<size_t>(n));
                }
                char gbuf[256] = {0};
                ssize_t got = -1;
                int rd_fd = mem_fd_ro >= 0 ? mem_fd_ro : mem_fd_rw;
                if (rd_fd >= 0) {
                    got = ::pread(rd_fd, gbuf, sizeof(gbuf) - 1, static_cast<off_t>(path_addr));
                }
                if (got <= 0) {
                    // /proc/<pid>/mem read failed — fall back to process_vm_readv.
                    struct iovec liov{gbuf, sizeof(gbuf) - 1};
                    struct iovec riov{reinterpret_cast<void*>(path_addr), sizeof(gbuf) - 1};
                    got = ::process_vm_readv(pid, &liov, 1, &riov, 1, 0);
                }
                if (dbg.find("pread=") == std::string::npos) {
                    char b[64];
                    int n = ::snprintf(b, sizeof(b), "rd=%zd/%d ", got, errno);
                    if (n > 0) dbg.append(b, static_cast<size_t>(n));
                }
                if (got > 0) {
                    gbuf[got] = '\0';
                    if (observed_path.empty()) observed_path = gbuf;
                    // Rewrite the decoy to the real path, in place, before the
                    // kernel dereferences it.
                    if (std::string(gbuf) == decoy_path && fits) {
                        const size_t plen = real_path.size() + 1;
                        ssize_t wr = -1;
                        if (mem_fd_rw >= 0) {
                            wr = ::pwrite(mem_fd_rw, real_path.c_str(), plen,
                                          static_cast<off_t>(path_addr));
                        }
                        if (wr != static_cast<ssize_t>(plen)) {
                            struct iovec liov{const_cast<char*>(real_path.c_str()), plen};
                            struct iovec riov{reinterpret_cast<void*>(path_addr), plen};
                            wr = ::process_vm_writev(pid, &liov, 1, &riov, 1, 0);
                        }
                        if (dbg.find("wr=") == std::string::npos) {
                            char b[48];
                            int n = ::snprintf(b, sizeof(b), "wr=%zd/%d ", wr, errno);
                            if (n > 0) dbg.append(b, static_cast<size_t>(n));
                        }
                        if (wr == static_cast<ssize_t>(plen)) ++rewrites;
                    }
                }
            } else if (dbg.empty()) {
                char b[64];
                int n = ::snprintf(b, sizeof(b), "got_regs=%d nr=%llu ", got_regs ? 1 : 0,
                                   static_cast<unsigned long long>(regs[8]));
                if (n > 0) dbg.append(b, static_cast<size_t>(n));
            }
            ::ptrace(PTRACE_CONT, w, nullptr, nullptr);
            continue;
        }
        if (stopsig == SIGSTOP || stopsig == SIGTRAP) {
            if (!options_set) {
                ::ptrace(PTRACE_SETOPTIONS, pid, nullptr,
                         reinterpret_cast<void*>(static_cast<long>(PTRACE_O_TRACESECCOMP)));
                options_set = true;
            }
            ::ptrace(PTRACE_CONT, w, nullptr, nullptr);
            continue;
        }
        ::ptrace(PTRACE_CONT, w, nullptr,
                 reinterpret_cast<void*>(static_cast<long>(stopsig)));
    }
    if (mem_fd_rw >= 0) ::close(mem_fd_rw);
    if (mem_fd_ro >= 0) ::close(mem_fd_ro);

    std::string diag;
    try {
        diag = read_all_from_fd(diag_pipe[0]);
    } catch (...) {
    }
    ::close(diag_pipe[0]);
    ::unlink(real_path.c_str());

    const bool filter_ok = diag.find("FILTER_OK") != std::string::npos;
    const bool open_ok = diag.find("OPEN_OK") != std::string::npos;
    const bool marker_seen = diag.find(kMarker) != std::string::npos;
    const bool trapped_decoy = observed_path == decoy_path;
    const bool mediated = filter_ok && traps >= 1 && rewrites >= 1 && open_ok && marker_seen;

    out << "\nalr sc filter_install=" << (filter_ok ? "OK" : "BLOCKED");
    out << "\nalr sc seccomp_trace_events=" << traps;
    out << "\nalr sc observed_guest_path=" << (observed_path.empty() ? "(none)" : observed_path);
    out << "\nalr sc trapped_decoy_match=" << (trapped_decoy ? "yes" : "no");
    out << "\nalr sc path_rewrites=" << rewrites;
    out << "\nalr sc mem_diag=" << (dbg.empty() ? "(none)" : dbg);
    out << "\nalr sc child_exit=" << child_code;
    out << "\nalr sc child_diag=" << diag;
    out << "\nalr sc PATH_MEDIATION_VIABLE=" << (mediated ? "yes" : "no");
    return out.str();
}

// Wayland transport viability probe (Phase 2/3): can the app host a NAMED AF_UNIX
// socket that a forked guest connects to by path, with a byte exchange? This is
// the exact compositor<->client topology — the host compositor binds/listens at
// <files>/alr-wl-test.sock (an app_data_file the untrusted_app domain owns), and
// the guest (a forked child, same UID/SELinux domain) connect()s to that absolute
// Android path and exchanges PING/PONG. If this works, a stock libwayland-client
// guest can reach an in-app compositor with NO socket-path mediation (just inject
// XDG_RUNTIME_DIR pointing at the host socket dir). Forked so a failure is contained.
std::string build_unix_socket_probe(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR UNIX SOCKET TRANSPORT PROBE: PASS";
    const std::string base_dir = !input.app_files_dir.empty() ? input.app_files_dir
                                 : (!input.app_cache_dir.empty() ? input.app_cache_dir : "/data/local/tmp");
    const std::string sock_path = base_dir + "/alr-wl-test.sock";
    out << "\nalr us socket_path=" << sock_path;
    ::unlink(sock_path.c_str());

    const int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) {
        out << "\nalr us result=FAIL socket errno=" << errno;
        return out.str();
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sock_path.size() + 1 > sizeof(addr.sun_path)) {
        ::close(srv);
        out << "\nalr us result=FAIL path_too_long";
        return out.str();
    }
    std::memcpy(addr.sun_path, sock_path.c_str(), sock_path.size() + 1);
    const socklen_t alen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                                                  sock_path.size() + 1);
    const bool bind_ok = ::bind(srv, reinterpret_cast<struct sockaddr*>(&addr), alen) == 0;
    const int bind_errno = errno;
    const bool listen_ok = bind_ok && ::listen(srv, 1) == 0;
    out << "\nalr us bind=" << (bind_ok ? "OK" : "FAIL") << " errno=" << (bind_ok ? 0 : bind_errno);
    out << "\nalr us listen=" << (listen_ok ? "OK" : "FAIL");
    if (!listen_ok) {
        ::close(srv);
        ::unlink(sock_path.c_str());
        out << "\nalr us WAYLAND_TRANSPORT_VIABLE=no";
        return out.str();
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
        // The "guest": connect to the host socket by absolute path and exchange.
        ::close(srv);
        const int c = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (c < 0) {
            ::_exit(21);
        }
        if (::connect(c, reinterpret_cast<struct sockaddr*>(&addr), alen) != 0) {
            ::_exit(22);
        }
        if (::write(c, "PING", 4) != 4) {
            ::_exit(23);
        }
        char rb[8] = {0};
        const ssize_t n = ::read(c, rb, sizeof(rb) - 1);
        ::close(c);
        if (n == 4 && std::memcmp(rb, "PONG", 4) == 0) {
            ::_exit(0);
        }
        ::_exit(24);
    }

    bool accept_ok = false;
    bool exchanged = false;
    const int cli = ::accept(srv, nullptr, nullptr);
    if (cli >= 0) {
        accept_ok = true;
        char rb[8] = {0};
        const ssize_t n = ::read(cli, rb, sizeof(rb) - 1);
        if (n == 4 && std::memcmp(rb, "PING", 4) == 0) {
            exchanged = ::write(cli, "PONG", 4) == 4;
        }
        ::close(cli);
    }
    int child_code = -1;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        child_code = WEXITSTATUS(status);
    }
    ::close(srv);
    ::unlink(sock_path.c_str());

    const bool viable = bind_ok && listen_ok && accept_ok && exchanged && child_code == 0;
    out << "\nalr us accept=" << (accept_ok ? "OK" : "FAIL");
    out << "\nalr us byte_exchange=" << (exchanged ? "OK" : "FAIL");
    out << "\nalr us guest_connect_exit=" << child_code;
    out << "\nalr us WAYLAND_TRANSPORT_VIABLE=" << (viable ? "yes" : "no");
    return out.str();
}

// Measure whether the app domain can create anonymous executable memory, the
// prerequisite for the `anon-mmap-loader` W^X-safe exec fallback. A child maps
// an anonymous RW page, writes an arm64 stub that returns 42, flips it to RX
// with mprotect, and calls it. SELinux may deny the mprotect (EACCES) or fault
// the call (signal). Done in a forked child so a kill does not take down the app.
std::string build_execmem_probe() {
    int diag_pipe[2] = {-1, -1};
    std::ostringstream out;
    out << "ALR EXECMEM PROBE: android-native-attempt";
    if (::pipe(diag_pipe) != 0) {
        out << "\nALR EXECMEM ANON RX EXECUTION: FAIL";
        out << "\nalr execmem error=pipe errno=" << errno;
        return out.str();
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
        const int w = diag_pipe[1];
        ::close(diag_pipe[0]);
        void* mem = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            ::dprintf(w, "MMAP_FAIL errno=%d", errno);
            _exit(50);
        }
        // arm64: movz w0,#42 ; ret  (little-endian instruction words)
        const uint32_t code[2] = {0x52800540u, 0xd65f03c0u};
        std::memcpy(mem, code, sizeof(code));
        __builtin___clear_cache(reinterpret_cast<char*>(mem),
                                reinterpret_cast<char*>(mem) + sizeof(code));
        if (::mprotect(mem, 4096, PROT_READ | PROT_EXEC) != 0) {
            ::dprintf(w, "MPROTECT_FAIL errno=%d", errno);
            _exit(51);
        }
        ::dprintf(w, "MPROTECT_OK calling;");
        int (*fn)() = nullptr;
        std::memcpy(&fn, &mem, sizeof(fn));
        const int r = fn();
        ::dprintf(w, "CALL_RET=%d", r);
        _exit(r == 42 ? 0 : 60);
    }
    if (pid < 0) {
        ::close(diag_pipe[0]);
        ::close(diag_pipe[1]);
        out << "\nALR EXECMEM ANON RX EXECUTION: FAIL";
        out << "\nalr execmem error=fork errno=" << errno;
        return out.str();
    }
    ::close(diag_pipe[1]);
    std::string diag;
    try {
        diag = read_all_from_fd(diag_pipe[0]);
    } catch (const std::exception& exc) {
        diag = exc.what();
    }
    ::close(diag_pipe[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    const bool exited = WIFEXITED(status);
    const int code = exited ? WEXITSTATUS(status) : -1;
    const int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    const bool ran = exited && code == 0 && diag.find("CALL_RET=42") != std::string::npos;
    out << "\nALR EXECMEM ANON RX EXECUTION: " << (ran ? "PASS" : "FAIL");
    out << "\nalr execmem mprotect rx=" << (diag.find("MPROTECT_OK") != std::string::npos ? "ALLOWED"
                                            : diag.find("MPROTECT_FAIL") != std::string::npos ? "DENIED"
                                                                                             : "unknown");
    out << "\nalr execmem child exit=" << code;
    out << "\nalr execmem child signal=" << sig;
    out << "\nalr execmem diag=" << diag;
    return out.str();
}

// Measure the cost of crossing the guest->host GPU command boundary three ways,
// to decide whether low-overhead GPU passthrough is feasible. The host GPU
// render itself is already proven; this measures the boundary, which is what
// determines whether a draw-call-heavy frame fits a 60fps (16.67ms) budget:
//   (1) in-process function dispatch  - the ALR-in-host-address-space lower bound
//   (2) socket per-command round-trip - naive inter-process IPC (worst case)
//   (3) shared-memory batched ring    - gfxstream-style (one cross-process sync
//       per batch), the realistic inter-process passthrough cost
std::string build_gpu_boundary_probe() {
    using clock = std::chrono::steady_clock;
    auto ns_per = [](clock::duration d, long iters) -> long long {
        const double total = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
        return iters > 0 ? static_cast<long long>(total / static_cast<double>(iters) + 0.5) : 0;
    };
    auto per_frame = [](long long ns_op) -> long long {
        return ns_op > 0 ? static_cast<long long>(16666666LL / ns_op) : 0;
    };
    struct GpuCmd {
        uint32_t op;
        float r;
        float g;
        float b;
    };
    constexpr long kInprocN = 200000;
    constexpr long kSockN = 20000;
    constexpr int kBatch = 256;

    std::ostringstream out;
    out << "ALR GPU BOUNDARY PROBE: PASS";

    // (1) in-process dispatch through a function pointer (not inlined away).
    {
        double (*dispatch)(const GpuCmd&) = [](const GpuCmd& c) -> double {
            return static_cast<double>(c.op) + c.r + c.g + c.b;
        };
        volatile double sink = 0;
        const auto t0 = clock::now();
        for (long i = 0; i < kInprocN; ++i) {
            const GpuCmd c{static_cast<uint32_t>(i), 0.1f, 0.2f, 0.3f};
            sink = sink + dispatch(c);
        }
        const auto t1 = clock::now();
        (void)sink;
        const long long per = ns_per(t1 - t0, kInprocN);
        out << "\nalr gpu boundary inproc dispatch ns/op=" << per;
        out << "\nalr gpu boundary inproc cmds per 60fps frame=" << per_frame(per);
    }

    // (2) socket per-command round-trip.
    {
        int sv[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::close(sv[0]);
                GpuCmd c{};
                while (::read(sv[1], &c, sizeof(c)) == static_cast<ssize_t>(sizeof(c))) {
                    const char ack = 1;
                    if (::write(sv[1], &ack, 1) != 1) {
                        break;
                    }
                }
                _exit(0);
            }
            ::close(sv[1]);
            const auto t0 = clock::now();
            for (long i = 0; i < kSockN; ++i) {
                const GpuCmd c{static_cast<uint32_t>(i), 0.1f, 0.2f, 0.3f};
                if (::write(sv[0], &c, sizeof(c)) != static_cast<ssize_t>(sizeof(c))) {
                    break;
                }
                char ack = 0;
                if (::read(sv[0], &ack, 1) != 1) {
                    break;
                }
            }
            const auto t1 = clock::now();
            ::close(sv[0]);
            int st = 0;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            const long long per = ns_per(t1 - t0, kSockN);
            out << "\nalr gpu boundary socket per-cmd ns/op=" << per;
            out << "\nalr gpu boundary socket cmds per 60fps frame=" << per_frame(per);
        } else {
            out << "\nalr gpu boundary socket per-cmd ns/op=skip errno=" << errno;
        }
    }

    // (3) shared-memory batched ring with one cross-process sync per batch.
    {
        const std::size_t ring_bytes = static_cast<std::size_t>(kBatch) * sizeof(GpuCmd);
        void* ring = ::mmap(nullptr, ring_bytes, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int sv[2] = {-1, -1};
        if (ring != MAP_FAILED && ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            GpuCmd* slots = static_cast<GpuCmd*>(ring);
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::close(sv[0]);
                char sig = 0;
                volatile double csink = 0;
                while (::read(sv[1], &sig, 1) == 1) {
                    for (int j = 0; j < kBatch; ++j) {
                        csink = csink + slots[j].op + slots[j].r;
                    }
                    const char ack = 1;
                    if (::write(sv[1], &ack, 1) != 1) {
                        break;
                    }
                }
                (void)csink;
                _exit(0);
            }
            ::close(sv[1]);
            const auto t0 = clock::now();
            for (long i = 0; i < kInprocN; ++i) {
                const int s = static_cast<int>(i % kBatch);
                slots[s].op = static_cast<uint32_t>(i);
                slots[s].r = 0.1f;
                slots[s].g = 0.2f;
                slots[s].b = 0.3f;
                if (s == kBatch - 1) {
                    const char sig = 1;
                    if (::write(sv[0], &sig, 1) != 1) {
                        break;
                    }
                    char ack = 0;
                    if (::read(sv[0], &ack, 1) != 1) {
                        break;
                    }
                }
            }
            const auto t1 = clock::now();
            ::close(sv[0]);
            int st = 0;
            while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            const long long per = ns_per(t1 - t0, kInprocN);
            out << "\nalr gpu boundary shmem-ring ns/op=" << per;
            out << "\nalr gpu boundary shmem-ring cmds per 60fps frame=" << per_frame(per);
            ::munmap(ring, ring_bytes);
        } else {
            if (ring != MAP_FAILED) {
                ::munmap(ring, ring_bytes);
            }
            out << "\nalr gpu boundary shmem-ring ns/op=skip errno=" << errno;
        }
    }

    out << "\nalr gpu boundary note=in-process and shared-ring per-cmd costs gate GPU passthrough feasibility; naive socket-per-cmd does not scale to draw-call-heavy frames";
    return out.str();
}

// Actually attempt the W^X-safe native exec the ALR strategy plans: copy the
// packaged static guest ELF into an anonymous in-memory fd (not an
// app_data_file) and execveat() it with AT_EMPTY_PATH. This measures the real
// on-device SELinux/W^X outcome for memfd-execveat in the app domain.
std::string build_memfd_exec_probe(const alr::RuntimeReportInput& input) {
    constexpr unsigned kMfdCloexec = 0x0001u;
    constexpr unsigned kMfdExec = 0x0008u;       // Linux 6.3+; memfd is NOEXEC by default otherwise
    constexpr int kAtEmptyPath = 0x1000;

    std::ostringstream out;
    out << "ALR MEMFD EXEC PROBE: android-native-attempt";
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr_runtime_config_from_input(input, launch);
    const std::string guest_host_path = config.rootfs_dir + "/bin/hello";
    out << "\nalr memfd guest source=/bin/hello";

    const int src = ::open(guest_host_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (src < 0) {
        out << "\nALR MEMFD CREATE: SKIP";
        out << "\nALR MEMFD EXECVEAT W^X-SAFE EXECUTION: FAIL";
        out << "\nalr memfd error=open guest errno=" << errno;
        return out.str();
    }
    std::string bytes;
    {
        char buffer[65536];
        ssize_t n = 0;
        while ((n = ::read(src, buffer, sizeof(buffer))) > 0) {
            bytes.append(buffer, static_cast<std::size_t>(n));
        }
    }
    ::close(src);
    out << "\nalr memfd bytes=" << bytes.size();

    // Prefer MFD_EXEC (kernels that default memfd to non-executable); fall back
    // to plain memfd on kernels that reject the flag.
    std::string memfd_flag = "MFD_EXEC";
    long mfd = ::syscall(__NR_memfd_create, "alr-guest-exec", kMfdExec);
    if (mfd < 0 && (errno == EINVAL || errno == ENOSYS)) {
        memfd_flag = "plain";
        mfd = ::syscall(__NR_memfd_create, "alr-guest-exec", 0u);
    }
    if (mfd < 0) {
        out << "\nALR MEMFD CREATE: FAIL";
        out << "\nALR MEMFD EXECVEAT W^X-SAFE EXECUTION: FAIL";
        out << "\nalr memfd error=memfd_create errno=" << errno;
        return out.str();
    }
    (void)kMfdCloexec;
    const int memfd = static_cast<int>(mfd);
    out << "\nALR MEMFD CREATE: PASS";
    out << "\nalr memfd flag=" << memfd_flag;

    std::size_t off = 0;
    bool write_ok = true;
    while (off < bytes.size()) {
        const ssize_t w = ::write(memfd, bytes.data() + off, bytes.size() - off);
        if (w <= 0) {
            write_ok = false;
            break;
        }
        off += static_cast<std::size_t>(w);
    }
    if (!write_ok) {
        out << "\nALR MEMFD EXECVEAT W^X-SAFE EXECUTION: FAIL";
        out << "\nalr memfd error=write memfd errno=" << errno;
        ::close(memfd);
        return out.str();
    }

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0 || ::pipe(err_pipe) != 0) {
        out << "\nALR MEMFD EXECVEAT W^X-SAFE EXECUTION: FAIL";
        out << "\nalr memfd error=pipe errno=" << errno;
        ::close(memfd);
        return out.str();
    }

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        char arg0[] = "/bin/hello";
        char* child_argv[] = {arg0, nullptr};
        char* child_envp[] = {nullptr};
        ::syscall(__NR_execveat, memfd, "", child_argv, child_envp, kAtEmptyPath);
        const int e = errno;
        ::dprintf(STDERR_FILENO, "EXECVEAT_ERRNO=%d", e);
        _exit(127);
    }
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    std::string child_stdout;
    std::string child_stderr;
    try {
        child_stdout = read_all_from_fd(out_pipe[0]);
        child_stderr = read_all_from_fd(err_pipe[0]);
    } catch (const std::exception& exc) {
        child_stderr = exc.what();
    }
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    ::close(memfd);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    const int code = WIFEXITED(status) ? WEXITSTATUS(status)
                     : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                           : -1;
    const bool ran = child_stdout.find("hello from static arm64 rootfs") != std::string::npos;
    out << "\nALR MEMFD EXECVEAT W^X-SAFE EXECUTION: " << (ran ? "PASS" : "FAIL");
    out << "\nalr memfd child exit=" << code;
    out << "\nalr memfd child stdout=" << child_stdout;
    out << "\nalr memfd child stderr=" << child_stderr;
    return out.str();
}

#if defined(__aarch64__)
// Capture the exact fault site when the loaded guest crashes, so glibc startup
// failures can be pinpointed instead of guessed. Runs on an alternate stack so a
// corrupt guest sp does not break signal delivery; async-signal-safe.
static void diag_hex(int fd, unsigned long long v) {
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    int n = 2;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const int d = static_cast<int>((v >> shift) & 0xf);
        b[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[n++] = ' ';
    ::write(fd, b, static_cast<std::size_t>(n));
}

// arm64 trampoline: set sp to the prepared stack, clear x0 (rtld_fini must be
// null for a static _start), and branch to the guest entry. Defined in file
// asm so no reserved-register clobbers are needed; it never returns.
// x0=sp, x1=entry, x2=thread pointer (clean zeroed TCB; 0 for freestanding). Set
// TPIDR_EL0 to the clean TCB so the guest glibc does not read bionic's TCB before
// it sets up its own TLS.
extern "C" [[noreturn]] void alr_enter_guest(void* sp, void* entry, void* tcb);
__asm__(
    ".globl alr_enter_guest\n"
    ".hidden alr_enter_guest\n"
    "alr_enter_guest:\n"
    "  mov sp, x0\n"
    "  msr tpidr_el0, x2\n"
    "  mov x3, x1\n"
    "  mov x0, #0\n"
    "  br x3\n");

// ADR-003-v3 MECHANISM PROOF (R10 step 1). A resident, freestanding raw-syscall
// routine the supervisor PC-redirects the tracee to at an execve seccomp-trap (after
// cancelling the execve via NT_ARM_SYSTEM_CALL=-1). It runs IN the guest process —
// the loader's .text is inherited via fork and never unmapped, and &this function is
// identical in supervisor and tracee — so reaching it proves the cancel+PC-redirect
// path works WITHOUT any kernel execve. Step 2 replaces the body with the real
// in-process map(ld.so+target)+jump. No libc/TLS/stack assumptions: pure svc #0.
// Writes a marker to fd 2 (guest stderr → loader pipe → logcat) then _exit(123).
extern "C" [[noreturn]] void alr_inproc_reexec_probe();
__asm__(
    ".globl alr_inproc_reexec_probe\n"
    ".hidden alr_inproc_reexec_probe\n"
    "alr_inproc_reexec_probe:\n"
    "  mov x0, #2\n"            // fd = stderr
    "  adr x1, 1f\n"           // buf
    "  mov x2, #(2f - 1f)\n"   // len (assembler-computed)
    "  mov x8, #64\n"          // __NR_write
    "  svc #0\n"
    "  mov x0, #123\n"         // exit code
    "  mov x8, #93\n"          // __NR_exit
    "  svc #0\n"
    "  brk #0\n"               // unreachable
    "1: .ascii \"ALR-REEXEC: inproc trampoline reached (no execve)\\n\"\n"
    "2:\n");

// Install a SECOND seccomp filter (stacked on the zygote's) that SECCOMP_RET_TRACEs
// ONLY the path-taking syscalls, so the parent supervisor can rewrite a guest path
// to its rootfs host path before the kernel dereferences it. This is the
// low-overhead path-mediation primitive proven viable on-device by
// build_seccomp_pathtrap_probe: only file syscalls trap (not every syscall like
// PRoot), and NO_NEW_PRIVS (already set by zygote) satisfies the kernel gate.
// hello/mt-test never openat a file, so they trap zero times and are unaffected.
static bool alr_install_path_trace_filter(int dg) {
    ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    // Path arg is x1 for every syscall traced here (all are *at-style with the
    // pathname as the second argument), which the parent rewrites uniformly.
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),  // foreign arch -> allow
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 9, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat2, 8, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_newfstatat, 7, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_statx, 6, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat, 5, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_faccessat2, 4, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_readlinkat, 3, 0),
        // Write-path: dir/file creation must also land in the rootfs (GIMP creates
        // its config tree via mkdirat; apps unlink temp files). Both carry the
        // path in x1, so the supervisor's existing x1 rewrite handles them.
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mkdirat, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_unlinkat, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),  // not a path syscall
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),  // path syscall -> tracer
    };
    struct sock_fprog prog = {
        static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
        filter,
    };
    long fr = ::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0u, &prog);
    if (fr != 0) {
        fr = ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
    }
    if (fr != 0) {
        ::write(dg, "PFLT_FAIL;", 10);
        return false;
    }
    ::write(dg, "PFLT_OK;", 8);
    return true;
}

// PCGATE fast path (ALR_PCGATE != "0", the default): the LOADER traces ONLY
// execve/execveat and RET_ALLOWs everything else — INCLUDING the 9 path syscalls,
// which it deliberately does NOT trace here. In this mode the in-process
// LD_PRELOAD interposer installs its own PC-gated path filter (see
// libalr_interpose.c): syscalls it emits from its trampoline run un-traced
// (RET_ALLOW), while any path syscall outside that trampoline still RET_TRACEs as
// a backstop. seccomp filters STACK and the kernel takes the MOST-RESTRICTIVE
// action per syscall (ALLOW is the WEAKEST action — TRACE wins over ALLOW), so the
// loader MUST emit ALLOW for the path syscalls here; otherwise a loader TRACE
// would override the interposer's later ALLOW and nothing would speed up.
// We still trace execve/execveat so the supervisor retains W^X exec re-entry
// control hooks (the handler refinement that rewrites the exec'd program path into
// the rootfs is a noted follow-up; the trap point must exist first). The full
// 9-path-syscall filter above remains the ALR_PCGATE=="0" A/B baseline.
static bool alr_install_execve_trace_filter(int dg) {
    ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),  // foreign arch -> allow
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execve, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execveat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE),  // execve/execveat -> tracer
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),  // everything else -> allow
    };
    struct sock_fprog prog = {
        static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
        filter,
    };
    long fr = ::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0u, &prog);
    if (fr != 0) {
        fr = ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0);
    }
    if (fr != 0) {
        ::write(dg, "XFLT_FAIL;", 10);
        return false;
    }
    ::write(dg, "XFLT_OK;", 8);
    return true;
}

// Result of mapping one ELF image (program or interpreter) into execmem.
struct MappedImage {
    uintptr_t base = 0;   // load bias: runtime_addr = base + p_vaddr
    uintptr_t entry = 0;  // base + e_entry
    uintptr_t phdr = 0;   // runtime address of THIS image's program headers
    uint16_t phent = 0;   // e_phentsize
    uint16_t phnum = 0;   // e_phnum
    bool ok = false;
};

// Map every PT_LOAD of `img` (a full ELF file already read into memory, length
// `len`) into anonymous execmem via the v57-proven W^X-safe RW->memcpy->RX
// MAP_FIXED path, then apply R_AARCH64_IRELATIVE (IFUNC). Diagnostics go to `dg`
// with a short `tag` prefix ("PROG:"/"INTERP:") so the program and interpreter
// stages are distinguishable in the report. ET_DYN images (PIE programs and
// ld.so) get a PROT_NONE reservation first for a contiguous base; ET_EXEC keeps
// base==0. Runs in the forked child after TRACEME (raw mmap/mprotect/memcpy +
// write() only). Returns ok=false (and a tagged diag) on any failure.
static MappedImage map_elf_image_into_execmem(const char* img, std::size_t len,
                                              int dg, const char* tag) {
    MappedImage R{};
    if (len < sizeof(Elf64_Ehdr)) {
        ::write(dg, tag, ::strlen(tag));
        ::write(dg, "SHORT;", 6);
        return R;
    }
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(img);
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(img + eh->e_phoff);

    uintptr_t min_v = ~static_cast<uintptr_t>(0);
    uintptr_t max_v = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        const uintptr_t s = static_cast<uintptr_t>(ph[i].p_vaddr) & ~static_cast<uintptr_t>(0xfff);
        const uintptr_t e = static_cast<uintptr_t>(ph[i].p_vaddr + ph[i].p_memsz);
        if (s < min_v) min_v = s;
        if (e > max_v) max_v = e;
    }
    if (min_v == ~static_cast<uintptr_t>(0)) {
        ::write(dg, tag, ::strlen(tag));
        ::write(dg, "NO_LOAD;", 8);
        return R;
    }

    const std::size_t span = (max_v - min_v + 0xfff) & ~static_cast<uintptr_t>(0xfff);
    uintptr_t base = 0;
    if (eh->e_type == ET_DYN) {
        void* reserve = ::mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reserve == MAP_FAILED) {
            ::write(dg, tag, ::strlen(tag));
            ::write(dg, "RESERVE_FAIL;", 13);
            return R;
        }
        base = reinterpret_cast<uintptr_t>(reserve) - min_v;
    }

    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) {
            continue;
        }
        const uintptr_t seg_start = base + (static_cast<uintptr_t>(ph[i].p_vaddr) & ~static_cast<uintptr_t>(0xfff));
        const uintptr_t v_off = static_cast<uintptr_t>(ph[i].p_vaddr) & static_cast<uintptr_t>(0xfff);
        const std::size_t map_len = (v_off + ph[i].p_memsz + 0xfff) & ~static_cast<uintptr_t>(0xfff);
        void* m = ::mmap(reinterpret_cast<void*>(seg_start), map_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m == MAP_FAILED) {
            ::write(dg, tag, ::strlen(tag));
            ::write(dg, "SEG_MAP_FAIL;", 13);
            return R;
        }
        std::memcpy(reinterpret_cast<void*>(base + ph[i].p_vaddr), img + ph[i].p_offset, ph[i].p_filesz);
        int prot = 0;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
        if (::mprotect(reinterpret_cast<void*>(seg_start), map_len, prot) != 0) {
            ::write(dg, tag, ::strlen(tag));
            ::write(dg, "SEG_PROT_FAIL;", 14);
            return R;
        }
        if (prot & PROT_EXEC) {
            __builtin___clear_cache(reinterpret_cast<char*>(seg_start),
                                    reinterpret_cast<char*>(seg_start + map_len));
        }
    }

    // Apply R_AARCH64_IRELATIVE. Prefer PT_DYNAMIC's DT_RELA/DT_RELASZ (required
    // for ld.so, which is usually section-header-stripped); fall back to SHT_RELA
    // section headers only if PT_DYNAMIC is absent (the static-program case).
    int irel = 0;
    const Elf64_Phdr* dynph = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dynph = &ph[i];
            break;
        }
    }
    if (dynph != nullptr) {
        const auto* dyn = reinterpret_cast<const Elf64_Dyn*>(base + dynph->p_vaddr);
        uintptr_t rela = 0;
        std::size_t relasz = 0;
        std::size_t relaent = sizeof(Elf64_Rela);
        for (; dyn->d_tag != DT_NULL; ++dyn) {
            if (dyn->d_tag == DT_RELA) rela = base + dyn->d_un.d_ptr;
            if (dyn->d_tag == DT_RELASZ) relasz = static_cast<std::size_t>(dyn->d_un.d_val);
            if (dyn->d_tag == DT_RELAENT) relaent = static_cast<std::size_t>(dyn->d_un.d_val);
        }
        if (rela != 0 && relaent != 0) {
            for (std::size_t off = 0; off < relasz; off += relaent) {
                const auto* r = reinterpret_cast<const Elf64_Rela*>(rela + off);
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_IRELATIVE) {
                    continue;
                }
                struct IfuncArg {
                    unsigned long size;
                    unsigned long hwcap;
                    unsigned long hwcap2;
                } arg = {sizeof(IfuncArg), ::getauxval(AT_HWCAP), ::getauxval(AT_HWCAP2)};
                using Resolver = unsigned long (*)(unsigned long, const void*);
                Resolver resolver = reinterpret_cast<Resolver>(base + r->r_addend);
                *reinterpret_cast<uint64_t*>(base + r->r_offset) = resolver(arg.hwcap, &arg);
                ++irel;
            }
        }
    } else if (eh->e_shoff != 0 && eh->e_shentsize == sizeof(Elf64_Shdr)) {
        const auto* sh = reinterpret_cast<const Elf64_Shdr*>(img + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; ++i) {
            if (sh[i].sh_type != SHT_RELA || sh[i].sh_entsize != sizeof(Elf64_Rela)) {
                continue;
            }
            const auto* rl = reinterpret_cast<const Elf64_Rela*>(img + sh[i].sh_offset);
            const std::size_t cnt = sh[i].sh_size / sizeof(Elf64_Rela);
            for (std::size_t j = 0; j < cnt; ++j) {
                if (ELF64_R_TYPE(rl[j].r_info) != R_AARCH64_IRELATIVE) {
                    continue;
                }
                struct IfuncArg {
                    unsigned long size;
                    unsigned long hwcap;
                    unsigned long hwcap2;
                } arg = {sizeof(IfuncArg), ::getauxval(AT_HWCAP), ::getauxval(AT_HWCAP2)};
                using Resolver = unsigned long (*)(unsigned long, const void*);
                Resolver resolver = reinterpret_cast<Resolver>(base + rl[j].r_addend);
                *reinterpret_cast<uint64_t*>(base + rl[j].r_offset) = resolver(arg.hwcap, &arg);
                ++irel;
            }
        }
    }
    ::write(dg, tag, ::strlen(tag));
    ::write(dg, "IREL=", 5);
    diag_hex(dg, static_cast<unsigned long long>(irel));

    // AT_PHDR: prefer PT_PHDR; else the PT_LOAD covering e_phoff; else base+e_phoff.
    uintptr_t at_phdr = 0;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_PHDR) {
            at_phdr = base + ph[i].p_vaddr;
            break;
        }
    }
    if (at_phdr == 0) {
        for (int i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type == PT_LOAD && eh->e_phoff >= ph[i].p_offset &&
                eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
                at_phdr = base + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
                break;
            }
        }
    }
    if (at_phdr == 0) {
        at_phdr = base + eh->e_phoff;
    }

    R.base = base;
    R.entry = base + eh->e_entry;
    R.phdr = at_phdr;
    R.phent = eh->e_phentsize;
    R.phnum = eh->e_phnum;
    R.ok = true;
    return R;
}
#endif

// The decisive ALR native-exec proof: a userspace ELF loader. It maps a static
// guest binary's PT_LOAD segments into anonymous execmem (the v57-proven W^X-safe
// path), builds a SysV initial stack with argv/envp/auxv, and jumps to the entry
// point in a forked child. No PRoot, no ptrace, no writable-file exec. Staged
// diagnostics report how far it got; an alarm() guards against a hung guest.
std::string build_native_loader_probe(const alr::RuntimeReportInput& input) {
    std::ostringstream out;
    out << "ALR NATIVE LOADER PROBE: android-native-attempt";
    const auto launch = alr::build_alr_runtime_launch_plan(input);
    const auto config = alr_runtime_config_from_input(input, launch);
    // The guest binary is whatever the caller asked for (e.g. /bin/hello which
    // prints "hello from static arm64 rootfs", or /bin/mt-test which exercises
    // threads + fork). Success is a clean exit(0) with captured output.
    // The program spec is newline-delimited argv: the first token is the guest
    // path (used to open the binary) and the rest are arguments, so callers can
    // run e.g. "/bin/dash\n-c\necho hi" with a multi-word argument preserved.
    std::vector<std::string> guest_argv;
    {
        const std::string spec = input.program.empty() ? "/bin/hello" : input.program;
        std::size_t start = 0;
        while (start <= spec.size()) {
            const std::size_t nl = spec.find('\n', start);
            if (nl == std::string::npos) {
                guest_argv.push_back(spec.substr(start));
                break;
            }
            guest_argv.push_back(spec.substr(start, nl - start));
            start = nl + 1;
        }
        if (guest_argv.empty()) {
            guest_argv.push_back("/bin/hello");
        }
    }
    const std::string guest_rel = guest_argv[0];
    const std::string host_path = config.rootfs_dir + guest_rel;
    out << "\nalr native loader guest=" << guest_rel;
    out << "\nalr native loader argc=" << guest_argv.size();

    // The guest environment. Built in the parent (config/cache dirs are in scope)
    // and pushed onto the child stack. LD_LIBRARY_PATH lets the in-process ld.so
    // find rootfs libs; the Wayland/XDG vars point a GUI client at the in-app
    // compositor's AF_UNIX socket (harmless for non-GUI guests).
    const std::string xdg_runtime_dir =
        (input.app_cache_dir.empty() ? std::string("/data/local/tmp") : input.app_cache_dir) +
        "/alr-xdg";
    std::vector<std::string> guest_env;
    guest_env.push_back("GLIBC_TUNABLES=glibc.pthread.rseq=0");
    // CP-2: a GLES guest (glmark2) dlopens the GPU shim libEGL.so.1/libGLESv2.so.2 from
    // /usr/lib/androlinux — it MUST resolve ahead of any rootfs/vendor GL lib, so prepend
    // that dir. Gated on glmark2 so the general path never risks shim-shadowing a real
    // libEGL (non-GPU guests don't dlopen those sonames anyway).
    const std::string ld_shim =
        config.program.find("glmark2") != std::string::npos
            ? (config.rootfs_dir + "/usr/lib/androlinux:") : std::string();
    guest_env.push_back("LD_LIBRARY_PATH=" + ld_shim +
                        config.rootfs_dir + "/lib/aarch64-linux-gnu:" +
                        config.rootfs_dir + "/lib:" + config.rootfs_dir + "/usr/lib/aarch64-linux-gnu:" +
                        config.rootfs_dir + "/usr/lib");
    guest_env.push_back("PATH=/usr/bin:/bin:/usr/sbin:/sbin");
    guest_env.push_back("HOME=/root");
    guest_env.push_back("TERM=xterm-256color");
    guest_env.push_back("XDG_RUNTIME_DIR=" + xdg_runtime_dir);
    guest_env.push_back("WAYLAND_DISPLAY=wayland-0");
    guest_env.push_back("GDK_BACKEND=wayland");
    guest_env.push_back("SDL_VIDEODRIVER=wayland");
    // Qt6 apps: select the wayland QPA plugin (libqwayland-generic.so, shipped 0755 in
    // the qt6 demo overlay) instead of Qt's compiled-in default (xcb → no X server →
    // abort). Harmless for non-Qt guests. Pairs with the r4 qt6 analogclock launch.
    guest_env.push_back("QT_QPA_PLATFORM=wayland");
    // ALR compositor is wl_shm-only (no wl-egl / dmabuf for clients); skip Qt's
    // client-side decoration plugin (a SIGSEGV suspect) so it uses the SHM path.
    guest_env.push_back("QT_WAYLAND_DISABLE_WINDOWDECORATION=1");
    // Belt-and-braces with the r6 qt6 overlay (EGL/HW-integration plugins removed):
    // also disable Qt's wayland client buffer HW (EGL/dmabuf) integration at runtime
    // so it never tries eglGetDisplay (no ICD) — forces the wl_shm backing store.
    guest_env.push_back("QT_WAYLAND_DISABLE_HW_INTEGRATION=1");
    // GTK/GIMP startup: render with cairo (no client GL yet), an in-memory
    // GSettings backend (no dconf/D-Bus), a UTF-8 locale, and rootfs-relative XDG
    // dirs. Service-file paths (fontconfig, gdk-pixbuf loaders, gschemas) are
    // guest paths the loader's path mediation maps into the rootfs.
    guest_env.push_back("GDK_RENDERING=cairo");
    guest_env.push_back("GSETTINGS_BACKEND=memory");
    guest_env.push_back("LC_ALL=C.UTF-8");
    guest_env.push_back("LANG=C.UTF-8");
    guest_env.push_back("XDG_DATA_DIRS=/usr/local/share:/usr/share");
    // WS-1: libxkbcommon's compiled-in XKB_CONFIG_ROOT is the GUEST path
    // /usr/share/X11/xkb, and its keymap-file lookups (opendir/stat, not just open)
    // don't reliably path-mediate into the rootfs → keymap compile fails ("Couldn't
    // find rules/evdev") → GUI clients SIGSEGV on the NULL keymap. Point it at the
    // rootfs-ABSOLUTE xkb dir: that's an already-host path, so the supervisor's
    // idempotency guard skips rewriting it and libxkbcommon opens the real files.
    guest_env.push_back("XKB_CONFIG_ROOT=" + config.rootfs_dir + "/usr/share/X11/xkb");
    // WS-1: gdk-pixbuf가 loaders.cache + loader .so를 절대경로(/usr/lib/...)로 dlopen
    // 하는데, XKB와 동일하게 path-mediate가 안 잡혀 ENOENT → SVG 아이콘 로드 실패로 GTK abort.
    // rootfs-ABSOLUTE module file/dir로 직접 가리켜 mediation 우회(이미 host 경로 → idempotency 가드 skip).
    guest_env.push_back("GDK_PIXBUF_MODULE_FILE=" + config.rootfs_dir +
                        "/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache");
    guest_env.push_back("GDK_PIXBUF_MODULEDIR=" + config.rootfs_dir +
                        "/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders");
    guest_env.push_back("XDG_CONFIG_HOME=/root/.config");
    guest_env.push_back("XDG_CACHE_HOME=/root/.cache");  // fontconfig cache (writable)
    guest_env.push_back("FONTCONFIG_PATH=/etc/fonts");
    // The rootfs root, for an in-process LD_PRELOAD path interposer (fast path
    // mediation without a ptrace round-trip per file op). The interposer wraps
    // the libc file entry points (open/openat/stat/access/...) and rewrites
    // absolute guest paths to <rootfs>+path IN-PROCESS before the syscall.
    // Under PCGATE (ALR_PCGATE != "0", the default) the interposer also installs a
    // PC-gated seccomp filter: a path syscall it emits from its own trampoline
    // matches the gate's instruction-pointer range and is RET_ALLOWed (NO trap,
    // NO ptrace round-trip — this is what eliminates the per-file traps GIMP
    // hammers at startup, thousands of font/brush/data opens); a path syscall
    // OUTSIDE the trampoline (pre-constructor, or post-exec with a stale range)
    // falls to RET_TRACE and the supervisor rewrites it as the backstop. The
    // loader's own filter in this mode (alr_install_execve_trace_filter) traces
    // only execve/execveat, so it never overrides that gated ALLOW. The interposer
    // reads ALR_ROOTFS at init and self-disables (pure passthrough) if it is unset
    // — fail-safe; with the interposer absent (ALR_DISABLE_INTERPOSE=1) AND PCGATE
    // on, NO path filter exists at all, so that A/B arm measures the cost floor and
    // is expected to read wrong host paths. The full-trace baseline (ALR_PCGATE=="0")
    // keeps the supervisor doing every rewrite, exactly as before PCGATE.
    guest_env.push_back("ALR_ROOTFS=" + config.rootfs_dir);
    // ALR_GUEST_EXE: the GUEST-visible program path (argv[0]). The interposer returns
    // it for readlink("/proc/self/exe") — the real /proc/self/exe of this in-process
    // guest is the Android APK, so apps that locate their assets via the executable
    // path (chromium ICU/pak via PathService DIR_MODULE; many glibc apps) would look in
    // the wrong directory. With this, DIR_MODULE resolves under the rootfs and normal
    // path mediation maps the asset open. Only used when guest_rel is absolute.
    guest_env.push_back("ALR_GUEST_EXE=" + guest_rel);
    // Two A/B gates, both read from the HOST (app) environment and decided here in
    // the parent. They are hoisted to function scope (not an inner block) so the
    // report lines below — including the path-mediation traps/rewrites line — can
    // re-use them.
    //   ALR_DISABLE_INTERPOSE (default OFF, i.e. interposer ON): setting it to "1"
    //     omits the LD_PRELOAD interposer, so the seccomp/ptrace net does ALL path
    //     rewrites. Used to prove the interposer's effect (compare path_rewrites).
    //   ALR_PCGATE (default ON): "0" reproduces the full-9-syscall loader trace
    //     (pre-PCGATE baseline); anything else selects the reduced execve-only
    //     loader filter (alr_install_execve_trace_filter) plus the interposer's
    //     PC-gated path filter. NOTE the inverse polarity vs ALR_DISABLE_INTERPOSE:
    //     PCGATE defaults ON so it is on unless the first char is exactly '0'.
    // ALR_PCGATE is pushed into the guest env UNCONDITIONALLY (=0 or =1) so the
    // interposer constructor reads exactly what the loader chose (never its own
    // default); the loader's filter-install site (in the forked child) independently
    // re-reads ALR_PCGATE from the COW-inherited host env, so the two cannot disagree.
    const bool interpose_off = []{
        const char* d = ::getenv("ALR_DISABLE_INTERPOSE");
        return d != nullptr && d[0] == '1';
    }();
    const bool pcgate_on = []{
        const char* p = ::getenv("ALR_PCGATE");
        return !(p != nullptr && p[0] == '0');
    }();
    // ALR_FAKEROOT (default OFF): when the app-process sets it to "1" right before a
    // dpkg/apt drain (gated behind the .alr-aptdrain device marker), chain the
    // fakeroot .so FIRST in LD_PRELOAD (so its credential wrappers — getuid->0,
    // chown/stat no-ops — sit OUTSIDE the interposer) and push FAKEROOTUID/GID=0.
    // Read from the HOST (app) env via ::getenv, exactly like ALR_DISABLE_INTERPOSE
    // and ALR_PCGATE above — keeps the JNI signature fixed. When unset every line
    // below is byte-identical to the pre-fakeroot env, so normal launch is no-op.
    const bool fakeroot_on = []{
        const char* f = ::getenv("ALR_FAKEROOT");
        return f != nullptr && f[0] == '1';
    }();
    if (!interpose_off) {
        // R3 / bootstrap: LD_PRELOAD MUST be the ABSOLUTE ROOTFS HOST path, not the
        // guest path. In PCGATE=1 the loader no longer traces path syscalls, so ld.so's
        // open of the preload is NOT rewritten by the supervisor; a guest path
        // ("/usr/lib/...") would hit the HOST fs, the preload would fail to load, and the
        // interposer's PC-gate filter would never install (no speedup AND no mediation).
        // The host-absolute path opens with no mediation needed, and is idempotently
        // left alone by the PCGATE=0 supervisor too, so it is correct in both A/B arms.
        std::string preload =
            config.rootfs_dir + "/usr/lib/androlinux/libalr_interpose.so";
        if (fakeroot_on) {
            // fakeroot FIRST (credential outer layer), interpose KEPT. Both are
            // ABSOLUTE ROOTFS host paths (R3), matching chain_ld_preload() in the
            // host model tools/aptdrain_env_model.py.
            preload = config.rootfs_dir +
                      "/usr/lib/androlinux/libalr_fakeroot.so:" + preload;
        }
        guest_env.push_back("LD_PRELOAD=" + preload);
    }
    if (fakeroot_on) {
        // fakeroot identity contract: make the guest see uid/gid 0 so dpkg's
        // chown/stat root:root checks pass under a non-root Android process.
        guest_env.push_back("FAKEROOTUID=0");
        guest_env.push_back("FAKEROOTGID=0");
    }
    guest_env.push_back(pcgate_on ? "ALR_PCGATE=1" : "ALR_PCGATE=0");
    // ALR_INTERPOSE_DIAG (default OFF): when the app-process sets it, propagate
    // to the guest so the interposer emits its one-line chdir/relative-create
    // trace to stderr. Read from the HOST env like ALR_PCGATE; pure diagnostic.
    {
        const char* idiag = ::getenv("ALR_INTERPOSE_DIAG");
        if (idiag != nullptr && idiag[0] != '0' && idiag[0] != '\0') {
            guest_env.push_back(std::string("ALR_INTERPOSE_DIAG=") + idiag);
        }
    }
    // Record both arms in the report so each run is self-identifying for A/B.
    out << "\nalr native loader pcgate=" << (pcgate_on ? "on" : "off")
        << " interpose=" << (interpose_off ? "off" : "on");

    const int fd = ::open(host_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        out << "\nALR NATIVE LOADER PARSE: FAIL\nALR NATIVE LOADER MAP: FAIL"
               "\nALR NATIVE LOADER GUEST EXEC: FAIL";
        out << "\nalr native loader error=open errno=" << errno;
        return out.str();
    }
    std::string elf;
    {
        char buffer[65536];
        ssize_t n = 0;
        while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
            elf.append(buffer, static_cast<std::size_t>(n));
        }
    }
    ::close(fd);
    out << "\nalr native loader bytes=" << elf.size();
    if (elf.size() < sizeof(Elf64_Ehdr) || std::memcmp(elf.data(), "\x7f""ELF", 4) != 0) {
        out << "\nALR NATIVE LOADER PARSE: FAIL\nALR NATIVE LOADER MAP: FAIL"
               "\nALR NATIVE LOADER GUEST EXEC: FAIL";
        return out.str();
    }
    const char* elf_ptr = elf.data();
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(elf_ptr);
    out << "\nALR NATIVE LOADER PARSE: PASS";
    out << "\nalr native loader elf type="
        << (eh->e_type == ET_DYN ? "ET_DYN(static-PIE)" : eh->e_type == ET_EXEC ? "ET_EXEC" : "other");
    out << "\nalr native loader entry=0x" << std::hex << eh->e_entry << std::dec;
    out << "\nalr native loader phnum=" << eh->e_phnum;

    int out_pipe[2] = {-1, -1};
    int diag_pipe[2] = {-1, -1};
    // go_pipe is the SEIZE attach handshake (replaces the TRACEME initial-stop).
    // The child blocks reading one byte from go_pipe[0] right after fork(), BEFORE
    // it maps/jumps to the guest; the parent writes that byte ONLY after PTRACE_SEIZE
    // has attached with options installed. This guarantees the device-proven invariant
    // — TRACECLONE/FORK/VFORK/EXEC/TRACESECCOMP are live before the guest runs a single
    // traced syscall — without relying on SIGSTOP timing (SEIZE does not stop the
    // child, and a pre-SEIZE SIGSTOP would arrive as an ambiguous attach group-stop,
    // exactly the class of confusion this conversion removes).
    int go_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0 || ::pipe(diag_pipe) != 0 || ::pipe(go_pipe) != 0) {
        out << "\nALR NATIVE LOADER MAP: FAIL\nALR NATIVE LOADER GUEST EXEC: FAIL";
        out << "\nalr native loader error=pipe errno=" << errno;
        return out.str();
    }

    // WS-1 ↔ WS-2 (CP-0 §5-A): for a GLES guest (glmark2), attach the GPU command ring
    // BEFORE fork so the guest's libGLESv2 shim drives the host Mali executor. The
    // ring/doorbell fds are inheritable (non-CLOEXEC) → the forked guest keeps them.
    // present_window=null → executor renders headless (glmark2 fps = real Mali); on-
    // screen present lands with WS-3's PresentSource (§5-C). Non-GPU guests skip this
    // (no executor / Mali-thread cost). attach failure → CPU-only (no env pushed; the
    // shim then runs ring-less = quiet no-op).
    alr::gpu::GpuRing gpu_ring{};
    bool gpu_ring_attached = false;
    if (config.program.find("glmark2") != std::string::npos) {
        alr::gpu::GpuRingAttachConfig gcfg;
        gcfg.fb_w = 1280;
        gcfg.fb_h = 720;
        if (alr::gpu::alr_loader_attach_gpu_ring(gpu_ring, gcfg)) {
            gpu_ring_attached = true;
            for (const auto& kv : alr::gpu::gpu_ring_guest_env(gpu_ring))
                guest_env.push_back(kv);
        }
    }
    const auto t_exec_start = std::chrono::steady_clock::now();  // WS-1 M2: native-exec wall-clock (fork→reap)
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(diag_pipe[0]);
        ::close(go_pipe[1]);  // child reads the SEIZE-ready byte from go_pipe[0]
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(out_pipe[1], STDERR_FILENO);  // capture guest stderr too (ld.so/glib errors)
        const int dg = diag_pipe[1];
        // SEIZE attach handshake (replaces TRACEME + raise(SIGSTOP)). Under
        // PTRACE_SEIZE the PARENT attaches us — the child issues no PTRACE_TRACEME and
        // is never self-stopped. Instead we BLOCK here reading one byte the parent
        // writes only AFTER it has PTRACE_SEIZE'd us with all options installed. This
        // is the sync point that lets the parent set TRACECLONE/FORK/VFORK/EXEC/
        // TRACESECCOMP before we run any traced syscall (the guest's seccomp filter is
        // installed much later, just before alr_enter_guest, so this read() itself is
        // un-traced). The loop tolerates EINTR; on a hard read error we proceed anyway
        // (degrades to an untraced run rather than wedging the child).
        {
            char go = 0;
            ssize_t gr = 0;
            do {
                gr = ::read(go_pipe[0], &go, 1);
            } while (gr < 0 && errno == EINTR);
        }
        ::close(go_pipe[0]);
#if defined(__aarch64__)
        const Elf64_Ehdr* ce = reinterpret_cast<const Elf64_Ehdr*>(elf_ptr);
        const Elf64_Phdr* ph = reinterpret_cast<const Elf64_Phdr*>(elf_ptr + ce->e_phoff);

        // Detect dynamic vs static by PT_INTERP. For a dynamic executable we load
        // the guest's own ld-linux-aarch64.so.1 and hand control to IT (the
        // interpreter then maps the program's DT_NEEDED libraries and links them);
        // we do not re-implement dynamic linking.
        const char* interp_str = nullptr;  // e.g. "/lib/ld-linux-aarch64.so.1"
        for (int i = 0; i < ce->e_phnum; ++i) {
            if (ph[i].p_type == PT_INTERP) {
                interp_str = elf_ptr + ph[i].p_offset;  // NUL-terminated in the file
                break;
            }
        }
        const bool dynamic = (interp_str != nullptr);
        ::write(dg, dynamic ? "DYN;" : "STATIC;", dynamic ? 4 : 7);

        // Read the interpreter image from the rootfs (dynamic only).
        std::string interp_bytes;
        if (dynamic) {
            std::string interp_host = config.rootfs_dir;
            interp_host += interp_str;  // guest path begins with '/'
            const int ifd = ::open(interp_host.c_str(), O_RDONLY | O_CLOEXEC);
            if (ifd < 0) {
                ::write(dg, "INTERP_OPEN_FAIL;", 17);
                _exit(75);
            }
            char ibuf[65536];
            ssize_t in = 0;
            while ((in = ::read(ifd, ibuf, sizeof(ibuf))) > 0) {
                interp_bytes.append(ibuf, static_cast<std::size_t>(in));
            }
            ::close(ifd);
            if (interp_bytes.size() < sizeof(Elf64_Ehdr) ||
                std::memcmp(interp_bytes.data(), "\x7f""ELF", 4) != 0) {
                ::write(dg, "INTERP_BAD_ELF;", 15);
                _exit(76);
            }
            ::write(dg, "INTERP_READ=", 12);
            diag_hex(dg, static_cast<unsigned long long>(interp_bytes.size()));
        }

        // Map the program (ET_DYN PIE, or rare ET_EXEC) and, when dynamic, the
        // interpreter — both via the shared W^X-safe execmem mapper.
        MappedImage prog = map_elf_image_into_execmem(elf_ptr, elf.size(), dg, "PROG:");
        if (!prog.ok) {
            _exit(72);
        }
        MappedImage interp{};
        if (dynamic) {
            interp = map_elf_image_into_execmem(interp_bytes.data(), interp_bytes.size(),
                                                dg, "INTERP:");
            if (!interp.ok) {
                _exit(77);
            }
        }
        ::write(dg, "MAPPED;", 7);

        // Reset all signal dispositions to SIG_DFL via the RAW rt_sigaction
        // syscall, bypassing ART's libsigchain wrapper. libsigchain keeps a
        // process-wide SIGSEGV handler (inherited by this child) that, on any
        // signal, calls bionic pthread_getspecific using bionic's TLS; once the
        // guest sets its own TPIDR_EL0 that read faults (the captured crash was
        // libsigchain -> pthread_getspecific, masking the real guest fault).
        // Going straight to the kernel removes libsigchain from the path.
        {
            struct KSigaction {
                void (*handler)(int);
                unsigned long flags;
                unsigned long mask;
            } dfl = {nullptr, 0, 0};  // SIG_DFL
            for (int s = 1; s <= 64; ++s) {
                ::syscall(__NR_rt_sigaction, s, &dfl, nullptr, 8);
            }
            unsigned long unblock = 0;
            ::syscall(__NR_rt_sigprocmask, SIG_SETMASK, &unblock, nullptr, 8);
        }
        ::write(dg, "SIGRESET;", 9);

        const std::size_t stack_size = 512 * 1024;
        void* stk = ::mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stk == MAP_FAILED) {
            ::write(dg, "STACK_FAIL", 10);
            _exit(74);
        }
        uintptr_t top = reinterpret_cast<uintptr_t>(stk) + stack_size;
        const unsigned long host_random = ::getauxval(AT_RANDOM);
        top -= 16;
        if (host_random != 0) {
            std::memcpy(reinterpret_cast<void*>(top), reinterpret_cast<void*>(host_random), 16);
        } else {
            std::memset(reinterpret_cast<void*>(top), 0x5a, 16);
        }
        const uintptr_t at_random = top;
        // Push all argv strings (argv[0] is the guest path; the rest are the
        // caller's arguments, e.g. dash -c '<cmd>'). busybox/multi-call binaries
        // select their applet from argv[0]; shells need -c and its argument.
        constexpr std::size_t kMaxArgv = 64;
        const std::size_t argc = guest_argv.size() < kMaxArgv ? guest_argv.size() : kMaxArgv;
        uintptr_t argv_ptrs[kMaxArgv] = {0};
        for (std::size_t i = argc; i-- > 0;) {
            const std::size_t an = guest_argv[i].size() + 1;
            top -= an;
            std::memcpy(reinterpret_cast<void*>(top), guest_argv[i].c_str(), an);
            argv_ptrs[i] = top;
        }
        const uintptr_t argv0 = argv_ptrs[0];
        // Disable glibc's rseq registration: bionic already owns this thread's
        // rseq area, and a second registration conflicts in-process.
        // Push all envp strings (built in the parent, COW-inherited here).
        // Headroom: base env (~23) + ALR_ROOTFS/ALR_GUEST_EXE + LD_PRELOAD +
        // FAKEROOTUID/GID + ALR_PCGATE + ALR_INTERPOSE_DIAG + the GpuRing env block
        // can exceed 32; an undersized cap would silently TRUNCATE the tail
        // (dropping the GpuRing vars, or LD_PRELOAD itself if reordered), so keep
        // generous headroom. envp_ptrs[] is sized to match.
        constexpr std::size_t kMaxEnv = 48;
        const std::size_t n_env = guest_env.size() < kMaxEnv ? guest_env.size() : kMaxEnv;
        uintptr_t envp_ptrs[kMaxEnv] = {0};
        for (std::size_t i = n_env; i-- > 0;) {
            const std::size_t en = guest_env[i].size() + 1;
            top -= en;
            std::memcpy(reinterpret_cast<void*>(top), guest_env[i].c_str(), en);
            envp_ptrs[i] = top;
        }

        // The auxv describes the PROGRAM to the interpreter (AT_PHDR/AT_ENTRY are
        // the program's), but AT_BASE is the interpreter's load base — exactly what
        // the kernel's binfmt_elf hands ld.so. For the static case interp.base is
        // unused and AT_BASE is 0, the program entry is the jump target.
        const uint64_t aux[] = {
            AT_PHDR, prog.phdr,
            AT_PHENT, prog.phent,
            AT_PHNUM, prog.phnum,
            AT_PAGESZ, 4096,
            AT_BASE, dynamic ? interp.base : 0,
            AT_FLAGS, 0,
            AT_ENTRY, prog.entry,
            AT_EXECFN, argv0,
            AT_UID, ::getuid(),
            AT_EUID, ::geteuid(),
            AT_GID, ::getgid(),
            AT_EGID, ::getegid(),
            AT_HWCAP, ::getauxval(AT_HWCAP),
            AT_HWCAP2, ::getauxval(AT_HWCAP2),
            AT_CLKTCK, 100,
            AT_RANDOM, at_random,
            AT_SECURE, 0,
            AT_SYSINFO_EHDR, ::getauxval(AT_SYSINFO_EHDR),
            AT_NULL, 0,
        };
        const std::size_t n_aux = sizeof(aux) / sizeof(aux[0]);
        const std::size_t n_words = 1 + (argc + 1) + (n_env + 1) + n_aux;
        uintptr_t start = (top - n_words * 8) & ~static_cast<uintptr_t>(0xf);
        uint64_t* sp_words = reinterpret_cast<uint64_t*>(start);
        std::size_t k = 0;
        sp_words[k++] = argc;    // argc
        for (std::size_t i = 0; i < argc; ++i) {
            sp_words[k++] = argv_ptrs[i];  // argv[i]
        }
        sp_words[k++] = 0;       // argv NULL
        for (std::size_t i = 0; i < n_env; ++i) {
            sp_words[k++] = envp_ptrs[i];  // envp[i]
        }
        sp_words[k++] = 0;       // envp NULL
        for (std::size_t i = 0; i < n_aux; ++i) {
            sp_words[k++] = aux[i];
        }
        // A clean, zeroed thread pointer for the brief window before glibc sets up
        // its own TLS, so it never reads bionic's TCB.
        void* tcb_region = ::mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        const uintptr_t tcb = tcb_region == MAP_FAILED ? 0 : reinterpret_cast<uintptr_t>(tcb_region) + 8192;
        // Dynamic: jump to the INTERPRETER's entry (it links the program, then
        // jumps to AT_ENTRY). Static: jump straight to the program entry.
        const uintptr_t jump_entry = dynamic ? interp.entry : prog.entry;
        ::write(dg, "E@", 2); diag_hex(dg, jump_entry);
        ::write(dg, "code=", 5); diag_hex(dg, *reinterpret_cast<volatile uint64_t*>(jump_entry));
        ::write(dg, "P@", 2); diag_hex(dg, prog.phdr);
        ::write(dg, "p0=", 3); diag_hex(dg, *reinterpret_cast<volatile uint64_t*>(prog.phdr));
        ::write(dg, "sp@", 3); diag_hex(dg, start);
        ::write(dg, "argc=", 5); diag_hex(dg, sp_words[0]);
        if (dynamic) {
            ::write(dg, "BASE=", 5); diag_hex(dg, interp.base);
            ::write(dg, "PROGE=", 6); diag_hex(dg, prog.entry);
            ::write(dg, "PROGB=", 6); diag_hex(dg, prog.base);
            ::write(dg, "INTRE@", 6); diag_hex(dg, interp.entry);
        }
        // v2 DIAG (ALR_INTERPOSE_DIAG): dump the LD_PRELOAD / ALR_INTERPOSE_DIAG
        // entries actually present on the handoff stack envp, so a drain can SEE
        // whether the guest ld.so received the chained preload at all (vs the
        // interposer ctor silently not running). Reads the same envp the guest gets.
        {
            const char* edg = ::getenv("ALR_INTERPOSE_DIAG");
            if (edg != nullptr && edg[0] != '0' && edg[0] != '\0') {
                for (std::size_t i = 0; i < n_env; ++i) {
                    const char* e = reinterpret_cast<const char*>(envp_ptrs[i]);
                    bool is_pre = e[0]=='L'&&e[1]=='D'&&e[2]=='_'&&e[3]=='P';
                    bool is_dia = e[0]=='A'&&e[1]=='L'&&e[2]=='R'&&e[3]=='_'&&e[4]=='I';
                    if (is_pre || is_dia) {
                        ::write(dg, "\nENVP[", 6);
                        char nb[4]; int ni=0, v=(int)i; if(v==0)nb[ni++]='0';
                        char tmp[4]; int t=0; while(v){tmp[t++]=(char)('0'+v%10);v/=10;}
                        while(t)nb[ni++]=tmp[--t]; ::write(dg, nb, ni);
                        ::write(dg, "]=", 2);
                        std::size_t L=0; while(e[L]&&L<512)++L; ::write(dg, e, L);
                    }
                }
                ::write(dg, "\n", 1);
            }
        }
        ::write(dg, "JUMPING;", 8);
        // Stack the seccomp filter LAST, after the loader's own file reads (the
        // rootfs opens at the open() above and the ELF reads), so the guest runs
        // under it from its very first instruction. WHICH filter depends on
        // ALR_PCGATE, re-read here in the forked child; the host env is COW-inherited
        // across fork(), so this matches the parent's pcgate decision used for the
        // guest-env push and the report — they cannot disagree.
        //   ALR_PCGATE != "0" (default): reduced execve/execveat-only TRACE. The
        //     in-process interposer installs the PC-gated path filter, so path
        //     syscalls it emits post-rewrite run un-traced. FAIL-SAFE: if the
        //     interposer is absent (ALR_DISABLE_INTERPOSE=1) NO path filter exists,
        //     so path syscalls are ALLOWed un-mediated — that A/B arm measures the
        //     cost floor and is EXPECTED to read wrong host paths; it bounds cost,
        //     it is not a correctness configuration.
        //   ALR_PCGATE == "0" (baseline): the full 9-path-syscall TRACE filter, i.e.
        //     today's behavior, so the supervisor does every rewrite. Used for A/B.
        // Polarity matches the parent: on unless the first char is exactly '0'.
        {
            const char* pcgate = ::getenv("ALR_PCGATE");
            const bool pcgate_on = !(pcgate != nullptr && pcgate[0] == '0');
            if (pcgate_on) {
                alr_install_execve_trace_filter(dg);
            } else {
                alr_install_path_trace_filter(dg);
            }
        }
        // Dynamic GUI guests (GIMP) must persist long enough to be USED
        // interactively (touch -> redraw), not just rendered once; a tiny static
        // probe keeps a short leash. 1800s ~= 30 min of live, interactive GIMP.
        // Verification flow runs several long-lived GUI apps in sequence; each would
        // otherwise block the loader for the full interactive budget. 25s is enough to
        // map + render + measure (frame counter) each one, then SIGALRM ends it so the
        // next app runs (also bounds foot's interactive-shell wait). Was 1800 for a live
        // interactive GIMP session — parameterize per-launch when returning to that.
        // CP-6 storm re-diagnosis (PR #2 measure-first): chromium's 186MB+~200-.so
        // dynamic link + V8 bootstrap is a HEAVY single-init that the 25s window can
        // kill BEFORE the first worker clone — which would masquerade as a "1-thread
        // deadlock". Give a chromium guest a much larger window so a device drain can
        // read clone_events: >0 = it reached worker clones (the deadlock was a
        // misdiagnosis, it was the window); still 0 at 120s = a real stall to chase.
        const bool is_chromium = host_path.find("chrom") != std::string::npos;
        // GIMP (the Phase-6 interactive headline) has a HEAVY cold load — fontconfig
        // cache build + plug-in query (each plug-in a fork+exec) + babl/gegl init —
        // that does NOT finish inside the 25s verification window, so SIGALRM kills it
        // MID-LOAD and it looks "frozen on the loading screen". It runs LAST in the
        // sequence, so a long lifetime blocks nothing after it; give it a real
        // interactive window so the load completes and the user can actually use it
        // (this is the per-launch lifetime the old comment above promised). The 25s
        // default still cycles the intermediate verification apps quickly.
        const bool is_gimp = host_path.find("gimp") != std::string::npos;
        // dpkg/apt install class: the deep fork+exec install chain (dpkg ->
        // dpkg-deb/dpkg-split/tar + maintainer-script sh) runs ENTIRELY in-process
        // (ALR_REEXEC_INPROC), each re-mapped glibc child doing hundreds of path traps
        // under the single-threaded ptrace supervisor — legitimately ~10-50x slower
        // than native. The device drain that fixed the supervisor cross-talk
        // (__WNOTHREAD) showed galculator's dpkg -i now UNPACKS (unpacked=true,
        // exit -1/signal 14 = SIGALRM at ~26s) — i.e. it was making real progress and
        // the 25s alarm killed it MID-CONFIGURE, not a wedge. Give the package tools a
        // chromium-class window so unpack+configure completes ("Setting up …"). The
        // progress-aware watchdog below still SIGKILLs a genuinely stuck install
        // quickly (no-progress timeout), so this larger ceiling cannot mask a real hang.
        const bool is_pkgtool =
            host_path.find("/dpkg") != std::string::npos ||
            host_path.find("/apt") != std::string::npos;
        // CR-1 measure-first (chromium-run-plan / PR #2): a 140s drain showed
        // chromium --single-process --dump-dom did not render within 120s (the loader
        // went silent ~2min under chromium's thread/syscall storm via the serialized
        // ptrace supervision). Give chromium 600s to answer "does it render GIVEN time
        // (window-bound) or never (a real supervision-throughput wall)?".
        const unsigned alarm_sec =
            dynamic ? (is_gimp ? 1800u : ((is_chromium || is_pkgtool) ? 180u : 25u))
                    : 5u;
        ::alarm(alarm_sec);
        alr_enter_guest(reinterpret_cast<void*>(start), reinterpret_cast<void*>(jump_entry),
                        reinterpret_cast<void*>(tcb));
        _exit(99);  // unreachable
#else
        ::write(dg, "NOARCH", 6);
        _exit(80);
#endif
    }
    ::close(out_pipe[1]);
    ::close(diag_pipe[1]);
    ::close(go_pipe[0]);  // parent writes the SEIZE-ready byte to go_pipe[1]
    // === CONCURRENT PIPE DRAIN — device-root-caused (galculator dpkg configure) ===
    // The guest's stdout+stderr are dup'd onto out_pipe[1] and its diag onto
    // diag_pipe[1]; the parent USED to read both ONLY AFTER the supervisor loop
    // exited. But a chatty guest fills the 64 KiB kernel pipe buffer and then blocks
    // in pipe_write FOREVER, because nothing drains the read end while the loop runs.
    // Device signature (galculator dpkg -i, post-unpack configure): the re-mapped dpkg
    // emitted hundreds of `dpkg: warning: … missing 'Maintainer' field` lines (one per
    // package in /var/lib/dpkg/status) — well over 64 KiB — and wedged
    //   guest tid=20896 state=S wchan=pipe_write syscall=64   (write, blocked)
    // with the supervisor idle in wait4 and the no-progress watchdog firing at 41s.
    // This is NOT a ptrace stall (state=S, not t): the guest is alive but flow-control-
    // blocked on a full pipe. FIX: drain BOTH pipes on dedicated reader threads NOW, so
    // the guest never blocks on output. The readers hit EOF (read()==0) when the guest
    // and every in-process re-mapped child holding the dup'd write end have exited —
    // i.e. right as the supervisor reaps the last tracee — then we join them after the
    // loop. read_all_from_fd already loops to EOF; running it on a thread is behavior-
    // identical for a guest that never fills the buffer (hello/--version), so no
    // regression. Strings are filled by the threads and consumed after join().
    std::string guest_stdout_buf;
    std::string diag_buf;
    std::string drain_err;
    const int out_rd = out_pipe[0];
    const int diag_rd = diag_pipe[0];
    // CR-4 live tee: when ALR_TEE_GUEST_STDOUT=1, stream the guest's stdout/stderr +
    // the trampoline diag pipe to logcat LINE BY LINE as they arrive, instead of only
    // accumulating for the final report. This is the only way to see chromium's
    // --enable-logging=stderr --v=1 init trace (and the ALR-INPROC re-map diag) WHILE a
    // GUI chromium is wedged — the report is emitted only after the probe returns, but a
    // hung multiprocess guest never lets it return until the watchdog SIGKILLs (and the
    // post-kill reap can stall), so the buffered text would never surface. We still
    // append every line to the buffer so the report is byte-identical when it does
    // return. Gated (chromium --v=1 is very verbose) so normal GIMP/GPU runs are quiet.
    const bool tee_guest = []{
        const char* e = ::getenv("ALR_TEE_GUEST_STDOUT");
        return e != nullptr && e[0] == '1';
    }();
    // Line-buffered fd drain that optionally tees to logcat. Reuses read() directly
    // (read_all_from_fd reads to EOF, which defeats live streaming). Appends raw bytes
    // to *sink; flushes complete '\n'-delimited lines to logcat under `tag` when teeing.
    auto drain_fd_tee = [tee_guest](int fd, std::string* sink, const char* tag) {
        std::string line;
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n == 0) break;
            sink->append(buf, static_cast<size_t>(n));
            if (!tee_guest) continue;
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    __android_log_print(ANDROID_LOG_INFO, tag, "%s", line.c_str());
                    line.clear();
                } else if (line.size() < 8192) {
                    line.push_back(buf[i]);
                }
            }
        }
        if (tee_guest && !line.empty()) {
            __android_log_print(ANDROID_LOG_INFO, tag, "%s", line.c_str());
        }
    };
    std::thread out_reader([out_rd, &guest_stdout_buf, &drain_err, &drain_fd_tee]() {
        try { drain_fd_tee(out_rd, &guest_stdout_buf, "alr_cr_out"); }
        catch (const std::exception& e) { drain_err = e.what(); }
    });
    std::thread diag_reader([diag_rd, &diag_buf, &drain_fd_tee]() {
        // diag failures are non-fatal (the report still stands on stdout + exit code);
        // swallow so a diag-pipe error never masks a good run.
        try { drain_fd_tee(diag_rd, &diag_buf, "alr_cr_diag"); } catch (const std::exception&) {}
    });
    // SEIZE attach (replaces the child's PTRACE_TRACEME + initial SIGSTOP). The child
    // is blocked reading go_pipe[0]; we attach with PTRACE_SEIZE — which, unlike
    // TRACEME, installs all options ATOMICALLY at attach and does NOT stop the tracee
    // — then release the child by writing the go byte. Because SEIZE (with options)
    // completes before the child is unblocked, every thread/child it later spawns is
    // auto-traced and every path syscall is RET_TRACE-mediated from the guest's first
    // instruction, with no window where the guest runs un-optioned. The options are
    // byte-identical to the former post-SIGSTOP SETOPTIONS set, so single/few-thread
    // guests (GIMP, chromium --version) attach to the exact same traced configuration.
    const long kSeizeOpts = static_cast<long>(
        PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
        PTRACE_O_TRACEEXEC | PTRACE_O_TRACESECCOMP);
    bool seized = false;
    if (pid > 0) {
        seized = ::ptrace(PTRACE_SEIZE, pid, nullptr,
                          reinterpret_cast<void*>(kSeizeOpts)) == 0;
        // Release the child regardless: if SEIZE somehow failed we still unblock it so
        // it does not hang in read() forever (it then runs untraced — the same
        // degraded outcome the old code would reach if TRACEME had failed).
        const char go = 1;
        ssize_t gw = 0;
        do {
            gw = ::write(go_pipe[1], &go, 1);
        } while (gw < 0 && errno == EINTR);
    }
    ::close(go_pipe[1]);
    // Trace the child so a fatal-signal PC/fault address can be read from the
    // parent (whose TLS is intact). Resume on non-fatal signals; capture and let
    // the fatal one terminate the child. Pipes are drained only after it dies, to
    // avoid blocking on a child stopped under ptrace.
    int code = -1;
    int sig = 0;
    unsigned long long fault_pc = 0;
    unsigned long long fault_addr = 0;
    unsigned long long fault_lr = 0;
    unsigned long long fault_x0 = 0;
    unsigned long long fault_x8 = 0;
    int fault_signo = 0;
    int fault_syscall = -1;
    int emulated_syscalls = 0;
    int emulated_list[64] = {0};
    int guest_threads = 0;
    int path_traps = 0;
    int path_rewrites = 0;
    // ADR-003 exec re-entry instrumentation (M-R4-execmap counters). These are
    // ZERO for every guest that never execs (GIMP/foot/chromium --version), so
    // the no-exec path is byte-unchanged; they only advance on the new is_exec
    // branch below.
    int exec_traps = 0;       // execve/execveat EVENT_SECCOMP traps seen
    int exec_rewrites = 0;    // x0 program-path rewrites into the rootfs (B-1)
    int exec_events = 0;      // PTRACE_EVENT_EXEC stops (new image entered) (B-2)
    // ADR-003 §3 (B-3) child envp re-injection counters. envp_injected counts execs
    // whose envp we rebuilt (x2/x3 pointed at an augmented array) so the new image
    // re-enters ALR's interpose mediation (LD_PRELOAD=<abs rootfs .so>+ALR_ROOTFS).
    // ld_preload_set counts the subset where the rebuilt envp carries our LD_PRELOAD
    // (added or prepended). Both are 0 for any guest that never execs.
    int envp_injected = 0;
    int ld_preload_set = 0;
    // ADR-003-v2 (R8-A Option S): execve re-map via the static re-entry stub. The
    // device finding (v139 drain#17) is that B-1 path-rewrite + B-3 envp fire but
    // exec_events stays 0 — the kernel can't execve a glibc-aarch64 ELF (its
    // PT_INTERP = guest ld.so is unresolvable), so the new image never enters. The
    // fix: splice the exec to run <rootfs>/usr/lib/androlinux/alr-reentry (a STATIC
    // aarch64 ELF the kernel CAN load) with argv=[stub, target_host, orig argv1..];
    // the stub then maps the glibc target in-process (inheriting seccomp+SEIZE+envp)
    // and jumps to ld.so.
    // DEVICE FINDING (v140 drain#18, R9): the splice FIRES correctly (spliced=1, stub
    // + target paths right), but the kernel-execve of the stub NEVER completes
    // (exec_events=0, stub never runs) EVEN WITH the stub present at the path — because
    // the stub lives in app storage (app_data_file) and untrusted_app's W^X SELinux
    // policy (targetSdk 35) forbids execve() of any app-storage file. This is the SAME
    // wall that requires ALR's in-process ELF mapping. So Option S via a rootfs-path
    // stub is DEAD; default is now OFF (opt-in via ALR_EXEC_REENTRY=1). The mechanism
    // is retained for the two live continuations: (a) re-point the stub to
    // nativeLibraryDir (the bundled-executable hatch — needs extractNativeLibs=true)
    // and re-test the SELinux verdict, or (b) ADR-003-v3 in-process re-map (NO execve:
    // a resident file-backed-PROT_EXEC trampoline maps the new ELF in-process — the
    // mmap is allowed where execve is not). See docs/evidence/2026-06-02-round9-*.
    const bool exec_reentry_on = []{
        const char* e = ::getenv("ALR_EXEC_REENTRY");
        return e != nullptr && e[0] == '1';
    }();
    // ADR-003-v3 (R10) in-process re-map. On a guest execve seccomp-trap, CANCEL the
    // syscall (NT_ARM_SYSTEM_CALL=-1) and PC-redirect the tracee to a resident loader
    // trampoline (no kernel execve — the W^X-forbidden primitive). STEP 1 (this build):
    // the trampoline is a marker-print+exit probe, to prove the cancel+redirect path
    // reaches resident code in the guest. STEP 2 swaps in the real map+jump. Default ON
    // for the mechanism-proof drain (touches only the already-broken execve path).
    // PROVEN (v141 drain#19, R10 step 1): the trampoline IS reached in-guest with NO
    // kernel execve (`ALR-REEXEC: inproc trampoline reached`, inproc_redirected=1,
    // child exit=123), no regression. Now gated default-OFF (ALR_REEXEC_INPROC=1 to
    // opt in) until STEP 2 replaces the probe body with the real map(ld.so+target)+jump
    // — until then the probe just exits 123, which would break any exec'ing guest.
    const bool inproc_reexec_on = []{
        const char* e = ::getenv("ALR_REEXEC_INPROC");
        return e != nullptr && e[0] == '1';
    }();
    int exec_inproc_redirected = 0;  // execs PC-redirected into the in-process trampoline
    // CR-4 instrumentation + runaway guard. The GUI chromium re-map storm (this
    // session's wall): ozone-wayland spawns GPU/utility/renderer children, each a
    // fork()+execve("/proc/self/exe", ["--type=...", …]). GATE-1 below substitutes the
    // launch chrome (host_path) for EVERY such self-exe and in-process re-maps it — a
    // full ~800 MiB image per child. If a child crashes (e.g. the GPU child on
    // --disable-gpu) the browser RETRIES it, and each retry is another self-exe exec ->
    // another full re-map -> monotonic RSS growth (device: 542->801->1056->1311 MiB)
    // -> OOM before paint. CR-5 headless (--dump-dom) never hit this because it spawns
    // ~no children. We (1) LOG each self-exe redirect with its --type and the process
    // RSS so a device drain pins which child type recurs, and (2) CAP the total number
    // of self-exe re-maps per launch: past the cap we let the execve proceed as a plain
    // B-1 path-rewrite (no in-process re-map) so a runaway child fails to relaunch
    // instead of OOM-killing the whole app. The cap is generous (covers a healthy GUI's
    // browser+gpu+utility+renderer ≈ a handful of children plus a few restarts) but
    // finite, so a crash-retry loop is bounded, not unbounded. Env-overridable for
    // bring-up (ALR_SELFEXE_REMAP_CAP); 0/unset uses the default.
    int self_exe_remaps = 0;          // total /proc/self/exe -> chrome re-maps this launch
    const int self_exe_remap_cap = []{
        const char* e = ::getenv("ALR_SELFEXE_REMAP_CAP");
        if (e != nullptr) { int v = ::atoi(e); if (v > 0) return v; }
        return 12;  // healthy GUI: browser+gpu+utility+renderer + a few restarts
    }();
    bool self_exe_cap_logged = false;
    std::string first_self_exe_type;  // first observed --type= on a self-exe child
    // G1 seqint: execs DELIBERATELY NOT inproc-redirected (fell through to B-1/B-3).
    // The trampoline can only map a rootfs glibc target, so we must skip /proc/self/exe
    // (resolves to the loader's own ANDROID bionic binary, interp /system/bin/linker64),
    // any non-rootfs target, and the alr-reentry stub itself. Skipping them keeps the
    // serialized supervision from wedging (a device drain showed an over-broad redirect
    // stalled the onCreate probe sequence after ~9s on the chromium zygote's /proc/self/exe
    // exec) so WS-1 can safely flip ALR_REEXEC_INPROC default-ON.
    int exec_inproc_skipped = 0;
    std::string first_inproc_skip_reason;  // proc-self-exe | non-rootfs | stub
    std::string first_inproc_skip_target;  // the gp that was skipped
    int exec_reentry_spliced = 0;   // execs spliced to run via the re-entry stub
    std::string first_reentry_target;  // first target the stub was asked to re-map
    std::string first_exec_x0;       // first exec target the guest requested
    std::string first_exec_reason;   // its mediation reason (rewrite/sysdir/…)
    std::string first_exec_envp_reason;  // first exec's envp-injection reason (B-3)
    // M-R2 (ADR-002): storm decomposition — per-syscall-nr histograms at the two
    // EXISTING trap sites (no new ptrace op, no hot-loop pollution). trace_hist =
    // RET_TRACE/EVENT_SECCOMP (path-family + execve), emul_hist = SIGSYS-emulated
    // (blocked nrs). The path vs non-path nr ratio directly answers where a raw-svc
    // storm's round-trips originate (bench/syscall_mix.py parses these lines).
    std::unordered_map<int, uint64_t> trace_hist;
    std::unordered_map<int, uint64_t> emul_hist;
    std::string first_rewrite;
    bool captured = false;
    // Under PTRACE_SEIZE the options were installed ATOMICALLY at attach (the 4th arg
    // to PTRACE_SEIZE), so the leader is already fully optioned before its first stop —
    // there is no longer a TRACEME initial-SIGSTOP at which to call SETOPTIONS. The
    // latch therefore starts true; it is kept only so the defensive per-new-tid
    // SETOPTIONS re-apply below (now a belt-and-suspenders, since SEIZE auto-inherits
    // options to cloned tids) reads a sensible flag. seized==false (SEIZE itself
    // failed) is the lone case where options never got set; the loop still drains the
    // untraced child to a clean exit exactly as the old TRACEME-failure path would.
    bool options_set = seized;
    // --- Per-trap cost optimizations (behavior-identical to the open+close-every-
    // trap baseline; verified against the device-proven GIMP path mediation). ---
    //
    // (1) Per-tid /proc/<tid>/mem fd cache. The baseline open()+close()d
    // /proc/<tid>/mem on every PTRACE_EVENT_SECCOMP trap. Under a heavy guest
    // (Chromium --dump-dom: 22 threads hammering path syscalls) that is two extra
    // syscalls per trap. We instead open the mem fd O_RDWR LAZILY on the first
    // trap for a tid and REUSE it for every subsequent trap from that tid: pread/
    // pwrite take an explicit offset, so the same fd serves any path_addr. The fd
    // is closed when the tid is reaped (WIFEXITED/WIFSIGNALED below — the options
    // set on the leader do NOT include PTRACE_O_TRACEEXIT, so no PTRACE_EVENT_EXIT
    // fires; reaping is the reliable death signal here) and evicted+reopened if a
    // read/write ever fails with a stale-fd errno (ESRCH/EIO/EBADF). A final sweep
    // after the loop closes any survivors. The I/O itself — O_RDWR open, pread(path), pwrite
    // (scratch) — is byte-for-byte the SAME as the baseline, so mediation behavior
    // is unchanged; only the redundant open+close are removed.
    // known_tids: every tid we've already seen stop at least once. The leader pid is
    // pre-seeded. A PTRACE_EVENT_STOP from a tid NOT yet here is a freshly-cloned child's
    // INITIAL stop (resume with CONT); from a KNOWN tid with GETSIGINFO==EINVAL it is a
    // real group-stop (LISTEN). Without this split, GETSIGINFO==EINVAL on a new child's
    // first stop would wrongly LISTEN-park it, hanging the guest at the first clone.
    std::unordered_set<pid_t> known_tids{pid};
    // listening_tids: tids currently PTRACE_LISTEN-parked (group-stop BEGIN). THE
    // CHROMIUM DEADLOCK FIX (sd-design+sd-model, evidence 2026-06-02-cr1...): a
    // LISTEN-parked tid re-reports EVENT_STOP at the group-stop END (SIGCONT trailing
    // edge) whose GETSIGINFO is STILL EINVAL, so the old code re-classified it as a
    // fresh group-stop and re-LISTEN-ed it FOREVER — a sibling futex-waiting (nr 98,
    // un-traced) on it then never woke, the leader never exited, and waitpid blocked
    // forever (chromium --dump-dom, ~20 threads, 600s-immune hang). Fix: a KNOWN+EINVAL
    // stop from an ALREADY-listening tid is the END -> PTRACE_CONT it back to RUNNING
    // (never re-LISTEN across a SIGCONT). A fresh group-stop (not yet listening) still
    // LISTENs once.
    std::unordered_set<pid_t> listening_tids;
    std::unordered_map<pid_t, int> mem_fds;
    // mem_fd_for: return a cached O_RDWR /proc/<tid>/mem fd for `tid`, opening it
    // on first use. Returns -1 if the open fails (caller then skips, exactly as the
    // baseline skipped when its per-trap open failed — same observable behavior).
    auto mem_fd_for = [&mem_fds](pid_t tid) -> int {
        auto it = mem_fds.find(tid);
        if (it != mem_fds.end()) {
            return it->second;
        }
        const std::string memf = "/proc/" + std::to_string(tid) + "/mem";
        const int mfd = ::open(memf.c_str(), O_RDWR);
        // Cache the fd only on success; a failed open is retried on the next trap
        // (matching the baseline, which re-open()ed every trap).
        if (mfd >= 0) {
            mem_fds.emplace(tid, mfd);
        }
        return mfd;
    };
    // mem_fd_evict: close + drop a tid's cached fd. Used both when a tid exits and
    // when a pread/pwrite fails with a stale-fd errno so the next trap reopens it.
    auto mem_fd_evict = [&mem_fds](pid_t tid) {
        auto it = mem_fds.find(tid);
        if (it != mem_fds.end()) {
            if (it->second >= 0) {
                ::close(it->second);
            }
            mem_fds.erase(it);
        }
    };
    // (2) translate_rootfs_path result cache. translate_rootfs_path() is a PURE
    // function of (rootfs_dir, cwd, guest_path); in this handler rootfs_dir
    // (config.rootfs_dir) and cwd ("/") are CONSTANT for the whole run, so the
    // result depends ONLY on the guest path string. A repeated guest path (GIMP
    // re-opens the same font/brush/data files constantly) therefore yields the
    // IDENTICAL host path. We memoize guest_path -> host_path in a bounded hash
    // (cleared wholesale when it would exceed the cap — rootfs paths never change
    // during a run, so nothing needs targeted invalidation). A hit reuses the
    // cached host string for the scratch write and skips the translate call. Same
    // input => same output, so the rewrite is provably unchanged.
    std::unordered_map<std::string, std::string> xlate_cache;
    constexpr std::size_t kXlateCacheCap = 256;
    // === ADR-003-v3 §2.2/§4: parent-side stall WATCHDOG (chromium-deadlock bound +
    // diagnostic). The per-guest alarm() is armed in the CHILD (alr_enter_guest path),
    // but a thread that is ptrace-stopped / PTRACE_LISTEN-parked cannot service SIGALRM
    // — so a wedged multithread guest (chromium --dump-dom) makes this waitpid loop
    // block FOREVER (no event, no child death), silently hanging the loader. This
    // join-able watchdog thread sleeps a deadline (a margin past the child alarm); if
    // the supervisor has not finished, it DUMPS every tracee's /proc state (which
    // thread is stuck WHERE — state/wchan/syscall) to logcat, then SIGKILLs the guest
    // process group so waitpid returns and the probe reports instead of hanging. For a
    // healthy guest the supervisor finishes first (sup_done=true) and the watchdog
    // no-ops, so it cannot regress GIMP/glmark2/--version etc.
    std::atomic<bool> sup_done{false};
    // === diagnostic: ring buffer of the LAST supervisor events + §2.1 guard activity,
    // dumped by the watchdog so a stall shows the EXACT decision sequence that led to the
    // freeze (not just the frozen end-state). Each slot packs tid(16)|event(8)|stopsig(8).
    std::array<uint32_t, 48> ev_ring{};
    std::atomic<uint32_t> ev_head{0};
    std::atomic<uint32_t> guard_fires{0};  // times the all-parked INTERRUPT guard ran
    std::atomic<uint32_t> guard_ints{0};   // total PTRACE_INTERRUPTs it issued
    // Absolute lifetime CEILING (a hard upper bound). chromium and the dpkg/apt
    // install class both legitimately run long under the serialized in-process
    // supervisor, so they get the large ceiling; GIMP is interactive; everything else
    // keeps the short 40s ceiling.
    const unsigned watchdog_sec =
        (host_path.find("chrom") != std::string::npos ||
         host_path.find("/dpkg") != std::string::npos ||
         host_path.find("/apt") != std::string::npos) ? 200u
        : (host_path.find("gimp") != std::string::npos ? 1830u : 40u);
    // PROGRESS-AWARE no-progress timeout. The OLD watchdog was a FIXED deadline: it
    // killed any guest still alive at watchdog_sec, even one making steady progress —
    // which is why a legitimately-slow dpkg install (unpack+configure of galculator,
    // hundreds of in-process re-map path traps) got SIGKILLed mid-configure. We now
    // ALSO track event progress (ev_head, bumped on every waitpid stop): if the
    // supervisor has not processed a SINGLE new event for kNoProgressSec seconds it is
    // genuinely wedged (the device wedge signature was exactly this — leader state=t,
    // supervisor idle in wait4, ev_ring frozen), so fire EARLY regardless of the
    // ceiling. A guest that is still trapping/cloning keeps resetting the no-progress
    // timer and runs up to the ceiling. This catches a real hang as fast as the old
    // 40s (a wedged guest emits no events) while no longer killing a working install.
    const unsigned kNoProgressSec = 40u;
    const pid_t leader_pid = pid;
    const pid_t sup_tid = ::gettid();  // the single tracer thread, for self-diagnosis
    std::thread watchdog([leader_pid, sup_tid, watchdog_sec, &sup_done,
                          &ev_ring, &ev_head, &guard_fires, &guard_ints]() {
        uint32_t last_ev = ev_head.load(std::memory_order_acquire);
        unsigned stagnant = 0;   // consecutive seconds with NO event progress
        unsigned elapsed = 0;    // total seconds the guest has run
        const char* why = "ceiling";
        for (;;) {
            if (sup_done.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            ++elapsed;
            const uint32_t now_ev = ev_head.load(std::memory_order_acquire);
            if (now_ev != last_ev) {       // progress -> reset the no-progress timer
                last_ev = now_ev;
                stagnant = 0;
            } else {
                ++stagnant;
            }
            if (stagnant >= kNoProgressSec) { why = "no-progress"; break; }
            if (elapsed >= watchdog_sec) { why = "ceiling"; break; }
        }
        if (sup_done.load(std::memory_order_acquire)) return;
        // Stall confirmed: dump every tracee thread's kernel state, then kill.
        __android_log_print(ANDROID_LOG_WARN, "alr_loader",
                            "alr sup-stall WATCHDOG fired (%s) after %us leader=%d sup_tid=%d — dumping + SIGKILL",
                            why, elapsed, static_cast<int>(leader_pid), static_cast<int>(sup_tid));
        // Dump one /proc/<dir>/<tid>/{stat-state,wchan,syscall} line. `tag` flags whether
        // this is a tracee (guest) or the supervisor's OWN tracer thread — the latter
        // reveals WHY a tracee isn't resumed: supervisor wchan/syscall = a blocking pread
        // on /proc/<dead-tid>/mem (candidate c) vs parked in waitpid (mis-resume, d/e).
        auto dump_one = [](const char* dir, const char* tid, const char* tag) {
            char p[96];
            char stat_s[16] = "?", wchan_s[64] = "?", sysc_s[80] = "?";
            std::snprintf(p, sizeof(p), "%s/%s/stat", dir, tid);
            if (int fd = ::open(p, O_RDONLY | O_CLOEXEC); fd >= 0) {
                char buf[256] = {0};
                if (::read(fd, buf, sizeof(buf) - 1) > 0) {
                    const char* rp = std::strrchr(buf, ')');  // state = char after "pid (comm) "
                    if (rp && rp[1] && rp[2]) { stat_s[0] = rp[2]; stat_s[1] = '\0'; }
                }
                ::close(fd);
            }
            std::snprintf(p, sizeof(p), "%s/%s/wchan", dir, tid);
            if (int fd = ::open(p, O_RDONLY | O_CLOEXEC); fd >= 0) {
                ssize_t n = ::read(fd, wchan_s, sizeof(wchan_s) - 1);
                if (n > 0) wchan_s[n] = '\0';
                ::close(fd);
            }
            std::snprintf(p, sizeof(p), "%s/%s/syscall", dir, tid);
            if (int fd = ::open(p, O_RDONLY | O_CLOEXEC); fd >= 0) {
                ssize_t n = ::read(fd, sysc_s, sizeof(sysc_s) - 1);
                if (n > 0) { sysc_s[n] = '\0'; if (char* nl = std::strchr(sysc_s, '\n')) *nl = '\0'; }
                ::close(fd);
            }
            __android_log_print(ANDROID_LOG_WARN, "alr_loader",
                                "alr sup-stall %s tid=%s state=%s wchan=%s syscall=%s",
                                tag, tid, stat_s, wchan_s, sysc_s);
        };
        char taskdir[64];
        std::snprintf(taskdir, sizeof(taskdir), "/proc/%d/task", static_cast<int>(leader_pid));
        if (DIR* d = ::opendir(taskdir)) {
            while (struct dirent* e = ::readdir(d)) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                dump_one(taskdir, e->d_name, "guest");
            }
            ::closedir(d);
        }
        // The supervisor's OWN tracer thread (same process = /proc/self) — pinpoints the
        // wedge: if it is in pread/process_vm_readv it is blocked on a dead tid's mem.
        char sup_tid_s[16];
        std::snprintf(sup_tid_s, sizeof(sup_tid_s), "%d", static_cast<int>(sup_tid));
        dump_one("/proc/self/task", sup_tid_s, "SUPERVISOR");
        // The recent-event ring + guard activity: the EXACT decision sequence before the
        // freeze. event: 0=seccomp-path/exec done elsewhere; for EVENT_STOP we record the
        // raw `event` (128) and stopsig. Decode tid|event|stopsig per slot.
        __android_log_print(ANDROID_LOG_WARN, "alr_loader",
                            "alr sup-stall guard_fires=%u guard_ints=%u ev_total=%u",
                            guard_fires.load(), guard_ints.load(), ev_head.load());
        const uint32_t head = ev_head.load(std::memory_order_acquire);
        const uint32_t n = head < 48 ? head : 48;
        char line[512];
        int off = 0;
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t slot = (head - n + k) % 48;
            const uint32_t v = ev_ring[slot];
            off += std::snprintf(line + off, sizeof(line) - off, "%u:e%u/s%u ",
                                 (v >> 16) & 0xffff, (v >> 8) & 0xff, v & 0xff);
            if (off > static_cast<int>(sizeof(line)) - 24) break;
        }
        __android_log_print(ANDROID_LOG_WARN, "alr_loader",
                            "alr sup-stall ev_ring [tid:eEVENT/sSTOPSIG] %s", line);
        ::kill(-leader_pid, SIGKILL);
        ::kill(leader_pid, SIGKILL);
    });
    bool sigtrap_fwd_logged = false;  // one-shot: log the first genuine forwarded SIGTRAP
    // Multi-tracee supervisor: waitpid(-1, __WALL|__WNOTHREAD) catches the guest plus
    // every thread it clones and every process it forks/execs. Each blocked syscall
    // (SIGSYS) is emulated per-tracee; only blocked syscalls trap, so overhead
    // stays far below PRoot's trap-every-syscall model.
    //
    // === CONCURRENT-SUPERVISOR CROSS-TALK FIX (__WNOTHREAD) — device-root-caused ===
    // build_native_loader_probe() runs once per nativeAlrNativeLoaderProbe JNI call,
    // and MainActivity fires MANY of these concurrently (the onCreate probe sequence
    // + the aptdrain dpkg -i, each on its own Java thread). Every such call forks its
    // own guest IN ITS OWN THREAD and runs THIS loop. All those supervisor threads
    // live in ONE process, so a bare waitpid(-1) from supervisor A also matches
    // supervisor B's guest: a tracee's child-state-change is visible to every thread
    // of the tracer's thread group via the natural-children traversal, even though
    // the PTRACE_CONT can only be issued by the actual tracer thread. The device
    // signature was decisive (galculator dpkg, two live supervisors 19614/19615):
    //   sup=19615 seccomp tid=19708 ... (146×, all CONT rc=0)   <- 19708 IS 19615's tracee
    //   sup=19614 seccomp-CONT tid=19708 rc=-1 errno=3 (ESRCH)  <- 19614 STOLE the stop
    // Supervisor 19614 dequeued 19708's seccomp-stop, ran path-mediation on it, then
    // PTRACE_CONT'd it -> ESRCH (not 19614's tracee). The stop was CONSUMED, so the
    // real tracer 19615's waitpid NEVER saw it: 19708 sat state=t (ptrace-stopped)
    // forever, 19615 blocked in wait4, the 40s watchdog SIGKILLed it. galculator's
    // deep dpkg fork tree (dpkg->dpkg-deb/dpkg-split/tar/sh, each re-mapped in-process
    // -> a high path-trap rate -> a high collision probability) hit this every run;
    // chromium multiprocess is the same class (many tids racing two supervisors).
    // FIX: __WNOTHREAD restricts each waitpid to children of the CALLING THREAD only.
    // ptrace reparents a tracee's ->parent to its tracer thread, so a tracer still
    // reaps ALL of its own tracees (the forked guest + every PTRACE_O_TRACE{FORK,CLONE,
    // VFORK}-attached descendant — their ->parent IS this thread) while it can no
    // longer dequeue another supervisor thread's tracee. Single-supervisor guests
    // (GIMP, glmark2, chromium --version run as the only forker on their thread) are
    // byte-unaffected: their only children are their own tracees, which __WNOTHREAD
    // still matches. This is the "supervisor 멀티스레드 ptrace 벽" — the shared
    // serialized-supervision wall both galculator and chromium MP were stuck behind.
    while (true) {
        // === ADR-003-v3 §2.1 PRIMARY FIX: never block forever on an all-parked group ===
        // A group-stop is parked with PTRACE_LISTEN (below) and only re-reports when the
        // group-stop ENDs — which, for a headless in-process guest, requires a SIGCONT
        // that no one ever sends. If EVERY live tracee is LISTEN-parked, no waitpid event
        // can ever arrive → the supervisor blocks in wait4 forever (device-confirmed:
        // SUPERVISOR state=S wchan=do_wait while the guest sits state=t LISTEN-parked).
        // PTRACE_INTERRUPT forces a parked tracee to re-report as a PTRACE_EVENT_STOP even
        // while the group-stop persists (and works for ANY stop type, unlike SIGCONT which
        // only clears job-control stops — which is why the earlier SIGCONT cure failed for
        // dpkg-query). The re-report is then CONT-resumed by the group-stop-END handling
        // below. This fires ONLY when provably wedged (all live tids parked), so it adds
        // no overhead to a healthy guest and cannot re-stop a running one. (A partial park
        // with a futex-blocked sibling is the bounded residual the stall watchdog covers.)
        if (!known_tids.empty() && listening_tids.size() == known_tids.size()) {
            guard_fires.fetch_add(1, std::memory_order_relaxed);
            for (const pid_t lt : listening_tids) {
                ::ptrace(PTRACE_INTERRUPT, lt, nullptr, nullptr);  // ESRCH benign
                guard_ints.fetch_add(1, std::memory_order_relaxed);
            }
        }
        int status = 0;
        const pid_t w = ::waitpid(-1, &status, __WALL | __WNOTHREAD);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;  // ECHILD: every tracee has been reaped
        }
        if (WIFEXITED(status)) {
            mem_fd_evict(w);  // tid reaped: close its cached /proc/<tid>/mem fd
            known_tids.erase(w);      // drop from the live set (§2.1 all-parked guard)
            listening_tids.erase(w);  // a parked tid that died is no longer parked
            if (w == pid) {
                code = WEXITSTATUS(status);
            }
            continue;
        }
        if (WIFSIGNALED(status)) {
            mem_fd_evict(w);  // tid killed: close its cached /proc/<tid>/mem fd
            known_tids.erase(w);
            listening_tids.erase(w);
            if (w == pid) {
                sig = WTERMSIG(status);
            }
            continue;
        }
        if (!WIFSTOPPED(status)) {
            continue;
        }
        const int stopsig = WSTOPSIG(status);
        const int event = status >> 16;
        {
            const uint32_t i = ev_head.fetch_add(1, std::memory_order_relaxed);
            ev_ring[i % 48] = (static_cast<uint32_t>(w & 0xffff) << 16) |
                              (static_cast<uint32_t>(event & 0xff) << 8) |
                              static_cast<uint32_t>(stopsig & 0xff);
        }
        if (event == PTRACE_EVENT_SECCOMP) {
            // Path-mediation: the guest issued an openat-family syscall (RET_TRACE
            // by our stacked filter). The tracee is frozen at syscall-entry, before
            // the kernel dereferences the path, so we rewrite x1 (the pathname) to
            // its rootfs host location, TOCTOU-safe, into a per-thread stack scratch.
            ++path_traps;
            uint64_t regs[34] = {0};
            struct iovec io{regs, sizeof(regs)};
            if (::ptrace(PTRACE_GETREGSET, w, reinterpret_cast<void*>(NT_PRSTATUS), &io) == 0) {
                // aarch64 carries the syscall nr in x8 at the seccomp-entry stop. In
                // PCGATE=1 the loader also RET_TRACEs execve/execveat, so those traps
                // reach this handler too. The 9 path syscalls put the pathname in x1,
                // but execve(path,argv,envp) puts it in x0 (x1=argv char**, x2=envp);
                // rewriting x1 for an exec would corrupt argv. So exec takes a SEPARATE
                // branch below (ADR-003 §3 (B-1)) that reads x0 and never touches
                // x1/x2. The non-exec path-family branch is unchanged.
                const uint64_t sysno = regs[8];
                ++trace_hist[static_cast<int>(sysno)];  // M-R2 storm decomposition
                const bool is_exec =
                    (sysno == static_cast<uint64_t>(__NR_execve) ||
                     sysno == static_cast<uint64_t>(__NR_execveat));
                const uintptr_t path_addr = static_cast<uintptr_t>(regs[1]);
                if (!is_exec && path_addr != 0) {
                    // Cached O_RDWR /proc/<tid>/mem fd (opened lazily, reused across
                    // this tid's traps), replacing the baseline's per-trap open+close.
                    int mfd = mem_fd_for(w);
                    if (mfd >= 0) {
                        char gp[512] = {0};
                        ssize_t got =
                            ::pread(mfd, gp, sizeof(gp) - 1, static_cast<off_t>(path_addr));
                        // Stale-fd recovery: if the cached fd's pread fails with an
                        // errno that means the fd no longer maps this tid's address
                        // space (the tracee re-exec'd, was reaped+reused, or the fd
                        // went bad), evict and reopen ONCE so this trap behaves
                        // exactly like the baseline's always-fresh per-trap open.
                        if (got < 0 && (errno == ESRCH || errno == EIO || errno == EBADF)) {
                            mem_fd_evict(w);
                            mfd = mem_fd_for(w);
                            if (mfd >= 0) {
                                got = ::pread(mfd, gp, sizeof(gp) - 1,
                                              static_cast<off_t>(path_addr));
                            }
                        }
                        if (got > 0) {
                            gp[got] = '\0';
                            // Only mediate absolute guest paths; leave relative
                            // paths (resolved against the guest cwd) untouched.
                            // Kernel virtual filesystems (/proc, /sys, /dev) are
                            // NOT rewritten into the rootfs — they have no rootfs
                            // backing and need synthesis (a later interposer pass);
                            // leaving them native keeps e.g. /proc/self/exe valid.
                            auto under = [](const char* p, const char* d) {
                                const std::size_t n = ::strlen(d);
                                return ::strncmp(p, d, n) == 0 && (p[n] == '/' || p[n] == '\0');
                            };
                            const bool sysdir = under(gp, "/proc") ||
                                                under(gp, "/sys") || under(gp, "/dev");
                            // Idempotency guard: never re-translate a path the guest
                            // already presented as a rootfs host path (e.g. learned
                            // from /proc/self/maps, or rewritten by a future
                            // LD_PRELOAD interposer) — else we'd produce
                            // <rootfs><rootfs>/… and the open would fail.
                            const bool already_host =
                                under(gp, config.rootfs_dir.c_str());
                            if (gp[0] == '/' && !sysdir && !already_host) {
                                // translate_rootfs_path is pure in (rootfs_dir, cwd,
                                // path); rootfs_dir and cwd are fixed for the run, so
                                // a repeated guest path yields an identical host path.
                                // Memoize guest_path -> host_path; a hit reuses the
                                // exact same string the translate call would return.
                                auto cit = xlate_cache.find(gp);
                                if (cit == xlate_cache.end()) {
                                    const auto t = alr::runtime::translate_rootfs_path(
                                        config.rootfs_dir, "/", gp);
                                    // Bounded: rootfs paths are stable for a run, so
                                    // wholesale clearing at the cap (rather than per-
                                    // entry eviction) is correct and keeps it simple.
                                    if (xlate_cache.size() >= kXlateCacheCap) {
                                        xlate_cache.clear();
                                    }
                                    cit = xlate_cache.emplace(gp, t.host_path).first;
                                }
                                const std::string& host = cit->second;
                                const uintptr_t sp = static_cast<uintptr_t>(regs[31]);
                                const uintptr_t scratch =
                                    (sp - 2048) & ~static_cast<uintptr_t>(0xf);
                                ssize_t wr = ::pwrite(mfd, host.c_str(),
                                                      host.size() + 1,
                                                      static_cast<off_t>(scratch));
                                // Same stale-fd recovery as the read: a cached fd that
                                // went bad between read and write is evicted+reopened
                                // once, so the write is as robust as the baseline's
                                // fresh per-trap fd.
                                if (wr < 0 &&
                                    (errno == ESRCH || errno == EIO || errno == EBADF)) {
                                    mem_fd_evict(w);
                                    mfd = mem_fd_for(w);
                                    if (mfd >= 0) {
                                        wr = ::pwrite(mfd, host.c_str(), host.size() + 1,
                                                      static_cast<off_t>(scratch));
                                    }
                                }
                                if (wr == static_cast<ssize_t>(host.size() + 1)) {
                                    regs[1] = scratch;
                                    if (::ptrace(PTRACE_SETREGSET, w,
                                                 reinterpret_cast<void*>(NT_PRSTATUS),
                                                 &io) == 0) {
                                        ++path_rewrites;
                                        if (first_rewrite.empty()) {
                                            first_rewrite = std::string(gp) + "=>" + host;
                                        }
                                    }
                                }
                            }
                        }
                        // NB: no per-trap ::close(mfd) — the fd is cached and closed
                        // when the tid exits (WIFEXITED/WIFSIGNALED) or is evicted on
                        // a stale-fd error above.
                    }
                }
                // === ADR-003 §3 (B-1): execve/execveat program-path mediation ===
                // A guest fork()+execve()'d a rootfs binary (chromium zygote/gpu/
                // utility; apt/dpkg helpers; GIMP plugins). The tracee is frozen at
                // syscall-entry, so we rewrite the PROGRAM PATH (x0 for execve; x1 for
                // execveat(dirfd, path, argv, envp, flags)) into its rootfs
                // host location — TOCTOU-safe — exactly like the path-family branch but
                // reading the EXEC-correct register and NEVER touching argv/envp. The
                // kernel then loads the rootfs ELF, which (man seccomp.2 / ptrace.2)
                // keeps the new image under the SAME stacked seccomp filter + SEIZE
                // trace, so no loader re-map is needed; PTRACE_EVENT_EXEC (below) just
                // re-validates the fd cache. argv (and argv[0]) stay byte-unchanged.
                else if (is_exec) {
                    ++exec_traps;
                    // ADR-003-v3 in-process re-map redirect is at the END of this branch
                    // (after the gp/med read), so it has the host target path + final say
                    // on the regs. See the STEP-2 block before this branch's close.
                    // execve: path=x0. execveat: path=x1 (dirfd=x0). Pick the path reg
                    // by syscall nr so argv/envp are never mistaken for the path.
                    const bool is_at =
                        (sysno == static_cast<uint64_t>(__NR_execveat));
                    const uintptr_t exec_path_addr =
                        is_at ? static_cast<uintptr_t>(regs[1])
                              : static_cast<uintptr_t>(regs[0]);
                    if (exec_path_addr != 0) {
                        int mfd = mem_fd_for(w);
                        if (mfd >= 0) {
                            char gp[512] = {0};
                            ssize_t got = ::pread(mfd, gp, sizeof(gp) - 1,
                                                  static_cast<off_t>(exec_path_addr));
                            if (got < 0 &&
                                (errno == ESRCH || errno == EIO || errno == EBADF)) {
                                mem_fd_evict(w);
                                mfd = mem_fd_for(w);
                                if (mfd >= 0) {
                                    got = ::pread(mfd, gp, sizeof(gp) - 1,
                                                  static_cast<off_t>(exec_path_addr));
                                }
                            }
                            if (got > 0) {
                                gp[got] = '\0';
                                // Pure, host-tested classifier (alr_exec.cpp): decides
                                // whether to rewrite and to what, with the SAME sysdir/
                                // already-host exclusions as the path-family branch.
                                const auto med =
                                    alr::runtime::decide_exec_path_mediation(
                                        config.rootfs_dir, gp);
                                if (first_exec_x0.empty()) {
                                    first_exec_x0 = gp;
                                    first_exec_reason = med.reason;
                                }
                                // === ADR-003-v3 STEP 2: in-process re-map (NO execve) ===
                                // Cancel the (W^X-forbidden) execve and PC-redirect the
                                // tracee into the resident map+jump trampoline, which maps
                                // the target ELF + guest ld.so in-process and jumps —
                                // inheriting the seccomp filter + SEIZE, no kernel exec.
                                // ABI: x19=host target path, x20=argv, x21=envp,
                                // x22=rootfs (callee-saved -> survive the syscall-skip).
                                // Strings go in the [sp-2048, sp) scratch (below sp, proven
                                // writable; the B-3 envp window is BELOW sp-2048). Critical
                                // regs (x19-x22, pc) and the syscall-nr regset are not
                                // touched by the splice/B-1/B-3 SETREGSETs that follow, so
                                // this redirect survives them — EXCEPT that B-3 rebuilds the
                                // envp and re-points x2; since the worker reads envp from x21
                                // (a snapshot of x2 taken HERE), B-3 must also re-point x21 at
                                // the augmented array or the in-process guest runs WITHOUT the
                                // LD_PRELOAD interposer/fakeroot (the data-extract child then
                                // writes *.dpkg-new to bare Android fs -> unpack/configure
                                // fails). We track the redirect with this flag so B-3 can
                                // mirror its envp re-point into x21 (G1 fix).
                                bool inproc_redirected_this_trap = false;
#if defined(__aarch64__)
                                // G1 seqint: SCOPE the inproc redirect. The trampoline can
                                // only map a rootfs glibc target; redirecting everything
                                // wedges the serialized supervision (device drain: onCreate
                                // probe sequence stalled ~9s on the chromium zygote's
                                // /proc/self/exe exec, whose PT_INTERP=/system/bin/linker64
                                // is the ANDROID binary the trampoline cannot map into the
                                // Debian rootfs). Decide whether to skip BEFORE writing regs;
                                // on skip we fall through to the existing B-1/B-3 behavior
                                // (no redirect), exactly as if inproc were off for this exec.
                                const char* inproc_skip_reason = nullptr;
                                // GATE-1 (CR-5): chromium's zygote/gpu/utility children
                                // re-exec their OWN binary via "/proc/self/exe" (or
                                // "/proc/<pid>/exe"). The old code SKIPPED every "/proc/*"
                                // -> the fresh-execve child ran the ANDROID linker64 image,
                                // never re-mapped into the rootfs. We now SUBSTITUTE the
                                // launch guest's rootfs host path (host_path, L1507 —
                                // already in-scope here, all chromium children re-exec the
                                // SAME chrome binary) for those self-exe forms and fall into
                                // the existing re-map path below (regs[19]=host). Non-exe
                                // "/proc/*" (maps/cpuinfo/self/root/...) are NOT exec targets
                                // and keep the conservative SKIP. host model + decision
                                // table: tools/proc_self_exe_model.py (is_self_exe_target /
                                // decide_exec_trap), tests/test_proc_self_exe_gate.py.
                                bool self_exe_subst = false;
                                if (inproc_reexec_on) {
                                    // (a) self-exe exec target: "/proc/self/exe" or
                                    // "/proc/<all-digits>/exe". Only these forms name "this
                                    // process's own binary" — divert them to the rootfs
                                    // chrome via host_path; any OTHER /proc/* keeps skipping.
                                    bool is_self_exe = (std::strcmp(gp, "/proc/self/exe") == 0);
                                    if (!is_self_exe && std::strncmp(gp, "/proc/", 6) == 0) {
                                        const char* d = gp + 6;          // after "/proc/"
                                        const char* p = d;
                                        while (*p >= '0' && *p <= '9') ++p;  // span digits
                                        // "/proc/<digits>/exe" with >=1 digit and exact tail.
                                        if (p != d && std::strcmp(p, "/exe") == 0) {
                                            is_self_exe = true;
                                        }
                                    }
                                    if (is_self_exe) {
                                        if (!host_path.empty()) {
                                            // SUBSTITUTE: leave skip_reason null so we fall
                                            // into the re-map path; the host-selection below
                                            // reads self_exe_subst to use host_path (not gp).
                                            self_exe_subst = true;
                                            // CR-4 instrumentation: read the child's
                                            // argv[1..] to recover its --type=, and sample
                                            // this process's RSS, so a device drain can see
                                            // WHICH chromium child type recurs and how RSS
                                            // climbs per re-map. argv reg: execve=x1,
                                            // execveat=x2 (already chosen by is_at below).
                                            std::string child_type = "?";
                                            {
                                                const uintptr_t av =
                                                    is_at
                                                        ? static_cast<uintptr_t>(regs[2])
                                                        : static_cast<uintptr_t>(regs[1]);
                                                // Scan up to a few argv entries for "--type=".
                                                for (int ai = 1; av && ai < 8; ++ai) {
                                                    uintptr_t sp_ptr = 0;
                                                    if (::pread(mfd, &sp_ptr,
                                                                sizeof(sp_ptr),
                                                                static_cast<off_t>(
                                                                    av + ai * sizeof(
                                                                        uintptr_t))) !=
                                                        static_cast<ssize_t>(
                                                            sizeof(sp_ptr)) ||
                                                        sp_ptr == 0) {
                                                        break;
                                                    }
                                                    char ab[64] = {0};
                                                    if (::pread(mfd, ab, sizeof(ab) - 1,
                                                                static_cast<off_t>(
                                                                    sp_ptr)) > 0 &&
                                                        std::strncmp(ab, "--type=",
                                                                     7) == 0) {
                                                        child_type = ab + 7;
                                                        break;
                                                    }
                                                }
                                            }
                                            if (first_self_exe_type.empty()) {
                                                first_self_exe_type = child_type;
                                            }
                                            // Sample RSS (KiB) from /proc/self/statm
                                            // field 2 (resident pages) * page size.
                                            long rss_mib = -1;
                                            {
                                                int sfd = ::open("/proc/self/statm",
                                                                 O_RDONLY | O_CLOEXEC);
                                                if (sfd >= 0) {
                                                    char sb[128] = {0};
                                                    ssize_t sn = ::read(sfd, sb,
                                                                        sizeof(sb) - 1);
                                                    ::close(sfd);
                                                    if (sn > 0) {
                                                        long total_pg = 0, res_pg = 0;
                                                        if (std::sscanf(sb, "%ld %ld",
                                                                        &total_pg,
                                                                        &res_pg) == 2) {
                                                            rss_mib =
                                                                (res_pg *
                                                                 (long)::sysconf(
                                                                     _SC_PAGESIZE)) /
                                                                (1024 * 1024);
                                                        }
                                                    }
                                                }
                                            }
                                            // CR-4 runaway guard: past the cap, refuse the
                                            // in-process re-map (fall back to B-1) so a
                                            // crash-retry storm is bounded, not OOM.
                                            if (self_exe_remaps >= self_exe_remap_cap) {
                                                inproc_skip_reason = "selfexe-cap";
                                                self_exe_subst = false;
                                                if (!self_exe_cap_logged) {
                                                    self_exe_cap_logged = true;
                                                    __android_log_print(
                                                        ANDROID_LOG_WARN, "alr_loader",
                                                        "alr CR4 self-exe re-map CAP hit "
                                                        "(%d>=%d) type=%s rss=%ldMiB "
                                                        "tid=%d — refusing further "
                                                        "re-maps (runaway guard)",
                                                        self_exe_remaps,
                                                        self_exe_remap_cap,
                                                        child_type.c_str(), rss_mib,
                                                        static_cast<int>(w));
                                                }
                                            } else {
                                                __android_log_print(
                                                    ANDROID_LOG_INFO, "alr_loader",
                                                    "alr CR4 self-exe re-map #%d "
                                                    "type=%s rss=%ldMiB tid=%d",
                                                    self_exe_remaps + 1,
                                                    child_type.c_str(), rss_mib,
                                                    static_cast<int>(w));
                                            }
                                        } else {
                                            // Launch guest unknown -> conservative SKIP.
                                            inproc_skip_reason = "proc-self-exe";
                                        }
                                    } else if (std::strncmp(gp, "/proc/", 6) == 0) {
                                        // non-exe /proc/* (not an exec target) -> SKIP.
                                        inproc_skip_reason = "proc-self-exe";
                                    }
                                    // (c) idempotency: never re-map the static re-entry stub.
                                    // GATE-1: a self-exe SUBSTITUTE already resolved the
                                    // target to the rootfs chrome (host_path), so it must
                                    // bypass the stub/non-rootfs gates below — they classify
                                    // `gp` (=/proc/self/exe), which is neither the stub nor a
                                    // rootfs path, and would otherwise re-skip the very exec
                                    // GATE-1 just diverted (device drain v163: skip reason was
                                    // "non-rootfs" because med.reason=="sysdir" for /proc/*).
                                    // Mirrors proc_self_exe_model.decide_exec_trap returning
                                    // SUBSTITUTE *before* the stub/non-rootfs checks.
                                    if (inproc_skip_reason == nullptr && !self_exe_subst) {
                                        const char* gpb = std::strrchr(gp, '/');
                                        if ((gpb != nullptr &&
                                             std::strcmp(gpb, "/alr-reentry") == 0) ||
                                            std::strcmp(gp, "alr-reentry") == 0) {
                                            inproc_skip_reason = "stub";
                                        }
                                    }
                                    // (b) only re-map a real rootfs binary: the resolved host
                                    // target must be under config.rootfs_dir. The pure
                                    // classifier says so iff it asked to rewrite (reason
                                    // "rewrite" -> host_path under rootfs) or the guest path
                                    // is ALREADY under rootfs (reason "already-host"). Any
                                    // other case (relative/empty/sysdir/native) is non-rootfs.
                                    if (inproc_skip_reason == nullptr && !self_exe_subst &&
                                        !med.should_rewrite &&
                                        med.reason != "already-host") {
                                        inproc_skip_reason = "non-rootfs";
                                    }
                                    if (inproc_skip_reason != nullptr) {
                                        ++exec_inproc_skipped;
                                        if (first_inproc_skip_reason.empty()) {
                                            first_inproc_skip_reason = inproc_skip_reason;
                                            first_inproc_skip_target = gp;
                                        }
                                    }
                                }
                                if (inproc_reexec_on && inproc_skip_reason == nullptr) {
                                    // GATE-1: a self-exe target ("/proc/self/exe" etc.)
                                    // re-maps the launch guest's rootfs chrome (host_path),
                                    // NOT the literal /proc path (med.should_rewrite is false
                                    // for it, so the old ternary would have kept gp).
                                    const std::string host =
                                        self_exe_subst
                                            ? host_path
                                            : (med.should_rewrite ? med.host_path
                                                                  : std::string(gp));
                                    const std::string& rootfs = config.rootfs_dir;
                                    const uintptr_t sp =
                                        static_cast<uintptr_t>(regs[31]);
                                    const uintptr_t hp_addr =
                                        (sp - 2048) & ~static_cast<uintptr_t>(0xf);
                                    const uintptr_t rf_addr =
                                        (hp_addr + host.size() + 16) &
                                        ~static_cast<uintptr_t>(0xf);
                                    if (rf_addr + rootfs.size() + 1 < sp - 16) {
                                        const ssize_t w1 = ::pwrite(
                                            mfd, host.c_str(), host.size() + 1,
                                            static_cast<off_t>(hp_addr));
                                        const ssize_t w2 = ::pwrite(
                                            mfd, rootfs.c_str(), rootfs.size() + 1,
                                            static_cast<off_t>(rf_addr));
                                        if (w1 == static_cast<ssize_t>(host.size() + 1) &&
                                            w2 == static_cast<ssize_t>(
                                                      rootfs.size() + 1)) {
                                            regs[19] = hp_addr;
                                            regs[20] = is_at
                                                ? static_cast<uint64_t>(regs[2])
                                                : static_cast<uint64_t>(regs[1]);
                                            regs[21] = is_at
                                                ? static_cast<uint64_t>(regs[3])
                                                : static_cast<uint64_t>(regs[2]);
                                            regs[22] = rf_addr;
                                            regs[32] = static_cast<uint64_t>(
                                                reinterpret_cast<uintptr_t>(
                                                    &alr_inproc_reexec_trampoline));
                                            if (::ptrace(PTRACE_SETREGSET, w,
                                                         reinterpret_cast<void*>(
                                                             NT_PRSTATUS),
                                                         &io) == 0) {
                                                int newsys = -1;
                                                struct iovec sio{&newsys,
                                                                 sizeof(newsys)};
                                                if (::ptrace(
                                                        PTRACE_SETREGSET, w,
                                                        reinterpret_cast<void*>(
                                                            NT_ARM_SYSTEM_CALL),
                                                        &sio) == 0) {
                                                    ++exec_inproc_redirected;
                                                    inproc_redirected_this_trap = true;
                                                    // CR-4: count only self-exe
                                                    // substitutions toward the runaway
                                                    // cap (a legit rootfs-binary exec is
                                                    // not part of the chromium child
                                                    // storm and must not be throttled).
                                                    if (self_exe_subst) {
                                                        ++self_exe_remaps;
                                                    }
                                                    if (first_reentry_target
                                                            .empty()) {
                                                        first_reentry_target = host;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
#endif
                                // === ADR-003-v2 (Option S): re-entry-stub splice ===
                                // Prefer running the static re-entry stub (which the
                                // kernel CAN execve, then re-maps the glibc target
                                // in-process) over the plain B-1 x0 rewrite (which the
                                // kernel can't complete for a glibc ELF -> exec_events=0).
                                // We splice argv=[stub, target_host, orig argv1..] and
                                // point x0/argv at it. Idempotent: never re-splice the
                                // stub itself. Touches only x0/argv (and argv strings in
                                // the sp-2048 scratch); x2/x3 envp is left for the B-3
                                // block below; the B-3 envp window lives BELOW sp-2048.
                                const std::string stub_host =
                                    config.rootfs_dir +
                                    "/usr/lib/androlinux/alr-reentry";
                                const char* gp_base = std::strrchr(gp, '/');
                                const bool already_stub =
                                    (std::string(gp) == stub_host) ||
                                    (gp_base != nullptr &&
                                     std::strcmp(gp_base, "/alr-reentry") == 0);
                                bool spliced = false;
                                if (exec_reentry_on && !already_stub) {
                                    const std::string target_host =
                                        med.should_rewrite ? med.host_path
                                                           : std::string(gp);
                                    const uintptr_t argv_addr =
                                        is_at ? static_cast<uintptr_t>(regs[2])
                                              : static_cast<uintptr_t>(regs[1]);
                                    // Read the guest's original argv pointer array
                                    // (bounded, NULL-terminated). New argv =
                                    // [stub, target, orig argv[1..]]; the guest's own
                                    // argv[0] is dropped (the re-mapped guest sees
                                    // argv[0]=target — argv[0] multicall is a follow-up).
                                    constexpr std::size_t kArgvMax = 512;
                                    std::vector<uintptr_t> orig_argv;
                                    bool argv_ok = (argv_addr != 0);
                                    if (argv_ok) {
                                        orig_argv.reserve(8);
                                        for (std::size_t i = 0; i < kArgvMax; ++i) {
                                            uintptr_t p = 0;
                                            ssize_t r = ::pread(
                                                mfd, &p, sizeof(p),
                                                static_cast<off_t>(
                                                    argv_addr +
                                                    i * sizeof(uintptr_t)));
                                            if (r != static_cast<ssize_t>(sizeof(p))) {
                                                argv_ok = false;
                                                break;
                                            }
                                            orig_argv.push_back(p);
                                            if (p == 0) break;  // NULL terminator
                                        }
                                    }
                                    if (argv_ok && !orig_argv.empty()) {
                                        const uintptr_t sp =
                                            static_cast<uintptr_t>(regs[31]);
                                        // Strings grow UP from sp-2048 toward sp, staying
                                        // below it; the new argv array follows. Same scratch
                                        // B-1 used (mutually exclusive with it).
                                        uintptr_t cur =
                                            (sp - 2048) &
                                            ~static_cast<uintptr_t>(0xf);
                                        const uintptr_t win_end = sp - 64;
                                        uintptr_t stub_str = 0, target_str = 0;
                                        bool blob_ok = true;
                                        auto put = [&](const std::string& s,
                                                       uintptr_t& out) {
                                            const std::size_t n = s.size() + 1;
                                            if (cur + n > win_end) {
                                                blob_ok = false;
                                                return;
                                            }
                                            if (::pwrite(mfd, s.c_str(), n,
                                                         static_cast<off_t>(cur)) !=
                                                static_cast<ssize_t>(n)) {
                                                blob_ok = false;
                                                return;
                                            }
                                            out = cur;
                                            cur = (cur + n + 7) &
                                                  ~static_cast<uintptr_t>(7);
                                        };
                                        put(stub_host, stub_str);
                                        if (blob_ok) put(target_host, target_str);
                                        std::vector<uintptr_t> new_argv;
                                        new_argv.push_back(stub_str);
                                        new_argv.push_back(target_str);
                                        for (std::size_t i = 1; i < orig_argv.size();
                                             ++i) {
                                            new_argv.push_back(orig_argv[i]);
                                        }
                                        if (new_argv.empty() || new_argv.back() != 0) {
                                            new_argv.push_back(0);
                                        }
                                        const uintptr_t arr_base =
                                            (cur + 7) & ~static_cast<uintptr_t>(7);
                                        const std::size_t arr_bytes =
                                            new_argv.size() * sizeof(uintptr_t);
                                        if (blob_ok &&
                                            arr_base + arr_bytes <= win_end) {
                                            bool arr_ok = true;
                                            for (std::size_t i = 0;
                                                 i < new_argv.size(); ++i) {
                                                const uintptr_t v = new_argv[i];
                                                if (::pwrite(
                                                        mfd, &v, sizeof(v),
                                                        static_cast<off_t>(
                                                            arr_base +
                                                            i * sizeof(v))) !=
                                                    static_cast<ssize_t>(
                                                        sizeof(v))) {
                                                    arr_ok = false;
                                                    break;
                                                }
                                            }
                                            if (arr_ok) {
                                                // x0=stub (execve) or x1=stub
                                                // (execveat; abs path -> dirfd x0
                                                // ignored); argv reg -> new array.
                                                if (is_at) {
                                                    regs[1] = stub_str;
                                                    regs[2] = arr_base;
                                                } else {
                                                    regs[0] = stub_str;
                                                    regs[1] = arr_base;
                                                }
                                                if (::ptrace(
                                                        PTRACE_SETREGSET, w,
                                                        reinterpret_cast<void*>(
                                                            NT_PRSTATUS),
                                                        &io) == 0) {
                                                    spliced = true;
                                                    ++exec_reentry_spliced;
                                                    ++exec_rewrites;
                                                    if (first_reentry_target.empty()) {
                                                        first_reentry_target =
                                                            target_host;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                // B-1 fallback (reentry off, or splice could not be
                                // built): plain x0 program-path rewrite into the rootfs.
                                if (!spliced && med.should_rewrite) {
                                    const uintptr_t sp =
                                        static_cast<uintptr_t>(regs[31]);
                                    const uintptr_t scratch =
                                        (sp - 2048) & ~static_cast<uintptr_t>(0xf);
                                    const std::string& host = med.host_path;
                                    ssize_t wr = ::pwrite(mfd, host.c_str(),
                                                          host.size() + 1,
                                                          static_cast<off_t>(scratch));
                                    if (wr < 0 && (errno == ESRCH || errno == EIO ||
                                                   errno == EBADF)) {
                                        mem_fd_evict(w);
                                        mfd = mem_fd_for(w);
                                        if (mfd >= 0) {
                                            wr = ::pwrite(mfd, host.c_str(),
                                                          host.size() + 1,
                                                          static_cast<off_t>(scratch));
                                        }
                                    }
                                    if (wr == static_cast<ssize_t>(host.size() + 1)) {
                                        // Rewrite ONLY the program-path register; argv
                                        // (x1)/envp (x2) for execve, and dirfd (x0)/
                                        // argv (x2)/envp (x3) for execveat, are left
                                        // exactly as the guest set them.
                                        if (is_at) {
                                            regs[1] = scratch;
                                        } else {
                                            regs[0] = scratch;
                                        }
                                        if (::ptrace(PTRACE_SETREGSET, w,
                                                     reinterpret_cast<void*>(NT_PRSTATUS),
                                                     &io) == 0) {
                                            ++exec_rewrites;
                                        }
                                    }
                                }
                                // === ADR-003 §3 (B-3): child envp re-injection ===
                                // The exec'd child inherits the guest's envp ARRAY
                                // verbatim (execve(path,argv,envp): x2; execveat(dirfd,
                                // path,argv,envp,flags): x3). If that envp lacks
                                // LD_PRELOAD=<abs rootfs interpose .so> and ALR_ROOTFS,
                                // the new image's in-process path interposer never loads
                                // and dpkg→sh→dpkg-deb hit the bare Android fs (unpack
                                // fails). So we read the child's envp char** out of
                                // tracee memory, ask the PURE classifier what must be
                                // added/replaced, and — if anything — rebuild an
                                // augmented envp (string blob + new pointer array) in a
                                // scratch window BELOW the program-path scratch, then
                                // point x2/x3 at it. This runs on EVERY exec trap (not
                                // just the first), so it persists across the whole
                                // dpkg→maintainer-script→sh→dpkg-deb chain; a child whose
                                // parent we already injected inherits a satisfied envp
                                // and the classifier returns a no-op (idempotent). Only
                                // the envp register is touched — path/argv are unchanged.
                                {
                                    const uintptr_t envp_addr =
                                        is_at ? static_cast<uintptr_t>(regs[3])
                                              : static_cast<uintptr_t>(regs[2]);
                                    // Read the guest envp pointer array: a NULL-terminated
                                    // char*[] at envp_addr. Bounded to kEnvMax entries so a
                                    // corrupt/huge array cannot run us away; a real dpkg/sh
                                    // envp is ~40-80 entries.
                                    constexpr std::size_t kEnvMax = 512;
                                    std::vector<std::string> env_entries;
                                    bool envp_read_ok = (envp_addr != 0);
                                    if (envp_read_ok) {
                                        env_entries.reserve(64);
                                        for (std::size_t i = 0; i < kEnvMax; ++i) {
                                            uintptr_t p = 0;
                                            ssize_t r = ::pread(
                                                mfd, &p, sizeof(p),
                                                static_cast<off_t>(envp_addr +
                                                                   i * sizeof(uintptr_t)));
                                            if (r != static_cast<ssize_t>(sizeof(p))) {
                                                envp_read_ok = false;
                                                break;
                                            }
                                            if (p == 0) {
                                                break;  // NULL terminator: array end
                                            }
                                            char es[1024] = {0};
                                            ssize_t er = ::pread(mfd, es, sizeof(es) - 1,
                                                                 static_cast<off_t>(p));
                                            if (er <= 0) {
                                                envp_read_ok = false;
                                                break;
                                            }
                                            es[er] = '\0';
                                            env_entries.emplace_back(es);
                                        }
                                    }
                                    if (envp_read_ok) {
                                        const auto inj =
                                            alr::runtime::decide_exec_envp_injection(
                                                config.rootfs_dir, env_entries,
                                                /*fakeroot=*/fakeroot_on);
                                        if (first_exec_envp_reason.empty()) {
                                            first_exec_envp_reason = inj.reason;
                                        }
                                        if (inj.should_inject) {
                                            // Build the rebuilt envp in a scratch window
                                            // BELOW the program-path scratch (sp-2048):
                                            // strings grow up from str_base, then the new
                                            // pointer array follows. The whole window is
                                            // [sp-kEnvScratch, sp-2048); ample for a dpkg
                                            // envp. The program path (if rewritten) lives
                                            // at sp-2048 and is NOT overwritten.
                                            const uintptr_t sp =
                                                static_cast<uintptr_t>(regs[31]);
                                            constexpr uintptr_t kEnvScratch = 128 * 1024;
                                            constexpr uintptr_t kPathScratchTop = 2048;
                                            const uintptr_t str_base =
                                                (sp - kEnvScratch) &
                                                ~static_cast<uintptr_t>(0xf);
                                            const uintptr_t window_end = sp - kPathScratchTop;

                                            // The rebuilt entries: every guest entry
                                            // (skipping a replaced LD_PRELOAD) followed by
                                            // the classifier's add_entries. We write each
                                            // string into the blob, recording its tracee
                                            // address for the new pointer array.
                                            std::vector<uintptr_t> new_ptrs;
                                            new_ptrs.reserve(env_entries.size() +
                                                             inj.add_entries.size() + 1);
                                            uintptr_t cur = str_base;
                                            bool blob_ok = true;
                                            auto write_str = [&](const std::string& s)
                                                -> bool {
                                                const std::size_t n = s.size() + 1;
                                                if (cur + n > window_end) {
                                                    return false;  // window exhausted
                                                }
                                                ssize_t w2 = ::pwrite(
                                                    mfd, s.c_str(), n,
                                                    static_cast<off_t>(cur));
                                                if (w2 != static_cast<ssize_t>(n)) {
                                                    return false;
                                                }
                                                new_ptrs.push_back(cur);
                                                cur += n;
                                                // 8-byte align the next string so the
                                                // trailing pointer array is naturally
                                                // aligned regardless of string lengths.
                                                cur = (cur + 7) &
                                                      ~static_cast<uintptr_t>(7);
                                                return true;
                                            };
                                            // Copy guest entries, dropping the old
                                            // LD_PRELOAD only when we are replacing it
                                            // (prepend case). Otherwise keep all verbatim.
                                            for (const auto& e : env_entries) {
                                                if (inj.replace_ld_preload &&
                                                    e.rfind("LD_PRELOAD=", 0) == 0) {
                                                    continue;  // dropped; re-added below
                                                }
                                                if (!write_str(e)) {
                                                    blob_ok = false;
                                                    break;
                                                }
                                            }
                                            if (blob_ok) {
                                                for (const auto& e : inj.add_entries) {
                                                    if (!write_str(e)) {
                                                        blob_ok = false;
                                                        break;
                                                    }
                                                }
                                            }
                                            // Place the new char*[] array (pointers +
                                            // NULL) after the string blob, 8-byte aligned.
                                            const uintptr_t arr_base =
                                                (cur + 7) & ~static_cast<uintptr_t>(7);
                                            const std::size_t arr_bytes =
                                                (new_ptrs.size() + 1) * sizeof(uintptr_t);
                                            if (blob_ok &&
                                                arr_base + arr_bytes <= window_end) {
                                                bool arr_ok = true;
                                                for (std::size_t i = 0;
                                                     i < new_ptrs.size(); ++i) {
                                                    const uintptr_t v = new_ptrs[i];
                                                    if (::pwrite(mfd, &v, sizeof(v),
                                                                 static_cast<off_t>(
                                                                     arr_base +
                                                                     i * sizeof(v))) !=
                                                        static_cast<ssize_t>(sizeof(v))) {
                                                        arr_ok = false;
                                                        break;
                                                    }
                                                }
                                                const uintptr_t nul = 0;
                                                if (arr_ok &&
                                                    ::pwrite(
                                                        mfd, &nul, sizeof(nul),
                                                        static_cast<off_t>(
                                                            arr_base +
                                                            new_ptrs.size() *
                                                                sizeof(nul))) ==
                                                        static_cast<ssize_t>(
                                                            sizeof(nul))) {
                                                    // Point the envp register at the new
                                                    // array; argv/path are untouched.
                                                    if (is_at) {
                                                        regs[3] = arr_base;
                                                    } else {
                                                        regs[2] = arr_base;
                                                    }
                                                    // G1 fix: if this exec was PC-redirected
                                                    // into the in-process worker, the worker
                                                    // reads envp from x21 (a snapshot of x2
                                                    // taken at redirect time, BEFORE this
                                                    // augmentation). Mirror the new envp into
                                                    // x21 so the re-mapped guest sees the
                                                    // augmented array (LD_PRELOAD interposer +
                                                    // fakeroot + ALR_ROOTFS) — without this the
                                                    // worker runs the un-augmented envp and the
                                                    // dpkg data-extract child writes *.dpkg-new
                                                    // to the bare Android fs (unpack fails).
                                                    if (inproc_redirected_this_trap) {
                                                        regs[21] = arr_base;
                                                    }
                                                    if (::ptrace(
                                                            PTRACE_SETREGSET, w,
                                                            reinterpret_cast<void*>(
                                                                NT_PRSTATUS),
                                                            &io) == 0) {
                                                        ++envp_injected;
                                                        if (!inj.ld_preload_value
                                                                 .empty()) {
                                                            ++ld_preload_set;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            ::ptrace(PTRACE_CONT, w, nullptr, nullptr);
            continue;
        }
        if (event == PTRACE_EVENT_EXEC) {
            // === ADR-003 §3 (B-2): a tracee completed execve into a NEW image ===
            // PTRACE_O_TRACEEXEC reports this stop AFTER the kernel swapped the address
            // space. The cached /proc/<tid>/mem fd now maps a DIFFERENT image, so we
            // MUST evict it (else the next path/exec rewrite would pread/pwrite into the
            // wrong address space). The stale-fd recovery on ESRCH/EIO/EBADF already
            // catches this lazily, but an explicit evict here is the robust, race-free
            // signal ADR-003 §2 (B-2) calls for. The seccomp filter + SEIZE trace are
            // inherited across exec by the kernel, so we simply re-validate state and
            // CONT — no loader re-map, no re-SETOPTIONS. The tid may differ from the
            // exec'ing thread's (a non-leader thread that execs becomes the new leader,
            // adopting the group tid); PTRACE_GETEVENTMSG gives the FORMER tid whose
            // mem fd is now stale, so we evict both.
            ++exec_events;
            unsigned long former = 0;
            if (::ptrace(PTRACE_GETEVENTMSG, w, nullptr, &former) == 0 &&
                static_cast<pid_t>(former) != w) {
                mem_fd_evict(static_cast<pid_t>(former));
            }
            mem_fd_evict(w);
            ::ptrace(PTRACE_CONT, w, nullptr, nullptr);
            continue;
        }
        if (event == PTRACE_EVENT_STOP) {
            // === THE MULTI-THREAD FIX (PTRACE_SEIZE only) ===
            // Under SEIZE, status>>8 == (SIGTRAP | PTRACE_EVENT_STOP<<8) — i.e.
            // event==PTRACE_EVENT_STOP (128), stopsig==SIGTRAP — is reported for THREE
            // distinct situations that classic TRACEME could not tell apart:
            //   (a) a freshly-cloned thread's initial stop,
            //   (b) a GROUP-STOP (the whole thread group was stopped by a job-control
            //       signal: SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU), and
            //   (c) a PTRACE_INTERRUPT-induced stop.
            // The classic-TRACEME deadlock (Chromium, ~22 threads: one thread parked in
            // state 't' forever) was precisely the tracer mistaking a group-stop for a
            // real signal-stop and either forwarding the stop signal (re-stopping the
            // group while a sibling blocks on its futex) or PTRACE_CONT-ing a
            // group-stopped thread (spuriously running it). SEIZE makes the three cases
            // UNAMBIGUOUS via PTRACE_GETSIGINFO:
            //   - a GROUP-STOP has NO siginfo: GETSIGINFO fails with EINVAL. The correct
            //     resume is PTRACE_LISTEN — the thread stays stopped+listening, is NOT
            //     run, and re-reports when the group-stop ends (SIGCONT). NEVER
            //     PTRACE_CONT a group-stop (that is the bug). LISTEN is the whole fix.
            //   - a new-thread initial stop or an INTERRUPT stop HAS siginfo (a SIGTRAP):
            //     GETSIGINFO succeeds; resume normally with PTRACE_CONT(w, 0).
            // A racing tid (exited/exec'd between stop and here) returns ESRCH from
            // GETSIGINFO — treat it like the new-thread case path (the CONT/LISTEN below
            // then no-ops with ESRCH, which we ignore), so we never wedge.
            // FIRST: a PTRACE_EVENT_STOP from a tid we've never seen is a freshly-cloned
            // child's INITIAL stop, NOT a group-stop. GETSIGINFO can also return EINVAL
            // for that initial stop, so classifying purely by siginfo (below) would
            // wrongly LISTEN-park a new worker thread at birth — the Chromium 1-thread
            // hang. A brand-new tid is always resumed (CONT), never parked.
            if (known_tids.insert(w).second) {
                if (w != pid) {
                    ::ptrace(PTRACE_SETOPTIONS, w, nullptr,
                             reinterpret_cast<void*>(kSeizeOpts));
                }
                if (::ptrace(PTRACE_CONT, w, nullptr, nullptr) != 0 && errno == ESRCH) {
                    continue;
                }
                continue;
            }
            // KNOWN tid: now GETSIGINFO unambiguously splits a real GROUP-STOP (no
            // siginfo -> EINVAL -> LISTEN) from a PTRACE_INTERRUPT-stop (has siginfo -> CONT).
            siginfo_t esi{};
            errno = 0;
            const long gsi = ::ptrace(PTRACE_GETSIGINFO, w, nullptr, &esi);
            const bool group_stop = (gsi != 0 && errno == EINVAL);
            if (group_stop) {
                if (listening_tids.find(w) != listening_tids.end()) {
                    // GROUP-STOP END (SIGCONT trailing edge): an already-LISTEN-parked
                    // tid re-reports EVENT_STOP, GETSIGINFO STILL EINVAL. Re-LISTEN here
                    // would re-park it forever (THE chromium deadlock). Resume it to
                    // RUNNING with signal 0 so its futex-waiting siblings can wake.
                    listening_tids.erase(w);
                    if (::ptrace(PTRACE_CONT, w, nullptr, nullptr) != 0 &&
                        errno == ESRCH) {
                        continue;
                    }
                    continue;
                }
                // GROUP-STOP BEGIN (first time): park the thread with LISTEN (do NOT run
                // it) — the single/multi-thread-safe handling. Record it so its END
                // re-report (above) resumes instead of re-parking. ESRCH is benign.
                // GROUP-STOP BEGIN: park with LISTEN (do NOT run it — that was the
                // classic re-stops-the-group bug). It re-reports either when the
                // group-stop naturally ends OR when the all-parked guard at the top of
                // the loop PTRACE_INTERRUPTs it (the §2.1 release path for a headless
                // guest with no SIGCONT source). Recording it in listening_tids drives
                // both that guard and the END-resume branch above. ESRCH is benign.
                listening_tids.insert(w);
                if (::ptrace(PTRACE_LISTEN, w, nullptr, nullptr) != 0 && errno == ESRCH) {
                    listening_tids.erase(w);
                    continue;
                }
                continue;
            }
            // A known tid that is NOT a group-stop (new-thread/INTERRUPT stop, or a
            // LISTEN-parked tid re-reporting with a non-EINVAL siginfo) is about to be
            // CONT-ed below — it is no longer parked, so drop it from the listening set.
            listening_tids.erase(w);
            // New-thread initial stop (or PTRACE_INTERRUPT): it is already auto-attached
            // and inherits the leader's SEIZE options, so just resume it with signal 0.
            // (We never reach the old TRACEME w!=pid SETOPTIONS dance here — SEIZE option
            // inheritance is atomic — but a single belt-and-suspenders re-apply on a new
            // tid keeps parity with the device-proven defensive code, and is idempotent.)
            if (w != pid && !options_set) {
                ::ptrace(PTRACE_SETOPTIONS, w, nullptr,
                         reinterpret_cast<void*>(kSeizeOpts));
            }
            if (::ptrace(PTRACE_CONT, w, nullptr, nullptr) != 0 && errno == ESRCH) {
                continue;  // tid raced away between stop and resume; just loop
            }
            continue;
        }
        if (event != 0) {
            // A clone/fork/vfork/exec event: the new tracee is auto-attached and
            // inherits the options. Just resume the parent of the event. (event is in
            // 1..6 here; PTRACE_EVENT_STOP==128 was already handled above, so it never
            // falls into this generic resume — which previously would have wrongly
            // PTRACE_CONT'd a group-stop.)
            if (event == PTRACE_EVENT_CLONE) {
                ++guest_threads;
            }
            ::ptrace(PTRACE_CONT, w, nullptr, nullptr);
            continue;
        }
        if (stopsig == SIGTRAP) {
            // A genuine SIGTRAP signal-delivery-stop (event==0 here; every ptrace-mechanism
            // SIGTRAP is a PTRACE_EVENT_*, handled above). chromium/V8 execute a `brk`
            // (IMMEDIATE_CRASH / CHECK / __builtin_trap), which raises SIGTRAP with
            // si_code==TRAP_BRKPT. Suppressing it (resume sig=0) leaves the PC ON the brk
            // instruction → the guest re-executes it → an infinite SIGTRAP storm
            // (device-confirmed: chromium leader looped e0/s5 4434×, never reaped). A
            // genuine trap MUST be delivered: FORWARD it so the guest's own SIGTRAP handler
            // runs (V8/crash reporter) or the default action fires deterministically —
            // progress instead of a livelock. si_code<=0 (SI_USER/SI_QUEUE) or a non-brk
            // code keeps the historical suppress (those are not re-executable instructions).
            siginfo_t tsi{};
            const bool have_si =
                (::ptrace(PTRACE_GETSIGINFO, w, nullptr, &tsi) == 0);
            // A genuine instruction trap (brk/single-step/hw-bkpt) must be FORWARDED — its
            // PC is on a faulting instruction that re-fires if we suppress+resume. A
            // user/queue-sent SIGTRAP (si_code<=0) is not re-executable, so keep suppress.
            // si_code>0 is kernel-generated (TRAP_*, SI_KERNEL); forward those.
            const int fwd = (have_si && tsi.si_code > 0) ? SIGTRAP : 0;
            if (!sigtrap_fwd_logged) {
                sigtrap_fwd_logged = true;
                uint64_t r2[34] = {0};
                struct iovec io2{r2, sizeof(r2)};
                uintptr_t pc = 0;
                if (::ptrace(PTRACE_GETREGSET, w,
                             reinterpret_cast<void*>(NT_PRSTATUS), &io2) == 0) {
                    pc = static_cast<uintptr_t>(r2[32]);  // aarch64 PC
                }
                __android_log_print(ANDROID_LOG_WARN, "alr_loader",
                    "alr SIGTRAP tid=%d si_code=%d pc=%p fwd=%d (was livelock)",
                    static_cast<int>(w), have_si ? tsi.si_code : -999,
                    reinterpret_cast<void*>(pc), fwd);
            }
            if (::ptrace(PTRACE_CONT, w, nullptr,
                         reinterpret_cast<void*>(static_cast<long>(fwd))) != 0 &&
                errno == ESRCH) {
                continue;
            }
            continue;
        }
        if (stopsig == SIGSTOP) {
            // Genuine SIGSTOP signal-delivery-stop (not a group-stop / new-thread stop —
            // those are PTRACE_EVENT_STOP, handled above). NEVER forward SIGSTOP: delivering
            // it would re-stop the whole thread group → the classic deadlock. Suppress
            // (resume sig=0). The idempotent SETOPTIONS re-apply on a non-leader tid is a
            // backstop (SEIZE already inherits options to clones).
            if (w != pid) {
                ::ptrace(PTRACE_SETOPTIONS, w, nullptr,
                         reinterpret_cast<void*>(kSeizeOpts));
            }
            if (::ptrace(PTRACE_CONT, w, nullptr, nullptr) != 0 && errno == ESRCH) {
                continue;  // tid raced away between stop and resume; just loop
            }
            continue;
        }
        if (stopsig == SIGSYS) {
            // Only emulate a seccomp-policy SIGSYS (SYS_SECCOMP). A genuine bad
            // syscall SIGSYS (si_code != SYS_SECCOMP) must be DELIVERED so the
            // guest crashes visibly rather than silently continuing with a faked
            // -ENOSYS once the syscall surface broadens.
            siginfo_t ssi{};
            if (::ptrace(PTRACE_GETSIGINFO, w, nullptr, &ssi) != 0 ||
                ssi.si_code != SYS_SECCOMP) {
                ::ptrace(PTRACE_CONT, w, nullptr,
                         reinterpret_cast<void*>(static_cast<long>(stopsig)));
                continue;
            }
            uint64_t regs[34] = {0};
            struct iovec io{regs, sizeof(regs)};
            if (::ptrace(PTRACE_GETREGSET, w, reinterpret_cast<void*>(NT_PRSTATUS), &io) == 0) {
                const int blocked_nr = static_cast<int>(regs[8]);
                ++emul_hist[blocked_nr];  // M-R2 storm decomposition
                if (emulated_syscalls < 64) {
                    emulated_list[emulated_syscalls] = blocked_nr;
                }
                // CR-4: log the FIRST occurrence of each distinct blocked syscall nr to
                // logcat (bounded), so a device drain of a wedged chromium sees WHICH
                // syscalls are being -ENOSYS'd WITHOUT needing the (never-arriving) final
                // report's emul_hist. A blocked syscall that chromium cannot tolerate
                // (vs glibc's set_robust_list/rseq which ignore the result) is the prime
                // suspect for the GetCollectStatsConsent-then-brk crash. seen[] is a tiny
                // supervisor-thread-local set (no lock); we cap distinct logs at 24.
                {
                    static int seen_nrs[24];
                    static int seen_cnt = 0;
                    bool already = false;
                    for (int si = 0; si < seen_cnt; ++si) {
                        if (seen_nrs[si] == blocked_nr) { already = true; break; }
                    }
                    if (!already && seen_cnt < 24) {
                        seen_nrs[seen_cnt++] = blocked_nr;
                        __android_log_print(ANDROID_LOG_WARN, "alr_loader",
                            "alr CR4 blocked-syscall nr=%d -> ENOSYS (tid=%d, distinct#%d)",
                            blocked_nr, static_cast<int>(w), seen_cnt);
                    }
                }
                // Default: return -ENOSYS. glibc ignores the result of
                // set_robust_list/rseq, but for syscalls it has fallbacks for
                // (e.g. faccessat2 -> faccessat) the fallback keys specifically on
                // ENOSYS; faking success (0) or EPERM would break it.
                // CR-4 EXCEPTION: set_robust_list (aarch64 nr=99). The device drain of a
                // wedged single-process chromium showed nr=99 as the ONLY blocked syscall
                // right before the crash/wedge. glibc's thread setup calls set_robust_list
                // unconditionally and a robust-futex-aware runtime (chromium's
                // PartitionAlloc / base threading) can mis-handle an -ENOSYS here (the
                // kernel normally HAS this syscall, so ENOSYS is an unexpected value that
                // glibc records as "no robust list" but some TLS teardown paths still deref
                // the list head). Faking SUCCESS (0) matches what the real kernel returns
                // and is safe: we are not actually registering a robust list (we cancelled
                // the syscall), but neither glibc nor chromium reads back kernel robust-list
                // state — they only gate on the return code. This removes the one ENOSYS the
                // crash correlates with. (rseq nr=293 and the faccessat2 fallback class keep
                // ENOSYS — only nr=99 is special-cased.)
                regs[0] = (blocked_nr == 99) ? 0u : static_cast<uint64_t>(-38);  // 0 for set_robust_list, else -ENOSYS
                ++emulated_syscalls;
                ::ptrace(PTRACE_SETREGSET, w, reinterpret_cast<void*>(NT_PRSTATUS), &io);
                ::ptrace(PTRACE_CONT, w, nullptr, nullptr);  // suppress SIGSYS
            } else {
                // Couldn't read/patch regs — don't silently swallow the SIGSYS;
                // deliver it so the failure is deterministic.
                ::ptrace(PTRACE_CONT, w, nullptr,
                         reinterpret_cast<void*>(static_cast<long>(stopsig)));
            }
            if (emulated_syscalls > (1 << 20)) {
                // Runaway backstop. Raised from 8192 to ~1M for M-R2 (ADR-002): a
                // syscall-storm guest (chromium) legitimately emulates far more than
                // 8192 blocked nrs, and the alarm() already bounds wall-time, so the
                // lower cap truncated the measured distribution. Still catches a true
                // runaway, just far above any real guest's emulate count.
                ::kill(pid, SIGKILL);
            }
            continue;
        }
        if (!captured && (stopsig == SIGSEGV || stopsig == SIGBUS || stopsig == SIGILL)) {
            uint64_t regs[34] = {0};
            struct iovec io{regs, sizeof(regs)};
            if (::ptrace(PTRACE_GETREGSET, w, reinterpret_cast<void*>(NT_PRSTATUS), &io) == 0) {
                fault_pc = regs[32];  // arm64: regs[0..30], sp=31, pc=32, pstate=33
                fault_lr = regs[30];  // x30/LR = the caller's return address
                fault_x0 = regs[0];
                fault_x8 = regs[8];
            }
            siginfo_t si{};
            if (::ptrace(PTRACE_GETSIGINFO, w, nullptr, &si) == 0) {
                fault_addr = reinterpret_cast<unsigned long long>(si.si_addr);
            }
            fault_signo = stopsig;
            captured = true;
        }
        // Deliver any other signal to the tracee that received it — EXCEPT the
        // four group-stop signals. Under PTRACE_SEIZE a genuine group-stop is now
        // reported as PTRACE_EVENT_STOP and handled with PTRACE_LISTEN above, so this
        // suppression is a DEFENSIVE BACKSTOP: should a SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU
        // ever reach this default path as a signal-delivery-stop, forwarding it would
        // re-stop the whole thread group and a sibling waiting on that thread's futex
        // would block forever (the observed multi-thread hang, thread parked in state
        // 't'). So suppress them: resume with signal 0. SIGSTOP/SIGTRAP never actually
        // reach here (caught above), but listing SIGSTOP keeps the suppress set complete
        // and self-documenting. The single/few-thread guests (GIMP, chromium --version)
        // raise none of these in this branch, so their delivery path is byte-identical.
        const bool group_stop_sig =
            (stopsig == SIGSTOP || stopsig == SIGTSTP ||
             stopsig == SIGTTIN || stopsig == SIGTTOU);
        const long deliver = group_stop_sig ? 0L : static_cast<long>(stopsig);
        // Robust resume: a tid can race away (exit/exec) between its stop and our
        // PTRACE_CONT; ESRCH there is benign — just loop rather than wedge.
        if (::ptrace(PTRACE_CONT, w, nullptr, reinterpret_cast<void*>(deliver)) != 0 &&
            errno == ESRCH) {
            continue;
        }
    }
    // Supervisor finished (ECHILD: all tracees reaped) — tell the stall watchdog to
    // stand down BEFORE it could fire (it no-ops if sup_done, so a healthy guest is
    // never killed/dumped), then join it (≤1s: it polls sup_done each second).
    sup_done.store(true, std::memory_order_release);
    watchdog.join();
    // Supervisor loop exited (ECHILD: all tracees reaped). Per-exit eviction already
    // closed each tid's fd; close any survivors (e.g. a tid lost to the SIGKILL
    // runaway path before its WIFSIGNALED was observed) so no /proc/<tid>/mem fd leaks.
    for (const auto& kv : mem_fds) {
        if (kv.second >= 0) {
            ::close(kv.second);
        }
    }
    mem_fds.clear();
    // Join the concurrent pipe-drain readers (started before the loop). They finish at
    // EOF, which arrives once the guest + every in-process re-mapped child holding the
    // dup'd write end has exited — i.e. right as the loop reaped the last tracee. The
    // strings are then complete; do NOT re-read the pipes (the readers already consumed
    // them to EOF). A reader exception is surfaced via drain_err into diag.
    out_reader.join();
    diag_reader.join();
    ::close(out_pipe[0]);
    ::close(diag_pipe[0]);
    std::string guest_stdout = std::move(guest_stdout_buf);
    std::string diag = std::move(diag_buf);
    if (diag.empty() && !drain_err.empty()) {
        diag = drain_err;
    }
    const bool noarch = diag.find("NOARCH") != std::string::npos;
    const bool mapped = diag.find("MAPPED") != std::string::npos;
    const bool jumped = diag.find("JUMPING") != std::string::npos;
    // Generic success: the guest exited cleanly and produced output. (For
    // /bin/hello this is "hello from static arm64 rootfs"; /bin/mt-test prints
    // main/thread/child ok lines.)
    const bool ran = (code == 0 && !guest_stdout.empty());
    const bool is_dyn = diag.find("DYN;") != std::string::npos;
    const bool interp_ok = diag.find("INTERP:IREL=") != std::string::npos;
    out << "\nALR NATIVE LOADER MAP: " << (noarch ? "SKIP" : mapped ? "PASS" : "FAIL");
    out << "\nALR NATIVE LOADER LINK MODE: " << (is_dyn ? "DYNAMIC(interp-handoff)" : "STATIC");
    out << "\nALR NATIVE LOADER INTERP MAP: " << (!is_dyn ? "SKIP" : interp_ok ? "PASS" : "FAIL");
    out << "\nALR NATIVE LOADER GUEST EXEC: " << (noarch ? "SKIP" : ran ? "PASS" : "FAIL");
    out << "\nalr native loader reached="
        << (jumped ? "jumped-to-entry" : mapped ? "mapped-not-jumped" : "early-fail");
    out << "\nalr native loader child exit=" << code << " signal=" << sig;
    out << "\nalr native loader fault signo=" << fault_signo
        << " pc=0x" << std::hex << fault_pc
        << " addr=0x" << fault_addr
        << " lr=0x" << fault_lr
        << " x0=0x" << fault_x0
        << " x8=0x" << fault_x8 << std::dec
        << " syscall=" << fault_syscall;
    out << "\nalr native loader guest threads spawned=" << guest_threads;
    out << "\nalr native loader path-mediation traps=" << path_traps
        << " rewrites=" << path_rewrites
        << " (pcgate=" << (pcgate_on ? "on" : "off")
        << " interpose=" << (interpose_off ? "off" : "on") << ")";
    if (!first_rewrite.empty()) {
        out << "\nalr native loader first path rewrite=" << first_rewrite;
    }
    // ADR-003 exec re-entry telemetry (M-R4-execmap / M-R4-fork gate). For a guest
    // that never execs (GIMP/foot/chromium --version) all four are 0 and the first-
    // exec lines are omitted, so this is a strict superset add — the no-exec report
    // body is unchanged. clone_events==guest_threads is reported via the existing
    // 'guest threads spawned' line; exec_events>0 means the guest crossed the exec
    // wall (B), exec_events==0 with a forking guest means it stayed fork-only (A).
    out << "\nalr exec traps=" << exec_traps
        << " rewrites=" << exec_rewrites
        << " exec_events=" << exec_events
        << " clone_events=" << guest_threads;
    // ADR-003 §3 (B-3) child envp re-injection telemetry. envp_injected>0 proves
    // the supervisor rebuilt at least one exec'd child's envp so the new image
    // re-enters ALR interpose mediation; ld_preload_set is the subset carrying our
    // LD_PRELOAD. Both 0 for a no-exec guest (strict superset add). A device drain
    // of `apt install` should show envp_injected==exec_rewrites (each rewritten
    // exec also gets envp) and ld_preload_set>0 on the dpkg→sh→dpkg-deb chain.
    out << "\nalr exec envp_injected=" << envp_injected
        << " ld_preload_set=" << ld_preload_set;
    // ADR-003-v2 (Option S) re-entry-stub splice telemetry. reentry=on/off is the
    // ALR_EXEC_REENTRY gate; spliced>0 means at least one exec was rewritten to run
    // via the static stub. The KEY gate for this milestone is exec_events>0 WITH
    // spliced>0 (the static-stub execve completed where the glibc execve could not).
    out << "\nalr exec reentry=" << (exec_reentry_on ? "on" : "off")
        << " spliced=" << exec_reentry_spliced
        << " inproc=" << (inproc_reexec_on ? "on" : "off")
        << " inproc_redirected=" << exec_inproc_redirected
        << " inproc_skipped=" << exec_inproc_skipped;
    // CR-4: self-exe (/proc/self/exe) re-map storm telemetry. self_exe_remaps is the
    // count of chromium children re-mapped from the launch chrome; cap is the runaway
    // bound; first type names the first child kind seen (gpu-process/utility/renderer/
    // zygote). A drain comparing this to the per-#N RSS logs pins the storm shape.
    out << "\nalr CR4 self_exe_remaps=" << self_exe_remaps
        << " cap=" << self_exe_remap_cap
        << " cap_hit=" << (self_exe_cap_logged ? "yes" : "no")
        << " first_type=" << (first_self_exe_type.empty() ? "(none)"
                                                          : first_self_exe_type);
    // G1 seqint: when execs were correctly NOT redirected (e.g. the chromium zygote's
    // /proc/self/exe, or a non-rootfs/native target, or the alr-reentry stub), surface
    // the first reason+target so the WS-1 drain can confirm the scoping before flipping
    // ALR_REEXEC_INPROC default-ON.
    if (!first_inproc_skip_reason.empty()) {
        out << "\nalr exec inproc skip reason=" << first_inproc_skip_reason
            << " target=" << first_inproc_skip_target;
    }
    if (!first_reentry_target.empty()) {
        out << "\nalr exec reentry stub=" << config.rootfs_dir
            << "/usr/lib/androlinux/alr-reentry target=" << first_reentry_target;
    }
    if (!first_exec_x0.empty()) {
        out << "\nalr exec x0=" << first_exec_x0
            << " reason=" << first_exec_reason
            << " envp_reason=" << first_exec_envp_reason;
    }
    out << "\nalr native loader seccomp-emulated syscalls=" << emulated_syscalls;
    if (emulated_syscalls > 0) {
        out << " nums=";
        const int shown = emulated_syscalls < 12 ? emulated_syscalls : 12;
        for (int i = 0; i < shown; ++i) {
            out << emulated_list[i] << (i + 1 < shown ? "," : "");
        }
    }
    // M-R2 (ADR-002) storm decomposition: per-nr histograms (sorted by count desc) +
    // guest CPU/ctxt via getrusage(RUSAGE_CHILDREN) — robust after the guest is reaped
    // (no /proc/<pid>/stat race). bench/syscall_mix.py parses these 'alr sc ...' lines.
    {
        // Dump the busiest TOP_HIST=16 nrs (matches bench/syscall_mix.py's saturation
        // contract: len>=16 => the distribution may be truncated).
        auto dump_hist = [&out](const char* tag, const std::unordered_map<int, uint64_t>& h) {
            std::vector<std::pair<int, uint64_t>> v(h.begin(), h.end());
            std::sort(v.begin(), v.end(),
                      [](const std::pair<int, uint64_t>& a, const std::pair<int, uint64_t>& b) {
                          return a.second > b.second;
                      });
            out << "\nalr sc " << tag;
            const size_t shown = v.size() < 16 ? v.size() : 16;
            for (size_t i = 0; i < shown; ++i) out << " " << v[i].first << ":" << v[i].second;
        };
        dump_hist("trace_hist", trace_hist);
        dump_hist("emul_hist", emul_hist);
        struct rusage ru {};
        ::getrusage(RUSAGE_CHILDREN, &ru);
        const uint64_t stime_us =
            static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000ull + ru.ru_stime.tv_usec;
        const uint64_t utime_us =
            static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000ull + ru.ru_utime.tv_usec;
        // nonvol_ctxt = involuntary context switches (getrusage ru_nivcsw == the
        // aggregate of /proc/<tid>/status nonvoluntary_ctxt_switches the ADR specifies).
        out << "\nalr sc stime_us=" << stime_us << " utime_us=" << utime_us
            << " nonvol_ctxt=" << ru.ru_nivcsw
            << " traps=" << path_traps << " emul=" << emulated_syscalls;
    }
    if (fault_pc != 0) {
        out << "\nalr native loader fault pc-base-offset=0x" << std::hex << (fault_pc - 0x400000) << std::dec;
    }
    out << "\nalr native loader diag=" << diag;
    out << "\nalr native loader guest stdout=" << guest_stdout;

    // --- Surface the probe so it is observable off-device (logcat + pullable file).
    // Gated to the dynamic/GIMP path: tiny static probes (hello/mt-test) never reach
    // here with is_dyn==true, so they neither spam logcat nor write a file.
    if (is_dyn) {
        const std::string& full = out.str();

        // (1) logcat: counts + bounded head/tail of guest_stdout. __android_log_print
        // truncates each message to ~4 KB, so emit head and tail separately and cap them.
        constexpr std::size_t kHeadTail = 1500;
        const std::string head = guest_stdout.substr(0, kHeadTail);
        const std::string tail =
            guest_stdout.size() > kHeadTail
                ? guest_stdout.substr(guest_stdout.size() - kHeadTail)
                : std::string();
        const long long exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_exec_start).count();  // WS-1 M2: native-exec wall-clock
        __android_log_print(ANDROID_LOG_INFO, "alr_loader",
                            "gimp-probe guest=%s exit=%d sig=%d pcgate=%d interpose=%d "
                            "traps=%d rewrites=%d first_rewrite=%s stdout_bytes=%zu exec_ms=%lld",
                            guest_rel.c_str(), code, sig, pcgate_on ? 1 : 0,
                            interpose_off ? 0 : 1, path_traps, path_rewrites,
                            first_rewrite.empty() ? "(none)" : first_rewrite.c_str(),
                            guest_stdout.size(), exec_ms);
        __android_log_print(ANDROID_LOG_INFO, "alr_loader",
                            "gimp-probe stdout-head: %s", head.c_str());
        if (!tail.empty()) {
            __android_log_print(ANDROID_LOG_INFO, "alr_loader",
                                "gimp-probe stdout-tail: %s", tail.c_str());
        }

        // (2) full report -> <app_cache_dir>/gimp-probe-last.txt (pullable). Best-effort:
        // any failure is logged, never throws (this is a diagnostics side-channel).
        const std::string cache =
            input.app_cache_dir.empty() ? std::string("/data/local/tmp") : input.app_cache_dir;
        const std::string path = cache + "/gimp-probe-last.txt";
        const int lfd = ::open(path.c_str(),
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (lfd >= 0) {
            std::size_t off = 0;
            while (off < full.size()) {
                const ssize_t wn = ::write(lfd, full.data() + off, full.size() - off);
                if (wn > 0) {
                    off += static_cast<std::size_t>(wn);
                } else if (wn < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;  // give up on hard error; partial file is still useful
                }
            }
            ::close(lfd);
            __android_log_print(ANDROID_LOG_INFO, "alr_loader",
                                "gimp-probe wrote %zu bytes to %s", full.size(), path.c_str());
        } else {
            __android_log_print(ANDROID_LOG_WARN, "alr_loader",
                                "gimp-probe could not open %s errno=%d", path.c_str(), errno);
        }
    }

    // WS-1 / CP-3: same-binary CPU-overhead capture. When the guest is the microbench
    // (its single output line is "MICROBENCH mode=<m> iters=<n> ns=<t> ns_per_op=<x>"),
    // lift that line verbatim into logcat under the alr_loader tag with an
    // `alr-microbench:` prefix. This is the ALR-path counterpart to the device-native
    // baseline (`adb shell <microbench> compute|syscall`), so the integration drain can
    // diff the two ns_per_op values into a true apples-to-apples CP-3 overhead %
    // (`python -m bench overhead`), replacing the earlier ALR-getppid≈218ns proxy. The
    // guest's own MICROBENCH line is what's emitted, so the ALR and native sides parse
    // identically. The `exec_ms` (loader fork→reap wall-clock) is appended for context.
    // This block runs for ANY guest whose stdout carries a MICROBENCH line (the static
    // musl/glibc microbench), independent of the GIMP-gated is_dyn logging above.
    {
        const std::size_t mb = guest_stdout.find("MICROBENCH ");
        if (mb != std::string::npos) {
            std::size_t eol = guest_stdout.find('\n', mb);
            if (eol == std::string::npos) eol = guest_stdout.size();
            const std::string mb_line = guest_stdout.substr(mb, eol - mb);
            const long long exec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_exec_start).count();
            __android_log_print(ANDROID_LOG_INFO, "alr_loader",
                                "alr-microbench: guest=%s exit=%d sig=%d exec_ms=%lld %s",
                                guest_rel.c_str(), code, sig, exec_ms, mb_line.c_str());
        }
    }

    // WS-1 ↔ WS-2 (CP-0): guest reaped → stop the Mali executor + free ring/doorbell.
    if (gpu_ring_attached) alr::gpu::alr_loader_detach_gpu_ring();
    return out.str();
}

// Self-contained proof of the loader MECHANISM, free of glibc runtime coexistence
// issues: assemble a tiny freestanding PIE ELF whose entry is just
// `mov x0,#42; mov x8,#93(exit); svc #0`, load it through the same map-into-
// execmem + jump path, and confirm the child exits with code 42. If this passes,
// ALR can natively execute a guest ELF in-process, W^X-safe, with no PRoot.
std::string build_native_loader_selftest() {
    std::ostringstream out;
    out << "ALR NATIVE LOADER SELFTEST: android-native-attempt";
#if defined(__aarch64__)
    const uint32_t code[] = {0xd2800540u, 0xd2800ba8u, 0xd4000001u};  // mov x0,#42; mov x8,#93; svc #0
    const std::size_t ehsz = sizeof(Elf64_Ehdr);
    const std::size_t phsz = sizeof(Elf64_Phdr);
    const std::size_t code_off = ehsz + phsz;
    const std::size_t total = code_off + sizeof(code);

    std::string image;
    image.resize(total, '\0');
    Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7f; eh.e_ident[1] = 'E'; eh.e_ident[2] = 'L'; eh.e_ident[3] = 'F';
    eh.e_ident[4] = ELFCLASS64; eh.e_ident[5] = ELFDATA2LSB; eh.e_ident[6] = EV_CURRENT;
    eh.e_type = ET_DYN;
    eh.e_machine = EM_AARCH64;
    eh.e_version = EV_CURRENT;
    eh.e_entry = code_off;
    eh.e_phoff = ehsz;
    eh.e_ehsize = static_cast<uint16_t>(ehsz);
    eh.e_phentsize = static_cast<uint16_t>(phsz);
    eh.e_phnum = 1;
    Elf64_Phdr ph{};
    ph.p_type = PT_LOAD;
    ph.p_flags = PF_R | PF_X;
    ph.p_offset = 0;
    ph.p_vaddr = 0;
    ph.p_filesz = total;
    ph.p_memsz = total;
    ph.p_align = 0x1000;
    std::memcpy(&image[0], &eh, ehsz);
    std::memcpy(&image[ehsz], &ph, phsz);
    std::memcpy(&image[code_off], code, sizeof(code));

    const char* img = image.data();
    const std::size_t entry_off = code_off;

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::alarm(5);
        const std::size_t span = (total + 0xfff) & ~static_cast<std::size_t>(0xfff);
        void* mem = ::mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            _exit(60);
        }
        std::memcpy(mem, img, total);
        if (::mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
            _exit(61);
        }
        __builtin___clear_cache(reinterpret_cast<char*>(mem), reinterpret_cast<char*>(mem) + span);
        void* stk = ::mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stk == MAP_FAILED) {
            _exit(62);
        }
        uintptr_t sp = (reinterpret_cast<uintptr_t>(stk) + 65536 - 64) & ~static_cast<uintptr_t>(0xf);
        uint64_t* w = reinterpret_cast<uint64_t*>(sp);
        w[0] = 0;  // argc
        w[1] = 0;  // argv NULL
        w[2] = 0;  // envp NULL
        w[3] = AT_NULL;
        w[4] = 0;
        const uintptr_t entry = reinterpret_cast<uintptr_t>(mem) + entry_off;
        alr_enter_guest(reinterpret_cast<void*>(sp), reinterpret_cast<void*>(entry), nullptr);
        _exit(99);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    const int code_out = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    const bool ran = WIFEXITED(status) && code_out == 42;
    out << "\nALR NATIVE LOADER SELFTEST EXEC: " << (ran ? "PASS" : "FAIL");
    out << "\nalr native loader selftest bytes=" << total;
    out << "\nalr native loader selftest child exit=" << code_out << " signal=" << sig;
#else
    out << "\nALR NATIVE LOADER SELFTEST EXEC: SKIP (non-arm64 abi)";
#endif
    return out.str();
}


std::string egl_error_hex() {
    std::ostringstream out;
    out << "0x" << std::hex << eglGetError();
    return out.str();
}

std::string safe_gl_string(GLenum name) {
    const auto* value = glGetString(name);
    if (value == nullptr) {
        return "missing";
    }
    return reinterpret_cast<const char*>(value);
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool renderer_looks_software(const std::string& vendor, const std::string& renderer) {
    const auto combined = lower_copy(vendor + " " + renderer);
    return combined.find("swiftshader") != std::string::npos ||
           combined.find("llvmpipe") != std::string::npos ||
           combined.find("softpipe") != std::string::npos ||
           combined.find("software") != std::string::npos ||
           combined.find("mesa offscreen") != std::string::npos;
}

std::string vk_result_string(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_NOT_READY:
            return "VK_NOT_READY";
        case VK_TIMEOUT:
            return "VK_TIMEOUT";
        case VK_EVENT_SET:
            return "VK_EVENT_SET";
        case VK_EVENT_RESET:
            return "VK_EVENT_RESET";
        case VK_INCOMPLETE:
            return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:
            return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:
            return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:
            return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:
            return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_SUBOPTIMAL_KHR:
            return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:
            return "VK_ERROR_OUT_OF_DATE_KHR";
        default:
            break;
    }
    std::ostringstream out;
    out << static_cast<int>(result);
    return out.str();
}

std::string vk_api_version_string(uint32_t version) {
    std::ostringstream out;
    out << VK_VERSION_MAJOR(version) << "."
        << VK_VERSION_MINOR(version) << "."
        << VK_VERSION_PATCH(version);
    return out.str();
}

bool vulkan_name_looks_software(const std::string& name) {
    const auto lower = lower_copy(name);
    return lower.find("swiftshader") != std::string::npos ||
           lower.find("llvmpipe") != std::string::npos ||
           lower.find("softpipe") != std::string::npos ||
           lower.find("lavapipe") != std::string::npos ||
           lower.find("software") != std::string::npos;
}

std::string vulkan_device_type_string(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return "integrated-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return "discrete-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return "virtual-gpu";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return "cpu";
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        default:
            return "other";
    }
}

bool instance_extension_present(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
        return std::string(extension.extensionName) == name;
    });
}

bool device_extension_present(VkPhysicalDevice device, const char* name) {
    uint32_t extension_count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0) {
        result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return false;
        }
    }
    return instance_extension_present(extensions, name);
}

std::string vulkan_present_mode_string(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR:
            return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return "fifo-relaxed";
        default:
            break;
    }
    std::ostringstream out;
    out << static_cast<int>(mode);
    return out.str();
}

std::string vulkan_format_string(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return "r8g8b8a8-unorm";
        case VK_FORMAT_B8G8R8A8_UNORM:
            return "b8g8r8a8-unorm";
        case VK_FORMAT_R8G8B8A8_SRGB:
            return "r8g8b8a8-srgb";
        case VK_FORMAT_B8G8R8A8_SRGB:
            return "b8g8r8a8-srgb";
        default:
            break;
    }
    std::ostringstream out;
    out << static_cast<int>(format);
    return out.str();
}

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED) {
        return VkSurfaceFormatKHR{
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .colorSpace = formats.front().colorSpace,
        };
    }
    const std::array<VkFormat, 4> preferred_formats{
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_FORMAT_B8G8R8A8_SRGB,
    };
    for (const auto preferred : preferred_formats) {
        const auto found = std::find_if(formats.begin(), formats.end(), [preferred](const VkSurfaceFormatKHR& format) {
            return format.format == preferred;
        });
        if (found != formats.end()) {
            return *found;
        }
    }
    return formats.front();
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes) {
    const auto mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR);
    if (mailbox != modes.end()) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    const auto fifo = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR);
    if (fifo != modes.end()) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    return modes.empty() ? VK_PRESENT_MODE_FIFO_KHR : modes.front();
}

VkCompositeAlphaFlagBitsKHR choose_composite_alpha(const VkSurfaceCapabilitiesKHR& capabilities) {
    const std::array<VkCompositeAlphaFlagBitsKHR, 4> preferred{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const auto alpha : preferred) {
        if ((capabilities.supportedCompositeAlpha & alpha) != 0) {
            return alpha;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

VkExtent2D choose_swapchain_extent(const VkSurfaceCapabilitiesKHR& capabilities, ANativeWindow* window) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    const auto clamp_u32 = [](uint32_t value, uint32_t min_value, uint32_t max_value) {
        return std::max(min_value, std::min(max_value, value));
    };
    const auto window_width = static_cast<uint32_t>(std::max(1, ANativeWindow_getWidth(window)));
    const auto window_height = static_cast<uint32_t>(std::max(1, ANativeWindow_getHeight(window)));
    return VkExtent2D{
        .width = clamp_u32(window_width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        .height = clamp_u32(window_height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

std::string build_host_vulkan_probe_report() {
    std::ostringstream out;
    out << "host vulkan probe=android-vulkan-instance-device";

    uint32_t extension_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS) {
        out << "\nvulkan enumerate instance extensions=fail result=" << vk_result_string(result);
        return out.str();
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0) {
        result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            out << "\nvulkan enumerate instance extensions=fail result=" << vk_result_string(result);
            return out.str();
        }
    }
    const bool has_surface = instance_extension_present(extensions, VK_KHR_SURFACE_EXTENSION_NAME);
    const bool has_android_surface = instance_extension_present(extensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    out << "\nvulkan instance extensions count=" << extension_count;
    out << "\nhost vulkan surface extension=" << (has_surface ? "true" : "false");
    out << "\nhost vulkan android surface extension=" << (has_android_surface ? "true" : "false");

    const char* enabled_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    std::vector<const char*> requested_extensions;
    if (has_surface && has_android_surface) {
        requested_extensions.push_back(enabled_extensions[0]);
        requested_extensions.push_back(enabled_extensions[1]);
    }

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Android on Linux Runtime";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "ALR Host Probe";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(requested_extensions.size());
    create_info.ppEnabledExtensionNames = requested_extensions.empty() ? nullptr : requested_extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        out << "\nvulkan create instance=fail result=" << vk_result_string(result);
        out << "\nhost vulkan hardware candidate=false";
        return out.str();
    }
    out << "\nvulkan create instance=ok";

    uint32_t physical_device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
    if (result != VK_SUCCESS || physical_device_count == 0) {
        out << "\nvulkan enumerate physical devices=fail result=" << vk_result_string(result)
            << " count=" << physical_device_count;
        out << "\nhost vulkan hardware candidate=false";
        vkDestroyInstance(instance, nullptr);
        return out.str();
    }
    std::vector<VkPhysicalDevice> devices(physical_device_count);
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        out << "\nvulkan enumerate physical devices=fail result=" << vk_result_string(result);
        out << "\nhost vulkan hardware candidate=false";
        vkDestroyInstance(instance, nullptr);
        return out.str();
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(devices.front(), &properties);
    const std::string device_name = properties.deviceName;
    const auto device_type = vulkan_device_type_string(properties.deviceType);
    const bool software = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                          vulkan_name_looks_software(device_name);

    out << "\nvulkan physical devices count=" << physical_device_count;
    out << "\nhost vulkan device=" << device_name;
    out << "\nhost vulkan device type=" << device_type;
    out << "\nhost vulkan api version=" << vk_api_version_string(properties.apiVersion);
    out << "\nhost vulkan driver version=" << properties.driverVersion;
    out << "\nhost vulkan vendor id=0x" << std::hex << properties.vendorID << std::dec;
    out << "\nhost vulkan device id=0x" << std::hex << properties.deviceID << std::dec;
    out << "\nhost vulkan software renderer=" << (software ? "true" : "false");
    out << "\nhost vulkan hardware candidate="
        << (!software && has_surface && has_android_surface ? "true" : "false");

    vkDestroyInstance(instance, nullptr);
    return out.str();
}

std::string probe_vulkan_android_surface(JNIEnv* env, jobject surface_obj) {
    std::ostringstream out;
    out << "host vulkan surface probe=android-vulkan-surface-capabilities";
    if (surface_obj == nullptr) {
        out << "\nvulkan surface probe=fail reason=null-surface";
        out << "\nhost vulkan surface supported=false";
        return out.str();
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface_obj);
    if (window == nullptr) {
        out << "\nvulkan surface probe=fail reason=ANativeWindow_fromSurface";
        out << "\nhost vulkan surface supported=false";
        return out.str();
    }
    out << "\nvulkan surface window=ok width=" << ANativeWindow_getWidth(window)
        << " height=" << ANativeWindow_getHeight(window);

    uint32_t extension_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS) {
        out << "\nvulkan enumerate instance extensions=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface supported=false";
        ANativeWindow_release(window);
        return out.str();
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0) {
        result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            out << "\nvulkan enumerate instance extensions=fail result=" << vk_result_string(result);
            out << "\nhost vulkan surface supported=false";
            ANativeWindow_release(window);
            return out.str();
        }
    }
    const bool has_surface = instance_extension_present(extensions, VK_KHR_SURFACE_EXTENSION_NAME);
    const bool has_android_surface = instance_extension_present(extensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    out << "\nhost vulkan surface extension=" << (has_surface ? "true" : "false");
    out << "\nhost vulkan android surface extension=" << (has_android_surface ? "true" : "false");
    if (!has_surface || !has_android_surface) {
        out << "\nhost vulkan surface supported=false";
        ANativeWindow_release(window);
        return out.str();
    }

    const char* enabled_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Android on Linux Runtime Surface Probe";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "ALR Host Surface Probe";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_create_info{};
    instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pApplicationInfo = &app_info;
    instance_create_info.enabledExtensionCount = 2;
    instance_create_info.ppEnabledExtensionNames = enabled_extensions;

    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&instance_create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        out << "\nvulkan create surface probe instance=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface supported=false";
        ANativeWindow_release(window);
        return out.str();
    }
    out << "\nvulkan create surface probe instance=ok";

    VkAndroidSurfaceCreateInfoKHR surface_create_info{};
    surface_create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_create_info.window = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    result = vkCreateAndroidSurfaceKHR(instance, &surface_create_info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        out << "\nvulkan create android surface=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface supported=false";
        vkDestroyInstance(instance, nullptr);
        ANativeWindow_release(window);
        return out.str();
    }
    out << "\nvulkan create android surface=ok";

    uint32_t physical_device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
    if (result != VK_SUCCESS || physical_device_count == 0) {
        out << "\nvulkan enumerate surface physical devices=fail result=" << vk_result_string(result)
            << " count=" << physical_device_count;
        out << "\nhost vulkan surface supported=false";
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        ANativeWindow_release(window);
        return out.str();
    }
    std::vector<VkPhysicalDevice> devices(physical_device_count);
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        out << "\nvulkan enumerate surface physical devices=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface supported=false";
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        ANativeWindow_release(window);
        return out.str();
    }

    bool selected = false;
    std::string selected_device_name = "missing";
    std::string selected_device_type = "missing";
    bool selected_software = true;
    uint32_t selected_queue_family = 0;
    uint32_t selected_format_count = 0;
    uint32_t selected_present_mode_count = 0;
    VkSurfaceCapabilitiesKHR selected_capabilities{};
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        if (queue_family_count > 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());
        }
        for (uint32_t queue_index = 0; queue_index < queue_family_count; ++queue_index) {
            VkBool32 surface_supported = VK_FALSE;
            result = vkGetPhysicalDeviceSurfaceSupportKHR(device, queue_index, surface, &surface_supported);
            if (result != VK_SUCCESS || surface_supported != VK_TRUE) {
                continue;
            }
            uint32_t format_count = 0;
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
            if (result != VK_SUCCESS || format_count == 0) {
                continue;
            }
            uint32_t present_mode_count = 0;
            result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, nullptr);
            if (result != VK_SUCCESS || present_mode_count == 0) {
                continue;
            }
            VkSurfaceCapabilitiesKHR capabilities{};
            result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &capabilities);
            if (result != VK_SUCCESS) {
                continue;
            }
            selected = true;
            selected_device_name = properties.deviceName;
            selected_device_type = vulkan_device_type_string(properties.deviceType);
            selected_software = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                                vulkan_name_looks_software(selected_device_name);
            selected_queue_family = queue_index;
            selected_format_count = format_count;
            selected_present_mode_count = present_mode_count;
            selected_capabilities = capabilities;
            break;
        }
        if (selected) {
            break;
        }
    }

    out << "\nvulkan surface physical devices count=" << physical_device_count;
    out << "\nhost vulkan surface device=" << selected_device_name;
    out << "\nhost vulkan surface device type=" << selected_device_type;
    out << "\nhost vulkan surface queue family index=" << selected_queue_family;
    out << "\nhost vulkan surface formats count=" << selected_format_count;
    out << "\nhost vulkan surface present modes count=" << selected_present_mode_count;
    out << "\nhost vulkan surface current extent="
        << selected_capabilities.currentExtent.width << "x" << selected_capabilities.currentExtent.height;
    out << "\nhost vulkan surface min images=" << selected_capabilities.minImageCount;
    out << "\nhost vulkan surface max images=" << selected_capabilities.maxImageCount;
    out << "\nhost vulkan surface software renderer=" << (selected_software ? "true" : "false");
    out << "\nhost vulkan surface supported=" << (selected ? "true" : "false");
    out << "\nhost vulkan surface hardware candidate=" << (selected && !selected_software ? "true" : "false");

    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    ANativeWindow_release(window);
    return out.str();
}

// GPU command-marshalling proof: encode a guest-style GLES command stream into
// bytes, then decode it on the host and dispatch each command as a REAL GLES
// call in a pbuffer context, and verify the rendered pixels. This is the command-
// translation layer that low-overhead GPU passthrough needs on top of the
// (v59-measured) cheap transport boundary.
std::string build_gpu_marshalling_probe() {
    enum Op : unsigned char {
        OP_END = 0, OP_VIEWPORT = 1, OP_CLEARCOLOR = 2, OP_CLEAR = 3,
        OP_ENABLE_SCISSOR = 4, OP_SCISSOR = 5, OP_DISABLE_SCISSOR = 6,
    };
    std::ostringstream out;
    out << "ALR GPU MARSHALLING PROBE: android-egl-gles";

    // ---- guest side: encode a command stream ----
    std::vector<unsigned char> stream;
    auto put_u8 = [&](unsigned char v) { stream.push_back(v); };
    auto put_i32 = [&](int32_t v) {
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        stream.insert(stream.end(), b, b + 4);
    };
    auto put_f32 = [&](float v) {
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        stream.insert(stream.end(), b, b + 4);
    };
    put_u8(OP_VIEWPORT); put_i32(0); put_i32(0); put_i32(64); put_i32(64);
    put_u8(OP_CLEARCOLOR); put_f32(0.10F); put_f32(0.20F); put_f32(0.70F); put_f32(1.0F);
    put_u8(OP_CLEAR);
    put_u8(OP_ENABLE_SCISSOR);
    put_u8(OP_SCISSOR); put_i32(24); put_i32(24); put_i32(16); put_i32(16);
    put_u8(OP_CLEARCOLOR); put_f32(0.90F); put_f32(0.30F); put_f32(0.10F); put_f32(1.0F);
    put_u8(OP_CLEAR);
    put_u8(OP_DISABLE_SCISSOR);
    put_u8(OP_END);

    // ---- host side: EGL pbuffer context ----
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
        out << "\nALR GPU MARSHALLING DECODE+EXECUTE: FAIL\nALR GPU MARSHALLING HARDWARE RENDER: FAIL";
        out << "\nalr gpu marshalling error=egl init " << egl_error_hex();
        return out.str();
    }
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint cfg_count = 0;
    const EGLint pb_attribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    if (eglChooseConfig(display, cfg_attribs, &config, 1, &cfg_count) != EGL_TRUE || cfg_count < 1) {
        out << "\nALR GPU MARSHALLING DECODE+EXECUTE: FAIL\nALR GPU MARSHALLING HARDWARE RENDER: FAIL";
        out << "\nalr gpu marshalling error=choose config " << egl_error_hex();
        eglTerminate(display);
        return out.str();
    }
    EGLSurface surface = eglCreatePbufferSurface(display, config, pb_attribs);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        out << "\nALR GPU MARSHALLING DECODE+EXECUTE: FAIL\nALR GPU MARSHALLING HARDWARE RENDER: FAIL";
        out << "\nalr gpu marshalling error=context/makecurrent " << egl_error_hex();
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
        return out.str();
    }

    // ---- host side: decode the stream and dispatch real GLES calls ----
    int decoded = 0;
    bool decode_ok = true;
    std::size_t p = 0;
    auto get_i32 = [&](int32_t& v) {
        if (p + 4 > stream.size()) { decode_ok = false; return; }
        std::memcpy(&v, stream.data() + p, 4); p += 4;
    };
    auto get_f32 = [&](float& v) {
        if (p + 4 > stream.size()) { decode_ok = false; return; }
        std::memcpy(&v, stream.data() + p, 4); p += 4;
    };
    bool running = true;
    while (running && decode_ok && p < stream.size()) {
        const unsigned char op = stream[p++];
        switch (op) {
            case OP_VIEWPORT: {
                int32_t x, y, w, h;
                get_i32(x); get_i32(y); get_i32(w); get_i32(h);
                glViewport(x, y, w, h);
                ++decoded;
                break;
            }
            case OP_CLEARCOLOR: {
                float r, g, b, a;
                get_f32(r); get_f32(g); get_f32(b); get_f32(a);
                glClearColor(r, g, b, a);
                ++decoded;
                break;
            }
            case OP_CLEAR: glClear(GL_COLOR_BUFFER_BIT); ++decoded; break;
            case OP_ENABLE_SCISSOR: glEnable(GL_SCISSOR_TEST); ++decoded; break;
            case OP_SCISSOR: {
                int32_t x, y, w, h;
                get_i32(x); get_i32(y); get_i32(w); get_i32(h);
                glScissor(x, y, w, h);
                ++decoded;
                break;
            }
            case OP_DISABLE_SCISSOR: glDisable(GL_SCISSOR_TEST); ++decoded; break;
            case OP_END: running = false; break;
            default: decode_ok = false; break;
        }
    }
    glFinish();
    const GLenum gl_error = glGetError();

    // ---- verify: the scissored center got color B, the corner kept color A ----
    unsigned char center[4] = {0, 0, 0, 0};
    unsigned char corner[4] = {0, 0, 0, 0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    const bool center_is_b = center[0] > 180 && center[2] < 80;   // red-ish
    const bool corner_is_a = corner[0] < 80 && corner[2] > 150;   // blue-ish
    const auto vendor = safe_gl_string(GL_VENDOR);
    const auto renderer = safe_gl_string(GL_RENDERER);
    const bool software = renderer_looks_software(vendor, renderer);
    const bool execute_ok = decode_ok && !software && gl_error == GL_NO_ERROR &&
        center_is_b && corner_is_a && decoded == 8;

    out << "\nALR GPU MARSHALLING DECODE+EXECUTE: " << (decode_ok && center_is_b && corner_is_a ? "PASS" : "FAIL");
    out << "\nALR GPU MARSHALLING HARDWARE RENDER: " << (execute_ok ? "PASS" : "FAIL");
    out << "\nalr gpu marshalling stream bytes=" << stream.size();
    out << "\nalr gpu marshalling commands decoded=" << decoded;
    out << "\nalr gpu marshalling renderer=" << renderer;
    out << "\nalr gpu marshalling center pixel=" << static_cast<int>(center[0]) << ","
        << static_cast<int>(center[1]) << "," << static_cast<int>(center[2]);
    out << "\nalr gpu marshalling corner pixel=" << static_cast<int>(corner[0]) << ","
        << static_cast<int>(corner[1]) << "," << static_cast<int>(corner[2]);
    out << "\nalr gpu marshalling gl error=0x" << std::hex << gl_error << std::dec;
    out << "\nalr gpu marshalling software renderer=" << (software ? "true" : "false");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return out.str();
}

// AHardwareBuffer zero-copy display probe (GEGL-GPU design Option C, milestone M1).
// Proves the public-API zero-copy path the present optimization will use: allocate
// an AHardwareBuffer, CPU-fill a known pattern, import it as a GL texture via
// EGLImage with NO glTexImage2D copy (eglGetNativeClientBufferANDROID ->
// eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID) -> glEGLImageTargetTexture2DOES onto
// GL_TEXTURE_EXTERNAL_OES), sample it into an FBO, and glReadPixels-verify the hue
// round-trips on real Mali hardware. This is the canonical Android zero-copy path
// (AHardwareBuffer + GL_OES_EGL_image_external), 100% public NDK API.
std::string build_ahb_zerocopy_probe_report() {
    std::ostringstream out;
    out << "alr ahb zerocopy probe=ahardwarebuffer-eglimage-external-oes";

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
        out << "\nALR AHB ZEROCOPY: FAIL\nalr ahb error=egl-init " << egl_error_hex();
        return out.str();
    }
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (eglChooseConfig(display, cfg_attribs, &config, 1, &n) != EGL_TRUE || n < 1) {
        out << "\nALR AHB ZEROCOPY: FAIL\nalr ahb error=choose-config " << egl_error_hex();
        eglTerminate(display);
        return out.str();
    }
    const EGLint pb[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, pb);
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
        eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        out << "\nALR AHB ZEROCOPY: FAIL\nalr ahb error=make-current " << egl_error_hex();
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
        return out.str();
    }

    // Required extensions (Mali exposes all three).
    const char* egl_exts = eglQueryString(display, EGL_EXTENSIONS);
    const auto* gl_exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const std::string egl_ext = egl_exts ? egl_exts : "";
    const std::string gl_ext = gl_exts ? gl_exts : "";
    const bool has_native_buf = egl_ext.find("EGL_ANDROID_get_native_client_buffer") != std::string::npos;
    const bool has_image_base = egl_ext.find("EGL_KHR_image_base") != std::string::npos;
    const bool has_ext_oes = gl_ext.find("GL_OES_EGL_image_external") != std::string::npos;
    out << "\nalr ahb ext native_client_buffer=" << (has_native_buf ? "yes" : "no");
    out << "\nalr ahb ext egl_image_base=" << (has_image_base ? "yes" : "no");
    out << "\nalr ahb ext gl_oes_egl_image_external=" << (has_ext_oes ? "yes" : "no");

    // Resolve the extension entry points at runtime (not in the link-time GLES2 ABI).
    auto p_eglGetNativeClientBuffer =
        reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    auto p_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    auto p_eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    auto p_glEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!p_eglGetNativeClientBuffer || !p_eglCreateImageKHR ||
        !p_eglDestroyImageKHR || !p_glEGLImageTargetTexture2DOES) {
        out << "\nALR AHB ZEROCOPY: FAIL\nalr ahb error=missing-extension-procs";
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return out.str();
    }

    // 1) Allocate an AHardwareBuffer (GPU-sampleable + CPU-writable).
    constexpr uint32_t kW = 64, kH = 64;
    AHardwareBuffer_Desc desc{};
    desc.width = kW;
    desc.height = kH;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    if (AHardwareBuffer_allocate(&desc, &ahb) != 0 || ahb == nullptr) {
        out << "\nALR AHB ZEROCOPY: FAIL\nalr ahb error=allocate";
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return out.str();
    }

    // 2) CPU-fill a known pattern: solid orange (R=230,G=120,B=20,A=255). Honor the
    //    buffer's actual row stride (may exceed width*4).
    AHardwareBuffer_Desc got{};
    AHardwareBuffer_describe(ahb, &got);
    void* cpu = nullptr;
    bool filled = false;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &cpu) == 0 &&
        cpu != nullptr) {
        const uint32_t stride_px = got.stride ? got.stride : kW;  // stride is in PIXELS
        auto* px = static_cast<uint8_t*>(cpu);
        for (uint32_t y = 0; y < kH; ++y) {
            uint8_t* row = px + static_cast<size_t>(y) * stride_px * 4;
            for (uint32_t x = 0; x < kW; ++x) {
                row[x * 4 + 0] = 230;  // R
                row[x * 4 + 1] = 120;  // G
                row[x * 4 + 2] = 20;   // B
                row[x * 4 + 3] = 255;  // A
            }
        }
        AHardwareBuffer_unlock(ahb, nullptr);
        filled = true;
    }

    // 3) Import the AHB as a GL_TEXTURE_EXTERNAL_OES texture via EGLImage — NO copy.
    EGLClientBuffer client_buf = p_eglGetNativeClientBuffer(ahb);
    const EGLint img_attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = client_buf
        ? p_eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                              client_buf, img_attribs)
        : EGL_NO_IMAGE_KHR;
    GLuint ext_tex = 0;
    glGenTextures(1, &ext_tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext_tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (image != EGL_NO_IMAGE_KHR) {
        p_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
    }
    const GLenum bind_err = glGetError();

    // 4) Sample the external texture into an RGBA FBO and read back the center pixel.
    GLuint fbo_tex = 0, fbo = 0;
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);

    const char* vs_src =
        "attribute vec2 aPos;\nvarying vec2 vUv;\n"
        "void main(){ vUv = aPos*0.5+0.5; gl_Position = vec4(aPos,0.0,1.0); }\n";
    const char* fs_src =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\nvarying vec2 vUv;\n"
        "uniform samplerExternalOES uTex;\n"
        "void main(){ gl_FragColor = texture2D(uTex, vUv); }\n";
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { glDeleteShader(s); return 0; }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = 0;
    bool drew = false;
    unsigned char center[4] = {0, 0, 0, 0};
    if (vs && fs) {
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glBindAttribLocation(prog, 0, "aPos");
        glLinkProgram(prog);
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (linked) {
            glViewport(0, 0, kW, kH);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, ext_tex);
            glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
            const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glFinish();
            glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
            drew = true;
        }
    }
    const GLenum gl_error = glGetError();

    // 5) Verify the orange round-tripped through the zero-copy import (hue, not exact).
    const bool hue_ok = center[0] > 180 && center[1] > 70 && center[1] < 170 && center[2] < 90;
    const auto vendor = safe_gl_string(GL_VENDOR);
    const auto renderer = safe_gl_string(GL_RENDERER);
    const bool software = renderer_looks_software(vendor, renderer);
    const bool import_ok = (image != EGL_NO_IMAGE_KHR) && bind_err == GL_NO_ERROR;
    const bool zerocopy_ok = has_native_buf && has_ext_oes && filled && import_ok && drew &&
                             hue_ok && gl_error == GL_NO_ERROR && !software;

    out << "\nALR AHB ZEROCOPY IMPORT: " << (import_ok ? "PASS" : "FAIL");
    out << "\nALR AHB ZEROCOPY HARDWARE SAMPLE: " << (zerocopy_ok ? "PASS" : "FAIL");
    out << "\nalr ahb buffer=" << kW << "x" << kH << " stride_px=" << got.stride;
    out << "\nalr ahb center pixel=" << static_cast<int>(center[0]) << ","
        << static_cast<int>(center[1]) << "," << static_cast<int>(center[2]);
    out << "\nalr ahb expected~=230,120,20 (orange)";
    out << "\nalr ahb renderer=" << renderer;
    out << "\nalr ahb gl error=0x" << std::hex << gl_error << std::dec;
    out << "\nalr ahb software renderer=" << (software ? "true" : "false");

    if (prog) glDeleteProgram(prog);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (fbo_tex) glDeleteTextures(1, &fbo_tex);
    if (ext_tex) glDeleteTextures(1, &ext_tex);
    if (image != EGL_NO_IMAGE_KHR) p_eglDestroyImageKHR(display, image);
    if (ahb) AHardwareBuffer_release(ahb);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return out.str();
}

std::string build_host_gpu_probe_report() {
    std::ostringstream out;
    out << "host gpu probe=android-egl-gles-pbuffer";

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        out << "\negl display=fail error=" << egl_error_hex();
        return out.str();
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
        out << "\negl initialize=fail error=" << egl_error_hex();
        return out.str();
    }
    out << "\negl initialize=ok version=" << major << "." << minor;

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint config_count = 0;
    if (eglChooseConfig(display, config_attribs, &config, 1, &config_count) != EGL_TRUE || config_count < 1) {
        out << "\negl choose config=fail error=" << egl_error_hex();
        eglTerminate(display);
        return out.str();
    }
    out << "\negl choose config=ok count=" << config_count;

    const EGLint surface_attribs[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE,
    };
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attribs);
    if (surface == EGL_NO_SURFACE) {
        out << "\negl pbuffer surface=fail error=" << egl_error_hex();
        eglTerminate(display);
        return out.str();
    }
    out << "\negl pbuffer surface=ok size=16x16";

    const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (context == EGL_NO_CONTEXT) {
        out << "\negl context=fail error=" << egl_error_hex();
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return out.str();
    }
    out << "\negl context=ok api=gles2";

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        out << "\negl make current=fail error=" << egl_error_hex();
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return out.str();
    }
    out << "\negl make current=ok";

    glViewport(0, 0, 16, 16);
    glClearColor(0.125F, 0.25F, 0.5F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    const GLenum gl_error = glGetError();

    const auto vendor = safe_gl_string(GL_VENDOR);
    const auto renderer = safe_gl_string(GL_RENDERER);
    const auto version = safe_gl_string(GL_VERSION);
    const auto shading_language = safe_gl_string(GL_SHADING_LANGUAGE_VERSION);
    const bool software = renderer_looks_software(vendor, renderer);
    out << "\ngl vendor=" << vendor;
    out << "\ngl renderer=" << renderer;
    out << "\ngl version=" << version;
    out << "\ngl shading language=" << shading_language;
    out << "\ngl clear error=0x" << std::hex << gl_error << std::dec;
    out << "\nhost gpu software renderer=" << (software ? "true" : "false");
    out << "\nhost gpu hardware candidate=" << (!software && gl_error == GL_NO_ERROR ? "true" : "false");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return out.str();
}


struct SurfaceFrameCommand {
    float red = 0.05F;
    float green = 0.18F;
    float blue = 0.45F;
    std::string tag = "host-default";
};

std::vector<SurfaceFrameCommand> parse_surface_frames(const std::string& encoded) {
    std::vector<SurfaceFrameCommand> frames;
    std::istringstream input(encoded);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream parts(line);
        SurfaceFrameCommand cmd;
        if (parts >> cmd.red >> cmd.green >> cmd.blue >> cmd.tag) {
            cmd.red = std::max(0.0F, std::min(1.0F, cmd.red));
            cmd.green = std::max(0.0F, std::min(1.0F, cmd.green));
            cmd.blue = std::max(0.0F, std::min(1.0F, cmd.blue));
            frames.push_back(cmd);
        }
    }
    if (frames.empty()) {
        frames.push_back(SurfaceFrameCommand{});
    }
    return frames;
}

std::string render_vulkan_to_android_surface_frames(JNIEnv* env, jobject surface_obj, const std::string& encoded_frames) {
    const auto frames = parse_surface_frames(encoded_frames);
    std::ostringstream out;
    out << "host vulkan surface renderer=android-vulkan-swapchain-clear-present";
    out << "\nvulkan surface frame stream protocol=gui-compositor-clear-color-vulkan-v1";
    out << "\nvulkan surface requested frames=" << frames.size();
    if (surface_obj == nullptr) {
        out << "\nvulkan surface render=fail reason=null-surface";
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        return out.str();
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface_obj);
    if (window == nullptr) {
        out << "\nvulkan surface render=fail reason=ANativeWindow_fromSurface";
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        return out.str();
    }
    out << "\nvulkan surface window=ok width=" << ANativeWindow_getWidth(window)
        << " height=" << ANativeWindow_getHeight(window);

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> framebuffers;

    auto cleanup = [&]() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, command_pool, nullptr);
            }
            for (const auto framebuffer : framebuffers) {
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            }
            if (render_pass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device, render_pass, nullptr);
            }
            for (const auto image_view : image_views) {
                vkDestroyImageView(device, image_view, nullptr);
            }
            if (swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
        ANativeWindow_release(window);
    };

    uint32_t extension_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS) {
        out << "\nvulkan enumerate instance extensions=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count > 0) {
        result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            out << "\nvulkan enumerate instance extensions=fail result=" << vk_result_string(result);
            out << "\nhost vulkan surface present supported=false";
            out << "\nhost vulkan surface hardware render=false";
            cleanup();
            return out.str();
        }
    }
    const bool has_surface = instance_extension_present(extensions, VK_KHR_SURFACE_EXTENSION_NAME);
    const bool has_android_surface = instance_extension_present(extensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    out << "\nvulkan renderer surface extension=" << (has_surface ? "true" : "false");
    out << "\nvulkan renderer android surface extension=" << (has_android_surface ? "true" : "false");
    if (!has_surface || !has_android_surface) {
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }

    const char* enabled_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Android on Linux Runtime Vulkan Surface Renderer";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "ALR Host Vulkan Surface Renderer";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instance_create_info{};
    instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.pApplicationInfo = &app_info;
    instance_create_info.enabledExtensionCount = 2;
    instance_create_info.ppEnabledExtensionNames = enabled_extensions;
    result = vkCreateInstance(&instance_create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        out << "\nvulkan create renderer instance=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan create renderer instance=ok";

    VkAndroidSurfaceCreateInfoKHR surface_create_info{};
    surface_create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_create_info.window = window;
    result = vkCreateAndroidSurfaceKHR(instance, &surface_create_info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        out << "\nvulkan renderer create android surface=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan renderer create android surface=ok";

    uint32_t physical_device_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr);
    if (result != VK_SUCCESS || physical_device_count == 0) {
        out << "\nvulkan renderer enumerate physical devices=fail result=" << vk_result_string(result)
            << " count=" << physical_device_count;
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    std::vector<VkPhysicalDevice> devices(physical_device_count);
    result = vkEnumeratePhysicalDevices(instance, &physical_device_count, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        out << "\nvulkan renderer enumerate physical devices=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }

    VkPhysicalDevice selected_device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selected_properties{};
    VkSurfaceCapabilitiesKHR selected_capabilities{};
    VkSurfaceFormatKHR selected_surface_format{};
    VkPresentModeKHR selected_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t selected_queue_family = 0;
    uint32_t selected_format_count = 0;
    uint32_t selected_present_mode_count = 0;
    bool selected_software = true;

    for (const auto candidate : devices) {
        if (!device_extension_present(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        if (queue_family_count > 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, queue_families.data());
        }
        for (uint32_t queue_index = 0; queue_index < queue_family_count; ++queue_index) {
            if ((queue_families[queue_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 surface_supported = VK_FALSE;
            result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, queue_index, surface, &surface_supported);
            if (result != VK_SUCCESS || surface_supported != VK_TRUE) {
                continue;
            }
            VkSurfaceCapabilitiesKHR capabilities{};
            result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(candidate, surface, &capabilities);
            if (result != VK_SUCCESS) {
                continue;
            }
            uint32_t format_count = 0;
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &format_count, nullptr);
            if (result != VK_SUCCESS || format_count == 0) {
                continue;
            }
            std::vector<VkSurfaceFormatKHR> formats(format_count);
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &format_count, formats.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                continue;
            }
            uint32_t present_mode_count = 0;
            result = vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &present_mode_count, nullptr);
            if (result != VK_SUCCESS || present_mode_count == 0) {
                continue;
            }
            std::vector<VkPresentModeKHR> present_modes(present_mode_count);
            result = vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &present_mode_count, present_modes.data());
            if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
                continue;
            }
            selected_device = candidate;
            selected_properties = properties;
            selected_capabilities = capabilities;
            selected_surface_format = choose_surface_format(formats);
            selected_present_mode = choose_present_mode(present_modes);
            selected_queue_family = queue_index;
            selected_format_count = format_count;
            selected_present_mode_count = present_mode_count;
            selected_software = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
                                vulkan_name_looks_software(properties.deviceName);
            break;
        }
        if (selected_device != VK_NULL_HANDLE) {
            break;
        }
    }

    if (selected_device == VK_NULL_HANDLE) {
        out << "\nvulkan renderer select device=fail";
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan renderer physical devices count=" << physical_device_count;
    out << "\nvulkan renderer device=" << selected_properties.deviceName;
    out << "\nvulkan renderer device type=" << vulkan_device_type_string(selected_properties.deviceType);
    out << "\nvulkan renderer queue family index=" << selected_queue_family;
    out << "\nvulkan renderer surface formats count=" << selected_format_count;
    out << "\nvulkan renderer present modes count=" << selected_present_mode_count;
    out << "\nvulkan renderer surface format=" << vulkan_format_string(selected_surface_format.format);
    out << "\nvulkan renderer present mode=" << vulkan_present_mode_string(selected_present_mode);
    out << "\nvulkan renderer software renderer=" << (selected_software ? "true" : "false");

    const float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_create_info{};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = selected_queue_family;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_create_info{};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;
    device_create_info.enabledExtensionCount = 1;
    device_create_info.ppEnabledExtensionNames = device_extensions;
    result = vkCreateDevice(selected_device, &device_create_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        out << "\nvulkan renderer create device=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan renderer create device=ok";

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, selected_queue_family, 0, &queue);

    uint32_t image_count = selected_capabilities.minImageCount + 1;
    if (selected_capabilities.maxImageCount > 0 && image_count > selected_capabilities.maxImageCount) {
        image_count = selected_capabilities.maxImageCount;
    }
    const VkExtent2D extent = choose_swapchain_extent(selected_capabilities, window);
    out << "\nvulkan renderer swapchain extent=" << extent.width << "x" << extent.height;
    out << "\nvulkan renderer swapchain image count=" << image_count;

    VkSwapchainCreateInfoKHR swapchain_create_info{};
    swapchain_create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_create_info.surface = surface;
    swapchain_create_info.minImageCount = image_count;
    swapchain_create_info.imageFormat = selected_surface_format.format;
    swapchain_create_info.imageColorSpace = selected_surface_format.colorSpace;
    swapchain_create_info.imageExtent = extent;
    swapchain_create_info.imageArrayLayers = 1;
    swapchain_create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_create_info.preTransform = selected_capabilities.currentTransform;
    swapchain_create_info.compositeAlpha = choose_composite_alpha(selected_capabilities);
    swapchain_create_info.presentMode = selected_present_mode;
    swapchain_create_info.clipped = VK_TRUE;
    result = vkCreateSwapchainKHR(device, &swapchain_create_info, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
        out << "\nvulkan create swapchain=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan create swapchain=ok";

    uint32_t swapchain_image_count = 0;
    result = vkGetSwapchainImagesKHR(device, swapchain, &swapchain_image_count, nullptr);
    if (result != VK_SUCCESS || swapchain_image_count == 0) {
        out << "\nvulkan get swapchain images=fail result=" << vk_result_string(result)
            << " count=" << swapchain_image_count;
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    std::vector<VkImage> swapchain_images(swapchain_image_count);
    result = vkGetSwapchainImagesKHR(device, swapchain, &swapchain_image_count, swapchain_images.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        out << "\nvulkan get swapchain images=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan swapchain images count=" << swapchain_image_count;

    image_views.reserve(swapchain_images.size());
    for (const auto image : swapchain_images) {
        VkImageViewCreateInfo view_create_info{};
        view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_create_info.image = image;
        view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_create_info.format = selected_surface_format.format;
        view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_create_info.subresourceRange.levelCount = 1;
        view_create_info.subresourceRange.layerCount = 1;
        VkImageView image_view = VK_NULL_HANDLE;
        result = vkCreateImageView(device, &view_create_info, nullptr, &image_view);
        if (result != VK_SUCCESS) {
            out << "\nvulkan create image view=fail result=" << vk_result_string(result);
            out << "\nhost vulkan surface present supported=false";
            out << "\nhost vulkan surface hardware render=false";
            cleanup();
            return out.str();
        }
        image_views.push_back(image_view);
    }
    out << "\nvulkan image views count=" << image_views.size();

    VkAttachmentDescription color_attachment{};
    color_attachment.format = selected_surface_format.format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference color_attachment_ref{};
    color_attachment_ref.attachment = 0;
    color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment_ref;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo render_pass_create_info{};
    render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_create_info.attachmentCount = 1;
    render_pass_create_info.pAttachments = &color_attachment;
    render_pass_create_info.subpassCount = 1;
    render_pass_create_info.pSubpasses = &subpass;
    render_pass_create_info.dependencyCount = 1;
    render_pass_create_info.pDependencies = &dependency;
    result = vkCreateRenderPass(device, &render_pass_create_info, nullptr, &render_pass);
    if (result != VK_SUCCESS) {
        out << "\nvulkan create render pass=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan create render pass=ok";

    framebuffers.reserve(image_views.size());
    for (const auto image_view : image_views) {
        VkFramebufferCreateInfo framebuffer_create_info{};
        framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_create_info.renderPass = render_pass;
        framebuffer_create_info.attachmentCount = 1;
        framebuffer_create_info.pAttachments = &image_view;
        framebuffer_create_info.width = extent.width;
        framebuffer_create_info.height = extent.height;
        framebuffer_create_info.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        result = vkCreateFramebuffer(device, &framebuffer_create_info, nullptr, &framebuffer);
        if (result != VK_SUCCESS) {
            out << "\nvulkan create framebuffer=fail result=" << vk_result_string(result);
            out << "\nhost vulkan surface present supported=false";
            out << "\nhost vulkan surface hardware render=false";
            cleanup();
            return out.str();
        }
        framebuffers.push_back(framebuffer);
    }
    out << "\nvulkan framebuffers count=" << framebuffers.size();

    VkCommandPoolCreateInfo command_pool_create_info{};
    command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_create_info.queueFamilyIndex = selected_queue_family;
    result = vkCreateCommandPool(device, &command_pool_create_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        out << "\nvulkan create command pool=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }

    VkCommandBufferAllocateInfo command_buffer_allocate_info{};
    command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_allocate_info.commandPool = command_pool;
    command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_allocate_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &command_buffer_allocate_info, &command_buffer);
    if (result != VK_SUCCESS) {
        out << "\nvulkan allocate command buffer=fail result=" << vk_result_string(result);
        out << "\nhost vulkan surface present supported=false";
        out << "\nhost vulkan surface hardware render=false";
        cleanup();
        return out.str();
    }
    out << "\nvulkan command buffer=ok";

    int rendered = 0;
    int dropped = 0;
    VkResult last_acquire = VK_SUCCESS;
    VkResult last_present = VK_SUCCESS;
    std::string last_tag;
    for (size_t i = 0; i < frames.size(); ++i) {
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkSemaphore render_finished = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo semaphore_create_info{};
        semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        result = vkCreateSemaphore(device, &semaphore_create_info, nullptr, &image_available);
        if (result == VK_SUCCESS) {
            result = vkCreateSemaphore(device, &semaphore_create_info, nullptr, &render_finished);
        }
        if (result != VK_SUCCESS) {
            out << "\nvulkan surface frame " << (i + 1) << " semaphore=fail result=" << vk_result_string(result);
            if (image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, image_available, nullptr);
            }
            break;
        }

        uint32_t image_index = 0;
        last_acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, image_available, VK_NULL_HANDLE, &image_index);
        if (last_acquire != VK_SUCCESS && last_acquire != VK_SUBOPTIMAL_KHR) {
            out << "\nvulkan surface frame " << (i + 1) << " acquire=fail result=" << vk_result_string(last_acquire);
            vkDestroySemaphore(device, render_finished, nullptr);
            vkDestroySemaphore(device, image_available, nullptr);
            break;
        }

        result = vkResetCommandBuffer(command_buffer, 0);
        if (result != VK_SUCCESS) {
            out << "\nvulkan surface frame " << (i + 1) << " reset-command=fail result=" << vk_result_string(result);
            vkDestroySemaphore(device, render_finished, nullptr);
            vkDestroySemaphore(device, image_available, nullptr);
            break;
        }
        VkCommandBufferBeginInfo command_buffer_begin_info{};
        command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        result = vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info);
        if (result != VK_SUCCESS) {
            out << "\nvulkan surface frame " << (i + 1) << " begin-command=fail result=" << vk_result_string(result);
            vkDestroySemaphore(device, render_finished, nullptr);
            vkDestroySemaphore(device, image_available, nullptr);
            break;
        }
        const auto& frame = frames[i];
        VkClearValue clear_value{};
        clear_value.color.float32[0] = frame.red;
        clear_value.color.float32[1] = frame.green;
        clear_value.color.float32[2] = frame.blue;
        clear_value.color.float32[3] = 1.0F;
        VkRenderPassBeginInfo render_pass_begin_info{};
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.renderPass = render_pass;
        render_pass_begin_info.framebuffer = framebuffers[image_index];
        render_pass_begin_info.renderArea.offset = VkOffset2D{0, 0};
        render_pass_begin_info.renderArea.extent = extent;
        render_pass_begin_info.clearValueCount = 1;
        render_pass_begin_info.pClearValues = &clear_value;
        vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command_buffer);
        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS) {
            out << "\nvulkan surface frame " << (i + 1) << " end-command=fail result=" << vk_result_string(result);
            vkDestroySemaphore(device, render_finished, nullptr);
            vkDestroySemaphore(device, image_available, nullptr);
            break;
        }

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_available;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished;
        result = vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            out << "\nvulkan surface frame " << (i + 1) << " submit=fail result=" << vk_result_string(result);
            vkDestroySemaphore(device, render_finished, nullptr);
            vkDestroySemaphore(device, image_available, nullptr);
            break;
        }

        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        last_present = vkQueuePresentKHR(queue, &present_info);
        const VkResult wait_idle = vkQueueWaitIdle(queue);
        out << "\nvulkan surface frame " << (i + 1) << " tag=" << frame.tag
            << " image=" << image_index
            << " color=" << frame.red << "," << frame.green << "," << frame.blue
            << " acquire=" << vk_result_string(last_acquire)
            << " present=" << vk_result_string(last_present)
            << " wait=" << vk_result_string(wait_idle);
        vkDestroySemaphore(device, render_finished, nullptr);
        vkDestroySemaphore(device, image_available, nullptr);
        if ((last_present != VK_SUCCESS && last_present != VK_SUBOPTIMAL_KHR) || wait_idle != VK_SUCCESS) {
            break;
        }
        ++rendered;
        last_tag = frame.tag;
    }
    dropped = static_cast<int>(frames.size()) - rendered;
    out << "\nvulkan surface frames rendered=" << rendered;
    out << "\nvulkan surface frames dropped=" << dropped;
    out << "\nvulkan surface frame lossless=" << (dropped == 0 ? "true" : "false");
    out << "\nvulkan surface last guest command tag=" << (last_tag.empty() ? "missing" : last_tag);
    out << "\nvulkan surface last acquire=" << vk_result_string(last_acquire);
    out << "\nvulkan surface last present=" << vk_result_string(last_present);
    out << "\nhost vulkan surface present supported=true";
    out << "\nhost vulkan surface hardware render="
        << (!selected_software && rendered == static_cast<int>(frames.size()) && rendered > 0 ? "true" : "false");
    out << "\nguest vulkan clear present hardware render="
        << (!selected_software && rendered == static_cast<int>(frames.size()) && rendered > 0 ? "true" : "false");

    cleanup();
    return out.str();
}

std::string render_to_android_surface_frames(JNIEnv* env, jobject surface_obj, const std::string& encoded_frames) {
    const auto frames = parse_surface_frames(encoded_frames);
    std::ostringstream out;
    out << "host gpu surface renderer=android-surface-egl-gles";
    out << "\nsurface frame stream protocol=gui-compositor-clear-color-v4";
    out << "\nsurface requested frames=" << frames.size();
    if (surface_obj == nullptr) {
        out << "\nsurface render=fail reason=null-surface";
        return out.str();
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface_obj);
    if (window == nullptr) {
        out << "\nsurface render=fail reason=ANativeWindow_fromSurface";
        return out.str();
    }
    out << "\nsurface window=ok width=" << ANativeWindow_getWidth(window)
        << " height=" << ANativeWindow_getHeight(window);

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        out << "\nsurface egl display=fail error=" << egl_error_hex();
        ANativeWindow_release(window);
        return out.str();
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display, &major, &minor) != EGL_TRUE) {
        out << "\nsurface egl initialize=fail error=" << egl_error_hex();
        ANativeWindow_release(window);
        return out.str();
    }
    out << "\nsurface egl initialize=ok version=" << major << "." << minor;

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint config_count = 0;
    if (eglChooseConfig(display, config_attribs, &config, 1, &config_count) != EGL_TRUE || config_count < 1) {
        out << "\nsurface egl choose config=fail error=" << egl_error_hex();
        eglTerminate(display);
        ANativeWindow_release(window);
        return out.str();
    }
    out << "\nsurface egl choose config=ok count=" << config_count;

    EGLSurface egl_surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (egl_surface == EGL_NO_SURFACE) {
        out << "\nsurface egl window surface=fail error=" << egl_error_hex();
        eglTerminate(display);
        ANativeWindow_release(window);
        return out.str();
    }
    out << "\nsurface egl window surface=ok";

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (context == EGL_NO_CONTEXT) {
        out << "\nsurface egl context=fail error=" << egl_error_hex();
        eglDestroySurface(display, egl_surface);
        eglTerminate(display);
        ANativeWindow_release(window);
        return out.str();
    }
    out << "\nsurface egl context=ok api=gles2";

    if (eglMakeCurrent(display, egl_surface, egl_surface, context) != EGL_TRUE) {
        out << "\nsurface egl make current=fail error=" << egl_error_hex();
        eglDestroyContext(display, context);
        eglDestroySurface(display, egl_surface);
        eglTerminate(display);
        ANativeWindow_release(window);
        return out.str();
    }
    out << "\nsurface egl make current=ok";

    const int width = std::max(1, ANativeWindow_getWidth(window));
    const int height = std::max(1, ANativeWindow_getHeight(window));
    glViewport(0, 0, width, height);
    const auto vendor = safe_gl_string(GL_VENDOR);
    const auto renderer = safe_gl_string(GL_RENDERER);
    const bool software = renderer_looks_software(vendor, renderer);
    out << "\nsurface gl vendor=" << vendor;
    out << "\nsurface gl renderer=" << renderer;

    int rendered = 0;
    int wayland_rendered = 0;
    int x11_rendered = 0;
    int other_rendered = 0;
    GLenum last_gl_error = GL_NO_ERROR;
    EGLBoolean last_swapped = EGL_FALSE;
    std::string last_tag;
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        glClearColor(frame.red, frame.green, frame.blue, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        last_gl_error = glGetError();
        last_swapped = eglSwapBuffers(display, egl_surface);
        out << "\nsurface frame " << (i + 1) << " tag=" << frame.tag
            << " color=" << frame.red << "," << frame.green << "," << frame.blue
            << " gl_error=0x" << std::hex << last_gl_error << std::dec
            << " swap=" << (last_swapped == EGL_TRUE ? "ok" : "fail");
        if (last_swapped != EGL_TRUE) {
            out << " error=" << egl_error_hex();
            break;
        }
        if (last_gl_error != GL_NO_ERROR) {
            break;
        }
        ++rendered;
        if (frame.tag.rfind("WAYLAND-", 0) == 0) {
            ++wayland_rendered;
        } else if (frame.tag.rfind("X11-", 0) == 0) {
            ++x11_rendered;
        } else {
            ++other_rendered;
        }
        last_tag = frame.tag;
    }
    const int dropped = static_cast<int>(frames.size()) - rendered;
    out << "\nsurface wayland frames rendered=" << wayland_rendered;
    out << "\nsurface x11 frames rendered=" << x11_rendered;
    out << "\nsurface other frames rendered=" << other_rendered;
    out << "\nsurface gui total frames rendered=" << (wayland_rendered + x11_rendered);
    out << "\nsurface frames rendered=" << rendered;
    out << "\nsurface frames dropped=" << dropped;
    out << "\nsurface frame lossless=" << (dropped == 0 ? "true" : "false");
    out << "\nsurface last guest command tag=" << (last_tag.empty() ? "missing" : last_tag);
    out << "\nsurface gl clear error=0x" << std::hex << last_gl_error << std::dec;
    out << "\nsurface egl swap buffers=" << (last_swapped == EGL_TRUE ? "ok" : "fail");
    out << "\nsurface gpu software renderer=" << (software ? "true" : "false");
    out << "\nsurface gpu hardware render=" << (!software && last_gl_error == GL_NO_ERROR && last_swapped == EGL_TRUE ? "true" : "false");
    out << "\nguest gpu ipc bridge hardware render=" << (!software && rendered == static_cast<int>(frames.size()) && rendered > 0 ? "true" : "false");
    out << "\nguest gui gpu compositor hardware render=" << (!software && rendered == static_cast<int>(frames.size()) && rendered > 0 ? "true" : "false");
    out << "\nguest wayland/x11 gui gpu surface hardware render=" << (!software && rendered == static_cast<int>(frames.size()) && rendered > 0 ? "true" : "false");
    out << "\nguest gpu bridge hardware render=" << (!software && rendered > 0 ? "true" : "false");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, egl_surface);
    eglTerminate(display);
    ANativeWindow_release(window);
    return out.str();
}

void append_dlopen_probe(std::ostringstream& out, const std::string& path, const std::string& label) {
    dlerror();
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* error = dlerror();
        out << "\ndlopen " << label << "=fail: " << (error == nullptr ? "unknown" : error);
        return;
    }
    out << "\ndlopen " << label << "=ok";
    if (label == "libtalloc.so") {
        using VersionFn = int (*)();
        dlerror();
        auto major = reinterpret_cast<VersionFn>(dlsym(handle, "talloc_version_major"));
        const char* major_error = dlerror();
        dlerror();
        auto minor = reinterpret_cast<VersionFn>(dlsym(handle, "talloc_version_minor"));
        const char* minor_error = dlerror();
        if (major != nullptr && minor != nullptr) {
            out << " version=" << major() << "." << minor();
        } else {
            out << " version_symbol_error="
                << (major_error != nullptr ? major_error : "")
                << (minor_error != nullptr ? minor_error : "");
        }
    }
    dlclose(handle);
}

#ifdef ALR_HAVE_WAYLAND
// Presents committed wl_shm buffers from the in-app Wayland compositor onto the
// SurfaceView's ANativeWindow using the proven EGL/GLES2 path. The compositor
// invokes present() on its own thread per surface commit, so EGL is initialized
// lazily on that thread (GL contexts are thread-affine). shm ARGB8888 is BGRA in
// memory, so the fragment shader swizzles .bgra; UVs are V-flipped (shm top-left
// vs GL bottom-left).
struct WaylandPresenter {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0;
    GLuint texture = 0;                  // legacy single-surface texture
    GLint a_pos = -1;
    GLint a_uv = -1;
    GLint u_tex = -1;
    ANativeWindow* window = nullptr;
    std::vector<uint8_t> repack;         // scratch for padded-stride wl_shm buffers
    bool init_done = false;
    bool init_ok = false;
    std::string status = "no-frame-yet";
    int frames = 0;

    // Per-surface texture cache for multi-surface compositing. Keyed by the
    // compositor's stable surface_key; we re-upload a texture only when that
    // surface's content_serial changes. Textures whose surface vanished are reaped
    // each frame (any key not seen in the incoming list is deleted).
    //
    // M2 zero-copy: when AHardwareBuffer + EGLImage + external-OES are available
    // (proven by the M1 probe), each surface caches an AHB sized to its content.
    // On a content change we memcpy the committed pixels into the AHB's locked CPU
    // plane (replacing the old CPU->GPU glTexImage2D transfer — ~33 MB/frame at 4K)
    // and the GPU samples the AHB directly via a GL_TEXTURE_EXTERNAL_OES texture.
    // Falls back to the glTexImage2D path (tex) when AHB is unavailable.
    struct CachedTex {
        GLuint tex = 0;               // sampler2D texture (glTexImage2D fallback)
        GLuint ext_tex = 0;           // GL_TEXTURE_EXTERNAL_OES (AHB import)
        AHardwareBuffer* ahb = nullptr;
        void* image = nullptr;        // EGLImageKHR (void* to avoid header in struct)
        int ahb_w = 0, ahb_h = 0;     // AHB allocation size (realloc on resize)
        bool use_ahb = false;         // this surface is on the zero-copy path
        uint64_t content_serial = 0;
        bool seen = false;
        // §5-C zero-copy import of a WS-2-OWNED AHB (distinct from `ahb` above, which
        // the presenter allocates for the shm-opt path). We own ext_image (the
        // EGLImage view) but must NEVER release ext_ahb — WS-2 owns that buffer.
        void* ext_image = nullptr;            // EGLImageKHR over the WS-2 AHB
        AHardwareBuffer* ext_ahb = nullptr;   // WS-2 AHB, last imported (NOT owned)
    };
    std::map<uint64_t, CachedTex> tex_cache;
    std::mutex present_mutex;            // serialize present() vs present_list()

    // ---- M2 AHardwareBuffer zero-copy plumbing (resolved once at init) ----
    bool ahb_ready = false;             // all extensions + procs present
    GLuint ext_program = 0;             // samplerExternalOES program (for AHB path)
    GLint ext_a_pos = -1, ext_a_uv = -1, ext_u_tex = -1;
    // §5-C GPU path: a second external-OES program sampling .rgba (WS-2 renders its
    // AHB as R8G8B8A8_UNORM = RGBA in memory, vs the shm AHB which is BGRA -> .bgra).
    GLuint ext_program_rgba = 0;
    GLint ext_rgba_a_pos = -1, ext_rgba_a_uv = -1, ext_rgba_u_tex = -1;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_get_native_buf = nullptr;
    PFNEGLCREATEIMAGEKHRPROC p_create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC p_destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_image_target_tex = nullptr;
    int ahb_uploads = 0;                // count of zero-copy AHB content uploads
    int gltex_uploads = 0;             // count of fallback glTexImage2D uploads

    static GLuint compile(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool ensure_init() {
        if (init_done) {
            return init_ok;
        }
        init_done = true;
        if (window == nullptr) {
            status = "egl-init-fail:no-window";
            return false;
        }
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint major = 0, minor = 0;
        if (display == EGL_NO_DISPLAY || eglInitialize(display, &major, &minor) != EGL_TRUE) {
            status = "egl-init-fail:display";
            return false;
        }
        const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig cfg = nullptr;
        EGLint n = 0;
        if (eglChooseConfig(display, cfg_attribs, &cfg, 1, &n) != EGL_TRUE || n < 1) {
            status = "egl-init-fail:config";
            return false;
        }
        surface = eglCreateWindowSurface(display, cfg, window, nullptr);
        if (surface == EGL_NO_SURFACE) {
            status = "egl-init-fail:surface";
            return false;
        }
        const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, cfg, EGL_NO_CONTEXT, ctx_attribs);
        if (context == EGL_NO_CONTEXT ||
            eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
            status = "egl-init-fail:context";
            return false;
        }
        const char* vsrc =
            "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;"
            "void main(){ vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }";
        const char* fsrc =
            "precision mediump float; varying vec2 vUv; uniform sampler2D uTex;"
            "void main(){ gl_FragColor = texture2D(uTex, vUv).bgra; }";
        GLuint vs = compile(GL_VERTEX_SHADER, vsrc);
        GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc);
        if (vs == 0 || fs == 0) {
            status = "egl-init-fail:shader";
            return false;
        }
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            status = "egl-init-fail:link";
            return false;
        }
        a_pos = glGetAttribLocation(program, "aPos");
        a_uv = glGetAttribLocation(program, "aUv");
        u_tex = glGetUniformLocation(program, "uTex");
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // ---- M2: set up the AHardwareBuffer zero-copy path (best-effort). If any
        // piece is missing we leave ahb_ready=false and the present loop uses the
        // glTexImage2D fallback, so nothing regresses. ----
        init_ahb_path();

        init_ok = true;
        status = ahb_ready ? "egl-init-ok+ahb" : "egl-init-ok";
        return true;
    }

    // Resolve AHB/EGLImage/external-OES extensions + a samplerExternalOES program.
    void init_ahb_path() {
        const char* egl_exts = eglQueryString(display, EGL_EXTENSIONS);
        const auto* gl_exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        const std::string e = egl_exts ? egl_exts : "";
        const std::string g = gl_exts ? gl_exts : "";
        if (e.find("EGL_ANDROID_get_native_client_buffer") == std::string::npos ||
            e.find("EGL_KHR_image_base") == std::string::npos ||
            g.find("GL_OES_EGL_image_external") == std::string::npos) {
            return;  // not supported -> glTexImage2D fallback
        }
        p_get_native_buf = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        p_create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
            eglGetProcAddress("eglCreateImageKHR"));
        p_destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
        p_image_target_tex = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!p_get_native_buf || !p_create_image || !p_destroy_image || !p_image_target_tex) {
            return;
        }
        // External-OES sampler program. Same V-flip + .bgra swizzle as the sampler2D
        // path (AHB is R8G8B8A8 holding the wl_shm BGRA-in-memory bytes).
        const char* vsrc =
            "attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;"
            "void main(){ vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }";
        const char* fsrc =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision mediump float; varying vec2 vUv;"
            "uniform samplerExternalOES uTex;"
            "void main(){ gl_FragColor = texture2D(uTex, vUv).bgra; }";
        GLuint vs = compile(GL_VERTEX_SHADER, vsrc);
        GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc);
        if (vs == 0 || fs == 0) return;
        ext_program = glCreateProgram();
        glAttachShader(ext_program, vs);
        glAttachShader(ext_program, fs);
        glLinkProgram(ext_program);
        GLint linked = 0;
        glGetProgramiv(ext_program, GL_LINK_STATUS, &linked);
        if (!linked) { glDeleteProgram(ext_program); ext_program = 0; return; }
        ext_a_pos = glGetAttribLocation(ext_program, "aPos");
        ext_a_uv = glGetAttribLocation(ext_program, "aUv");
        ext_u_tex = glGetUniformLocation(ext_program, "uTex");
        // §5-C GPU-path program: identical, but samples .rgba (WS-2 AHB is RGBA, not
        // the shm BGRA). Non-fatal if it fails — the GPU branch just won't engage.
        const char* fsrc_rgba =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision mediump float; varying vec2 vUv;"
            "uniform samplerExternalOES uTex;"
            "void main(){ gl_FragColor = texture2D(uTex, vUv).rgba; }";
        GLuint vs2 = compile(GL_VERTEX_SHADER, vsrc);
        GLuint fs2 = compile(GL_FRAGMENT_SHADER, fsrc_rgba);
        if (vs2 != 0 && fs2 != 0) {
            ext_program_rgba = glCreateProgram();
            glAttachShader(ext_program_rgba, vs2);
            glAttachShader(ext_program_rgba, fs2);
            glLinkProgram(ext_program_rgba);
            GLint l2 = 0;
            glGetProgramiv(ext_program_rgba, GL_LINK_STATUS, &l2);
            if (!l2) { glDeleteProgram(ext_program_rgba); ext_program_rgba = 0; }
            else {
                ext_rgba_a_pos = glGetAttribLocation(ext_program_rgba, "aPos");
                ext_rgba_a_uv = glGetAttribLocation(ext_program_rgba, "aUv");
                ext_rgba_u_tex = glGetUniformLocation(ext_program_rgba, "uTex");
            }
        }
        ahb_ready = true;
    }

    // Allocate/realloc a surface's AHB to (w,h), import it as an external-OES
    // texture, and copy `pixels` (tight BGRA, w*4 stride) into it. Returns false on
    // any failure (caller falls back to glTexImage2D). Zero per-frame GPU upload:
    // the GPU samples the AHB directly; only a CPU memcpy into the AHB occurs (and
    // the compositor already produced these tight CPU pixels, so it's one copy, not
    // an extra GPU transfer).
    bool ahb_upload(CachedTex& ct, int w, int h, const void* pixels) {
        if (!ahb_ready || w <= 0 || h <= 0 || pixels == nullptr) return false;
        if (ct.ahb == nullptr || ct.ahb_w != w || ct.ahb_h != h) {
            ahb_release(ct);
            AHardwareBuffer_Desc d{};
            d.width = static_cast<uint32_t>(w);
            d.height = static_cast<uint32_t>(h);
            d.layers = 1;
            d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                      AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
            if (AHardwareBuffer_allocate(&d, &ct.ahb) != 0 || ct.ahb == nullptr) {
                ct.ahb = nullptr;
                return false;
            }
            ct.ahb_w = w; ct.ahb_h = h;
            EGLClientBuffer cb = p_get_native_buf(ct.ahb);
            const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            EGLImageKHR img = cb ? p_create_image(display, EGL_NO_CONTEXT,
                                                  EGL_NATIVE_BUFFER_ANDROID, cb, attribs)
                                 : EGL_NO_IMAGE_KHR;
            if (img == EGL_NO_IMAGE_KHR) { ahb_release(ct); return false; }
            ct.image = img;
            if (ct.ext_tex == 0) glGenTextures(1, &ct.ext_tex);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, ct.ext_tex);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            p_image_target_tex(GL_TEXTURE_EXTERNAL_OES,
                               static_cast<GLeglImageOES>(ct.image));
        }
        // Copy the committed pixels into the AHB CPU plane, honoring its stride.
        AHardwareBuffer_Desc got{};
        AHardwareBuffer_describe(ct.ahb, &got);
        void* dst = nullptr;
        if (AHardwareBuffer_lock(ct.ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1,
                                 nullptr, &dst) != 0 || dst == nullptr) {
            return false;
        }
        const int tight = w * 4;
        const uint32_t dst_stride = (got.stride ? got.stride : static_cast<uint32_t>(w)) * 4;
        const auto* src = static_cast<const uint8_t*>(pixels);
        auto* d8 = static_cast<uint8_t*>(dst);
        if (dst_stride == static_cast<uint32_t>(tight)) {
            std::memcpy(d8, src, static_cast<size_t>(tight) * h);
        } else {
            for (int y = 0; y < h; ++y) {
                std::memcpy(d8 + static_cast<size_t>(y) * dst_stride,
                            src + static_cast<size_t>(y) * tight,
                            static_cast<size_t>(tight));
            }
        }
        AHardwareBuffer_unlock(ct.ahb, nullptr);
        ct.use_ahb = true;
        return true;
    }

    void ahb_release(CachedTex& ct) {
        if (ct.image != nullptr && p_destroy_image) {
            p_destroy_image(display, static_cast<EGLImageKHR>(ct.image));
        }
        ct.image = nullptr;
        if (ct.ahb) { AHardwareBuffer_release(ct.ahb); ct.ahb = nullptr; }
        ct.ahb_w = ct.ahb_h = 0;
    }

    // §5-C zero-copy: import a WS-2-OWNED AHB as an external-OES texture. No allocate,
    // no CPU lock/memcpy, and we NEVER release the AHB (WS-2 owns it). Re-imports only
    // when the AHB pointer changes; if WS-2 re-renders into the SAME AHB the EGLImage
    // view samples the new content with no re-import.
    bool import_external_ahb(CachedTex& ct, AHardwareBuffer* extahb) {
        if (!ahb_ready || extahb == nullptr) return false;
        if (ct.ext_ahb == extahb && ct.ext_tex != 0 && ct.ext_image != nullptr)
            return true;  // same buffer already imported -> reuse
        if (ct.ext_image != nullptr && p_destroy_image) {
            p_destroy_image(display, static_cast<EGLImageKHR>(ct.ext_image));
            ct.ext_image = nullptr;
        }
        EGLClientBuffer cb = p_get_native_buf(extahb);
        const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        EGLImageKHR img = cb ? p_create_image(display, EGL_NO_CONTEXT,
                                              EGL_NATIVE_BUFFER_ANDROID, cb, attribs)
                             : EGL_NO_IMAGE_KHR;
        if (img == EGL_NO_IMAGE_KHR) return false;
        ct.ext_image = img;
        ct.ext_ahb = extahb;
        if (ct.ext_tex == 0) glGenTextures(1, &ct.ext_tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, ct.ext_tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_image_target_tex(GL_TEXTURE_EXTERNAL_OES, static_cast<GLeglImageOES>(ct.ext_image));
        return true;
    }

    // Free the EGLImage view over a WS-2 AHB (NOT the AHB — WS-2 owns it).
    void ext_image_release(CachedTex& ct) {
        if (ct.ext_image != nullptr && p_destroy_image)
            p_destroy_image(display, static_cast<EGLImageKHR>(ct.ext_image));
        ct.ext_image = nullptr;
        ct.ext_ahb = nullptr;
    }

    // Upload a tightly-packed (stride==w*4) BGRA-in-memory buffer into `tex`.
    // Used by both the legacy single-surface path (after its own repack) and the
    // multi-surface path (the compositor already delivers tight rows).
    void upload_tight(GLuint tex, int w, int h, const void* pixels) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
    }

    // Draw the currently-bound texture as a quad covering the NDC rect
    // [x0,x1] x [y0,y1]. UVs are V-flipped (wl_shm top-left vs GL bottom-left).
    // `ap`/`au` are the active program's aPos/aUv attribute locations (the AHB
    // external-OES program and the sampler2D program have their own).
    void draw_quad_ndc_attr(GLint ap, GLint au, float x0, float y0, float x1, float y1) {
        const GLfloat verts[] = {
            x0, y0, 0.0f, 1.0f,
            x1, y0, 1.0f, 1.0f,
            x0, y1, 0.0f, 0.0f,
            x1, y1, 1.0f, 0.0f,
        };
        glEnableVertexAttribArray(static_cast<GLuint>(ap));
        glVertexAttribPointer(static_cast<GLuint>(ap), 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), verts);
        glEnableVertexAttribArray(static_cast<GLuint>(au));
        glVertexAttribPointer(static_cast<GLuint>(au), 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(GLfloat), verts + 2);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    void draw_quad_ndc(float x0, float y0, float x1, float y1) {
        draw_quad_ndc_attr(a_pos, a_uv, x0, y0, x1, y1);
    }

    // ---- legacy single-surface present (unchanged behaviour) ----
    void present(const alr::wayland::PresentFrame& f) {
        std::lock_guard<std::mutex> lk(present_mutex);
        if (!ensure_init() || f.pixels == nullptr || f.width <= 0 || f.height <= 0) {
            return;
        }
        const int win_w = std::max(1, ANativeWindow_getWidth(window));
        const int win_h = std::max(1, ANativeWindow_getHeight(window));
        glViewport(0, 0, win_w, win_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(u_tex, 0);
        // wl_shm buffers may have stride > width*4 (row padding). GLES2 has no
        // GL_UNPACK_ROW_LENGTH, so repack non-tight rows into a scratch buffer.
        const int tight = f.width * 4;
        const void* upload = f.pixels;
        if (f.stride > 0 && f.stride != tight) {
            repack.resize(static_cast<std::size_t>(tight) * static_cast<std::size_t>(f.height));
            const auto* src = static_cast<const uint8_t*>(f.pixels);
            for (int y = 0; y < f.height; ++y) {
                std::memcpy(repack.data() + static_cast<std::size_t>(y) * tight,
                            src + static_cast<std::size_t>(y) * f.stride,
                            static_cast<std::size_t>(tight));
            }
            upload = repack.data();
        }
        upload_tight(texture, f.width, f.height, upload);
        draw_quad_ndc(-1.0f, -1.0f, 1.0f, 1.0f);
        eglSwapBuffers(display, surface);
        ++frames;
        status = "presented-frame";
    }

    // ---- multi-surface present: clear once, paint each placed quad bottom->top,
    // swap once. Each surface keeps its own cached texture, re-uploaded only when
    // its content_serial changes. Alpha blending is enabled so dialogs/popups with
    // transparent corners (GTK rounded windows) composite cleanly over the window
    // beneath them. ----
    void present_list(const std::vector<alr::wayland::PresentSurface>& surfaces,
                      int32_t out_w, int32_t out_h) {
        std::lock_guard<std::mutex> lk(present_mutex);
        if (!ensure_init()) return;

        const int win_w = std::max(1, ANativeWindow_getWidth(window));
        const int win_h = std::max(1, ANativeWindow_getHeight(window));
        // Placement rects were computed against (out_w,out_h); map them to the
        // physical window. Normally these match (config output == SurfaceView).
        const int ref_w = out_w > 0 ? out_w : win_w;
        const int ref_h = out_h > 0 ? out_h : win_h;

        glViewport(0, 0, win_w, win_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied-alpha over

        // Mark all cached textures unseen; reap the unseen ones after the pass.
        for (auto& kv : tex_cache) kv.second.seen = false;

        // `surfaces` is already ordered bottom->top by the compositor; paint as-is.
        for (const alr::wayland::PresentSurface& s : surfaces) {
            if ((s.pixels == nullptr && s.ahb == nullptr) ||
                s.width <= 0 || s.height <= 0) continue;
            CachedTex& ct = tex_cache[s.surface_key];
            ct.seen = true;  // mark before any no-draw skip so it isn't reaped
            const bool content_changed = (ct.content_serial != s.content_serial);

            // §5-C GPU zero-copy: the surface carries a WS-2-rendered AHB. Import it
            // directly (no allocate / lock / memcpy) and sample via the .rgba
            // external-OES program (WS-2 AHB is R8G8B8A8_UNORM). Highest precedence.
            bool drawn_via_gpu = false;
            if (s.ahb != nullptr && ahb_ready && ext_program_rgba != 0) {
                if (import_external_ahb(ct, static_cast<AHardwareBuffer*>(s.ahb))) {
                    if (content_changed) { ++ahb_uploads; ct.content_serial = s.content_serial; }
                    glUseProgram(ext_program_rgba);
                    glUniform1i(ext_rgba_u_tex, 0);
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ct.ext_tex);
                    drawn_via_gpu = true;
                }
            }

            // Shm zero-copy path: memcpy committed pixels into a presenter-owned AHB
            // and sample via external-OES (.bgra). On content change we memcpy into the
            // AHB; otherwise the existing import is reused.
            bool drawn_via_ahb = false;
            if (!drawn_via_gpu && s.pixels != nullptr && ahb_ready) {
                if (content_changed) {
                    if (ahb_upload(ct, s.width, s.height, s.pixels)) {
                        ++ahb_uploads;
                        ct.content_serial = s.content_serial;
                    }
                }
                if (ct.use_ahb && ct.ext_tex != 0) {
                    glUseProgram(ext_program);
                    glUniform1i(ext_u_tex, 0);
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, ct.ext_tex);
                    drawn_via_ahb = true;
                }
            }

            if (!drawn_via_gpu && !drawn_via_ahb) {
                if (s.pixels == nullptr) continue;  // GPU import failed + no shm pixels
                // Fallback: sampler2D + glTexImage2D (CPU->GPU upload on change).
                if (ct.tex == 0) {
                    glGenTextures(1, &ct.tex);
                    glBindTexture(GL_TEXTURE_2D, ct.tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                glUseProgram(program);
                glUniform1i(u_tex, 0);
                if (content_changed) {
                    upload_tight(ct.tex, s.width, s.height, s.pixels);
                    ++gltex_uploads;
                    ct.content_serial = s.content_serial;
                } else {
                    glBindTexture(GL_TEXTURE_2D, ct.tex);
                }
            }
            // Placement rect (output px, top-left origin, +Y down) -> NDC (bottom-up).
            const float x0 = (static_cast<float>(s.dst_x) / ref_w) * 2.0f - 1.0f;
            const float x1 = (static_cast<float>(s.dst_x + s.dst_w) / ref_w) * 2.0f - 1.0f;
            const float y_top    = 1.0f - (static_cast<float>(s.dst_y) / ref_h) * 2.0f;
            const float y_bottom = 1.0f - (static_cast<float>(s.dst_y + s.dst_h) / ref_h) * 2.0f;
            // draw_quad_ndc_attr(...,x0,y0,x1,y1): y0=bottom. The WS-2 GPU AHB is GL
            // bottom-up but reads top-left as a texture -> SAME V-flip as the shm path.
            if (drawn_via_gpu) {
                draw_quad_ndc_attr(ext_rgba_a_pos, ext_rgba_a_uv, x0, y_bottom, x1, y_top);
            } else if (drawn_via_ahb) {
                draw_quad_ndc_attr(ext_a_pos, ext_a_uv, x0, y_bottom, x1, y_top);
            } else {
                draw_quad_ndc_attr(a_pos, a_uv, x0, y_bottom, x1, y_top);
            }
        }

        // Reap textures + AHBs for surfaces that disappeared.
        for (auto it = tex_cache.begin(); it != tex_cache.end();) {
            if (!it->second.seen) {
                if (it->second.tex != 0) glDeleteTextures(1, &it->second.tex);
                if (it->second.ext_tex != 0) glDeleteTextures(1, &it->second.ext_tex);
                ahb_release(it->second);
                ext_image_release(it->second);  // §5-C: free the WS-2 AHB EGLImage view (not the AHB)
                it = tex_cache.erase(it);
            } else {
                ++it;
            }
        }

        glDisable(GL_BLEND);
        eglSwapBuffers(display, surface);
        ++frames;
        // Report which upload path is in use so the zero-copy win is observable:
        // ahb=<n> zero-copy AHB uploads vs gltex=<n> glTexImage2D fallback uploads.
        status = std::string("presented-scene ahb=") + std::to_string(ahb_uploads) +
                 " gltex=" + std::to_string(gltex_uploads) +
                 (ahb_ready ? " path=zerocopy" : " path=gltex-fallback");
    }
};

WaylandPresenter g_wl_presenter;

#endif  // ALR_HAVE_WAYLAND

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeRuntimeReport(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = alr::build_runtime_report(input, alr::select_execution_backend(alr::ExecutionBackendKind::Proot));
    const auto launch = alr::build_loader_launch_plan(input);
    const auto proot = alr::build_proot_launch_plan(input);
    const auto alr_runtime = alr::build_alr_runtime_launch_plan(input);

    std::ostringstream out;
    out << report.text << "\n\nloader argv:";
    for (const auto& arg : launch.argv) {
        out << "\n  " << arg;
    }
    out << "\n\nloader env:";
    out << "\n  ALR_ROOTFS=" << launch.env.at("ALR_ROOTFS");
    out << "\n  ALR_PROGRAM=" << launch.env.at("ALR_PROGRAM");
    out << "\n  PATH=" << launch.env.at("PATH");
    out << "\n\nproot argv:";
    for (const auto& arg : proot.argv) {
        out << "\n  " << arg;
    }
    out << "\n\nproot env:";
    out << "\n  ALR_ROOTFS=" << proot.env.at("ALR_ROOTFS");
    out << "\n  ALR_PROGRAM=" << proot.env.at("ALR_PROGRAM");
    out << "\n  PROOT_LOADER=" << proot.env.at("PROOT_LOADER");
    out << "\n  PROOT_TMP_DIR=" << proot.env.at("PROOT_TMP_DIR");
    out << "\n  PROOT_NO_SECCOMP=" << proot.env.at("PROOT_NO_SECCOMP");
    out << "\n  PROOT_VERBOSE=" << proot.env.at("PROOT_VERBOSE");
    out << "\n  LD_LIBRARY_PATH=" << proot.env.at("LD_LIBRARY_PATH");
    out << "\n  PATH=" << proot.env.at("PATH");
    out << "\n\nalr runtime argv:";
    for (const auto& arg : alr_runtime.argv) {
        out << "\n  " << arg;
    }
    out << "\n\nalr runtime env:";
    out << "\n  ALR_ROOTFS=" << alr_runtime.env.at("ALR_ROOTFS");
    out << "\n  ALR_PROGRAM=" << alr_runtime.env.at("ALR_PROGRAM");
    out << "\n  ALR_BACKEND=" << alr_runtime.env.at("ALR_BACKEND");
    out << "\n  ALR_HOOK_PATH=" << alr_runtime.env.at("ALR_HOOK_PATH");
    out << "\n  ALR_INTERPOSER_PATH=" << alr_runtime.env.at("ALR_INTERPOSER_PATH");
    out << "\n  ALR_BRIDGE_PATH=" << alr_runtime.env.at("ALR_BRIDGE_PATH");
    out << "\n  ALR_CONFIG_FORMAT=" << alr_runtime.env.at("ALR_CONFIG_FORMAT");
    out << "\n  ALR_FAKE_ROOT=" << alr_runtime.env.at("ALR_FAKE_ROOT");
    out << "\n  ALR_TRACE_PATH=" << alr_runtime.env.at("ALR_TRACE_PATH");
    out << "\n  ALR_TRACE_EXEC=" << alr_runtime.env.at("ALR_TRACE_EXEC");
    out << "\n  PATH=" << alr_runtime.env.at("PATH");
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeLibraryProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring native_library_dir) {
    const auto dir = jstring_to_string(env, native_library_dir);
    std::ostringstream out;
    append_dlopen_probe(out, join_path(dir, "libtalloc.so"), "libtalloc.so");
    append_dlopen_probe(out, join_path(dir, "libalr_proot.so"), "libalr_proot.so");
    append_dlopen_probe(out, join_path(dir, "libproot-loader.so"), "libproot-loader.so");
    append_dlopen_probe(out, join_path(dir, "libalr_runtime_launcher.so"), "libalr_runtime_launcher.so");
    append_dlopen_probe(out, join_path(dir, "libalr_runtime_hook.so"), "libalr_runtime_hook.so");
    append_dlopen_probe(out, join_path(dir, "libalr_runtime_interposer.so"), "libalr_runtime_interposer.so");
    return env->NewStringUTF(out.str().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrTrampolineContinueProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = run_packaged_trampoline_continue_probe(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrProcfsVirtualizationProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = build_procfs_virtualization_report(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrWxSafeExecProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = build_wx_safe_exec_report(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrPerfComparisonProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = build_perf_comparison_report(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrInterposeProcfsProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = build_interposer_procfs_report(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrMemfdExecProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = build_memfd_exec_probe(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrSyscallSandboxProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_syscall_capability_probe();
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrSeccompPathTrapProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring app_files_dir,
    jstring app_cache_dir) {
    alr::RuntimeReportInput input{};
    input.app_files_dir = jstring_to_string(env, app_files_dir);
    input.app_cache_dir = jstring_to_string(env, app_cache_dir);
    const auto report = build_seccomp_pathtrap_probe(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrUnixSocketProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring app_files_dir,
    jstring app_cache_dir) {
    alr::RuntimeReportInput input{};
    input.app_files_dir = jstring_to_string(env, app_files_dir);
    input.app_cache_dir = jstring_to_string(env, app_cache_dir);
    const auto report = build_unix_socket_probe(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrExecmemProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_execmem_probe();
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuBoundaryProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_gpu_boundary_probe();
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrNativeLoaderProbe(
    JNIEnv* env,
    jobject /* thiz */,
    jstring package_name,
    jstring native_library_dir,
    jstring app_files_dir,
    jstring app_cache_dir,
    jstring rootfs_name,
    jstring program) {
    const auto input = alr::RuntimeReportInput{
        .package_name = jstring_to_string(env, package_name),
        .native_library_dir = jstring_to_string(env, native_library_dir),
        .app_files_dir = jstring_to_string(env, app_files_dir),
        .app_cache_dir = jstring_to_string(env, app_cache_dir),
        .rootfs_name = jstring_to_string(env, rootfs_name),
        .program = jstring_to_string(env, program),
    };
    const auto report = build_native_loader_probe(input);
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrNativeLoaderSelftest(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_native_loader_selftest();
    return env->NewStringUTF(report.c_str());
}


extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeHostGpuProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_host_gpu_probe_report();
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuMarshallingProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_gpu_marshalling_probe();
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrAhbZeroCopyProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_ahb_zerocopy_probe_report();
    // Mirror to logcat (tag alr_loader) so the AHB zero-copy verdict is observable
    // off-device without scraping the 1px report view.
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "ahb-zerocopy:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// GPU-native app track M1: decode a hand-built shader+VBO+texture+draw op stream
// on the real Mali GPU and pixel-verify (proves the marshalling decoder runs real
// GLES, not just clear/scissor). Mirrors to logcat (tag alr_loader).
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuDrawProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_draw_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "gpu-draw:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// GPU-native app track M2: push the same op stream through the SPSC command ring,
// drain it host-side, decode on Mali, pixel-verify (proves the ring transport +
// decoder together). Mirrors to logcat (tag alr_loader).
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuRingProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_ring_draw_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "gpu-ring:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// GPU-native app track M4 (host half): render the decoded guest draw into an
// AHardwareBuffer-backed FBO, then sample that AHB zero-copy (external-OES) —
// proves the guest-draw -> AHB -> presentable loop on Mali. Mirrors to logcat.
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuFboProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_fbo_present_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "gpu-fbo:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// M4 LIVE INTEGRATION: in-process two-thread keystone — a producer pushes the
// guest op stream through the SPSC ring while the host GpuExecutorService thread
// (own Mali GLES2 ctx + AHB-FBO) drains, decodes per frame, and presents, synced
// by the req_seq/reply_seq handshake. Proves the live ring->executor->AHB loop on
// Mali without the loader fork (that fork is the only remaining step after this).
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuLiveProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_live_integration_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "gpu-live:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// "% of native-Mali throughput": renders the SAME triangle op-stream on the SAME
// Mali two ways — DIRECT (decode+glFinish, no ring) vs the full ALR ring+executor
// pipeline — and reports the FPS ratio (= the GPU per-call marshalling overhead,
// §0(b)). glmark2 can't run on bare-Android Mali, so this same-op ring-vs-direct
// is the valid on-device "ALR vs native-Mali" measurement.
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuThroughputProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_gpu_throughput_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "gpu-throughput:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeHostVulkanProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = build_host_vulkan_probe_report();
    return env->NewStringUTF(report.c_str());
}

// §VK-M2: guest Vulkan enumerate/props REQUEST stream -> SPSC ring -> host decode on
// the REAL vendor Mali libvulkan -> reply stream -> guest decode. Proves the Vulkan
// marshalling path end-to-end on hardware (the Vulkan analogue of the GLES ring probe).
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuVkMarshalProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_vk_marshal_mali_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "vk-marshal:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// §VK-M2 body: guest Vulkan device/queue/command-buffer/clear-submit marshalled to
// the real Mali libvulkan, rendering a clear into an AHB-backed color attachment +
// CPU readback (the Vulkan analogue of the GLES draw/AHB path).
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuVkRenderProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_vk_render_mali_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "vk-render:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// R12-G3: VK render BREADTH — a real graphics-pipeline vkCmdDraw (shader modules +
// vertex buffer + draw), not just a clear, marshalled to real Mali and read back
// from the AHB. Gating line: "ALR VK DRAW MARSHAL: PASS".
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuVkDrawProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::gpu::run_vk_draw_mali_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "vk-draw:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeProbeVulkanSurface(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface) {
    const auto report = probe_vulkan_android_surface(env, surface);
    return env->NewStringUTF(report.c_str());
}

// STEP B-1: render a spinning textured cube THROUGH the live GPU pipeline (guest op
// stream -> SPSC ring -> host executor -> AHB) and present it onto this SurfaceView's
// ANativeWindow zero-copy via external-OES. In-process (no fork/rootfs); the visible
// payoff of the v118 live-integration backbone. Mirrored to logcat tag alr_loader.
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeAlrGpuScreenCube(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface,
    jint frames) {
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
    if (win == nullptr) {
        return env->NewStringUTF(
            "ALR GPU SCREEN CUBE: FAIL\nreason=ANativeWindow_fromSurface returned null");
    }
    const auto report = alr::gpu::run_screen_cube_demo(win, static_cast<int>(frames));
    ANativeWindow_release(win);
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "gpu-screen-cube:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

// Goal-2 (Chromium) prep: does V8-style iterative W^X executable memory work on this
// untrusted_app domain? PASS => V8 JIT viable without --jitless. Mirrored to logcat.
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeJitWxProbe(
    JNIEnv* env,
    jobject /* thiz */) {
    const auto report = alr::jit::run_jit_wx_cycle_probe();
    __android_log_print(ANDROID_LOG_INFO, "alr_loader", "jit-wx:\n%s", report.c_str());
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeRenderVulkanSurfaceFrames(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface,
    jstring encoded_frames) {
    const auto report = render_vulkan_to_android_surface_frames(env, surface, jstring_to_string(env, encoded_frames));
    return env->NewStringUTF(report.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeRenderGpuSurfaceFrames(
    JNIEnv* env,
    jobject /* thiz */,
    jobject surface,
    jstring encoded_frames) {
    const auto report = render_to_android_surface_frames(env, surface, jstring_to_string(env, encoded_frames));
    return env->NewStringUTF(report.c_str());
}

// Start the in-app Wayland compositor on its own thread, presenting committed
// client buffers onto the given SurfaceView via EGL/GLES. The guest connects to
// the AF_UNIX socket under <cacheDir>/alr-xdg (the loader injects
// WAYLAND_DISPLAY/XDG_RUNTIME_DIR into the guest env to match).
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandCompositorStart(
    JNIEnv* env,
    jobject /* thiz */,
    jstring cache_dir,
    jobject surface,
    jint density_dpi,
    jfloat xdpi,
    jfloat ydpi,
    jint out_width_px,
    jint out_height_px,
    jint refresh_mhz) {
#ifdef ALR_HAVE_WAYLAND
    const std::string cache = jstring_to_string(env, cache_dir);
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    g_wl_presenter.window = window;
    alr::wayland::CompositorConfig cfg;
    cfg.socket_path = cache + "/alr-xdg/wayland-0";
    if (window != nullptr) {
        cfg.output_width = ANativeWindow_getWidth(window);
        cfg.output_height = ANativeWindow_getHeight(window);
    }
    // Device-exact panel size + refresh from Kotlin (Display.getRealSize / refreshRate)
    // override the SurfaceView-derived size, so wl_output advertises the TRUE resolution
    // and refresh (1200x1920 @ 90Hz here) instead of a ~size / hardcoded-60Hz default.
    if (out_width_px > 0) cfg.output_width = out_width_px;
    if (out_height_px > 0) cfg.output_height = out_height_px;
    if (refresh_mhz > 0) cfg.output_refresh_mhz = refresh_mhz;
    // Device display metrics -> Wayland output resolution + DPI + integer scale,
    // so the guest GUI renders at the device's real resolution/density.
    cfg.density_dpi = density_dpi;
    cfg.xdpi = xdpi;
    cfg.ydpi = ydpi;
    int wl_scale = density_dpi > 0 ? (density_dpi + 80) / 160 : 1;  // round(dpi/160)
    if (wl_scale < 1) wl_scale = 1;
    // Keep logical width >= 1024 px so a desktop app (GIMP) isn't forced wider
    // than the screen on very high-density panels.
    while (wl_scale > 1 && cfg.output_width > 0 && cfg.output_width / wl_scale < 1024)
        --wl_scale;
    cfg.output_scale = wl_scale;
    cfg.present = [](const alr::wayland::PresentFrame& f) { g_wl_presenter.present(f); };
    cfg.present_list = [](const std::vector<alr::wayland::PresentSurface>& surfaces,
                          int32_t out_w, int32_t out_h) {
        g_wl_presenter.present_list(surfaces, out_w, out_h);
    };
    const std::string status = alr::wayland::alr_start_wayland_compositor(cfg);
    return env->NewStringUTF(status.c_str());
#else
    (void)env;
    (void)cache_dir;
    (void)surface;
    (void)density_dpi;
    (void)xdpi;
    (void)ydpi;
    (void)out_width_px;
    (void)out_height_px;
    (void)refresh_mhz;
    return env->NewStringUTF("ALR WAYLAND COMPOSITOR: not-built");
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandCompositorStatus(
    JNIEnv* env,
    jobject /* thiz */) {
#ifdef ALR_HAVE_WAYLAND
    std::ostringstream out;
    out << "ALR WAYLAND COMPOSITOR STATUS: "
        << (alr::wayland::alr_wayland_compositor_running() ? "running" : "stopped")
        << "\nalr wl present=" << g_wl_presenter.status
        << "\nalr wl frames=" << g_wl_presenter.frames;
    return env->NewStringUTF(out.str().c_str());
#else
    return env->NewStringUTF("ALR WAYLAND COMPOSITOR STATUS: not-built");
#endif
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandCompositorStop(
    JNIEnv* env,
    jobject /* thiz */) {
#ifdef ALR_HAVE_WAYLAND
    const std::string status = alr::wayland::alr_stop_wayland_compositor();
    return env->NewStringUTF(status.c_str());
#else
    return env->NewStringUTF("ALR WAYLAND COMPOSITOR: not-built");
#endif
}

// Inject a synthetic input burst (pointer + touch + key) at (x,y) to verify the
// input path end to end against a connected client.
extern "C" JNIEXPORT jstring JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandInjectSelfTest(
    JNIEnv* env,
    jobject /* thiz */,
    jfloat x,
    jfloat y) {
#ifdef ALR_HAVE_WAYLAND
    const int n = alr::wayland::alr_wayland_inject_selftest(x, y);
    std::ostringstream o;
    o << "ALR WAYLAND INJECT SELFTEST: queued=" << n;
    return env->NewStringUTF(o.str().c_str());
#else
    (void)x;
    (void)y;
    return env->NewStringUTF("ALR WAYLAND INJECT SELFTEST: not-built");
#endif
}

// Forward a real Android touch (phase: 0=down, 1=move, 2=up) to the focused
// client as BOTH wl_touch and wl_pointer events (toolkit-agnostic).
extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandInjectTouch(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint id,
    jfloat x,
    jfloat y,
    jint phase) {
#ifdef ALR_HAVE_WAYLAND
    alr::wayland::alr_wayland_inject_pointer_motion(x, y);
    if (phase == 0) {
        alr::wayland::alr_wayland_inject_pointer_button(0x110, 1);  // BTN_LEFT down
    } else if (phase == 2) {
        alr::wayland::alr_wayland_inject_pointer_button(0x110, 0);  // BTN_LEFT up
    }
    alr::wayland::alr_wayland_inject_touch(id, x, y, phase);
#else
    (void)id;
    (void)x;
    (void)y;
    (void)phase;
#endif
}

// Forward a real Android hardware/IME key to the focused client as a wl_keyboard
// key. evdevKey is a Linux <linux/input-event-codes.h> keycode (the Kotlin side
// translates Android KeyEvent.keyCode -> evdev); pressed is 1=down, 0=up.
extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandInjectKey(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jint evdevKey,
    jint pressed) {
#ifdef ALR_HAVE_WAYLAND
    alr::wayland::alr_wayland_inject_key(static_cast<uint32_t>(evdevKey),
                                         static_cast<uint32_t>(pressed));
#else
    (void)evdevKey;
    (void)pressed;
#endif
}

// Mouse wheel / trackpad scroll -> wl_pointer.axis. Position the pointer at (x,y)
// first so the axis targets the surface under the cursor and the pointer is entered.
// value is the Wayland axis amount (positive = down/right); axis 0=vertical, 1=horizontal.
extern "C" JNIEXPORT void JNICALL
Java_dev_chanwoo_androlinux_MainActivity_nativeWaylandInjectScroll(
    JNIEnv* /* env */,
    jobject /* thiz */,
    jfloat x,
    jfloat y,
    jdouble value,
    jint axis) {
#ifdef ALR_HAVE_WAYLAND
    alr::wayland::alr_wayland_inject_pointer_motion(x, y);
    alr::wayland::alr_wayland_inject_pointer_axis(value, static_cast<int32_t>(axis));
#else
    (void)x; (void)y; (void)value; (void)axis;
#endif
}
