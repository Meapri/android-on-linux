# 2026-08-02 — M7/M8: 타깃 워크로드와 첫 성능 실측

기기: SM-X236N / Android 16 / `untrusted_app_27` / `Seccomp=2`.
⚠️ **SoC는 MediaTek MT8775다.** 호환성 결과는 SoC 무관하게 유효하지만 **성능 수치는 스냅드래곤 대표값이 아니다.**

## M7 — Node.js

`node v22.20.0` (nodejs.org LTS tarball, `alr` 호스트측 다운로드 → `<R>/opt/node`).
**apt의 18.19가 아니라 22를 고른 것이 중요하다** — libuv 1.45+라 `io_uring_setup`을 실제로 호출하므로 SIGSYS 구제가 시험된다.

```
node --version           v22.20.0
process.execPath         /opt/node/bin/node      ← 로더 경로 아님. /proc/self/exe 가상화 검증
fs.statSync              STAT_OK                 ← libuv raw syscall 인터포즈 검증
fs.readdirSync           DIR_OK
io_uring 생존            URING_OK                ← Node 22 가 SIGSYS 로 죽지 않음
child_process spawn      SPAWN_OK
npm --version            10.9.3
#!/usr/bin/env node      NODE_SHEBANG_OK
npm install is-odd       added 2 packages in 942ms   ← HTTPS + 리졸버 브리지
SIGSYS/실행              10
```

### 발견 — rootfs 안의 절대 심링크는 깨진다 ⚠️

`ln -sf /opt/node/bin/node <R>/usr/local/bin/node` 로 만든 심링크가 동작하지 않았다. 커널이 심링크 타깃을 **호스트 루트** 기준으로 해석하고, preload는 커널의 심링크 해석에 개입할 수 없기 때문이다.

Ubuntu 자신은 rootfs 안에서 거의 항상 상대 심링크를 쓴다(`/bin` → `usr/bin`, `/lib/ld-linux-aarch64.so.1` → `aarch64-linux-gnu/…`). **프로비저닝이 만드는 심링크도 반드시 상대여야 한다.**

깊이도 정확해야 한다: `/usr/local/bin/x` → 루트까지는 `../../../`이다. `../../`로 하면 `/usr/opt/...`가 되어 조용히 깨진다.

### 발견 — `alr`도 shebang을 분류해야 한다

`alr run /usr/local/bin/npm` 이 `file too short` 로 실패했다. preload의 `exec_dispatch`만 분류를 하고 **`alr` 자신은 항상 ELF 로 가정**해 ld.so 에 넘겼기 때문이다. npm/npx 를 비롯해 대부분의 언어 도구 진입점이 `#!` 스크립트이므로 이건 예외가 아니라 일반 경우다.

`alr.c`에 `shebang_resolve()`를 추가해 인터프리터를 게스트 네임스페이스에서 해석하고 재귀하도록 했다.

## M7 — Codex CLI

`codex-cli 0.146.0` (`codex-aarch64-unknown-linux-musl`, 101 MB 압축 / 257 MB 실행파일).

```
codex --version   codex-cli 0.146.0
codex --help      Codex CLI
```

샌드박스 플래그 확인 ([RISKS R8](../RISKS.md) 해소): `-s, --sandbox <SANDBOX_MODE>` 와 설정 키 `sandbox_permissions`가 존재한다. [05-provisioning-spec.md §5.2](../05-provisioning-spec.md)의 `sandbox_mode` 추정은 **CLI 플래그가 `--sandbox`** 라는 것으로 확정되었다. Landlock/bubblewrap 이 Android 앱 프로세스에서 동작하지 않으므로 이 값을 비활성 모드로 두어야 하며, 정확한 모드 이름은 실제 세션에서 확정한다.

> musl 정적 링크라 rootfs 의 glibc 에 의존하지 않는다. 원리적으로는 Termux 에서 직접 실행해도 되지만, 게스트 안에서 실행하면 git/node 와 같은 파일시스템 뷰를 공유한다는 이점이 있다.

## M8 — 첫 성능 실측

### `git status`, 10,000 파일 저장소

같은 저장소(`<R>/tmp/bench`, 100 디렉토리 × 100 파일), 5회 중 중앙값:

