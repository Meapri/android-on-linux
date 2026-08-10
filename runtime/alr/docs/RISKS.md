# RISKS — 미해결 리스크 및 디바이스 검증 대기

> 이 문서는 **알려진 미지수의 등록부**다. 항목이 해결되면 [01-platform-facts.md](01-platform-facts.md)로 옮기고 여기서 지운다. 조용히 지우지 않는다.

## 1. 제품을 끝낼 수 있는 리스크 (Fatal)

### ~~R1. `getrandom` 또는 `memfd_create` 차단~~ — **해소됨** ✅

두 syscall 모두 glibc 폴백이 없어 차단되면 제품이 성립하지 않았다.

**2026-08-02 실측 (SM-X236N, Android 16, `untrusted_app_27`, `Seccomp=2`): 둘 다 `allowed`.**
근거: [evidence/2026-08-02-device-bringup.md](evidence/2026-08-02-device-bringup.md).

`alr doctor` P10이 계속 검사하며, 차단된 기기를 만나면 여전히 **크게 실패**한다. Fatal 등급에서는 내린다. (구버전 Android 재확인 조건은 [ADR 0007](adr/0007-android-16-only.md) 로 삭제됐다 — P10 런타임 검사는 그대로 남는다.)

### R2. targetSdk 28 앱의 설치 자체가 막힘 — **이미 현실화됨** ⚠️

두 개의 별개 위협이다. 하나는 아직 멀었고, **하나는 이미 도착했다.**

**R2a — 플랫폼 하드 블록 (장기, 수년)**
Android가 `MIN_INSTALLABLE_TARGET_SDK`를 29로 올리면 F-Droid Termux 설치가 불가능해지고 `untrusted_app_27` 도메인의 exec 허용도 사라진다. 릴리스당 +1씩(23→24) 올라왔으므로 아직 멀다. 이것이 제품을 끝내는 유일한 플랫폼 변화다.

**R2b — Google Play Protect 차단 (지금 일어남)** — `MEASURED`

2026-08-02, SM-X236N / Android 16(SDK 36)에서 `adb install`이 **Play Protect에 의해 차단**되었다. 대화상자 원문:

```
Google Play 프로텍트
안전하지 않은 앱 차단됨
Termux
이 앱은 Android 이전 버전에 맞게 개발되었으며 최신 개인 정보 보호 기능을 포함하지 않습니다.
[세부정보 더보기]  [확인]
```

`pm path com.termux`는 비어 있었고 `mCurrentFocus`는 `com.android.vending/...PlayProtectDialogsActivity`였다. 즉 **설치가 완료되지 않았다.**

성격:
- 플랫폼 차단이 아니라 **Play Protect(GMS) 차단**이다. 우회 가능하지만 **사용자가 기기에서 직접 눌러야 한다** (`세부정보 더보기` → `무시하고 설치`).
- 기본 대화상자에는 진행 버튼이 없다. `확인`은 설치를 **취소**한다.
- GMS가 있는 모든 소비자 기기에서 발생한다 → **모든 사용자가 이 마찰을 겪는다.**

**제품에 대한 함의 (설계가 아니라 배포 문제)**:
1. 설치 안내에 이 대화상자를 **스크린샷과 함께** 명시해야 한다. 예상 못 하면 "Termux가 안 깔린다"로 이탈한다.
2. 이것은 `alr`이 고칠 수 없다 — Termux 앱의 targetSdk 문제이고 상류에서만 해결된다.
3. 자동화(CI, 기기 팜)에서는 `adb install` 전에 Play Protect 검증을 꺼야 하는데, 이는 **보안 설정 변경**이라 사용자 동의 없이 해서는 안 된다.
4. R2a의 선행 지표가 하나 더 생겼다: Play Protect의 차단이 "경고 후 우회 가능"에서 "우회 불가"로 바뀌는 시점.

**해결**: 특정 Android 버전에 마이그레이션 계획을 걸지 않는다. 첫 실행 때 스크래치 ELF execve를 프로브하고(`alr doctor` P3) `EACCES`면 크게 실패한다. `MIN_INSTALLABLE_TARGET_SDK`와 Play Protect 정책 양쪽을 릴리스마다 추적한다.

### ~~R3. `rw()` 성능 예산 초과~~ — **해소됨** ✅ `MEASURED`

2026-08-02, SM-X236N 온디바이스 (`bench/microbench/rw_cost.c`, 2M iterations):

| 케이스 | ns/op | 예산 | bare syscall 대비 |
|---|---|---|---|
| abs hit (deep, 최악) | **61.0** | ≤100 | 0.26× |
| abs hit (typical) | 37.3 | — | 0.16× |
| rel miss (git 핫패스) | **3.9** | ≤20 | 0.02× |
| sysdir 통과 | **13.8** | ≤40 | 0.06× |
| 교정: bare `getppid` | 233.9 | — | 1.00× |

**상위 프로젝트의 변환기는 cold 4,334 ns/op였다 — 우리는 61 ns로 71배 싸다.** 캐시를 넣지 않은 것이 옳았다(캐시 조회가 memcmp보다 비쌌을 것이다).

`git status` 10k 파일 모델(13,500회 재작성): 재작성 총비용 **0.82 ms**. PRoot는 같은 호출들에 5~20 µs의 ptrace 왕복을 내므로 67~270 ms다. 성능 논지가 큰 여유를 두고 성립한다.

> **위 표의 `git status` 모델은 그 뒤 실측이 뒤집었다 (더 좋은 쪽으로).** 13,500이라는 재작성 횟수는 문헌 추정이었다. `rw()` 안에 카운터를 넣어 재보니 `total=9912 rewritten=26 relative=9887 sysdir=1` — **재작성이 필요한 호출은 26회(0.26%)뿐**이고 경로 계층 총비용은 0.82 ms가 아니라 **≈40 µs**, 즉 모델보다 20배 싸다 ([M8](evidence/2026-08-02-m7-m8-workloads-perf.md)). `p[0] != '/'`를 함수 첫 줄에 둔 판단이 전체 호출의 99.7%를 3.9 ns에 처리한다. 이 비율은 `git status`에 대한 것이며 워크로드마다 다르다.

