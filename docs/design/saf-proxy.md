# T5-C — SAF 직통 프록시 설계 (진짜 마운트 + copy 폴백)

상태: 설계 + host-검증 순수 모델 확장(`tools/saf_bridge_model.py`,
`tests/test_saf_bridge_model.py`). 런타임/UI 배선 미구현(제안만). 소유:
research/product-ux. **runtime_report.cpp / MainActivity / AndroidManifest /
RootfsInstaller 직접 수정 없음** — 변경은 §7 제안으로만.

선행: `docs/design/file-bridge-saf.md`(2모드 트레이드오프 + §5-F `alr_saf_resolve`
계약), `docs/design/adr-003-multiprocess-exec-reentry.md`(supervisor trap 확장
패턴 + fd-주입 접점), `app/src/main/cpp/runtime_report.cpp`(EVENT_SECCOMP
path-rewrite 핸들러 L2071~).

## 0. 무엇이 바뀌나 (file-bridge-saf.md 와의 관계)

`file-bridge-saf.md`는 **하이브리드(권장)** 로 "1단계 copy → 3단계 옵트인 프록시"
로드맵을 폈다. 찬우 확정: **직통 프록시(진짜 마운트)가 최종 목표, copy는 중간단계
폴백.** 이 문서는 그 확정에 맞춰 _프록시 모드를 1급 시민으로_ 구체화한다 —
supervisor가 게스트 `openat("/mnt/android/...")` 의 **결과 fd 자체를 SAF가 연 fd로
치환**(`kSubstituteFd`)하는 메커니즘, 그 한계(seek/mmap/대용량/디렉터리 열거),
양방향(게스트 출력→MediaStore), 그리고 copy→proxy 전환의 device 게이트.

핵심 구분(이 문서 전체의 축):

| | **copy 모드** | **proxy 모드(직통)** |
|---|---|---|
| 게스트가 보는 것 | rootfs 안 실파일(copy-in 됨) | `/mnt/android/<label>/...` 가상 경로 |
| supervisor decision | `kRewritePath(host_path)` | `kSubstituteFd(fd)` |
| trap 핸들러 변경 | **0**(기존 x1 rewrite 재사용) | **신규**(openat 결과 fd 치환 — §3) |
| seek/mmap/대용량 | 완벽(로컬 파일) | 부분(ParcelFileDescriptor 능력에 종속) |
| 동기화 | 약함(명시 write-back/export) | 강함(즉시, 같은 fd) |
| 비root 안전성 | ✅ | ✅ (전부 public SAF + fd dup) |

두 모드는 **마운트 단위**로 고른다(`SafMount.bridge_mode`). 같은 게스트
경로공간(`/mnt/android/<label>`) 위에서, 라벨마다 copy 또는 proxy.

## 1. 마운트 라벨 + 경로공간 (두 모드 공통)

```
안드로이드(SAF tree URI)                게스트 경로공간          mode
content://…/tree/primary%3ADownload  ⇄  /mnt/android/downloads  proxy(rw)
content://…/tree/primary%3ADCIM      ⇄  /mnt/android/dcim       proxy(ro)
content://…/tree/primary%3ADocuments ⇄  /mnt/android/docs       copy(rw)   ← 폴백
ACTION_SEND 1회용                     →  /root/Android-Share/<n> (copy-in, 단방향)
게스트 출력                           →  MediaStore.<col> insert (copy-out, 단방향)
```

- 마운트 루트 = `SAF_MOUNT_ROOT = /mnt/android`. 그 아래 한 단계 = `label`.
- `label` 아래 **상대 경로(rel)** 컴포넌트만 SAF DocumentFile 트리로 내려간다.
- 두 모드 모두 동일한 `SafBridge.resolve(guest_path, op)` 로 분류·정책·escape
  검사를 통과한다. 모드 분기는 그 _다음_ 단계(decision 매핑)에서만 일어난다 —
  즉 **traversal/read-only/escape 안전성은 모드와 무관하게 한 곳에서 강제**된다.

## 2. copy 모드 (중간 폴백) — 요약

`file-bridge-saf.md` §2(a)/§6-1단계 그대로. 게스트가 `/mnt/android/docs/x.txt`
를 open하면:
1. supervisor가 `alr_saf_resolve` 호출 → 브리지가 `bridge_mode=copy` 확인.
2. 브리지(JNI)가 `DocumentFile.fromTreeUri → findFile(rel)` 로 문서 URI를 얻어
   `ContentResolver.openInputStream` 으로 rootfs 임시파일에 **copy-in**.
