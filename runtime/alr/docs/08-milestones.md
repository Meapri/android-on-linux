# 08 — 구현 마일스톤 (Codex 작업 지시서)

> **읽는 법**: 마일스톤은 **순서대로** 한다. 각 마일스톤은 `Exit` 조건을 전부 만족해야 다음으로 간다. `Exit`가 `PENDING_DEVICE`인 항목이 있으면 그 마일스톤은 **미완**이며, 디바이스 없이 다음으로 넘어가는 것은 M5까지만 허용된다.

## 의존 그래프

```
M0 스캐폴딩
 └─ M1 공통 코어 (호스트에서 테스트 가능)          ← macOS/Linux에서 전부 검증 가능
     ├─ M2 슈퍼바이저                              ← 디바이스 필요
     └─ M3 첫 게스트 부팅  ← M2 필수                ← 여기가 진짜 첫 증명
         └─ M4 경로 가상화
             └─ M5 exec 연속성
                 ├─ M6 패키지 매니저 (fakeroot, link2symlink)
                 └─ M7 타깃 워크로드 (git/node/codex)
                     └─ M8 성능 + A/B
                         └─ M9 배포
```

---

> **진행 상황** (2026-08-03) — 아래 2026-08-02 기록은 **그대로 둔다**. 그날 참이었던 것이고, 뒤집힌 항목만 여기서 정정한다.
> - **M6 의 "결정 사항" 두 개가 모두 답을 얻었다.** SysV IPC 는 **막힌다** — `shmget`/`semget`/`msgget` 이
>   전부 `ENOSYS` 이고, `ALR_LOG=2` 가 슈퍼바이저의 SIGSYS 에뮬레이션 3건을 보여준다(즉 zygote 필터의
>   `RET_TRAP`) [M16 §1](evidence/2026-08-03-m16-ipc-audit.md). 따라서 **업스트림 `fakeroot`(SysV 변종)
>   분기는 닫히고 자체 shim 을 유지한다.** `link(2)` 는 `EACCES` 이므로 link2symlink 계층도 끄지 않는다
>   ([브링업 P6](evidence/2026-08-02-device-bringup.md)). 아래 §M6 참조.
> - **M4/M5 의 잔여 목록이 닫혔다.** `mkstemp` 계열·`dlopen` 은 M6 경로에서, 절대 심링크 상대화는
>   [M10](evidence/2026-08-02-m10-apt-install-git.md), exec 진입점 13개 중 없던 6개
>   (`posix_spawn` 계열·`system`/`popen`/`fexecve`/`execveat`)는
>   [M12 §1](evidence/2026-08-03-m12-spawn-resolver.md), `/proc/self/cmdline` 합성은
>   [M15 §1](evidence/2026-08-03-m15-cmdline-2604.md). 심볼 정본 `wrappers.def` 와 그것을 읽는 게이트는
>   [M13](evidence/2026-08-03-m13-symbol-gate.md) — **만들자마자 누락 심볼 24개가 나왔다.**
> - **M7 은 git/node 로는 통과, codex 때문에 미완이다.** `npm ci` A/B(동일 node 바이너리·락파일·캐시):
>   proot-distro **6.25 s** vs alr **2.00 s** = **3.12×** [M12 §4](evidence/2026-08-03-m12-spawn-resolver.md).
>   codex 는 **정적 링크 musl** 이라 `LD_PRELOAD` 가 로드조차 되지 않는다 →
>   `ALR CODEX LINKAGE: KNOWN_FAIL:static-unhooked`. 아래 §M7 이 이것을 자세히 적는다.
> - **M8 성능은 실측 완료.** `git status`(10k 파일) native 42 / alr 49 / proot-distro **1,704 ms** → **34.8×**,
>   기동 native 24 / alr 28 / proot **304 ms** → **10.9×**. 경로 계층 자체는 호출 9,912회 중 **재작성 26회,
>   합계 ≈ 40 µs** ([M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)). `path_traps=0 syscall_stops=0` 유지.
>   **하네스를 만들어 두 줄을 마저 실측했다** ([M17](evidence/2026-08-03-m17-bench-ab.md)):
>   `ALR BENCH NODE COLD vs PROOT` **6.60×**(alr 55 / proot 363 ms, 동일 node 바이너리),
>   `ALR BENCH EXEC THROUGHPUT` **351 exec/s**(proot 135). 막고 있던 것은 기기가 아니라
>   `alr bench` 부재였고, 문서가 그렇게 적어 두고 있었다.
>   **남은 것은 auditallow 볼륨(R6) 하나**이며 그것은 앱 프로세스에서 측정 자체가 불가능하다
>   ([M16 §2](evidence/2026-08-03-m16-ipc-audit.md) — 외부 adb 관찰자가 필요하다).
>   `bench/regression_gate.py` 도 아직 없으므로 **M8 의 산출물은 여전히 미완이다.**
> - **ioctl §11 은 전제가 반증된 채로 끝났다.** PTY ioctl 인구조사 실측: `TCGETS` `TCSETS` `TIOCGWINSZ`
>   `TIOCSWINSZ` **`FIONREAD`** `TIOCOUTQ` 는 그냥 허용된다 — §11 이 "가장 중요" 로 꼽아 에뮬레이션을
>   요구했던 `FIONREAD` 가 필요 없었다. 거부되던 `TCGETS2` `TIOCGSID` `TIOCGETD` `TIOCEXCL` 은 번역했고
>   `TIOCSTI` 는 의도대로 계속 거부한다 ([M14 §1](evidence/2026-08-03-m14-ioctl-php.md)).
> - **Ubuntu 26.04 는 v1 대상이 아니다 — 비목표.** `/proc/self/cmdline`·`--argv0` 수정 후 26.04 는 부팅하지만
>   **uutils coreutils 가 inline `svc` 74개**로 syscall 을 직접 내므로(정상 Rust 바이너리 0, GNU coreutils 0)
>   `ls`/`cat`/`echo` 가 통째로 안 된다 ([M15](evidence/2026-08-03-m15-cmdline-2604.md)).
>   가로챌 방법은 **있다** — seccomp user notification 은 `no_new_privs` 만 켜면 동작한다 — 그러나
>   가로채기 왕복이 **154 µs/syscall**(베이스라인 438 ns)이라 비용으로 기각했다. 필터 *평가* 자체는 공짜이고,
>   값싼 대안인 arm64 `PR_SET_SYSCALL_USER_DISPATCH` 는 이 커널(6.1.145-android14)에 없다.
>   [ADR 0006](adr/0006-raw-syscall-binaries.md).
> - **M10~M16 은 이 문서의 마일스톤이 아니다.** `docs/evidence/` 의 라운드 번호일 뿐이다 —
>   설치 재현성(M10), 호환성 폭(M11), spawn·리졸버(M12), 심볼 게이트(M13), ioctl·php(M14),
>   cmdline·26.04(M15), IPC·audit(M16). 여기 M0~M9 의 구조를 바꾸지 않고 증거로만 연결한다.
> - **다음: auditallow 볼륨을 외부 adb 관찰자로 측정 → codex 샌드박스 키 확정 → M9 배포 → 스냅드래곤 재측정.**
>
> **진행 상황** (2026-08-02)
> - **M-1 기기 브링업 — 완료.** [evidence/2026-08-02-device-bringup.md](evidence/2026-08-02-device-bringup.md). `alr doctor` VERDICT READY, FATAL 0.
> - **M0 스캐폴딩 — 완료.** `Makefile`, `scripts/dev-push.sh`, `scripts/dev-bootstrap.md`.
> - **M1 공통 코어 — 완료.** Exit 3개 PASS(+1 M2로 이월), **macOS와 기기 양쪽에서 동일 결과**.
> - **M2 슈퍼바이저 — 완료.** 기기에서 **12/12 PASS**. 누적 `sigsys_seen=16 emulated=15 passthrough=1`,
>   불변식 `syscall_stops=0 path_traps=0` 유지.
>   재현: `ALR_SSH_KEY=... ./scripts/dev-push.sh supervisor`
> - **M3 첫 게스트 부팅 — 완료.** [evidence/2026-08-02-m3-first-boot.md](evidence/2026-08-02-m3-first-boot.md).
>   **스톡 Ubuntu 24.04(glibc 2.39, 무패치)가 부팅.** `sigsys=1 path_traps=0 syscall_stops=0`.
>   ADR 0001/0002가 반증 테스트로 실증됨(슈퍼바이저 없이 exit 159=SIGSYS, ld.so 없이 exit 127=ENOENT).
> - **M4 경로 가상화 / M5 exec 연속성 — 핵심 경로 동작, 미완.**
>   [evidence/2026-08-02-m4-m5-path-exec.md](evidence/2026-08-02-m4-m5-path-exec.md).
>   게스트가 rootfs를 `/`로 보고 `apt-get`/`dpkg`가 응답한다. ABI 게이트(GLIBC_2.17 단독) 통과.
>   **`rw()` 성능 게이트 통과 (MEASURED)**: abs 61.0 ns / rel 3.9 ns / sysdir 13.8 ns.
>   상위 프로젝트 변환기(4,334 ns/op) 대비 71배 싸다 → [RISKS R3 해소](RISKS.md).
>   **완료 선언하지 않는다** — `mkstemp` 계열, link2symlink, `/proc/mounts` 가상화, `dlopen`,
>   `posix_spawn`/`system`/`popen` 이 남아 있다. 특히 앞의 둘 없이는 M6이 진행되지 않는다.
> - **M6 패키지 매니저 — 완료.** [evidence/2026-08-02-m6-package-manager.md](evidence/2026-08-02-m6-package-manager.md).
>   `apt-get install git` 성공, **git 2.43.0 동작** (init/commit/status/clone --local 전부 PASS).
>   온디바이스 acceptance **PASS=36 FAIL=0**.
> - **M7 부분 완료.** [evidence/2026-08-02-m7-m8-workloads-perf.md](evidence/2026-08-02-m7-m8-workloads-perf.md).
>   Node v22.20.0 + npm 10.9.3 동작 — `process.execPath`, libuv `fs.statSync`, **io_uring 생존**,
>   `npm install` 네트워크 성공. Codex 는 미완(GitHub API 응답 실패).
> - **M8 첫 실측.** `git status` 10k 파일: native 44 ms vs alr 56 ms (**+27%**), 기동 **+8 ms**.
>   `path_traps=0 syscall_stops=0` 유지. **PRoot A/B 는 `PENDING`** (Termux proot 로더 실패).
> - **PRoot A/B 완료 (MEASURED).** proot-distro 대비 `git status` **34.8×**, 기동 **10.9×** 빠르다.
>   [증거](evidence/2026-08-02-m7-m8-workloads-perf.md). §4 의 1.5–4× 추정은 과소평가였다.
> - **재현성 작업 진행 중.** [evidence/2026-08-02-m9-reproducibility.md](evidence/2026-08-02-m9-reproducibility.md).
>   `alr install --with node,codex` 동작, **`--with git` 미완** (apt 스테이징).
> - **다음: `--with git` → 호환성 폭 측정 → `/proc/mounts` → 배포 → 스냅드래곤.**
>
> <details><summary>이전 M6 진행 기록</summary>
>
> - **M6 진행 중.** `apt-get update` **완주**(35.2 MB), 의존성 해석 정상, `dpkg --unpack` 신규 설치 성공.
>   막힌 곳: **패키지 업그레이드 경로**의 dpkg 언팩. 정확한 좁힘은
>   [evidence/2026-08-02-m4-m5-path-exec.md](evidence/2026-08-02-m4-m5-path-exec.md) "미해결" 절.
>   구현 완료: 리졸버 브리지, `mkstemp` 계열, `statvfs` 계열, `dlopen`, fakeroot(신원만), link→copy 폴백.
> </details>

