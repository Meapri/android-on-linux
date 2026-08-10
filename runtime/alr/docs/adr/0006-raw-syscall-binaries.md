# ADR 0006 — raw-syscall 바이너리를 어떻게 할 것인가

- **상태**: 채택 (2026-08-03)
- **관련**: [ADR 0001](0001-signal-only-ptrace-supervisor.md), [00-product.md §5](../00-product.md), [M15](../evidence/2026-08-03-m15-cmdline-2604.md)

## 배경

`LD_PRELOAD` 인터포저는 **libc 를 거치는 호출만** 볼 수 있다. inline `svc` 로 커널에 직접 가는 바이너리는 경로 가상화가 닿지 않는다. 해당하는 것:

- Go 로 컴파일된 모든 바이너리 (처음부터 비목표로 문서화)
- **uutils coreutils** — Ubuntu 26.04 가 GNU coreutils 를 대체해 채택한 것. 실측 inline `svc` **74개** ([M15 §3](../evidence/2026-08-03-m15-cmdline-2604.md))

즉 26.04 에서는 `ls`·`cat`·`echo` 가 통째로 동작하지 않는다.

## 이전 판단이 틀렸다

이 프로젝트는 "seccomp user notification 은 zygote 필터가 자리를 차지해 불가능" 이라고 기록해 왔다. **틀렸다.**

두 가지를 잘못 알고 있었다.

**1. 액션 우선순위.** 스택된 seccomp 필터에서 커널은 **수치가 낮은** 액션을 택한다. `SECCOMP_RET_USER_NOTIF` 는 `0x7fc00000`, `SECCOMP_RET_ALLOW` 는 `0x7fff0000` 이므로, **zygote 가 ALLOW 하는 syscall 에 대해서는 우리 USER_NOTIF 가 이긴다.** 자리를 빼앗기지 않는다. (zygote 가 `RET_TRAP`(0x30000) 으로 막는 것은 여전히 TRAP 이 이긴다 — 그건 이미 SIGSYS 로 처리 중이다.)

**2. EPERM 의 원인.** 필터 설치가 `EPERM` 으로 실패한 것은 정책이 아니라 **`no_new_privs` 가 0이었기 때문**이다. Android 앱 프로세스는 그것을 켜지 않는다. 우리가 직접 켜면 된다:

```
status   : NoNewPrivs: 0  Seccomp: 2
set NNP  : ok
NOTIF    : LISTENER fd=3        ← 설치됨
FILTER   : INSTALLED
```

`tests/device/probe_intercept.c`.

## 그래서 쓸 수 있는가 — 비용을 쟀다

`bench/microbench/notif_cost.c`, 같은 프로세스·같은 syscall(`getppid`, 인자 없음 → 순수 가로채기 비용):

| | ns/call |
|---|---|
| 베이스라인 (필터 없음) | 438 |
| `RET_ALLOW` 필터 추가 | 266 |
| `USER_NOTIF` + supervisor 응답 | **154,270** |

두 가지가 동시에 읽힌다.

**필터 평가 자체는 공짜다.** ALLOW 필터를 얹어도 베이스라인과 차이가 없다(266 vs 438 은 주파수 스케일링 잡음 수준). 즉 **몇 개 syscall 만 notify 하고 나머지는 ALLOW 하는 필터는 나머지에 아무 비용도 물리지 않는다.**

**하지만 알림 왕복은 154 µs 다** — 베이스라인의 352배. 그리고 이건 supervisor 가 같은 프로세스의 **스레드**인 하한값이다. 실제 구현은 별도 프로세스여야 한다(자기 supervisor 와 데드락하지 않으려면).

비교: proot-distro 의 `git status`(10k 파일) 전체가 1,704 ms 였다. 경로 syscall 을 3만 번쯤 하는 워크로드에 154 µs 를 물리면 **가로채기만으로 4.6초** 다. PRoot 보다 느리다.

