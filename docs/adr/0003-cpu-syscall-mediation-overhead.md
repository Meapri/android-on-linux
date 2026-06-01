# ADR 0003 — CPU syscall mediation 오버헤드 모델 (in-process ptrace vs out-of-process USER_NOTIF)

## Status
Accepted (WS-1, 2026-06-01). 일반 앱 결정 확정; chromium류 syscall-storm은 open frontier로 명문화.

## Context
ALR은 glibc arm64 앱을 **in-process native-exec**(map ELF + guest ld.so + fork + ptrace supervisor)로 실행하고, **PCGATE**(PC-gated seccomp) + `libalr_interpose`(LD_PRELOAD)로 path syscall을 중재한다(rootfs path rewrite).

device 측정 (2026-06-01, SM-X236N; evidence `docs/evidence/2026-06-01-ws1-m2-cpu-mediation-overhead.md`):
- **일반 앱**(env/id/dash/gimp-console/gtk3-widget-factory/foot/alr-*-test 등): `traps=0 rewrites=0` — interposer가 path syscall을 in-process로 rewrite, **supervisor ptrace 라운드트립 0**. 실행 wall-clock 일반 CLI **~18-20ms**(native 프로세스 수준). path-xlate microbench 4334 ns/op(cold, `xlate_cache`로 분할 상환), getppid 218 ns/op.
- **syscall-storm**(chromium-class): 앱이 raw `svc`(libc 우회)로 syscall을 직접 발행 → LD_PRELOAD interposer가 후킹 불가 → 모든 path syscall이 RET_TRACE → **ptrace 라운드트립 × 수천 = 벽**(메모리 chromium-native-goal).

## Decision
1. **일반 앱: in-process ptrace + PCGATE interposer 유지.** device-verified로 path-mediation 라운드트립 0, native CPU 속도. 사용자 목표 "오버헤드 제로"를 일반 앱(GUI 포함)에서 달성한다. 이것이 ALR의 주 타깃.
2. **chromium류 syscall-storm: out-of-process USER_NOTIF를 일반 경로로는 채택하지 않고 open frontier로 명문화.**
   - `SECCOMP_RET_USER_NOTIF`(~10× 싼 per-event)는 (a) in-process 모델(게스트가 앱 프로세스 내 fork-child)과 비호환 — 별도 supervisor 프로세스를 요구; (b) **arg mutation(path rewrite) 불가** — USER_NOTIF는 syscall 인자를 바꿀 수 없다(ptrace는 가능). ALR path-mediation의 핵심 기능을 잃는다.
   - 따라서 일반 앱엔 부적합. chromium류 전용 별 트랙(out-of-process supervisor + path-mediation 대체 설계)으로 분리하며, 모델 변경급 R&D로 **보류**.
3. **대안(미채택, 기록): 부분 정적 패치** — chromium의 raw `svc` 사이트를 trampoline으로 rewrite. 바이너리별 fragile, 보류.

## Consequences
- 일반/GUI Linux 앱은 제로 오버헤드로 동작(CP-1 + WS-1 M2 device-verified). ALR의 주 사용 사례가 성립.
- chromium-class 멀티프로세스 + raw-svc 앱은 in-process ptrace 오버헤드 벽이 남으며, 해결은 모델 변경(out-of-process)을 요하는 미해결 프론티어다.
- WS-5 bench(`bench/cpu_overhead.py`)는 이 경계를 코드로 반영: syscall-light는 `<5%` wall-clock gate, syscall-storm은 report-only(게이트 제외, "ptrace round-trip wall").
- 관련: ADR 0001(runtime-model), `docs/research/chromium-native-plan.md`, `docs/research/orchestration-5session-plan.md`(WS-1 M3), 메모리 chromium-native-goal / alr-pcgate-status.