## M0 — 스캐폴딩

**목표**: 빌드가 돌고 CI가 초록.

**산출물**
- `Makefile` — 두 개의 타깃: `alr`(호스트/bionic), `libalr_preload.so`(게스트/glibc). **정정**: 별도 `libalr_fakeroot.so` 는 만들지 않는다 — fakeroot 신원 사칭은 `ALR_FAKEROOT=1` 뒤에서 preload 안에 구현되어 있다(`src/preload/alr_preload.c` §6.10). 릴리스 레이아웃이 영원히 존재하지 않을 파일을 약속해서는 안 된다
- `scripts/build-host.sh` — NDK 29, `--target=aarch64-linux-android24`, `-Wl,-z,max-page-size=16384`
- `scripts/build-preload.sh` — `zig cc --target=aarch64-linux-gnu.2.17`, 플래그는 [04-preload-spec.md §1](04-preload-spec.md) 그대로
- `scripts/test-native-core.sh` — macOS/Linux에서 `src/common/` 테스트
- `src/common/alr_sys.h` — API 24 위 syscall의 inline `syscall()` shim: `memfd_create`, `statx`, `renameat2`, `pidfd_open`, `openat2`, `faccessat2`, `seccomp`
- CI: 세 타깃 빌드 + 호스트 테스트 + `readelf -V` 게이트