## 2. 큰 재작업을 유발할 수 있는 리스크 (Major)

### ~~R15. 게스트 DNS가 동작하지 않는다~~ — **해결됨** ✅ (리졸버 브리지 구현)

아래는 진단 기록이다. 해결: `src/cli/alr_resolvd.c`(Termux측 bionic 리졸버 스레드) + preload의 `getaddrinfo`/`freeaddrinfo` 인터포즈. 검증: `apt-get update`가 35.2 MB를 받고 완주.

> ⚠️ **브리지에는 두 가지 함정이 있었고 둘 다 2026-08-04 에 고쳤다. 둘 다 `dig -v` 가 `free(): invalid pointer` 로 죽는 것으로 나타났다** — 이름 해석 문제처럼 보이지도 않는 증상이다.
>
> **(a) 브리지 소켓 주소가 재작성됐다.** `bind`/`connect` 인터포즈([§6.18](04-preload-spec.md))가 들어오자, preload 자신의 `rb_connect()` 가 부르는 `connect()` 도 우리 래퍼로 갔다. `ALR_RESOLV_SOCK` 은 Termux `$TMPDIR` 아래의 **호스트** 경로라 `rw()` 가 rootfs 안으로 접두사를 붙였고, 존재하지 않는 주소가 됐다. **일반화된 규칙: alr 자신의 경로는 절대 `rw()` 를 통과시키지 않는다.** 게스트는 그 경로를 이름 붙일 수도 없고 닿을 이유도 없다 — 우리만 쓰고, 우리는 이미 호스트 형식으로 들고 있다. 내부 호출자는 `real_*` 를 쓴다.
>
> **(b) 폴백 경로의 addrinfo 레이아웃이 우리 것이 아니었다.** 브리지가 없을 때(데몬이 안 뜬 기기, 구버전 alr) `getaddrinfo` 는 glibc 자체 리졸버로 폴백한다 — Private DNS 도 VPN 도 없는 기기에서는 그게 맞으므로 **지원되는 경로**다. 그런데 glibc 는 체인 전체를 **한 덩어리**로 할당해서 `ai_addr`·`ai_canonname` 이 같은 할당 **내부**를 가리키고, 우리 `freeaddrinfo()` 는 브리지 경로의 할당 방식대로 셋을 **따로** 해제한다. 그래서 호출자의 `freeaddrinfo()` 가 내부 포인터를 `free()` 했다. **데몬이 한 번 안 뜨면 게스트의 모든 이름 조회가 죽는 거리였다.** 이제 폴백 결과를 우리 레이아웃으로 깊은 복사하고 원본은 `real_freeaddrinfo` 로 해제한다 — **레이아웃의 주인은 하나다.**
>
> **수용 시험이 왜 못 봤나**: `RESOLV HOSTS/AHOSTS/REVERSE` 세 개가 전부 `/etc/hosts` 에서 답을 받아 브리지에 닿지 않고, `RESOLV LEGACY DNS` 는 네트워크가 없어 `SKIP` 이었다. 잡은 것은 `dig -v` 를 돌리는 폭 검사였다. 이제 `RESOLV BRIDGE ABSENT` 와 `ALR DIG VERSION{, NO BRIDGE}` 가 매번 돈다.

### R15 (원 진단). 게스트 DNS가 동작하지 않는다 — Private DNS / VPN 환경 — `MEASURED`

**2026-08-02 실측 (SM-X236N).** 게스트에서 이름 해석이 전부 실패한다:

```
게스트  getent hosts ports.ubuntu.com   → (빈 결과)
게스트  bash /dev/tcp/ports.ubuntu.com  → Temporary failure in name resolution
게스트  8.8.8.8:53 로 원시 DNS 질의     → 타임아웃 (응답 없음)
게스트  TCP 1.1.1.1:53 핸드셰이크       → OK       ← 네트워크 자체는 정상
Termux  ping ports.ubuntu.com           → 91.189.91.102  ← 호스트는 정상
```

**원인은 alr이 아니라 기기 네트워크 구성이다.** `dumpsys connectivity`:
- `UsePrivateDns: true`, `PrivateDnsServerName: 6cf6a6f2.d.adguard-dns.com` (DNS-over-TLS)
- VPN 인터페이스 `tun0` (`com.adguard.android`)가 사실상 모든 IPv4 경로를 잡고 있음

Android는 앱이 **netd를 통해** 해석하기를 기대한다. bionic의 `getaddrinfo`는 netd로 가므로 DoT/VPN을 존중하고 정상 동작한다. **glibc의 리졸버는 netd를 모르고 `/etc/resolv.conf`의 서버로 직접 UDP/TCP 53을 쏘는데, 그 경로가 막혀 있다.**

`nss_dns` 로드 카운트가 0인 것은 **정상**이다 — glibc 2.34+는 `files`/`dns` NSS를 libc에 내장해 dlopen하지 않는다. (초기 가설은 틀렸다.)

**영향 범위**: VPN이나 Private DNS를 쓰는 모든 사용자. 광고 차단기 사용자가 많으므로 드문 구성이 아니다. `apt update`, `git clone https://`, `npm install` 이 전부 막힌다 — **M6/M7의 하드 블로커**.

**해결책 후보**

| 안 | 내용 | 평가 |
|---|---|---|
| A. 사용자가 VPN/Private DNS를 끈다 | — | ❌ 사용자의 보안 설정이다. 제품이 요구할 수 없다 |
| B. `/etc/hosts` 정적 채움 | 필요한 호스트만 | ❌ 확장 불가 |
| C. `nameserver 127.0.0.1` + 로컬 프록시 | Termux에서 DNS 프록시 | ❌ 53 포트 바인딩에 `CAP_NET_BIND_SERVICE` 필요, 앱은 불가. `resolv.conf`는 포트 지정 불가 |
| **D. `getaddrinfo` 인터포즈 + 리졸버 브리지** | preload가 `getaddrinfo`/`gethostbyname`/`getnameinfo`를 가로채 Unix 소켓으로 Termux측 `alr-resolvd`에 위임. 그쪽은 **bionic `getaddrinfo`**를 쓰므로 netd·DoT·VPN을 그대로 존중 | ✅ **권장.** 유일하게 사용자 설정을 건드리지 않으면서 모든 환경에서 동작 |

