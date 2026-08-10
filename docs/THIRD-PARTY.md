# Third-party components

This project is MIT (see [LICENSE](../LICENSE)), and it stays MIT because
nothing copyleft is linked into or shipped with it.

## Shipped in the APK

| Component | Origin | Licence |
|---|---|---|
| `runtime/alr` | this project | MIT |
| `libdl.so.2` stub | this project (`runtime/alr/src/preload/libdl_stub.c`) | MIT |

The libdl stub is written here rather than copied from a distribution
precisely so that no glibc-licensed binary is vendored. It reproduces what
Ubuntu's own stub does — export `__libdl_version_placeholder@GLIBC_2.17` and
nothing else — in 3 KB of our own source.

## Removed

**PRoot (GPL-2.0)** was bundled as `libalr_proot.so`, with `libtalloc.so` and
`libproot-loader.so`, and was the execution backend until alr replaced it. It
is gone: no binary, no source, no build target. Two reasons, and the licence
was the smaller one.

It could not `dpkg -i` in this app's SELinux domain, and at `targetSdk 28` it
stopped working altogether — its void-syscall cancellation lands in a SIGSYS
handler that returns `ENOSYS`, so `execve("/bin/hello")` failed with "Function
not implemented". A fallback that cannot run anything is worse than no
fallback: it invites a silent downgrade to a path nobody tests.

## Downloaded at runtime, never linked

The guest rootfs is a stock distribution image (Ubuntu 24.04 / 26.04
`ubuntu-base`, Debian) fetched and verified at provisioning time. Those images
carry their own licences and are not part of this project's source or binary
distribution — the same relationship a virtual machine has with the OS a user
installs into it.