**Exit**
```
ALR BUILD HOST:               PASS
ALR BUILD PRELOAD:            PASS
ALR PRELOAD GLIBC FLOOR 2.17: PASS
```

**주의**
- **NDK 버전과 zig 버전을 정확히 핀한다.** `zig version`이 다르면 빌드를 실패시킨다 — 재현성 주장은 고정 버전에서만 참이다.
- Termux 네이티브 clang을 릴리스 경로로 쓰지 말 것. 온디바이스 이너 루프 전용이다.

---

## M1 — 공통 코어

**목표**: 디바이스 없이 검증 가능한 순수 로직을 전부 끝낸다. **여기에 시간을 아끼지 말 것** — 이후 모든 디버깅이 여기 정확성에 의존한다.

**산출물**
- `src/common/alr_path_rule.h` — **Linux 헤더를 include하지 않는다.** 정규화 + guest→host 변환 + host→guest 역변환. `..`는 루트에서 클램프.
- ~~`src/common/alr_config.{h,c}` — `alr-config-v1` 탭 구분 + `%XX` 이스케이프 + FNV-1a 체크섬~~
  → **만들지 않았다. 비목표** (아래 Exit 주석)
- `src/common/alr_elf.{h,c}` — ELF64 aarch64 헤더 리더, `PT_INTERP` 추출, static/dynamic 분류
- `src/common/alr_exec_rule.{h,c}` — 순수 결정 커널: `decide_exec_path_mediation`, `decide_exec_envp_injection`, `path_under`, `colon_list_contains`, `split_env_entry`, shebang 파서
- `tests/cases/paths.tsv` — `(ALR_ROOT, input, expected)` 공유 테이블
- `tests/host/` — C 테스트 + Python 레퍼런스 모델. **둘 다 같은 tsv를 읽는다.**

**Exit** — ✅ 달성 (2026-08-02)
```
ALR PATH RULE HOST TESTS: PASS   (63 tsv cases, 73 assertions)
ALR ELF CLASSIFY:         PASS
ALR EXEC RULE TESTS:      PASS   (44 assertions)
ALR CONFIG ROUNDTRIP:     SKIP   — 비목표, 컴포넌트 자체가 없다 (아래 주석)
```

> `alr_config`(`alr-config-v1` 직렬화)는 **M2로 미뤘었다.** 상위 프로젝트는 execve를 넘어 상태를 넘기려고 이 포맷이 필요했지만, 이 설계는 env 변수(`ALR_ROOT`/`ALR_GUEST_EXE`/`LD_PRELOAD`)로 직접 넘긴다([02-architecture.md §6](02-architecture.md)). 슈퍼바이저가 체크섬 있는 핸드오프를 실제로 요구할 때 만든다 — 쓰이지 않을 포맷을 미리 구현하지 않는다.
>
> **정정 (2026-08-03)**: M2 는 기기에서 12/12 PASS 로 끝났고 그 포맷을 **한 번도 요구하지 않았다.** `src/common/` 에 `alr_config.{h,c}` 는 존재하지 않는다. 따라서 이 항목은 "M2 로 미룬 것"이 아니라 **필요해질 때까지 비목표**다 — 마일스톤을 붙잡아 두는 미완 항목으로 세지 않는다. 되살릴 조건은 위와 같다: env 변수로 못 넘기는 상태가 실제로 생길 때.

**양쪽 실행 결과가 동일해야 한다** (이것이 M1의 진짜 검증):
```
macOS  : make test
device : ALR_SSH_KEY=... ./scripts/dev-push.sh test
         → context OK: uid=10297 Seccomp=2 u:r:untrusted_app_27:s0
```
`dev-push.sh`는 `uid>=10000 ∧ Seccomp==2`가 아니면 **실행을 거부**한다.

