# ALR Loader Feature Gaps — what is blocked, and what it unlocks (WS-5 / L5 SSOT)

> **"지금 ALR 로더에서 막혀 있는 큰 기능이 무엇이고, 각각이 무엇을 잠금해제하는가"** 를 한 곳에
> 모은 SSOT. 자매 문서 `gui-universality-status.md`(어떤 GUI 가 device 에 뜨는가) 와
> `alr-compat-matrix.md`(앱×결과 표) 가 개별 셀에서 "막힘"을 표시하면, **왜 막혔는지 / 무엇을
> 풀어주는지 / 난이도·의존**은 여기로 모은다. 셀 여러 개가 같은 한 기능에 막혀 있을 때, 그 기능을
> 중복 서술하지 않고 여기 한 줄로 가리킨다.
>
> 소유: WS-5 (L5). HOST-ONLY — 새 device 측정 없음, 기존 `docs/evidence/` 인용만. 벤치 문서 아님
> (성능 숫자는 `docs/PERFORMANCE.md`/`cp2-gpu-ratio-glmark2.md`/`cp3-cpu-overhead-ratio.md`).
>
> baseline: 통합 트리 v143 (round-10 step2 device-proven `docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md`;
> round-10 step1 `docs/evidence/2026-06-02-round10-step1-inproc-reexec-mechanism-proven.md`;
> round-9 `docs/evidence/2026-06-02-round9-optionS-dead-wx-execve.md`;
> round-7 drain `docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`;
> round-6 `docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`;
> round-5 drain#14 `docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`
> + CP-6 M-R2 `docs/evidence/2026-06-02-cp6-mr2-chromium-syscall-mix.md`).
> 디바이스 `R5KL20B6S3X` (SM-X236N, mt6878, Mali-G615 MC2, Android 16, 1200×1920@90Hz, untrusted_app).
>
> **round-9→round-10 device 갱신(이 갱신, G1):** G1(최고 레버리지 exec-re-entry)의 **벽이 개념적으로
> 정복**됐다 — **in-process 재-맵(커널 execve 전무)이 map+jump 까지 device-proven.** round-9 v140 이
> ADR-003-v2 의 **Option S(커널이 적재 가능한 정적 re-entry stub 을 execve)를 DEAD** 로 입증했다:
> targetSdk 35 untrusted_app 의 W^X SELinux 정책(`neverallow untrusted_app … app_data_file:file
> execute`)이 app-storage 안의 *어떤* 파일도 `execve` 못 하게 막는다(stub 존재 확인+재푸시에도
> `exec_events=0`, stub 미실행). round-10 이 정답으로 피벗(ADR-003-v3): step1 v141 이 **메커니즘을
> device-proven** — execve seccomp-trap 에서 syscall 취소(`NT_ARM_SYSTEM_CALL=-1`) + tracee PC 를
> **resident(fork-공유 `.text`) 트램폴린**으로 redirect(`ALR-REEXEC: inproc trampoline reached`,
> child exit=123, 커널 execve 0). step2 v143 이 **진짜 map+jump 를 device-proven** — 트램폴린이 target
> ELF 를 `mmap(PROT_EXEC)`(W^X-허용) 로 in-process map 하고 entry 로 점프(`ALR-INPROC: mapped, jumping
> entry=0x400640`, static+dynamic 경로 wired, 커널 execve 0). 따라서 G1 은 "재-맵 벽(미구현)"에서
> **"재-맵 메커니즘 + map/jump device-proven; 남은 것 = 재-맵된 게스트 실행 정확성(static glibc startup
> SIGILL) + `/proc/self/exe` pass-through + 비-root `dpkg` superuser"**로 전진한다. 단 셀 승급은
> 아직: 재-맵된 게스트가 깨끗이 실행되기 전엔 apt/dpkg/GIMP-plugin 체인이 device-동작하지 않는다.
>
> **round-6 device-verified 요약(이 갱신):** G1(최고 레버리지 exec-re-entry)은 **ADR-003 B-1(execve
> x0 path-rewrite)이 device-fires**(`alr exec x0=/bin/sh reason=rewrite` traps=1 rewrites=1; no-exec
> 게스트 0 무회귀)로 전진 — 단 full dpkg chain 은 **B-3(child envp 재주입) in-flight(round-7)**,
> apt-install 은 여전히 `unpacked=false`(device-pending). G2(Qt6)는 **DONE** — round-6 v138 에서
> `EGL→wl_shm` 전환(EGL QPA/HwIntegration 플러그인 제외 + `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`)으로
> analogclock 이 **device-렌더(`qt6gui-result: rendered=true` frames 2215→2216)** → 7-toolkit 범용
> GUI 셋 완성. G3(Vulkan render)는 **round-7 v139 에서 device-검증✓** — R7-A 가 마샬 device 를
> AHB device-ext(`VK_ANDROID_external_memory_android_hardware_buffer`) 활성 + tiler readback barrier 로
> 고쳐 clear `vkQueueSubmit`=**VK_SUCCESS**(round-6 의 submit=FAIL 해결). CP-6 보류 박스에 M-R5
> svc-scan(read-only ROI 프로브) 명시.

