# Agent Sync

Append-only coordination log for Codex, Hermes, and any other agent working on this repo.

Do not delete previous entries. Do not rewrite another agent's entry except to fix an obvious typo before it is consumed.

## 2026-05-15 - Codex - Documentation Baseline

Branch/worktree:
- Current local checkout at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `README.md`
- `docs/product-requirements.md`
- `docs/clean-room-protocol.md`
- `docs/alr-execution-backend-spec.md`
- `docs/android-graphics-bridge-spec.md`
- `docs/plans/implementation-milestones.md`
- `tests/test_project_documentation_bundle.py`
- `docs/agent-coordination.md`
- `docs/plans/parallel-workstreams.md`
- `docs/prompts/codex-clean-alr-runtime.md`
- `docs/prompts/hermes-proroot-ab-and-device-evidence.md`

What changed:
- Added the initial documentation baseline for product goals, clean-room rules, execution backend scope, graphics bridge scope, and multi-agent coordination.

Commands/tests:
- `scripts/test-native-core.sh` -> PASS
- `python3 -m pytest tests -q` -> SKIP, host Python is missing `pytest`

Evidence:
- Native core output: runtime plan, backend policy, and ALR path/env tests passed.

Blockers:
- `pytest` is not installed in the host Python used by `/Library/Developer/CommandLineTools/usr/bin/python3`.

Next recommended action:
- Hermes can begin optional backend/device evidence work using `docs/prompts/hermes-proroot-ab-and-device-evidence.md`.
- Codex can begin ALR launcher or path hook implementation using `docs/prompts/codex-clean-alr-runtime.md`.

## 2026-05-15 - Codex - GitHub Collaboration Setup

Branch/worktree:
- Current local checkout at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `.github/ISSUE_TEMPLATE/workstream-task.yml`
- `.github/ISSUE_TEMPLATE/device-evidence.yml`
- `.github/pull_request_template.md`
- `README.md`

What changed:
- Added GitHub issue and PR templates for workstream tasks, device evidence, clean-room checks, verification, and handoff notes.

Commands/tests:
- Pending in this session.

Evidence:
- GitHub repo target: public `Meapri/Plib`.

Blockers:
- None known.

Next recommended action:
- Create public GitHub repo, push `main`, configure labels, and open initial coordination issues.

## 2026-05-15 - Codex - Bundle C Start

Branch/worktree:
- `codex/alr-runtime-launcher` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- Pending

What changed:
- Starting GitHub Issue #2 / Bundle C: ALR runtime launcher skeleton.

Commands/tests:
- Pending

Evidence:
- Local test environment restored before start: `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 137 passed.

Blockers:
- None known.

Next recommended action:
- Add ALR runtime launcher/config/report skeleton without claiming guest execution.

## 2026-05-15 - Codex - Bundle C Implementation

Branch/worktree:
- `codex/alr-runtime-launcher` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime_launcher.cpp`
- `app/src/main/cpp/runtime_plan.hpp`
- `app/src/main/cpp/runtime_plan.cpp`
- `app/src/main/cpp/runtime_report.cpp`
- `tools/runtime_launch_plan.py`
- `tests/native_runtime_plan_test.cpp`
- `tests/native_backend_policy_test.cpp`
- `tests/test_alr_runtime_launcher_sources.py`
- `tests/test_alr_runtime_path_sources.py`
- `tests/test_android_loader_plan_report.py`
- `tests/test_runtime_launch_plan.py`

What changed:
- Added a packaged `libalr_runtime_launcher.so` skeleton and ALR runtime launch-plan/config report plumbing.
- Added absent-safe report lines for `ALR RUNTIME LAUNCHER AVAILABLE`, `ALR RUNTIME CONFIG BUILD`, and `ALR RUNTIME DIRECT APP-DATA EXEC POLICY`.
- Preserved `can_execute=false` for ALR runtime because guest execution is not implemented yet.
- Kept PRoot launch plan and fallback behavior intact.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 142 passed.
- `scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_launcher'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `40365e0c48e6a560c07d807bb776d960c7391dc0f7f060e1edfefd93c36e0597`
- APK contains `libalr_runtime_launcher.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Blockers:
- None for Bundle C source/build verification.