| 실행 | 시간 | 비고 |
|---|---|---|
| native git 2.55 (bionic, Termux) | **44 ms** | 호스트 파일시스템 직접 |
| alr git 2.43 (Ubuntu glibc) | **56 ms** | **+27%** |

`supervisor: pids=21 sigsys=22 emulated=22 path_traps=0 syscall_stops=0`

### 기동 오버헤드

| | native | alr | 차이 |
|---|---|---|---|
| `/bin/true` (10회 중앙값) | 21 ms | 29 ms | **+8 ms** |

### 해석 — 정직하게

**+27%는 `alr` 오버헤드의 상한이지 순수 오버헤드가 아니다.** 두 실행은 서로 다른 git 빌드(2.55 vs 2.43), 다른 libc(bionic vs glibc), 다른 컴파일 옵션이다. 이 비교로는 런타임 비용과 바이너리 차이를 분리할 수 없다.

분리되는 것은 **기동 비용 +8 ms**다. 이건 명시적 ld.so 호출(argv 조립 + 분류를 위한 open/read), preload DSO 추가 매핑·재배치, 전역 심볼 스코프 확대, 그리고 `auditallow` 감사 레코드의 합이다.

`path_traps=0 syscall_stops=0` 은 유지된다 — **PRoot 와 갈리는 불변식은 실측에서도 성립한다.**

### 경로 재작성 호출 수 — `MEASURED` (모델을 20배 뒤집음) ⚠️

`strace` 대신 `rw()` 안에 카운터를 넣어 쟀다(`ALR_COUNT=1`). strace보다 정확하다 — 이 계층이 실제로 처리한 호출만, 상대경로 미스까지 구분해 센다.

`git status`, 10,000 파일:

```
alr rw: total=9912  rewritten=26  relative=9887  sysdir=1
```

**9,912회 중 재작성이 필요했던 것은 26회(0.26%)뿐이다. 9,887회(99.7%)는 상대경로라 첫 바이트 검사 하나로 통과했다.**

비용 재계산:

| | 호출 수 | ns/op | 합계 |
|---|---|---|---|
| 재작성 | 26 | 61.0 | 1.6 µs |
| 상대경로 미스 | 9,887 | 3.9 | 38.6 µs |
| sysdir | 1 | 13.8 | 0.01 µs |
| **총 경로 계층 비용** | | | **≈ 40 µs** |

**모델은 13,500회 재작성 × 61 ns = 0.82 ms 였다. 실측은 그보다 20배 싸다.**

두 가지가 확인된다:

1. **`p[0] != '/'` 를 함수 첫 줄에 두기로 한 판단이 정확히 들어맞았다.** git은 `openat(dirfd, 상대경로)`를 압도적으로 쓴다([§D1](../01-platform-facts.md)의 예측대로다). 이 한 줄이 전체 호출의 99.7%를 3.9 ns에 처리한다.
2. **위에서 잰 +27%는 경로 계층 때문이 아니다.** 56 ms 중 경로 재작성은 0.04 ms, 즉 **0.07%**다. 나머지는 프로세스 기동(+8 ms), 다른 git 빌드, 다른 libc다.

> 이 숫자는 `git status`에 대한 것이다. 워크로드마다 절대/상대 비율이 다르므로 다른 워크로드에 그대로 적용하지 말 것.

### PRoot A/B — ✅ `MEASURED`

`proot-distro` 는 **자체 rootfs 로는 정상 동작한다**(`PD_OK`). 앞서 실패한 것은 우리 rootfs 와의 조합이며 PRoot 자체 문제가 아니었다. 따라서 [00-product.md §4](../00-product.md)가 비교 대상으로 삼은 바로 그 proot-distro 로 3자 비교가 가능하다.

동일 워크로드(10,000 파일 `git status`), 각 5회 중앙값:

| 실행 | git | 시간 | native 대비 | **alr 대비** |
|---|---|---|---|---|
| native (Termux bionic) | 2.55 | **42 ms** | 1.00× | — |
| **alr** (Ubuntu glibc) | 2.43 | **49 ms** | 1.17× | 1.00× |
| **proot-distro** (Ubuntu) | 2.53 | **1,704 ms** | 40.6× | **34.8× 느림** |

기동(`/bin/true`), 9회 중앙값:

| | 시간 | alr 대비 |
|---|---|---|
| native | 24 ms | — |
| **alr** | 28 ms | 1.00× |
| **proot-distro** | 304 ms | **10.9× 느림** |

