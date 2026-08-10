# ADR 0003 — LD_PRELOAD 경로 가상화

## Status

Accepted (설계 단계, 2026-08-02).

## Context

게스트가 rootfs를 `/`로 봐야 한다. 방법은 세 가지뿐이다.

1. **커널 수준** — mount namespace + `pivot_root`, 또는 `chroot`
2. **ptrace** — 트레이서가 게스트 메모리의 경로 문자열을 재작성 (PRoot)
3. **libc 수준** — `LD_PRELOAD`로 경로를 받는 libc 함수를 감싸 in-process 재작성 (fakechroot)

## Decision

**3번. `LD_PRELOAD` 경로 가상화.**

## Rationale — 이건 선택이 아니라 유일한 선택지다

1번이 불가능함이 확정되었다 ([§B4](../01-platform-facts.md), `SOURCE`):
- `unshare(CLONE_NEWUSER)` → **`EINVAL`** (커널이 기능 없이 빌드됨. `EPERM`이 아니다)
- `mount(2)`, `chroot(2)` → seccomp `RET_TRAP` → **SIGSYS로 프로세스 사망**
- `unshare(CLONE_NEWNS)` → `CAP_SYS_ADMIN` 필요

2번은 우리가 이기려는 대상이다. PRoot는 path-bearing syscall마다 4회 컨텍스트 스위치 + 6~15회 ptrace/`process_vm_*` 연산 ≈ **5~20 µs**를 낸다.

**따라서 3번은 타협이 아니다.** 이것은 PRoot가 존재하는 이유이자 그것이 느린 이유이기도 하다 — 우리는 같은 문제를 더 싼 층에서 푼다.

## Consequences

### 성능 계약 — 이것을 어기면 프로젝트가 무의미해진다

| 측정 | 예산 |
|---|---|
| 절대경로 재작성 | ≤ **100 ns/op** |
| 상대경로 (재작성 불필요) | ≤ 20 ns/op |
| sysdir 통과 | ≤ 40 ns/op |

> ✅ **실측 정정** (2026-08-02): 10k 파일 `git status` 는 `rw()` 를 9,912회 부르지만 **재작성이 필요한 것은 26회뿐**이고 9,887회는 상대경로다. `p[0] != '/'` 를 첫 줄에 둔 설계가 전체의 99.7%를 3.9 ns 에 처리한다. 경로 계층 총비용 ≈ 40 µs — 아래 추정의 1/20. [증거](../evidence/2026-08-02-m7-m8-workloads-perf.md)

근거(원래 추정): `git status` 10k 파일 = 12,000~15,000회 재작성. 1 µs짜리 인터포저는 12~15 ms, 4 µs짜리는 50~60 ms를 먹는데 이는 **`git status` 이득 전체**와 맞먹는다 ([§D1](../01-platform-facts.md)).

경고: 상위 프로젝트의 in-process 변환기는 cold **4,334.7 ns/op**를 측정했다 — 예산의 40배다. 256엔트리 캐시로 상환했다지만 히트율이 load-bearing 미지수였다. **우리는 캐시를 쓰지 않고 예산을 지킨다.**

구현 규칙 ([04-preload-spec.md §5.1](../04-preload-spec.md)):
- `p[0] != '/'` 검사가 함수의 **첫 줄**. git은 `openat(dirfd, 상대경로)`를 점점 더 쓴다.
- **캐시 금지.** memcmp + memcpy보다 비싼 조회는 순손실.
- **네거티브 경로가 포지티브만큼 싸야 한다.** `.gitignore` 프로브가 디렉토리당 ~2회의 대부분 ENOENT인 open을 만든다.
- malloc 금지, 락 금지, syscall 금지.

### 알려진 구멍 — 정직하게 문서화한다