> 이 수치는 유휴 CPU 깨우기 지연에 지배될 가능성이 있다(폰의 idle exit + big.LITTLE 마이그레이션). 바쁜 supervisor 로 재보려 했으나 벤치마크가 걸려 측정하지 못했다 — **UNVERIFIED**. 다만 결론을 뒤집으려면 30배 이상 빨라져야 하는데, 그 정도는 기대하기 어렵다.

## 값싼 길은 이 커널에 없다

`PR_SET_SYSCALL_USER_DISPATCH` (SUD) 는 정확히 이 문제를 위한 기능이다. 지정한 PC 범위 **밖에서** 발행된 syscall 을 **같은 스레드에서** SIGSYS 로 잡는다 — tracer 도, 컨텍스트 스위치도 없다. 스레드별 바이트 하나로 켜고 끄므로 우리 자신의 libc 호출은 전속력을 유지한다.

```
SUD      : UNAVAILABLE (range=Invalid argument empty=Invalid argument)
kernel   : 6.1.145-android14
```

인자를 두 형태로 시도해 **인자 실수가 아님을 확인**했다. arm64 SUD 는 이 커널에 없다.

## 결정

**1. 기본 경로는 바꾸지 않는다.** `LD_PRELOAD` + signal-only supervisor 가 그대로 v1 이다. 실측 96/96 패키지가 이 경로로 동작하고, 경로 계층 총비용은 `git status` 49 ms 중 약 40 µs 다.

**2. raw-syscall 바이너리는 계속 비목표로 둔다.** 다만 근거를 정정한다 — "원리적으로 가로챌 수 없다" 가 아니라 **"가로챌 수는 있으나 그 비용이 이 프로젝트의 존재 이유를 지운다"** 이다.

**3. 선택적 적용은 열어 둔다 (미구현).** 필터 평가가 공짜라는 것이 설계 여지를 만든다:

- `alr` 은 이미 exec 시점에 바이너리를 분류한다(`alr_classify`). 여기에 inline `svc` 스캔을 더할 수 있다.
- `svc` 를 가진 바이너리에 대해서만, 그 자식 프로세스에 한해, **경로를 나르는 syscall 에만** USER_NOTIF 필터를 건다.
- 그 바이너리는 PRoot 급(혹은 그 이하) 속도로 **동작은 한다**. 나머지 시스템은 아무 영향도 받지 않는다.

이걸 하려면 먼저 답해야 할 것들이 있고, 전부 미검증이다:

- supervisor 가 죽으면 notify 대상 syscall 이 `ENOSYS` 로 실패한다. 필터는 제거할 수 없다 — 신뢰성 설계가 필요하다.
- `no_new_privs` 는 되돌릴 수 없고 setuid 의미론을 죽인다. 이미 비목표라 수용 가능하지만 게스트 의미론의 실제 변경이다.
- 경로 인자 재작성은 `SECCOMP_IOCTL_NOTIF_ADDFD` 로 fd 를 주입하거나 `/proc/pid/mem` 을 쓰는 방식이 필요하다. 후자는 TOCTOU 가 있는데, **alr 은 보안 경계가 아니므로**([00-product.md §5](../00-product.md)) 그 자체는 결격 사유가 아니다.
- 바쁜 supervisor 로 154 µs 가 얼마나 줄어드는지.

**4. 26.04 는 v1 대상이 아니다.** 설치는 되고 coreutils 를 제외한 나머지는 동작하며, `alr install` 이 그 사실을 경고한다.

## 추가 실측 — uutils 가 죽는 지점은 생각보다 앞이다 (2026-08-03, 참조 기기 #2)

`svc` 74개는 "경로 가상화가 닿지 않는다"는 이야기였다. 실제로 26.04 게스트에서 `ls` 를 돌리면 경로 문제까지 가지도 못하고 **디스패치에서 죽는다**:

```
$ alr -d ubuntu-26.04 run /bin/ls /
coreutils: unknown program 'ld-linux-aarch64.so'
```

uutils coreutils 는 멀티콜 바이너리라 자기 이름으로 어떤 applet 인지 정한다. 그 이름을 **`argv[0]` 에서 얻지 않는다.** `alr` 이 `--argv0` 을 제대로 넘기는 것은 확인했다:

