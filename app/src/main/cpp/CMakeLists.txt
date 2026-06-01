cmake_minimum_required(VERSION 3.22.1)
project(androlinux_runtime_lab LANGUAGES CXX)

add_library(alr_runtime STATIC
    alr_runtime/alr_config.cpp
    alr_runtime/alr_elf.cpp
    alr_runtime/alr_exec.cpp
    alr_runtime/alr_launch.cpp
    alr_runtime/alr_path.cpp
    alr_runtime/alr_procfs.cpp
    alr_runtime/alr_wx.cpp
    alr_runtime/alr_perf.cpp
    alr_runtime/alr_trampoline.cpp
    alr_runtime/alr_env.cpp
    alr_runtime/alr_hook.cpp
    alr_runtime/alr_interposer.cpp
)
add_library(alr_loader SHARED runtime_report.cpp runtime_plan.cpp)
add_library(alr_runtime_launcher SHARED alr_runtime_launcher.cpp runtime_plan.cpp)
add_library(alr_runtime_hook SHARED alr_runtime_hook.cpp)
add_library(alr_runtime_interposer SHARED alr_runtime_interposer.cpp)
add_executable(alr_test_command test_command.cpp)
add_executable(alr_proot_candidate proot_candidate.cpp)
add_executable(alr_runtime_trampoline alr_runtime_trampoline.cpp)
set_target_properties(alr_test_command PROPERTIES OUTPUT_NAME "alr-test-command")
set_target_properties(alr_proot_candidate PROPERTIES OUTPUT_NAME "alr-proot-candidate")
set_target_properties(alr_runtime_trampoline PROPERTIES OUTPUT_NAME "alr-runtime-trampoline")

target_compile_features(alr_runtime PRIVATE cxx_std_20)
target_compile_features(alr_loader PRIVATE cxx_std_20)
target_compile_features(alr_runtime_launcher PRIVATE cxx_std_20)
target_compile_features(alr_runtime_hook PRIVATE cxx_std_20)
target_compile_features(alr_runtime_interposer PRIVATE cxx_std_20)
target_compile_features(alr_test_command PRIVATE cxx_std_20)
target_compile_features(alr_proot_candidate PRIVATE cxx_std_20)
target_compile_features(alr_runtime_trampoline PRIVATE cxx_std_20)

