# 09 — Codex 작업 플레이북

이 문서는 **구현 에이전트(Codex)를 위한 운영 규칙**이다. 매 세션 시작 시 이 문서와 [01-platform-facts.md](01-platform-facts.md)를 먼저 읽는다.

## 1. 세션 시작 체크리스트

```
1. docs/08-milestones.md 에서 현재 마일스톤 확인
2. 그 마일스톤의 Exit 조건을 읽는다
3. docs/01-platform-facts.md 에서 관련 § 를 읽는다
4. 해당 스펙 문서(03/04/05/06)의 관련 절을 읽는다
5. 작업 시작
```

**마일스톤을 건너뛰지 않는다.** M3이 통과하기 전에 M4 코드를 쓰면, 경로 재작성 버그와 부팅 버그가 섞여 원인 분리가 불가능해진다.

## 2. 절대 위반 금지 불변식

이것들은 취향이 아니라 **제품의 존재 근거**다. 어기면 프로젝트가 다른 제품이 된다.

### I1. `PTRACE_SYSCALL`을 절대 호출하지 않는다

```
검사: grep -rn "PTRACE_SYSCALL" src/   → 결과가 있으면 실패
게이트: supervisor.syscall_stops == 0
```

**왜**: 이걸 쓰는 순간 이 제품은 PRoot다. 성능 주장 전체가 무효화되고 존재 이유가 사라진다.

**"path syscall을 트랩하면 더 쉬울 것 같다"는 생각이 들면**: 그 문제는 preload에서 in-process로 풀어야 한다. 그게 안 되는 유형의 문제라면 (raw `svc`를 쓰는 Go 바이너리 등) **그건 지원 범위 밖**이지 슈퍼바이저를 바꿀 이유가 아니다.

### I2. 자체 seccomp 필터를 설치하지 않는다 (v1)

**왜**: [§A3](01-platform-facts.md) — 완화는 구조적으로 불가능하다(TRAP이 ERRNO를 이긴다).

**근거 정정 (2026-08-03).** 예전에 함께 적혀 있던 두 번째 이유 — "추가는 syscall당 ~24 ns를 더하니 얻을 게 없다" — 는 실측으로 무너졌다 ([ADR 0006](adr/0006-raw-syscall-binaries.md)).

- **필터 평가 비용은 관측되지 않았다.** 같은 프로세스·같은 syscall 로 재보니 베이스라인 438 ns, `RET_ALLOW` 필터를 얹은 뒤 266 ns — 주파수 스케일링 잡음 범위다. "syscall 마다 비용이 붙는다"로 결정을 정당화하면 안 된다.
- **"얻을 게 없다"도 틀렸다.** `SECCOMP_RET_USER_NOTIF`(0x7fc00000)는 zygote 의 `RET_ALLOW`(0x7fff0000)를 이긴다. 필터 설치가 `EPERM` 이던 것도 정책이 아니라 `no_new_privs=0` 때문이었고, 우리가 켜면 설치된다.

그래도 v1 이 필터를 설치하지 않는 진짜 이유는 **알림 왕복이 154 µs**(베이스라인의 352배)라는 것이다. 경로 syscall 을 그렇게 가로채면 proot-distro 보다 느려진다 — 그 순간 이 제품의 존재 이유가 사라진다. ADR 0006 은 정적/raw-syscall 바이너리에 **한정한** 선택 적용을 미구현 상태로 열어 두었다. 그 예외 밖에서는 이 불변식이 그대로다.

### I3. 경로 재작성 규칙은 `src/common/alr_path_rule.h` 한 곳에만 있다

```
검사: 재작성 로직이 두 곳에 있으면 실패
```

**왜**: 두 개의 재작성기가 어긋나면 증상이 "가끔 파일을 못 찾음"으로 나타나 추적이 극도로 어렵다.

### I4. preload는 malloc / stat 호출 / realpath 호출을 하지 않는다

**왜**: [04-preload-spec.md §2](04-preload-spec.md). 각각 게스트 allocator 초기화 전 호출, `.2.17` 링크 위반, 자기 재귀를 유발한다.

### I5. `mount()` / `chroot()`를 코드 어디에도 쓰지 않는다