Next recommended action:
- Open a PR for Issue #2, then start Bundle D path hook MVP after review/merge.

## 2026-05-15 - Codex - Bundle D Start

Branch/worktree:
- `codex/alr-path-hook-mvp` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- Pending

What changed:
- Merged Bundle C PR #6 into `main`; Issue #2 closed.
- Started GitHub Issue #7 / Bundle D: ALR path hook MVP.

Commands/tests:
- Pending

Evidence:
- Bundle C merge commit on main: `e1fbf106401181bb30e756ddef857223f5798f10`

Blockers:
- None known.

Next recommended action:
- Add a packaged ALR hook library and host-testable open/stat smoke using clean path translation.

## 2026-05-15 - Codex - Bundle D Implementation

Branch/worktree:
- `codex/alr-path-hook-mvp` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime/alr_hook.hpp`
- `app/src/main/cpp/alr_runtime/alr_hook.cpp`
- `app/src/main/cpp/alr_runtime_hook.cpp`
- `app/src/main/cpp/runtime_plan.cpp`
- `app/src/main/cpp/runtime_report.cpp`
- `scripts/test-native-core.sh`
- `tests/native_alr_runtime_hook_test.cpp`
- `tests/native_runtime_plan_test.cpp`
- `tests/test_alr_runtime_hook_sources.py`
- `tests/test_alr_runtime_launcher_sources.py`

What changed:
- Added a packaged `libalr_runtime_hook.so` smoke hook library.
- Added clean-room host-path smoke plumbing that translates a guest path through the ALR rootfs path mapper and performs direct `stat`/`open` checks on the translated app-private file.
- Added report lines for `ALR HOOK LOAD`, `ALR HOOK CONFIG BUILD`, `ALR STAT ROOTFS FILE`, and `ALR OPEN ROOTFS FILE`.
- Added source, host-native, and Android APK packaging coverage.
- Still does not claim full guest execution or LD_PRELOAD syscall/path interposition.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 147 passed.
- `scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_hook|libalr_runtime_launcher'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `70b4c9b81dac2dad061ae0650e567596cb5810143295b503d5a373562a62a119`
- APK contains `libalr_runtime_hook.so` and `libalr_runtime_launcher.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Blockers:
- None for Bundle D source/build verification.
- Device-side hook smoke still needs Hermes/device evidence before claiming runtime behavior on Android hardware.

Next recommended action:
- Open a PR for Issue #7.
- Hermes can use the new hook report lines as APK/device smoke assertions while continuing Issue #1 and Issue #3 evidence work.

## 2026-05-15 - Codex - Bundle E Implementation

Branch/worktree:
- `codex/alr-interposer-scaffold` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `docs/agent-coordination.md`
- `docs/plans/parallel-workstreams.md`
- `docs/prompts/hermes-proroot-ab-and-device-evidence.md`
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime/alr_hook.hpp`
- `app/src/main/cpp/alr_runtime/alr_hook.cpp`
- `app/src/main/cpp/alr_runtime/alr_interposer.hpp`
- `app/src/main/cpp/alr_runtime/alr_interposer.cpp`
- `app/src/main/cpp/alr_runtime_interposer.cpp`
- `app/src/main/cpp/runtime_plan.cpp`
- `app/src/main/cpp/runtime_report.cpp`
- `scripts/test-native-core.sh`
- `tests/native_alr_runtime_interposer_test.cpp`
- `tests/native_runtime_plan_test.cpp`
- `tests/test_alr_runtime_interposer_sources.py`
- `tests/test_alr_runtime_launcher_sources.py`

