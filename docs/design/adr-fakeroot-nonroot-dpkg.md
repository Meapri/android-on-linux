> 생성 경위: R12(fakeroot — non-root dpkg/apt unpack) 심층연구 4축(F1 mechanism / F2 metadb / F3 coexist-envp / F4 aux) + 자가 적대검증 종합. 격리 worktree `/Users/naen/Documents/alr-fakeroot`, base `main` 69cd5da/v144, branch `research/fakeroot`. 코드 사실은 본체 파일 **읽기 전용** 직접 확인(`runtime_report.cpp`, `alr_interpose/libalr_interpose.c`, `alr_inproc_reexec.c`, `alr_runtime/alr_exec.cpp`) + 신규 fakeroot 소스(`app/src/main/cpp/alr_fakeroot/`) 직접 확인 + zig 크로스컴파일·host 테스트 실측. 리뷰 대상: 본체 세션(envp/LD_PRELOAD 두 string-build 지점), 통합 세션(device DEVICE-REQ).

# ADR-fakeroot — 비root dpkg/apt unpack: LD_PRELOAD fakeroot(.so 분리) + (dev,ino) mmap 메타DB

- 상태: **PROTOTYPE-DEVICE-PENDING** — mechanism·빌드·DB semantics는 host로 풀림(증명 첨부), 통합 동작(2-.so 공존 런타임, fork+exec DB 일관, dpkg unpacked=true)은 device 미검증. **device evidence 없이 "non-root unpack 풀림" 주장 금지.**
- 워크스트림: **fakeroot 연구 세션**(이 ADR + `app/src/main/cpp/alr_fakeroot/**` + `tests/test_fakeroot*` 주관). 본체 세션이 §5b의 두 string-build 지점 반영. 통합 세션이 §6 DEVICE-REQ 실행.
- 작성: 2026-06-02. 선행: `docs/design/adr-003-multiprocess-exec-reentry.md`(exec re-entry, R11/v144 device-proven), `docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md`(이 게이트의 발단). 톤: ADR-002/003.
- 대상: R11(v144)이 exec re-entry(no-execve map+jump)를 device-proved한 뒤, `apt install`이 여전히 `unpacked=false`로 막히는 **별개·독립 게이트** — round-10 step2 evidence의 `dpkg: error: requires superuser privilege`. 비root dpkg가 unpack 중 chown/mknod를 EPERM받는 credentials/permissions 벽. proot의 "fake uid/gid 0"(fakeroot) 기능에 해당하는 것을 ALR에 in-process로 구현.
- HARD CONSTRAINTS(불변): 비root(untrusted_app, no CAP_*), public Android API only, SELinux 우회 금지, W^X-safe(이 .so는 ordinary file-backed r-x, 런타임 codegen 0), in-process(PRoot fallback-only), device evidence 없이 완료 주장 금지, version stamp 불변, 본체 파일(runtime_report.cpp/libalr_interpose.c/alr_inproc_reexec.c/alr_exec.cpp/MainActivity.kt 등) **읽기만**.

---

## 1. 한 줄 결론

**비root dpkg가 막히는 곳은 "시작 시 superuser abort 게이트"가 아니라 *압축 해제 중 첫 chown/mknod의 EPERM*이다 — 따라서 해법은 (1) credential getter(getuid/geteuid/…)를 0으로 spoof해 dpkg가 "나는 root"로 믿게 하고, (2) chown을 실호출 없이 (dev,ino)→uid/gid 메타DB에 기록한 뒤 stat이 그 값을 되돌려주게 해 self-consistency(verify/maintainer-script 일치)를 주고, (3) char/block device의 mknod를 placeholder+DB로 에뮬레이트해 "fakeroot 없이는 절대 못 넘는 단 하나의 hard wall"(dpkg는 mknod 실패를 force 래퍼 없이 무조건 abort)을 돌파하는 것이다. 이를 interposer와 *분리된 별도 LD_PRELOAD .so*(`libalr_fakeroot.so`)로 구현하되, 심볼 책임을 path=interposer / credential+metadata=fakeroot로 파티션하고 stat-family만 fakeroot가 win→RTLD_NEXT로 interposer에 체인하면 두 .so가 충돌 0으로 공존한다.** dpkg의 `--force-not-root`는 chown EPERM을 흡수하지만 self-consistency도 mknod wall도 안 줘서 **보완재이지 대체재가 아니다** — fakeroot가 본체, force 플래그는 aux. 본 설계는 host에서 빌드(zig, 경고0 aarch64 .so, 36 FUNC export)·DB semantics(`test_fakeroot_metadb.py` 10/10, `test_fakeroot_db_shared.py` 3/3)·심볼 파티션(interposer path 심볼과 교집합 0; 겹치는 정의 15개 전부 fakeroot가 LD_PRELOAD 선두로 win)이 전부 증명되었고, **남은 단 하나는 device 통합**(2-.so 동시로드 시 cred priority·fork+exec mmap 일관·dpkg unpacked=true)이다.