데드코드, 에러 경로, 주석 처리된 코드 포함. **왜**: [§B4](01-platform-facts.md) — 부르면 SIGSYS로 프로세스가 죽는다.

### I6. `LD_PRELOAD`는 항상 절대 호스트 경로

**왜**: preload가 로드되기 전이라 게스트 경로는 재작성되지 않는다. 조용히 실패한다.

### I7. `LD_PRELOAD`를 지울 때는 항목을 제거한다. 빈 문자열을 넣지 않는다

**왜**: [§B7](01-platform-facts.md) — 빈 `LD_PRELOAD`는 bionic 링커에서 `CANNOT LINK EXECUTABLE`을 유발한다. no-op이 아니라 진짜 버그다.

### I8. 성능 주장은 MEASURED만

`relative_to_proot`는 실측 전까지 `PENDING_DEVICE`다. **속도 향상을 지어내지 않는다.**

이 문장은 규칙이라 그대로 둔다([07-acceptance.md §4.3](07-acceptance.md), [00-product.md §6](00-product.md)과 같은 문장이다). 다만 **A/B 는 이미 돌았다.** 이 칸을 채울 일이 생기면 새 숫자를 만들지 말고 아래를 인용한다:

| 워크로드 | alr | proot-distro | `relative_to_proot` |
|---|---|---|---|
| `git status` 10k 파일 (5회 중앙값) | 49 ms | 1,704 ms | **34.8×** |
| 기동 `/bin/true` (9회 중앙값) | 28 ms | 304 ms | **10.9×** |
| `npm ci` 105 패키지 (3회) | 2.00 s | 6.24–6.87 s | **3.12×** |

출처: [M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md), [M12 §4](evidence/2026-08-03-m12-spawn-resolver.md).

**세 줄 모두 참조 기기 #1(MediaTek MT8775) 단일 세션 값이다.** `git` 비교는 세 실행의 git 빌드가 다르고 proot 쪽 rootfs 도 달라 그만큼 약하다. `npm ci` 는 node 바이너리·락파일·캐시를 복사해 그 결함을 제거했지만 베이스 배포판이 다르다(alr 24.04 vs proot 26.04). 그래서 헤드라인은 M7/M8 이 정한 표현 — "동일 기기·동일 워크로드에서 `git status` 30배 이상, 기동 10배 이상, 단일 기기 1회 세션" — 을 넘지 않는다. 스냅드래곤 배수는 이제 있다 — `git status` 10k 25.8×, `node` 콜드 5.49× ([M19 §6](evidence/2026-08-03-m19-snapdragon.md)). **배수는 기기별이므로 기기를 함께 인용한다.**

### I9. permissive 디바이스의 측정은 무효

`getenforce != Enforcing` 또는 `Seccomp: != 2`면 결과에 `INVALID`를 붙인다. permissive에서는 zygote seccomp 필터가 아예 설치되지 않아 **모든 문제가 사라져 보인다.**

## 3. 막혔을 때

### 3.1 게스트가 부팅하지 않는다

```
1. ALR_LOG=2 로 실행
2. 슈퍼바이저 로그에서 "alr sigsys: nr=<n>" 을 찾는다
3. 그 nr이 alr_sigsys_table.h 에 있는가?
     없다 → 추가한다. 기본 -ENOSYS 로 시작.
     있다 → 반환값이 틀렸다. 0 과 -ENOSYS 를 바꿔 본다.
4. SIGSYS 로그가 아예 없다면 → 슈퍼바이저가 붙지 않은 것.
     go-pipe 순서와 PTRACE_SETOPTIONS 를 확인.
5. 그래도 안 되면 alr doctor --full 로 P2 스윕을 돌려 실제 차단 집합을 본다.
```

### 3.2 "파일을 못 찾음"이 산발적으로 난다

**먼저 `ALR_LOG=2`로 그 경로가 재작성 로그에 나타나는지 본다.** 나타나지 않으면 인터포즈 자체를 빠져나간 것(b~f), 나타나는데 결과가 틀리면 재작성 규칙 문제(a, g)다.