**필수 테스트 케이스 (최소)**
절대경로 / 상대경로 / `/proc`·`/sys`·`/dev` 통과 / 이미 rootfs 아래(멱등) / `..` 클램프 / `.` 제거 / 중복 슬래시 / 후행 슬래시 / 루트 자체(`/`) / 빈 문자열 / 임베디드 NUL 거부 / `ALR_PBUF` 초과 / 컴포넌트 경계(`/proctology`는 `/proc` 아님) / 역변환 왕복

**주의**
- `alr_path_rule.h`가 Linux 헤더를 include하면 macOS 테스트가 죽고 개발 루프가 10배 느려진다. **이것이 M1의 핵심 제약이다.**
- 경로 규칙은 여기에만 존재한다. preload가 자기 복사본을 만들면 드리프트로 "가끔 파일을 못 찾음"이 발생한다.

---

## M2 — 슈퍼바이저 ⚠️ 디바이스 필요

**목표**: SIGSYS 구제가 실제 디바이스에서 동작함을 증명.

**선행**: 참조 디바이스 확보 + `alr doctor` P1/P2 실행. **`getenforce == Enforcing` 이고 `Seccomp: 2`가 아니면 이 마일스톤의 모든 결과가 무효다.**

**산출물**
- `src/supervisor/alr_supervisor.c` — [03-supervisor-spec.md](03-supervisor-spec.md) 전체
- `src/supervisor/alr_sigsys_table.h` — 에뮬레이션 테이블 ([§5](03-supervisor-spec.md))
- `src/cli/doctor.c` — P1, P2, P3, P4, P5, P7, P10
- `tests/device/supervisor_*.c` — 테스트 프로그램들

**Exit**: [07-acceptance.md §2 M2](07-acceptance.md)의 9개 전부 PASS

**주의**
- `PTRACE_TRACEME` + go-pipe 순서를 정확히 지킨다. `PTRACE_SEIZE`를 쓰면 경합으로 자식이 `set_robust_list`에서 죽는다.
- `regs[0]`을 반드시 쓴다 (진입 시 arg0이 들어 있다).
- `regs[32] = si_call_addr`를 매번 쓴다 (`-ERESTARTNOINTR` 무한루프 방지).
- `waitpid`에 `__WNOTHREAD`를 넣는다.
- `si_code == SYS_SECCOMP`을 검증한다 — 게스트 자신의 SIGSYS를 삼키면 안 된다.
- **`PTRACE_SYSCALL`을 쓰고 싶어지면 멈추고 [09-codex-playbook.md](09-codex-playbook.md)를 읽는다.**

---

## M3 — 첫 게스트 부팅 ⚠️ **프로젝트의 진짜 첫 증명**

**목표**: 스톡 Ubuntu 24.04 rootfs에서 `/bin/true`가 exit 0으로 끝난다.

**산출물**
- `src/cli/install.c` — [05-provisioning-spec.md §1~4](05-provisioning-spec.md)
- `src/cli/alr_untar.c` — 안전 추출
- `src/cli/launch.c` — ld.so 호출 argv 조립 ([§C2](01-platform-facts.md) 규격 정확히)
- `src/preload/` 최소 골격 — 생성자 + `ALR_ROOT` 읽기만. **아직 재작성 없음**

**Exit**
```
INSTALL DOWNLOAD:      PASS
INSTALL VERIFY SHA256: PASS
INSTALL EXTRACT:       PASS
INSTALL REPAIR:        PASS
INSTALL LDSO OPTIONS:  PASS   argv0=yes preload=yes library-path=yes inhibit-cache=yes
ALR BOOT /bin/true:    PASS   exit=0
ALR BOOT /bin/echo:    PASS   stdout="alr"
ALR GUEST GLIBC VERSION: 2.39
```

**주의**
- `--library-path`를 빼면 라이브러리를 못 찾는다. ld.so 자신의 탐색은 인터포즈 불가라 **선언적으로** 풀어야 한다.
- 프로그램 인자에 반드시 `/`가 있어야 한다 (`elf/dl-load.c:2017` — 슬래시 없으면 `$PATH`가 아니라 라이브러리 검색 경로를 탄다).
- `LD_PRELOAD`는 절대 호스트 경로여야 한다.
- Termux의 `LD_PRELOAD`를 **제거**한다 (빈 문자열 대입 금지).
- `GLIBC_TUNABLES=glibc.pthread.rseq=0` 설정.
- 여기서 실패하면 M2의 SIGSYS 테이블에 항목이 빠진 것이다. `ALR_LOG=2`로 어느 nr에서 죽는지 본다.

**이 마일스톤이 통과하는 순간이 "스톡 glibc가 Android seccomp 아래에서 부팅한다"는 증명이다.** 여기까지 오면 나머지는 엔지니어링이다.

---

## M4 — 경로 가상화

**목표**: 게스트가 rootfs를 `/`로 본다.

**산출물**
- `src/preload/alr_rewrite.c` — `rw()`, `guest_canon()`, sysdir/멱등 규칙
- `src/preload/wrappers.def` — 심볼 표 정본
- `src/preload/wrappers_*.c` — `wrappers.def`에서 생성 또는 그것에 맞춰 수동 작성
- `src/preload/alr_procfs.c` — `/proc/self/{exe,cmdline,root,cwd,mounts,status}` 가상화
- `src/preload/alr_dlopen.c` — §6.13 (Node 네이티브 애드온 / Python C 확장에 필수)
- `src/preload/alr_tmpfile.c` — §6.14 (`mkstemp` 계열 — 없으면 M6의 apt가 깨진다)
- `src/preload/alr_devfull.c` — §12 + doctor P9
- `bench/microbench/rw_cost.c` — `rw()` 마이크로벤치