What changed:
- Switched agent collaboration docs to large-batch handoffs instead of frequent sync pings.
- Added a packaged `libalr_runtime_interposer.so` scaffold.
- Added clean-room interposer smoke logic that resolves guest absolute and relative paths through ALR rootfs translation, then performs direct translated-path `stat`/`open` checks.
- Added report lines for `ALR INTERPOSER LOAD`, `ALR INTERPOSER CONFIG BUILD`, `ALR INTERPOSER STAT PATH`, and `ALR INTERPOSER OPEN PATH`.
- Initialized hook/interposer result structs defensively so absent/missing-file paths report deterministic FAIL/errno instead of reading uninitialized booleans.
- Still does not claim full guest execution, LD_PRELOAD coverage, syscall emulation, or performance wins.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 152 passed.
- `scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_(launcher|hook|interposer)'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `cf23a5c3c70f0b19dfdd068d1ae650a6626d6650a80fef5dd46f2d2862bb453a`
- APK contains `libalr_runtime_launcher.so`, `libalr_runtime_hook.so`, and `libalr_runtime_interposer.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Blockers:
- None for Bundle E source/build verification.
- Runtime preload behavior still needs a later bundle with a real guest process path and device evidence before any execution/performance claim.

Next recommended action:
- Open a PR for Issue #9.
- Next Codex implementation bundle should connect the interposer scaffold to a controlled guest process launch path or config serialization layer.
- Hermes should keep working in one large evidence bundle and rebase PR #5 before integration.

## 2026-05-15 - Codex - Bundle F Implementation

Branch/worktree:
- `codex/alr-config-serialization` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime/alr_config.hpp`
- `app/src/main/cpp/alr_runtime/alr_config.cpp`
- `app/src/main/cpp/runtime_plan.cpp`
- `app/src/main/cpp/runtime_report.cpp`
- `scripts/test-native-core.sh`
- `tests/native_alr_runtime_config_test.cpp`
- `tests/native_runtime_plan_test.cpp`
- `tests/test_alr_runtime_config_sources.py`
- `tests/test_android_loader_plan_report.py`
- `tests/test_runtime_launch_plan.py`
- `tools/runtime_launch_plan.py`

What changed:
- Added a deterministic `alr-config-v1` runtime config contract for rootfs, cwd, program, environment, binds, hook/interposer/bridge paths, fake-root, verbosity, and trace flags.
- Added C++ serialization, parsing, validation, and FNV-1a checksum helpers.
- Added matching Python-side launch-plan config serialization helper for tooling parity.
- Wired runtime reports to emit `ALR CONFIG SERIALIZE`, `ALR CONFIG PARSE`, config byte count, and config checksum.
- Added `ALR_CONFIG_FORMAT`, `ALR_INTERPOSER_PATH`, `ALR_TRACE_PATH`, and `ALR_TRACE_EXEC` to ALR runtime launch/env reporting.
- Still does not claim guest execution, child process continuity, or performance wins.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 156 passed.
- `scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_(launcher|hook|interposer)'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `eb91fb3bcd8f0837115550742b13c8849b62f83075473919ff24dd1b2fe88885`
- APK contains `libalr_runtime_launcher.so`, `libalr_runtime_hook.so`, and `libalr_runtime_interposer.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Blockers:
- None for Bundle F source/build verification.
- A future bundle still needs to move this config into a real fd/env handoff for an actual guest process launch path.

Next recommended action:
- Open a PR for Issue #11.
- Next Codex implementation bundle should use `alr-config-v1` to drive guest executable resolution and the first controlled ALR hello launch attempt.

## 2026-05-15 - Codex - Bundle G Implementation

Branch/worktree:
- `codex/alr-exec-resolution` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime/alr_exec.hpp`
- `app/src/main/cpp/alr_runtime/alr_exec.cpp`
- `app/src/main/cpp/alr_runtime_launcher.cpp`
- `app/src/main/cpp/runtime_plan.cpp`
- `scripts/test-native-core.sh`
- `tests/native_alr_runtime_exec_test.cpp`
- `tests/native_runtime_plan_test.cpp`
- `tests/test_alr_runtime_exec_sources.py`
- `tests/test_android_loader_plan_report.py`