```
a) 재작성기 드리프트      → I3 위반. 규칙이 두 곳에 있는지 확인.
b) raw syscall 경로       → syscall() 인터포즈 표에 그 nr 이 빠졌다 (§10).
                             Node 의 fs.stat 이면 거의 항상 이것.
c) 경로 인자 비대칭       → symlinkat 은 인덱스 2 다 (0=target 재작성 금지,
                             1=newdirfd 는 정수). linkat/renameat/renameat2 는
                             인자가 둘이므로 mask 를 쓰지 않으면 하나만 재작성된다.
d) dlopen 절대 경로       → §6.13 누락. Node 네이티브 애드온, Python C 확장,
                             PAM/NSS 플러그인이 여기서 죽는다.
e) mkstemp 계열           → §6.14 누락. glibc 가 __gen_tempname 안에서 내부
                             hidden alias 를 호출해 open 래퍼로는 절대 안 잡힌다.
                             증상: apt/dpkg/gpg/git 이 작업 파일 생성에서 실패.
f) 정적 링크 / Go 바이너리 → 원리적으로 인터포즈 불가. alr doctor P11 확인.
g) 정규화 순서            → /proc/../etc/passwd 류가 sysdir 로 통과해 호스트
                             파일을 읽고 있다면 rw() 에서 정규화가 sysdir 검사보다
                             뒤에 있다. 이건 "못 찾음"이 아니라 "엉뚱한 걸 찾음"이라
                             더 위험하다.
h) ENAMETOOLONG           → ALR_PBUF 가 PATH_MAX 미만이면 정상 경로를 거부한다.
                             깊은 node_modules/dpkg 트리에서 나타난다.
```

### 3.3 Node가 이상하게 동작한다

```
process.execPath 가 ld.so 경로인가?   → /proc/self/exe 가상화 누락 (M4)
fs.stat 이 ENOENT 인가?              → syscall() 인터포즈 누락 (M5)
"Bad system call" 로 죽는가?          → io_uring. 슈퍼바이저 테이블 425/426/427 확인
npm 이 재spawn 에서 깨지는가?          → process.execPath 문제와 동일 원인
```

### 3.4 성능이 목표에 못 미친다

```
1. supervisor 통계의 syscall_stops 를 본다. 0 이 아니면 I1 위반.
2. sigsys 수를 본다. 프로세스당 8 을 넘으면 테이블에 빠진 항목이 있다.
3. rw() 마이크로벤치를 돌린다. 100 ns 를 넘으면:
     - 캐시를 넣었는가? 빼라.
     - p[0] != '/' 검사가 첫 줄인가?
     - memcmp 대신 strcmp/strncmp 를 쓰는가?
4. exec 처리량을 본다. LD_PRELOAD 의 per-exec 비용이 여기 나타난다 (§D2).
```

### 3.5 디바이스가 없다

M1까지는 디바이스 없이 전부 검증 가능하다. M2부터는 필요하다.

**디바이스 없이 할 수 있는 것**: `src/common/` 테스트, 경로 케이스 테이블 확장, `wrappers.def` 작성, 문서, `qemu-aarch64`로 크로스 빌드 오브젝트 실행.

**디바이스 없이 하면 안 되는 것**: SIGSYS 테이블 "완성" 선언, 성능 숫자, acceptance PASS 선언.

## 4. 온디바이스 개발 루프

`adb shell`은 Termux 앱 컨텍스트로 들어가지 못한다. 올바른 방법:

```bash
# Termux 쪽 (1회 설정)
pkg install openssh
passwd
sshd

# 개발 머신 쪽
adb forward tcp:8022 tcp:8022
ssh -p 8022 localhost
```

`scripts/dev-push.sh`가 이것을 감싼다: 빌드 → scp → 원격 실행 → acceptance 문자열 수집.

## 5. 커밋 규율

- **한 커밋 = 한 acceptance 문자열의 상태 변화** 가 이상적이다.
- 커밋 메시지 1행: `<마일스톤>: <무엇을 통과시켰나>` — 예: `M3: ALR BOOT /bin/true PASS`
- 실패를 통과로 만들지 않는 커밋(리팩터링/문서)은 `chore:` / `docs:`.
- **acceptance 문자열을 바꾸는 커밋은 07-acceptance.md 를 함께 고친다.** 문자열은 grep 대상이라 임의 변경이 집계를 깨뜨린다.

