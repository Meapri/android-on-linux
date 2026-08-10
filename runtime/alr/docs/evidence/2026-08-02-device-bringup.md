# 2026-08-02 — 참조 기기 #1 브링업

첫 실제 기기 확보 및 측정 컨텍스트 확립 기록. `alr doctor` 실행 이전까지의 사실만 담는다.

## 기기

| 항목 | 값 |
|---|---|
| 모델 | Samsung SM-X236N (Galaxy Tab A9+) |
| Android | 16 (SDK 36) |
| SELinux | **Enforcing** ✅ |
| SoC | MediaTek MT8775 / `ro.hardware=mt6878` ⚠️ |
| 커널 | `6.1.145-android14-11-abX236NKOS6BZG2` aarch64 |
| ABI | arm64-v8a |
| 페이지 크기 | 4096 |

> ⚠️ **스냅드래곤이 아니다.** 제품 목표는 스냅드래곤 기준이다. seccomp·SELinux·exec 정책은 전부 AOSP 레벨이라 **호환성 결과는 SoC와 무관하게 유효**하지만, [00-product.md §4](../00-product.md)의 성능 배수는 이 기기로 확정할 수 없다. M8 전에 참조 기기 #2(Snapdragon)가 필요하다.

## Termux

`termux/termux-app` v0.118.3 `github-debug` arm64-v8a.
SHA256 `72fdb596045116bf5ba1b5bdf5b26fddb9acc0bd074ad9f2da9eb0ae85e83a4e` — 게시자의 `..._sha256sums`와 **일치 확인**.

```
versionCode=1002  minSdk=24  targetSdk=28   ✅ 설계 요구 충족
flags=[ DEBUGGABLE HAS_CODE ALLOW_CLEAR_USER_DATA ]
app uid = u0_a297
```

`targetSdk=28`이 확인되었으므로 `untrusted_app_27` 도메인의 `app_data_file:file execute_no_trans` 허용이 성립한다 ([§B1](../01-platform-facts.md)). `alr doctor` P3가 이를 실제 execve로 확증한다.

## 발견 1 — Play Protect가 설치를 차단한다 (R2b)

`adb install`이 완료되지 않고 `com.android.vending/...PlayProtectDialogsActivity`에서 정지:

```
Google Play 프로텍트 / 안전하지 않은 앱 차단됨 / Termux
이 앱은 Android 이전 버전에 맞게 개발되었으며 최신 개인 정보 보호 기능을 포함하지 않습니다.
[세부정보 더보기]  [확인]
```

- 기본 대화상자에 진행 버튼이 **없다**. `확인`은 설치를 취소한다.
- 진행 경로: `세부정보 더보기` → 확장된 본문의 `무시하고 설치하기` 링크.
- 승인 후 설치 정상 완료.

**함의**: GMS가 있는 모든 소비자 기기에서 사용자가 이 마찰을 겪는다. `alr`이 고칠 수 없고(Termux의 targetSdk에 달림) 최종 설치 안내에 반드시 포함해야 한다. 상세는 [RISKS.md R2b](../RISKS.md).

## 발견 2 — 측정 가능한 컨텍스트는 하나뿐 (실측 확증)

```
adb shell → uid=2000(shell)  context=u:r:shell:s0  Seccomp: 0  Seccomp_filters: 0
```

**필터가 아예 없다.** 여기서 잰 syscall 결과는 전부 거짓 ALLOWED다.

이 Termux 빌드는 `DEBUGGABLE`이라 `run-as com.termux`가 동작하지만 **그것도 쓰면 안 된다** — zygote를 거치지 않아 마찬가지로 필터가 없고 SELinux 도메인도 `runas_app`으로 다르다. (`run-as`는 파일 배치용으로만 사용했다.)

유효한 컨텍스트는 **Termux 앱이 fork한 프로세스**뿐이며, 그래서 sshd 경유 접속이 필요하다 ([scripts/dev-bootstrap.md](../../scripts/dev-bootstrap.md)).

`alr doctor` P1이 이 조건(`uid≥10000` ∧ `Seccomp==2` ∧ 도메인이 `untrusted_app*`)을 검사하고, 불만족이면 **나머지 프로브를 거부**한다. 거짓 PASS보다 무응답이 낫다.

## 발견 3 — 16KB 페이지 사전 경고 (이 기기에는 무영향)

Termux 첫 실행 시 Android 호환성 대화상자:

```
이 앱은 16KB와 호환되지 않습니다. ELF 정렬 검사에 실패했습니다.
• lib/arm64-v8a/libtermux.so: LOAD 세그먼트가 정렬되지 않음
• lib/arm64-v8a/libtermux-bootstrap.so: LOAD 세그먼트가 정렬되지 않음
```

이 기기는 `getconf PAGE_SIZE == 4096`이라 실제 영향은 없다. Android 15+가 16KB 페이지 기기 대비 사전 고지하는 것이다.

**함의**: [§F1](../01-platform-facts.md)의 `-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384` 요구가 확인되었다. `alr` 아티팩트가 같은 경고를 내지 않도록 처음부터 넣는다. 16KB 페이지 기기를 참조 기기 #3으로 확보할 가치가 있다.

