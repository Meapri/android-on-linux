# CP-3 CPU-overhead Ratio — native vs ALR (WS-5)

> CP-3 = "CPU 오버헤드 정량" 게이트. **같은 바이너리**(static musl microbench)를
> native(`adb shell` 직접) 와 ALR loader 양쪽에서 돌려, native 대비 ALR 의
> wall-clock/ns-per-op 비율을 잰다. 이건 **CLOSED ✅** — device-증명, 새 측정 불필요.
> 이 문서는 그 결과를 §0 "CPU 오버헤드 < 5% (syscall-light)" 기준과 대조해 최종 해석을
> 박는다(CP-2 ratio 문서 `cp2-gpu-ratio-glmark2.md` 의 자매 문서).
>
> 소유: WS-5 (L5). host-only — 숫자는 기존 device evidence 인용, 새 device 측정 없음.
> 비율 모델은 `bench/cpu_overhead.py` (`python -m bench overhead`) 에 구현돼 있다.

작성 baseline: 통합 트리 main `310c759`. 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878,
Mali-G615, Android 16). evidence: `docs/evidence/2026-06-01-cp3-apples-to-apples-gtk3-svg-perm.md`,
`docs/evidence/2026-06-01-ws5-cpu-overhead-quantified.md`.

---

## 1. §0 목표 연결

`docs/research/orchestration-5session-plan.md` §0 (CPU 축):

| 축 | 측정 | 목표 |
|----|------|------|
| CPU 오버헤드 | syscall-light 앱: native(adb shell) 대비 wall-clock/syscall 지연 | **< 5% (syscall-light)**, syscall-storm 앱(chromium)도 usable |

§0 (a): "오버헤드 제로의 본질적 적 = CPU 측 ptrace/seccomp 중재 라운드트립". CP-3 가
이걸 정면으로 잰다. 게이트 = **syscall-light 워크로드에서 ALR/native < 1.05 (오버헤드 < 5%)**.

---

## 2. 결과 — apples-to-apples, 같은 바이너리 (device-증명) ✅

WS-1 이 동일 microbench(static musl, native baseline 과 **바이트-동일** 바이너리)를
native(`adb shell` 직접) + ALR loader(MainActivity guest-probe → `alr-microbench` logcat)
양쪽에서 실행:

| mode | 무엇 | native ns/op | ALR ns/op | overhead | §0 게이트 (<5%) |
|------|------|-------------|-----------|----------|----------------|
| **compute** | CPU-bound, syscall 0 | 4.06 | 4.06 | **+0.00%** | **PASS** ✅ |
| **syscall** | `getpid` 루프 (양쪽 동일 call) | 200.36 | 224.36 | **+11.98%** | reported (syscall-storm, gated 아님) |

- **compute = +0.00%**: syscall 없는 순수 연산은 ALR 가 native 와 **정확히 동일** 속도
  (4.06 = 4.06). PCGATE 인터포저는 in-process, 연산엔 seccomp/ptrace 가 안 걸린다.
- **syscall = +11.98%**: `getpid` 한 call 당 ~24 ns 추가(224.36 vs 200.36). 이건
  **seccomp 디스패치 고정비용** — 커널이 모든 syscall 을 BPF 필터에 통과시키는 비용.
  BPF 슬림화로도 ~1 ns 만 줄었다(223.37 vs 224.36) → seccomp 를 켜는 한 syscall 0% 는
  원천 불가. per-app `ALR_PCGATE=0` 옵션만 가능(raw-svc 백스톱이라 전역 off 는 위험).

host 재현(파싱/게이트만, 새 측정 아님):
```sh
# compute: native 4.06 ns/op, ALR 4.06 ns/op → 0% (gated PASS)
python -m bench overhead --native-ns 406 --native-samples 100 --alr-ns 406 --alr-samples 100
# syscall: native 200.36, ALR 224.36 ns/op → ~12% (storm, not gated)
# (양쪽 동일하게 100 samples 로 스케일 — 안 그러면 per-sample 분모가 어긋나 % 왜곡)
python -m bench overhead --native-ns 20036 --native-samples 100 --alr-ns 22436 --alr-samples 100 --storm
```