---

## 0. 한눈에 — 레버리지 순

레버리지 = "이 하나를 풀면 동시에 몇 개의 막힌 셀이 풀리는가". exec-re-entry 가 압도적 1위다.

| # | 기능 갭 | 잠금해제하는 것 | 난이도 | 의존 | 상태 |
|---|---------|----------------|--------|------|------|
| **G1** | **exec-re-entry** (rootfs 바이너리 `execve` → ALR 로더가 새 ELF 를 **in-process 재-맵**) | `apt`/`dpkg` 실제 설치, GIMP plugin(fork+exec), 임의 멀티프로세스 Linux 앱, 셸 파이프라인 | **높음** | clone3/fork 시맨틱(메모리: device-evidence-mali-android16); 설계=ADR-003(v1) → ADR-003-v2(Option S, R9 가 DEAD 입증) → **ADR-003-v3 (in-process 재-맵, NO execve)** | **MECHANISM CONQUERED→실행 정확성 벽** (round-10 v141/v143: **in-process 재-맵 메커니즘 device-proven** — execve trap 에서 syscall 취소(`NT_ARM_SYSTEM_CALL=-1`) + PC-redirect → resident 트램폴린이 target ELF 를 `mmap(PROT_EXEC)` 로 map+jump(`entry=0x400640`), **커널 execve 0**. round-9 v140: Option S(커널-execve stub)는 W^X(targetSdk 35 untrusted_app `app_data_file:execute` neverallow)로 **DEAD**. 남은 것 = 재-맵된 static glibc 게스트가 자기 startup 중 **SIGILL** + `/proc/self/exe`(chromium zygote) pass-through + 비-root `dpkg` superuser) |
| **G2** | **Qt6 wl_shm 경로** (EGL hwintegration 회피 → 소프트웨어 client-buffer) | Qt6 GUI 앱 전반(analogclock→KDE/Qt 앱군) | 중간 | EGL 플러그인 비활성/wl_shm 강제(WS-4 env+overlay); G1 무관 | **DONE** (round-6 v138: `EGL→wl_shm` → analogclock **device-렌더** rendered=true frames 2215→2216) |
| **G3** | **Vulkan render pipeline** (ring 명령 body + ICD + AHB color-attach) | Vulkan-native 게임, Wine/DXVK/VKD3D, ANGLE-GLES | 높음 | enumerate/props backbone(**device-verified✓**) → VK-M2 명령 body | **PARTIAL→render device✓** (backbone device✓ round-5; **round-7 v139 VK-M2 render device-검증: device created + clear `vkQueueSubmit`=VK_SUCCESS**; 남은 것=ICD + textured/multi-draw 파이프라인) |
| **G4** | **netsurf 네트워크/입력 interaction** | 실 웹 페이지 로드(자산 fetch)·클릭/스크롤 입력 | 중간 | WS-3 입력 라우팅 + 게스트 네트워크 정책 | **PARTIAL** (정적 `about:welcome` RENDERS✓) |
| **G5** | **GLES3 전체 scene 커버리지** (전체 glmark2 14-scene + GLES3+) | 풀 GPU 벤치 매트릭스, GLES3 앱/에뮬레이터, GTK4 GL 렌더러 | 중간 | shim op 커버리지(WS-2); G3 와 일부 공유 | **PARTIAL** (build+texture Score~1000+✓, 2-scene) |