## 6. 새 플랫폼 사실을 발견했을 때

```
1. 1차 소스로 검증한다 (커널 소스 / glibc 소스 / bionic googlesource / man page)
2. docs/01-platform-facts.md 에 등급(SOURCE/FIELD/PENDING_DEVICE)과 함께 추가
3. 그 사실이 설계를 바꾸면 ADR 을 쓴다
4. 영향받는 스펙 문서를 갱신한다
```

**"해봤더니 되더라"는 SOURCE가 아니다.** 특히 permissive 디바이스나 한 대의 디바이스에서만 확인한 것은 `PENDING_DEVICE`다.

> **이 규칙은 지금 우리 자신에게 걸려 있다.** 2026-08-03 기준 이 저장소의 필드 증거는 **참조 기기 2대**(MediaTek 커널 6.1, Snapdragon 8 Elite 커널 6.6, 둘 다 Android 16)에서 나왔다 — MediaTek MT8775 / Android 16 / kernel 6.1.145-android14 / `getenforce=Enforcing` / `Seccomp: 2` ([브링업 기록](evidence/2026-08-02-device-bringup.md)).
>
> 규칙을 그대로 적용하면 셋으로 갈린다.
>
> - **부팅 여부와 execve 정책**은 `targetSdkVersion=28` 인 Termux 라는 조건에서 AOSP 정책이 정하는 것이라 기기를 넘어 유효하다고 볼 근거가 있다.
> - **차단 syscall 집합은 아니다.** allowlist 는 릴리스마다 자랐고(android12 365줄 → android16 392줄), 이 기기에서 이미 **AOSP 유래 예측이 반증되었다** — `openat2`(437)·`faccessat2`(439) 를 차단으로 예측했는데 실측은 허용이었다([브링업 §P4](evidence/2026-08-02-device-bringup.md)). §A6 는 이 예측 실패를 기록으로 남긴다. 그 항목 자체는 [ADR 0007](adr/0007-android-16-only.md) 로 닫혔지만, **AOSP 소스로 차단 여부를 추론하지 말 것** 이라는 교훈은 남는다.
> - **성능 배수**도 아니다 — 이 기기의 값일 뿐이고, ptrace 왕복이 호출당 ≈167 µs 로 [§D1](01-platform-facts.md)의 5~20 µs 모델보다 훨씬 비싼 것이 이 기기/커널 특성일 가능성이 남아 있다([M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)).
>
> 끝내는 데 필요한 측정은 **둘이었고 둘 다 했다.** 성능 쪽은 두 기기의 배수를 나란히 실었고([M19 §6.3](evidence/2026-08-03-m19-snapdragon.md)), 호환성 쪽은 `alr doctor` 스윕을 다시 돌려 diff 했다 — **diff 는 0이다.** 239개 집합이 완전히 같으므로 `src/supervisor/alr_sigsys_table.h` 는 상수(기본값)로 취급할 수 있다([§A6](01-platform-facts.md)).
>
> **기기 축은 닫혔다 — 측정이 아니라 결정으로.** [§A6](01-platform-facts.md) 가 요구한 둘 중 "다른 OEM/SoC 한 대"는 확보해 답을 냈고(239개 집합 동일), **Android 12~15 는 재지 않기로 했다**([ADR 0007](adr/0007-android-16-only.md)). **구버전 기기를 구하러 가지 말 것.** 새 기기를 얻는다면 Android 16 이어야 하고, 그때 할 일은 스윕 후 `scripts/diff-sweep.sh` 다.

## 7. 하지 말아야 할 유혹들