**Exit**: [07-acceptance.md §2 M4](07-acceptance.md) 전부 PASS. 특히:
```
PRELOAD RW ABS COST: <= 100 ns/op
PRELOAD RW REL COST: <= 20 ns/op
```
> **MEASURED — 게이트 통과.** abs **61.0 ns** / rel **3.9 ns** / sysdir **13.8 ns**
> ([M4/M5](evidence/2026-08-02-m4-m5-path-exec.md)). 실사용 분포까지 재 보면 `git status` 10k 에서
> 호출 9,912회 중 **재작성은 26회(0.26%)**, 나머지 99.7% 는 상대경로라 첫 바이트 검사로 끝난다 —
> 경로 계층 총비용 **≈ 40 µs**, 모델(0.82 ms)보다 20배 싸다
> ([M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)). `p[0] != '/'` 를 첫 줄에 둔 판단이 여기에 있다.
> `/proc/self/cmdline` 가상화([04-preload-spec.md §7](04-preload-spec.md))는 스펙이 요구했는데 구현된 적이
> 없었고 — 그동안 게스트의 `cmdline` 이 로더 호출과 호스트 경로를 통째로 노출했다 —
> [M15 §1](evidence/2026-08-03-m15-cmdline-2604.md) 에서 닫혔다.

**주의**
- **`p[0] != '/'` 검사가 함수의 첫 줄이어야 한다.** git이 상대경로를 많이 쓴다.
- **정규화가 sysdir/멱등성 검사보다 먼저 와야 한다.** 순서가 반대면 `/proc/../etc/passwd`가 재작성 없이 통과해 게스트가 **Android 호스트의 `/etc/passwd`를 읽는다** ([04-preload-spec.md §5.1](04-preload-spec.md)의 경고).
- **`ALR_PBUF = PATH_MAX`(4096).** 더 작게 잡으면 커널이 받아들일 정상 경로를 거부한다.
- **캐시를 넣지 말 것.** memcmp보다 비싼 조회는 순손실이다.
- **malloc 금지.** 스택 버퍼만.
- **`stat` 계열을 호출하지 말 것.** 정의만 한다. `.2.17` 타깃이 링크 시점에 강제한다.
- `__*_chk` 5개(`__openat_2`, `__openat64_2`, `__getcwd_chk`, `__getwd_chk`, `__ttyname_r_chk`)를 빠뜨리지 말 것. 상위 프로젝트가 빠뜨린 것들이다.
- pre-2.33 stat 이름(`__xstat` 계열) 8개 **그리고 `fstat`/`fstat64`**도 정의한다 (M6 link2symlink가 `fstat`을 요구한다).
- `dlopen`(§6.13)과 `mkstemp` 계열(§6.14)을 M4에서 함께 한다. 둘 다 M6/M7에서 발견하면 원인 추적이 어렵다.
- `/proc/self/exe` 가상화는 **하드 요구사항**이다. Node의 `process.execPath`가 여기 달려 있고, 안 하면 npm/npx가 "Node 버그처럼" 깨진다.

---

## M5 — exec 연속성

**목표**: 게스트 안의 모든 exec가 후킹된 채 이어진다.

**산출물**
- `src/preload/alr_exec.c` — 13개 exec 함수 전부 + envp 재주입 + shebang
- `src/preload/alr_syscall.c` — `syscall()` 인터포즈 + 경로 인자 인덱스 표
- `src/preload/alr_guard.c` — `mount`/`umount2`/`chroot`/`pivot_root` → `EPERM`

**Exit**: [07-acceptance.md §2 M5](07-acceptance.md) 전부

> **경고의 실증 (2026-08-03)**: "13개를 전부 한다"를 지키지 않았다. **구현되어 있던 것은 7개뿐이었고**
> `posix_spawn` `posix_spawnp` `system` `popen` `pclose` `fexecve` `execveat` 이 없었다. 그동안 `make` 가 통째로
> 깨져 있었는데 **아무 테스트도 그것을 알려주지 않았다** — breadth 의 `build-essential` 검사가 `sh -c` 로
> 부르는 경로였기 때문이다. [M12 §1](evidence/2026-08-03-m12-spawn-resolver.md) 에서 메웠고,
> 심볼 정본과 게이트는 [M13](evidence/2026-08-03-m13-symbol-gate.md) 에서 만들었다(만들자 24개가 더 나왔다).

**주의**
- **13개를 전부 한다.** 하나라도 빠지면 그 경로의 자식이 `ENOENT`로 죽고, 디버깅이 매우 어렵다.
- shebang 재귀 깊이 상한 4.
- `symlinkat`의 `target`은 **재작성하지 않는다** — 심링크 내용은 게스트 네임스페이스 문자열이다. 이런 비대칭이 여러 개 있으니 표를 신중히 만들고 항목마다 테스트를 붙인다.
- bionic 경계(게스트가 `/system/*`이나 `$PREFIX`를 부를 때)에서 `LD_PRELOAD`를 **교체**한다.
- `syscall()` 인터포즈가 없으면 Node의 `fs.stat`이 전부 `ENOENT`다. 성능이 아니라 **정확성** 문제다.

---

## M6 — 패키지 매니저

**목표**: `apt install`이 게스트 안에서 동작한다.

**산출물**
- `src/fakeroot/` — uid0 스푸핑 + mmap 메타DB
- `src/preload/alr_l2s.c` — link2symlink ([04-preload-spec.md §8](04-preload-spec.md))
- `src/cli/doctor.c` P6 추가
- `<R>/etc/apt/apt.conf.d/99-alr-no-sandbox` 등 수리 항목
- fakeroot ↔ preload **체인 계약** 구현 ([02-architecture.md §4.4](02-architecture.md) (3)번 부류). fakeroot는 `chown`/`chmod`/`mknod`/`stat` 계열의 **경로를 스스로 재작성하지 않고** `dlsym(RTLD_NEXT)`로 preload에 체인한다

