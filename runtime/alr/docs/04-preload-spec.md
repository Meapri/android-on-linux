# 04 — `libalr_preload.so` 정밀 스펙

구현 위치: `src/preload/`. 게스트 glibc ABI. 배치: `<R>/usr/lib/alr/libalr_preload.so`.

이 라이브러리가 **저오버헤드 주장이 실제로 사는 곳**이다. 여기서 100 ns를 잃으면 `git status`의 이득 전체가 사라진다 ([01-platform-facts.md §D1](01-platform-facts.md)).

## 1. 빌드

```sh
zig cc --target=aarch64-linux-gnu.2.17 \
       -shared -fPIC -O2 -D_GNU_SOURCE \
       -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
       -fno-stack-protector -fvisibility=default \
       -Wall -Wextra -Werror \
       -o libalr_preload.so src/preload/*.c
```

- **zig 버전을 정확히 핀**한다 (예: 0.16.0). `zig version`이 다르면 빌드 실패시킨다. 재현성 주장은 고정 버전에서만 참이다.
- **`-D_FORTIFY_SOURCE=0`은 필수다.** 켜져 있으면 이 라이브러리가 **자기가 정의한 `__*_chk` 심볼을 호출**해 무한 재귀에 빠진다.
- `.2.17` 타깃은 `stat`/`fstatat`(GLIBC_2.33+) **링크를 금지**한다. 이건 버그가 아니라 강제 장치다 — 인터포저는 stat 계열을 **호출**하면 안 되고 **정의**만 해야 한다 (§5.3).
- 산출물 옆에 `manifest.json`: `{zig_version, target, source_sha256, output_sha256}`.
- **CI 게이트**: `readelf -V libalr_preload.so`의 `DT_VERNEED`에 GLIBC_2.17 초과 버전이 없어야 한다.


> **재현 가능 빌드의 정확한 범위** — `MEASURED` 2026-08-04.
> `alr version` 이 찍는 sha256 이 값을 가지려면 **다른 사람이 같은 태그에서 같은 바이트를 다시 만들 수 있어야** 한다. 그게 성립하는 조건은 두 가지고, 둘 다 실측으로 확인했다.
>
> | 빌드 | sha256 | `.comment` |
> |---|---|---|
> | Homebrew zig (macOS) | `e46688bd…` | Homebrew clang 21.1.8 |
> | 공식 tarball, **오염된 캐시** | `6790d602…` | **Homebrew clang 21.1.8** (!) |
> | 공식 tarball, 깨끗한 캐시 (macOS) | `052e7410…` | clang 21.1.0 |
> | CI (공식 tarball, Linux) | `052e7410…` | clang 21.1.0 |
>
> **(1) `zig version` 핀만으로는 부족하다.** `0.16.0` 이라고 답하는 설치가 둘 이상이다 — Homebrew 것은 Homebrew LLVM 에 링크하고 공식 tarball 은 자기 것을 번들한다. 서로 다른 컴파일러니 서로 다른 바이트가 나오는 게 **맞다**. 그래서 매니페스트가 `cc_identity` 를 함께 기록한다. 컴파일러 정체 없이 인용된 해시는 남의 것과 맞춰 볼 수가 없다.
>
> **(2) zig 전역 캐시가 툴체인 사이에 샜다.** 위 표의 두 번째 줄이 위험한 줄이다 — **공식 컴파일러가 만든 바이너리에 다른 컴파일러의 정체가 찍혔다.** zig 는 compiler-rt 와 libc 스텁을 `$ZIG_GLOBAL_CACHE_DIR` 에 한 번 만들어 두고 기계 위의 모든 zig 가 공유하는데, 나중에 도는 쪽이 앞선 쪽의 오브젝트를 그대로 재사용한다. `build-preload.sh` 는 이제 캐시 디렉토리를 **zig 실행파일의 실제 경로 + 버전**으로 키를 잡는다 — compiler-rt 를 매번 다시 만들지 않으면서(속도 유지) 설치 간 재사용은 불가능하게.
>
> **게이트는 이걸 볼 수 없었다.** `PRELOAD REPRODUCIBLE` 은 자기 두 빌드에 각각 캐시를 주므로, 둘은 서로 일치하면서 **다른 모든 기계와 불일치**할 수 있었다. 그리고 두 번째 빌드가 **출력 경로만** 달랐어서 결정성만 증명하고 있었다. 이제 다른 이름의 디렉토리로 소스를 복사해 거기서 빌드한다.
>
> 이 표는 v0.4.0 을 **자기가 발행한 tarball 로 설치해 보다가** 나왔다. `alr version` 이 릴리스된 `.so` 와 여기서 빌드한 것 사이의 MISMATCH 를 정확히 잡아냈고, 그 기능이 아니었으면 아무도 몰랐다.

## 2. 절대 규칙

```
R1. malloc / free / calloc / realloc 를 호출하지 않는다. 스택 버퍼만.
R2. stat 계열을 호출하지 않는다. 정의만 한다.
R3. realpath 를 호출하지 않는다 (glibc 내부 lstat 순회가 자기 래퍼로 재귀).
R4. 재작성 경로에서 락을 잡지 않는다.
R5. 재작성 경로에서 어떤 syscall도 발행하지 않는다 (문자열 연산만).
R6. 경로 재작성 규칙은 src/common/alr_path_rule.h 에서만 온다. 복제 금지.
R7. 진단 출력은 ALR_LOG_FD 로만. stderr 를 오염시키지 않는다.
```

R1이 필요한 이유: 게스트 allocator 초기화 전에 래퍼가 호출될 수 있고, 게스트가 자기 malloc을 후킹했을 수도 있다 (jemalloc, Node의 allocator).

## 3. 초기화

```c
__attribute__((constructor(101)))
static void alr_ctor(void)
{
    g_root      = getenv("ALR_ROOT");          // 후행 슬래시 제거
    g_root_len  = strlen(g_root);
    g_guest_exe = getenv("ALR_GUEST_EXE");
    g_log_fd    = parse_fd(getenv("ALR_LOG_FD"));
    g_log_level = parse_int(getenv("ALR_LOG"));
    /* dlsym(RTLD_NEXT, …) 로 실제 심볼 주소 캐싱 — §4 */
    /* /dev/full 에뮬레이션 상태 초기화 */
    /* link2symlink 그림자 디렉토리 확인/생성 */
}
```

> ⚠️ **정정 (실측으로 반박됨).** 이 문장은 틀렸다. 생성자 우선순위는 **한 오브젝트 안의** `.init_array` 항목만 정렬한다. 크로스-DSO 순서는 로더가 정하고, glibc `_dl_init()`은 `l_initfini`를 내림차순으로 도는데 preload는 인덱스 1이라 **libc를 포함한 다른 모든 DSO보다 늦게** 초기화된다.
>
> 그동안 래퍼들은 이미 인터포즈 중이므로 `real_*`가 NULL인 창이 존재하고, 다른 라이브러리의 생성자가 그 창에서 래핑 심볼을 부르면 `main()` 전에 SIGSEGV로 죽는다. → **생성자에 의존하지 말고 최초 사용 시 초기화(`ensure_init()`)** 할 것. [RISKS R14](RISKS.md).

**`ALR_ROOT`가 없거나 절대경로가 아니면** 진단을 내고 **재작성을 전부 끄는 패스스루 모드**로 동작한다. 조용히 잘못된 경로를 만드는 것보다 낫다.

## 4. `dlsym(RTLD_NEXT)` 처리

```c
#define REAL(name) \
    (g_real_##name ? g_real_##name \
                   : (g_real_##name = dlsym(RTLD_NEXT, #name)))
```