target_include_directories(alr_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(alr_loader PRIVATE alr_runtime log android EGL GLESv2 vulkan)
target_link_libraries(alr_runtime_launcher PRIVATE alr_runtime)
target_link_libraries(alr_runtime_hook PRIVATE alr_runtime)
target_link_libraries(alr_runtime_interposer PRIVATE alr_runtime)
target_link_libraries(alr_runtime_trampoline PRIVATE alr_runtime)

# ===========================================================================
# ALR Wayland compositor (Phase 3) — APPEND-ONLY BLOCK. Do not interleave with
# the lines above; everything the compositor needs is self-contained here so it
# can be merged/reverted as a unit.
#
# Vendored under third_party/: canonical libwayland-server 1.25.0 + libffi 3.5.2
# + wayland-scanner-generated protocol glue (wayland.xml, xdg-shell.xml).
# See third_party/VENDORING.md for exact versions / regeneration commands.
#
# The compositor uses aarch64-only libffi assembly (third_party/libffi/src/
# aarch64/sysv.S) and a hand-written aarch64 fficonfig.h, so the whole stack is
# built ONLY for arm64-v8a (the real-device target). Other ABIs build the app
# without the compositor (alr_loader simply isn't linked against it there).
# ===========================================================================
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(ALR_TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

    # The top-level project() above declares only CXX. libffi + libwayland are
    # C, and libffi has one .S (preprocessed assembly). Enable both here
    # (append-only; cannot touch project()). The earlier "CMAKE_C_COMPILE_OBJECT
    # not set" failure was simply C not being enabled — once C is on, ASM works.
    enable_language(C)
    enable_language(ASM)

    # ---- libffi (aarch64) ----
    add_library(ffi STATIC
        ${ALR_TP}/libffi/src/prep_cif.c
        ${ALR_TP}/libffi/src/types.c
        ${ALR_TP}/libffi/src/raw_api.c
        ${ALR_TP}/libffi/src/java_raw_api.c
        ${ALR_TP}/libffi/src/closures.c
        ${ALR_TP}/libffi/src/tramp.c
        ${ALR_TP}/libffi/src/aarch64/ffi.c
        ${ALR_TP}/libffi/src/aarch64/sysv.S
    )
    # Mixed C + ASM sources: tell CMake the (static) link language is C so it
    # doesn't fail with "can not determine linker language". sysv.S is assembled
    # by the ASM toolchain (clang -x assembler-with-cpp); it self-#defines
    # LIBFFI_ASM and includes fficonfig.h/ffitarget.h via the include dirs.
    set_target_properties(ffi PROPERTIES LINKER_LANGUAGE C)
    set_source_files_properties(${ALR_TP}/libffi/src/aarch64/sysv.S
        PROPERTIES INCLUDE_DIRECTORIES
        "${ALR_TP}/libffi/include;${ALR_TP}/libffi/include/android-aarch64;${ALR_TP}/libffi/src/aarch64")
    target_include_directories(ffi PUBLIC
        ${ALR_TP}/libffi/include
        ${ALR_TP}/libffi/include/android-aarch64   # fficonfig.h
        ${ALR_TP}/libffi/src/aarch64               # internal.h
    )
    # libffi expects FFI_HIDDEN/symbols from fficonfig.h; _GNU_SOURCE for memfd.
    target_compile_definitions(ffi PRIVATE _GNU_SOURCE=1)
    target_compile_definitions(ffi PUBLIC HAVE_AS_CFI_PSEUDO_OP=1)
    # Quiet libffi's own -Wall noise; it is third-party and must not fail -Werror.
    # (sysv.S self-#defines LIBFFI_ASM; do NOT pass it on the command line.)
    target_compile_options(ffi PRIVATE -w)

    # ---- libwayland-server (vendored .c + generated protocol marshalling) ----
    add_library(wayland_server STATIC
        ${ALR_TP}/wayland/src/wayland-server.c
        ${ALR_TP}/wayland/src/event-loop.c
        ${ALR_TP}/wayland/src/wayland-os.c
        ${ALR_TP}/wayland/src/connection.c
        ${ALR_TP}/wayland/src/wayland-shm.c
        ${ALR_TP}/wayland/src/wayland-util.c
        ${ALR_TP}/wayland_generated/wayland-protocol.c
        ${ALR_TP}/wayland_generated/xdg-shell-protocol.c
    )
    set_target_properties(wayland_server PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(wayland_server PUBLIC
        ${ALR_TP}/wayland/src          # wayland-server.h, config.h shim, etc.
        ${ALR_TP}/wayland_generated    # *-server-protocol.h
    )
    target_compile_definitions(wayland_server PRIVATE
        _GNU_SOURCE=1
        HAVE_ACCEPT4=1
        WL_HIDE_DEPRECATED=1)
    target_link_libraries(wayland_server PUBLIC ffi)
    # Vendored C; do not let upstream warnings trip -Werror.
    target_compile_options(wayland_server PRIVATE -w)

    # ---- ALR compositor glue (our code; keep strict flags) ----
    add_library(alr_wayland STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/alr_wayland/alr_compositor.cpp
    )
    target_compile_features(alr_wayland PRIVATE cxx_std_20)
    target_include_directories(alr_wayland PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(alr_wayland PUBLIC wayland_server log android)

    # Link the compositor into the app's loader target and announce availability
    # so runtime_report.cpp / MainActivity can call alr_start_wayland_compositor.
    target_link_libraries(alr_loader PRIVATE alr_wayland)
    target_compile_definitions(alr_loader PRIVATE ALR_HAVE_WAYLAND=1)
endif()