**Exit**: [07-acceptance.md §2 M6](07-acceptance.md) 전부

**결정 사항 — 둘 다 답이 나왔다 (2026-08-03). 더 이상 미결이 아니다.**
- ~~SysV IPC 를 허용하면 업스트림 `fakeroot` 를 쓴다~~ → **허용되지 않는다. 자체 shim 을 유지한다.**
  게스트에서 `shmget`/`semget`/`msgget` 이 전부 **`ENOSYS`** 를 돌려주고, 같은 실행의 `ALR_LOG=2` 가
  IPC 3건에 대한 슈퍼바이저 SIGSYS 에뮬레이션을 보여준다 — 즉 세 syscall 모두 **zygote 필터의
  `SECCOMP_RET_TRAP`** 에 걸린다 ([M16 §1](evidence/2026-08-03-m16-ipc-audit.md)).
  이 결정의 전제("막지 않는다면")가 반증되었으므로 **업스트림 `fakeroot`(SysV 변종) 분기는 닫힌다.**
  현재의 `ALR_FAKEROOT=1` 인프로세스 신원 사칭을 유지한다.
  > 대가는 정직하게 적는다: SysV IPC 를 쓰는 소프트웨어(일부 DBMS, X11 MIT-SHM)는 게스트에서 동작하지 않는다.
  > 대상 워크로드 중 쓰는 것이 없어 영향이 제한적일 뿐이다. `ENOSYS` 는 라이브러리들이 폴백 경로를 타게 하는
  > 표준 신호이기도 하다.
  > 남은 갈래 하나 — Debian/Ubuntu 의 `fakeroot` 가 SysV 를 쓰지 않는 **TCP 변종(`fakeroot-tcp`)** 도 함께
  > 배포한다고 알려져 있으나 **이 저장소가 확인한 사실이 아니다(UNVERIFIED)**. 게스트에서
  > `fakeroot-tcp -- dpkg --unpack` 한 번이면 판정된다. 자체 shim 의 유지보수 부채가 커질 때 재검토할
  > 값어치가 있는 갈래이지, 위 결정이 미결이라는 뜻은 아니다.
- ~~P6 이 `link(2)` 성공을 보고하면 link2symlink 를 끈다~~ → **`EACCES` 였다. 끄지 않는다.**
  브링업의 doctor P6 실측 ([evidence/2026-08-02-device-bringup.md](evidence/2026-08-02-device-bringup.md),
  [ADR 0004](adr/0004-link2symlink.md)). 단 구현은 ADR 0004 의 shadow-file 전면 구현이 아니라
  **inode 동일성만 만족시키는 복사 폴백 + `st_dev`/`st_ino` 테이블**이다 — 호출자가 실제로 검사하는 것이
  그것 하나였기 때문 ([M6](evidence/2026-08-02-m6-package-manager.md)).

**주의**
- fakeroot와 preload의 **심볼 분할이 load-bearing 계약**이다 ([02-architecture.md §4.4](02-architecture.md)). 공유 심볼(`stat` 계열, `chown`/`chmod`/`mknod` 계열)은 fakeroot가 바인딩을 이기되 **경로를 재작성하지 않고** `dlsym(RTLD_NEXT)`로 preload에 체인한다. 이걸 종단 처리로 만들면 `--fakeroot`(기본 켜짐)에서 모든 `chown`/`chmod`가 재작성 안 된 경로로 나가 `ENOENT`가 된다.
- link2symlink는 `stat`/`lstat`/**`fstat`**/`readlink`/`unlink`/`rename`/**`readdir`**/**`scandir`**를 함께 인터포즈하지 않으면 깨진다. `fstat` 누락 → `stat`과 `fstat`의 nlink 불일치로 dpkg 무결성 검사 실패. `readdir` `d_type` 보정 누락 → `find -type f`가 하드링크 파일을 전부 놓치고 `git add`가 설명 불가능하게 실패.

---

## M7 — 타깃 워크로드

**목표**: git, node, codex가 실사용 가능.

**Exit**: [07-acceptance.md §2 M7](07-acceptance.md) 전부

**이 마일스톤의 회귀 테스트가 특히 중요하다**
| 테스트 | 무엇을 지키는가 |
|---|---|
| `ALR GIT CLONE LOCAL` | link2symlink |
| `ALR GIT CLONE HTTPS` | NSS + resolver + 서브프로세스 exec |
| `ALR GIT HOOKS` | shebang exec |
| `ALR NODE EXECPATH` | `/proc/self/exe` 가상화 |
| `ALR NODE FS STAT` | `syscall()` 인터포즈 |
| `ALR NODE IO_URING SURVIVE` | 슈퍼바이저 SIGSYS 구제 (Node 22로 테스트) |

**Codex 관련 항목** — 하나는 답이 나왔고(우려보다 나쁘다), 하나는 아직 남았다

- ~~Codex가 `rustix` 크레이트의 raw-syscall 백엔드를 쓰는지 확인한다~~
  → **답: 백엔드를 볼 필요조차 없다. 더 나쁘다.** ([RISKS R7](RISKS.md) 해소)
  배포되는 `codex-aarch64-unknown-linux-musl` 은 **정적 링크 바이너리**다 — `INTERP` 프로그램 헤더도
  `NEEDED` 엔트리도 없다. 따라서 raw syscall 이든 libc 래퍼든 무관하게 **`LD_PRELOAD` 가 애초에 로드되지
  않는다**(`ALR_LOG=2` 에서 `alr preload:` 줄이 codex 는 0, 대조군 git 은 1).
  codex 의 모든 경로 연산은 rootfs 가 아니라 **Android 파일시스템**으로 간다
  ([M12 §8](evidence/2026-08-03-m12-spawn-resolver.md)).
  → **`ALR CODEX VERSION: PASS` 는 "바이너리가 실행된다"는 뜻이지 "게스트 안에서 동작한다"는 뜻이 아니다.**
  수용 테스트는 이 상태를 `ALR CODEX LINKAGE: KNOWN_FAIL:static-unhooked` 로 추적한다 — 향후 동적 빌드가
  나오면 자동으로 뒤집힌다. 대응 선택지(정적 바이너리 일반)는 [ADR 0006](adr/0006-raw-syscall-binaries.md).