**재귀 함정**: `dlsym`은 내부적으로 malloc과 경로 관련 호출을 할 수 있다. 대응:
- 생성자에서 **전부 미리 해석**해 둔다. 지연 해석은 첫 호출이 하필 초기화 중일 때 폭발한다.
- 그래도 `NULL`이면 **재작성 없이 원본 인자로** `syscall()` 직행 폴백. 죽지 않는다.
- 해석 중 재진입을 막는 thread-local `g_in_dlsym` 플래그.

## 5. 경로 재작성

### 5.1 핵심 함수 — 여기가 hot path다

```c
/* 반환: 재작성이 필요하면 buf, 아니면 원본 포인터 p (복사 없음) */
static inline const char *rw(const char *p, char buf[ALR_PBUF])
{
    if (!p)          return p;          // NULL 통과
    if (p[0] != '/') return p;          // ⚠️ 상대경로 = 재작성 불필요. 첫 바이트 검사가 전부
    if (!g_root_len) return p;          // 패스스루 모드
    if (g_rw_suppress) return p;        // realpath 내부 순회 중 (§5.3)

    /* ── 정규화가 먼저다. 순서를 바꾸면 sysdir 를 통한 탈출이 열린다 (아래 경고) ── */
    const char *q = p;
    if (needs_normalize(p)) {                       // '.' '..' '//' 후행 '/' 스캔
        if (!alr_normalize_guest_abs(p, buf))       // src/common/alr_path_rule.h
            { errno = ENAMETOOLONG; return NULL; }  // '..' 는 루트에서 클램프됨
        q = buf;                                    // 정규화 결과는 buf 에 있다
    }

    if (is_sysdir(q))          return (q == buf) ? buf : p;   // /proc /sys /dev → 통과
    if (already_under_root(q)) return (q == buf) ? buf : p;   // 멱등성 가드

    size_t n = strlen(q);
    if (g_root_len + n >= ALR_PBUF) { errno = ENAMETOOLONG; return NULL; }
    memmove(buf + g_root_len, q, n + 1);            // q 가 buf 를 가리킬 수 있으므로 memmove
    memcpy(buf, g_root, g_root_len);
    return buf;
}
```

> ⚠️ **정규화는 sysdir/멱등성 검사보다 먼저 와야 한다.** 두 검사는 컴포넌트 경계 접두사 매칭이라 정규화되지 않은 입력에도 발동한다. 순서가 반대면:
> - `"/proc/../etc/passwd"` → `is_sysdir` 매치 → 재작성 없이 통과 → 호스트 커널이 **Android의 `/etc/passwd`로 해석**. 게스트가 호스트 파일을 조용히 읽는다.
> - `"<R>/../x"` → `already_under_root` 매치 → 같은 방식으로 rootfs 밖으로 탈출.
>
> `..`를 루트에서 클램프하는 것이 [02-architecture.md §7.1](02-architecture.md)의 "이스케이프가 구조적으로 불가"를 성립시키는 유일한 근거이므로, 이 순서가 그 주장의 전제다.

> **`needs_normalize()` 빠른 경로**: 대부분의 경로는 이미 정규형이다. `.`/`..`/`//`/후행 `/`가 없으면 정규화를 건너뛰고 `q = p`로 간다. 이 스캔은 `strlen`과 같은 한 번의 선형 통과이므로 예산 안이다.

**성능 계약 (예산 ≤ 100 ns):**
- `p[0] != '/'` 검사가 **가장 먼저**. git은 `openat(dirfd, 상대경로)`를 점점 더 쓰고, 상대경로는 재작성이 필요 없다. 이 한 줄이 `git status` 호출의 상당 비율을 즉시 반환시킨다.
- **캐시를 넣지 말 것.** `memcmp` + `memcpy`보다 비싼 조회는 순손실이다 ([§D1](01-platform-facts.md)). 상위 프로젝트의 cold 4,334 ns/op 변환기는 예산의 40배다.
- **네거티브 경로가 포지티브만큼 싸야 한다.** `.gitignore` 프로브 때문에 디렉토리당 ~2회의 대부분 ENOENT인 open이 발생한다.
- **`ALR_PBUF = PATH_MAX` (4096).** 가드 `g_root_len + n >= ALR_PBUF`가 있으면 최대 기록량이 `g_root_len + n + 1 <= 4096`이므로 4096이 정확히 충분하다.
  > ⚠️ 2304 같은 값을 쓰면 **커널이 받아들일 정상 경로를 거부한다.** `<R>`이 `/data/data/com.termux/files/usr/var/lib/alr/distros/ubuntu-24.04`(~62바이트)일 때 게스트 경로가 ~2240바이트만 넘으면 `ENAMETOOLONG`이 뜬다. 깊은 pnpm/node_modules 스토어, 중첩 모노레포, dpkg 언팩 트리가 2 KB를 일상적으로 넘는다. 증상은 "파일을 못 찾음"으로 보여 추적이 어렵다.
  > 두 개의 경로 인자를 갖는 래퍼(`renameat`, `renameat2`, `linkat`)는 버퍼를 두 개 쓰므로 프레임이 8 KB가 된다 — R1(malloc 금지, 스택만) 아래에서 이것이 상한이다. 그 이상 늘리지 말 것.

### 5.2 통과 규칙

| 접두사 | 동작 | 이유 |
|---|---|---|
| `/proc`, `/sys`, `/dev` | 통과 (단 §7의 가상화 대상 제외) | 커널 가상 파일시스템. rootfs로 리다이렉트하면 안 된다 |
| 이미 `<R>` 아래 | 통과 | 멱등성. 이중 접두사 방지 |
| 상대경로 | 통과 | dirfd 기준으로 커널이 해석 |

**컴포넌트 경계 매칭**을 쓴다: `/proctology`는 `/proc`이 아니다. `path_under(p, "/proc")`는 `p[5]`가 `\0` 또는 `/`인지 확인해야 한다.

### 5.3 역변환 (`guest_canon`)

호스트 경로를 **반환**하는 함수는 게스트 경로로 되돌려야 한다. `<R>` 접두사를 벗기고, 벗긴 결과가 빈 문자열이면 `/`.

적용 대상: `getcwd`, `realpath`, `readlink`(rootfs 내부 심링크가 절대 타깃일 때), `ttyname_r`.

**`realpath` 특수 처리** (R3과 관련):
1. 입력 경로를 **한 번** 재작성
2. thread-local `g_rw_suppress = 1` 설정 → glibc의 내부 lstat 순회 동안 `rw()`가 아무것도 하지 않음
3. `REAL(realpath)` 호출
4. `g_rw_suppress = 0`
5. 결과에서 `<R>` 접두사 제거

이 순서를 지키지 않으면 apt-key의 키링 경로와 gpgv가 서로 다른 경로를 보게 된다 (상위 프로젝트가 디바이스에서 겪은 버그).

## 6. 래퍼 심볼 표

**`src/preload/wrappers.def`를 단일 정본으로 둔다.** C 소스, CI 심볼 존재 테스트, 문서가 모두 여기서 생성된다. 세 곳이 어긋나는 것을 막는 유일한 방법이다.

형식: `ALR_WRAP(반환형, 이름, 시그니처, path_arg_index, flags)`

> **이 표는 기억이 아니라 게이트가 지킨다.** `bind`/`connect` 는 프로젝트 존속 기간 내내 빠져 있었고 아무것도 그걸 가리킬 수 없었다 — 커버리지 근거가 *"우리가 떠올린 것들은 감쌌다"* 였기 때문이다. 그건 근거가 아니라 **누가 무엇을 기억했는지의 목록**이다.
> `scripts/check-path-coverage.sh` 가 경로 인자를 받는 libc 진입점 목록을 `.so` 의 심볼 테이블과 대조한다. 첫 실행에서 한 번에 나온 것: xattr 8개, `inotify_add_watch`, `pathconf`, `glob`/`glob64`, `ftw`/`ftw64`, `nftw`/`nftw64`, `getsockname`/`getpeername`/`accept4`.
> 게이트는 심볼이 **export 됐는지**만 증명한다. 재작성이 맞는지는 실기기의 `tests/device/probe_pathcov.c` 만 증명한다. **둘은 한 쌍이고 어느 하나로는 부족하다.**
> 인터포즈하지 **않는** 항목도 이유와 함께 같은 파일에 적는다. 첫 실행이 저자가 `skip` 으로 추론해 넣은 다섯 개(`setmntent`, `pivot_root`, `name_to_handle_at`, `tmpnam`, `tempnam`)를 전부 반려했다 — 코드는 이미 이유를 갖고 인터포즈하고 있었다.

