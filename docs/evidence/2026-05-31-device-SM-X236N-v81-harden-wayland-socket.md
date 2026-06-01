# Device Evidence — SM-X236N, v81: supervisor hardening + Wayland socket transport viable

Two Phase-1/Phase-2 steps toward GUI: the multi-tracee supervisor is hardened (prerequisite for broadening the syscall surface), and the **Wayland transport substrate is proven** — a named `AF_UNIX` socket lets the host compositor and a forked guest exchange bytes, with no path mediation needed.

Device SM-X236N (MediaTek MT6878, Mali-G615, Android 16 / API 36, untrusted_app). APK `0.4.81-android-harden-socket-v81`, SHA-256 `3d7ca7babc60f4dd25e67f5e0c4c1cb765e7781cba87390ef231f7f815700cfc`.

## Result

```
ALR WAYLAND SOCKET TRANSPORT (named AF_UNIX host↔guest):  VIABLE
   socket_path=/data/user/0/dev.chanwoo.androlinux/files/alr-wl-test.sock
   bind=OK  listen=OK  accept=OK  byte_exchange=OK  guest_connect_exit=0

(no regression — all still PASS: glibc static / threads+fork / dynamic / env / id /
 dash -c / write-path / seccomp path-mediation)
```

## 1. Supervisor hardening (prerequisite, from the Phase-1 servicing design + loader audit)

Three correctness fixes to the multi-tracee supervisor, landed with zero behavior change for existing probes (all gates still green):

- **`si_code == SYS_SECCOMP` guard.** A `SIGSYS` is now only emulated as `-ENOSYS` when `PTRACE_GETSIGINFO` confirms it came from the seccomp filter. A *genuine* bad-syscall `SIGSYS` is delivered so the guest crashes visibly instead of silently continuing — essential as the syscall surface broadens for GUI apps.
- **Idempotency guard.** The path-mediation rewrite now skips a guest path that is *already* under `rootfs_dir` (e.g. one learned from `/proc/self/maps`, or pre-translated by a future `LD_PRELOAD` interposer), so we never produce `<rootfs><rootfs>/…`. This is the precondition for layering a fast-path interposer over the ptrace slow path.
- **Deterministic failure.** On `PTRACE_GETREGSET`/`SETREGSET` failure in the SIGSYS arm, the signal is now re-injected (not silently swallowed + counted as emulated).

## 2. Wayland socket transport (Phase 2/3 enabler)

The Phase-3 design decision is that the in-app Wayland compositor hosts a named `AF_UNIX` socket and the guest connects to it by an absolute Android path (with `XDG_RUNTIME_DIR` injected into the guest env), so **no socket-path mediation is required**. This probe proves that exact topology on-device: the app (compositor role) `bind`/`listen`s on `<filesDir>/alr-wl-test.sock`, a forked child (guest role, same UID/SELinux `untrusted_app` domain) `connect`s to that path and exchanges `PING`/`PONG`. All steps succeed. This confirms a stock `libwayland-client` guest can reach an in-process compositor with no new mediation — the transport for the whole GUI bridge.

(The supervisor *can* also rewrite a `sockaddr_un.sun_path` for guests that hardcode a `/run` or `/tmp/.X11-unix` path — designed, deferred — but Wayland with `XDG_RUNTIME_DIR` avoids needing it.)

## Position / next

Phase 1 (execution substrate) is now ~85%: + argv, + write-path, + hardened supervisor, + socket transport. The critical-path Phase 3 (a minimal real Wayland compositor presenting to the SurfaceView) is being scaffolded in parallel: vendor `libwayland-server` + `libffi` with the NDK, a compositor on its own app thread, `wl_shm` buffers uploaded to a GLES texture and drawn onto the `ANativeWindow` via the proven EGL path. Once it stands up, the next milestone is: start the compositor, run a stock guest `wl_client`/GTK over this socket, and see its first window on the Android screen.

Verification: pytest 235 passed, native-core PASS, validate-host PASS, Gradle BUILD SUCCESSFUL.