- 샌드박스 비활성화 키 — **절반은 측정됐고, 절반은 `PENDING_DEVICE` 로 남는다** ([RISKS R8](RISKS.md))
  - **MEASURED**: 기기에서 `codex --help` 를 돌려 **`-s, --sandbox <SANDBOX_MODE>` 플래그**와 설정 키
    `sandbox_permissions` 의 존재를 확인했다 ([M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)).
  - **아직 아닌 것**: `alr install --with codex` 가 실제로 쓰는 것은 `<R>/root/.codex/config.toml` 에
    적는 **`sandbox_mode = "danger-full-access"`** 이고(`src/cli/alr.c` `with_codex()`), 이 **키 이름과
    모드 문자열은 `--help` 출력으로 대조된 적이 없다.** 소스의 주석도 아직 "확인하라"로 남아 있다.
    설치 시 `alr: NOTE codex sandbox disabled; alr is not a security boundary` 를 출력하는 부분만 구현됐다.
  - **더 큰 문제 (UNVERIFIED)**: 위 항목대로 codex 는 후킹되지 않는데 `alr` 은 게스트에 `HOME=/root` 를
    준다(`src/cli/alr.c`). 후킹되지 않은 프로세스에게 `/root/.codex/config.toml` 은 rootfs 안이 아니라
    **Android 의 `/root`** 다. 즉 **우리가 쓴 설정 파일을 codex 가 읽고 있지 않을 가능성이 높다.**
    설치 시 codex 가 내는 `could not create PATH aliases: Read-only file system` 경고가 같은 방향을
    가리키지만, 이것으로 읽기 여부를 단정할 수는 없다.
  - **무엇이 이것을 끝내는가**: 기기에서 (1) `codex --help` 의 키/모드 철자를 **그대로 받아 적고**,
    (2) 그 경로에 고의로 망가뜨린 TOML 을 두어 codex 가 **불평하는지**로 파일을 실제로 읽는지 판정하고,
    (3) 읽지 않으면 설정 파일 대신 `--sandbox` 플래그를 넘기도록 `with_codex()` 를 고친다.
    그 뒤 [05-provisioning-spec.md §5.2](05-provisioning-spec.md) 를 갱신한다.
  - **무엇이 막고 있는가**: 디바이스 세션 하나. **추측해서 하드코딩하지 말 것** — 지금 값이 바로 그 추측이고,
    그래서 (2)가 (1)보다 먼저 필요할 수도 있다.

> **따라서 M7 은 완료가 아니다** ([00-product.md §6](00-product.md) 규칙 3). git·node·npm 쪽은 전부 PASS 이고
> `npm ci` 3.12× 까지 실측됐지만, codex 는 `KNOWN_FAIL:static-unhooked` 이고 `ALR CODEX SANDBOX DISABLED` 는
> 위 이유로 판정되지 않았다. **codex 를 "실사용 가능" 으로 적지 않는다.**

---

## M8 — 성능 + A/B

**목표**: **아무도 공개한 적 없는 숫자를 만든다.**

**산출물**
- `bench/` 이식 완료
- `alr bench --vs proot` 동작
- `bench/regression_gate.py`
- 참조 디바이스 2종에서 측정한 리포트 (벤더·커널 상이, 둘 다 Android 16 — [ADR 0007](adr/0007-android-16-only.md))

> **현재 상태 (2026-08-03, 갱신) — 4줄 중 3줄 충족, 남은 하나는 기기다.**
> - ✅ **하네스**: `tests/device/bench.sh`(A/B), `tests/device/rw_bench.sh`(재작성 총비용).
>   [M17 §5](evidence/2026-08-03-m17-bench-ab.md)에 적은 대로 `alr bench` **서브커맨드**가 아니라
>   기기 하네스로 만들었다 — 측정 대상 런타임 안에서 proot 를 오케스트레이션하는 것은 자리가 틀리다.
> - ✅ **`bench/regression_gate.py`**: 있다. 하드 불변식 + 기기별 회귀 검사 + `--self-test`.
> - ✅ **2기종 리포트**: [M19](evidence/2026-08-03-m19-snapdragon.md) — MediaTek(커널 6.1)과
>   Snapdragon 8 Elite(커널 6.6). 차단 집합 239개 동일, 수용 78, 폭 96/96 일치.
> - ✅ **참조 디바이스 2종**: MediaTek(커널 6.1)·Snapdragon 8 Elite(커널 6.6), 둘 다 Android 16.
>   구버전 Android 는 산출물에서 내렸다 — 미측정이 아니라 **범위 밖**이다([ADR 0007](adr/0007-android-16-only.md)).
>
> 배수도 갱신됐다: `git status` 10k 는 **25.8×**(양쪽 git 2.53.0 동일)가 인용할 값이고, 34.8× 는
> 세 실행의 git 빌드가 서로 달랐던 M8 수치다([M19 §6.1](evidence/2026-08-03-m19-snapdragon.md)).