3. decision = `kRewritePath(host_path=<rootfs 임시파일>)` → 기존 x1 rewrite로
   처리(trap 핸들러 변경 0). 게스트는 평범한 로컬 파일을 본다.
4. 게스트가 닫고(또는 fsync) write 의도였으면 **copy-out(write-back)**:
   rootfs 임시파일 바이트를 `openOutputStream(문서 URI)` 로 되돌린다(rw 마운트만).

copy 모드의 값어치: **seek/mmap/`O_APPEND`/rename/대용량 임의접근 100% 호환**
(전부 로컬 파일). proxy가 깨지는 앱(아래 §4)의 안전한 폴백.

## 3. proxy 모드 (직통, 진짜 마운트) — fd 치환 메커니즘

목표: 게스트가 `/mnt/android/<label>/a/b.png` 를 open하면, **copy 없이** SAF가 연
실제 fd(`ParcelFileDescriptor`)를 게스트의 openat 결과로 돌려준다. 게스트 입장에선
그 경로가 "진짜 열린 파일"이다.

### 3-A. 왜 x1 rewrite(copy 모델)로는 안 되나

현 supervisor 핸들러(`runtime_report.cpp` L2071~)는 **경로를 경로로** 바꾼다:
`pread(x1=path)` → `translate_rootfs_path` → 스택 scratch에 host 경로 `pwrite`
→ `regs[1]=scratch` → `SETREGSET`. openat은 그 _다음_ 커널이 정상 수행하고,
**결과 fd는 게스트가 정한다.** SAF 문서는 rootfs 안에 _경로가 없으므로_(URI만
있음) 이 모델로는 못 연다 — 어떤 host 경로로도 rewrite할 수 없다. 그래서
proxy는 "경로 치환"이 아니라 **"openat 결과 fd 치환"** 이라는 별도 메커니즘이
필요하다. 이것이 `file-bridge-saf.md` §5-F가 `kSubstituteFd`로 격리한 후속 작업
이고, ADR-003이 든 "trap 핸들러가 fd를 주입하게 되는" 확장 접점이다.

### 3-B. fd 치환의 두 후보 기법 (비root·public API 한계 안에서)

supervisor는 이미 tracee를 `PTRACE_SEIZE` + seccomp `RET_TRACE(openat)` 로 잡고
있고, `/proc/<tid>/mem` O_RDWR fd로 레지스터/메모리를 읽고 쓴다. fd를 게스트에
넣는 길은 둘:

**(기법 A — 권장) syscall을 EINVAL로 죽이고, 게스트 fd 테이블에 우리 fd를 심기.**
seccomp 트랩은 syscall-entry에서 멈춘다. supervisor가:
1. entry stop에서 path를 읽어 `alr_saf_resolve` → `kSubstituteFd(host_fd)` 결정
   (host_fd = ALR 프로세스가 `ParcelFileDescriptor.getFd()` 로 가진, SAF가 연 fd).
2. **openat을 무력화**: `regs[8]`(syscall nr)을 무효값(예 `-1`)으로 `SETREGSET`
   하거나, 경로를 존재-안-하는 sentinel로 rewrite해 openat이 실패하게 둔 뒤,
   syscall-**exit** stop에서 `regs[0]`(반환 fd)을 우리가 게스트로 복제한 fd 번호로
   덮어쓴다. 단, **게스트와 supervisor는 별 프로세스**라 host_fd 번호는 게스트
   fd 테이블에서 의미 없다 → 게스트 주소공간에 fd를 _실제로_ 만들어야 한다.
   비root에서 다른 프로세스 fd 테이블에 fd를 직접 꽂는 public API는 없다
   (`pidfd_getfd`는 `CAP_SYS_PTRACE`/같은 사용자+yama 요건, Android untrusted_app
   에서 미보장). → **supervisor가 tracee를 시켜 fd를 받게** 한다: §3-C.