---

## 2. 메커니즘 — non-root dpkg unpack의 정확한 권한 요구 + 최소 후킹셋 (F1)

### 2-a. dpkg가 막히는 정확한 지점 (오해 정정)

`requires superuser privilege` 메시지의 흔한 오독은 "dpkg가 시작 시 uid를 보고 abort"이지만, dpkg 1.23.x 소스 확인 결과 **시작 단계 superuser abort는 없다**(`can_invoke_hooks()`는 pre/post-invoke 훅 실행 여부만 결정 — `getuid()||geteuid()`면 훅만 건너뜀). 실제 권한 거부는 **`tarobject_extract` 중 첫 `fchown`/`lchown`/`fchmod`의 EPERM**에서 발생한다. dpkg 1.23.7 추출 syscall 맵(archives.c):

| tar 엔트리 | syscall 시퀀스 | 권한 의존 |
| --- | --- | --- |
| FILE | open(O_CREAT\|O_EXCL,0)→write→**fchown(fd)**→**fchmod(fd)**→close→utimes | fchown/fchmod |
| DIR | mkdir(path,0)→**lchown(path)**+**chmod(path)**→utimes | lchown/chmod |
| SYMLINK | symlink→**lchown(path)**→lutimes | lchown (chmod 없음) |
| HARDLINK | link(src,path)→utimes | 없음(원본 공유) |
| FIFO | mkfifo(path,0)→lchown+chmod→utimes | lchown/chmod |
| CHARDEV | **mknod(S_IFCHR,dev)**→lchown+chmod→utimes | **mknod** + lchown/chmod |
| BLOCKDEV | **mknod(S_IFBLK,dev)**→lchown+chmod→utimes | **mknod** + lchown/chmod |

**결정적 에러처리 비대칭:** chown/chmod 계열은 모두 `if (forcible_nonroot_error(rc)) ohshite(...)` 래핑 — `FORCE_NON_ROOT`(dpkg≥1.21.8, `--force-not-root`)면 EPERM을 흡수(0 반환)해 abort 안 함. **그러나 mknod는 `if (mknod(...)) ohshite(...)` — force 래퍼 없이 무조건 abort.** 즉:
- `--force-not-root` 단독 = chown/chmod EPERM은 넘지만 (a) self-consistency 없음(chown은 no-op되나 stat은 여전히 실 uid 보고 → verify/maintainer-script 불일치 가능), (b) **char/block device node 포함 패키지는 mknod에서 여전히 abort**.
- fakeroot의 추가가치 = getuid→0(self-consistency 기반) + chown→DB(stat 되돌림) + mknod→placeholder+DB(device node wall 돌파).

> **미검증(§7-U1):** mknod abort 무조건성은 WebFetch 요약 경유라 1.23.7 archives.c 원문 mknod 블록을 device 통합 전 직접 확인 권장. 신버전이 mknod에도 forcible 래퍼를 추가했다면 device wall이 `--force-not-root`만으로 풀릴 수도 있다(그러면 fakeroot mknod 에뮬은 self-consistency용으로만).
> **미검증(§7-U2):** noble base-system .deb들이 실제로 static device node(mknod entry)를 포함하는지 미확인 — 현대 base-files/udev는 devtmpfs 의존이라 .deb에 static node가 거의 없다. 0개면 mknod wall은 실무상 안 닥치고 chown만으로 충분. **host에서 사전 스캔 가능**(§6 probe-E, device 불필요).