| 유혹 | 왜 안 되는가 |
|---|---|
| "ptrace로 path syscall도 잡으면 raw syscall 문제가 다 풀린다" | I1. 그 순간 PRoot가 된다 |
| "glibc를 조금만 패치하면 슈퍼바이저가 필요 없다" | 스톡 rootfs 호환성이 유일한 차별점이다 ([§00-product §4](00-product.md)). 그걸 버리면 grun과 같아지고, grun이 더 성숙하다 |
| "`rw()`에 LRU 캐시를 넣으면 빠를 것 같다" | memcmp가 이미 캐시 조회보다 싸다. 상위 프로젝트가 4,334 ns/op로 증명했다 |
| "user namespace를 한 번 더 시도해 보자" | `EINVAL`이다. 커널에 기능이 없다 ([§B4](01-platform-facts.md)) |
| "Play Store Termux도 지원하자" | glibc를 로드할 로더가 없다 ([ADR 0005](adr/0005-play-store-unsupported.md)) |
| "`/dev/full`을 `/dev/null`로 심링크하자" | ENOSPC 테스트가 조용히 통과해 버린다 |
| "정적 바이너리도 후킹하자" | LD_PRELOAD가 원리적으로 닿지 않는다 — codex 실측 확인(정적 musl, `alr preload:` 로그 0줄, [M12 §8](evidence/2026-08-03-m12-spawn-resolver.md)). `KNOWN_FAIL`로 분류하고 문서화한다. **단, 근거를 잘못 인용하지 말 것**: 가로채기 자체는 가능하다(seccomp USER_NOTIF). 비용이 154 µs/call 이라 안 하는 것이다 ([ADR 0006](adr/0006-raw-syscall-binaries.md)) |
| "Go 바이너리도 지원하자" | raw `svc`라 인터포즈 불가. `alr doctor` P11이 경고하는 것으로 충분하다. 26.04 의 uutils coreutils 도 같은 부류다(inline `svc` 74개 실측, [M15](evidence/2026-08-03-m15-cmdline-2604.md)) — 그래서 26.04 는 v1 대상이 아니다 |
| "acceptance를 SKIP으로 바꿔 넘어가자" | `SKIP`은 "이 환경에 해당 없음"이지 "아직 못 고침"이 아니다. 후자는 `KNOWN_FAIL:<reason>` |

## 8. 상위 프로젝트 참조 규칙

`android-on-linux` 저장소는 **설계 참고 자료**다. [02-architecture.md §7](02-architecture.md)에 무엇을 가져오고 무엇을 버리는지 파일 단위로 정리했다.

- 알고리즘과 교훈(버그 수정, 디바이스 증거)은 가져온다.
- **소스를 그대로 복사하지 않는다** — 라이선스 검토가 끝나기 전까지. 알고리즘을 이해하고 다시 구현한다.
- 상위 프로젝트가 Android W^X 때문에 만든 우회 장치(in-proc ELF 리맵, PC-gated seccomp, ptrace path 중재)는 **Termux에서 불필요하다.** 그걸 이식하려는 충동이 들면 [02-architecture.md §2](02-architecture.md)를 다시 읽는다.

## 9. 이 프로젝트가 성공했다고 말할 수 있는 조건

```
1. 스톡 Ubuntu 24.04 rootfs가 glibc 패치 0회로 부팅한다        (M3)
2. supervisor.syscall_stops == 0 이고 path_traps == 0 이다      (M8)
3. git status 10k 가 proot-distro 대비 1.5x 이상 빠르다 (MEASURED) (M8)
4. apt install / git clone / npm ci / codex 가 동작한다          (M6, M7)
5. 상위 M개 패키지 중 N개가 무수정 설치·실행 (숫자 공개)          (M9)
```

3번만 있고 1번이 없으면 grun의 열등한 복제품이다. 1번만 있고 3번이 없으면 proot-distro의 복잡한 복제품이다. **둘 다여야 한다.**

> **4번의 `codex` 를 과대 해석하지 않는다.** codex 릴리스는 정적 musl 링크라 `LD_PRELOAD` 가 로드조차 되지 않는다. 실행은 되지만 경로 가상화가 전혀 걸리지 않아 rootfs 가 아니라 Android 파일시스템을 본다 ([M12 §8](evidence/2026-08-03-m12-spawn-resolver.md)). `ALR CODEX VERSION: PASS` 는 "바이너리가 뜬다"까지이지 "게스트 안에서 동작한다"가 아니다. 4번을 다 채웠다고 말하려면 codex 가 동적 링크로 바뀌거나 ADR 0006 의 선택 적용이 구현되어야 한다.