## `alr doctor` 결과 — **VERDICT: READY (FATAL 0)**

측정 컨텍스트: `uid=10297`, `u:r:untrusted_app_27:s0`, `Seccomp=2` (P1 PASS).
sshd → ssh 세션으로 접속. 온디바이스 clang 21.1.8로 컴파일.

### 설계 핵심 가정 — 전부 확증

| 프로브 | 결과 | 의미 |
|---|---|---|
| **P2a `set_robust_list`(99)** | **BLOCKED** | ✅ [ADR 0001](../adr/0001-signal-only-ptrace-supervisor.md)의 전제 확증. 순수 LD_PRELOAD로는 스톡 glibc가 부팅 불가. **슈퍼바이저 필수** |
| **P10 `getrandom`(278)** | **allowed** | ✅ [R1 해소](../RISKS.md) — Fatal 리스크 제거 |
| **P10 `memfd_create`(279)** | **allowed** | ✅ R1 해소 |
| **P3 app-private ELF execve** | **PASS** | ✅ targetSdk 28 exec 허용 확증. 설계 전체의 전제 |
| **P5 file-backed `PROT_EXEC` mmap** | **PASS** | ✅ ld.so가 게스트 `.so`를 매핑 가능. 기반 안전 |
| P4 익명 mmap RW→RX | PASS | Node/V8 JIT 가능 |
| P7 `unshare(CLONE_NEWUSER)` | **EINVAL** | ✅ 예측대로 커널에 기능 없음. 경로 가상화가 유일 선택지 |
| P6 `link(2)` | **EACCES** | link2symlink **필수** ([ADR 0004](../adr/0004-link2symlink.md)) |
| P9 `/dev/full` | **EACCES** | 에뮬레이션 **필수** |
| P8 PTY (`/dev/ptmx`) | PASS (pts/1) | socketpair PTY 에뮬레이션 불필요 |
| `rseq`(293) | BLOCKED | `GLIBC_TUNABLES=glibc.pthread.rseq=0`으로 회피 |
| `io_uring_setup`(425) | BLOCKED | Node≥20/libuv≥1.45 생존에 슈퍼바이저 필수 확증 |
| `faccessat2`(439), `openat2`(437), `statx`(291) | allowed | 문헌 추정과 다름 — 아래 참조 |

### syscall 스윕 — 468개 중 **239개 차단**

`hung=0`, `not-implemented=10`, `skipped=18`(위험 목록). 생성된 테이블은 [`src/supervisor/alr_sigsys_table.h`](../../src/supervisor/alr_sigsys_table.h)에 반영 (기본값 `-ENOSYS` + 명시 항목 30개).

**문헌 추정과 어긋난 것들** (실측이 정본):

- `openat2`(437), `faccessat2`(439)가 **허용된다.** [§A6](../01-platform-facts.md)은 bionic allowlist 부재를 근거로 차단으로 추정했으나 이 기기(Android 16)에서는 통과한다. Android 16에서 allowlist가 넓어진 것으로 보인다. → **`openat2(RESOLVE_IN_ROOT)` fast path가 다시 후보가 된다** ([ADR 0003](../adr/0003-ld-preload-path-virtualization.md) 대안 (A)의 기각 사유가 이 기기에서는 성립하지 않음). Android 12~15 기기에서 재확인 필요.
- `setresuid`(147)와 `getres[ug]id`(148/150)는 **차단되지 않는다.** cred-drop 계열 중 143/144/145/146/149/151/152/159만 차단. 설계 문서의 표가 과대했다 — 허용된 syscall을 에뮬레이션해도 발동하지 않으므로 무해하지만, 테이블은 실측본을 쓴다.
- `clock_settime`(112), `clock_adjtime`(266), `settimeofday`(170), `adjtimex`(171), `swapon/off`(224/225), `acct`(89), `syslog`(116), `init_module`/`delete_module`(105/106)도 차단 — 리뷰에서 추가한 `-EPERM` 매핑이 맞았다.

### 미해결

- **`/dev/tty` 가 ENXIO** (`No such device or address`). `ssh -t`로도 동일. ssh 비대화형 실행의 부작용인지 기기 제약인지 미확정 — **실제 Termux 터미널 세션에서 재확인 필요** (`PENDING_DEVICE`). git 비밀번호 프롬프트, `sudo` 류가 `/dev/tty`를 쓰므로 중요하다.
- 16KB 페이지 기기, Android 12~15 기기, Snapdragon 기기에서의 재측정.

## 상태

- [x] 기기 확보, Termux targetSdk 28 설치
- [x] 측정 유효 컨텍스트 확립 (sshd 경유, `Seccomp=2` 확인)
- [x] `alr doctor` P1~P10 실행 — **READY, FATAL 0**
- [x] SIGSYS 에뮬레이션 테이블 확정 → `src/supervisor/alr_sigsys_table.h`
- [ ] `/dev/tty` 재확인 (대화형 세션)
- [ ] 참조 기기 #2 (Snapdragon) — 성능 측정용
- [ ] M1 공통 코어 구현 시작