### 2-b. 후킹 심볼별 에뮬레이션 (실제 구현 = `libalr_fakeroot.c`)

| 그룹 | 후킹 | 동작 |
| --- | --- | --- |
| credentials→root | getuid/geteuid/getgid/getegid → 0; getres[ug]id → 0; getgroups → {0} | **순수 상수**(fork-safe, 공유상태 불요). dpkg superuser 체크가 0을 봄 |
| set*id | setuid/seteuid/setgid/setegid/setre[ug]id → 0 success | 실 syscall 안 함(비root면 EPERM). maintainer script(adduser 등) 흡수 |
| chown | chown/lchown/fchown/fchownat | **실 chown 안 함**(EPERM 회피). real fstatat로 (dev,ino) 키 도출 → DB에 uid/gid 기록 → 0 반환 |
| chmod | chmod/fchmod/fchmodat | real chmod **best-effort**(소유 파일 perm bits는 대개 성공) + 요청 mode 전체(setuid/sticky 포함, 커널이 strip해도)를 DB 기록 |
| mknod | mknod/mknodat | regular/FIFO/socket = forward(커널 허용); **char/block special = 0-len placeholder regular file 생성 + (S_IFCHR/BLK, rdev) DB 기록**. ← 유일 hard wall 돌파 |
| stat overlay | stat/lstat/fstat/fstatat/newfstatat | real stat 먼저(RTLD_NEXT→interposer) → (dev,ino) 조회 → DB 있으면 st_uid/gid/mode/rdev 오버레이 |

**glibc 2.39(noble) 심볼 현실:** glibc는 `__xstat`/`__lxstat`/`__fxstat` 래퍼를 제거하고 `stat`/`lstat`/`fstat`/`fstatat`/`newfstatat`를 직접 export(내부 newfstatat/statx). 따라서 noble dpkg/maintainer script는 modern `stat()` 직접 심볼을 호출 → fakeroot가 이 modern 심볼을 후킹해야 주 경로가 잡힌다(host 확인: noble glibc 2.39 stat 직접 바인딩, dead `__xstat` vtable 아님). 본 .so는 modern 심볼을 주 경로로 후킹하면서 구 `__xstat`/`__lxstat`/`__fxstat`/`__fxstatat`도 호환 fallback으로 정의(구 게스트 바이너리용).

> **statx 갭 — 해소됨(개정 R12b):** 초기 .c는 statx 미구현이었으나 본 개정에서 `statx` wrapper + `fr_overlay_statx`를 추가했다. statx는 (dev,ino)를 major/minor 쌍·`stx_ino`로 노출하고 rdev도 major/minor 분리이므로, 커널 dev_t 비트레이아웃(glibc gnu_dev) 역산으로 (dev,ino)를 재조립해 동일 DB 엔트리를 조회·오버레이한다. coreutils `stat`/`ls`·dpkg-deb가 noble에서 statx로 메타를 읽어도 overlay가 걸린다(host 빌드 확인; `struct statx` 필드 오버레이 컴파일 통과). **단 statx의 dev_t 역산 정확성·실제 dpkg statx 사용분은 device 실측 잔여(§6-a U-STATX).**

---

## 3. 메타DB — 프로세스 공유 방식 ((dev,ino) mmap 파일) (F2)

### 3-a. 왜 mmap 공유파일인가 (libfakeroot daemon 기각)

libfakeroot는 (ino→uid/gid/mode/dev) 맵을 `faked` 데몬에 두고 SysV msg queue로 통신한다. 비root Android(untrusted_app)에서:
- **SysV IPC(msgget/msgsnd)는 bionic seccomp 비허용 + 우리가 spawn할 privileged faked 없음** → 기각.
- abstract unix socket은 same-UID/same-app은 보통 허용이나 데몬 lifecycle/연결관리 부담 → 회피.
- **mmap(MAP_SHARED) 단일 파일 = 추가 권한 0, 데몬 0, 가장 단순.** mmap+ftruncate+futex는 코드베이스 GPU ring/supervisor에서 이미 device-proven allowed.