What changed:
- Added clean ALR guest executable resolution before any real guest execution attempt.
- Resolves absolute guest paths and bare command names through guest `PATH`.
- Classifies resolved files as ELF, shebang script, missing, or unsupported.
- Parses simple shebang interpreter plus optional argument.
- Wires resolution/classification reports into runtime and launcher reports.
- Keeps `can_execute=false` and `alr runtime guest execution=not-claimed`.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 159 passed.
- `scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_(launcher|hook|interposer)'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `7c1659aca4291b5202f13663cd72798075858b9234027ea6b73eb3ed203361f5`
- APK contains `libalr_runtime_launcher.so`, `libalr_runtime_hook.so`, and `libalr_runtime_interposer.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Blockers:
- None for Bundle G source/build verification.
- A future bundle still needs a real low-overhead guest launch path after resolution.

Next recommended action:
- Open a PR for Issue #13.
- Next Codex implementation bundle should turn the resolved ELF/shebang plan into the first controlled ALR hello launch attempt.

## 2026-05-15 - Codex - Bundle H Implementation

Branch/worktree:
- `codex/alr-controlled-launch-attempt` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime/alr_launch.hpp`
- `app/src/main/cpp/alr_runtime/alr_launch.cpp`
- `app/src/main/cpp/alr_runtime_launcher.cpp`
- `app/src/main/cpp/runtime_plan.cpp`
- `scripts/test-native-core.sh`
- `tests/native_alr_runtime_launch_test.cpp`
- `tests/native_runtime_plan_test.cpp`
- `tests/test_alr_runtime_launch_sources.py`
- `tests/test_android_loader_plan_report.py`

What changed:
- Added the first controlled ALR launch-attempt engine after executable resolution.
- Default policy blocks direct rootfs host exec and reports `SKIP` honestly.
- Host-native tests can explicitly enable direct host exec for a tiny shebang fixture to prove fork/execve, stdout/stderr capture, and exit-code plumbing.
- Runtime and launcher reports now include `ALR LAUNCH ATTEMPT`, `ALR LAUNCH MODE`, and `ALR LOW-OVERHEAD RUNTIME HELLO EXECUTION`.
- Still does not claim Android device rootfs execution, glibc execution, shell `-c`, or performance wins.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 162 passed.
- `scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_(launcher|hook|interposer)'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `56a5ce839c886b12d33ac78b407a4d3d053695a7bfb664c39eaadba47fb407da`
- APK contains `libalr_runtime_launcher.so`, `libalr_runtime_hook.so`, and `libalr_runtime_interposer.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Blockers:
- None for Bundle H source/build verification.
- Real Android ALR hello PASS still needs a future execution strategy that does not rely on direct writable rootfs exec, plus device evidence.

Next recommended action:
- Open a PR for Issue #15.
- Next Codex implementation bundle should replace policy-blocked direct exec with a real low-overhead packaged loader/trampoline path for static hello.

## 2026-05-15 - Codex - Bundle I Implementation

Branch/worktree:
- `codex/alr-elf-load-plan` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime/alr_elf.hpp`
- `app/src/main/cpp/alr_runtime/alr_elf.cpp`
- `app/src/main/cpp/alr_runtime/alr_elf_format.hpp`
- `app/src/main/cpp/alr_runtime/alr_launch.cpp`
- `scripts/test-native-core.sh`
- `tests/native_alr_runtime_elf_test.cpp`
- `tests/native_alr_runtime_config_test.cpp`
- `tests/native_alr_runtime_exec_test.cpp`
- `tests/native_alr_runtime_hook_test.cpp`
- `tests/native_alr_runtime_interposer_test.cpp`
- `tests/native_alr_runtime_launch_test.cpp`
- `tests/native_runtime_plan_test.cpp`
- `tests/test_alr_runtime_elf_sources.py`
- `tests/test_android_loader_plan_report.py`