---

## 3. §0 기준 대조 — 해석

§0 의 CPU 게이트는 **"syscall-light 앱 < 5%"** 다. 이걸 두 regime 로 분해해 읽는다:

### 3.1 syscall-light (= §0 게이트 대상) → PASS ✅
- 일반 연산/CLI 워크로드의 **지배적 비용은 compute** 이고, 그건 **+0.00%** = native 속도.
- 일반 CLI native-exec wall-clock(`exec_ms`)도 ~18–20 ms = native-process 수준
  (`dynhello`/`env`/`id`/`dash`/`alr-png-test`, traps=0).
- path-mediation 도 **supervisor 라운드트립 ZERO**(traps=0, in-process translate;
  cold path-xlate 4334.7 ns/op 이나 256-entry cache 로 분할 상환, ptrace trap 없음).
- → **§0 "CPU 오버헤드 < 5% (syscall-light)" = device-증명 PASS.** 0% < 5%.

### 3.2 syscall-storm (= §0 게이트 아님, "usable" 만 요구) → ~12%, 본질적
- `getpid`-storm(순수 syscall 루프)은 ~12%. 이건 **seccomp 디스패치 고정비용**이지
  ALR 의 비효율이 아니다 — seccomp 를 켜는 한 (W^X/raw-svc 백스톱에 필수) 모든 syscall
  은 BPF 를 통과해야 한다. §0 도 syscall-storm 은 "<5%" 가 아니라 **"usable"** 만 요구.
- 단, 이 ~12% 는 **light getpid 루프** 다. chromium-class **raw `svc` syscall-storm** 은
  LD_PRELOAD 인터포저로 후킹 불가 → seccomp RET_TRACE supervisor 라운드트립(ptrace 벽)
  으로 떨어진다 = 별개의 더 큰 벽(CP-6 / L1.M3 USER_NOTIF ADR 후보). CP-3 의 12% 와
  혼동 금지: CP-3 는 "seccomp 디스패치"(고정 ~24 ns), CP-6 는 "ptrace 라운드트립"(훨씬 큼).

### 3.3 한 줄 결론
**일반 연산/CLI(=§0 게이트 대상 syscall-light) = native급 0% 오버헤드, device-증명.**
비-제로 regime 은 raw syscall 뿐이고, light syscall-storm ~12% 는 seccomp 디스패치 본질,
heavy raw-svc storm(chromium)은 CP-6 의 ptrace 벽으로 별도 추적.

---

## 4. baseline / 잔여 (정직한 scope)

| 항목 | 상태 |
|------|------|
| same-binary native vs ALR (compute/syscall) | **CLOSED ✅** (device-증명, 위 §2) |
| PRoot A/B baseline (ALR 우위 정량) | **보류** — app-private rootfs exec 이 SELinux 로 막힘. native-vs-ALR 결과에는 불필요(부수). 통합 세션이 SELinux 우회 가능해지면 채움 |
| chromium-class raw-svc storm 정량 | CP-6 (L1.M3) — 별개 벽, ptrace 라운드트립 |

PRoot A/B 는 §0 의 native-vs-ALR 게이트(이미 PASS)에 **필수 아님** — ALR 가 PRoot 대비
빠르다는 건 아키텍처적으로 자명(PRoot=trap-every-syscall ptrace, ALR=traps=0 in-process).
정량 baseline 만 SELinux 로 보류 상태(부수적, 게이트 비차단).

---

## 5. 요약
CP-3 **CLOSED ✅**: 같은 바이너리 native-vs-ALR — **compute +0.00%**(4.06=4.06, §0 <5%
게이트 PASS), **syscall +11.98%**(seccomp 디스패치 고정비용, §0 게이트 아님). §0 "CPU
오버헤드 < 5% (syscall-light)" 는 device-증명으로 충족. 잔여 = PRoot A/B(SELinux 보류,
부수) + chromium raw-svc storm(CP-6 별개 벽).