핵심 요구: dpkg는 maintainer script(preinst/postinst/…)를 **fork()+exec()**한다. 자식이 한 가짜 chown이 부모 dpkg의 후속 stat에 보여야 → DB가 fork+exec를 넘어 공유돼야 한다. mmap(MAP_SHARED) 파일을 모든 게스트가 같은 경로($ALR_FAKEROOT_DB)로 map → child write가 parent에 즉시 보임(공유 페이지, 메시지 왕복 0).

### 3-b. 레이아웃 (`alr_fakeroot_db.h`)

- 키 = **(st_dev, st_ino)** — 경로 아님. interposer가 어떤 host 경로로 rewrite했든 안정 → post-rewrite 경로 문자열 합의 불필요. rename(inode 불변→오버레이 자동 추종)·hardlink(같은 inode→공유, 실 Unix 의미와 일치)·삭제후재생성(새 inode→stale 자동 폐기, tombstone `ALR_FR_F_DELETED`)이 전부 올바르게 동작.
- 32B 엔트리(`alr_fr_entry`): key_dev, key_ino(0=empty), f_uid, f_gid, f_mode(type+perm), f_rdev_lo/hi, f_flags(HAVE_UIDGID/HAVE_MODE/HAVE_RDEV/DELETED).
- 헤더(`alr_fr_header`): magic, version, **futex lock word**(0=free/1=held; FUTEX는 Android 허용), n_slots(2의 거듭제곱), n_used(advisory). 슬롯은 2번째 페이지(`ALR_FR_SLOTS_OFF=4096`)부터.
- 고정크기 open-addressing(FNV-1a over (dev,ino), linear probe), 기본 262144 슬롯(~8MiB), 리사이즈 없음. `ALR_FAKEROOT_DB_SLOTS`로 override.
- **동시성:** writer는 헤더 futex spinlock(CAS+FUTEX_WAIT/WAKE)으로 cross-process 직렬화. 슬롯 발행 = value 필드 먼저 채우고 `key_ino`를 release-store로 마지막 publish(부분쓰기 슬롯은 reader가 acquire-load로 못 찾아 real stat fallback). reader는 lock-free. dpkg는 패키지당 대체로 단일스레드 unpack이라 경합 낮음.
- **오버플로 = fail-safe**: 슬롯 full이면 오버라이드 미기록, real on-disk uid/gid 보고 — 단일유저 rootfs는 guest 자신이 이미 소유라 dpkg가 믿고 싶은 값과 근사.

### 3-c. exec 생존 (ALR 특수)

fork 자식은 MAP_SHARED 매핑을 **상속**(재open 불요). exec 자식은 주소공간 교체로 매핑이 사라지나 ctor에서 같은 $ALR_FAKEROOT_DB 경로로 **re-open+remap** → 공유 페이지 복원. **ALR in-process re-map**(R11/v144) 경로도: `alr_inproc_reexec.c`가 envp를 x21에서 그대로 통과(line 655 "envp comes straight from x21", 685-693 envp 복사)하고 새 SysV 스택+auxv 합성 → guest ld.so 재실행 → libalr_fakeroot.so ctor 재실행 → 같은 DB 재map. 따라서 dpkg→maintainer fork+exec 체인과 ALR re-map 양쪽에서 DB가 유지된다.

> **권고:** DB 파일은 rootfs **밖** app private dir(ALR run dir)에 두고 interposer rewrite 대상에서 제외(raw 절대 host path). DB는 run-scoped(설치 세션마다 새로 — 다음 부팅 시 실파일 소유와 동기화 문제 회피; 영속화는 v2).

### 3-d. host 증명

`tests/test_fakeroot_metadb.py` **10/10 PASS**(dev+ino 키잉, rename 추종, hardlink 공유, inode-reuse 미적용, mknod placeholder+rdev, lchown 의미). `tests/test_fakeroot_db_shared.py` **3/3 PASS**(fork-inherit coherence, reopen-by-path coherence, open-addressing collision). 모델: `tests/fakeroot_metadb_model.py`.