**D안 주의점**: `getaddrinfo`는 `struct addrinfo` 링크드 리스트를 malloc으로 반환하므로 preload의 R1(no-malloc)이 이 경로에는 적용될 수 없다 — 핫패스가 아니므로 예외로 명시한다. `freeaddrinfo`도 함께 인터포즈해야 짝이 맞는다. `res_*` 계열을 직접 쓰는 프로그램(dig, nslookup)은 여전히 못 잡는다.

**우선순위**: M6 착수 전 필수. 현재 `apt update`가 이것 때문에 진행되지 않는다.

### ~~R4. SIGSYS ptrace 왕복 비용이 예상보다 큼~~ — **해소됨** ✅ `MEASURED`

설계는 "프로세스당 몇 번뿐"을 전제했다. 우려는 어떤 워크로드가 차단 syscall을 루프에서 부르면(예: 어떤 라이브러리가 `faccessat2`를 반복 호출) 비용이 누적된다는 것이었다.

**모든 실행이 내는 슈퍼바이저 요약 한 줄이 그 자체로 측정치다.** 실측:

| 워크로드 | 실측 | 프로세스당 |
|---|---|---|
| `/bin/true` ([M3](evidence/2026-08-02-m3-first-boot.md)) | `pids=1 sigsys=1 emulated=1` | 1.0 |
| exec 체인 ([M4/M5](evidence/2026-08-02-m4-m5-path-exec.md)) | `pids=2 sigsys=3 emulated=3` | 1.5 |
| `git status` 10k 파일 ([M8](evidence/2026-08-02-m7-m8-workloads-perf.md)) | `pids=21 sigsys=22 emulated=22` | **1.05** |
| SysV IPC 프로브 ([M16](evidence/2026-08-03-m16-ipc-audit.md)) | `sigsys=4` (`set_robust_list` 1 + IPC 3) | 4 † |

† M16은 `pids`를 적지 않았다. 이 프로브는 차단 syscall 세 개를 **일부러 밟는** 단일 프로그램이므로 워크로드가 아니라 인위적 최악 케이스다.

워크로드 행은 전부 `path_traps=0 syscall_stops=0` 이다 — PRoot와 갈리는 불변식이 실측에서 유지된다.

**우려했던 "루프 안의 차단 syscall"은 나타나지 않았다.** 21 프로세스짜리 워크로드가 22회다. 계수가 **syscall 수가 아니라 프로세스 수에 비례**한다: `set_robust_list`가 프로세스당 1회이고, 나머지는 그 프로세스가 실제로 한 번씩 시도한 것뿐이다(Node의 `io_uring_setup`, IPC 프로브의 3건). 관측된 최대는 Node 검증 스크립트 1회 실행의 **10건**이지만([M7](evidence/2026-08-02-m7-m8-workloads-perf.md)) 그 실행 자체가 여러 프로세스다.

**왕복 1회의 단가는 따로 재지 않았다 — 잴 필요가 없었다.** 이 트랩들은 이미 M8의 종단 수치(`git status` 49 ms, 기동 28 ms) **안에** 포함되어 있고, 계수가 O(프로세스)이므로 워크로드가 커져도 늘지 않는다. [ADR 0001](adr/0001-signal-only-ptrace-supervisor.md) 옵션 (C)(부팅 창에서만 ptrace 후 detach) 재검토는 불필요하다.

소프트 게이트 `sigsys_per_process <= 8`은 **회귀 감지용으로 남긴다.** 초과는 비용 문제가 아니라 에뮬레이션 테이블에 빠진 항목이 있다는 신호로 읽는다 ([09-codex-playbook.md](09-codex-playbook.md)).

### ~~R5. exec 오버헤드가 히어로 벤치를 잡아먹음~~ — **해소됨** ✅ `MEASURED`

`LD_PRELOAD` DSO가 execve마다 하나 더 매핑·재배치되고 전역 심볼 스코프가 커진다. 여기에 명시적 ld.so 호출(argv 조립 + 파일 분류를 위한 open/read)이 더해진다. **`npm ci`가 히어로 벤치인데 오버헤드가 드러나는 곳도 exec다** — 히어로 벤치가 자기 자신을 잡아먹을 수 있다는 것이 리스크였다.

기준은 미리 정해 두었다: **`npm ci` 비율이 1.5배 미만이면 히어로 벤치에서 내리고 `git status`만 남긴다.**

기동 3자 비교, `/bin/true` 9회 중앙값 ([M8](evidence/2026-08-02-m7-m8-workloads-perf.md)):

| | 시간 | alr 대비 |
|---|---|---|
| native (Termux bionic) | 24 ms | — |
| **alr** | **28 ms** | 1.00× |
| proot-distro | 304 ms | **10.9× 느림** |

exec당 절대 오버헤드는 **+4~8 ms**다(같은 문서의 두 측정: 21→29, 24→28). **이건 실재하고 사라지지 않는다.** 다만 proot는 같은 exec에 +280 ms를 낸다.

`npm ci` 105 패키지, **동일 조건 A/B** ([M12 §4](evidence/2026-08-03-m12-spawn-resolver.md) — node 바이너리·npm·락파일·npm 캐시를 복사해 양쪽을 같게 맞춰, M8의 "git 빌드 상이" 약점을 제거했다):

| | proot-distro | alr | 배수 |
|---|---|---|---|
| `npm ci` | 6.87 / 6.24 / 6.26 s | 2.00 / 2.00 / 1.99 s | **3.12×** |

**3.12×는 기준 1.5×의 두 배다 — `npm ci`는 히어로 벤치로 남는다.** exec 오버헤드가 실재함에도 그렇다. 프로세스를 대량 생성하는 워크로드일수록 proot의 기동 비용(10.9×)이 우리 것(+4~8 ms)보다 훨씬 빨리 누적되기 때문이다.

> M12가 적어 둔 caveat는 그대로 유효하다: proot 게스트가 26.04, alr 게스트가 24.04이고 단일 MediaTek 기기 1회 세션이다. 그건 배수의 **정밀도** 문제이지 R5(오버헤드가 벤치를 잡아먹는가)의 답을 바꾸지 않는다.