> **CP-6 / chromium 은 사용자(찬우) 보류.** chromium `--dump-dom`/`--headless` 의 raw-`svc`
> syscall-storm 벽(`alr-compat-matrix.md` §브라우저, `chromium-native-plan.md`)은 이 SSOT 의
> 레버리지 목록에 **올리지 않는다** — 명시적으로 보류 상태이며 round-6 범위 밖이다. 단 보류는
> *프로브/계측*까지 막지 않는다: round-6 에서 CP-6 M-R2 가 chromium `--version` 을 device-실행하고
> verdict=**mediation-negligible**(traps=0/emul=1)을 냈으며(`docs/evidence/2026-06-02-cp6-mr2-chromium-syscall-mix.md`),
> M-R5-svcscan(`svc` rewrite ROI **read-only** 프로브)·M-R1(USER_NOTIF 가용성 프로브)은 보류와 무관히
> *평가만* 진행 가능하다. CP-6 진행 SSOT 와 다음 분기는 **별도 문서 `docs/research/cp6-status.md`**
> (ADR-001/002/003 상호참조). 이 문서(loader-feature-gaps)는 storm 벽을 *갭으로 격상하지 않는다*.

---

## G1 — exec-re-entry (최고 레버리지)

**무엇이 막혔나.** rootfs 안의 바이너리가 `execve`(또는 `posix_spawn`/`fork`+`exec`)로 또 다른
rootfs 바이너리를 띄울 때, 그 자식이 다시 ALR 네이티브 로더를 통과해 in-process glibc 게스트로
실행되는 경로(=exec-re-entry)가 아직 없었다. 현재 로더는 **앱이 직접 launch 하는 단일 게스트**만
in-process 로 띄웠다. **round-10 에서 그 in-process 재-맵 메커니즘이 device-proven 됐다(아래
"round-9→round-10").**

**★ round-9→round-10 device 정복(메커니즘 + map/jump device-proven, 커널 execve 0).**
ADR-003 lineage 가 세 갈래로 진화·검증됐다 — v1(상속만으로 충분: round-7 가 `exec_events=0` 로
반증) → v2(Option S: 커널이 적재 가능한 정적 re-entry stub 을 execve: **round-9 가 W^X 로 DEAD
입증**) → **v3(in-process 재-맵, NO execve: round-10 이 map+jump 까지 device-proven)**.

**round-9 v140 — Option S 는 DEAD(W^X).** `docs/evidence/2026-06-02-round9-optionS-dead-wx-execve.md`:
supervisor splice 는 정상 발화(`alr exec reentry stub=… target=…` `spliced=1`)했으나 **모든 execve 에서
`exec_events=0`** 이고 stub 은 **단 한 줄도 출력하지 않았다**(stub 을 2초마다 재푸시해 splice 경로에
존재함을 확인한 뒤에도 동일). 근인 = **W^X SELinux 거부**: rootfs 파일은 `app_data_file` 로 라벨되고,
targetSdk ≥ 29 (이 앱 35) untrusted_app 정책의 `neverallow untrusted_app … app_data_file:file
execute` 가 app-storage 안의 *어떤* 파일도 `execve` 못 하게 막는다. 즉 **커널-execve 로 re-entry
stub 을 띄우는 길은 비-root untrusted_app 에서 구조적으로 죽었다**(`nativeLibraryDir` execve 해치도
`extractNativeLibs` unset 이라 부재). 이것이 정확히 ALR 이 in-process 매핑(file-backed `mmap(PROT_EXEC)`
는 허용)으로 존재하는 이유다.

**round-10 step1 v141 — 메커니즘 device-proven.**
`docs/evidence/2026-06-02-round10-step1-inproc-reexec-mechanism-proven.md`:
```
ALR-REEXEC: inproc trampoline reached (no execve)
alr exec reentry=off spliced=0 inproc=on inproc_redirected=1
alr native loader child exit=123 signal=0
```
execve seccomp-trap 에서 supervisor 가 (1) `NT_ARM_SYSTEM_CALL=-1` 로 **커널이 execve 를 skip**
하게 하고(W^X-금지 exec 미실행) (2) `regs[32]`(pc)를 fork 로 상속된(절대 unmap 안 되는) loader `.text`
안의 **resident raw-syscall 루틴**으로 redirect → tracee 가 **커널 execve 0** 으로 그 코드에 점프.
keystone: **비-root untrusted_app 에서 커널 execve 없는 exec-re-entry 가 viable** — R9 가 죽인
`app_data_file:execute` 벽을 우회한다(파일 exec 을 커널에 요청하지 않고, 이미 매핑된 코드로 PC 만 돌린다).

