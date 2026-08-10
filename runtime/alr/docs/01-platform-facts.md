# 01 — 검증된 플랫폼 사실

> **이 문서의 규칙: 여기 없는 플랫폼 사실은 가정하지 말 것.** 새 사실이 필요하면 1차 소스(커널 소스, glibc 소스, bionic googlesource, man page)로 검증하고 이 문서에 등급과 함께 추가한다.

**증거 등급**
- `SOURCE` — 커널/glibc/bionic/AOSP 소스 코드 또는 man page로 확인. 가장 강함.
- `FIELD` — 신뢰할 만한 3자 보고(이슈 트래커, 프로젝트 문서)로 확인.
- `PENDING_DEVICE` — 실제 디바이스에서 확인해야 함. `alr doctor`가 측정한다.

---

## A. Android seccomp — 설계의 지배 제약

### A1. zygote 필터의 기본 액션은 `SECCOMP_RET_TRAP`이다 — `SOURCE`

bionic `libc/seccomp/seccomp_policy.cpp`:
```cpp
inline void Disallow(filter& f) { f.push_back(BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP)); }
```
`_set_seccomp_filter()`는 allow-tree를 붙인 뒤 `Disallow(f)`로 끝난다. 즉 **allowlist에 없는 syscall = SIGSYS**. errno가 아니다.

설치 지점: `frameworks/base/core/jni/com_android_internal_os_Zygote.cpp` `SetUpSeccompFilter()` — `if (uid >= AID_APP_START) set_app_seccomp_filter();`. Termux의 uid는 ≥ 10000이므로 **APP 필터를 받는다.**

> **탈출구 1개**: `if (!gIsSecurityEnforced) return;` — SELinux permissive(루팅/userdebug) 디바이스에는 필터가 **아예 없다**. 그런 디바이스의 측정은 무효다.

### A1a. 측정 가능한 컨텍스트는 하나뿐이다 — `SOURCE` + 실측 확인

필터는 zygote가 **uid ≥ 10000인 앱을 fork할 때만** 설치한다. 따라서 앱 프로세스의 자손이 아닌 곳에서 잰 결과는 **전부 거짓 ALLOWED**다.

| 컨텍스트 | uid | SELinux | Seccomp | 측정 가능? |
|---|---|---|---|---|
| `adb shell` | 2000 | `u:r:shell:s0` | **0** | ❌ |
| `run-as <pkg>` (debug 빌드) | 앱 uid | `u:r:runas_app:s0` | **0** | ❌ |
| Termux 세션에서 fork된 프로세스 | 앱 uid | `u:r:untrusted_app_27:s0` | **2** | ✅ |