**Exit**: [07-acceptance.md §2 M8](07-acceptance.md) + 다음
```
ALR MEDIATION INVARIANT: path_traps=0 syscall_stops=0
```
> `ALR MEDIATION INVARIANT` 는 **MEASURED — PASS**. `git status` 10k 실행에서
> `pids=21 sigsys=22 emulated=22 path_traps=0 syscall_stops=0`
> ([M7/M8](evidence/2026-08-02-m7-m8-workloads-perf.md)). PRoot 와 갈리는 불변식이 실측에서도 성립한다.

**주의**
- **`getenforce`/`Seccomp:` 검증 없이 결과를 발표하지 않는다.**
- PRoot 베이스라인을 하나로 고정한다 (`PROOT_NO_SECCOMP=1`과 기본값을 한 차트에 섞지 말 것).
- 목표 배수는 [00-product.md §4](00-product.md)의 방어 가능한 범위 안이어야 한다. 넘으면 측정이 틀렸거나 베이스라인이 잘못된 것이다.
  > **정정 (2026-08-03)**: `git status` 는 실제로 넘었다 — §4 의 1.5~4× 추정에 대해 실측 **34.8×**.
  > 재검토 결과 틀린 것은 측정이 아니라 추정 쪽이었다("PRoot 는 필터 테이블의 syscall 만 트랩한다"는 전제).
  > 반면 `npm ci` 는 3.12× 로 추정 범위 안이다. **넘었을 때 먼저 의심할 것은 여전히 자기 측정이다.**
- **auditallow 로그 볼륨** ([07-acceptance.md §6](07-acceptance.md), [RISKS R6](RISKS.md)) —
  **`PENDING_DEVICE` 유지. 다만 무엇이 막고 있는지는 이제 안다.**
  - 앱 프로세스 **안에서는 잴 수 없다**: exec 를 6회 낸 전후로 `logcat -b events -d` 가 **에러 없이 0줄**을
    내는 반면 `logcat -b main -d` 는 내용을 낸다. Android 가 events 버퍼를 권한 있는 리더에게만 열기 때문이다
    ([M16 §2](evidence/2026-08-03-m16-ipc-audit.md)).
  - **이 0 을 "오버헤드 없음" 으로 적으면 권한 실패를 측정으로 둔갑시키는 것이다.** 그렇게 적지 않는다.
  - **무엇이 이것을 끝내는가**: 워크로드(`git rebase`, npm postinstall 같은 exec 집약)는 반드시 Termux 앱
    컨텍스트(`uid>=10000 ∧ Seccomp=2`)에서 돌리고, 로그는 **외부 관찰자 adb** 가 동시에 읽는다.
    관찰자는 실행 조건을 바꾸지 않으므로 이 조합은 유효한 증거가 된다.
  - **무엇이 막고 있는가**: adb 가 붙은 디바이스 세션. 이 숫자가 없으면 **오버헤드 주장을 발표하지 않는다** —
    native 대비 기동 오버헤드(같은 문서의 두 세션에서 +8 ms, +4 ms) 중 감사 레코드 몫이 얼마인지 아직 모른다.

---

## M9 — 배포

**산출물**
- termux-packages `build.sh` (`pkg install alr`)
- GitHub 릴리스: `alr` 바이너리 + 두 개의 `.so` + `manifest.json`
- README에 방어 가능한 숫자만 담은 성능 표
- **호환성 폭 리포트** ([07-acceptance.md §5](07-acceptance.md)) — grun 대비 유일한 차별점이므로 헤드라인 지표

> **현재 상태 (2026-08-03)**
> - `packaging/termux/alr/build.sh` 는 **있다.** 업스트림 termux-packages 에 올라간 적은 없다.
> - GitHub 릴리스·`manifest.json` 은 **아직 없다.**
> - 호환성 폭은 **MEASURED**: 큐레이션된 96개 패키지에서 설치 96/96, 실행 96/96
>   ([M11](evidence/2026-08-02-m11-breadth.md) 에서 96/96·95/96,
>   [M14 §2](evidence/2026-08-03-m14-ioctl-php.md) 에서 php-cli 가 풀려 실행 96/96,
>   [M15](evidence/2026-08-03-m15-cmdline-2604.md) 에서 회귀 없음 확인).
>   **인용할 때의 정직한 표현은 [00-product.md §3](00-product.md) 을 따른다** — 아카이브 전체가 아니라
>   큐레이션된 96개, 단일 MediaTek 기기다. 그리고 php 는 **원인을 모른 채 임계값 위에 있을 뿐이다**
>   (M14 §2) — "고쳤다" 로 적지 않는다.
> - **M8 의 auditallow 숫자가 없는 채로 README 성능 표를 내지 않는다.** 위 §M8 참조.

---

## 시간 배분 권고

| 마일스톤 | 상대 난이도 | 비고 |
|---|---|---|
| M0, M1 | 낮음 | 디바이스 없이 전부. 여기 정확성이 이후를 좌우하니 서두르지 말 것 |
| M2 | **높음** | ptrace + 시그널 의미론. 함정 3개가 전부 여기 있다 |
| M3 | 중간 | 대부분 배관. 여기서 막히면 M2 테이블 문제 |
| M4 | 중간 | 심볼 수가 많지만 반복적. 성능 예산이 진짜 제약 |
| M5 | **높음** | exec 13종 + 비대칭 경로 인자. 버그가 가장 잘 숨는 곳 |
| M6 | 중간 | ~~P6이 꺼줄 수도 있다~~ — P6은 `EACCES`였다. link2symlink는 켠 채로 간다 |
| M7 | 중간 | 대부분 회귀 테스트 작성. 단 codex 는 정적 링크라 **테스트로 해결되지 않는다** |
| M8 | 낮음 | 이식 + 측정 |
