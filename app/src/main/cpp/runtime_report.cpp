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
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032  // (1027 is R_AARCH64_RELATIVE; IRELATIVE is 1032)
#endif

#include <chrono>

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

#include "alr_runtime/alr_config.hpp"
#include "alr_runtime/alr_exec.hpp"
#include "alr_runtime/alr_interposer.hpp"
#include "alr_runtime/alr_path.hpp"
#include "alr_runtime/alr_perf.hpp"
#include "alr_runtime/alr_procfs.hpp"
#include "alr_runtime/alr_wx.hpp"
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
    if (!interpose_off) {
        // R3 / bootstrap: LD_PRELOAD MUST be the ABSOLUTE ROOTFS HOST path, not the
        // guest path. In PCGATE=1 the loader no longer traces path syscalls, so ld.so's
        // open of the preload is NOT rewritten by the supervisor; a guest path
        // ("/usr/lib/...") would hit the HOST fs, the preload would fail to load, and the
        // interposer's PC-gate filter would never install (no speedup AND no mediation).
        // The host-absolute path opens with no mediation needed, and is idempotently
        // left alone by the PCGATE=0 supervisor too, so it is correct in both A/B arms.
        guest_env.push_back("LD_PRELOAD=" + config.rootfs_dir +
                            "/usr/lib/androlinux/libalr_interpose.so");
    }
    guest_env.push_back(pcgate_on ? "ALR_PCGATE=1" : "ALR_PCGATE=0");
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
        constexpr std::size_t kMaxEnv = 32;
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
        ::alarm(dynamic ? 25 : 5);
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
    // Multi-tracee supervisor: waitpid(-1, __WALL) catches the guest plus every
    // thread it clones and every process it forks/execs. Each blocked syscall
    // (SIGSYS) is emulated per-tracee; only blocked syscalls trap, so overhead
    // stays far below PRoot's trap-every-syscall model.
    while (true) {
        int status = 0;
        const pid_t w = ::waitpid(-1, &status, __WALL);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;  // ECHILD: every tracee has been reaped
        }
        if (WIFEXITED(status)) {
            mem_fd_evict(w);  // tid reaped: close its cached /proc/<tid>/mem fd
            if (w == pid) {
                code = WEXITSTATUS(status);
            }
            continue;
        }
        if (WIFSIGNALED(status)) {
            mem_fd_evict(w);  // tid killed: close its cached /proc/<tid>/mem fd
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
                // PCGATE=1 the loader also RET_TRACEs execve/execveat (a future exec
                // re-entry hook), so those traps reach this handler too — but only the
                // 9 path syscalls put the pathname in x1; execve's x1 is argv (a char**),
                // so blindly rewriting x1 would corrupt argv. Rewriting an exec'd program
                // path into the rootfs is a deliberate follow-up; until then never treat
                // an exec syscall's x1 as a path.
                const uint64_t sysno = regs[8];
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
            }
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
                // Group-stop: keep the thread parked with LISTEN (do NOT run it). This
                // is the single/multi-thread-safe handling that classic TRACEME lacked.
                // ESRCH (tid raced away) is benign — just loop.
                if (::ptrace(PTRACE_LISTEN, w, nullptr, nullptr) != 0 && errno == ESRCH) {
                    continue;
                }
                continue;
            }
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
        if (stopsig == SIGSTOP || stopsig == SIGTRAP) {
            // Under SEIZE this branch is reached only for a genuine SIGNAL-DELIVERY-stop
            // carrying SIGSTOP or SIGTRAP — NOT the attach sync (there is none: SEIZE
            // does not stop the child and the go-pipe handshake replaced the TRACEME
            // initial SIGSTOP), NOT a group-stop and NOT a new-thread/INTERRUPT stop
            // (both are PTRACE_EVENT_STOP, handled above). A defensive, idempotent
            // SETOPTIONS re-apply on a non-leader tid is kept purely as a backstop
            // (SEIZE already inherits options to clones); the single/few-thread guest
            // never enters this w!=pid path, so its behavior is byte-identical.
            if (w != pid) {
                ::ptrace(PTRACE_SETOPTIONS, w, nullptr,
                         reinterpret_cast<void*>(kSeizeOpts));
            }
            // Never forward SIGSTOP/SIGTRAP to the tracee: delivering SIGSTOP would
            // re-stop the thread group → deadlock, and SIGTRAP is the ptrace event
            // vehicle. Resume with signal 0 — unchanged from the TRACEME version for
            // the single-thread path.
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
                if (emulated_syscalls < 64) {
                    emulated_list[emulated_syscalls] = static_cast<int>(regs[8]);
                }
                // Return -ENOSYS (not 0). glibc ignores the result of
                // set_robust_list/rseq, but for syscalls it has fallbacks for
                // (e.g. faccessat2 -> faccessat) the fallback keys specifically on
                // ENOSYS; faking success (0) or EPERM would break it.
                regs[0] = static_cast<uint64_t>(-38);  // -ENOSYS
                ++emulated_syscalls;
                ::ptrace(PTRACE_SETREGSET, w, reinterpret_cast<void*>(NT_PRSTATUS), &io);
                ::ptrace(PTRACE_CONT, w, nullptr, nullptr);  // suppress SIGSYS
            } else {
                // Couldn't read/patch regs — don't silently swallow the SIGSYS;
                // deliver it so the failure is deterministic.
                ::ptrace(PTRACE_CONT, w, nullptr,
                         reinterpret_cast<void*>(static_cast<long>(stopsig)));
            }
            if (emulated_syscalls > 8192) {
                // Runaway backstop: kill the guest leader; the multi-tracee loop
                // reaps the rest as they're orphaned.
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
    // Supervisor loop exited (ECHILD: all tracees reaped). Per-exit eviction already
    // closed each tid's fd; close any survivors (e.g. a tid lost to the SIGKILL
    // runaway path before its WIFSIGNALED was observed) so no /proc/<tid>/mem fd leaks.
    for (const auto& kv : mem_fds) {
        if (kv.second >= 0) {
            ::close(kv.second);
        }
    }
    mem_fds.clear();
    std::string guest_stdout;
    std::string diag;
    try {
        guest_stdout = read_all_from_fd(out_pipe[0]);
        diag = read_all_from_fd(diag_pipe[0]);
    } catch (const std::exception& exc) {
        diag = exc.what();
    }
    ::close(out_pipe[0]);
    ::close(diag_pipe[0]);
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
    out << "\nalr native loader seccomp-emulated syscalls=" << emulated_syscalls;
    if (emulated_syscalls > 0) {
        out << " nums=";
        const int shown = emulated_syscalls < 12 ? emulated_syscalls : 12;
        for (int i = 0; i < shown; ++i) {
            out << emulated_list[i] << (i + 1 < shown ? "," : "");
        }
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