| 구멍 | 대응 |
|---|---|
| **ld.so 자신** (모든 프로세스 기동마다 도는 raw-syscall 컴포넌트) | 선언적으로 해결: `--library-path` + `--inhibit-cache` ([ADR 0002](0002-explicit-ldso-invocation.md)) |
| **libuv의 raw syscall** — Node의 모든 `fs.stat`이 재작성 없이 커널 직행 → `ENOENT` | `syscall()` 자체를 인터포즈해 path-bearing `__NR_*`의 인자 재작성 ([04-preload-spec.md §10](../04-preload-spec.md)). **성능이 아니라 정확성 문제다** |
| **정적 바이너리** | `LD_PRELOAD`가 원리적으로 불가능. `KNOWN_FAIL:unhooked-static-binary` |
| **Go 바이너리** (인라인 `svc`) | 인터포즈 불가. `alr doctor` P11이 rootfs를 스캔해 경고 |
| **NSS / `getaddrinfo`** (`/etc/{passwd,group,hosts,resolv.conf,nsswitch.conf}`) | 인터포즈로 못 푼다. 게스트 `/etc`가 glibc가 실제로 열 호스트 경로에서 도달 가능해야 한다 |
| **`realpath`의 내부 lstat 순회** | thread-local `g_rw_suppress`로 순회 동안 재작성 억제 후 결과에서 접두사 제거 ([04-preload-spec.md §5.3](../04-preload-spec.md)) |

측정으로 확인된 좋은 소식: **`git`/`node`/`bash`/`dpkg` 바이너리 본체는 raw syscall을 쓰지 않는다.** 핵심 베팅은 성립한다.

### 보안 아님

경로 재작성은 **방어 경계가 아니다.** `..` 클램프는 실수 방지이지 악의적 게스트로부터의 격리가 아니다. 게스트는 언제든 raw syscall로 호스트 파일시스템 전체에 접근할 수 있다. [00-product.md §5](../00-product.md)에 비목표로 명시했다.

## Alternatives considered

### (A) `openat2(RESOLVE_IN_ROOT)` — 커널 강제 격리

상위 프로젝트가 fast path에서 쓴다. 커널이 심링크 탈출을 막아 주므로 더 안전하고 빠르다.

**기각** — 단 **사유가 2026-08-03 에 교체되었다.**

> ⚠️ **원래 사유는 실측으로 반증됐다.** 여기 적혀 있던 것은 "`openat2`(437)가 bionic allowlist에 없어 **SIGSYS** 이므로 매 호출이 ptrace 왕복" 이었다. **틀렸다** — 참조 기기 2대 모두에서 `openat2` 와 `faccessat2` 는 **허용**이고, 437 은 양쪽 239개 차단 집합 어디에도 없다([§A6](../01-platform-facts.md), [브링업 §P4](../evidence/2026-08-02-device-bringup.md)).

**기각 사유(현행, [ADR 0007 §3](0007-android-16-only.md))**:

- **`RESOLVE_IN_ROOT` 가 이 설계와 충돌한다.** `alr_is_sysdir()` 는 `/proc`·`/sys`·`/dev` 를 **재작성하지 않고 호스트로 통과시킨다**([`alr_path_rule.h:47-52, :152`](../../src/common/alr_path_rule.h)) — `RESOLVE_IN_ROOT` 는 그것들을 rootfs 안에 가두므로 `realpath` 와 `/proc/self/fd/N` 복구가 깨진다. fast path 를 쓰려 해도 sysdir 판정이 먼저 돌아야 하므로 남는 절약은 접두사 `memcpy` 하나다.
- **덮는 면적이 좁다.** `openat2` 는 `open` 계열만 대체하는데 경로 가상화는 `stat`·`lstat`·`readlink`·`rename`·`opendir`·`exec` 등 **심볼 163개**에 걸쳐 있다. 채택해도 문자열 재작성은 그대로 남으므로 **경로가 하나로 줄지 않고 둘로 는다.**
- **성능 이득이 측정되지 않는다.** 재작성 총비용은 `git status` 10k 실측 **73.6 µs**, 예산 1.5 ms 의 1/20 이다([M19 §7](../evidence/2026-08-03-m19-snapdragon.md)).
- **안전성 이득은 비목표다.** `RESOLVE_IN_ROOT` 가 주는 것은 심링크 탈출 차단인데 [00-product.md §5](../00-product.md) 가 `alr` 은 보안 경계가 **아니다**라고 못박는다.

### (B) fakechroot 재사용

**기각**: Android/Termux bionic 포트가 없고, 우리가 필요한 것들(`/proc/self/exe` 가상화, exec 재디스패치, link2symlink, `syscall()` 인터포즈)이 전부 없다. 심볼 표는 참고 자료로 유용하다.