### R6. `auditallow` 로그 볼륨 — `PENDING_DEVICE` (귀속만 미해결, 위험은 해소)

`untrusted_app_27`은 `execute`와 `execute_no_trans` 양쪽에 `auditallow`가 걸려 있다. 게스트의 모든 execve와 모든 `.so` 매핑이 logd에 감사 레코드를 남긴다. Node 하나가 시작 시 `.so` ~40개 → 레코드 ~40개.

**이것이 PRoot 대비 이 설계의 지배적 숨은 비용일 수 있다.** PRoot는 게스트 바이너리를 직접 exec하지 않으므로 이 비용이 없다.

**2026-08-03 측정 시도 — 앱 프로세스 안에서는 잴 수 없다** ([M16 §2](evidence/2026-08-03-m16-ipc-audit.md)):

```
logcat -b events -d | wc -l   → 0     (에러 메시지조차 없음)
logcat -b main   -d | wc -l   → 8     ← 양성 대조: 계측기 자체는 동작한다
```

`events`만 비고 `main`은 내용이 있다. Android는 events 버퍼를 권한 있는 리더에게만 연다. **이 `0`은 "오버헤드 없음"이 아니라 권한 실패다.** 그대로 기록할 뻔했다.

**adb 로도 안 됐다** — 위 문단은 원래 "기기 밖 관찰자(adb)를 붙이면 된다" 로 끝나 있었다. 이 저장소의 하네스는 **이미 adb 를 쓴다**(`dev-push.sh` 의 `adb forward`). 바로 해 봤고, 버퍼별 양성 대조를 먼저 돌렸다:

```
adb shell logcat -b events -d | wc -l   → 18,552   (읽힌다)
adb shell logcat -b main   -d | wc -l   → 379,191  (읽힌다)
adb shell logcat -b kernel -d | wc -l   → 0        ← 안 읽힌다
adb shell logcat -b all    -d | grep -c avc → 0
```

**auditallow 레코드가 가는 곳은 커널 audit 서브시스템이고 `logcat -b kernel` 은 root 없이 열리지 않는다.** adb 는 앱 uid 를 넘겨 주지만 root 를 주지는 않는다. `selinux` 문자열 282건은 전부 `LocationAccessPolicy`·`thermal_core` 잡음이었다.

**그래서 이 위험은 다시 읽어야 한다.** R6 의 원문은 "지배적인 **숨은** 비용일 수 있다" 였다. 레코드를 세지 못하는 것과 별개로, **숨어 있을 수가 없다**: `git status` 49 ms, 기동 28 ms, `npm ci` 2.00 s, exec 351/s — 이 숫자들은 전부 auditallow 가 일어나는 채로 측정됐고 우리가 발표하는 모든 수치 안에 이미 들어 있다.

남는 것은 위험이 아니라 **귀속 질문**이다: 측정된 비용 중 몇 %가 감사 레코드인가.

**무엇이 이것을 끝내는가**: root 로 커널 audit 을 읽어 레코드를 세거나, `auditallow` 가 없는 정책의 기기와 A/B 한다. 둘 다 오늘 없다.

**그때까지의 규칙**: 오버헤드 수치를 낼 때 auditallow 를 "0" 이나 "무시 가능" 으로 적지 않는다. 반대로 **"측정되지 않은 추가 비용" 으로도 적지 않는다** — 이미 포함된 값이다.

> ⚠️ 상한을 잡으려고 네이티브 exec 처리량과 비교했다가 **무효 측정**을 만들었다(native 137 vs alr 354 exec/s). alr 쪽이 빠르게 나왔는데, 양쪽이 다른 셸(Termux mksh vs 게스트 dash)·다른 libc·다른 ABI 라 exec 비용이 아니라 셸의 fork 루프를 잰 것이다. **숫자가 유리하게 나왔을 때 특히 의심할 것.** 기록은 [M16 §2.3](evidence/2026-08-03-m16-ipc-audit.md).

### ~~R7. Codex의 `rustix` raw-syscall 백엔드~~ — **답 나옴, 그리고 더 나쁘다** ✅ `MEASURED`

질문은 "Codex가 `rustix`의 raw syscall 백엔드를 써서 경로 가상화를 우회하는가"였다. **답은 그 질문보다 앞에 있다: `LD_PRELOAD`가 애초에 로드되지 않는다.**

[M12 §8](evidence/2026-08-03-m12-spawn-resolver.md) 실측:

```
llvm-readelf -l  …/codex                                    → ET_EXEC, INTERP 프로그램 헤더 없음
llvm-readelf -d  …/codex | grep NEEDED                      → (없음)
ALR_LOG=2 alr run …/codex --version | grep -c 'alr preload:' → 0    (대조: git 은 1)
```

배포 바이너리가 **정적 링크 musl**(269 MB)이다. 동적 링커가 개입하지 않으므로 preload DSO는 매핑조차 되지 않는다. 백엔드가 `rustix`든 `libc` 크레이트든 **무관하다** — Codex의 모든 경로 연산이 rootfs가 아니라 Android 파일시스템으로 간다. 시작 시의 `could not create PATH aliases: Read-only file system`이 그 증거다.

따라서 `ALR CODEX VERSION: PASS`는 **바이너리가 실행된다**는 뜻이지 **게스트 안에서 동작한다**는 뜻이 아니다. 수용 테스트 `ALR CODEX LINKAGE`가 이 상태를 추적하며, 상류가 동적 빌드로 바꾸면 알아챈다.

원래 R7이 적어 둔 완화책("최악의 경우 Termux에서 직접 실행")은 **이미 사실상의 현재 상태다.** 게스트에서 띄우든 Termux에서 띄우든 Codex가 보는 파일시스템은 같다.

→ 아래 §4의 **"정적 링크 게스트 바이너리"** 및 바로 그 아래 codex 행으로 이관. `KNOWN_FAIL:unhooked-static-binary`.

### ~~R8. Codex 샌드박스 비활성화 키~~ — **해소** ✅ `MEASURED` (읽힐 수 없음이 확정)