**`alr` 은 `git status` 에서 proot-distro 보다 34.8배, 프로세스 기동에서 10.9배 빠르다.**

#### 이 숫자를 어떻게 쓸 것인가 — 정직성 규칙

[00-product.md §4](../00-product.md)의 "방어 가능한 주장"은 `git status` 1.5~4× 였다. **실측은 그 상한을 크게 넘는다.** 그렇다고 34.8× 를 그대로 헤드라인으로 쓰면 안 된다:

1. **세 실행의 git 빌드가 다르다** (2.55 / 2.43 / 2.53). git 자체의 성능 차이가 섞여 있다.
2. **proot-distro 는 자체 rootfs 를 쓴다.** 파일 배치와 페이지 캐시 상태가 alr 쪽과 동일하지 않다.
3. **기기가 MediaTek MT8775 다.** 스냅드래곤 수치가 아니다.
4. **1회 측정 세션이다.** thermal 상태가 고정되지 않았다.

그러나 **34.8× 와 10.9× 는 위 요인들로 설명할 수 있는 크기를 훨씬 넘는다.** git 빌드 차이는 수 % 규모이고, 우리 자신의 native-대비 수치가 1.17× 인 것이 그 상한을 보여준다. 격차의 본체는 PRoot 의 path syscall 마다의 ptrace 왕복이다.

→ **권장 표현**: "동일 기기·동일 워크로드에서 proot-distro 대비 `git status` 30배 이상, 프로세스 기동 10배 이상. 단일 기기(MediaTek) 1회 세션 측정이며 git 빌드가 서로 다르다."
→ §4 의 1.5~4× 추정은 **과소평가였다.** 그 추정은 "PRoot 는 필터 테이블의 syscall 만 트랩한다"는 정정된 전제에서 나왔는데, 실제 격차는 그보다 훨씬 크다.

#### 왜 이렇게까지 차이가 나는가

`rw()` 카운터 실측(위)에 따르면 `git status` 는 경로 호출 9,912회를 낸다. proot 는 그 대부분에 ptrace 왕복을 내고, alr 은 **재작성 26회 + 상대경로 통과 9,887회 = 총 40 µs** 로 끝낸다. 1,704 ms − 49 ms = 1,655 ms 를 9,912 로 나누면 **호출당 ≈ 167 µs** 로, [§D1](../01-platform-facts.md)의 5~20 µs 모델보다도 크다. 이 기기/커널에서 ptrace 왕복이 특히 비싼 것으로 보이며, 별도 확인 가치가 있다.

### PRoot A/B — 이전 진단 기록

같은 rootfs 를 PRoot 에 물려 비교하려 했으나 Termux 의 `proot` 가 자체 로더 초기화에서 실패한다:

```
proot info: * the loader was not found or doesn't work.
fatal error: see `proot --help`.
```

추적 결과 근본 원인은 `$PREFIX/tmp` 부재가 아니었다. 디렉토리를 만들고(쓰기·chmod 모두 정상 확인) `PROOT_TMP_DIR`를 `$PREFIX/tmp`와 `$HOME/.prtmp` 양쪽으로 지정해도 동일하게 실패한다:

```
proot error: can't chmod '$PREFIX/tmp/proot-<pid>-XXXXXX': No such file or directory
```

proot가 방금 만든 임시 파일을 chmod하지 못한다. `-0`, `PROOT_NO_SECCOMP=1`, `PROOT_LOADER` 명시, rootfs 없는 최소 실행 모두 같다. 이 Termux/Android 16 조합의 proot 문제로 보이며(참고: proot-distro #567 "Android 16 이후 극심한 저하"), **alr 쪽 문제가 아니다.**

**따라서 [00-product.md §4](../00-product.md)의 proot 대비 배수는 여전히 `PENDING_DEVICE`다.** 위 숫자로 그 표를 채우면 안 된다 — 다른 비교다.

## 남은 것

- PRoot A/B (로더 문제 해결 후)
- 스냅드래곤 기기에서 재측정
- ~~재작성 호출 수 실측~~ ✅ 완료 — 위 참조. 모델 13,500 → 실측 26 (재작성) + 9,887 (상대경로)
- `os.cpus()` 가 0을 반환 — Node 가 읽는 `/proc`/`/sys` 경로 확인 필요