**(기법 B — 폴백) memfd + 내용 복제(=copy의 fd판).** proxy가 진짜 라이브 fd를
못 주는 경우, supervisor가 `memfd_create` 로 익명 fd를 만들고 SAF fd 내용을
복사해 그 memfd를 게스트에 심는다. write-back은 close에서. 이건 **proxy의 외형
(즉시 fd, 경로 그대로)에 copy의 일관성 한계**를 합친 중간형 — §4의 mmap이
필요하나 라이브 SAF fd가 seek 못 줄 때의 안전판.

### 3-C. fd를 게스트 fd 테이블로 넣는 실제 경로 (ADR-003 fd-주입 접점)

게스트(tracee)는 SEIZE+seccomp 하에 있으므로, supervisor는 **tracee가 자기
손으로 fd를 받게** 조종할 수 있다. 비root에서 fd를 프로세스 경계로 넘기는 public
메커니즘 = **`SCM_RIGHTS`(AF_UNIX ancillary fd passing)** — ALR과 게스트는 같은
in-process 주소공간이 아니라 **부모(supervisor) ↔ 자식(게스트 tracee)** 관계라,
부모가 미리 만든 socketpair의 한쪽을 자식이 상속(`alr_enter_guest` 이전 fork
시점)하면, 부모가 SAF fd를 `sendmsg(SCM_RIGHTS)` 로 보내고 _게스트가 `recvmsg`
하면_ 게스트 fd 테이블에 새 fd가 생긴다. supervisor는 이 `recvmsg` 를 게스트를
대신해 **트램펄린 주입**으로 실행시킨다:

1. (사전) fork 직후, 게스트가 `alr_enter_guest` 로 들어가기 전 supervisor가
   socketpair `(sv_parent, sv_guest)` 를 만들고 `sv_guest` 를 게스트가 상속.
   이 fd 번호는 부팅 시 1회 고정(게스트는 안 씀, supervisor 전용 채널).
2. (openat 트랩 시) supervisor가 `sendmsg(sv_parent, SCM_RIGHTS=[host_saf_fd])`.
3. supervisor가 게스트 레지스터를 저장 → `recvmsg(sv_guest, …)` 를 게스트가
   실행하도록 PC/레지스터를 세팅(트램펄린: in-process loader가 이미 쓰는 syscall
   주입 패턴)하고 single-step → 게스트 fd 테이블에 **새 fd(=게스트가 본 SAF fd)**
   생성. 게스트 레지스터 복원.
4. 원래 openat이 그 새 fd 번호를 반환하도록 syscall-exit `regs[0]` 치환.

이 경로는 **새 ptrace op·새 권한·새 syscall 0**(전부 기존 trap 사이트 + 게스트
자신의 recvmsg). ADR-003 §2-(B) "기존 trap 사이트 확장"과 정합. _단_ 트램펄린
주입·socketpair 상속·single-step은 **device-only 검증**(§6 게이트). host 모델은
"어느 트랩에서 어떤 fd를 어디로"의 _결정·정책_만 검증한다(§5, 순수 모델).

### 3-D. 한계 (proxy가 copy보다 약한 지점)

SAF가 주는 fd는 `ContentResolver.openFileDescriptor(uri, mode)` 의
`ParcelFileDescriptor` 다. 그 능력은 **provider 구현에 종속**:

- **seek/pread/pwrite**: `externalstorage`/로컬 SAF provider는 보통 실파일을 열어
  seekable fd를 준다(대부분 OK). 그러나 **네트워크/클라우드 provider**(Drive,
  일부 DocumentsProvider)는 pipe-backed fd → `lseek`/`pread` 실패(`ESPIPE`).
  이 경우 proxy 깨짐 → **copy 폴백 자동 강등**(§4-policy).
- **mmap**: pipe-backed fd는 `mmap` 불가. 실파일 fd는 `mmap` 가능(read), 단
  쓰기 mmap+동기화 의미(MAP_SHARED writeback)는 provider별. mmap 의존 앱(예
  GIMP의 일부 경로, sqlite mmap I/O)은 copy 폴백이 안전.
- **대용량**: proxy는 복사 0(스트리밍/직접 fd)이라 _대용량에 유리_. 단 라이브 fd
  를 못 주고 §3-B 기법 B(memfd 복제)로 떨어지면 대용량은 다시 copy 비용.