실측 (참조 기기 #1, 2026-08-02):
```
model      SM-X236N (Galaxy Tab A9+)
android    16 (SDK 36)          selinux  Enforcing
soc        MediaTek MT8775 / mt6878        ⚠️ Snapdragon 아님 — 성능 수치는 대표성 없음
kernel     6.1.145-android14-11 aarch64
page size  4096                            (16KB 페이지 기기 아님)
termux     v0.118.3 github-debug, minSdk=24 targetSdk=28  ✅ 설계 요구 충족

adb shell → uid=2000(shell) context=u:r:shell:s0  Seccomp: 0  Seccomp_filters: 0
```

> **성능 측정 주의**: 이 기기는 MediaTek이다. seccomp/SELinux/exec 정책은 전부 AOSP 레벨이라 SoC와 무관하게 이 기기의 **호환성 결과는 그대로 유효**하지만, 벤치마크 배수는 이 기기 것이다.
> 실제로 M8은 참조 기기 #2 없이 이 기기에서 돌았다: 10k 파일 `git status` native 42 ms / alr 49 ms / proot-distro 1,704 ms → **34.8배**, 프로세스 기동 24 / 28 / 304 ms → **10.9배** ([증거](evidence/2026-08-02-m7-m8-workloads-perf.md)), `npm ci` proot 6.25 s vs alr 2.00 s → **3.12배** ([증거](evidence/2026-08-03-m12-spawn-resolver.md)). **그러므로 배수를 인용할 때는 "MediaTek MT8775 / Android 16 기준"을 함께 쓴다.** **두 이유 모두 해소됐다** — 참조 기기 #2(Snapdragon 8 Elite)에서 재측정했고([M19 §6](evidence/2026-08-03-m19-snapdragon.md)), §A6의 차단 집합도 동일함을 확인했다. 다만 배수는 기기마다 다르므로(`node` 콜드 6.60× vs 5.49×) **인용할 때 기기를 함께 쓴다.**

`run-as`가 debug 서명 빌드에서 동작한다는 이유로 쓰면 안 된다 — zygote를 거치지 않아 필터가 없고, SELinux 도메인도 다르다. 진입 절차는 [scripts/dev-bootstrap.md](../scripts/dev-bootstrap.md).

**`alr doctor` P1이 이 조건을 검사하고, 만족하지 않으면 나머지 프로브를 아예 거부한다.** 거짓 PASS를 내는 것보다 아무것도 안 내는 게 낫다.

### A2. 필터는 fork로 상속되고 execve를 넘어 유지된다 — `SOURCE`

`seccomp(2)`: *"If execve(2) is allowed, the existing filters will be preserved across a call to execve(2)."* 커널에서 필터 체인은 `current->seccomp.filter`에 걸려 있고 execve가 지우지 않는다. **제거·완화 인터페이스는 존재하지 않는다.**

→ **exec로 도망갈 깨끗한 방은 없다.** glibc 게스트는 bionic을 링크하지 않아도 bionic 모양의 allowlist를 영구히 상속한다.

### A3. 스택 필터는 가장 제한적인 액션이 이긴다 — `SOURCE`

`kernel/seccomp.c` `seccomp_run_filters()`:
```c
/* All filters in the list are evaluated and the lowest BPF return value always takes priority */
if (ACTION_ONLY(cur_ret) < ACTION_ONLY(ret)) { ret = cur_ret; *match = f; }
```
`SECCOMP_RET_TRAP == 0x00030000` < `SECCOMP_RET_ERRNO == 0x00050000`.

→ **자체 `RET_ERRNO` 필터로 Android의 TRAP을 구제하는 것은 구조적으로 불가능하다.** 유저스페이스에서 완화는 불가능하고, 추가는 언제나 더 제한적인 방향으로만 간다.

### A4. `set_robust_list`(99)와 `rseq`(293)는 차단되어 있고, **ld.so가 생성자보다 먼저 호출한다** — `SOURCE` ⚠️ 최중요

glibc 2.39 `sysdeps/nptl/dl-tls_init_tp.c` `__tls_init_tp()` 호출 순서:
1. `set_tid_address` (96) — **허용됨**
2. `set_robust_list` (99) — **차단됨 → SIGSYS**
3. `rseq` (293) — **차단됨 → SIGSYS** (`glibc.pthread.rseq=0` 튜너블로 회피 가능)

bionic `SYSCALLS.TXT` + `SECCOMP_ALLOWLIST_{COMMON,APP}.TXT`를 `android12-release` ~ `android16-release` + `main` 전 브랜치에서 grep한 결과 `rseq`, `robust_list` 매치 **0건**. 번호상으로도 깨끗한 구멍이다: 96(허용) 97(허용) 98 futex(허용) **99 차단** **100 차단** 101(허용).

`__tls_init_tp()`는 `_dl_init` **이전**에 실행된다. `DT_INIT_ARRAY`도 `DT_PREINIT_ARRAY`도 이보다 먼저 실행되지 않는다.

> **결론: 순수 `LD_PRELOAD` 설계로는 스톡 glibc가 부팅조차 못 한다.** 생성자가 SIGSYS 핸들러를 설치할 기회 자체가 없다. 이 사실 하나가 [ADR 0001](adr/0001-signal-only-ptrace-supervisor.md)의 근거다.
>
> 현장 확증: dev.to "Running Native glibc (Debian) Binaries on Android 15 Without PRoot" — `Bad system call (SIGSYS) set_robust_list: Function not implemented` (`FIELD`)

### A5. aarch64 SIGSYS 핸들러로 syscall을 에뮬레이션할 수 있다 — `SOURCE` (단, 3개 함정)

- syscall은 **실행되지 않는다**: `kernel/seccomp.c`가 `syscall_rollback()` 후 `force_sig_seccomp()` → `goto skip`.
- `uc_mcontext.pc`는 **이미 svc 다음을 가리킨다** (arm64는 syscall 진입 시 ELR_EL1이 이미 진행됨). **PC 보정 불필요.**
- `si_code == SYS_SECCOMP`, `si_syscall == nr`, `si_arch == AUDIT_ARCH_AARCH64`.

**함정 3개 (반드시 반영):**

1. **`regs[0]` 쓰기는 필수다.** `syscall_rollback()`이 arm64에서 `regs->regs[0] = regs->orig_x0`을 수행하므로, 핸들러 진입 시 `regs[0]`에는 **첫 번째 인자**가 들어 있다. 쓰지 않고 리턴하면 인자값이 반환값이 된다.
2. **`si_call_addr`은 svc 주소가 아니다.** 커널은 `KSTK_EIP`를 넣으므로 aarch64에서는 post-svc 주소 = `uc_mcontext.pc`와 동일. svc 주소를 원하면 `-4`.
3. **그냥 리턴하면 무한루프에 빠질 수 있다.** `arch/arm64/kernel/signal.c`가 시그널 전달 **전에** `regs->regs[0]`으로 syscall-restart 판정을 하는데, rollback 후 그 값은 arg0이다. arg0이 `-512`/`-513`/`-514`/`-516`이면 커널이 `regs->pc = restart_addr`(svc 자체)로 되돌린다. `-ERESTARTNOINTR(-513)`은 revert 분기가 없어 **영원히 재트랩**한다.
   → **매 호출마다 `uc_mcontext.pc = (uintptr_t)info->si_call_addr`를 방어적으로 대입한다.** 저장 1회 비용으로 이 케이스가 무해해진다.

검증된 선례: Chromium `sandbox/linux/bpf_dsl/seccomp_macros.h` aarch64 분기 — `SECCOMP_REG(ctx,r) ((ctx)->uc_mcontext.regs[r])`, `SECCOMP_SYSCALL(ctx) SECCOMP_REG(ctx,8)`, `SECCOMP_IP(ctx) (ctx)->uc_mcontext.pc`.

### A6. 차단 syscall 목록 — `SOURCE` + `MEASURED` (기기 2대 스윕 완료, 집합 동일)

`SECCOMP_BLOCKLIST_APP.TXT`에 명시된 것: `setuid`/`setgid`/`setreuid`/`setregid`/`setresuid`/`setresgid`/`setfsuid`/`setfsgid`/`setgroups` 전체, `adjtimex`, `clock_adjtime`, `clock_settime`, `settimeofday`, `acct`, `syslog`, **`chroot`**, `init_module`, `delete_module`, **`mount`**, **`umount2`**, `swapon`, `swapoff`, `setdomainname`, `sethostname`, `reboot`.

allowlist 부재로 차단되는 것(확인됨): `set_robust_list`(99), `get_robust_list`(100), `rseq`(293).

> ⚠️ **`openat2`(437)와 `faccessat2`(439)는 추정이 틀렸다** — `MEASURED` 2026-08-02.
> bionic allowlist 부재를 근거로 차단으로 적었으나, SM-X236N(Android 16)에서 **둘 다 허용**된다. Android 16에서 allowlist가 넓어진 것으로 보인다.
> **결과**: [ADR 0003](adr/0003-ld-preload-path-virtualization.md) 이 `openat2(RESOLVE_IN_ROOT)` 를 기각한 사유(SIGSYS)는 **지원 대상 기기에서 성립하지 않으므로 폐기됐다.** 다만 **결론은 바뀌지 않았다** — 기각은 유지되고 사유가 교체됐다(아래, [ADR 0007 §3](adr/0007-android-16-only.md)).
> **채택하지 않기로 최종 결정했다** — [ADR 0007 §3](adr/0007-android-16-only.md). 구버전 재확인 조건은 [ADR 0007](adr/0007-android-16-only.md) 로 사라졌지만, 결론은 그대로 기각이고 근거가 바뀌었다: `openat2` 는 **open 계열만** 덮는데 경로 가상화는 심볼 163개에 걸쳐 있어 재작성기를 대체할 수 없고(경로가 둘로 늘 뿐이다), 재작성 총비용은 이미 예산의 1/20 이라 성능 동기가 없으며, `RESOLVE_IN_ROOT` 의 이점인 심링크 탈출 차단은 [§5 비목표](00-product.md)다.

> ⚠️ **cred-drop 계열 중 `setresuid`(147), `getresuid`(148), `getresgid`(150)는 차단되지 않는다** — `MEASURED`.
> 차단되는 것은 143/144/145/146/149/151/152/159뿐이다.

전체 실측 결과(468개 중 239개 차단)와 생성된 에뮬레이션 테이블: [evidence/2026-08-02-device-bringup.md](evidence/2026-08-02-device-bringup.md), [`src/supervisor/alr_sigsys_table.h`](../src/supervisor/alr_sigsys_table.h). 그 헤더 끝의 `#if 0` 블록이 기기 ground truth(차단 번호 전체)이며, 회귀 diff의 기준선이다.

허용되는 것(확인됨): `prctl`, `seccomp`, `ptrace`, `execve`, `execveat`, `clone`, **`clone3`** (`SECCOMP_ALLOWLIST_COMMON.TXT`에 `clone3(clone_args*, size_t) all` — Docker 시대의 clone3 참사는 여기서 반복되지 않는다).

> ⚠️ **`accept`(202)는 차단, `accept4`(242)는 허용** — `MEASURED` 2026-08-03.
> 소켓 계열 중 `socket`(198)·`bind`(200)·`listen`(201)·`connect`(203)·`accept4`(242)는 전부 허용인데 **`accept` 하나만 막힌다.** 표 기본값이 `-ENOSYS` 라 슈퍼바이저가 그렇게 에뮬레이션했고, 결과적으로 **게스트에서 연결을 받는 모든 프로그램이 깨져 있었다.** tmux 서버가 소켓을 만들고 나서 클라이언트가 붙는 순간 `server exited unexpectedly` 로 죽는 형태로 드러났다.
> **해결**: preload 가 `accept(f,a,l)` 을 `accept4(f,a,l,0)` 으로 구현한다 — 둘은 flags 인자 하나만 다르다. 슈퍼바이저에서 syscall 을 갈아 끼우는 것보다 싸고 단순하다(레지스터 재작성이 필요 없다).
> **왜 이제야 나왔나**: 서버를 띄우는 수용 시험이 없었다. `PRELOAD UNIX SOCKET PATH` 프로브의 첫 판본조차 `connect` 까지만 해서 **깨진 상태로 통과했다** — `connect` 는 listen 백로그에 붙는 것만으로 성공하기 때문이다. 검사는 실패하는 지점까지 가야 한다.

> ✅ **SysV IPC는 게스트에서 쓸 수 없다** — `MEASURED` 2026-08-03. ([RISKS R9](RISKS.md) 종결)
> 스윕의 차단 집합에 **180–197이 통째로** 들어 있다: POSIX mqueue(180–185)와 SysV IPC 전체 — `msgget`(186) `msgctl`(187) `msgrcv`(188) `msgsnd`(189) `semget`(190) `semctl`(191) `semtimedop`(192) `semop`(193) `shmget`(194) `shmctl`(195) `shmat`(196) `shmdt`(197). 즉 zygote 블록리스트에 있고 "혹시 안 막혔을 수도"가 아니다.
> 게스트에서 직접 불러 확증했다: `shmget`/`semget`/`msgget` 전부 `ENOSYS`(테이블 기본값)를 받고, 같은 실행의 `ALR_LOG`가 SIGSYS 트랩 **3건**을 에뮬레이션했다고 보고한다 — 즉 커널이 실제로 TRAP을 냈고 슈퍼바이저가 받아냈다. 근거: [M16 §1](evidence/2026-08-03-m16-ipc-audit.md) (프로브 소스 `tests/device/probe_ipc.c`).
> **결과**: R9이 걸어 둔 조건("막지 않는다면 업스트림 `fakeroot`를 그대로 쓴다")이 성립하지 않는다. SysV IPC 백엔드를 쓰는 fakeroot는 게스트에서 동작할 수 없으므로 자체 shim(또는 IPC를 안 쓰는 변종)만 남는다.

> ✅ **SoC·커널이 갈려도 차단 집합은 같다 — 기기 2대 diff 완료** `MEASURED` 2026-08-03. ([M19](evidence/2026-08-03-m19-snapdragon.md))
>
> | | 참조 #1 | 참조 #2 |
> |---|---|---|
> | 기기 | SM-X236N | SM-S937N (Galaxy S25 Edge) |
> | SoC | MediaTek MT8775 | **Qualcomm** Snapdragon 8 Elite SM8750 |
> | 커널 | 6.1.145-**android14** | **6.6.98-android15** |
> | 컨텍스트 | uid=10297 Seccomp=2 `untrusted_app_27` | uid=10447 Seccomp=2 `untrusted_app_27` |
> | 차단 | 468개 중 **239** | 468개 중 **239** |
>
> **239개 집합이 완전히 동일하다.** 개수만 같은 게 아니라 전체 diff에서 양쪽 모두 0이다:
> ```
> $ scripts/diff-sweep.sh docs/evidence/sweeps/mediatek-mt8775-android16-k6.1.txt \
>                         docs/evidence/sweeps/snapdragon-8elite-android16-k6.6.txt
> ALR SWEEP DIFF: IDENTICAL (239 syscalls, both devices)
> ```
> 위 문단의 음성 주장(147/148/150 비차단)도 양쪽에서 성립한다. **이게 결정적인 부분이다** — 범위 필터였다면 이웃한 143–152와 함께 쓸려 갔을 번호들이라, 개수 우연이 아니라 지문 일치다.
>
> **결과**: `alr_sigsys_table.h`를 릴리스에 동봉하는 현재 방식이 옳다. 표는 기기별 생성물이 아니라 **기본값**으로 취급할 수 있고, `alr doctor`는 여전히 재생성 수단으로 남는다.
>
> 🚫 **남은 변수였던 Android 릴리스는 재지 않기로 했다** ([ADR 0007](adr/0007-android-16-only.md)). 아래 문단의 실측·논증은 그대로 유효하며, 그것이 바로 범위를 16으로 좁힌 근거다.
>
> **근거**: 두 기기 모두 Android 16이다. 갈린 것(SoC 벤더, 커널 6.1→6.6, android14→android15 공통 브랜치)은 전부 무관했고, **정작 allowlist가 실제로 따라가는 축**(android12 365줄 → android16 392줄)은 고정된 채로 남았다. 즉 이 결과는 **"SoC 벤더·커널은 상관없다"** 는 강한 증거이고 "Android 버전도 상관없다"는 증거는 **아니다**. 그 증거를 만들지 않기로 했으므로 지원 범위에서 뺀다.
>
> ⚠️ **고정된 축이 하나 더 있다 — OEM.** 두 기기 다 **Samsung** 이다(`docs/evidence/sweeps/*.txt` 의 `# device` 줄). allowlist 는 SoC 벤더가 아니라 **OEM 이 빌드한 플랫폼 이미지 안의 bionic** 에서 온다. 그러므로 이 실측이 닫은 것은 **SoC·커널 축**이다.
>
> **이 축은 재지 못했고 잴 수단도 없다.** 릴리스 축과 달리 **변한다는 증거는 없다** — `SECCOMP_ALLOWLIST_*.TXT` 는 AOSP bionic 이 배포하고 OEM 이 고친 사례를 알지 못한다. 그러나 모르는 것은 잰 것이 아니므로, 다른 OEM 을 **된다고도 안 된다고도 적지 않는다.** 그 기기에서는 `alr doctor` 스윕 + `scripts/diff-sweep.sh` 가 사용자 자신의 계측기다.
> **무엇이 이것을 끝내는가**: 아무것도 필요 없다 — **결정으로 닫혔다.** 이 항목이 걸고 있던 두 질문(차단 집합 일반화, `openat2`/`faccessat2` 채택)은 각각 [ADR 0007](adr/0007-android-16-only.md) 의 Decision 과 §3 에서 답이 났다.
> **막고 있던 것**: 구버전 Android 기기였다. 기술적 장애물은 없었고 지금도 없다 — 그래서 이것은 blocker 가 아니라 **선택**이었다. 새 기기(같은 Android 16, 다른 벤더)를 얻으면 여전히 스윕을 돌려 `scripts/diff-sweep.sh` 로 비교한다. `PTRACE_SECCOMP_GET_FILTER` 지름길은 `CAP_SYS_ADMIN` 이 필요해 여전히 불가이므로 스윕이 유일한 길이다.
>
> 스윕 원본은 이제 [`docs/evidence/sweeps/`](evidence/sweeps/)에 그대로 들어간다. 이전에는 `alr_sigsys_table.h` 끝의 `#if 0` 블록에만 있었고, 이번 diff는 표 행만 grep한 탓에 **"원본이 없어서 비교 불가"라고 결론 내릴 뻔했다.**

---

## B. Termux 실행 능력

### B1. `$PREFIX`/`$HOME`에서 execve가 된다 (F-Droid 빌드) — `SOURCE`

Termux F-Droid/GitHub 빌드는 `targetSdkVersion=28`, `minSdkVersion=21`. targetSdk ≤ 28은 SELinux `untrusted_app_27` 도메인을 받고, 이 도메인은 `app_data_file`에 대한 `execute_no_trans`를 **유지**한다. targetSdk ≥ 29 도메인은 잃었다.

- Android 10 ~ 16 및 AOSP main까지 이 grant는 **깨지지 않는다.**
- 위협 모델은 "Android가 규칙을 지운다"가 아니라 **"Android가 `MIN_INSTALLABLE_TARGET_SDK`를 28 위로 올린다"**이다. 릴리스당 +1씩 올라왔으므로(23→24) 29까지는 수년 남았다.
- **대응**: 마이그레이션 계획을 특정 Android 버전에 걸지 말 것. 대신 첫 실행 때 `$PREFIX`의 스크래치 ELF를 execve해 보고 `EACCES`면 **크게 실패**한다.

> ⚠️ `auditallow`가 `execute`와 `execute_no_trans` **양쪽에** 걸려 있다. 게스트의 모든 execve와 모든 `.so` 매핑이 logd에 감사 레코드를 남긴다. Node 프로세스 하나가 시작 시 `.so` ~40개를 매핑하면 레코드 ~40개다. **이것이 이 설계의 숨은 비용이며 오버헤드 주장을 발표하기 전에 측정해야 한다** (`PENDING_DEVICE`, [RISKS R6](RISKS.md)).
>
> **왜 아직 못 쟀는지는 2026-08-03에 확정됐다: Termux 앱 프로세스 안에서는 잴 수 없다.** 같은 프로세스에서 `logcat -b events -d`는 **에러 없이 0줄**을 내고 `logcat -b main -d`는 내용을 낸다. events 버퍼가 비권한 앱에게 읽히지 않는 것이지, 감사 레코드가 없다는 뜻이 아니다.
> **그러므로 "0줄"을 "오버헤드 없음"으로 기록하면 안 된다** — 권한 실패를 측정값으로 읽는 것이고, [00-product.md §6](00-product.md)이 금지하는 바로 그 종류의 거짓 PASS다.
> **무엇이 이것을 끝내는가**: 외부 관찰자. adb로 붙은 호스트에서 `logcat -b events`를 흘려보내는 동안 워크로드는 **Termux 안에서** 돌리고(§A1a — adb shell에는 필터가 없어 거기서 돌린 워크로드는 무효다), exec 집약 구간(`git rebase`, npm postinstall)의 레코드 수와 벽시계 시간을 짝지어 잰다. 관찰만 밖에서, 실행은 안에서.
> **무엇이 막고 있는가**: 에이전트 세션에서 기기에 붙는 것이 금지되어 있다. 사람 세션 + adb 호스트가 필요하다.

### B2. Play Store Termux는 v1 미지원 — `SOURCE`

Play 빌드(`termux-apps`)는 targetSdk 37이라 `execute_no_trans`가 없다. termux-exec의 `system_linker_exec` 모드는 `/system/bin/linker64`로 우회하지만 그것은 **bionic 링커라 glibc 프로그램을 로드할 수 없다.**

이론적으로는 `execve("/system/bin/linker64", ["linker64", "<app-data의 bionic 트램폴린>", ...])` 후 그 트램폴린이 유저스페이스 ELF 로더로 glibc ld.so를 매핑하는 경로가 가능하다(§B3에 따라 file-backed `PROT_EXEC` mmap은 허용됨). 하지만 bionic과 glibc가 한 주소공간에 공존하며 `TPIDR_EL0`을 다투게 되고, 구현 선례가 없다.

→ **[ADR 0005](adr/0005-play-store-unsupported.md): v1 미지원. 시작 시 감지하고 명확한 메시지로 거부한다.**

### B3. 실행 메모리 — `SOURCE`

| 동작 | 결과 | 비고 |
|---|---|---|
| 익명 `mmap(RW)` → `mprotect(RX)` | ✅ 허용 | V8/Node JIT에 필요한 전부 |
| rootfs 파일의 file-backed `PROT_EXEC` mmap | ✅ 허용 | **ld.so가 게스트 `.so`를 매핑하는 데 필수. 설계의 기반이 안전하다.** |
| brk 영역에서 `mprotect(PROT_EXEC)` | ❌ `EACCES` | `PROCESS__EXECHEAP` 미허용. JIT 아레나는 반드시 mmap에서 나와야 함 |
| 스택에 `PROT_EXEC` | ❌ `EACCES` | `PROCESS__EXECSTACK` 미허용. **RWX `PT_GNU_STACK`을 가진 게스트 `.so`는 로드 실패** → rootfs 빌드타임 린트 필요 |
| `execmod` (텍스트 재배치) | ✅ targetSdk 28에서만 | 의존하지 말 것. targetSdk ≥ 29에는 없는 유일한 exec 권한 |

### B4. 사용자 네임스페이스는 존재하지 않는다 — `SOURCE` ⚠️

- `unshare(CLONE_NEWUSER)` → **`EINVAL`** (커널이 기능 없이 빌드됨). `EPERM`이 아니다.
- `mount(2)`, `chroot(2)` → **SIGSYS로 프로세스 사망** (A6).
- `unshare(CLONE_NEWNS)` → `CAP_SYS_ADMIN` 필요.

→ **비루팅 Android에 제로 오버헤드 네임스페이스 rootfs는 없다.** `LD_PRELOAD` 경로 가상화는 타협이 아니라 유일한 선택지다. 이건 PRoot가 존재하는 이유이기도 하다.

→ **런타임 코드 어디에도 `mount()`/`chroot()`를 호출하지 말 것** (데드코드·에러 경로 포함). 그리고 게스트가 도달하지 못하게 preload에서 `mount`/`umount2`/`chroot`를 인터포즈해 `EPERM`을 반환한다. 안 하면 게스트의 `mount` 호출이 "Bad system call"로 프로세스를 죽인다.

→ 시작 시 `unshare(CLONE_NEWUSER)`가 `EINVAL`인지 어서션하고 로그에 남긴다. 미래에 USER_NS가 있는 커스텀 커널을 감지해 더 빠른 티어를 열 여지를 남긴다 (v1 아님).

### B5. PTY는 네이티브로 동작한다 — `SOURCE`

`/dev/ptmx` + devpts가 정상 동작한다. `posix_openpt`/`openpty`/`grantpt`/`forkpty` 모두 수정 없이 된다.

→ **상위 프로젝트의 socketpair PTY 에뮬레이션(`alr_pts.c`)은 필요 없다.** tmux, `script`, node-pty, Codex의 raw-mode TTY가 그대로 동작한다.

**단, 두 가지 예외:**

1. **`/dev/full`은 동작하지 않는다.** `ueventd.rc`가 생성하지만 sepolicy에 `full_device` 타입이 없어 `u:object_r:device:s0`로 떨어지고, `domain.te`의 `neverallow domain device:chr_file { open read write }`에 걸려 **모든 도메인에서 `EACCES`**. 스톡 Ubuntu와 그 테스트 스위트들이 `/dev/full`을 가정한다.

   > **에뮬레이션은 비목표로 확정됐다** — 2026-08-03. 이 문서가 요구하던 "쓰기 시 `ENOSPC`" 에뮬레이션은 **구현하지 않기로 했다**: 서빙하려면 프로세스에서 가장 뜨거운 `write()`를 인터포즈해야 하는데 대상 워크로드 중 이 노드를 쓰는 것이 없고, preload 자신이 내부적으로 `write()`를 부르므로 정의하면 자기 호출을 가로챈다(`__*_chk`와 같은 자기 재귀 계열). 게다가 실패 표면이 열려 있어(`puts`, `putchar`, `fwrite_unlocked`, `dprintf`, …) **빠뜨린 심볼은 전부 조용히 성공한 쓰기**가 된다 — 열거 가능하고 요란하게 실패하는 `mkstemp`·NSS 계열과 다르다. [증거](evidence/2026-08-03-m12-spawn-resolver.md) §9, [M13](evidence/2026-08-03-m13-symbol-gate.md) §6.15.
   > 수용 테스트는 `PRELOAD DEV FULL ENOSPC`를 `KNOWN_FAIL:non-goal-devfull`로 남겨 이 상태를 계속 보이게 한다. **`/dev/null`로 심링크하지 말 것** — ENOSPC 테스트가 조용히 통과해 버린다(그리고 프로브는 `open`이 아니라 실제 **쓰기**로 해야 한다. `: < /dev/full`은 O_RDONLY라 그 지름길을 잡지 못한다).
2. **PTY 슬레이브의 ioctl은 화이트리스트다.** sepolicy `unpriv_tty_ioctls`가 허용하는 것은 13개: `TIOCOUTQ FIOCLEX FIONCLEX TCGETS TCSETS TCSETSW TCSETSF TIOCGWINSZ TIOCSWINSZ TIOCSCTTY TCFLSH TIOCSPGRP TIOCGPGRP`. `TIOCSTI`는 `neverallowxperm`으로 원천 차단.
   → preload가 슬레이브 fd의 ioctl을 인터포즈해 흔한 것들을 번역해야 한다. 생 `EACCES`를 돌려주면 readline/ncurses 깊은 곳에서 이해 불가능한 실패가 난다.

   > ⚠️ **`FIONREAD`가 `EACCES`라는 위 목록의 추론은 틀렸다** — `MEASURED` 2026-08-03. 게스트가 `/dev/ptmx`로 직접 연 페어에 대한 실측 인구조사([증거](evidence/2026-08-03-m14-ioctl-php.md) §1):
   > **허용**: `TCGETS` `TCSETS` `TIOCGWINSZ` `TIOCSWINSZ` **`FIONREAD`** `TIOCOUTQ`.
   > **거부(`EACCES`)**: `TCGETS2` `TIOCGSID` `TIOCGETD` `TIOCEXCL` `TIOCSTI`.
   > `FIONREAD`는 그냥 허용된다. **"마스터 쪽 non-blocking read로 에뮬레이션"이라는 요구는 존재하지 않는 문제에 대한 것이었고**, 게스트는 마스터 fd를 쥐고 있지도 않아 그 요구가 §11 구현 전체를 잠가 두고 있었다. 전제를 재는 것이 잠금을 풀었다.
   > 실제로 필요했던 번역은 작다: `TCGETS2`→`TCGETS`(커널 `termios` 레이아웃, NCCS=19 — glibc의 것이 아니다), `TIOCGSID`→`getsid()`, `TIOCGETD`/`TIOCSETD`→`N_TTY`, `TIOCEXCL`/`TIOCNXCL`/`TIOCNOTTY`→0. **`TIOCSTI`는 계속 `EACCES`로 둔다** — `neverallowxperm`이라 허용될 수 없고, 성공을 가장하면 주입된 입력이 조용히 사라진다.
   > `TIOCPKT`/`TIOCLINUX`/`TIOCINQ`는 아직 재지 않았다. `TIOCINQ`는 `FIONREAD`와 같은 번호라 함께 허용이다.

### B6. 하드링크는 실패한다 — `SOURCE` ⚠️

같은 앱-데이터 디렉토리 안의 두 파일에 대한 `link(2)`가 **`EACCES`로 실패**한다 (Android 10 ~ 16 + main, SELinux AVC). ext4/f2fs 무관, `fs.protected_hardlinks` 무관.

**errno가 `EACCES`이지 `EPERM`/`EXDEV`가 아니라는 점이 중요하다** — `EXDEV`나 `EPERM`만 잡는 폴백 코드는 발동하지 않는다.

깨지는 것: `dpkg -i`(하드링크 멤버를 가진 tar), `git clone --local`(객체를 기본으로 하드링크), `pnpm`(콘텐츠 주소 저장소 전체가 하드링크).

→ **preload가 link2symlink 에뮬레이션을 구현해야 한다** ([ADR 0004](adr/0004-link2symlink.md)). 경로 재작성만으로는 부족하다.

### B7. Termux 자체 `LD_PRELOAD`를 반드시 정리해야 한다 — `SOURCE`

Termux는 `LD_PRELOAD`를 **bionic** `.so`(`libtermux-exec-ld-preload.so`)로 설정한다. 그게 glibc 자식으로 새면 게스트 ld.so가 로드에 실패한다.

**규칙 3개:**

1. **envp에서 항목을 제거한다. 빈 문자열로 두지 말 것.** termux-exec 자체 주석이 기록한다: `LD_PRELOAD= <command>`는 bionic 링커에서 `CANNOT LINK EXECUTABLE`로 실패한다. 빈 `LD_PRELOAD`는 no-op이 아니라 진짜 버그다.
2. **양방향·상태 유지다.** 두 개의 preload 값을 들고 exec 경계마다 교체한다.
   - bionic → glibc: Termux 값 제거, alr의 glibc `.so` 설치.
   - glibc → bionic (게스트가 `/system/bin/*`, `termux-open`, `am`, `pm` 호출): alr의 glibc `.so` 제거, Termux 값 복원.
3. `LD_LIBRARY_PATH`도 경계에서 함께 정리한다.

### B8. 시그널·잡 컨트롤은 정상, 그러나 **프로세스 수 제한**이 있다 — `SOURCE`

`setsid`/`tcsetpgrp`/시그널 포워딩에 Android 제약 없다. POSIX대로 구현하면 된다.

**진짜 제약은 Android 12+ phantom process 모니터링이다.** `ActivityManagerConstants.java`: `DEFAULT_MAX_PHANTOM_PROCESSES = 32` (android16-release에도 존재). 앱이 fork한 네이티브 프로세스가 이 수를 넘으면 ActivityManager가 죽이고, 앱이 포그라운드가 아니면 더 공격적으로 회수한다.

Node + Codex + 언어 서버 + git 서브프로세스는 32에 생각보다 빨리 도달한다.

→ (1) 살아 있는 자손 수를 세고 ~24 넘으면 경고, (2) Termux 포그라운드 알림(웨이크락)을 권장, (3) 파워유저용 탈출구 문서화 (`device_config put activity_manager max_phantom_processes <N>`, adb 필요), (4) **자식이 상태 없이 사라지면 "Android phantom process monitor에 의해 종료됨"이라고 보이게** 한다. 일반적인 행업으로 보이면 안 된다.

---

## C. glibc / ld.so 동작

### C1. 스톡 게스트 바이너리는 그냥 execve할 수 없다 — `SOURCE` ⚠️

게스트 바이너리의 `PT_INTERP`는 문자열 `/lib/ld-linux-aarch64.so.1`이고, **커널이 execve 시점에 실제 호스트 루트 기준으로 해석**한다 (`fs/binfmt_elf.c`). rootfs가 `$PREFIX/var/lib/alr/...`에 있으면 그 경로는 없다 → `ENOENT`.

→ **게스트 로더를 명시적으로 호출해야 한다.** [ADR 0002](adr/0002-explicit-ldso-invocation.md).

### C2. 명시적 ld.so 호출 규격 — `SOURCE`

```
execve("<R>/lib/ld-linux-aarch64.so.1",
       ["<R>/lib/ld-linux-aarch64.so.1",
        "--library-path", "<R>/lib/aarch64-linux-gnu:<R>/usr/lib/aarch64-linux-gnu:<R>/lib:<R>/usr/lib:<R>/usr/local/lib/aarch64-linux-gnu",
        "--inhibit-cache",
        "--argv0", "<게스트가 봐야 할 argv[0]>",
        "--preload", "<R>/usr/lib/alr/libalr_preload.so",
        "<프로그램의 호스트 절대경로>",
        <원래 argv[1..]>],
       envp)
```

- **`--argv0`은 glibc 2.33+**. Ubuntu 24.04(2.39)는 OK. Ubuntu 20.04/Debian 11(2.31)은 **없다** → 그런 rootfs에서는 argv[0]이 호스트 경로로 샌다. "아무 glibc rootfs나 지원"을 광고하려면 첫 실행 때 `ld.so --help`를 파싱해 지원 옵션 집합을 캐시할 것.
- **프로그램 인자에는 반드시 `/`가 있어야 한다.** `elf/dl-load.c:2017` `if (strchr(name,'/') == NULL)` — 슬래시 없는 이름은 `$PATH`가 아니라 **라이브러리 검색 경로**를 탄다. 절대 호스트 경로를 넘긴다.
- **`--library-path`는 필수다.** ld.so 자신의 라이브러리 탐색은 인터포즈할 수 없는 raw syscall이다(§C6). `LD_LIBRARY_PATH`로 대체하지 말 것 — 인터셉트하지 못한 경로로 재exec하는 자식에게 상속되고 `AT_SECURE`에서는 지워진다.
- **`--inhibit-cache`는 방어용.** 지금은 `/system/etc/ld.so.cache`가 없어 무해하게 실패하지만, 그 경로에 호스트 파일이 생기면 모든 라이브러리 해석이 조용히 오염된다.
- **정적 바이너리**: glibc ≥ 2.35의 ld.so는 정적 바이너리를 만나면 스스로 `execve`한다(거부가 아니다). 직접 분기하지 말 것. 다만 그 재exec는 **preload 없이** 일어나므로 그 프로세스가 후킹되지 않는다는 걸 알아야 한다.
- **유일한 하드 거부**: 게스트 프로그램의 `DT_SONAME`이 로더 자신의 SONAME과 같을 때. 런처에서 걸러낸다.
- **setuid는 조용히 드롭된다** (거부가 아님). `/data`가 `nosuid`라 어차피 무의미. `sudo`류는 동작하지 않으며 실패 모드가 혼란스러운 권한 오류라는 걸 문서화한다.

### C3. `/proc/self/exe`가 새는 것을 반드시 막아야 한다 — `SOURCE` ⚠️

명시적 로더 호출에서 `AT_EXECFN`은 glibc가 **복구해 준다**(흔한 오해). 실제로 새는 것은:
- **`/proc/self/exe`** → ld.so를 가리킨다.
- **`/proc/self/cmdline`** → 트릭 전체가 노출된다.

이건 nicety가 아니라 **하드 요구사항**이다. Node는 `process.execPath`를 `uv_exepath`로 얻고, libuv 구현은 문자 그대로 `readlink("/proc/self/exe", ...)`다. 보정하지 않으면 `process.execPath`가 ld.so 경로가 되고, **`process.execPath`로 재spawn하는 npm/npx/corepack이 "Node 버그처럼 보이는" 방식으로 깨진다.**

→ preload가 `readlink`/`readlinkat`/`open`/`openat`/`stat`/`realpath`에서 `/proc/self/exe`, `/proc/<자기 pid>/exe`, `/proc/thread-self/exe`를 가로채 게스트 경로를 돌려준다. `/proc/self/cmdline`도 가상화한다(git, ps류, 크래시 리포터가 읽는다).

### C4. `clone3` 폴백은 ENOSYS에서 동작한다 — `SOURCE`

`__clone3_internal`의 함수 로컬 `static int clone3_supported`. 첫 스레드만 트랩 비용을 내고 이후는 바로 `__clone`. `posix_spawn`(`spawni.c:415-420`)은 독립적인 clone3 시도가 있고 폴백 조건이 `errno == ENOSYS || errno == EINVAL`이다.

→ **`ENOSYS`를 에뮬레이션하면 두 경로 다 구제된다.** (단 `POSIX_SPAWN_SETCGROUP`은 폴백이 없다 — git/node에는 무관하지만 "posix_spawn 완전 지원"이라고 주장하지 말 것.)

**다만 A6에 따라 Android는 `clone3`을 실제로 허용하므로** 이 경로는 대개 타지 않는다.

### C5. ENOSYS 폴백을 가진 glibc 래퍼 — `SOURCE` (정정 포함)

**정정된 사실:**
- `access()`는 aarch64에서 `faccessat2`가 아니라 **`faccessat`**를 쓰며 폴백이 없다.
- **glibc 2.39는 `openat2`를 전혀 호출하지 않는다.**
- **aarch64에서 `stat()`/`fstatat()`는 `statx`를 타지 않는다.** `FSTATAT_USE_STATX`는 32비트 kABI 아키(arc, riscv32)에만 설정된다. aarch64는 `newfstatat`을 직접 호출한다. `statx`는 애플리케이션이 명시적으로 부를 때만 쓰인다.

**SIGSYS ENOSYS 구제가 실제로 살려내는 것**: `faccessat2`, `close_range`, `epoll_pwait2`, `futex_waitv`, `fchmodat2`, `io_uring_*`.

**폴백이 없어 위험한 것**: `getrandom` (Node/OpenSSL/glibc의 `arc4random` 시작 경로에 있음), `memfd_create`. 차단되면 프로세스가 우아한 저하 없이 죽는다. → **`alr doctor`가 명시적으로 프로브하고 크게 실패**해야 한다.

`close_range` 사이트는 `errno == ENOSYS`가 아니라 `r != 0`을 검사하므로 아무 에러나 반환해도 안전하지만, 폴백이 `/proc/self/fd`를 순회하므로 `/proc`이 보이고 올바르게 가상화되어 있어야 한다.

### C6. ld.so 자신은 인터포즈 불가능하다 — `SOURCE`

`git`/`node`/`bash`/`dpkg` 바이너리 본체는 raw syscall을 쓰지 않는다 (측정 확인). **그러나 게스트 로더 자체는 모든 프로세스 기동마다 도는 인터포즈 불가 raw-syscall 컴포넌트**이고, 하필 가상화하고 싶은 경로 해석을 담당한다.

→ 인터셉션이 아니라 **선언적으로 푼다**: `--library-path`로 로더가 재작성이 필요한 경로를 아예 만나지 않게 하고, `--inhibit-cache`로 호스트 캐시를 안 보게 한다.

### C7. libuv는 raw syscall을 쓴다 — `SOURCE` ⚠️ Node 워크로드 최대 위협

Node의 모든 `fs.stat`/`fs.lstat`/`fs.statSync`가 **게스트 경로를 재작성 없이 커널로 직행**시켜 Android 실제 루트 기준으로 해석 → `ENOENT`. 성능 문제가 아니라 **하드 브레이크**이고, 증상은 "Node가 가끔 파일을 못 본다"로 보인다.

**대응 (선호 순):**
- (a) preload에서 **`syscall()` 자체를 인터포즈**해 path-bearing `__NR_*`의 인자를 재작성한다. `long` 하나에 대한 switch — 싸고, libuv를 잡는다. Go/Rust asm의 인라인 `svc`는 못 잡는다.
- (b) (a)와 병행.

### C8. Node 20+ / libuv 1.45+는 `io_uring_setup`을 호출한다 — `SOURCE` ⚠️

루프 초기화 시 호출 → 차단되면 **SIGSYS로 사망**. `UV_USE_IO_URING=0`은 SQPOLL 링만 게이트하므로 **고치지 못한다**.

- Ubuntu 24.04 아카이브의 nodejs는 18.19 / libuv 1.44.2라 **안전**하다.
- 사용자가 NodeSource나 nvm으로 Node 20/22를 깔면 **즉시 터진다** — Codex 사용자는 거의 다 그렇게 한다.

→ SIGSYS 구제로 `-ENOSYS`를 반환하면 libuv의 `ringfd=-1` 실패 경로가 발동하고 스레드풀로 올바르게 저하된다. 루프 초기화당 stop 1회 비용. **이것이 슈퍼바이저를 상시 유지해야 하는 두 번째 이유다.**

---

## D. 워크로드 특성

### D1. `git status`는 최고의 벤치이자 최악의 인터포저 부하 — `SOURCE`

지배 syscall: `openat`/`newfstatat`/`getdents64`. 10k 파일 저장소에서 **재작성 호출 12–15k회**.

**여기서 직접 따라 나오는 최적화 2개:**
1. git은 `openat(dirfd, 상대경로)`를 점점 더 쓴다. **상대 경로는 재작성이 전혀 필요 없으므로**, memcmp 전에 **첫 바이트가 `/`인지만 검사**하는 경로를 둔다.
2. `.gitignore` 프로브 때문에 디렉토리당 ~2회의 대부분 ENOENT인 open이 발생한다. **네거티브 경로가 포지티브 경로만큼 싸야 한다.**

> ✅ **실측으로 확인됨** (2026-08-02, 10k 파일 `git status`): 전체 9,912회 중 **재작성은 26회뿐이고 9,887회(99.7%)가 상대경로**였다. 경로 계층 총비용 ≈ **40 µs**. 아래 모델(13,500회 재작성)은 20배 과대평가였다. [증거](evidence/2026-08-02-m7-m8-workloads-perf.md)

**예산: 재작성 1회당 ≤ 100 ns.** 12–15k 호출에서 1 µs짜리 인터포저는 12–15 ms, 4 µs짜리는 50–60 ms를 먹는다 — `git status` 이득 전체와 맞먹는다. 참고로 상위 프로젝트의 in-process 변환기는 **cold 4,334.7 ns/op**(≈ bare syscall 20회)를 측정했다. 이것이 정상 상태 비용이라면 예산의 **40배**다. 캐시 히트율이 load-bearing 미지수다.
**memcmp보다 비싼 캐시 조회를 추가하지 말 것.**

### D2. LD_PRELOAD의 per-exec 비용 — `SOURCE`

per-syscall 비용은 없지만 **per-exec 비용은 있다**: execve마다 DSO 하나가 추가로 매핑·재배치되고, 전역 심볼 검색 스코프에 객체가 하나 더 늘어 모든 lazy PLT 해석이 느려진다.

→ exec 집약 워크로드(`npm ci` 라이프사이클 스크립트, `./configure` 루프, git hook, git의 `git-remote-https`/`ssh`/pager spawn)에서 **오버헤드가 드러나는 곳은 여기지 `git status`가 아니다**. `getpid` 처리량이 아니라 **exec 처리량**을 측정할 것.

### D3. 인터포즈 불가능한 병리적 워크로드 — `SOURCE`

- **NSS / `getaddrinfo`**: `/etc/{passwd,group,hosts,resolv.conf,nsswitch.conf}` — 인터포즈로 못 푼다. 게스트의 `/etc`가 glibc가 실제로 열 호스트 경로에서 도달 가능해야 하거나, 문제되는 경로를 피하는 `nsswitch.conf`를 게스트에 넣어야 한다.
- **Node 모듈 해석**: 깊은 `node_modules`에 대한 realpath + stat 폭풍.
- **Go 툴체인 바이너리** (`gh`, docker CLI, hugo, 대부분의 GitHub 릴리스 바이너리): 계층을 그냥 무시한다.

### D4. Codex CLI는 Rust다 — `FIELD`

`openai/codex`는 Rust(`codex-rs`)이고 npm 배포용 Node shim은 선택적이다. 아티팩트: `codex-aarch64-unknown-linux-musl.tar.gz` (릴리스 태그 `rust-v0.146.0` 기준, ~105 MB).

**결과 2개:**
1. 바이너리 직접 설치를 권장하면 **Codex 경로에서 Node를 완전히 제거**할 수 있다 → C8의 io_uring SIGSYS 위험도 사라진다. (사용자 자기 프로젝트용 Node는 여전히 필요하겠지만.)
2. **musl 정적 링크는 "rootfs의 glibc에 의존하지 않는다"가 아니라 "`LD_PRELOAD`가 닿지 않는다"는 뜻이다** — `MEASURED` 2026-08-03. `rustix`의 raw-syscall 백엔드를 쓰는지는 이제 물을 필요가 없는 질문이 됐다. 그보다 앞에서 끝난다: 설치된 `codex`에는 `PT_INTERP`도 `DT_NEEDED`도 없고(ET_EXEC), `ALR_LOG=2`로 돌리면 `alr preload:` 줄이 **0개**다(대조군 `git`은 1개). **preload가 애초에 로드되지 않으므로 경로 가상화가 전혀 적용되지 않고, codex의 모든 경로 연산은 rootfs가 아니라 Android 파일시스템으로 간다.** 겉으로 드러나는 증상은 시작 시의 `WARNING: proceeding, even though we could not create PATH aliases: Read-only file system`이다. [증거](evidence/2026-08-03-m12-spawn-resolver.md) §8, [RISKS R7](RISKS.md).
   → 따라서 `codex --version`이 도는 것은 **바이너리가 실행된다**는 뜻이지 **게스트 안에서 동작한다**는 뜻이 아니다. 수용 테스트 `ALR CODEX LINKAGE`가 `KNOWN_FAIL:static-unhooked`로 이 상태를 추적하고, 업스트림이 동적 빌드로 바뀌면 자동으로 알아챈다.
   → 정적/raw-syscall 바이너리는 계속 **비목표**다. 다만 근거가 정정되었다 — "원리적으로 가로챌 수 없다"가 아니라 **비용**이다. seccomp user notification은 이 커널에서 실제로 동작하지만(`no_new_privs`만 켜면 된다) 가로챈 syscall 하나당 **154 µs**이고 필터 없는 베이스라인은 **438 ns**다. 필터 *평가*는 공짜다. arm64 `PR_SET_SYSCALL_USER_DISPATCH`는 이 커널에 없다(인자 두 형태 모두 `EINVAL`). [ADR 0006](adr/0006-raw-syscall-binaries.md).

**Codex의 Linux 샌드박스는 반드시 꺼야 한다** — Landlock과 bubblewrap이 Android 앱 프로세스에서 동작하지 않는다. seccomp 절반은 실제로 동작하지만(prctl 허용, no_new_privs는 우리가 켤 수 있음, 필터 스택 허용 — ADR 0006) 그것만으로는 부족하다.

→ **지금 실제로 하는 일**: `alr install --with codex`가 `<rootfs>/root/.codex/config.toml`에 `sandbox_mode = "danger-full-access"`를 쓰고, `alr: NOTE codex sandbox disabled; alr is not a security boundary`를 출력한다 ([`src/cli/alr.c`](../src/cli/alr.c) `with_codex()`).

→ **실측으로 닫혔다 (2026-08-04).** 두 질문 다 답이 나왔다.

**철자**: `codex --help`(codex-cli 0.146.0)가 CLI 플래그 `-s, --sandbox <SANDBOX_MODE>` 와 값 `read-only | workspace-write | danger-full-access` 를 보여 준다. 설정 키 `sandbox_mode` 는 `codex doctor` 가 되읽어 확인해 준다 — `sandbox_mode = "danger-full-access"` 를 쓴 뒤 doctor 가 `sandbox  unrestricted fs + enabled network` 로 보고한다.

**codex 가 그 파일을 읽는가**: **읽는다 — 단, `HOME` 이 호스트 형식일 때만.** 판정 방법은 센티널이었다. `<R>/root/.codex/config.toml` 에 `model = "alr-sentinel-model"` 을 넣고 `codex doctor` 의 Configuration 절을 본다:

| `HOME` | doctor 보고 |
|---|---|
| `/root` (게스트 형식, 이전 동작) | `model <default>` — **설정을 못 읽음** |
| `<R>/root` (호스트 형식) | `model alr-sentinel-model`, `config.toml parse ok` |

당연한 결과다. codex 는 정적이라 `/root` 를 **Android 의** `/root` 로 푼다 — 읽을 것도 없고 쓸 수도 없다(그래서 `could not create PATH aliases: Read-only file system` 이 나왔다). `alr` 은 이제 정적 타깃에 호스트 형식 환경을 준다([ADR 0008 보론](adr/0008-static-guest-binaries-non-goal.md)).
  - 함께 확인해야 할 것: **그 파일을 codex가 읽기는 하는가.** `alr`은 게스트 환경에 `HOME=/root`를 넣는데(`alr.c`), codex는 후킹되지 않으므로 `/root`를 rootfs가 아니라 **Android 루트** 기준으로 푼다. 그렇다면 우리가 쓴 config에 도달하지 못한다. 이것은 두 사실(위 2번의 무후킹 + `HOME=/root`)로부터의 추론이고 **UNVERIFIED**다 — 관찰로 확정한 적이 없다.
  - **무엇이 이것을 끝내는가**: 기기에서 `codex --help` 출력 1회, 그리고 codex가 실제로 여는 config 경로 1회. 후자는 `ALR_LOG`로는 보이지 않는다(preload를 안 타므로) — `strace -f`나 codex 자체 진단이 필요하다.
  - **무엇이 막고 있는가**: 기기 세션. 기술적 장애물은 없다.

→ 부수 효과: 중첩 필터는 커널의 전체 32768 instruction 예산을 공유한다. **자체 필터는 작게 유지**할 것.

---

## E. 프로비저닝 사실

### E1. Ubuntu base tarball — `FIELD` (정정 포함)

- **`ubuntu-base-24.04-base-arm64.tar.gz`는 존재하지 않는다 (404).** 파일명은 포인트 릴리스로 붙는다: `ubuntu-base-24.04.4-base-arm64.tar.gz` (~29.9 MB). `latest` 심링크 없음.
- → 프로비저너는 (1) `.../releases/24.04/release/SHA256SUMS`를 받고, (2) `ubuntu-base-24\.04\.(\d+)-base-arm64\.tar\.gz` 중 최고 버전을 정규식으로 뽑고(파일명 앞 `*` 제거), (3) 그 이름을 받아 같은 파일의 해시로 검증한다. 오프라인용으로 24.04.4를 폴백 핀으로 둔다.
- **proot-distro는 더 이상 tarball URL을 가진 셸 플러그인이 아니다.** v5.0.2(2026-05-17)부터 Python이고 `registry-1.docker.io`에서 OCI 이미지를 받는다.
- Debian은 익명 Docker Hub OCI 풀 3단계(`auth.docker.io` 토큰 → 매니페스트 인덱스 → `platform.architecture==arm64 && os==linux` 선택 → blob GET)로 구현한다. **같은 코드가 Ubuntu에도 동작하므로 다운로더 하나로 두 배포판을 커버**한다. cdimage 경로는 Ubuntu 전용 최적화다. 예산: Ubuntu ~30 MB, Debian ~50 MB.

### E2. 비루트 추출에서 실제로 깨지는 것 — `FIELD` (정정 포함)

**깨지지 않는 것 (통념 정정):**
- ubuntu-base와 Debian OCI 레이어에 **디바이스 노드가 없다.** `/dev`, `/proc`, `/sys`, `/run`은 빈 디렉토리로 온다. `mknod`는 시도조차 되지 않는다.
- **xattr / file capability가 없다** (`SCHILY.xattr.*` pax 헤더 0건). `ping`은 ubuntu-base에 아예 없다.

**진짜 문제:**
- 소유권이 추출 UID로 붕괴한다 (chown 불가).
- setuid/setgid 비트는 설정 불가하며 어차피 무의미(`/data`는 nosuid). 해당 12개 바이너리(`mount`, `umount`, `su`, `passwd`, `chfn`, `chsh`, `gpasswd`, `newgrp`, `chage`, `expiry`, `unix_chkpwd`, `pam_extrausers_chkpwd`)는 필요도 원하지도 않는 것들이다 → `0755`로 마스킹해 이후 경고가 안 나게 한다.
- 하드링크가 복사로 저하될 수 있다 (B6).

**추출기 요구사항**: blk/chr/fifo 멤버 무조건 스킵(방어용), `/dev`·`/proc`·`/sys`·`/run` 직접 생성, `chown`/`lchown` 절대 호출 금지, xattr 스킵.

### E3. 추출 후 수리는 매우 짧다 — `FIELD` (정정 포함)

**불필요한 것 (통념 정정):**
- ❌ `ubuntu.sources` 작성 — arm64 tarball이 이미 `ports.ubuntu.com/ubuntu-ports`와 noble/updates/backports/security, 올바른 `Signed-By` 키링을 갖고 온다. **덮어쓰면 오히려 망가진다.**
- ❌ `/etc/passwd`·`/etc/group`에 apt용 항목 추가 — 완비되어 있고 `_apt`(uid 42), `shadow`(gid 42)가 이미 있다.
- ❌ dpkg `path-exclude` — `/etc/dpkg/dpkg.cfg.d/excludes`가 이미 있다.

**반드시 써야 하는 것** (둘 다 0바이트로 오며, 나중 이미지가 심링크로 줄 수 있으니 **먼저 unlink**):
- `/etc/resolv.conf` → `nameserver 8.8.8.8\nnameserver 8.8.4.4\n`
- `/etc/hosts` → `127.0.0.1 localhost\n::1 localhost ip6-localhost ip6-loopback\n`

**써야 하는 것:**
- `/etc/apt/apt.conf.d/99-alr-no-sandbox` → `APT::Sandbox::User "root";`
  **첫 `apt update` 이전에 반드시**. 안 하면 apt가 `setgroups()`에서 즉사한다 (setuid 계열이 seccomp로 차단됨, A6). apt 자체의 자동 저하는 접근성 검사에서만 발동하는데 Android에서는 `setgroups()`가 먼저 치명적이다.
- `/etc/dpkg/dpkg.cfg.d/99-alr` → `force-unsafe-io` (fsync 생략 — Android 플래시에서 큰 속도 향상)
- `/etc/passwd`·`/etc/group`에 Termux UID/GID 줄 추가 (게스트의 `ls -l`, `getpwuid()` 해석용)

**건드리지 말 것**: `/etc/apt/sources.list`, `/etc/dpkg/dpkg.cfg.d/excludes`, `/etc/nsswitch.conf`, `/etc/passwd`, `/etc/group` 본문.

`ca-certificates`가 없다 — apt(http)에는 무관하지만 **HTTPS 다운로드는 깨진다.**

### E4. git / node / codex 설치 경로 — `FIELD`

권장 순서:
1. `resolv.conf`/`hosts`/apt 샌드박스 설정 작성
2. `apt-get update && apt-get install -y --no-install-recommends ca-certificates git xz-utils` (plain http)
3. Node: **apt 대신 nodejs.org tarball**. apt의 18.19.1은 EOL이다. `https://nodejs.org/dist/index.json`에서 `lts`가 truthy인 첫 항목을 동적으로 뽑고(폴백 v24.18.1), `SHASUMS256.txt`로 검증, `/opt/node`에 풀고 `/usr/local/bin`에 심링크. **단계 2 이후여야 한다** (HTTPS라 ca-certificates 필요). NodeSource는 MVP에서 배제.
4. Codex: GitHub 릴리스 tarball 직접 다운로드. npm 경로 금지(~129 MB 플랫폼 tgz + Node 런타임을 같은 바이너리 exec하려고 끌어온다). `https://chatgpt.com/codex/install.sh | sh` 금지 — 루트 가능한 정상 Linux를 가정한다.

**git은 apt에서만 온다** (업스트림 prebuilt 없음, main에 있음).

---

## F. 툴체인 사실

### F1. 호스트 측(bionic) — `FIELD`

**NDK를 고정한다**: NDK 29, `--target=aarch64-linux-android24`.
Termux 네이티브 clang을 **릴리스 경로로 쓰지 말 것** — LLVM 버전이 사용자의 `pkg upgrade`를 따라가고, 디바이스 없이는 CI가 못 돌고, 사용자 간 재현이 안 된다. 온디바이스 이너 루프로만 쓴다.

API 24는 **래퍼**를 게이트하지 커널을 게이트하지 않는다. API 24 위의 syscall은 전부 `syscall(__NR_x, ...)`와 로컬 선언 struct로 간다: `memfd_create`, `statx`, `renameat2`(API 30), `pidfd_open`(31), 그리고 bionic이 절대 export하지 않는 `openat2`/`faccessat2`/`seccomp`.
→ **`alr_sys.h` 하나**에 이것들을 inline `syscall()` shim으로 선언해 어떤 소스 파일도 API 레벨에 우연히 의존하지 않게 한다.

`-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384`를 지금 넣는다 (NDK 29에서는 기본값이라 무해). 16 KB 페이지 arm64 디바이스에서 로드 가능하게 유지.

### F2. 게스트 측(glibc ABI) — `FIELD` (정정 포함)

`zig cc`를 쓰되 **zig 버전을 정확히 핀**하고(예: 0.16.0), 타깃은 **`aarch64-linux-gnu.2.17`** (상위 프로젝트의 `.2.36`이 아니다).

2.17이 AArch64 ABI 베이스라인이고 인터포저는 그보다 새 것을 참조하지 않는다. 게다가 `.2.17`은 `stat`/`fstatat`(GLIBC_2.33+) **링크를 금지**하는데, 이것이 올바른 강제 장치다: 인터포저는 stat 계열을 **호출**하면 안 되고 **정의**만 해야 한다.

→ 인터포저는 **≥2.33 이름과 pre-2.33 이름을 모두 정의**해야 한다:
`stat`, `stat64`, `lstat`, `lstat64`, `fstat`, `fstat64`, `fstatat`, `fstatat64` **그리고**
`__xstat`, `__xstat64`, `__lxstat`, `__lxstat64`, `__fxstat`, `__fxstat64`, `__fxstatat`, `__fxstatat64`.
심볼 정의에는 링크타임 스텁이 필요 없으므로 2.17에서 비용 0이다.

**재현성**: "zig는 빌드 타임스탬프를 안 박으므로 반복 실행이 비트 동일"은 **고정된 zig 버전에서만** 참이다. zig 업그레이드는 번들 compiler-rt와 LLVM 코드젠을 바꾼다. → `{zig_version, target, source_sha256, output_sha256}` JSON 매니페스트를 `.so` 옆에 낸다.

**CI 게이트**: `readelf -V libalr_preload.so`의 `DT_VERNEED`에 GLIBC_2.17 초과 버전이 **없어야** 한다. 이 검사 하나가 "우리가 지원하는 모든 rootfs에서 이 `.so`가 로드된다"를 보장한다.

### F3. 인터포저 컴파일 플래그 — `FIELD` (정정 포함)

```
zig cc --target=aarch64-linux-gnu.2.17 -shared -fPIC -O2 -D_GNU_SOURCE \
       -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
       -fno-stack-protector -fvisibility=default -Wall -Wextra
```

- **`-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0`은 놓치기 쉬운 필수 항목이다.** 배포판/CI 기본 CFLAGS가 `-D_FORTIFY_SOURCE=2|3`을 넣으면, 인터포저가 **자기가 정의한 `__*_chk` 심볼을 호출**하게 되어 자기 재귀에 빠진다.
- `-fno-stack-protector`는 정확성상 필수는 아니지만(초기 생성자/크로스 libc/실패 시 재귀 케이스에 대한 표적 완화) 비용 0에 원인 불명 행업 한 부류를 없애므로 넣는다.

**`__*_chk` 래퍼 목록이 상위 프로젝트에서 불완전하다.** 반드시 추가: `__openat_2`, `__openat64_2`, `__getcwd_chk`, `__getwd_chk`, `__ttyname_r_chk`. 빠지면 fortify된 게스트가 `openat()`(mode 없이), `getcwd()`, `ttyname_r()`을 부를 때 경로 중재를 빠져나간다.

각 래퍼는 후행 size 인자를 **그대로 전달**해 glibc 자체 오버플로 가드가 동일하게 동작하게 한다.

**래퍼 표는 단일 생성 목록**(name, signature, path-arg index)으로 관리해 C 소스 / CI 심볼 존재 테스트 / 문서가 서로 어긋나지 못하게 한다. CI에서 `nm -D --defined-only`로 표의 모든 이름이 `GLOBAL DEFAULT`로 있는지 검사한다.

### F4. 인라인 syscall 트램폴린 — `FIELD`

naked 함수 + named section + `__start_`/`__stop_` 바운드 심볼 구성은 검증되어 있다. 단:
- 컴파일러가 clang(zig cc 또는 NDK clang)인지 빌드타임 어서션. GCC 경로가 필요해지면 독립 `alr_tramp.S`로 분리한다 (`.section alr_tramp,"ax",@progbits`).
- 섹션 GC 방어: `__attribute__((retain))` 또는 `-Wl,-z,nostart-stop-gc`. `__start_`/`__stop_`을 직접 정의하지 말 것.
- 두 extern 선언에 `visibility("hidden")`.
- **포스트링크 CI 검사**: `alr_tramp` 섹션을 디스어셈블해 `svc`가 **정확히 1개**이고 `__stop - __start`가 예상 명령어 수 × 4인지 어서션.

> **v1에서는 자체 seccomp 필터를 설치하지 않으므로 PC 게이트가 필요 없다.** 그러나 트램폴린 구성 자체는 §C7의 `syscall()` 인터포즈 구현에 유용하다. 필터를 다시 도입하게 되면(compat 모드) PC 게이트를 함께 되살린다.

### F5. 개발 루프 — `FIELD`

- **호스트 유닛 테스트**: 경로 변환 규칙을 **한 곳**에 두고 Linux 헤더를 전혀 include하지 않는 TU로 격리한다(`alr_path_rule.h`). 그러면 macOS에서 `clang++`로 컴파일·실행된다.
- **드리프트 방지**: `(ALR_ROOT, input) -> expected` 공유 테이블 하나를 C 테스트 / C++ 테스트 / Python 레퍼런스 모델이 모두 소비한다. 슈퍼바이저 쪽과 인터포저 쪽 재작성기가 어긋나는 것을 막는 유일한 방어다.
- **CI 크로스 실행**: `qemu-aarch64 -L /usr/aarch64-linux-gnu ./test`로 진짜 크로스 빌드 오브젝트를 CI에서 돌린다.
- **온디바이스**: `adb shell`은 Termux 앱 컨텍스트로 들어가지 못한다. Termux 안에서 `sshd`를 띄우고 `adb forward tcp:8022 tcp:8022` 후 ssh로 들어가는 것이 가장 확실하다. `scripts/dev-push.sh`가 이 루프를 감싼다.

---

## G. 즉시 프로브해야 할 항목 (`alr doctor`의 최소 집합)

| # | 프로브 | 실패 시 |
|---|---|---|
| P0 | `ro.build.version.release == 16` | 미지원 릴리스 → `WARN` 후 **계속 진행**. 거절하지 않는다 ([ADR 0007](adr/0007-android-16-only.md)) |
| P1 | `getenforce == Enforcing` && `/proc/self/status`의 `Seccomp: 2` | 벤치마크 결과 무효 표시 |
| P2 | syscall 0..460 스윕 → 디바이스 실제 차단 집합 덤프 | 슈퍼바이저 에뮬 테이블 자동 확장 |
| P3 | `$PREFIX`의 스크래치 ELF execve | `EACCES` → 치명적, Play 빌드 또는 정책 변경 |
| P4 | 익명 `mmap RW → mprotect RX → call` | 실패 → Node JIT 불가 |
| P5 | rootfs 파일의 file-backed `PROT_EXEC` mmap | 실패 → 설계 근본 붕괴 |
| P6 | 같은 디렉토리 `link(2)` | `EACCES` → link2symlink 활성화 |
| P7 | `unshare(CLONE_NEWUSER)` | `EINVAL` 기대. 아니면 미래 fast tier 후보로 로깅 |
| P8 | `posix_openpt`/`grantpt`/`unlockpt` | 실패 → PTY 에뮬레이션 필요 (예상 밖) |
| P9 | `open("/dev/full")` | `EACCES` 기대 → 에뮬레이션 활성화 |
| P10 | `getrandom`, `memfd_create` 가용성 | 차단 → **크게 실패**, 폴백 없음 |
| P11 | `svc #0` 스캔: raw syscall 발행 바이너리 탐지 | `alr doctor --scan <바이너리>`. 0 → PASS, 그 외 → WARN |
| P12 | 살아 있는 자손 수 vs phantom 한도 32 | 40개 fork 후 생존 수. 40 미만 → WARN |

> ✅ **P11·P12 구현됨 — 2026-08-04.** 이 표에 오래 설계로만 있었고 다른 문서는 "P1~P12 를 전부 실행한다" 고 적으며 예시 출력까지 보여 주고 있었다.
>
> **P11 이 왜 `--scan` 을 요구하는가.** 스캔할 대상이 있어야 답이 있다. 인자가 없으면 `SKIP` 이고, 그건 검사가 아니므로 수용 시험이 **항상 인자를 준다** — 세 갈래를 각각 밟는 세 개의 결정적 대상으로:
>
> | 대상 | 결과 (`MEASURED` 2026-08-04) |
> |---|---|
> | `/usr/bin/git` (동적 C) | dynamic, 3,884,492 exec bytes, **0** `svc #0` → PASS |
> | `ld-linux-aarch64.so.1` | **STATIC** (PT_INTERP 없음) → WARN |
> | `codex` 0.146.0 (정적 musl Rust) | **STATIC**, 263,228,576 exec bytes, **684** `svc #0` → WARN |
> | `libc.so.6` (대조) | dynamic, 1,678,737 exec bytes, **517** `svc #0` |
>
> aarch64 `svc #0` 은 `0xd4000001` 이고 명령어는 4바이트 정렬이라 **정렬된 워드만 본다** — 데이터에 우연히 같은 바이트열이 어긋난 오프셋에 있어도 잡히지 않는다. `PT_LOAD` 중 `PF_X` 세그먼트만, 64 KiB 씩 나눠 읽는다(codex 는 269 MB 다).
>
> **숫자를 읽는 법이 출력에 함께 나온다.** libc 자체는 정의상 `svc` 로 가득하므로 libc 를 스캔한 수는 아무 뜻이 없다. 정상적인 동적 실행파일은 **0** 이다. 0 이 아닌 동적 실행파일은 그 호출들만큼 인터포저를 우회하고 있다는 뜻이다. 정적 실행파일은 개수와 **무관하게** 이미 진 싸움이다 — `LD_PRELOAD` 가 아예 로드되지 않는다.
>
> 이건 기존 `reason=unhooked-static-binary` 보다 정확하다. 그건 **정적 링크를 프록시로** 쓰기 때문에 동적으로 링크된 Go·rustix 바이너리를 통째로 놓치고, libc 만 부르는 정적 C 바이너리를 문제인 것처럼 표시한다.
>
> **P12 는 `MEASURED` 2026-08-04, SM-X236N: 40/40 생존 → PASS.** 이 기기에서는 phantom killer 가 40개까지 작동하지 않는다. 좀비를 생존으로 세지 않도록 `kill(pid,0)` 이 아니라 `waitpid(WNOHANG)` 으로 센다 — phantom 에 죽은 자식은 수확 전까지 좀비이고 `kill(pid,0)` 은 좀비에게 성공하므로, 8개를 죽인 기기에서 40 생존을 보고했을 것이다.