---

## 4. interposer 공존 + LD_PRELOAD/envp 전파 계약 (F3) — §5 인터페이스

### 5-a. 심볼 파티션 (host 검증: fakeroot 36 export, interposer와 겹침 15개, 전부 fakeroot가 win)

책임 분리: **경로 = interposer, credential+metadata = fakeroot.** `libalr_fakeroot.so` 실측 export = **36개 FUNC**(objdump -T 교차계산):
```
__fxstat __fxstatat __lxstat __xstat chmod chown fchmod fchmodat fchown
fchownat fstat fstat64 fstatat fstatat64 getegid geteuid getgid getgroups
getresgid getresuid getuid lchown lstat lstat64 mknod mknodat newfstatat
setegid seteuid setgid setregid setreuid setuid stat stat64 statx
```
interposer가 정의하는 **path 심볼**(open/openat/creat/access/readlink/opendir/mkdir/rename/link/symlink/unlink/utimensat/realpath/chdir/remove 등)을 fakeroot는 **정의하지 않음 → path 책임 충돌 0**(host에서 명시 확인: fakeroot export 36개 중 path 심볼과 교집합 0). 두 .so가 동시에 정의하는 심볼은 **정확히 15개**(stat-family 풀커버 확장 후. 주의: interposer는 bare `stat`/`lstat`/`fstat`/`stat64`/`lstat64`를 정의하지 *않고* legacy `__xstat`/`__lxstat` 버전 심볼 + modern `fstatat`/`newfstatat`/`statx`/`fstatat64`만 정의하므로, 겹침은 그 modern·legacy 심볼에 한정):
```
__fxstatat __lxstat __xstat chmod fchmodat fstatat fstatat64 getegid geteuid
getgid getuid mknod mknodat newfstatat statx
```
(host에서 objdump -T(fakeroot 36 export) ∩ 소스 정의 스캔(interposer 60 def) = **15**, 교차 계산 스크립트 실측.) 이 15개는 **fakeroot가 LD_PRELOAD 선두라 전부 win한다.** bare `stat`/`lstat`/`fstat`/`stat64`/`lstat64`는 fakeroot 단독 정의(interposer 미정의)이므로 RTLD_NEXT가 libc로 떨어지나, 그 경로의 path rewrite는 fakeroot가 호출하는 내부 `fstatat`/`newfstatat`(=interposer 정의, 트램폴린 경유)에서 이미 일어나므로 손실 없음. 충돌별 처리:

1. **cred getter(getuid/geteuid/getgid/getegid) — CRITICAL.** interposer는 PCGATE에서 이 4개를 *실제 비root값으로* memoize한다(코드 확인: `getuid()`가 `alr_cached_id(&c, __NR_getuid)` → `alr_tramp_syscall`로 raw getuid 캐시). fakeroot는 0을 반환해야 한다. **ELF 인터포지션 규칙: LD_PRELOAD 리스트에서 먼저 나온 .so 정의가 이긴다** → fakeroot를 interposer보다 **앞에** 두면 fakeroot의 getuid=0이 이기고 interposer의 memoize는 호출조차 안 됨(dead path). interposer 내부에서 자기 getuid를 직접 부르는 hot path는 없음(rw()는 uid 비의존, 확인) → 무해. **순서 역전 시 interposer의 캐시된 real uid가 win해 fakeroot가 무력화** → 로더가 순서를 보장해야 함(§5b, DEVICE-REQ).
2. **chmod/fchmodat/mknod/mknodat.** interposer는 path-rewrite용으로만 정의. fakeroot가 win하되 **스스로 `fr_rw()`로 동일 $ALR_ROOTFS rewrite**(interposer rw()의 규칙을 mirror: /proc·/sys·/dev·이미-rootfs-아래는 passthrough, 아니면 prepend)하므로 path 중재 손실 0. interposer의 `a_under` idempotency guard(확인)가 double-rewrite를 막으므로 fakeroot가 추가로 RTLD_NEXT를 타도 `<rootfs><rootfs>` 안 생김.
3. **stat-family(stat/lstat/fstat/fstatat/newfstatat).** fakeroot가 win하되 **자체 path rewrite 안 함** — `dlsym(RTLD_NEXT,"stat")`로 다음 정의(=interposer stat)를 호출 → interposer가 rootfs rewrite+트램폴린 newfstatat emit → 커널이 buf 채움 → fakeroot가 (dev,ino)로 DB 오버레이만 덮음. 체인: `guest stat → fakeroot(stat) → [RTLD_NEXT] interposer(stat)[rewrite+emit] → kernel → interposer fills buf → fakeroot overlays DB`. **path 책임 100% interposer, metadata 100% fakeroot.** interposer 부재 시 RTLD_NEXT가 libc real stat로 떨어져도 fakeroot는 오버레이만(rootfs rewrite 없이 — 그건 interposer 일).

