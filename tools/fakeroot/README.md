# ALR non-root fakeroot LD_PRELOAD shim

`libalr_fakeroot.so` makes a non-root Android `untrusted_app` process believe it
is `uid 0` so `dpkg -i` / `apt install` can unpack a `.deb` without
`requires superuser privilege` and without `chown`/`chmod` `EPERM` aborting the
unpack.

- Source: `tools/fakeroot/libalr_fakeroot.c`
- Builder: `tools/build_fakeroot_overlay.py`
- Ships at (rootfs-absolute): `./usr/lib/androlinux/libalr_fakeroot.so`

It is a from-scratch slice of `fakeroot`'s `libfakeroot` — **no `faked` daemon,
no SysV IPC**. The fake-ownership DB lives in-process (a `(st_dev, st_ino)` hash),
which is all a single local-`.deb` unpack needs: dpkg chowns a freshly-unpacked
file then stats it within the same process; `execve()` re-loads the `.so` into
each maintainer-script child, which re-derives the DB by re-running its chowns.

## What it intercepts

| group        | symbols                                                       | behaviour                                   |
|--------------|---------------------------------------------------------------|---------------------------------------------|
| credentials  | `getuid geteuid getgid getegid getresuid getresgid`           | report the faked id (default `0:0`)         |
| set-id       | `setuid seteuid setgid setegid setre*id setres*id`            | no-op success                               |
| ownership    | `chown lchown fchown fchownat`                                | record `(uid,gid)` in the DB, return `0`    |
| mode         | `chmod fchmod fchmodat`                                       | try the real call; on `EPERM` record + `0`  |
| stat         | `stat lstat fstat fstatat newfstatat statx` (+ `*64`, `__*stat*`) | real call, then overlay faked owner/mode |

## Building (host)

```sh
python3 -m tools.build_fakeroot_overlay --out /tmp/fakeroot-stage.tar
# dry-run (no compiler): list the intercepted symbols
python3 -m tools.build_fakeroot_overlay --list
# print the device command (below)
python3 -m tools.build_fakeroot_overlay --device-cmd --rootfs /data/.../rootfs
```

The builder cross-compiles with `zig cc --target=aarch64-linux-gnu.2.36 -shared
-fPIC -O2` — the **same** toolchain/flags `scripts/build-interpose.sh` uses for
the path interposer.

## Device invocation (WS-1) — CHAINING IS LOAD-BEARING

ALR **always** injects `LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_interpose.so`
(see `runtime_report.cpp`). The fakeroot preload must **chain** with it, never
replace it — the two are orthogonal:

- `libalr_interpose.so` rewrites the **path argument** (guest abs path → `rootfs`+path)
- `libalr_fakeroot.so` rewrites **credentials** and the **stat result**

Each fakeroot wrapper calls the *next* implementation via `dlsym(RTLD_NEXT, …)`,
so with the fakeroot `.so` listed **first** the chain is:

```
guest stat("/x")
  → fakeroot stat()          [calls RTLD_NEXT stat]
    → interpose stat()       [rewrites "/x" → "<rootfs>/x", does the syscall]
  ← fakeroot patches buf->st_uid/st_gid/st_mode from the fake DB
```

The exact value to use (both paths are **rootfs-absolute host paths**, colon-joined,
fakeroot first):

```sh
LD_PRELOAD=<ROOTFS>/usr/lib/androlinux/libalr_fakeroot.so:<ROOTFS>/usr/lib/androlinux/libalr_interpose.so \
  ALR_ROOTFS=<ROOTFS> FAKEROOTUID=0 FAKEROOTGID=0 \
  dpkg --force-not-root --force-bad-path -i hello_2.10-3build1_arm64.deb
```

apt path (same env), forcing apt's sandbox user to root so it does not drop privs:

```sh
LD_PRELOAD=<ROOTFS>/usr/lib/androlinux/libalr_fakeroot.so:<ROOTFS>/usr/lib/androlinux/libalr_interpose.so \
  ALR_ROOTFS=<ROOTFS> FAKEROOTUID=0 FAKEROOTGID=0 \
  apt-get -o APT::Sandbox::User=root install ./hello_2.10-3build1_arm64.deb
```

`<ROOTFS>` is ALR's `config.rootfs_dir` (the on-device rootfs host dir). Generate
the exact line with `--device-cmd --rootfs <ROOTFS>`.

### Prereqs on device

- The R8 apt+dpkg staging overlay (`tools/build_apt_dpkg_overlay.py` →
  `apt-dpkg-stage.tar`) is extracted, so `dpkg`/`apt` and the
  `var/lib/dpkg` admindir exist.
- The test `.deb`: `python3 -m tools.build_apt_dpkg_overlay --fetch-test-deb <dir>`.

### Device gate

`dpkg -i hello.deb` succeeds non-root under the chained preload — `dpkg --status
hello` reports `Status: install ok installed` and the unpacked
`/usr/bin/hello` exists in the rootfs (`unpacked=true`).

## W^X / SELinux

Pure libc interposition: **no** executable memory, **no** seccomp filter, **no**
ptrace, **no** SELinux interaction. The fake identity is visible only inside the
process; an unpacked file is still owned by the real uid on disk. The shim is
strictly more permissive only *within its own process's view* and changes
nothing the kernel enforces.