```
argv[4]=--argv0
argv[5]=/bin/ls          <- 정확하다
argv[8]=<root>/bin/ls
```

그런데도 `ld-linux-aarch64.so` 라고 말한다 — `ld-linux-aarch64.so.1` 에서 확장자를 뗀 모양이다. 즉 applet 이름의 출처는 argv[0] 이 아니라 **실행 파일 자신의 정체**(`/proc/self/exe` 계열)이고, [ADR 0002](0002-explicit-ldso-invocation.md) 의 명시적 로더 호출은 그것을 필연적으로 **로더**로 만든다. preload 가 `/proc/self/exe` 를 합성해 두지만 uutils 는 raw `svc` 라 그 합성이 닿지 않는다.

대조군으로 같은 게스트의 `git`(평범한 동적 glibc 바이너리)은 정상이다 — `git --exec-path` 가 `/usr/lib/git-core` 를 올바로 답한다. 그리고 **`git status` 는 26.04 게스트에서 완전히 동작한다**([M19 §6](../evidence/2026-08-03-m19-snapdragon.md)). 깨지는 것은 coreutils 이지 26.04 전체가 아니다.

### 떠오르는 완화책 하나 — 그리고 그것을 검증하지 못한 이유

정체가 이름에서 온다면, **로더를 applet 이름의 심링크로 exec** 하면(`<farm>/ls` → `ld.so`) `/proc/self/exe` 의 basename 이 `ls` 가 되어 디스패치가 맞을 수 있다. 설치 때 심링크 팜을 만들고 `exec_build()` 가 로더 경로만 바꾸면 되는, 작은 변경이다.

**검증하지 못했다.** 이 실험은 `alr` 바깥에서 로더를 직접 띄워야 하는데, 그러면 슈퍼바이저가 없어 차단 syscall 에서 즉사한다:

```
$ <root>/lib/ld-linux-aarch64.so.1 --argv0 /bin/ls ... <root>/bin/ls /
Unknown signal 31        # SIGSYS — ADR 0001 이 말하는 그것
```

로더 이름을 바꾼 쪽도 **똑같이** SIGSYS 로 죽는다. 즉 A/B 가 성립하지 않는다. 이 가설을 재려면 `alr` 에 별칭 exec 경로를 먼저 넣어야 하고, 그것은 검증이 아니라 기능 추가다.

**그러므로 지금 확정된 것은 부정형뿐이다**: 원인은 `argv[0]` 이 아니다. 심링크 팜은 *유망한 미검증 가설*이며, 위 3번(선택적 USER_NOTIF)보다 훨씬 싸므로 26.04 를 다시 볼 때 **먼저** 시험할 후보다. 다만 이것이 통해도 고쳐지는 것은 **디스패치**뿐이고, `svc` 74개가 만드는 경로 가상화 부재는 그대로 남는다 — 즉 `ls /etc` 는 게스트가 아니라 Android 의 `/etc` 를 보게 된다. 완전한 해법이 아니다.

## 다시 볼 조건

- **arm64 SUD 를 지원하는 커널이 흔해지면** 이 결정을 뒤집어야 한다. SUD 는 프로세스 안에서 처리되므로 알림 왕복이 없고, raw-syscall 바이너리를 native 에 가까운 비용으로 지원할 수 있는 유일한 알려진 길이다.
  - **커널 6.6 에서도 아직 없다** — 참조 기기 #2(Snapdragon 8 Elite, `6.6.98-android15`)에서 `PR_SET_SYSCALL_USER_DISPATCH` 는 인자 두 형태 모두 `EINVAL` 이다([M19 §2](../evidence/2026-08-03-m19-snapdragon.md)). 이 ADR 의 근거는 6.1 과 6.6 양쪽에서 성립한다.
- Ubuntu 가 26.04 이후로도 uutils 를 유지하고 사용자가 최신 LTS 를 요구하면, 위 3번(선택적 적용)이 "느리지만 동작" 과 "아예 안 됨" 중 하나를 고르는 문제가 된다. 그 전에 심링크 팜 가설부터 시험한다.