### 5-a-bis. PCGATE seccomp 무충돌

fakeroot는 BPF/트램폴린 없음. fakeroot의 chown/mknod는 **실 path syscall을 안 냄**(no-op 또는 placeholder open만) → 트램폴린/트레이스와 무관. fakeroot가 내는 유일한 실 I/O = placeholder openat·real stat용 newfstatat인데, 이는 **RTLD_NEXT(interposer wrapper) 경유**로 나가 신뢰 PC(트램폴린)로 ALLOW. **fakeroot는 raw inline syscall을 직접 내면 안 됨**(트램폴린 밖 PC → RET_TRACE backstop, 느리지만 안전). 계약: fakeroot의 모든 실 I/O는 RTLD_NEXT 경유만.

### 5-b. envp/LD_PRELOAD 전파 계약 (본체 세션 요구사항 — 본체 파일 안 건드림)

현 본체의 LD_PRELOAD 구성은 **두 string-build 지점**에 hard-code됨. fakeroot가 cred/stat을 win하려면 **fakeroot.so가 interpose.so보다 앞**에 와야 함:

1. **`runtime_report.cpp:1623-1624`** (첫 게스트 env push) — 현재:
   ```
   guest_env.push_back("LD_PRELOAD=" + config.rootfs_dir + "/usr/lib/androlinux/libalr_interpose.so");
   ```
   **요구:** fakeroot.so를 **앞에** colon-prepend →
   `LD_PRELOAD=<rootfs>/usr/lib/androlinux/libalr_fakeroot.so:<rootfs>/usr/lib/androlinux/libalr_interpose.so`
   + `ALR_FAKEROOT_DB=<per-run dir>/fakeroot.db` + (선택) `ALR_FAKEROOT_UID=0` env 추가.
2. **`alr_runtime/alr_exec.cpp` `decide_exec_envp_injection`** (B-3 자식 envp 재주입, line 340–391) — 현재 `out.interpose_so = <rootfs>/.../libalr_interpose.so` 단일, `colon_list_contains`/prepend 인프라 이미 존재. **요구:** fakeroot.so를 interpose.so **앞에** 두도록 colon-list를 확장(`fakeroot_so + ":" + interpose_so` 형태로 `ld_preload_value` 구성, `colon_list_contains` 만족 검사도 fakeroot.so 포함). 한 줄급 변경.
3. **exec re-entry(`alr_inproc_reexec.c`)** — envp를 x21에서 그대로 복사(line 655, 685-693)하므로 부모 envp에 fakeroot.so가 있으면 re-map된 게스트도 상속 — **별도 작업 불필요**(round-7 envp_reason=already가 LD_PRELOAD 상속 입증).

즉 본체 변경은 (1)(2) 두 지점뿐. 이 ADR이 그 계약을 명시하고 본체 세션이 반영.

---

## 5. device 단계 + DEVICE-REQ (§6)

researcher(나)는 device 없음 → 아래는 **통합 세션이 device SM-X236N에서 실행**. 산출 로그에 `ALR-FAKEROOT db=mapped slots=N geteuid=0 rootfs_len=L` 한 줄 diag를 ctor에서 stderr emit해 MainActivity 리포트에 노출 권장(android-visible-probe-wiring 체인).