- **디렉터리 열거(`getdents`/`readdir`)**: SAF엔 "디렉터리 fd" 개념이 없다 —
  `DocumentFile.listFiles()` 가 자식 URI 목록을 줄 뿐. proxy는 **디렉터리 open을
  fd로 못 치환**한다. → 디렉터리 listing은 _별도 합성_(supervisor가
  `getdents64` 트랩을 가로채 `listFiles()` 결과를 게스트 버퍼로 합성)이거나, 1차
  엔 "디렉터리는 copy 모드처럼 가상 엔트리만"(file-bridge-saf.md §6-2단계)으로
  처리. **proxy fd 치환은 _레귤러 파일 open_ 에만 적용**, 디렉터리는 listing
  경로로 분기.
- **read-only vs read-write**: 모드는 `SafMount.mode`(ro/rw)로 강제. proxy fd도
  `openFileDescriptor(uri, "r")` vs `"rw"` 로 연다 — ro 마운트에 쓰기 의도
  (`O_WRONLY/O_RDWR/O_CREAT`)면 `alr_saf_resolve` 가 `kDeny(EROFS)` 반환(브리지
  정책, §5). fd를 열기도 전에 거부 → ro 불변식이 모드 무관하게 한 곳에서 유지.
- **`O_CREAT`(새 파일)**: proxy에서 없는 파일 open+create는 `DocumentFile
  .createFile(mime, name)` 로 먼저 자식 문서를 만든 뒤 그 URI의 fd를 준다. rw
  마운트만. 이름/mime 추론은 브리지 책임.

### 3-E. 양방향 (게스트 출력 → 안드로이드)

- **proxy(rw)**: 같은 라이브 fd에 쓰면 SAF provider가 원본에 즉시 반영 — 별도
  export 불요(직통의 핵심 이점). close/fsync 시 `ParcelFileDescriptor` flush.
- **copy(rw)**: close에서 write-back(§2-4).
- **MediaStore export**(두 모드 공통, tree 권한 없을 때): 게스트 출력 파일을
  `MediaStore.<col>.insert(ContentValues(RELATIVE_PATH))` 후 그 URI로 copy-out.
  `export_to_mediastore`(기존 모델) 그대로.

## 4. copy ↔ proxy 정책 (자동 강등 + 마운트 기본값)

`SafMount.bridge_mode` 가 _희망 모드_. 실제 동작은 런타임 능력에 따라 강등될 수
있다(브리지가 결정, 게스트엔 투명):

```
희망 proxy → openFileDescriptor 성공 + seekable + (mmap 불요 or 실파일 fd)
           → 라이브 fd 치환(kSubstituteFd)                     ✅ 직통
희망 proxy → fd가 pipe-backed(ESPIPE) AND 게스트가 seek/mmap 요구
           → memfd 복제(§3-B 기법 B) 또는 copy 폴백             ⚠ 강등
희망 proxy → 디렉터리 open
           → listing 합성 경로(파일 아님)                       — 분기
희망 copy  → 항상 copy-in/out                                    ✅ 폴백
ro 마운트 + 쓰기 의도 (모드 무관)
           → kDeny(EROFS)                                        ✋ 거부
```

브리지는 마운트별로 `effective_mode`(proxy를 시도했다 강등된 결과)를 기록해
같은 라벨의 다음 open이 불필요한 재시도를 피하게 한다(정책 메타데이터, §5).

## 5. 순수 모델 확장 (`tools/saf_bridge_model.py`)

런타임/UI 독립. Android API 호출 0. 이 PR이 확장하는 부분:

- `BridgeMode(Enum)`: `COPY`("copy") / `PROXY`("proxy"). 마운트가 _희망_ 하는
  브리지 메커니즘.
- `SafMount(tree_uri, label, mode, bridge_mode=COPY)`: `bridge_mode` 필드 추가.
  기본 `COPY`(가장 안전한 폴백). `mode`(ro/rw)와 직교.
- `ResolvedPath(... , bridge_mode)`: resolve 결과에 모드 전파(런타임이 decision
  매핑에 사용).
- `BridgeDecision(Enum)`: `FALLTHROUGH`/`REWRITE_PATH`/`SUBSTITUTE_FD`/`DENY` —
  `file-bridge-saf.md` §5-F의 C++ `SafDecision` 과 1:1. host 모델이 "어떤 트랩이
  어떤 decision"인지 _순수 함수_로 고정(아래).
