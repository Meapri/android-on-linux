# ADR 0002 — 명시적 ld.so 호출

## Status

Accepted (설계 단계, 2026-08-02).

## Context

Termux는 앱 데이터 경로의 execve를 허용하므로([§B1](../01-platform-facts.md)) 게스트 바이너리를 그냥 `execve`하면 될 것처럼 보인다. 하지만 **안 된다.**

스톡 Ubuntu 바이너리의 `PT_INTERP`는 문자열 `/lib/ld-linux-aarch64.so.1`이고, **커널이 execve 시점에 실제 호스트 루트 기준으로 해석**한다 (`fs/binfmt_elf.c`). rootfs는 `$PREFIX/var/lib/alr/distros/...`에 있으므로 호스트 루트에 그 경로는 없다 → `ENOENT`.

커널은 rootfs를 모른다. 그리고 `mount`/`chroot`/user namespace가 전부 막혀 있어([§B4](../01-platform-facts.md)) 그 경로를 존재하게 만들 방법도 없다.

## Decision

**게스트 로더를 명시적으로 호출한다.**

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

그리고 **preload가 게스트 안의 모든 exec를 같은 형태로 재작성한다** ([04-preload-spec.md §9](../04-preload-spec.md)).

## Consequences

### 필수 세부사항 (하나라도 틀리면 조용히 깨진다)

| 항목 | 근거 |
|---|---|
| **프로그램 인자에 반드시 `/`가 있어야 한다** | `elf/dl-load.c:2017` `if (strchr(name,'/') == NULL)` — 슬래시 없는 이름은 `$PATH`가 아니라 **라이브러리 검색 경로**를 탄다. 절대 호스트 경로를 넘긴다 |
| **`--library-path`는 필수** | ld.so 자신의 라이브러리 탐색은 인터포즈 불가능한 raw syscall이다([§C6](../01-platform-facts.md)). 인터셉션이 아니라 **선언적으로** 푼다. `LD_LIBRARY_PATH`로 대체하면 안 된다 — 인터셉트 못 한 경로로 재exec하는 자식에게 상속되고 `AT_SECURE`에서 지워진다 |
| **`--inhibit-cache`는 방어용** | 지금은 `/system/etc/ld.so.cache`가 없어 무해히 실패하지만, 그 경로에 호스트 파일이 생기면 모든 라이브러리 해석이 조용히 오염된다 |
| **`--argv0`은 glibc 2.33+** | Ubuntu 24.04(2.39) OK. Ubuntu 20.04/Debian 11(2.31)에는 **없다** → argv[0]이 호스트 경로로 샌다. busybox류 multi-call 바이너리, bash의 로그인 셸 판정, argv[0]으로 자기를 재exec하는 프로그램이 깨진다. "아무 glibc rootfs나 지원"을 광고하려면 `ld.so --help`를 파싱해 지원 옵션을 캐시할 것 |
| **`LD_PRELOAD`는 절대 호스트 경로** | preload가 로드되기 전이라 게스트 경로는 재작성되지 않는다 |

### `/proc/self/exe` 누출 — 하드 요구사항

명시적 로더 호출에서 `AT_EXECFN`은 glibc가 복구해 준다(흔한 오해). 실제로 새는 것은 **`/proc/self/exe`**(ld.so를 가리킴)와 **`/proc/self/cmdline`**(트릭 전체 노출)이다.

Node는 `process.execPath`를 `uv_exepath`로 얻고, libuv 구현은 문자 그대로 `readlink("/proc/self/exe", ...)`다. 보정하지 않으면 `process.execPath`가 ld.so 경로가 되고, **`process.execPath`로 재spawn하는 npm/npx/corepack이 "Node 버그처럼 보이는" 방식으로 깨진다.**

→ preload가 반드시 가상화한다 ([04-preload-spec.md §7](../04-preload-spec.md)).

### 정적 바이너리

glibc ≥ 2.35의 ld.so는 정적 바이너리를 만나면 **스스로 `execve`한다** (거부가 아니다. 2.35 이전에는 크래시였다).

→ "정적이면 ld.so를 안 쓴다"는 분기를 **직접 만들지 말 것.** ld.so가 이미 처리하고, 직접 하면 동작이 갈린다.
→ 다만 **분류는 해야 한다.** 그 재exec는 `--preload` 없이 일어나므로 그 프로세스가 후킹되지 않는다. `KNOWN_FAIL:unhooked-static-binary`로 분류하고 로깅한다.

### 기타

- **유일한 하드 거부**: 게스트 프로그램의 `DT_SONAME`이 로더 자신의 SONAME과 같을 때. 프로세스가 맨 오류 메시지와 함께 죽고 유용한 종료 코드가 없다 → 런처에서 걸러낸다.
- **setuid는 조용히 드롭된다** (거부가 아님). `/data`가 `nosuid`라 어차피 무의미. `sudo`류는 동작하지 않으며 실패 모드가 혼란스러운 권한 오류다 → 문서화한다.
- **exec 비용이 네이티브보다 비싸진다.** LD_PRELOAD DSO가 execve마다 하나 더 매핑·재배치되고 전역 심볼 스코프가 커져 lazy PLT 해석이 느려진다([§D2](../01-platform-facts.md)). `npm ci` 라이프사이클 스크립트와 `./configure` 루프에서 오버헤드가 드러나는 곳이 여기다. `getpid` 처리량이 아니라 **exec 처리량**을 측정한다.
- **shebang을 직접 파싱해야 한다.** 커널 `binfmt_script`는 인터프리터를 호스트 루트 기준으로 찾는다 ([04-preload-spec.md §9.3](../04-preload-spec.md)).

## Alternatives considered

### (A) 모든 게스트 바이너리를 patchelf (grun 방식)

`patchelf --set-interpreter <R>/lib/ld-linux-aarch64.so.1 --set-rpath ...`

**기각**: 침습적이고 apt 업그레이드마다 다시 해야 한다. 새로 설치되는 패키지의 바이너리를 잡을 훅이 없다. "스톡 rootfs"라는 성질을 잃는다.

### (B) 호스트 루트에 `/lib/ld-linux-aarch64.so.1` 만들기

**기각**: 루트 없이 `/`에 쓸 수 없다.