| # | probe | 명령/관찰 | PASS 기준 |
| --- | --- | --- | --- |
| A | cred-priority | 2-.so 로드(fakeroot 먼저) 후 guest `id -u` / `geteuid()` | `0` 출력 = fakeroot가 interposer memoize를 이김 |
| B | superuser-gate | `dpkg --unpack <small.deb>` (fakeroot+interposer) | round-10 step2 `requires superuser privilege` **소멸** + unpack 진행 |
| C | self-consistency | `touch f; chown 0:0 f; stat -c '%u:%g' f` | `0:0` (DB 오버레이 왕복). 대조군: `--force-not-root` 단독은 실 uid |
| D | cross-exec coherence | `sh -c 'chown 0:0 f'`(fork+exec 자식 기록) 후 부모 stat | 부모가 `0:0` 봄(fork-share mmap DB) |
| E | **target-deb-scan (host, device 불필요)** | `dpkg-deb --fsys-tarfile X.deb \| tar -tv \| grep '^[cb]'` | mknod entry 유무 → 있으면 mknod 에뮬 critical, 없으면 chown 우선 |
| F | mknod-wall | device node 포함 패키지 `dpkg --force-not-root --unpack`: (A) 에뮬 OFF vs (B) ON | A=device-node abst / B=통과 → mknod 에뮬 가치 확증 |
| G | exec re-entry 생존 | `ALR_REEXEC_INPROC=1` re-map 후 `geteuid()` | `0` 유지(DB env·매핑 re-map 생존) |
| H | FUTEX | 동시 두 guest가 같은 파일 chown 폭주 | DB 손상/데드락 0 |
| I | apt-e2e | `apt-get -o DPkg::Options::=--force-not-root install <pkg>` (fakeroot+interposer) | `unpacked=true` 도달 = **최종 성공 기준** |

**최종 성공 기준:** round-10 step2의 `requires superuser privilege`가 사라지고 `apt install` `unpacked=true` 도달.

---

## 6. 정직 — device 전 미확정 + 잔여 구멍 + fallback

### 6-a. 미검증 (device 필요, host 재현 불가)

- **U-RTLD_NEXT:** fakeroot stat → interposer stat의 cross-.so RTLD_NEXT 체인. macOS dyld가 LD_PRELOAD two-level 체인을 무시해 host 재현 불가 — glibc RTLD_NEXT 의미상(호출 객체 search-order 다음부터) 성립은 확립이나 PCGATE 트램폴린 emit과의 상호작용은 device 증명 필요.
- **U-CRED-PRIORITY:** LD_PRELOAD 순서가 cred getter 우선순위를 실제로 결정하는지는 ELF 규칙상 맞지만(둘 다 STB_GLOBAL default-visibility 가정) device 실측 필요. 역전 시 fakeroot 무력화.
- **U-FORK-SHARE:** fork+exec 자식의 가짜 chown이 부모 dpkg stat에 보이는 cross-exec 경로는 host로 부분 증명(reopen-by-path coherence)했으나 실 dpkg maintainer 체인은 device.
- **U-FUTEX:** untrusted_app에서 FUTEX_WAIT/WAKE가 cross-process 공유 mmap word에 동작하는지(문헌상 허용, 실측 미확인).
- **U-REEXEC-CTOR:** ALR re-map 후 guest ld.so가 libalr_fakeroot.so ctor를 실제 재실행하고 $ALR_FAKEROOT_DB로 같은 파일 remap하는지(envp 통과는 코드 확인, ctor 재실행은 미실측).
- **U-MKNOD-ABORT(§2-a 각주)** / **U-DEB-NODE(§2-a 각주)**: §2 각주 참조.
- **U-STATX:** statx wrapper+overlay는 구현됨(host 빌드 통과)이나, (a) statx의 (dev,ino)/rdev major-minor → dev_t 역산이 device의 실제 glibc dev_t 비트레이아웃과 정확히 일치하는지, (b) noble dpkg/coreutils가 실제로 statx로 메타를 읽는 비중은 device 실측 잔여.
- **U-OVERFLOW:** 262144 슬롯이 base-system 전체 unpack inode 수에 충분한지 device 실측.
- **U-APT-SANDBOX:** apt가 getuid=0을 보면 sandbox drop(`_apt` seteuid)이 발동할 수 있고 후속 EPERM 유발 가능(setuid는 fakeroot no-op success로 흡수하나 후속 동작 영향 미확인). `apt`가 `--force-not-root`를 dpkg에 전달하는지/`Dpkg::Options`로 주입 가능한지도 미확정.