### 6.1 open 계열
`open`, `open64`, `openat`, `openat64`, `creat`, `creat64`, `__open_2`, `__open64_2`, **`__openat_2`**, **`__openat64_2`**, `fopen`, `fopen64`, `freopen`, `freopen64`

### 6.2 stat 계열 — **양쪽 이름 모두 정의** (§1)
`stat`, `stat64`, `lstat`, `lstat64`, **`fstat`**, **`fstat64`**, `fstatat`, `fstatat64`, `newfstatat`, `statx`, `statvfs`, `statvfs64`, `statfs`, `statfs64`
**그리고 pre-2.33 이름**: `__xstat`, `__xstat64`, `__lxstat`, `__lxstat64`, `__fxstat`, `__fxstat64`, `__fxstatat`, `__fxstatat64`

> `fstat`/`fstat64`는 경로 인자가 없어(`path_arg_index = -1`) 재작성과 무관하지만 **link2symlink에 필수다**(§8.2). 빠뜨리면 `stat(path)`는 `st_nlink=2`를 보고하는데 같은 파일의 `fstat(fd)`는 1을 보고해 dpkg 무결성 검사가 깨진다. Ubuntu 24.04(glibc 2.39) 바이너리는 `__fxstat`이 아니라 `fstat`을 호출하므로 pre-2.33 이름만으로는 못 잡는다.

> 두 세트를 다 정의해야 어느 배포판에서 빌드된 게스트 바이너리든 잡는다. 정의는 링크타임 스텁이 필요 없어 `.2.17`에서 비용 0이다.

### 6.3 access 계열
`access`, `eaccess`, `euidaccess`, `faccessat`, `faccessat2`

### 6.4 링크/심링크
`readlink`, `readlinkat`, `__readlink_chk`, `__readlinkat_chk`, `symlink`, `symlinkat`, **`link`**, **`linkat`** (→ §8 link2symlink)

### 6.5 디렉토리
`opendir`, `scandir`, `scandir64`, `mkdir`, `mkdirat`, `rmdir`, `chdir`, `getcwd`, **`__getcwd_chk`**, `getwd`, **`__getwd_chk`**, `get_current_dir_name`

### 6.6 삭제/이동
`unlink`, `unlinkat`, `remove`, `rename`, `renameat`, `renameat2`

### 6.7 메타데이터
`chmod`, `fchmodat`, `chown`, `lchown`, `fchownat`, `utime`, `utimes`, `lutimes`, `utimensat`, `truncate`, `truncate64`, `mknod`, `mknodat`, `mkfifo`, `mkfifoat`, `name_to_handle_at`, `pathconf`

**xattr 계열** (경로 인자 0): `getxattr`, `lgetxattr`, `setxattr`, `lsetxattr`, `listxattr`, `llistxattr`, `removexattr`, `lremovexattr`

> 이미지에 xattr 이 하나도 없으니(§A6) 필요 없어 보이지만, 증상은 xattr 이 아니라 **에러**로 나온다. coreutils 의 `ls` 는 ACL 이 없을 때가 아니라 **ACL 조회가 실패했을 때** 모드 뒤에 `?` 를 찍는다. 재작성 안 된 경로의 `getxattr` 은 맨 ENOENT 다.
> `MEASURED` 2026-08-03: `drwx------? 6 alr alr … /root` → 수정 후 `drwx------.`
> 같은 호출이 `cp -a`, `tar --xattrs`, `rsync -X`, `getfacl`, `install -Z` 밑에 있다.

**inotify**: `inotify_add_watch` (경로 인자 1)

> 파일 감시자 전부 — node 의 `fs.watch`/chokidar(즉 **모든 JS 개발 서버**), `inotifywait`, `entr`, watchman. 틀린 경로를 감시하면 **에러가 나지 않고 그냥 영원히 안 터진다.** G5 워크로드에 직접 걸린다.

### 6.8 exec 계열 — **하나라도 빠지면 그 자식이 죽는다** (§9)
`execve`, `execveat`, `execv`, `execvp`, `execvpe`, `execl`, `execlp`, `execle`, `fexecve`, `posix_spawn`, `posix_spawnp`, `system`, `popen`

### 6.9 경로 반환
`realpath`, `canonicalize_file_name`, `__realpath_chk`, `ttyname`, `ttyname_r`, **`__ttyname_r_chk`**

### 6.10 방어 (SIGSYS 사망 방지)
`mount`, `umount`, `umount2`, `chroot`, `pivot_root` → 재작성 없이 즉시 `errno = EPERM; return -1`
> [§B4](01-platform-facts.md): 이것들은 seccomp `RET_TRAP`이라 실제로 부르면 "Bad system call"로 프로세스가 죽는다.

### 6.11 raw syscall 대응 — **Node 워크로드 필수**
`syscall` (§10)

### 6.12 터미널
`ioctl` (§11)

### 6.13 동적 로딩 — **Node 네이티브 애드온 / Python C 확장 필수**

`dlopen` (경로 인자 0), `dlmopen` (경로 인자 **1** — `void *dlmopen(Lmid_t lmid, const char *path, int flags)`)

절대 게스트 경로의 `dlopen`은 `--library-path`가 해결해 주지 않는다. 요청이 ld.so로 가는데 그것이 유일한 인터포즈 불가 컴포넌트다([§C6](01-platform-facts.md)) → 게스트 경로가 Android 호스트 루트를 때리고 실패한다. 정확히 타깃 워크로드가 깨진다: Node 네이티브 애드온(`process.dlopen` → `uv_dlopen` → `dlopen("/…/node_modules/x/build/Release/x.node")`), 모든 Python C 확장 import, PAM/NSS 플러그인 로드.

**손으로 쓰는 래퍼가 필요하다** (단순 `ALR_WRAP` 항목으로 불충분):

1. **재작성 조건 — 전부 만족할 때만**: `path != NULL` && `path[0] == '/'` && 문자열에 `$`가 없음 && `rw()`의 sysdir/멱등성 규칙 통과.
   `$ORIGIN`/`$LIB`/`$PLATFORM`은 **절대 접두사를 붙이면 안 된다** — `expand_dynamic_string_token`이 로더 맵 기준으로 전개한다. 나머지(NULL, 슬래시 없는 soname, 상대경로, `$` 토큰 경로)는 그대로 통과.
2. **호출자 보존**: glibc `dlfcn/dlopen.c`가 `RETURN_ADDRESS(0)`로 호출 객체를 식별한다. 평범한 `REAL(dlopen)(...)` 호출은 호출자를 preload 자신으로 바꿔 버려 `$ORIGIN` 전개와 RPATH 해석이 틀어진다. 래퍼는 `__builtin_return_address(0)`를 넘기는 `dlopen`의 내부 진입점을 쓰거나, 그것이 불가능하면 이 한계를 문서화한다.

### 6.14 임시 파일 — **apt/dpkg/git 필수**

**Group A — 템플릿을 제자리 수정 (arg 0 = IN_OUT)**:
`mkstemp`, `mkstemp64`, `mkstemps`, `mkstemps64`, `mkostemp`, `mkostemp64`, `mkostemps`, `mkostemps64`, `mkdtemp`

