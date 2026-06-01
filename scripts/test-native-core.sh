#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

cxx="${CXX:-g++}"
"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_runtime_plan_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_elf.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  app/src/main/cpp/alr_runtime/alr_launch.cpp \
  app/src/main/cpp/alr_runtime/alr_trampoline.cpp \
  app/src/main/cpp/runtime_plan.cpp \
  -o /tmp/alr-native-runtime-plan-test

/tmp/alr-native-runtime-plan-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_backend_policy_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_elf.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  app/src/main/cpp/alr_runtime/alr_launch.cpp \
  app/src/main/cpp/alr_runtime/alr_trampoline.cpp \
  app/src/main/cpp/runtime_plan.cpp \
  -o /tmp/alr-native-backend-policy-test

/tmp/alr-native-backend-policy-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_path_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_env.cpp \
  -o /tmp/alr-native-runtime-path-test

/tmp/alr-native-runtime-path-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_config_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  -o /tmp/alr-native-runtime-config-test

/tmp/alr-native-runtime-config-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_elf_test.cpp \
  app/src/main/cpp/alr_runtime/alr_elf.cpp \
  -o /tmp/alr-native-runtime-elf-test

/tmp/alr-native-runtime-elf-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_trampoline_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_elf.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  app/src/main/cpp/alr_runtime/alr_launch.cpp \
  app/src/main/cpp/alr_runtime/alr_trampoline.cpp \
  -o /tmp/alr-native-runtime-trampoline-test

/tmp/alr-native-runtime-trampoline-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  app/src/main/cpp/alr_runtime_trampoline.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  -o /tmp/alr-runtime-trampoline-host-smoke

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_trampoline_continue_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  -o /tmp/alr-native-runtime-trampoline-continue-test

/tmp/alr-native-runtime-trampoline-continue-test /tmp/alr-runtime-trampoline-host-smoke

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_exec_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  -o /tmp/alr-native-runtime-exec-test

/tmp/alr-native-runtime-exec-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_procfs_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  app/src/main/cpp/alr_runtime/alr_procfs.cpp \
  -o /tmp/alr-native-runtime-procfs-test

/tmp/alr-native-runtime-procfs-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_wx_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  app/src/main/cpp/alr_runtime/alr_wx.cpp \
  -o /tmp/alr-native-runtime-wx-test

/tmp/alr-native-runtime-wx-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_perf_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_perf.cpp \
  -o /tmp/alr-native-runtime-perf-test

/tmp/alr-native-runtime-perf-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_launch_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_elf.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  app/src/main/cpp/alr_runtime/alr_launch.cpp \
  app/src/main/cpp/alr_runtime/alr_trampoline.cpp \
  -o /tmp/alr-native-runtime-launch-test

/tmp/alr-native-runtime-launch-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_hook_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_hook.cpp \
  -o /tmp/alr-native-runtime-hook-test

/tmp/alr-native-runtime-hook-test

"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_alr_runtime_interposer_test.cpp \
  app/src/main/cpp/alr_runtime/alr_path.cpp \
  app/src/main/cpp/alr_runtime/alr_config.cpp \
  app/src/main/cpp/alr_runtime/alr_exec.cpp \
  app/src/main/cpp/alr_runtime/alr_procfs.cpp \
  app/src/main/cpp/alr_runtime/alr_interposer.cpp \
  -o /tmp/alr-native-runtime-interposer-test

/tmp/alr-native-runtime-interposer-test

# GPU command ring (Phase 4 / M2 transport): SPSC byte-ring integrity across
# wrap-around + the sync handshake. Header-only (alr_gpu/alr_gpu_ring.hpp), no
# GLES needed on the host.
"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_gpu_ring_test.cpp \
  -o /tmp/alr-native-gpu-ring-test

/tmp/alr-native-gpu-ring-test