### 6-b. 영영 안 풀릴 수 있는 구멍 (백스톱)

- **raw-svc chown 구멍:** dpkg/maintainer가 libc wrapper를 우회해 **raw `svc`(inline syscall)로 chown**하면 LD_PRELOAD가 못 잡음. dpkg는 표준 glibc wrapper를 쓰므로 실무상 드물지만, 잡히면 supervisor(ptrace) 백스톱이 그 chown을 가로채 no-op+DB 기록해야 함 — 이는 **본체(supervisor) 협력 필요분**으로, fakeroot LD_PRELOAD만으로는 못 막음. ADR에 명시: 발견 시 본체 세션과 조율.
- **statx dev_t 역산:** statx overlay는 (dev,ino)를 major/minor에서 역조립하므로 glibc gnu_dev 비트레이아웃 가정에 의존한다(host 빌드 확인, device 실측 잔여 U-STATX). 만약 어떤 게스트가 statx만 쓰면서 dev_t 역산이 1비트라도 어긋나면 그 파일은 overlay miss(real owner 보고) — fail-safe(잘못된 fake owner를 주는 것보다 안전)지만 self-consistency는 깨질 수 있음.

### 6-c. fallback (영영 안 될 시)

fakeroot 경로가 device에서 끝내 unpacked=true를 못 주면, **v1 stage-tar 유지** — 패키지를 host에서 미리 unpack해 overlay tar로 배포하는 기존 방식(GIMP/GPU에서 검증된 패턴). fakeroot는 "in-process apt install"이라는 상위 목표용이며, 미달 시 사용자 가치(앱 동작)는 stage-tar로 보존됨. **즉 fakeroot는 편의·완성도 향상이지 기존 동작의 회귀 위험이 아니다**(ALR_REEXEC_INPROC처럼 opt-in 게이트로 둬 main 거동 불변 유지 권장).

---

## 7. host-buildable 요약 (실측)

- **빌드:** `zig cc --target=aarch64-linux-gnu.2.39 -shared -fPIC -O2 -Wall -Wextra -o libalr_fakeroot.so libalr_fakeroot.c` → **exit 0, 경고 0, 52KB aarch64 ELF**(zig 0.16.0, noble glibc 2.39 타겟). **36 FUNC export**(objdump -T 확인; stat-family 풀커버 = stat/lstat/fstat/fstatat/newfstatat + statx + *64 4종 + 구 __xstat/__lxstat/__fxstat/__fxstatat).
- **심볼 충돌:** interposer path 심볼과 교집합 0; 겹치는 **15 심볼**(cred getter 4: get[e][ug]id + chmod/fchmodat + mknod/mknodat + stat-family 7: fstatat/fstatat64/newfstatat/statx/__xstat/__lxstat/__fxstatat) 전부 fakeroot가 선두 win. 분류 unclassified=0(interposer 60 source-def ∩ fakeroot 36 export 교차계산 실측).
- **DB semantics:** `test_fakeroot_metadb.py` 10/10 + `test_fakeroot_db_shared.py` 3/3 PASS(pytest via uvx).
- **host 불가(device 필요):** 위 §6-a 전부 — cred priority 실측, fork-shared mmap 일관, RTLD_NEXT cross-.so 체인, dpkg --force-not-root 실 unpack, apt e2e.

**verdict: prototype-first / adopt.** mechanism·빌드·DB는 host로 풀림(증명 첨부). non-root unpack "풀림" 주장은 §6 DEVICE-REQ(특히 probe-I `unpacked=true`)가 device에서 PASS한 후에만.
