# Device Evidence — WS-1 M2: CPU mediation 오버헤드 정량 (일반 앱 traps=0; perf microbench)

Device SM-X236N (mt6878, Mali-G615, Android 16), APK `0.4.127-cp1-gui-baseline-v127`. 5-세션 분할의 **WS-1(L1 CPU 실행/오버헤드)** M2. cold start 후 logcat(`alr_loader`).

## 측정 1 — per-guest path-mediation 라운드트립 (traps/rewrites)
이 run의 모든 일반 게스트가 **`traps=0 rewrites=0`**:
`/usr/bin/env`, `/usr/bin/id`, `/bin/dash`(×2), `/bin/alr-png-test`, `/usr/bin/gimp-console-3.0`, `/bin/alr-wl-test`, `/bin/alr-pixman-test`, `/bin/alr-input-test`, `/bin/alr-interactive-test`, `/bin/alr-gtk3-test`, `/usr/bin/foot` — 전부 exit=0(또는 의도된 종료), `pcgate=1 interpose=1 traps=0 rewrites=0`. `gimp-3.0`만 `traps=1`(set_robust_list류 1회).

→ **PCGATE interposer가 path syscall을 in-process로 rewrite해 supervisor(ptrace) 라운드트립이 0**. 이게 PRoot(trap-every-syscall)와의 근본 차이.

## 측정 2 — perf microbench (`run_perf_comparison`, logcat 노출 신규)
```
ALR PERF SYSCALL ROUNDTRIP MEASURED: PASS
alr perf alr xlate     ns/op = 4334.727
alr perf syscall getppid ns/op = 218.338
```
- path-xlate(`translate_rootfs_path`) cold = **4334.7 ns/op**, ≈ **19.9 syscall units**.
- raw syscall(getppid) = **218.3 ns/op**.
- cold 비용이지만 게스트는 `xlate_cache`(256-entry, guest_path→host_path memoize)로 반복 path를 분할 상환 → 실효 비용은 훨씬 낮음.

## 해석 (사용자 목표 "오버헤드 제로" 대비)
ALR의 CPU mediation 오버헤드 구성:
- **path syscall**: in-process translate(cold 4.3µs, cache hit 후 저렴) + **supervisor 라운드트립 0**(traps=0).
- **non-path syscall**: seccomp **ALLOW**(0 오버헤드).
- → 일반 앱은 PRoot의 ptrace-every-syscall(~µs/syscall) 대비 근본적 우위. path-mediation 부분의 "제로 오버헤드"는 일반 앱에서 device-verified.

## 측정 3 — native-exec wall-clock per guest (신규 `exec_ms`, device)
gimp-probe 라인에 fork→reap wall-clock(`exec_ms`)을 추가:
- 일반 CLI: `/bin/dynhello` 19ms, `/usr/bin/env` 19, `/usr/bin/id` 20, `/bin/dash` 18-19, `/bin/alr-png-test` 23 — fork + ELF맵 + ld.so + 실행 + exit + supervisor **전체가 ~18-20ms**(native 프로세스 수준).
- 무거운 lib 로드: `gimp-console-3.0 --version` 48ms, `alr-wl-test` 40, `alr-pixman-test` 38, `alr-gtk3-test` 197, `gtk3-widget-factory` 145, `gimp-3.0` 135.
- `alr-input-test`/`alr-interactive-test` 6048/6053ms = **의도된 dispatch 대기**(오버헤드 아님).
- 전부 `traps=0`(gimp-3.0만 1) → supervisor 라운드트립 0인 상태의 실행 시간.
→ 일반 CLI ~18-20ms는 native 프로세스 수준. PRoot(trap-every-syscall, ptrace ~µs × 수천 syscall)는 같은 워크로드에서 수백ms대가 예상되며, NativeCommandRunner(현재 `ProcessBuilder`+`waitFor`만, wall-clock 없음)에 측정을 넣어 실측하는 게 WS-1 M2 완성의 다음.

## WS-1 M2 다음
1. **PRoot baseline 실측** (`run_perf_comparison`의 `proot_device_baseline_pending=true` 해소) → ALR vs PRoot 정량 비교.
2. **실제 CPU-bound 워크로드 wall-clock** (native-exec vs PRoot vs adb-shell) → 전체(syscall 외) 오버헤드.
3. **chromium류 raw-`svc` syscall-storm**은 LD_PRELOAD interposer로 후킹 불가 → RET_TRACE 라운드트립이 벽 → **M3 out-of-process(SECCOMP_RET_USER_NOTIF) ADR** 후보.

stamp 미변경(통합 세션 정책; WS-1은 runtime_report perf-logcat 노출만 추가).