What changed:
- Added a clean ELF64 little-endian AArch64 load-plan parser for ALR.
- Reports ELF class, machine, type, status, entry point, virtual-address span, interpreter path, and PT_LOAD count.
- Classifies static AArch64 ET_EXEC without PT_INTERP as a static hello candidate.
- Classifies dynamic glibc-shaped ET_DYN/PT_INTERP as interpreter-needed, not executable yet.
- Wires the ELF report into the existing launch-attempt report while preserving honest SKIP lines for non-ELF paths.
- Added a local ELF format compatibility header so host-native tests can build on macOS and Linux without assuming system `<elf.h>` behavior.
- Fixed Hermes-observed Linux/aarch64 warning-as-error portability by fully initializing the `RuntimeConfig` fixture.
- Gave native temp directories PID suffixes to avoid parallel-test races.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 164 passed.
- `scripts/test-native-core.sh` -> PASS.
- `CXX=clang++ scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_(launcher|hook|interposer)|libalr_loader|libalr_test_command|libalr_proot_candidate'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `a597d5a0b819731ee325e990520b23db214f06bd87d6935cdda3780d7a6b9819`
- APK contains `libalr_loader.so`, `libalr_runtime_launcher.so`, `libalr_runtime_hook.so`, `libalr_runtime_interposer.so`, and `libalr_test_command.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Coordination:
- Codex continues Bundle I ELF load-plan work.
- Hermes is expected to continue its separate Bundle J large-batch evidence/implementation work without frequent sync pings.

Blockers:
- None for Bundle I source/build verification.
- Static hello still needs the next packaged trampoline/entry path before Android device PASS can be claimed.

## 2026-05-15 - Codex - Packaged Trampoline Implementation

Branch/worktree:
- `codex/alr-packaged-trampoline` at `/Users/naen/Documents/Plib/androlinux-runtime-lab`

Touched files:
- `app/build.gradle.kts`
- `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/cpp/alr_runtime/alr_launch.hpp`
- `app/src/main/cpp/alr_runtime/alr_launch.cpp`
- `app/src/main/cpp/alr_runtime/alr_trampoline.hpp`
- `app/src/main/cpp/alr_runtime/alr_trampoline.cpp`
- `app/src/main/cpp/alr_runtime_trampoline.cpp`
- `app/src/main/cpp/runtime_plan.cpp`
- `scripts/test-native-core.sh`
- `tests/native_alr_runtime_trampoline_test.cpp`
- `tests/native_runtime_plan_test.cpp`
- `tests/test_alr_runtime_trampoline_sources.py`
- `tests/test_android_loader_plan_report.py`

What changed:
- Added a packaged ALR trampoline executable that is built as `alr-runtime-trampoline` and copied into the APK as `libalr_runtime_trampoline.so`.
- Added clean trampoline report plumbing for availability, config handoff, policy preflight, and static hello attempt status.
- Added `ALR_TRAMPOLINE_PATH` to the ALR runtime launch plan and report.
- Kept direct writable rootfs exec blocked by default.
- Host-native tests prove trampoline config handoff and packaged-command preflight plumbing with a safe fixture.
- `ALR STATIC HELLO VIA TRAMPOLINE` still reports `SKIP`; this bundle does not claim guest entry execution or Android device PASS.

Commands/tests:
- `/Users/naen/.venvs/plib-py313/bin/python -m pytest tests -q` -> PASS, 168 passed.
- `scripts/test-native-core.sh` -> PASS.
- `CXX=clang++ scripts/test-native-core.sh` -> PASS.
- `JAVA_HOME=/Users/naen/.jdks/jdk-17.0.19+10/Contents/Home ./gradlew :app:assembleDebug --no-daemon` -> PASS.
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | rg 'libalr_runtime_trampoline|libalr_runtime_(launcher|hook|interposer)|libalr_loader|libalr_test_command'` -> PASS.

Evidence:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- APK sha256: `2bbd5ad8350d9cd0a91aecf4462724689e261a2386152986d4df73e3b92a22d0`
- APK contains `libalr_runtime_trampoline.so` for `arm64-v8a`, `armeabi-v7a`, `x86`, and `x86_64`.

Coordination:
- Hermes can treat the new trampoline report lines as device evidence targets after rebasing onto this bundle.
- Codex still owns the clean-room ALR implementation path; Hermes should keep optional backend/device findings black-box.

Blockers:
- No Android device PASS is claimed yet.
- The next Codex implementation step is a real static AArch64 entry trampoline/loader rather than report-only preflight.