**alr 이 실제로 하는 일** (`src/cli/alr.c` `with_codex()`, 2026-08-03 실측):
`<R>/root/.codex/config.toml` 에 228 바이트를 쓴다 — `sandbox_mode = "danger-full-access"` 와 주석 3줄. 파일은 정상적으로 생성된다.

**음성 대조 3종 — 전부 무반응** ([M18](evidence/2026-08-03-m18-codex-config.md)):

| 조작 | codex 반응 |
|---|---|
| 완전히 깨진 TOML (`=== not toml === [[[`) | `codex-cli 0.146.0` (동일) |
| 파일 삭제 | `codex-cli 0.146.0` (동일) |
| 존재하지 않는 모드 값 | `codex-cli 0.146.0` (동일) |

**그러나 이것은 "읽지 않는다" 의 증명이 아니다.** `codex --version` 이 애초에 config 를 파싱하지 않는 조기 종료 경로일 수 있다 — php 가 `-h` 는 되고 `--version` 은 죽었던 것과 같은 구조다. 진짜 config 를 소비하는 명령은 인증·네트워크를 요구해 이 하네스에서 돌릴 수 없다.

**정황은 강하다**: codex 는 정적 musl 이라 `LD_PRELOAD` 가 닿지 않고([R7](#r7), [M12 §8](evidence/2026-08-03-m12-spawn-resolver.md)), 따라서 `~/.codex` 를 **Android 루트 기준**으로 푼다. Android 에는 `/root` 가 아예 없다(`ls -ld /root` → No such file or directory). 시작할 때마다 내는 `could not create PATH aliases: Read-only file system` 이 같은 방향을 가리킨다.

**결과적으로 `alr` 의 출력 문구를 고쳤다.** 이전에는 `NOTE codex sandbox disabled` 라고 단정했는데, 그건 우리가 아는 것보다 많은 주장이다. 지금은 "파일을 썼고, codex 가 그것을 읽는지 확인할 수 없으며, 어느 쪽이든 게스트는 샌드박스되지 않은 것으로 취급하라" 고 말한다.

**끝났다 — 2026-08-03 실측.** 인증 세션도 `strace` 도 필요 없었다. `-e` 옵션이 생겨 `HOME` 을 바꿔 넣을 수 있게 되자 **차이가 바로 드러났다**:

| 실행 | 결과 |
|---|---|
| `alr run codex --version` (기본 `HOME=/root`) | `WARNING: ... could not create PATH aliases: Read-only file system` |
| `alr run -e HOME=$HOME/codexhome codex --version` | **경고 없음**, 그리고 그 디렉토리에 `.codex` 를 **생성** |

즉 codex 는 `~/.codex` 를 **Android 기준**으로 풀고, 기본 `HOME=/root` 에서는 Android 에 `/root` 가 없어 **디렉토리를 만들지도 못한다.** 따라서 `alr` 이 `<R>/root/.codex/config.toml` 에 쓰는 파일은 **codex 가 절대 볼 수 없는 위치**에 있다. "읽히는지 미확인" 이 아니라 **읽힐 수 없다.**

**결과**:
1. `alr` 의 NOTE 가 이 사실을 그대로 말하고, **동작하는 호출 방법**을 같이 알려 준다 — `alr run -e HOME=$HOME/codexhome /usr/local/bin/codex`.
2. 파일 쓰기 자체는 남긴다(무해하고 의도를 기록한다). 다만 그것이 codex 가 읽는 파일이 **아니라고** 명시한다.
3. codex 는 [ADR 0008](adr/0008-static-guest-binaries-non-goal.md) 로 비목표가 되었으므로, 이 항목은 목표에 대한 리스크가 아니라 **사용법 기록**이다.


### ~~R14. preload SIGSEGV~~ — **근본 원인 확정 및 수정** ✅

**원인: preload의 생성자는 먼저 실행되지 않는다.**

glibc `_dl_init()`은 `l_initfini`를 **내림차순**으로 순회하고, preload는 인덱스 1(메인 맵 바로 뒤)에 놓인다. 따라서 **libc와 다른 모든 DSO보다 늦게** 초기화된다. 그동안 73개 래퍼는 이미 인터포즈 중이므로, 다른 라이브러리의 생성자가 래핑된 심볼을 건드리면 아직 NULL인 `real_*`로 점프해 `main()` 전에 죽는다.

- `libselinux`의 `init_lib()` → `fopen("/proc/filesystems")`
- `libgcrypt`의 자체 무결성 검사 → `/proc/self/maps` open

**이것이 `cat`/`true`/`readlink`는 되고 `ls`/`find`/`apt-get`은 죽은 이유다.** 앞 그룹은 libc만 링크하는데, libc 자신의 초기화는 인터포즈 불가능한 내부 별칭(`__openat_nocancel`, `__fstatat64_time64`)을 쓴다.

> `__attribute__((constructor(101)))`은 **도움이 되지 않는다.** 생성자 우선순위는 **한 오브젝트 안의** `.init_array` 항목만 정렬하며, 이 `.so`에는 항목이 하나뿐이라 완전한 no-op다. [04-preload-spec.md §3](04-preload-spec.md)이 반대로 적었고 **그것이 틀렸다.**

**"미바인딩 심볼 0개"는 이 가설의 반증이 아니라 예측이었다.** `g_missing`은 dlsym이 *실패한* 심볼만 기록한다. 여기서의 NULL 창은 어휘적(심볼이 없음)이 아니라 **시간적**(호출이 생성자보다 먼저 도착)이다. 모든 심볼은 나중에 정상 해석된다.

**수정: 생성자 의존을 버리고 최초 사용 시 초기화(lazy init).** `ensure_init()`을 모든 래퍼 진입점(`rw()`, `NEED`, `NEEDN`, fakeroot 게터)에 넣고, `dlsym` 재진입은 thread-local `g_in_init`으로 막아 그동안은 패스스루로 동작시킨다. 생성자는 최적화로만 남긴다.

**NULL 가드만으로는 부족했던 이유**: 가드는 크래시를 막지만 그 창에서 `g_root_len`이 0이라 `alr_rw()`가 **패스스루 모드**였다. 초기 생성자 경로의 호출들이 rootfs가 아니라 **Android 네임스페이스**를 조용히 건드리고 있었다. 10/10 안정은 운이었다(프로브성 초기화 코드가 실패를 "기능 없음"으로 처리).

함께 수정한 것:
- `exec_path_search()`의 무가드 `real_access()` 호출 (파일 내 유일하게 남아 있던 것)
- `readlink()` 역매핑의 잠재 스택 오버플로: `tmp[ALR_PBUF]`가 호출자의 `sz`로만 경계 지어져 있었다

## 3. 관리 가능한 리스크 (Minor)

### ~~R9. SysV IPC 가용성~~ — **해소됨** ✅ `MEASURED` (막힌다)

질문은 "Android seccomp가 SysV IPC를 막는가, 막지 않는다면 업스트림 `fakeroot` 패키지를 그대로 쓸 수 있는가"였다.

**2026-08-03 실측** ([M16 §1](evidence/2026-08-03-m16-ipc-audit.md), `tests/device/probe_ipc.c`):

```
shmget : Function not implemented
semget : Function not implemented
msgget : Function not implemented
```

`EPERM`이 아니라 **`ENOSYS`**라는 점이 원인을 말해 준다. 같은 실행의 `ALR_LOG=2` 로그에 `alr sigsys` 4건 = `set_robust_list` 1 + IPC 3이 찍힌다. 즉 세 syscall 모두 zygote 필터에 `SECCOMP_RET_TRAP`으로 걸려 SIGSYS로 오고, 우리 슈퍼바이저가 기본값 `-ENOSYS`로 에뮬레이션한 것이다. **게스트 안에서 SysV IPC는 쓸 수 없다.**

**결론: 업스트림 `fakeroot`는 후보에서 탈락한다.** 그 구현은 `faked` 데몬 + SysV 메시지큐이고, 그 메시지큐가 이 플랫폼에 없다. [02-architecture.md §4](02-architecture.md)의 `mmap(MAP_SHARED)` 공유 DB shim은 "유지보수 부채를 감수한 선택"이 아니라 **유일한 선택지**였다. M6의 A/B는 실행할 A가 없어 결정 자체가 필요 없다.

게스트 일반에 대한 영향은 제한적이다 — 대상 워크로드 중 SysV IPC를 쓰는 것이 없고, 쓰는 소프트웨어(일부 DBMS, X11 MIT-SHM)는 애초에 비목표다. `ENOSYS`는 라이브러리가 폴백 경로를 타게 하는 표준 신호이기도 하다.

### R10. 하드링크가 일부 디바이스에서는 동작할 수 있음 — **해소** (범위 결정으로 닫힘)

**이 기기에서는 실패한다.** `alr doctor` P6이 같은 디렉토리 `link(2)`에 **`EACCES`**를 보고했다 ([device-bringup](evidence/2026-08-02-device-bringup.md)). rootfs를 앱 사설 저장소에 풀 때 tar가 하드링크 항목에서 실패하는 것과 정확히 일치한다 ([M3](evidence/2026-08-02-m3-first-boot.md)) — 그 경고를 흘려보내면 `perl5.38.2`와 `uncompress`가 없는 **조용히 깨진 rootfs**가 된다. 따라서 link2symlink 계층은 이 기기에서 **켜져 있고, 끌 수 없다.**

**두 번째 기기에서도 실패한다** — `MEASURED` 2026-08-03 ([M19](evidence/2026-08-03-m19-snapdragon.md)). Snapdragon 8 Elite / 커널 6.6.98 에서 `alr doctor` P6 이 **같은 `EACCES`** 를 낸다:

```
[P6 ] link(2) same-dir -> Permission denied (EACCES as documented)   -> MITIGATED
```

설치 로그도 같은 말을 한다 — `hardlink members: 0 linked, 2 copied, 0 failed`(24.04), `0 linked, 115 copied`(26.04). 링크가 되는 항목이 **한 건도 없다.**

**결과**: 벤더·커널·스토리지 스택이 갈려도 결과가 같으므로, 하드링크 폴백은 **끄지 않는다.**

**이 항목은 닫혔다.** 지원 대상은 Android 16 뿐이고([ADR 0007](adr/0007-android-16-only.md)), 그 위에서는 SoC 벤더·커널이 갈려도 `EACCES` 다(두 기기 다 Samsung 이므로 OEM 축은 미측정). 구버전 릴리스는 여전히 미지이지만 **범위 밖이므로 미해결이 아니다.** 하드링크 폴백은 **무조건 켜짐**이며, 이에 맞춰 [ADR 0004](adr/0004-link2symlink.md) 의 조건부 Status 도 무조건으로 개정했다.

> ⚠️ 여기 적혀 있던 런타임 스위치(`state/<name>/doctor.json` 의 `link2symlink`)는 **구현된 적이 없다** — 코드에서 그 파일을 읽거나 쓰는 곳이 0건이다. 없는 안전장치를 문서가 있다고 적고 있었고, 이제 필요도 없으므로 **구현하지 않고 내린다.**

### R11. phantom process 32개 한도 — `SOURCE`, 완화만 가능

Node + Codex + 언어 서버 + git 서브프로세스가 32에 도달할 수 있다.

**해결**: 자손 수 추적, ~24에서 경고, wake lock 권장, 자손이 상태 없이 사라지면 `reason=android-phantom-process-kill`로 분류. `device_config put activity_manager max_phantom_processes <N>`은 adb가 필요한 파워유저용 탈출구다.

### R12. `--argv0` 없는 구형 rootfs — `SOURCE`

Ubuntu 20.04 / Debian 11(glibc 2.31)에는 `--argv0`이 없어 argv[0]이 호스트 경로로 샌다.

**해결**: Ubuntu 24.04만 v1 타깃. 구형 지원을 추가하려면 `ld.so --help`를 파싱해 지원 옵션 집합을 캐시하고, `argv0_leaks=true`를 상태에 기록한다.

### ~~R13. PTY 슬레이브 ioctl 화이트리스트~~ — **해소됨** ✅ `MEASURED` (전제가 틀렸다)

이 항목은 "13개만 허용되고 **`FIONREAD`가 없는 것이 가장 아프다**(readline/ncurses가 쓴다)"고 적고 있었다. **그 전제가 실측으로 반박되었다** ([M14 §1](evidence/2026-08-03-m14-ioctl-php.md), `tests/device/probe_ioctl.c` — 게스트가 `/dev/ptmx`로 직접 연 페어에 대한 census):

| 허용 | 거부 (`EACCES`) |
|---|---|
| `TCGETS` `TCSETS` `TIOCGWINSZ` `TIOCSWINSZ` **`FIONREAD`** `TIOCOUTQ` | `TCGETS2` `TIOCGSID` `TIOCGETD` `TIOCEXCL` `TIOCSTI` |

**`FIONREAD`는 그냥 허용된다.** [04-preload-spec.md §11](04-preload-spec.md)이 요구하던 PTY 마스터 fd 에뮬레이션은 애초에 필요가 없었고, 바로 그 요구가 이 절 전체를 미구현으로 묶어 두고 있었다. 남은 4개는 국소 번역으로 끝났다(`TCGETS2`→`TCGETS` 등). `TIOCSTI`만 **의도적으로 계속 거부**한다 — `neverallowxperm`이라 절대 허용될 수 없고, 성공을 가장하면 주입된 입력이 조용히 사라진다.

회귀 테스트 `IOCTL PTY TRANSLATED` / `IOCTL TIOCSTI STAYS DENIED`가 지킨다.

## 4. 명시적으로 수용한 한계 (리스크 아님)

이것들은 해결하지 않기로 **결정한** 것이다. 다시 논의하지 않는다.

| 한계 | 근거 |
|---|---|
| Go 컴파일 바이너리 (`gh`, docker CLI, hugo 등) | 인라인 `svc`. `LD_PRELOAD`로 원리적 불가. `alr doctor` P11이 경고 |
| 하드링크의 `st_nlink` 정확성 | 복사 폴백 + inode 신원 테이블로 대체. 프로세스 단위·1024개 제한(링크당 2 엔트리). 프로세스를 넘는 nlink 검사는 진실을 본다 ([M6 증거](evidence/2026-08-02-m6-package-manager.md)) |
| `ld.so.cache` | 로더를 항상 `--inhibit-cache`로 호출하므로 소비자가 없다. `ldconfig`은 no-op으로 대체하고, **`dpkg-divert` 로 등록**해 libc-bin 업그레이드에도 살아남게 한다 (아래 주석) |
| 그룹 멤버 64명 초과 | NSS `files` 백엔드가 잘라내고 `ALR_LOG` 에 기록한다. 조용한 절단이 아니다 |
| `initgroups()` | 미구현. `setgroups` 가 seccomp 로 차단되어 보조 그룹 설정 자체가 불가능하다 |
| 로케일 아카이브 | `LOCPATH` 를 rootfs 로 지정하므로 glibc 가 `locale-archive` 를 건너뛴다. 추가 로케일은 `locale-gen --no-archive` 로 디렉토리 생성해야 한다 (아래 주석) |
| `/dev/full` | **영구 비목표.** 서빙하려면 프로세스에서 가장 뜨거운 syscall 인 `write()` 를 인터포즈해야 하는데, 대상 워크로드 중 이 디바이스 노드를 쓰는 것이 없다. 게다가 stdio 심볼(`puts`/`putchar`/`fwrite_unlocked`/`dprintf`/…)을 하나라도 빠뜨리면 **성공한 쓰기로 조용히 통과**한다 — 열거 가능하고 요란하게 실패하는 `mkstemp`(9개)·NSS(15개) 계열과 다르다. `KNOWN_FAIL:non-goal-devfull` |
| §11 `ioctl` 번역 | **구현됨** ([M14](evidence/2026-08-03-m14-ioctl-php.md)). 실측이 스펙 전제를 반박했다 — `FIONREAD` 는 네이티브로 허용되어 마스터 fd 에뮬레이션이 애초에 불필요했다. `TIOCSTI` 만 의도적으로 계속 거부(`neverallowxperm`) |
| `php-cli` abort 재발 가능성 | 출하 빌드에서는 동작하나 **원인 미규명**. preload 심볼 테이블 크기에 민감(경계 ~152). 심볼을 덜어내면 재발할 수 있고 범인은 그 변경이 아니다. `breadth.sh` 가 회귀를 잡지만 오귀속 주의 |
| Ubuntu 26.04 의 uutils coreutils | **비용 때문에 비목표** ([ADR 0006](adr/0006-raw-syscall-binaries.md) — 이전의 "원리적으로 불가" 는 정정되었다. seccomp user notification 은 동작하지만 왕복이 154 µs/syscall 이다). 26.04 는 GNU coreutils 를 uutils(Rust 멀티콜)로 교체했는데, 이 바이너리는 **inline `svc` 로 raw syscall 을 발행한다**(74개 확인. 일반 Rust 바이너리와 GNU coreutils 는 0개). `LD_PRELOAD` 는 libc 를 거치는 호출만 볼 수 있으므로 경로 가상화가 닿지 않고, 자기 이름을 `/proc/self/exe` raw syscall 로 읽어 로더를 본다. **위의 "Go 컴파일 바이너리" 와 동일한 한계이며 alr 의 결함이 아니다.** proot 는 syscall 마다 ptrace 를 걸어 이것을 잡지만, 그 비용을 내지 않는 것이 이 프로젝트의 존재 이유다([ADR 0001](adr/0001-signal-only-ptrace-supervisor.md)). 26.04 의 나머지(`bash`·`apt`·`dpkg`·`find`·`grep`·`sed`·`awk`·`tar`·`getent`)는 정상 동작하며, `alr install` 이 설치 시점에 이 사실을 경고한다 ([M15](evidence/2026-08-03-m15-cmdline-2604.md)) |
| `/proc/self/cmdline` | **합성됨** ([M15 §1](evidence/2026-08-03-m15-cmdline-2604.md)). 이전에는 로더 호출 전체와 모든 호스트 경로가 노출됐다. 단, 이것은 **자기 프로세스에 한한다** — `ps` 처럼 다른 pid 의 `/proc/<pid>/cmdline` 을 읽는 쪽은 여전히 로더 호출을 본다. 그 프로세스 밖에서는 고칠 수 없다 |
| `/proc/stat` 시간 필드 | Android 가 `/proc/stat` 을 앱 uid 에 `EACCES` 로 막는다. CPU **개수**는 `sched_getaffinity` 로 진짜 값을 얻어 합성하지만(`nproc`=8 과 일치), **시간 필드는 전부 0** 이다. 델타로 CPU 사용률을 계산하는 도구는 항상 0% 를 본다. `/proc/cpuinfo` 는 읽히므로 건드리지 않는다 |

> **`LOCPATH` 를 왜 쓰는가.** glibc 의 `_nl_find_locale` 은 리터럴 `/usr/lib/locale` 을 내부 호출로 열고, Android 호스트에는 그 디렉토리가 아예 없다. 그래서 게스트는 rootfs 가 `C.utf8` 을 배포하고 있는데도 `C`/`POSIX` 밖에 못 봤고, `tmux` 는 "need UTF-8 locale (LC_CTYPE) but have ANSI_X3.4-1968" 로 거부했다. `LOCPATH` 는 glibc 자신의 탈출구다. 대가는 `locale-archive` 를 못 읽는 것인데, 호스트에 로케일이 전무한 현 상태보다는 확실히 낫다.

> **`ldconfig` no-op 은 파일을 덮어쓰는 것만으로는 부족하다.** `libc-bin` 업그레이드가 진짜 래퍼를 복원하고, 그 래퍼가 정적 링크된 `ldconfig.real` 을 실행해 실패한다. 그 실패는 `libc-bin` 을 half-configured 로 남기므로 **이후의 모든 `apt install` 이 함께 죽는다** — 호환성 폭 측정에서 96개 패키지가 전부 "설치 불가" 로 보이는 형태로 드러났다. `alr` 이 매번 셰임을 다시 쓰는 것으로도 부족하다: 게스트 안에서 `alr` 을 거치지 않고 apt 를 돌리면 재발한다. 그래서 `alr install` 이 `dpkg-divert --local --rename --add /sbin/ldconfig` 를 등록한다. 이후 dpkg 는 새 버전을 `/sbin/ldconfig.distrib` 에 쓰고 우리 no-op 을 건드리지 않는다.
| 정적 링크 게스트 바이너리 | `LD_PRELOAD` 원리적 불가. `KNOWN_FAIL:unhooked-static-binary` |
| **`codex` 가 바로 그 사례다** | 배포 바이너리에 `INTERP` 도 `NEEDED` 도 없다(ET_EXEC, 269 MB). 실행은 되지만 **경로 가상화가 전혀 적용되지 않아** Android 파일시스템을 본다. 시작 시의 `could not create PATH aliases: Read-only file system` 이 그 증거다. `codex --version` 통과를 "게스트 안에서 동작함" 으로 읽으면 안 된다. 수용 테스트 `ALR CODEX LINKAGE` 가 추적한다 |
| Play Store Termux | [ADR 0005](adr/0005-play-store-unsupported.md) |
| setuid 의미론 / `sudo` | `/data`는 nosuid, setuid 계열은 seccomp 차단 |
| 보안 격리 | alr은 샌드박스가 아니다. 경로 재작성은 방어 경계가 아니다 |
| 커널 네임스페이스 | `unshare(CLONE_NEWUSER)` = `EINVAL`. 커널에 기능 없음 |
| GUI / GPU | 상위 프로젝트의 영역 |
| grun 대비 속도 우위 | 무승부에 가깝다. 차별점은 스톡 rootfs 호환성 |

## 5. 검증 우선순위

> **이 목록은 1번 기기에서 이미 전부 돌았다** — MediaTek MT8775 / Android 16 / 커널 `6.1.145-android14` / `untrusted_app_27` / `Seccomp=2`. 결과는 [device-bringup](evidence/2026-08-02-device-bringup.md)에 있다(P1~P10 `READY, FATAL 0`, syscall 468개 중 239개 차단). **2번 기기(Snapdragon 8 Elite)에서도 전부 돌았다** — [M19](evidence/2026-08-03-m19-snapdragon.md), 결과 동일. 따라서 아래는 **앞으로 얻을 기기에 대한 프로토콜**이다. R10 은 더 이상 이것을 기다리지 않는다([ADR 0007](adr/0007-android-16-only.md) 로 종결).
>
> 두 항목은 이 목록이 세워질 때와 의미가 달라졌다: **P9 `/dev/full`은 이제 영구 비목표**라(§4) 결과를 기록만 하고 대응하지 않는다. **P2는 SysV IPC까지 답했다**(R9).

디바이스를 확보하면 **이 순서로** 확인한다. 앞의 것이 실패하면 뒤는 의미가 없다.

```
1. P1  getenforce == Enforcing && Seccomp: 2     (모든 측정의 전제)
2. P3  $PREFIX 스크래치 ELF execve                (설계 전체의 전제)
3. P5  rootfs 파일 file-backed PROT_EXEC mmap     (ld.so 동작의 전제)
4. P10 getrandom / memfd_create                   (R1 — Fatal)
5. P2  syscall 0..460 스윕                        (에뮬레이션 테이블 확정)
6. P4  익명 mmap RW→RX                            (Node JIT)
7. P6  link(2)                                    (R10 — link2symlink 필요 여부)
8. P8  posix_openpt / grantpt                     (PTY)
9. P9  /dev/full                                  (에뮬 필요 여부)
10. P7 unshare(CLONE_NEWUSER)                     (EINVAL 확인)
11. P11 rootfs raw svc 스캔                       (Go 바이너리 목록)
```

**Android 16 기기를 새로 얻을 때마다** 돌린다 — 변하는 축은 벤더·커널이다([ADR 0007](adr/0007-android-16-only.md)). 2026-08-03 기준 2대 완료(MediaTek 커널 6.1 / Snapdragon 8 Elite 커널 6.6)이고 차단 집합 239개가 동일했다. 다른 릴리스는 범위 밖이라 이 프로토콜의 대상이 아니다.
