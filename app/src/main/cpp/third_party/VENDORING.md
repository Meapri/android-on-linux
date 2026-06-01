# Vendored third-party sources — ALR Wayland compositor (Phase 3)

This directory contains the canonical upstream sources required to build a real
`libwayland-server` based compositor into the Android app with the NDK, plus the
`wayland-scanner`-generated protocol glue (checked in, because no host
`wayland-scanner`/`pkg-config` is available on this machine).

Everything here builds **only for `arm64-v8a`** (see the gated block in
`../CMakeLists.txt`). The hand-written libffi config + the vendored libffi
assembly are aarch64-only; the real-device target is aarch64 (Mali / Android 16).
Other ABIs build the app without the compositor.

---

## Versions / provenance (exact)

| Component | Version | Upstream | Commit (pinned) |
|---|---|---|---|
| wayland (server) | **1.25.0** | https://gitlab.freedesktop.org/wayland/wayland | `3e673a438b0a9749e3bdf5cac4befac86333024c` |
| wayland-protocols (xdg-shell) | **1.48** | https://gitlab.freedesktop.org/wayland/wayland-protocols | `02e63e74a807afed95bc25a386173110afef24e3` |
| libffi | **3.5.2** | https://github.com/libffi/libffi | `e2eda0cf72a0598b44278cc91860ea402273fa29` |

License: wayland + wayland-protocols are MIT (HDMI-style X11/MIT); libffi is the
libffi (MIT-style) license. The license text lives in each upstream tree; only
the source files needed to build are vendored here.

---

## Directory layout

```
third_party/
  VENDORING.md                      (this file)
  wayland/
    config.h                        HAND-WRITTEN. Replaces meson/autotools config.h
                                    for aarch64-linux-android (bionic, API>=26).
    src/                            Server-side closure of libwayland (verbatim 1.25.0):
      config.h                      shim: #include "../config.h"
      wayland-server.c  event-loop.c  wayland-os.c  connection.c
      wayland-shm.c     wayland-util.c
      *.h                           wayland-server{,-core,-private}.h, wayland-private.h,
                                    wayland-os.h, wayland-util.h, timespec-util.h,
                                    wayland-version.h (generated from .h.in, see below)
    protocol/wayland.xml            provenance for the generated glue
  protocols/xdg-shell/xdg-shell.xml provenance for the generated glue
  wayland_generated/                CHECKED-IN wayland-scanner output (see "Regenerating"):
    wayland-server-protocol.h   wayland-protocol.c
    xdg-shell-server-protocol.h  xdg-shell-protocol.c
    wayland-client-protocol.h   xdg-shell-client-protocol.h   (for the guest test client)
  libffi/
    include/
      ffi.h                         GENERATED from ffi.h.in (token substitution, see below)
      ffi_common.h  ffi_cfi.h  tramp.h  ffitarget.h   (verbatim 3.5.2; ffitarget.h = src/aarch64/ffitarget.h)
      android-aarch64/fficonfig.h    HAND-WRITTEN. autotools-equivalent config for
                                    aarch64-linux-android (FFI_MMAP_EXEC_WRIT, no
                                    memfd — see note in the file).
    src/
      prep_cif.c types.c raw_api.c java_raw_api.c closures.c tramp.c dlmalloc.c
      aarch64/{ffi.c, sysv.S, internal.h}
                                    (dlmalloc.c is #include'd by closures.c, NOT a
                                     standalone TU — do not add it to CMake sources.)
```

---

## Hand-written (non-upstream) files — review these

1. `wayland/config.h` — the meson/autotools `config.h`. Sets `HAVE_ACCEPT4`,
   `HAVE_MEMFD_CREATE` (server only uses `fcntl(F_GET_SEALS)` under this, never
   calls `memfd_create`), `HAVE_GETTID`, `_GNU_SOURCE`. Leaves BSD-only
   `HAVE_SYS_UCRED_H`/`HAVE_XUCRED_CR_PID`/`HAVE_BROKEN_MSG_CMSG_CLOEXEC` unset.
2. `wayland/src/config.h` — one-line shim so both `#include "config.h"` and
   `#include "../config.h"` resolve.
3. `libffi/include/ffi.h` — generated from `ffi.h.in` with these substitutions
   for aarch64-linux-android: `@VERSION@`→`3.5.2`, `@TARGET@`→`AARCH64`,
   `@HAVE_LONG_DOUBLE@`→`1`, `@FFI_EXEC_TRAMPOLINE_TABLE@`→`0` (Android uses
   `FFI_MMAP_EXEC_WRIT`, not the iOS-style table), `@FFI_VERSION_STRING@`→`3.5.2`,
   `@FFI_VERSION_NUMBER@`→`30502`.