**Group B — 경로 반환**: `tmpnam`, `tmpnam_r`, `tempnam` (결과에 `guest_canon` 적용)

**Group C — 경로 없음**: `tmpfile`, `tmpfile64` (내부 `TMPDIR` 사용 — 게스트 env의 `TMPDIR=/tmp`가 재작성되므로 대개 자동 동작하나 테스트할 것)

이들은 `open` 래퍼로 커버되지 **않는다**. glibc가 `__gen_tempname`에서 구현하고 그 함수는 내부 hidden alias `__open`/`__mkdir`을 호출하는데, 이는 libc 내부 바인딩이라 `LD_PRELOAD`가 가로챌 수 없다. 결과적으로 `mkstemp("/tmp/apt.XXXXXX")`가 Android 호스트의 `/tmp`(존재하지 않음)에 발행되어 `ENOENT`. **모든 path syscall 래퍼가 갖춰져 있어도 G2(`apt install`)가 깨진다.**

Group A 래퍼 형태 — 수정된 템플릿을 되돌려 써야 한다:
```c
int mkstemp(char *tmpl)
{
    char hbuf[ALR_PBUF];
    const char *h = rw(tmpl, hbuf);
    if (!h) return -1;                                  /* rw() 가 ENAMETOOLONG 설정 */
    int fd = REAL(mkstemp)((char *)h);
    if (h == hbuf)                                      /* 재작성된 경우에만 */
        memcpy(tmpl, hbuf + g_root_len, strlen(tmpl));  /* 채워진 XXXXXX 를 복사 */
    return fd;
}
```
`h == hbuf` 검사가 핵심이다 — `rw()`는 상대경로/sysdir/이미-rootfs-아래인 경우 **원본 포인터를 그대로 반환**하므로 그때는 되돌려 쓸 것이 없다.

### 6.15 fd 상태 (→ §12 `/dev/full`) — ⛔ **구현하지 않음**