- `decide(bridge, guest_path, *, write, create) -> SafResolution`: **§5-F 계약의
  host 절반**. `(guest_path, 쓰기/생성 의도)` → `BridgeDecision` + 페이로드
  (proxy면 마운트 prefix·rel·ro 여부 = fd-주입 _대상_ 메타데이터; copy면 host
  임시경로 자리표시·복사 방향). 분기 규칙:

  | `SafBridge.resolve` | 의도 | bridge_mode | decision |
  |---|---|---|---|
  | `NOT_MAPPED` | any | — | `FALLTHROUGH`(런타임 rootfs 폴백) |
  | ok | write/create | ro 마운트 | `DENY(EROFS)` |
  | ok | any | `PROXY` | `SUBSTITUTE_FD`(fd-주입 대상) |
  | ok | any | `COPY` | `REWRITE_PATH`(copy-in 후 host 경로) |
  | `ESCAPE` | — | — | `DENY(EACCES)` |
  | `READ_ONLY_VIOLATION` | — | — | `DENY(EROFS)` |

- `SafResolution`(frozen dataclass): `decision`, `resolved`(`ResolvedPath` or
  None), `deny_errno`(DENY일 때 13/30), `proxy_target`(SUBSTITUTE_FD일 때:
  `mount_prefix=/mnt/android/<label>`, `rel_path`, `read_only` — supervisor의
  §3-C fd-주입이 SAF URI를 만들 입력), `copy_writeback`(REWRITE_PATH일 때 쓰기면
  True = close에서 write-back 필요).

**proxy의 `/mnt/android` prefix 매칭 → fd-주입 대상 판정**(이 PR 테스트의 핵심):
`decide` 가 `SUBSTITUTE_FD` 를 낼 때 `proxy_target.mount_prefix` 가 정확히
`SAF_MOUNT_ROOT/<label>` 이고 `rel_path` 가 그 아래 상대경로임을 보장 →
supervisor가 "이 트랩(openat, 경로가 mount prefix로 시작)에서 어떤 SAF URI의 어떤
rel을 열어 fd로 치환하나"를 모호함 없이 안다. prefix 밖이면 애초에
`FALLTHROUGH`(fd-주입 비대상).

**불변식(모드 추가로도 안 깨짐):**
1. escape/traversal/ro 검사는 `decide`가 모드 분기 _전에_ `resolve`로 강제 →
   proxy든 copy든 `/mnt/android/<label>` 밖을 못 만지고 ro에 못 쓴다.
2. `SUBSTITUTE_FD`(proxy)는 항상 mount prefix 안의 경로에 대해서만 — fd-주입이
   rootfs/Android FS 임의 경로로 새지 않는다.
3. `bridge_mode`는 _희망_; ro/escape 정책이 항상 이긴다(DENY가 SUBSTITUTE_FD/
   REWRITE_PATH보다 우선).

## 6. copy→proxy 전환 단계 + device 게이트

`file-bridge-saf.md` §6의 단계 로드맵을 proxy-우선으로 재배치:

1. **단계 0(현재)** — host 순수 모델: 모드 분기·decision 매핑·proxy prefix→fd-주입
   대상 판정·copy 폴백 라우팅을 `decide`로 고정 + 무회귀 테스트. **device 불요.**
   (이 PR)
2. **단계 1(copy 출시)** — `bridge_mode=COPY` 마운트만 배선. `alr_saf_resolve`가
   `REWRITE_PATH`만 반환 → 기존 x1 rewrite 재사용, **trap 핸들러 변경 0**. share-in
   / MediaStore-out 동시. **게이트: `DEVICE-REQ: ALR-SAF-copy — SM-X236N (am
   force-stop first), 게스트 앱이 /mnt/android/<label>/file 을 open→read→
   modify→close; gate = copy-in host 경로로 열림(path_rewrites≥1) AND close 후
   원본 SAF 문서가 변경 반영(write-back) AND escape/ro 위반 0.`**
