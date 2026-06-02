// alr_inproc_reexec.h — resident in-process re-exec trampoline (ADR-003-v3, R10 step2).
//
// The supervisor, at the guest's execve seccomp-trap, CANCELS the syscall
// (NT_ARM_SYSTEM_CALL=-1) and PC-redirects the tracee into this RESIDENT
// trampoline (compiled into the loader .so), which maps the new ELF IN-PROCESS
// (mmap PROT_EXEC, which W^X allows) and jumps to it — NO execve.
//
// ENTRY ABI (set by the supervisor via PTRACE_SETREGSET before resuming):
//   x19 = const char* host_target_path  (NUL-term, ABSOLUTE rootfs host path,
//                                         already rewritten; open() it directly)
//   x20 = char**       argv             (guest's original argv array, still valid)
//   x21 = char**       envp             (guest's original envp array, still valid;
//                                         carries LD_PRELOAD + ALR_ROOTFS)
//   x22 = const char*  rootfs_dir       (NUL-term rootfs dir; guest ld.so lives at
//                                         <rootfs_dir><PT_INTERP>)
// x19-x22 are callee-saved, so they survive the -ENOSYS return clobber of x0.
// The trampoline never returns: it maps and jumps, or _exit()s on fatal error.

#ifndef ALR_INPROC_REEXEC_H
#define ALR_INPROC_REEXEC_H

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn)) void alr_inproc_reexec_trampoline(void);

#ifdef __cplusplus
}
#endif

#endif  // ALR_INPROC_REEXEC_H