> 아래 열두 개는 `src/preload/wrappers.def` 의 `DELIBERATELY NOT IMPLEMENTED` 블록에 있다. [§12](#12-devfull-에뮬레이션) 의 정정을 볼 것. 커버리지 감사에서 **누락 심볼로 세지 말 것.**

`write`, `pwrite`, `pwrite64`, `writev`, `pwritev`, `pwritev2`
그리고 stdio 표면: `fwrite`, `fputs`, `fputc`, `fprintf`, `vfprintf`, `fflush`

`read`/`lseek`/`fstat`/`close`는 §12가 `/dev/zero` 백킹을 쓰므로 래퍼가 필요 없다.

### 6.16 심링크 타깃 상대화 — **apt/dpkg 필수**

`symlink`, `symlinkat`

경로 인자(`linkpath`)는 §4 규칙대로 재작성한다. 그런데 **타깃(`target`)은 경로 인자가 아니라 링크의 *내용*이다.** 여기에 호스트 경로를 넣으면 rootfs 이미지에 설치 위치가 박히므로 재작성해서는 안 된다 — 하지만 절대 경로 그대로 두는 것도 똑같이 깨진다: **커널이 심링크 내용을 실제 호스트 루트 기준으로 푼다.** 이 해석은 커널 안에서 일어나 어떤 libc 훅보다 아래에 있다.

유일하게 살아남는 표현은 **상대 경로**다. 커널이 링크 자신의 디렉토리 기준으로 풀고, 그 디렉토리는 이미 rootfs 안이다.

| 입력 | 저장되는 타깃 |
|---|---|
| `symlink("/etc/os-release", "/tmp/x")` | `../etc/os-release` |
| `symlink("/etc/os-release", "/usr/local/bin/x")` | `../../../etc/os-release` |
| `symlink("/proc/self/exe", "/tmp/x")` | `/proc/self/exe` — **sysdir 은 절대 유지** |
| `symlink("../etc/hostname", "/tmp/x")` | `../etc/hostname` — 이미 상대, 무변경 |

깊이는 링크의 **게스트** 경로에서 계산한다. `symlinkat(dfd, 상대경로)` 는 `/proc/self/fd/N` 을 되읽어 게스트 경로를 복원한다. **복원에 실패하면 타깃을 건드리지 않는다** — 깊이를 틀리게 잡는 것이 절대 링크보다 나쁘다.

`/proc`·`/sys`·`/dev` 로 향하는 타깃을 상대화하면 안 되는 이유는 이들이 §4에서 재작성 대상이 아니기 때문이다. 절대 경로가 이미 정답이다.

> 이 항목이 없으면 apt 의 `/tmp/apt-dpkg-install-XXXXXX` 스테이징 디렉토리가 깨진 링크로 채워지고, `dpkg-deb: cannot stat pathname …` 으로 설치가 실패한다. ([M10 증거](evidence/2026-08-02-m10-apt-install-git.md) §1)

### 6.17 glibc 내부 진입점 우회 — **shadow/NSS 필수**

이 절의 심볼들은 공통점이 하나다: **glibc 내부에서 리터럴 `/etc/...` 경로를 내부 전용 호출로 연다.** `__open64_nocancel`, `__nss_files_fopen` 같은 것들은 공개 심볼이 아니라 `LD_PRELOAD` 가 닿지 못한다. 그래서 그 경로는 Android 의 `/etc` → **읽기 전용 `/system/etc`** 로 간다.

원칙은 §6.14 의 `mkstemp`/`__open` 과 같다: **내부 진입점은 못 잡아도 공개 진입점은 잡을 수 있다.** 공개 API 를 통째로 가져와 우리가 직접 파일을 연다.

**(a) 암호 파일 잠금** — `lckpwdf`, `ulckpwdf`

shadow 의 `commonio_lock()` 은 자체 잠금 파일보다 **먼저** `lckpwdf()` 를 부르고, 실패하면 바로 포기한다. glibc 는 그 안에서 리터럴 `/etc/.pwd.lock` 을 연다. 우리가 `rw()` 를 거쳐 rootfs 쪽을 열고 `fcntl(F_SETLK)` 로 잠근다. glibc 는 `alarm(15)`+`SIGALRM` 로 대기를 제한하지만 **preload 가 게스트 몰래 시그널 핸들러를 설치해서는 안 되므로** 같은 상한을 재시도로 구현한다.

없으면: `groupadd: cannot lock /etc/group; try again later.` — `/etc` 에 파일이 하나도 안 생긴 채 실패한다.

**(b) NSS `files` 백엔드** — passwd/group 전체

glibc 2.34+ 는 `files` 백엔드를 libc 에 내장했고, 리터럴 `/etc/passwd`·`/etc/group` 을 `__nss_files_fopen` 으로 연다. 보정하지 않으면 **게스트의 모든 이름 조회가 폰의 89바이트 AID 테이블에서 답을 받는다.**

| 구분 | 심볼 |
|---|---|
| 조회 | `getpwnam` `getpwuid` `getgrnam` `getgrgid` |
| 재진입 | `getpwnam_r` `getpwuid_r` `getgrnam_r` `getgrgid_r` |
| 열거 | `setpwent` `getpwent` `endpwent` `setgrent` `getgrent` `endgrent` |
| 보조 | `getgrouplist` |

`files` 는 여기서 동작할 수 있는 **유일한** 소스다 — `nss_systemd`·`nss_ldap` 은 게스트가 띄우지 않는 데몬을 요구한다.

의도된 한계: 그룹당 멤버 **64명** 상한(초과 시 `ALR_LOG` 에 기록하고 절단 — 조용한 절단이 아니다), `initgroups()` 미구현(`setgroups` 가 seccomp 로 막혀 애초에 불가능).

**(d) 경로 순회 API** — `glob`, `glob64`, `ftw`, `ftw64`, `nftw`, `nftw64`

`glob()` 은 자기 **내부** `opendir`/`lstat` 로 패턴을 걷는다. PLT 를 거치지 않으므로 `LD_PRELOAD` 가 못 본다.
> `MEASURED` 2026-08-03: 게스트에서 `glob("/etc/os-relea*")` → `GLOB_NOMATCH`. Android 의 `/etc` 를 뒤졌다.

**패턴만 재작성해서는 안 된다.** `glob()` 은 결과를 *패턴의 리터럴 접두사 + 디렉토리 엔트리*로 조립하므로, 패턴을 재작성하면 `<R>` 접두사가 붙은 경로 — **게스트가 이름 붙일 수 없는 주소의 올바른 매치** — 를 돌려준다. 모든 결과를 `guest_canon` 으로 되돌린다. 이게 `GLOB_NOCHECK`(매치 없으면 패턴 자체를 반환)와 `GLOB_APPEND`(이미 게스트 형식인 항목은 그대로 두므로 멱등) 도 함께 덮는다.

`GLOB_ALTDIRFUNC` 는 손대지 않고 통과시킨다 — 호출자가 자기 `opendir`/`lstat` 을 준 것이라 glibc 가 만드는 경로는 커널이 아니라 **그쪽 코드로** 간다.

`ftw`/`nftw` 는 여기에 하나가 더 붙는다: **콜백이** 각 경로를 받는다. 스레드 로컬에 사용자 콜백을 저장하고 트램폴린을 넘겨 `guest_canon` 을 거친 경로를 전달한다. `struct FTW.base` 는 `fpath` **안의 바이트 오프셋**이라 잘려나간 만큼 같이 옮겨야 한다. 저장/복원을 실제 호출 바깥에 두므로 콜백이 다시 `nftw()` 를 불러도 동작한다.

**알려진 한계**: `GLOB_TILDE`. glibc 가 우리 재작성 *뒤에* `~` 를 내부에서 전개하고 같은 내부 호출로 걷는다. 쫓을 가치가 없다 — 틸드를 쓰는 셸은 호출 전에 자기가 전개한다.

**coreutils 는 여기 해당 없다.** `du`, `rm -r`, `chmod -R`, `cp -r`, `find` 는 gnulib 의 `fts` 를 들고 다니고 그건 PLT 를 거친다.
> `MEASURED`: `du -sh /etc` 가 게스트의 2.2M 을 보고하고, mkdir -p / chmod -R / cp -r / rm -rf 왕복이 깨끗하다.

**(c) audit 소켓** — `socket`

`socket(AF_NETLINK, *, NETLINK_AUDIT)` 를 **`EPROTONOSUPPORT`** 로 응답한다. shadow 의 `audit_help_open()` 은 `EINVAL`/`EPROTONOSUPPORT`/`EAFNOSUPPORT` 만 "커널에 audit 없음" 으로 보고 계속 진행하고, 그 외 errno 는 하드 에러로 취급해 즉시 종료한다. Android 는 SELinux 로 `EACCES`/`EPERM` 를 주므로 보정이 필요하다. 다른 도메인·프로토콜은 그대로 통과시킨다.

없으면: `Cannot open audit interface - aborting.`

### 6.18 AF_UNIX 소켓 주소 — **모든 게스트 서버**

`bind`, `connect` (들어가는 쪽) / `getsockname`, `getpeername`, `accept`, `accept4` (나오는 쪽)

`sockaddr_un.sun_path` 는 **경로다.** 프로젝트 존속 기간 내내 아무도 인터포즈하지 않았고, 그래서 게스트가 `bind("/tmp/tmux-10297/default")` 하면 **Android 쪽** 경로에 붙었다. tmux 가 눈에 보이는 피해자였다 — 디렉토리를 만들고 게스트에서 보이는 것까지 확인해도 실패했다. **디렉토리는 재작성되고 주소는 안 됐기 때문이다.** 부류는 훨씬 넓다: dbus, gpg-agent, ssh-agent, X11, `/var/run/<db>/.s.*` 아래 모든 DB.

**들어가는 쪽에서 절대 하면 안 되는 두 가지**
- **추상 소켓 재작성 금지.** `sun_path[0] == '\0'` 는 파일시스템에 존재하지 않는 Linux 추상 네임스페이스다. 접두사를 붙이면 **다른 주소를 발명하는 것**이고, `"\0alr-probe"` 로 합의한 두 프로세스가 서로를 못 찾게 된다.
- **절단 금지.** `sun_path` 는 108바이트고 루트 접두사가 그중 ~57바이트다. 안 들어가면 `ENAMETOOLONG` 으로 실패해야 한다 — 조용히 잘린 주소는 **틀린 경로의 소켓**이 되어 반쯤 동작한다.

**나오는 쪽**은 `getcwd` 와 같은 이유로 되돌린다. 커널이 재작성된 `sun_path` 를 그대로 돌려주므로, 자기가 어디 묶였는지 묻는 게스트는 Android 경로를 배운다.
> `MEASURED` 2026-08-03: `getsockname` → `/data/data/com.termux/.../tmp/x.sock`, 원하는 값 `/tmp/x.sock`
> 프로그램은 이 경로를 **출력하고, 락 파일과 pid 파일에 쓰고, 다른 프로세스에 넘긴다.**

> ⛔ **preload 내부에서 소켓을 여는 코드는 `real_bind`/`real_connect` 를 써야 한다.** `rb_connect()`(리졸버 브리지)가 평범한 `connect()` 를 부르고 있었고, 그건 같은 번역 단위의 **우리 래퍼**로 바인딩된다. `ALR_RESOLV_SOCK` 은 호스트 경로라 재작성되면서 브리지가 통째로 끊겼다 — 증상은 `dig -v` 의 `free(): invalid pointer` 였다([RISKS R15](RISKS.md)). 규칙: **alr 자신의 경로는 `rw()` 를 통과시키지 않는다.**

`accept(2)` 자체가 zygote 필터에 막혀 있다는 것은 별개의 사실이다 — [§A6](01-platform-facts.md) 참조. preload 가 `accept4(f,a,l,0)` 로 구현한다.

## 7. `/proc` 가상화

| 경로 | 답 | 필요 이유 |
|---|---|---|
| `/proc/self/exe`, `/proc/<자기 pid>/exe`, `/proc/thread-self/exe` | `ALR_GUEST_EXE` 값 | **하드 요구사항.** Node의 `process.execPath`가 `readlink("/proc/self/exe")`이고, npm/npx/corepack이 그걸로 재spawn한다. 보정 안 하면 "Node 버그처럼 보이는" 방식으로 깨진다 ([§C3](01-platform-facts.md)) |
| `/proc/self/cmdline` | 합성 (게스트 argv, NUL 구분) | git, ps류, 크래시 리포터가 읽는다. ld.so 트릭이 그대로 노출된다 |
| `/proc/self/root` | `/` | |
| `/proc/self/cwd` | 게스트 cwd | |
| `/proc/mounts`, `/proc/self/mounts`, `/proc/self/mountinfo` | 합성 마운트 테이블 | 호스트 백킹스토어 노출 금지. **구현됨** — `open`/`openat`/`fopen`/`setmntent` 에서 게스트 `/tmp` 의 unlink 된 파일로 materialize |
| `/proc/self/status` `Name:`, `/proc/self/comm` | 게스트 바이너리 basename | **구현됨** — 파일 합성이 아니라 `prctl(PR_SET_NAME)`. 모든 프로세스가 로더로 exec 되므로(ADR 0002) 커널이 `ld-linux-aarch64` 를 태스크 이름으로 기록하고, `ps`/`top`/`htop` 과 자기 이름을 되읽는 프로그램에 그대로 노출된다. prctl 한 번이 세 뷰를 동시에 고친다 |
| `/proc/self/status` 나머지 | 통과 (fakeroot 시 `Uid:`/`Gid:` 0으로 패치) | |

**인터포즈 지점**: `readlink`, `readlinkat`, `open`, `openat`, `fopen`, `setmntent`, `stat`, `lstat`, `realpath`.

`setmntent` 가 목록에 있는 이유는 §6.17 과 같다 — glibc 의 `setmntent` 는 내부 `fopen` 별칭을 쓰므로 우리 `fopen` 래퍼를 거치지 않는다. `df` 가 정확히 그 경로로 `/proc/mounts` 를 읽는다.

**호스트 경로 누출 가드**: 합성 마운트 테이블의 source/options 컬럼에 `<R>`, `/data/`, `/system`, `/vendor`, `/apex`, `/storage`, `/mnt/` 가 나타나면 거부한다.

**`/proc/self/cmdline` 구현 주의**: 이건 `read()`이지 `readlink()`가 아니다. 합성 내용을 담은 임시 파일을 materialize하고 `open`을 그쪽으로 리다이렉트하거나, `memfd`를 쓴다.

## 8. link2symlink

`link(2)`가 앱 데이터에서 `EACCES`로 실패한다 ([§B6](01-platform-facts.md)). **errno가 `EACCES`이지 `EPERM`/`EXDEV`가 아니라는 점이 중요하다** — 게스트의 폴백 코드는 대개 `EXDEV`만 잡으므로 발동하지 않는다.

깨지는 것: `dpkg -i`(하드링크 tar 멤버), `git clone --local`(객체 하드링크), `pnpm`(콘텐츠 주소 저장소 전체).

> ⛔ **정정 (2026-08-03, [ADR 0004](adr/0004-link2symlink.md)).** 아래 §8.1 이 적은 **shadow 디렉토리 방식은 채택되지 않았다.** 실제로 구현된 것은 그 ADR 이 대안으로 검토했던 **copy + `lid_record`** 쪽이다. §8.2 의 함께-인터포즈 목록과 §8.4 의 테스트 매트릭스는 그대로 유효하다.
>
> `alr` 은 게스트에서 `link(2)` 가 되는지 먼저 보고(§8.3 이 "무조건 켜짐" 이라 적은 것은 정확하지 않다), 안 될 때만 이 계층이 동작한다 — 수용 시험의 `PRELOAD LINK FALLBACK` 이 그 경로다.

### 8.1 알고리즘

`link(old, new)` 요청 시:
1. `old`가 아직 그림자화되지 않았으면: `old`를 `<R>/.alr/l2s/<n>`로 **rename**하고, `old` 자리에 그 그림자를 가리키는 심링크를 만든다. 링크 카운트 = 1로 기록.
2. `new` 자리에 같은 그림자를 가리키는 심링크를 만든다. 링크 카운트 += 1.
3. 그림자 인덱스와 카운트를 메타DB(fakeroot DB와 동일 mmap 방식 또는 별도)에 기록.

### 8.2 함께 인터포즈해야 하는 것 — 빠뜨리면 일관성이 깨진다

- `stat`/`lstat`/`statx`: 그림자 심링크에 대해 **`st_nlink`를 기록된 카운트로**, `st_ino`/`st_dev`를 그림자의 것으로 보고. 그리고 `lstat`이 심링크가 아니라 **일반 파일로 보이게** 해야 한다 (dpkg가 심링크를 다르게 취급한다).
- **`fstat`/`fstat64`**: fd는 `open()`이 이미 심링크를 따라간 뒤라 `st_dev`/`st_ino`는 그림자의 것이라 이미 올바르다. **`st_nlink`만** 보정한다. 빠뜨리면 `stat(path)`는 2를, 같은 파일의 `fstat(fd)`는 1을 보고한다.
- `readlink`/`readlinkat`: 그림자 심링크에 `EINVAL` (일반 파일인 척).
- **`readdir`/`readdir64`/`readdir_r`/`readdir64_r`**: 그림자 항목의 `d_type`이 `DT_LNK`로 나와 위의 `lstat` 뷰와 모순된다. l2s 그림자로 해석되는 항목은 **`d_type`을 `DT_REG`로 바꾼다**.
- **`scandir`/`scandir64`**: glibc의 `scandir`는 내부 hidden alias `__readdir`을 호출하므로 **`readdir` 인터포즈가 커버하지 못한다.** 별도로 같은 `d_type` 보정을 건다.
- `unlink`/`unlinkat`: 카운트 감소. 0이 되면 그림자 삭제.
- `rename`/`renameat`: 그림자 참조 유지.
- `open`: 심링크를 따라가므로 대개 그냥 동작하지만 `O_NOFOLLOW` 케이스 확인.

> **`d_type` 보정을 빠뜨리면**: `d_type`을 믿고 `lstat`을 부르지 않는 도구들이 다른 파일시스템을 본다. `find -type f`가 모든 하드링크 파일을 놓치고(pnpm 스토어, dpkg 언팩 트리), `find -type l`이 그것들을 심링크로 나열하며, gnulib fts 사용자(`cp -r`, `du`, `chmod -R`)가 심링크 분기를 탄다. 특히 git의 `dir.c`가 readdir의 `DT_LNK`를 쓰므로 `git add`가 심링크 경로로 가서 `readlink`(위에서 `EINVAL`로 만든)에 걸려 설명 불가능하게 실패한다.
>
> raw `syscall(SYS_getdents64, ...)`는 반환된 `struct linux_dirent64` 레코드를 순회하며 `d_type`을 제자리 수정하는 **사후 처리**가 필요하다 (§10의 단순 인자 재작성 모델로는 부족한 유일한 케이스).

### 8.3 활성화

`alr doctor` P6이 `link(2)`를 실제로 테스트한다. **참조 기기 2대 모두 `EACCES` 였고**, 지원 대상(Android 16, [ADR 0007](adr/0007-android-16-only.md))에서 이 조건이 뒤집힐 기기는 없다. 따라서 이 계층은 **무조건 켜짐**이다.

> ⚠️ 여기 적혀 있던 런타임 스위치(`state/<name>/doctor.json` 의 `link2symlink: true|false`)는 **구현된 적이 없다.** 코드에서 그 파일을 읽거나 쓰는 곳이 0건이고, `alr doctor` 는 리포트를 찍을 뿐이다. 이제 필요도 없으므로 구현하지 않고 내린다 — 없는 안전장치를 있다고 적어 두는 것이 실제 위험이다.

### 8.4 테스트 매트릭스 (필수)

`dpkg -i` (하드링크 멤버 포함 .deb) / `git clone --local` / `pnpm install` / `find -samefile` / `ln a b && stat -c %h a`

## 9. exec 재디스패치

[02-architecture.md §5.2](02-architecture.md)의 알고리즘을 §6.8의 **13개 함수 전부**에서 구현한다.

### 9.1 공통 경로

```c
static int alr_exec_common(const char *guest_path, char *const argv[],
                           char *const envp[], int flags)
{
    char hbuf[ALR_PBUF];
    const char *host = rw(guest_path, hbuf);
    if (!host) return -1;                       // errno 설정됨

    /* 1. 분류: 첫 256바이트 읽기 */
    enum { ELF_DYN, ELF_STATIC, SHEBANG, UNSUPPORTED } kind = classify(host);

    /* 2. envp 재주입 (멱등) */
    char **env2 = alr_inject_env(envp);         // LD_PRELOAD, ALR_ROOT, ALR_GUEST_EXE…

    switch (kind) {
    case ELF_DYN:
        return REAL(execve)(g_ldso_host, build_ldso_argv(host, guest_path, argv), env2);
    case ELF_STATIC:
        alr_log(1, "static binary, running unhooked: %s", guest_path);
        return REAL(execve)(host, argv, env2);   // preload 없이 실행됨
    case SHEBANG:
        return alr_exec_shebang(host, guest_path, argv, env2);   // §9.3
    default:
        errno = ENOEXEC; return -1;
    }
}
```

### 9.2 envp 재주입 규칙

멱등이어야 한다 — 이미 올바르면 아무것도 하지 않는다.

- `LD_PRELOAD`: 원하는 `.so`들이 **콜론 구분 항목 전체로** 존재하는지 검사. 없으면 앞에 붙인다(순서: fakeroot → preload → 게스트 기존 항목, 중복 제거).
- `ALR_ROOT`, `ALR_GUEST_EXE`(새 프로그램의 게스트 경로로 갱신), `ALR_GUEST_ARGV0` 설정.
- **bionic 경계**: 타깃이 `/system/`이나 `$PREFIX` 아래면 (게스트가 `am`/`pm`/`termux-open`을 부르는 경우) **alr의 glibc `.so`를 제거하고 Termux의 원래 `LD_PRELOAD`를 복원**한다 ([§B7](01-platform-facts.md)). 이 값은 `ALR_HOST_LD_PRELOAD`로 보존해 둔다.

### 9.3 shebang

커널 `binfmt_script`를 쓸 수 없다 (인터프리터를 호스트 루트 기준으로 찾는다).

- `#!` 다음 최대 **255바이트** (`fs/binfmt_script.c`와 동일)
- 인터프리터 + **분리되지 않는 단일 인자** 하나 (Linux 의미론: `#!/usr/bin/env -S foo bar`의 `-S foo bar`가 통째로 argv[1])
- 새 argv = `[인터프리터, (선택 인자,) 스크립트의_게스트_경로, 원래 argv[1..]]`
- 인터프리터를 게스트 네임스페이스에서 해석 후 §9.1로 **재귀**
- 재귀 깊이 상한 **4** (`BINPRM_MAX_RECURSION`)

### 9.4 `posix_spawn` / `system` / `popen`

`posix_spawn`은 파일 액션과 attr를 보존하며 재작성해야 한다. 가장 안전한 구현은 **`fork` + `alr_exec_common` + 파일 액션 수동 적용**이지만 `POSIX_SPAWN_SETSID` 등 의미론을 잃는다. 대안은 `REAL(posix_spawn)`에 재작성된 path/argv/envp를 넘기는 것 — 이쪽을 기본으로 한다.

`system`/`popen`은 `/bin/sh -c`로 귀결되므로 셸 경로만 재작성하면 나머지는 셸 안의 exec가 처리한다.

## 10. `syscall()` 인터포즈 — Node 필수

libuv가 **raw syscall로 경로를 넘긴다** ([§C7](01-platform-facts.md)). 모든 `fs.stat`/`fs.statSync`가 게스트 경로를 재작성 없이 커널에 직행시켜 `ENOENT`가 난다. 성능 문제가 아니라 **하드 브레이크**이고, 증상은 "Node가 가끔 파일을 못 본다"다.

**인덱스 하나로는 표현할 수 없다.** `linkat`/`renameat`/`renameat2`는 경로 인자가 **둘**이다. 단일 `int` 반환으로 설계하면 구현자가 하나만 재작성하고, `syscall(SYS_renameat2, AT_FDCWD, "/var/lib/dpkg/status", AT_FDCWD, "/var/lib/dpkg/status-new", 0)`의 목적지가 Android 호스트 루트로 나가 `ENOENT`가 된다 — dpkg의 원자적 교체가 깨진다.

**비트마스크를 쓰되 단일 경로 fast path는 버퍼 하나로 유지한다** (프레임을 8 KB로 키우지 않기 위해):

```c
long syscall(long nr, ...)
{
    va_list ap; va_start(ap, nr);
    long a[6]; for (int i = 0; i < 6; i++) a[i] = va_arg(ap, long);
    va_end(ap);

    unsigned m = alr_path_arg_mask(nr);            // 0 = 경로 인자 없음
    if (m & (m - 1))                               // 2비트 이상 = 드묾
        return alr_syscall_multipath(nr, a, m);    // noinline, 버퍼 2개

    char buf[ALR_PBUF];
    if (m) {
        int pi = __builtin_ctz(m);
        if (a[pi]) {
            const char *n = rw((const char *)a[pi], buf);
            if (!n) return -1;                     // rw() 가 errno 설정
            a[pi] = (long)n;
        }
    }
    return REAL(syscall)(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
}
```

`alr_path_arg_mask()`가 커버해야 하는 최소 집합 (aarch64 번호, `include/uapi/asm-generic/unistd.h`):

| nr | 이름 | 경로 인자 (비트) | 비고 |
|---|---|---|---|
| 56 | `openat` | 1 | |
| 79 | `newfstatat` | 1 | |
| 291 | `statx` | 1 | |
| 48 | `faccessat` | 1 | |
| 439 | `faccessat2` | 1 | |
| 78 | `readlinkat` | 1 | |
| 34 | `mkdirat` | 1 | |
| 35 | `unlinkat` | 1 | |
| **36** | **`symlinkat`** | **2** | ⚠️ **인자 0은 `target`으로 재작성 금지, 인자 1은 `newdirfd`(int)다.** 이 표에서 dirfd가 arg 0이 아닌 **유일한** 항목 |
| 37 | `linkat` | 1, 3 | 양쪽 재작성 |
| 38 | `renameat` | 1, 3 | 양쪽 |
| 276 | `renameat2` | 1, 3 | 양쪽 |
| 53 | `fchmodat` | 1 | |
| 54 | `fchownat` | 1 | |
| 88 | `utimensat` | 1 | |
| 33 | `mknodat` | 1 | |
| 437 | `openat2` | 1 | |

> ⚠️ **`symlinkat(const char *target, int newdirfd, const char *linkpath)`** — 인자 0이 `target`(심링크 *내용*, 게스트 네임스페이스 문자열이므로 재작성하면 안 됨), 인자 1이 `newdirfd`(**정수 fd**), 인자 2가 `linkpath`(재작성 대상)다.
> 인덱스를 1로 두면 `rw()`에 `AT_FDCWD`(-100)가 `const char *`로 들어가 **핫패스에서 wild pointer를 역참조**하고, `linkpath`는 재작성되지 않는다. §14의 `PRELOAD SYMLINKAT ASYMMETRY` 테스트가 이것을 지킨다.

> `getdents64`는 경로 인자가 없어 이 표에 넣지 않는다. link2symlink의 `d_type` 보정이 필요하면 §8.2의 별도 후처리로 다룬다.

이 인터포즈는 **Go/Rust asm의 인라인 `svc`는 잡지 못한다.** 그건 `alr doctor` P11이 경고로 처리한다.

## 11. `ioctl` 번역

PTY 슬레이브의 ioctl은 13개 화이트리스트뿐이다 ([§B5](01-platform-facts.md)). 그 밖은 `EACCES`.

| ioctl | 처리 |
|---|---|
| `FIONREAD` / `TIOCINQ` | 마스터 쪽 non-blocking read로 에뮬레이션 (가장 중요 — readline/ncurses가 쓴다) |
| `TIOCGSID` | `getsid()`로 답한다 |
| `TIOCNOTTY`, `TIOCEXCL`, `TIOCPKT` | 성공(0) 반환하고 무시 |
| `TCGETS2` / `TCSETS2` / `TCSETSW2` / `TCSETSF2` | `TCGETS`/`TCSETS` 계열로 변환 (커널 termios2 → termios 레이아웃) |
| `TIOCGETD` / `TIOCSETD` | `N_TTY`로 답한다 |
| `TIOCSTI` | `EACCES` + 설명 로그. `neverallowxperm`이라 절대 안 된다 |

생 `EACCES`를 그대로 돌려주면 readline/ncurses 깊은 곳에서 이해 불가능한 실패가 난다.

## 12. `/dev/full` 에뮬레이션

> ⛔ **정정 (2026-08-03, 영구 비목표).** 아래 절과 [§6.15](#615-fd-상태--12-devfull) 의 열두 개 심볼은 **구현하지 않으며, 앞으로도 하지 않는다.** [RISKS](RISKS.md) · [00 §5](00-product.md) · `src/preload/wrappers.def` 의 `DELIBERATELY NOT IMPLEMENTED` 블록 · `alr doctor` 의 `[P9] EXPECTED (non-goal, not emulated)` 가 모두 같은 결정을 기록한다. 수용 시험은 `PRELOAD DEV FULL ENOSPC: KNOWN_FAIL:non-goal-devfull` 로 이 상태를 감시한다.
>
> **왜 남겨 두는가**: 설계 근거를 지우면 다음 사람이 같은 길을 다시 판다. **왜 하지 않는가**: `/dev/full` 하나를 위해 프로세스에서 가장 뜨거운 시스템콜인 `write()` 를 인터포즈해야 하고, stdio 심볼을 하나라도 빠뜨리면 **성공한 쓰기로 조용히 위장**된다. 얻는 것은 `/dev/full` 을 쓰는 테스트 스위트 하나이고, 거는 것은 모든 게스트의 모든 쓰기다.
>
> **[§6](#6-래퍼-심볼-표) 이 `wrappers.def` 를 단일 정본이라 선언한다.** §6.15 는 그 정본과 어긋나 있었고, 이 정정이 그 어긋남을 닫는다.


`/dev/full`은 sepolicy 타입이 없어 **모든 도메인에서 `EACCES`**다 ([§B5](01-platform-facts.md)). 스톡 Ubuntu와 그 테스트 스위트들이 존재를 가정한다.

`open("/dev/full", flags)` → **실제로는 `/dev/zero`를 열어** 그 fd를 반환하고, 고정 크기 fd 테이블(BSS, R1에 따라 malloc 금지)에 기록한다.

`/dev/zero`가 읽기·seek 의미론을 그대로 준다 (`man 4 full`: 읽기는 NUL 반환, seek는 항상 성공). 따라서 **`read`/`lseek`/`fstat`/`close`는 래퍼가 필요 없다.** 기록된 fd에 대한 **write 계열만** 가로채 `ENOSPC`를 반환한다 (§6.15).

테이블은 `dup`/`dup2`/`dup3`/`fcntl(F_DUPFD)`에서 갱신하고 `close`에서 해제한다. **execve 후에는 유지되지 않는다** — 상속된 fd는 에뮬레이션되지 않으며 이것을 알려진 한계로 명시한다.

**`/dev/null`로 심링크하지 말 것.** ENOSPC를 기대하는 테스트가 조용히 통과해 버린다.

## 13. 성능 검증

M4 acceptance:

| 측정 | 목표 | 강제 방식 |
|---|---|---|
| **`git status`(10k) 재작성 총비용** | **≤ 1.5 ms** | **하드 게이트** (`preload.rw_total_us`) |
| `rw()` 절대경로 히트 | ≤ 100 ns/op (참조) | 기기별 회귀 검사 2.5× |
| `rw()` 상대경로 미스 | ≤ 20 ns/op (참조) | 기기별 회귀 검사 2.5× |
| `rw()` sysdir 통과 | ≤ 40 ns/op (참조) | 기기별 회귀 검사 2.5× |
| `rw()` already-under-root | (신규) | 기기별 회귀 검사 2.5× |

> ⚠️ **per-op 절대 예산은 기기 간 이식되지 않는다** — `MEASURED` 2026-08-03.
> 동일한 바이너리가 참조 #1 에서 ~79 ns, 참조 #2 에서 106–115 ns 다. 100 ns 선은 코드가 아니라 폰의 속성이다.
> 그래서 하드 게이트는 **총비용** 쪽으로 옮겼고, per-op 는 **그 기기 자신의 기준선 대비 2.5×** 회귀 검사로 남겼다.
> 2.5× 는 느슨해서가 아니라 **실측 잡음 바닥**이다 — best-of-7, 300 ms 시간 워밍업, 코어 고정을 모두 적용하고도
> 스프레드가 ~2× 다(Android 가 앱을 cpuset 에 가두어 8코어 중 4개만 고정 가능).
> **잡을 수 있는 것**: §5.1 이 경고하는 캐시/변환기 삽입(업스트림 실측 4,334 ns/op = 33×).
> **못 잡는 것**: 50% 수준의 회귀. 근거 [M19 §7](evidence/2026-08-03-m19-snapdragon.md).
>
> 총비용은 **모델이 아니라 실측**이다: 호출 믹스는 preload 의 `ALR_COUNT` 카운터에서, 단가는 `rw_cost` 에서
> 같은 세션에 잰다(`tests/device/rw_bench.sh`). 이전 판은 절대경로 단가를 13,500회 전부에 곱해 **약 24배 과대**했다.
| `git status` (10k 파일) 재작성 호출 수 | 12,000–15,000 (기록만, 게이트 아님) |
| `git status` 재작성 총 비용 | ≤ 1.5 ms |
| LD_PRELOAD 적용 시 execve 지연 증가 | 측정하고 기록 (per-exec DSO 매핑 + PLT 스코프 확대 — [§D2](01-platform-facts.md)) |

마이크로벤치는 `bench/microbench/` 에 두고 게스트 안에서 돌린다.

## 14. 테스트

`tests/cases/paths.tsv` — `(ALR_ROOT, input, expected)` 공유 테이블. C 테스트, C++ 테스트, Python 레퍼런스 모델이 **모두** 소비한다. 재작성기 드리프트를 막는 유일한 방어다.

| 테스트 | 검증 |
|---|---|
| `PRELOAD PATH ABS` / `REL` / `SYSDIR` / `IDEMPOTENT` | §5 |
| `PRELOAD DOTDOT CLAMP` | `..`가 rootfs를 탈출하지 못함 |
| `PRELOAD PROC SELF EXE` | Node `process.execPath`가 게스트 경로 |
| `PRELOAD EXEC DYNAMIC` / `SHEBANG` / `STATIC` | §9 |
| `PRELOAD EXEC ENVP IDEMPOTENT` | 재주입이 중복을 만들지 않음 |
| `PRELOAD SYSCALL REWRITE` | `syscall(__NR_newfstatat, …)`가 재작성됨 |
| `PRELOAD SYMLINKAT ASYMMETRY` | `symlinkat`의 target이 재작성되지 **않음** |
| `PRELOAD LINK2SYMLINK` | §8.4 매트릭스 |
| `PRELOAD LINK2SYMLINK FSTAT NLINK` | `stat(path)`와 `fstat(fd)`의 `st_nlink` 일치 |
| `PRELOAD LINK2SYMLINK DTYPE` | `find -type f`가 그림자 파일을 찾음 (readdir `d_type` 보정) |
| `PRELOAD DLOPEN ABS PATH` | 절대 게스트 경로 `dlopen` 성공 (§6.13) |
| `PRELOAD DLOPEN ORIGIN TOKEN` | `$ORIGIN` 경로가 재작성되지 **않음** |
| `PRELOAD MKSTEMP` | `mkstemp("/tmp/x.XXXXXX")` 성공 + 템플릿 되쓰기 (§6.14) |
| `PRELOAD DEV FULL ENOSPC` | §12 |
| `PRELOAD NORMALIZE BEFORE SYSDIR` | `/proc/../etc/passwd`가 호스트 `/etc/passwd`로 새지 않음 (§5.1) |
| `PRELOAD PBUF PATH_MAX` | 3000바이트 게스트 경로가 `ENAMETOOLONG` 없이 동작 |
| `PRELOAD CHK SYMBOLS PRESENT` | `nm -D --defined-only`가 `wrappers.def`의 모든 이름을 보고 |
| `PRELOAD NO GLIBC ABOVE 2.17` | `readelf -V` DT_VERNEED |
| `PRELOAD NO MALLOC` | 재작성 경로에 malloc 호출 없음 (심볼 검사 또는 후킹 테스트) |