3. **단계 2(proxy 레귤러 파일, ro 먼저)** — `SUBSTITUTE_FD` 구현: §3-C socketpair
   상속 + SCM_RIGHTS + 트램펄린 recvmsg + syscall-exit `regs[0]` 치환. **읽기
   전용 미디어(dcim ro)부터** — 쓰기/create 없는 가장 단순 경로. **게이트:
   `DEVICE-REQ: ALR-SAF-proxy-ro — SM-X236N (am force-stop first), 게스트가
   /mnt/android/dcim/<img> 를 open(O_RDONLY)→read N bytes→close; gate = openat이
   copy 없이 SAF fd로 치환(memfd/copy 흔적 0) AND read 바이트가 원본과 일치 AND
   seekable이면 lseek+pread도 일치. pipe-backed면 자동 강등 로그(copy 폴백).`**
4. **단계 3(proxy rw + create)** — `O_RDWR`/`O_CREAT` 라이브 fd, 양방향 즉시 반영
   (§3-E). **게이트: `DEVICE-REQ: ALR-SAF-proxy-rw — SM-X236N, 게스트가
   /mnt/android/downloads/out.txt 를 O_RDWR|O_CREAT 로 열고 write→fsync→close;
   gate = 별도 export 없이 SAF 원본에 즉시 반영 AND ro 마운트 동일 시도는
   EROFS.`**
5. **단계 4(mmap/디렉터리 합성, 옵트인)** — mmap 의존·`getdents` 디렉터리 열거.
   깨지는 앱이 실측될 때만. mmap 불가 fd는 copy 강등이 안전판.

각 단계는 _이전 단계 게이트 PASS 후_ 진행. 단계 1(copy)이 trap 핸들러 변경 0으로
즉시 가능한 게 핵심 — proxy의 supervisor 확장(§3-C)이 device로 검증되기 전까지
사용자는 copy로 모든 파일연동을 쓴다.

## 7. UI / runtime / Manifest 변경 제안 (직접 수정 금지 — 통합 세션 결정)

1. **runtime JNI(`runtime_report.cpp`, WS-1 소유)** — `alr_saf_resolve`(§5-F C++
   훅) 안에서 `SafBridge`+`decide` 로직 호출. 단계 1은 `REWRITE_PATH`만(변경 0).
   단계 2+는 (a) fork 직후 socketpair 상속(§3-C-1), (b) openat 트랩에서
   `SUBSTITUTE_FD` 시 SCM_RIGHTS 전송 + 트램펄린 recvmsg + exit `regs[0]` 치환.
   _전부 기존 trap 사이트 확장, 새 ptrace op/권한/syscall 0._
2. **MainActivity / 신규 SettingsActivity(다른 트랙 소유)** —
   `ACTION_OPEN_DOCUMENT_TREE` + `takePersistableUriPermission` + 라벨·모드
   (ro/rw)·**bridge_mode(copy/proxy)** 입력 → `SafBridge` 등록. proxy fd 제공
   JNI 콜백: `ContentResolver.openFileDescriptor(uri, mode)` →
   `ParcelFileDescriptor.detachFd()` → host fd(§3-C-2).
3. **AndroidManifest.xml(다른 트랙 소유)** — share 수신 인텐트필터만(file-bridge
   -saf.md §8-1 그대로). `MANAGE_EXTERNAL_STORAGE`/`READ_MEDIA_*` 추가 안 함 —
   proxy도 전부 사용자-선택 tree URI + fd dup 으로 동작(광역 권한 0).

## 8. 비root / 안전성 점검

- mount/FUSE 0. proxy의 "마운트感"은 **fd 치환**(public `ParcelFileDescriptor`
  + `SCM_RIGHTS` + ptrace 트램펄린)일 뿐, 실제 커널 마운트 0.
- 권한: persistable tree URI(사용자 선택 트리 한정). proxy fd도 그 URI에서만 파생
  → 광역 저장소 권한 없음. `MANAGE_EXTERNAL_STORAGE` 미사용.
- fd-주입 안전성: SCM_RIGHTS 채널은 supervisor↔게스트 _자기 자식_ 간(부팅 시 상속
  한 socketpair). 외부 프로세스 fd 주입 아님. `pidfd_getfd`(권한 요건 불충족)
  미사용.
- traversal/escape/ro: §5 불변식 — proxy도 copy도 `decide`가 모드 분기 전에
  `resolve`로 강제. SUBSTITUTE_FD는 `/mnt/android/<label>` prefix 안에서만.
- W^X 무관(파일 I/O fd). 단계 2+ 트램펄린은 코드 실행 _주입_이 아니라 게스트의
  _기존_ recvmsg 호출 유도(새 실행 페이지 0).