**round-10 step2 v143 — 진짜 map+jump device-proven.**
`docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md`:
```
ALR-INPROC: worker target=/data/.../rootfs/debian-arm64/bin/sh
ALR-INPROC: static target (no PT_INTERP) — direct map+jump
ALR-INPROC: mapped, jumping entry=0x400640
alr exec ... inproc=on inproc_redirected=2
```
트램폴린(`alr_inproc_reexec.c`, `alr_reentry.c` freestanding 매퍼 재사용)이 target ELF 를 열고
PT_LOAD 를 `mmap(PROT_EXEC)`(W^X-허용) 로 in-process map → 새 SysV stack(argv/envp/auxv) 구성 →
**entry 로 점프(커널 execve 0)**. static(no PT_INTERP → AT_BASE=0, program entry 직점프) + dynamic
경로 둘 다 wired. 즉 **cancel execve → PC-redirect → resident 트램폴린 → target in-process map → jump**
전 체인이 device 에서 end-to-end 동작. → 개념적·기계적 벽은 **정복**(ADR-003-v3).

**남은 것(정복된 토대 위 focused iteration — 개념적 미지가 아님).**
- 재-맵된 static `/bin/sh` 가 jump 후 **자기 startup 중 SIGILL**(foot 인터랙티브 셸:
  `terminal.c:1770: slave exited with signal 4 (Illegal instruction)`) — map+jump 가 entry 에 도달하나
  static glibc 가 자기 부트스트랩에서 fault(후보: static IRELATIVE/IFUNC, BSS-tail 0-fill, TLS/TPIDR,
  auxv 필드). **이 실행-정확성 버그가 셀 승급을 막는 잔여 게이트.**
- 게스트가 `**/proc/self/exe**`(interp `/system/bin/linker64`)를 exec — chromium zygote 가 *Android*
  앱 바이너리를 re-exec → Debian rootfs 로 매개 불가(`interp open/read fail`). **별도 pass-through 경로**
  필요(rootfs-바이너리 재-맵과 구분).
- `apt install` 은 여전히 `unpacked=false` 이나 핵심 driver 가 **exec-re-entry 와 독립**으로 드러남:
  `dpkg: error: requires superuser privilege`(비-root dpkg 가 unpack 거부) → **fakeroot/root-emulation**
  경로 별도 필요.

**device 증거(B-1+B-3 결정은 device-fires 하나 `exec_events=0` ⇒ execve 자체가 미완 — THE WALL).**
round-7 v139(`docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`):
```
alr exec x0=/bin/dash reason=rewrite envp_reason=already
alr exec envp_injected=0 ld_preload_set=0      (모든 exec)
alr exec traps=1 rewrites=1 exec_events=0 clone_events=7   (/bin/sh, round-6 repro)
apt-install: unpacked=false configured=false exec=[ALR NATIVE LOADER GUEST EXEC: FAIL]
```
ADR-003 **B-1**(execve x0 path-rewrite)은 계속 device-fires(`x0=/bin/dash reason=rewrite`,
argv/envp 불변). **B-3**(child envp 재주입) 결정함수도 모든 execve/execveat trap 에서 평가되며 —
`/bin/dash` 에 대해 `envp_reason=already`(게스트가 exec 한 child 가 로더-설정 `LD_PRELOAD`/
`ALR_ROOTFS` 를 *상속*하므로 재주입 불필요 → `envp_injected=0` 이 **올바른 no-op**, 실패가 아님).
**그러나 결정적 관측: 관측된 모든 exec 에서 `exec_events=0`** — `PTRACE_EVENT_EXEC`(새 program
image 가 실제로 로더 아래에서 실행에 진입)가 **단 한 번도 발화하지 않는다**, 심지어 pre-exec
seccomp trap 이 경로를 재작성한 뒤에도. 근인: 커널이 glibc-aarch64 ELF 를 execve 할 수 없다 — 그
`PT_INTERP` = 게스트 ld.so `/lib/ld-linux-aarch64.so.1` 를 Android 커널이 resolve 못 함 → execve
실패, 새 이미지 없음. 즉 **B-1 + B-3 는 necessary 이나 NOT sufficient** — 진짜 벽은
**loader 의 on-exec in-process 새-ELF 재-맵(re-entry stub / loader-as-bootstrap)**이다.
`apt` top-level 은 더 앞에서 실패(`GUEST EXEC FAIL`, `traps=0`) — apt 가 minimally-staged 라
(`apt-config-stage.tar` 10KiB; 전체 apt+dpkg+solver closure 부재) execve trap 까지 도달조차 못 한다.
(round-6 첫 관측 `docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`; round-4
`docs/evidence/2026-06-02-round4-milestones-drain.md`.) clone3 계열 PRoot 한계는 별도 메모리
(device-evidence-mali-android16)에 기록.

