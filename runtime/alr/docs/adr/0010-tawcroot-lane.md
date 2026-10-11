# ADR 0010 — libc를 거치지 않는 게스트 바이너리는 tawcroot 레인으로 실행한다

## Status

Accepted (2026-10-11). [ADR 0006](0006-raw-syscall-binaries.md)의 결정(raw-syscall 바이너리는 비목표)과 [ADR 0008](0008-static-guest-binaries-non-goal.md)의 결정(정적 바이너리는 경로 가상화 없이 실행)을 대체한다. 상위 결정은 [ADR 0005](https://github.com/Meapri/android-on-linux/blob/main/docs/adr/0005-termux-first-goals.md) T5다. [VENDORING.md](../../third_party/tawcroot/VENDORING.md) 규칙 2가 요구하는 "새 ADR"이 이 문서다.

## Context

preload(`LD_PRELOAD`)는 libc를 거치는 호출만 본다. 두 부류의 게스트 바이너리는 libc를 거치지 않고 경로 syscall을 낸다.

- **정적 ELF**: ld.so를 거치지 않으므로 preload가 아예 로드되지 않는다. 대표 예는 codex(정적 musl), busybox-static, Go 툴체인이다.
- **동적이지만 libc를 우회하는 바이너리**: cgo로 빌드한 Go 프로그램이 대표적이다. Go 런타임은 파일 syscall을 직접 낸다.

지금까지 alr은 정적 자식에게 호스트 경로 인자를 주고, 경로 가상화 없이 실행했다(ADR 0008 보론). Go 툴체인은 이 방식으로 동작하지 않는다. `go build`는 `mkdir /tmp/go-build…: permission denied`로 실패한다([T5 uutils 증거](../evidence/2026-10-11-t5-uutils-preload.md) §남은 것).

[T5 스파이크](../evidence/2026-10-11-t5-coexistence-spike.md)가 확인한 사실은 다음과 같다.

- tawcroot(MIT, vendored)는 이 기기에서 빌드되고 동작한다. 프로세스 안의 `SECCOMP_RET_TRAP` 왕복은 0.5 µs다.
- alr의 슈퍼바이저와 공존할 수 있다. 다만 ptrace 아래에서는 트랩마다 약 60 µs가 들고, `find` 워크로드에서 40배 느려진다.
- tawcroot를 단독으로 실행하면 같은 `find`에서 네이티브 대비 2.2배다. preload는 1.0배다.

## Decision

1. **레인 선택**: alr은 다음 ELF를 tawcroot 레인으로 실행한다.
   - `PT_INTERP`가 없는 ELF(정적)
   - `PT_INTERP`가 있어도 `Go` 소유자의 ELF 노트가 있는 ELF(cgo Go)

   나머지 동적 ELF와 스크립트는 지금처럼 preload 레인으로 실행한다. 레인은 바이너리 단위로 정해진다.
2. **레인 바이너리**: `third_party/tawcroot`를 `alr-lane`이라는 별도 실행 파일로 빌드해 `alr`과 함께 배포한다. alr에 링크하지 않는다. 라이선스 고지는 함께 배포한다.
3. **추적 해제**: 슈퍼바이저는 추적 중인 프로세스가 `alr-lane`으로 exec하면 `PTRACE_DETACH`한다. 레인 안의 zygote 트랩은 tawcroot가 처리한다. 레인의 자손은 seccomp 필터를 상속하므로 계속 레인에 남는다.
4. **인자와 환경**: 레인으로 넘어가는 프로세스는 게스트 경로 인자를 그대로 받는다(호스트 경로로 바꾸지 않는다). alr의 loader 환경 변수(`LD_PRELOAD`, `LD_LIBRARY_PATH`, `ALR_*` 등)는 지운다. `/proc`, `/dev`, `/sys`는 바인드한다.
5. **끄는 방법**: `ALR_LANE=0`이면 이전 동작(정적 바이너리를 가상화 없이 실행)으로 돌아간다.
6. **한 rootfs, 두 레인**: 한 레인이 쓴 것을 다른 레인이 그대로 읽어야 하고, 프로세스가 레인을 옮겨도 신원이 바뀌면 안 된다. 그래서 alr-lane은 `-DTAWCROOT_ALR`로 빌드하고, 다음을 preload와 같게 맞춘다([VENDORING.md](../../third_party/tawcroot/VENDORING.md) §Local changes).
   - 신원은 실제 uid/gid이고 stat을 root 소유로 꾸미지 않는다. `ALR_FAKEROOT=1`이면 alr이 `--fakeroot`를 넘겨 upstream의 fake root로 돌아간다.
   - 절대 심링크 대상은 링크의 부모 깊이로 상대화해서 저장한다.
   - 하드링크가 거부되면 복사한다. upstream의 link store(`tawcroot:link:<token>` 심링크)는 쓰지 않는다. preload 레인에서는 그 심링크를 읽을 수 없기 때문이다.
7. **SIGSEGV와 SIGBUS는 alr-lane이 갖는다**: alr-lane은 게스트의 경로와 stat 버퍼를 `process_vm_readv` 없이 직접 읽고 쓴다. 잘못된 포인터가 낸 fault는 alr-lane이 받아 EFAULT로 바꾼다. 그래서 커널에는 두 시그널에 alr-lane의 진입점을 걸어 두고, 게스트의 `rt_sigaction`에는 게스트가 설정한 값을 그대로 보여 준다([VENDORING.md](../../third_party/tawcroot/VENDORING.md) 10).
   - 게스트 코드의 fault는 게스트 핸들러에 커널이 부른 것과 같은 인자, 스택, 마스크로 전달한다. `SIG_DFL`이면 프로세스가 원래처럼 그 시그널로 죽고, `SIG_IGN`이 exec를 넘어 상속되는 것도 같다. Go의 nil 역참조 `recover`와 `SetPanicOnFault`가 이 경로를 쓴다.
   - 게스트가 볼 수 있는 차이는 하나다. `/proc/<pid>/status`의 `SigCgt`에 두 시그널이 잡혀 있다. 이것을 없애려면 직접 읽기를 포기하거나 읽기마다 검증 syscall을 더해야 하고, 직접 읽기를 끈 빌드는 같은 측정에서 `find`·`ls -lR`이 1.38배였다(직접 읽기 1.20~1.21배)([비용 증거](../evidence/2026-10-11-t5-lane-cost.md) §5).

## 결과

- codex, Go 툴체인, busybox-static이 rootfs를 `/`로 본다.
- T2 워크로드는 레인에서 모두 1.2배 이내다(`go version` 1.04배, `go build` 0.98배). 경로 syscall만 반복하는 워크로드는 같은 busybox로 `ALR_LANE=0`과 비교해 `find`가 1.08배, `ls -lR`이 1.09배다([비용 증거](../evidence/2026-10-11-t5-lane-cost.md)). 통합 직후에는 1.34배와 1.65배였다([통합 증거](../evidence/2026-10-11-t5-lane.md) §5). 남은 비용은 트랩마다 드는 시그널 왕복(0.5~0.65 µs)이다. 레인의 자식(예: codex가 띄운 git)도 레인에 남는다. 그 git의 `git status`(1만 파일)는 preload 레인 대비 1.07배였다. preload 레인으로 돌려보내는 브로커는 이 ADR의 범위 밖이며, 필요하면 따로 결정한다.
- 레인 안의 신원은 preload 레인과 같다(결정 6).
- 레인의 정확성은 tawcroot에 달려 있다. tawcroot를 로컬에서 고치면 VENDORING.md에 기록한다.

## 다시 볼 조건

- 경로가 많은 실사용 워크로드가 레인에서 T2 예산(1.2배)을 넘을 때. 게스트 경로를 직접 읽는 단계(결정 7)는 끝났다. 남은 것은 시그널 왕복 자체이고, 그것을 없애려면 게스트의 `svc`를 바꿔 써야 한다.
- 게스트가 SIGSEGV 처리의 차이(결정 7)에 걸릴 때. 예를 들어 `SigCgt`를 검사하거나, alr-lane이 아직 시작 중일 때 비동기 SIGSEGV를 받는 프로그램이다.
- 레인 자식의 비용이 실사용에서 문제가 될 때. 그때는 자식을 preload 레인으로 돌려보내는 브로커를 검토한다.
- Android가 zygote 필터를 바꿔 tawcroot의 트랩 방식이 막힐 때.
