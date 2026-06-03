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

# GPU Vulkan enumerate/props marshalling (Phase 4 / VK-M2 first step): the
# request -> ring -> host-decode -> reply -> ring -> guest-decode round trip,
# proven host-side with a synthetic Mali provider (NO Vulkan SDK needed). The
# same codec runs on the real vendor libvulkan on device (ALR_VK_DECODE_REAL).
# Header-only (alr_gpu/alr_gpu_vk_{proto,decode,marshal_probe}.hpp).
"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_vk_marshal_test.cpp \
  -o /tmp/alr-native-vk-marshal-test

/tmp/alr-native-vk-marshal-test

# GENERATED Vulkan passthrough render batch (Phase 4 codegen): the first render-batch
# entrypoints emitted by tools/gen_vk_passthrough.py from vk.xml — device memory (the
# same-process MAP_SHARED arena, so vkMapMemory is a zero-copy local pointer), buffers,
# images, image views + their reqs/bind/destroy — driven through the SAME decode_vk_batch
# escape hand-off with a synthetic Mali provider (NO Vulkan SDK). Proves the generated
# encode/decode round trip AND the arena pointer is real + writable.
"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_vk_gen_passthrough_test.cpp \
  -o /tmp/alr-native-vk-gen-passthrough-test

/tmp/alr-native-vk-gen-passthrough-test

# Vulkan CMD-LOG recording band (the vkCmd* command-recording mechanism — the hard part of
# GPU full-passthrough): a guest-local per-command-buffer record log in the shared arena
# (vkBeginCommandBuffer..vkCmd*..vkEndCommandBuffer, NO ring op per call), the host replay
# round trip (the log decodes back to the same opcodes + handles the host replays into a real
# Mali VkCommandBuffer), and the vkQueueSubmit / fence / semaphore / wait sync ops over a
# DISJOINT escape sub-op band (0x4000) that coexists with the create-forwards codegen band.
# NO Vulkan SDK (synthetic Mali provider); the real-Mali replay is syntax-verified against
# the NDK vulkan.h by tests/test_vk_cmdlog.py.
"$cxx" -std=c++20 -Wall -Wextra -Werror \
  -Iapp/src/main/cpp \
  tests/native_vk_cmdlog_test.cpp \
  -o /tmp/alr-native-vk-cmdlog-test

/tmp/alr-native-vk-cmdlog-test
