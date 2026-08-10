# ADR 0008 — 정적 링크 게스트 바이너리는 비목표, codex 는 G5 에서 뺀다

## Status

**Superseded in part (2026-08-04) — [§보론](#보론-2026-08-04--후킹하지-않고-동작시킨다) 을 먼저 읽을 것.**
원래 결정(2026-08-03, Accepted)의 전제는 옳다: 정적 바이너리를 **후킹하는 것은 불가능**하다. 틀린 것은 거기서 끌어낸 결론이다 — *후킹할 수 없으면 쓸모없다*. 후킹하지 않고도 동작시킬 수 있고, 실제로 동작한다.

## Context

[00-product.md §3](../00-product.md) 의 목표 **G5** 는 "`node`, `npm`, `codex` 가 실사용 가능" 이었고, 상태는 오래 **부분 달성**이었다. 이유는 하나다 — codex 배포 바이너리가 **정적 링크 musl**(269 MB, `ET_EXEC`, `INTERP` 없음, `NEEDED` 없음)이라 **`LD_PRELOAD` 가 원리적으로 닿지 않는다.** 동적 링커가 개입하지 않으므로 우리 `.so` 는 매핑조차 되지 않고, codex 의 모든 경로 연산은 rootfs 가 아니라 Android 파일시스템으로 간다. 시작할 때마다 나오는 `could not create PATH aliases: Read-only file system` 이 그 증거다.

같은 문서의 [RISKS §4](../RISKS.md) 는 **"정적 링크 게스트 바이너리"** 를 이미 **수용된 영구 한계**로 올려 두었다.

**두 문장은 양립할 수 없다.** 한쪽은 codex 를 달성해야 할 목표로 두고, 다른 쪽은 그것이 원리적으로 불가능하다고 인정한다. 그대로 두면 G5 는 영원히 "부분 달성" 이고, 그 미달성은 우리가 고칠 수 있는 무언가를 가리키는 것처럼 읽힌다.

## Decision

**정적 링크 게스트 바이너리는 지원 대상 워크로드가 아니다. codex 를 G5 에서 뺀다.**

- G5 는 **`node` / `npm`** 으로 정의되고, 그 기준으로 **달성**이다(`npm ci` 실측 proot-distro 대비 3.12×, [M12](../evidence/2026-08-03-m12-spawn-resolver.md)).
- `alr install --with codex` 는 **남긴다.** 설치는 되고 실행도 되며, 원하는 사람이 있다. 다만 **경로 가상화 없이 도는 것을 알고 쓰는 것**이고, `alr` 이 그렇게 말한다.

> **쓸 수 있는가? — 쓸 수 있다. 게스트 rootfs 를 보지 못할 뿐이다.** `MEASURED` 2026-08-03:
>
> | 실행 | 결과 |
> |---|---|
> | `alr run codex --version` | `WARNING: ... could not create PATH aliases: Read-only file system` |
> | `alr run -e HOME=$HOME/codexhome codex --version` | **경고 없음**, `.codex` 정상 생성 |
>
> codex 는 경로를 **Android 기준**으로 푼다. 기본 `HOME=/root` 는 Android 에 없으므로 실패하고, **Android 에서 보이는 경로**를 주면 정상 동작한다. 이것이 [RISKS R8](../RISKS.md) 도 함께 답했다 — `alr` 이 rootfs 안에 쓰던 config 는 codex 가 **볼 수 없는 위치**였다.
>
> **그런데 더 정확한 결론이 있다: codex 는 게스트 프로그램이 아니다.** 같은 바이너리를 **alr 없이 Termux 에서 직접** 돌리면 출력이 동일하다(실측). 즉 `alr run codex` 는 아무것도 더해 주지 않는다 — 경로 가상화가 안 붙으니 당연하다.
>
> 실제로 부딪히는 것:
>
> | | codex 가 보는 것 |
> |---|---|
> | `/bin/sh`, `/bin/ls` | **있음** (Android toybox) |
> | `/bin/bash`, `/usr/bin/env` | **없음** |
> | Ubuntu 게스트의 도구 전부 | **없음** |
>
> 즉 codex 가 스스로 돌리는 셸 명령은 Ubuntu 가 아니라 **Android 의 빈약한 도구 집합**을 만난다. 그래서 권장 사용법은 alr 을 거치는 것이 아니라 **Termux 경로를 주는 것**이다:
>
> ```bash
> PATH=$PREFIX/bin:/system/bin <R>/usr/local/bin/codex
> ```
>
> `--with codex` 는 남긴다 — 설치 편의는 여전히 있다. 다만 그것이 **게스트 안에서 돈다는 뜻은 아니다.**
- 수용 시험의 `ALR CODEX LINKAGE` 는 `KNOWN_FAIL` 이 아니라 **관찰 라인**이다. `KNOWN_FAIL` 은 "원하는데 안 된다" 를 뜻하는데, 이제 원하지 않는다.

## Rationale

**왜 고칠 수 없는가.** [ADR 0006](0006-raw-syscall-binaries.md) 이 raw `svc` 바이너리에 대해 답한 것과 같은 계열이지만 기제가 다르다 — raw `svc` 는 libc 를 **우회**하고, 정적 링크는 libc 를 **가져가 버린다.** 후자는 더 근본적이다: 후킹할 동적 심볼이 존재하지 않는다. ADR 0006 이 검토한 seccomp user notification 이 이론상 유일한 길인데, 실측 **154 µs/syscall** 로 이 제품의 존재 이유를 지운다.

**왜 `--with codex` 를 남기는가.** 설치·실행 자체는 동작하고, 경로 가상화가 필요 없는 사용(예: 인증, 순수 네트워크 작업)이 있을 수 있다. 지원하지 않는 것과 막는 것은 다르다 — [ADR 0007](0007-android-16-only.md) 이 범위 밖 Android 릴리스에 대해 내린 것과 같은 판단이다: **말하되 막지 않는다.**

**왜 목표에서 빼는 것이 정직한가.** 목표는 우리가 달성할 수 있고 달성하려는 것의 목록이다. 원리적으로 불가능한 항목을 남겨 두면 목록이 "안 된 일" 과 "안 할 일" 을 섞어 보여 준다 — [ADR 0007](0007-android-16-only.md) 이 Android 12~15 에 대해 정리한 것과 같은 이유다.

## Consequences

**1. G5 가 달성으로 바뀐다.** 근거는 이미 있다 — node/npm 은 동적 링크라 경로 가상화가 적용되고 `npm ci` 가 실측되어 있다.

**2. `alr` 이 정적 ELF 를 만나면 말한다.** 지금까지는 `alr_classify` 가 `ALR_EXE_ELF_STATIC`(주석: `no PT_INTERP -> unhookable`)을 알고도 기본 verbosity 에서 아무 말도 하지 않았다. 이제 한 줄 경고를 낸다 — 사용자가 "게스트 안에서 도는 중" 이라고 오해할 수 있는 유일한 순간이 그때이기 때문이다.

**3. `docs/00-product.md` §5 비목표에 항목이 생긴다**: 정적 링크 게스트 바이너리.

**4. `KNOWN_FAIL` 이 2개에서 1개로 준다.** 남는 것은 `/dev/full` 뿐이고, 그것도 의도된 비목표다. 즉 **수용 시험의 `KNOWN_FAIL` 은 이제 전부 "안 할 일"** 이며, "못 한 일" 은 0이다.

## Alternatives considered

**(A) codex 를 G5 에 남기고 영원히 부분 달성으로 둔다.** 기각 — 목표 표가 정보를 잃는다. 읽는 사람이 "언젠가 되겠지" 로 읽고, 실제로는 아무도 시도할 계획이 없다.

**(B) `--with codex` 를 삭제한다.** 기각 — 동작하는 것을 막는 것은 증거보다 강한 주장이다. 우리가 잰 것은 "경로 가상화가 안 붙는다" 이지 "쓸모없다" 가 아니다.

**(C) seccomp user notification 으로 정적 바이너리를 후킹한다.** 기각 — [ADR 0006](0006-raw-syscall-binaries.md) 이 실측 154 µs/syscall 로 이미 답했다. `git status` 하나에 4.6초가 붙는다.

## 다시 볼 조건

- **codex 가 동적 링크 빌드를 배포하면** 자동으로 다시 후보가 된다. 수용 시험의 `ALR CODEX LINKAGE` 가 `NEEDED` 유무를 매번 확인하므로, 그날 라인이 바뀌어 알려 준다 — 가정이 아니라 계측으로.
- **arm64 SUD 를 지원하는 커널이 흔해지면** ADR 0006 과 함께 다시 본다.


## 보론 (2026-08-04) — 후킹하지 않고 동작시킨다

이 ADR 은 *"`LD_PRELOAD` 가 닿지 않는다 → 경로가 Android 로 간다 → 못 쓴다"* 로 끝났다. 두 번째 화살표가 비약이었다.

**경로를 재작성할 수 없다면, 재작성이 필요 없게 만들면 된다.** 정적 바이너리의 경로는 실제 루트에서 풀린다. 그러니 게스트 형식이 아니라 **호스트 형식 환경**을 주면, 재작성 없이도 정확히 같은 파일에 도달한다. 동적 바이너리에 게스트 경로를 주는 것의 **정확한 쌍대**다.

`MEASURED` 2026-08-04, codex-cli 0.146.0 (정적 musl, 269 MB), SM-X236N:

| | 이전 (게스트 형식 env) | 이후 (호스트 형식 env) |
|---|---|---|
| 시작 | `could not create PATH aliases: Read-only file system` | 경고 없음 |
| `codex doctor` config | `model <default>` — 설정 파일을 못 읽음 | `config loaded`, `config.toml parse ok` |
| git | `git not found` | **`git version 2.43.0`** (게스트의 것) |
| ripgrep | `search command could not be verified` | **`ripgrep 14.1.0 (system, rg)`** (게스트의 것) |
| 셸 명령 | 불가 | `bash -lc "pwd; cat /etc/os-release"` → `/root`, `Ubuntu 24.04.4 LTS` |

### 세 가지가 필요했고, 셋 다 실측으로 찾았다

**(1) `LD_PRELOAD`·`LD_LIBRARY_PATH` 를 주지 않는다.** 정적 바이너리에는 쓸모없을 뿐 아니라 **해롭다** — 그것이 띄우는 모든 자식이 상속하는데, 그 자식은 Android 바이너리이고 bionic 링커는 glibc 오브젝트를 거부한다:
```
CANNOT LINK EXECUTABLE "$PREFIX/bin/bash":
  library "libc.so.6" not found: needed by libalr_preload.so
```
**정적 게스트가 띄우려던 모든 프로세스가 main() 전에 죽고 있었다.**

**(2) 호스트 측 래퍼(`<R>/usr/lib/alr/hostbin/`).** 정적 바이너리는 게스트 프로그램을 exec 할 수 없다 — 게스트 바이너리의 `PT_INTERP` 는 Android 에 없는 로더를 가리키고, 그게 `ADR0002 BARE EXECVE FAILS` 가 재는 바로 그 사실이다. exec 할 수 **있는** 것은 Android 바이너리다. 그래서 각 래퍼는 인터프리터가 Termux 의 bash 인 스크립트이고, `alr run` 으로 다시 들어가 게스트 도구를 정상 경로로 띄운다.

**(3) 중첩 `alr run` 은 바깥 supervisor 를 물려받는다.** 프로세스에는 tracer 가 **하나**뿐이라 안쪽 alr 은 두 번째 supervisor 를 띄울 수 없다 — 그리고 띄울 필요도 없다. 바깥 supervisor 가 이미 자손 트리 전체의 SIGSYS 를 구제한다. 고치기 전 실측: `alr supervisor: pids=0 ... elapsed_ms=0`, `rc=126`. `already_traced()` 가 `/proc/self/status` 의 `TracerPid` 를 읽어 자동으로 판별한다.

### 무엇이 바뀌고 무엇이 그대로인가

**그대로**: 후킹은 여전히 불가능하다. codex 안에서 일어나는 경로 연산은 재작성되지 않으며, 앞으로도 그렇다. `ALR CODEX LINKAGE` 는 계속 그 사실을 감시한다. G5 의 정의도 그대로 `node`/`npm` 이다.

**바뀜**: *"정적 바이너리는 지원 대상 워크로드가 아니다"* 는 **너무 넓었다.** 정확한 문장은 **"정적 바이너리는 후킹 대상이 아니다"** 이고, 후킹되지 않는 채로도 유용하게 동작한다. `alr install --with codex` 는 이제 실제로 쓸 수 있는 codex 를 남긴다.

**여전히 안 되는 것**: codex 의 자체 샌드박스(`codex sandbox`)는 bubblewrap 을 요구하고 Android 에는 없다. `sandbox_mode = "danger-full-access"` 로 끄고 쓴다 — `alr` 은 애초에 보안 경계가 아니다([00 §1](../00-product.md)).