**무엇을 잠금해제하나(레버리지).**
- **`apt install` / `dpkg -i` 실제 설치** — 패키지 매니저가 maintainer-script 를 실행할 수 있게 됨
  (현재 `apt-get --version`/`dpkg-query` *실행*만 device-PROVEN, drain#9).
- **GIMP plugin** — GIMP 가 babl/gegl 필터·script-fu·python-fu plugin 을 별도 프로세스로 fork+exec.
  babl/gegl 모듈 *로드*는 확인됐으나(drain#13 `gimp-filter: ok=true`), 풀 필터 exercise 는 이 경로 의존.
- **임의 멀티프로세스 Linux 앱 / 셸 파이프라인** — `sh -c 'a | b'`, 빌드 스크립트, 데몬 더블-fork 등
  "프로세스를 띄우는 프로세스"가 전부 여기 걸린다. 단일-게스트 가정을 넘어 **범용성의 다음 큰 도약**.

**난이도: 높음.** fork/clone 후 자식 주소공간에서 로더를 재초기화(seccomp/PCGATE 재설치, ring fd
상속, ld.so 재진입)해야 하고, `execve` 가로채기가 in-process map 교체로 동작해야 한다. 단순 추가가
아니라 별개의 큰 로더 마일스톤.

**round-6→round-7 재구도(PARTIAL → 진짜 벽 = loader 재-맵).** 설계 **ADR-003**
(`docs/design/adr-003-multiprocess-exec-reentry.md`)는 자식을 두 클래스로 나눈다: **(A) zygote-fork
자식(renderer 다수)은 execve 를 안 거치므로 이미 매개된 주소공간 + 상속 seccomp + SEIZE-trace 로
_자동_ 매개**, **(B) fresh-execve 자식(zygote/gpu)만 진짜 벽**. round-6 v138 에서 ADR-003 의 전제
("(B) 는 'loader 재진입'이 아니라 seccomp-across-execve + `PTRACE_O_TRACEEXEC` 자동 재포착으로 풀린다")
하에 **(B-1) execve x0 path-rewrite 가 device-fires**(`alr exec x0=/bin/sh reason=rewrite` traps=1
rewrites=1)했고, round-7 v139 에서 **(B-3) child envp 재주입 결정함수도 device 에서 평가**됨
(`envp_reason=already` — 상속-envp execs 에선 재주입 불필요라 `envp_injected=0` 이 올바른 no-op).
**그러나 round-7 의 결정적 device 관측이 ADR-003 의 전제를 _반증_했다: 모든 execve 에서
`exec_events=0`** — `PTRACE_EVENT_EXEC` 가 끝내 발화하지 않는다. 커널이 glibc-aarch64 ELF 의 execve 를
완료하지 못하기 때문(그 `PT_INTERP`=게스트 ld.so `/lib/ld-linux-aarch64.so.1` 를 Android 커널이
resolve 불가). 따라서 **B-1 + B-3 는 necessary-but-NOT-sufficient** 이고, G1 은 "path-rewrite +
envp 주입"에서 **"on-exec 새 ELF 의 in-process 재-맵(re-entry stub / loader-as-bootstrap)"으로
재구도**된다 — 즉 ADR-003 의 "loader 재진입 기각, 상속 채택" 전제가 하드웨어로 disproven 이며, 실제
exec-re-entry 는 로더가 exec 시 새 ELF 를 _직접_ 재-맵해야 한다. 이 재-맵 설계는 **ADR-003-v2**
(docs/design/, 병행 세션 소유 — 이 SSOT 는 경로만 참조, 편집하지 않음)가 담당하며 full apt 스테이징
오버레이도 같은 라운드에서 진행된다(`apt-install: unpacked=false` 의 두 번째 원인 = apt minimally-staged).
host 프로토타입(WS-5): `tests/exec_map_model.py`(clone:exec 분류) + `tests/test_execve_pathrw.py`
(x0 vs x1 분기 결정모델). device 프로브 게이트 = M-R4-fork/execmap/envprop(ADR-003 §5, read-only).

**의존.** fork/clone3 시맨틱 안정화(메모리: device-evidence-mali-android16); 설계 lineage =
ADR-003(v1, 상속-가설 round-7 반증) → ADR-003-v2(Option S 커널-execve stub, **round-9 W^X DEAD**)
→ **ADR-003-v3(in-process 재-맵, NO execve; round-10 map+jump device-proven)**. G2/G3/G4/G5 와
독립(이들은 G1 없이도 부분 진행 가능). 단 GIMP 풀 필터(babl/gegl) 와 apt 설치는 **G1(재-맵)에 강하게
의존**. apt 설치는 추가로 (a) 재-맵된 게스트 실행 정확성(SIGILL), (b) 비-root dpkg 의
fakeroot/root-emulation(`dpkg: requires superuser`), (c) full apt+dpkg+solver 스테이징 오버레이(현
`apt-config-stage.tar` 10KiB minimally-staged)에 의존 — (b)/(c)는 exec-re-entry 와 **독립**.

---

## G2 — Qt6 wl_shm 경로 (EGL hwintegration 회피) — **DONE (round-6 v138)**

**무엇이 막혔었나(해소됨).** Qt6 overlay 는 stage + 클로저 완전(round-5 에서 35 reachable libs,
`QT_QPA_PLATFORM=wayland`)이었으나 fork 된 게스트 child 가 **Qt platform init 도중 SIGSEGV** 했다.
round-5 가 원인을 device 로 좁혔다: **overlay 미완이 아니라 EGL client-buffer hwintegration**(Qt 가
wayland **EGL** client-buffer integration 을 골라 ICD 없는 `eglGetDisplay` 호출). round-6 v138 이 이를
**`EGL→wl_shm` 백엔드 강제로 device-해소**했다.

**device 증거(해소).** round-6 v138(`docs/evidence/2026-06-02-round6-qt6-execreentry-vkrender.md`):
```
qt6gui-result: rendered=true frames=2215->2216  bin=.../qt6/examples/widgets/widgets/analogclock/analogclock
```
fix = qt6 overlay 에서 **EGL QPA platform 플러그인 + compositor HwIntegration 플러그인 제외** +
`QT_WAYLAND_DISABLE_HW_INTEGRATION=1`. Qt 가 **wl_shm backing store** 를 사용해 analogclock 창이
SurfaceView 에 합성(frame counter 전진, crash 없음). round-4
(`docs/evidence/2026-06-02-round4-milestones-drain.md`)가 SIGSEGV 를 처음 관측, round-5 가 "overlay
부족" 가설을 기각하고 EGL 으로 재분류, round-6 이 wl_shm 강제로 닫았다.

**무엇을 잠금해제했나.** Qt6 GUI 앱 전반 — analogclock(데모)에서 시작해 Qt/KDE 위젯 앱군.
GTK3/native-Wayland/SDL2 에 이어 **다섯 번째 독립 toolkit** 으로 범용 GUI 셋을 **7 toolkit** 으로 확장
(`gui-universality-status.md` §1). 잔여(증분, 회귀 아님): 더 많은 Qt/KDE 앱 + Qt 입력 interaction.

**난이도: 중간(해소됨).** overlay 가 아니라 **client-buffer 백엔드 선택** 문제였다 — Qt 의 EGL
hwintegration 플러그인을 overlay 에서 제외하고 **wl_shm 소프트웨어 client-buffer** 로 강제해 ICD 없는
`eglGetDisplay` 경로를 피한 뒤, netsurf/foot/SDL2 처럼 컴포지터에 display-backed launch.

**의존.** WS-4 overlay/env(EGL 플러그인 제외 + `QT_WAYLAND_DISABLE_HW_INTEGRATION=1`). G1 무관
(단일 게스트로 device-렌더). **round-6 DONE.**

---

## G3 — Vulkan render pipeline

**무엇이 막혔나.** 게스트 Vulkan 호출을 실 Mali Vulkan 드라이버까지 끌고 가는 풀 파이프라인.
**enumerate/props backbone(round-5 VK 1.3) + VK-M2 render(round-7 v139) 둘 다 실 Mali 에
device-verified.** 남은 막힘은 **ICD(VK-M3) + textured/multi-draw 파이프라인 + 컴포지터 sample**.

**증거(현 상태).** round-5 drain#14(`docs/evidence/2026-06-02-round5-vulkan-device-marshal.md`):
`ALR VK ENUM MARSHAL: PASS`, `mode=mali-libvulkan`, `VK_SUCCESS`/`Mali-G615 MC2`/**api=1.3**/
`vendorID=0x13b5(ARM)`. **round-7 v139(`docs/evidence/2026-06-02-round7-vkrender-pass-drain.md`):**
```
ALR VK RENDER MARSHAL: PASS
ops decoded=11  device created=yes  gfx queue family=0  submit result=VK_SUCCESS
```
즉 VK-M2 body 가 게스트 `vkCreateDevice` + `vkGetDeviceQueue` + command-pool/buffer + clear 를 **실 Mali
libvulkan** 으로 마샬하고 clear `vkQueueSubmit` 이 **VK_SUCCESS** 로 완료된다. round-6 의 submit=FAIL 은
**근인이 마샬 device 를 확장 없이 생성**한 것이었다 — clear 경로가 AHB color target 을
`VK_ANDROID_external_memory_android_hardware_buffer` 로 import 하는데 그 entry point 는 device-ext 가
`vkCreateDevice` 에서 활성일 때만 합법. R7-A 가 AHB device-ext(+ `VK_EXT_queue_family_foreign` 가능 시)
활성 + tiler readback barrier(render-pass `finalLayout`→`GENERAL` + `VK_QUEUE_FAMILY_EXTERNAL` release)
를 추가해 해결. enumerate + render 모두 device-PASS. 전략 전체는
`docs/research/gpu-guest-accel-strategy.md`(VK-M1/M2/M3 트랙).

**무엇을 잠금해제하나.** Vulkan-native 게임, **모든 Wine/DXVK/VKD3D**(전략 문서 옵션 A),
그리고 ANGLE→Vulkan 으로 robust GLES 경로(옵션 B). GUI 의 cairo-SW 가 아니라 게스트 *자체* 3D.

**난이도: 높음.** device/queue 마샬(device-created✓) + clear `vkQueueSubmit`(VK_SUCCESS, round-7✓)
다음은 (a) 게스트 `libvulkan_alr.so` ICD + manifest(VK-M3), (b) textured/multi-draw 파이프라인,
(c) `VK_ANDROID_external_memory_AHB` color-attach 렌더타깃 → 컴포지터 sample.
Mali proprietary Vulkan 한계(no transform_feedback/geometry/tess)는 전략 문서에 기록.

**의존.** GPU ring/AHB present 인프라(CP-2/CP-4, device-verified✓) 재사용. G5(GLES3) 와 일부 공유
(ANGLE 경로). G1 무관.

---

## G4 — netsurf 네트워크/입력 interaction

**무엇이 막혔나.** netsurf-gtk 가 정적 `about:welcome` 을 **RENDERS**(drain#12, rendered=true,
5 threads)하지만, 실제 웹 페이지 로드(네트워크 자산 fetch)와 클릭/스크롤 입력 interaction 은 미검증.

**증거(현 상태).** `docs/evidence/2026-06-02-netsurf-browser-renders.md` — `rendered=true`
frames 2214→2217, 그러나 잔여(증분, 회귀 아님)로 "전체 페이지 자산/네트워크/입력 interaction" 명시.

**무엇을 잠금해제하나.** 실 브라우징(원격 페이지 로드) + 사용자 입력으로 페이지 조작 = netsurf 가
데모를 넘어 **실사용 브라우저**가 됨.

**난이도: 중간.** (a) 게스트 네트워크 정책(소켓 syscall → Android 네트워크, untrusted_app 권한 내),
(b) WS-3 입력 라우팅을 GTK 위젯 hit-test 까지 전달(GIMP 에서 입력 주입 USABLE✓ 이므로 토대는 있음).

**의존.** WS-3 입력(M4). G1 무관(단일 프로세스 브라우저). round-4/5 증분.

---

## G5 — GLES3 전체 scene 커버리지

**무엇이 막혔나.** glmark2 가 **build+texture 2-scene** 으로 Mali 에 렌더(Score~1000+,
software=false, device-verified)하지만, **전체 14-scene** 과 GLES3+ 기능(현재 GLES2 19-op
state-setter 커버리지)은 미완.

**증거(현 상태).** `docs/evidence/2026-06-02-cp2-FINAL-glmark2-score-1074.md`,
`docs/evidence/2026-06-02-cp5-batch-8mibring-texture-ws4-overlays.md`(Score 1163),
`docs/evidence/2026-06-02-breadth-fanout-drain.md`(19-op GLES2 커버리지). host Mali context 는
이미 GLES **3.2** 로 확인됨(GL_RENDERER passthrough, drain#9).

**무엇을 잠금해제하나.** 풀 GPU 벤치 매트릭스(ALR vs Mali-직접 ratio 의 분자 완성,
`cp2-gpu-ratio-glmark2.md`), GLES3 앱/에뮬레이터, **GTK4 GL 렌더러**(실앱 GL → Mali, WS-2 M4).

**난이도: 중간.** shim op 커버리지를 GLES3 entry point 까지 확장(현 GLES2 19-op → GLES3),
decoder replay + wire-check 케이스 추가(host 게이트, off-device round-trip). 전체 14-scene 은
duration↑ 또는 분할 launch.

**의존.** WS-2 shim/decoder. G3(Vulkan/ANGLE) 와 ANGLE 경로 공유 가능. G1 무관.

---

## 갱신 규칙

- 갭이 **풀리면**(device evidence): 해당 G# 를 BLOCKED/PARTIAL → DONE 으로 내리고, 가리키던
  `gui-universality-status.md`/`alr-compat-matrix.md` 셀을 같은 evidence 로 승급. 이 문서가
  "막힘"의 SSOT 이므로 다른 문서는 여기를 가리키기만 한다(중복 서술 금지).
- **상태 어휘**: `BLOCKED`(device 벽 확인, 진전 없음) → `IN-PROGRESS`(설계 채택/host 프로토타입/
  원인 device-규명은 됐으나 device 해소 evidence 없음) → `PARTIAL`(일부 device-verified) → `DONE`
  (목표 device-verified). **round-6 v138 전이**: **G2 는 EGL→wl_shm device-렌더로 IN-PROGRESS→DONE**
  (analogclock rendered=true), G1 은 B-1 execve x0 path-rewrite device-fires 로 IN-PROGRESS→PARTIAL
  (B-3 child envp 재주입은 round-7 in-flight, apt-install 아직 `unpacked=false`), G3 는 VK-M2 device
  created=yes 이나 clear-submit FAIL 로 PARTIAL 유지(round-7 clear-submit fix). **IN-PROGRESS/PARTIAL
  은 RENDERS/USABLE/DONE 이 아니다** — device 해소 전엔 셀 승급 금지(G1 apt-install·G3 clear-submit
  은 아직 device-실패라 셀 승급 불가).
- **round-7 v139 전이**: **G3 는 clear `vkQueueSubmit`=VK_SUCCESS 로 render device-검증✓**(R7-A
  가 AHB device-ext `VK_ANDROID_external_memory_android_hardware_buffer` 활성 + tiler readback
  barrier 로 round-6 의 submit=FAIL 해결; backbone 과 합쳐 enumerate+render 둘 다 device-PASS — 단
  ICD/textured/multi-draw 가 남아 PARTIAL 유지). **G1 은 진짜 벽이 재구도**됨 — B-1 path-rewrite +
  B-3 envp 결정 모두 device-fires 하나 **모든 execve 에서 `exec_events=0`** 가 커널이 glibc-aarch64
  ELF(`PT_INTERP`=게스트 ld.so)를 execve 완료 못 함을 입증 ⇒ G1 의 벽은 "path-rewrite + envp"가
  아니라 **loader 의 on-exec in-process 재-맵(re-entry stub)**; 설계=**ADR-003-v2**(병행 세션 소유).
  G1 셀은 여전히 미승급(apt-install `unpacked=false` device-실패 + 재-맵 미구현).
- **round-9→round-10 전이(G1 벽 정복)**: round-7 이 지목한 "loader 재-맵" 벽이 **device-정복**됐다.
  round-9 v140 이 ADR-003-v2 **Option S(커널-execve stub)를 W^X 로 DEAD** 입증(`exec_events=0`, stub
  미실행; `app_data_file:execute` neverallow). round-10 이 ADR-003-v3 로 피벗해 step1 v141(메커니즘:
  execve 취소 + PC-redirect → resident 트램폴린, child exit=123) → step2 v143(진짜 map+jump:
  `mmap(PROT_EXEC)` + entry 점프 `entry=0x400640`, 커널 execve 0)을 device-proven. G1 은 "재-맵 벽
  (미구현)"→"**재-맵 메커니즘 + map/jump device-proven; 남은 것 = 재-맵 게스트 실행 정확성(SIGILL) +
  `/proc/self/exe` pass-through + 비-root dpkg superuser**"로 전진. **그러나 셀 미승급**: 재-맵된
  static glibc 게스트가 자기 startup 에서 SIGILL 하므로(map+jump 는 entry 도달, 실행은 미완) apt/dpkg/
  GIMP-plugin 체인은 아직 device-동작하지 않는다 — 메커니즘 device-proof ≠ 셀(RUNS) 승급.
- **새 측정 금지**(WS-5 HOST-ONLY) — 기존 `docs/evidence/` 인용만. device evidence 없이
  BLOCKED→DONE 승급 금지.
- **레버리지 순서 유지** — exec-re-entry(G1)가 최고 레버리지라는 판단은 "한 기능이 푸는 막힌 셀
  수"에 근거. 새 갭은 같은 기준으로 #0 표에 끼워 넣는다.
- **CP-6/chromium 은 보류** — 사용자 보류 상태이므로 이 SSOT 의 레버리지 목록에 올리지 않는다.
  CP-6 진행/분기는 `docs/research/cp6-status.md`(별도 SSOT, ADR-001/002/003 상호참조)가 소유.