4. `libffi/include/android-aarch64/fficonfig.h` — autotools-equivalent. Key
   choices: `FFI_MMAP_EXEC_WRIT=1`, `FFI_EXEC_STATIC_TRAMP=1`,
   **`HAVE_MEMFD_CREATE` left UNDEFINED** (bionic only declares `memfd_create`
   for API>=30; we target 26, so closures.c falls back to `mkostemp`).

---

## Regenerating the checked-in protocol glue

No `wayland-scanner` / `pkg-config` exists on this host, so a host build of
`wayland-scanner` from the vendored 1.25.0 source was used. To reproduce
(macOS host, libexpat from the Xpat in the macOS SDK — no Homebrew needed):

```sh
# 0) get the upstream trees (matching the pins above)
git clone --depth 1 --branch 1.25.0 https://gitlab.freedesktop.org/wayland/wayland.git
git clone --depth 1 --branch 1.48   https://gitlab.freedesktop.org/wayland/wayland-protocols.git
git clone --depth 1 --branch v3.5.2 https://github.com/libffi/libffi.git

# 1) generate wayland-version.h + wayland.dtd.h, then build wayland-scanner on the HOST
cd wayland
sed -e 's/@WAYLAND_VERSION_MAJOR@/1/' -e 's/@WAYLAND_VERSION_MINOR@/25/' \
    -e 's/@WAYLAND_VERSION_MICRO@/0/' -e 's/@WAYLAND_VERSION@/1.25.0/' \
    src/wayland-version.h.in > src/wayland-version.h
python3 src/embed.py protocol/wayland.dtd wayland_dtd > src/wayland.dtd.h
SDK=$(xcrun --show-sdk-path)            # provides <expat.h> + libexpat.tbd
cc -O2 -D_GNU_SOURCE -I src src/scanner.c src/wayland-util.c -lexpat -o wayland-scanner
#   (built WITHOUT -DHAVE_LIBXML: that flag only adds DTD *validation*, output is identical)

# 2) emit the server glue + client headers (checked into wayland_generated/)
W=protocol/wayland.xml ; X=../wayland-protocols/stable/xdg-shell/xdg-shell.xml
./wayland-scanner server-header $W wayland-server-protocol.h
./wayland-scanner private-code   $W wayland-protocol.c
./wayland-scanner server-header $X xdg-shell-server-protocol.h
./wayland-scanner private-code   $X xdg-shell-protocol.c
./wayland-scanner client-header $W wayland-client-protocol.h
./wayland-scanner client-header $X xdg-shell-client-protocol.h
```

> Use `private-code` (not `public-code`): the marshalling tables are linked
> statically into the app and not re-exported.

If a host `wayland-scanner` IS available (Linux dev box), the above collapses to
the four `wayland-scanner {server-header,private-code} ...` calls.

---

## Build notes (CMake)

The appended block in `../CMakeLists.txt`:
* `enable_language(C)` + `enable_language(ASM)` — the top-level `project()`
  declares only `CXX`; libffi/wayland are C and libffi has one `.S`. (Symptom if
  missing: `CMAKE_C_COMPILE_OBJECT not set`.)
* `ffi` STATIC — libffi C sources + `aarch64/sysv.S`, include dirs point at the
  generated `ffi.h`, the `android-aarch64/fficonfig.h`, and `src/aarch64` for
  `internal.h`. Compiled with `-w` (third-party warnings must not trip
  `-Werror`). `LINKER_LANGUAGE C` set (mixed C/ASM static lib).
* `wayland_server` STATIC — the 6 server `.c` + the 2 generated `*-protocol.c`,
  `-DHAVE_ACCEPT4=1 -D_GNU_SOURCE=1 -DWL_HIDE_DEPRECATED=1`, links `ffi`.
* `alr_wayland` STATIC — our `alr_wayland/alr_compositor.cpp` (kept under the
  project's strict `-std=c++20 -Wall -Wextra -Werror`), links `wayland_server log android`.
* `alr_loader` gains `PRIVATE alr_wayland` + `-DALR_HAVE_WAYLAND=1`.

Verified: `:app:assembleDebug` → `BUILD SUCCESSFUL`; `libffi.a`,
`libwayland_server.a`, `libalr_wayland.a` all produced for `arm64-v8a`;
`alr_start_wayland_compositor` present in `libalr_wayland.a`; `wl_display_create`
present in `libwayland_server.a`.
