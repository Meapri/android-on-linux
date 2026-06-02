# T5 — SAF 파일 브리지 (안드로이드 저장소 ↔ 게스트 rootfs)

상태: 설계 + host-검증 순수 모델(`tools/saf_bridge_model.py`,
`tests/test_saf_bridge_model.py`, 32 pass). UI/런타임 배선은 미구현(제안만).
소유: research/product-ux. **MainActivity/Manifest/RootfsInstaller 직접 수정 없음**
(필요 변경은 §8 제안으로만).

## 0. 문제

ALR은 비root Android(SM-X236N, Android16)에서 glibc arm64 앱을 in-process로
돌린다. 게스트는 app-private `files/rootfs/debian-arm64/` 안에서만 산다. 사용자가
폰에 가진 **사진/다운로드/문서**를 게스트 앱(GIMP, 텍스트 에디터 등)에서 열고
저장하려면 다리가 필요하다.

근본 제약: **비root + public API only**.
- `mount`/FUSE/bind-mount = 불가(루트 필요, 비공개 syscall).
- 앱 외부 저장소 직접 접근 = Android 10+ Scoped Storage로 막힘.
- `MANAGE_EXTERNAL_STORAGE`("모든 파일 접근") = Play 정책상 거의 거절 + 과도 권한.

남는 정공법은 **Storage Access Framework(SAF)** 와 **MediaStore**:
사용자가 명시적으로 고른 디렉토리/파일에 대해서만, ContentResolver를 통해
스트림으로 읽고 쓴다. 권한 프롬프트가 사용자 의사 표시이고, 권한 범위가
사용자가 고른 트리로 한정된다(최소 권한·UX-친화).

## 1. SAF 기본기 (어떤 public API를 쓰나)

| 목적 | Intent / API | 결과물 | 영속성 |
|------|--------------|--------|--------|
| 디렉토리 트리 노출 | `ACTION_OPEN_DOCUMENT_TREE` | tree URI (`content://…/tree/…`) | `takePersistableUriPermission` 으로 재부팅 후도 유지 |
| 파일 1개 열기 | `ACTION_OPEN_DOCUMENT` | document URI | persistable 가능 |
| 파일 1개 저장 | `ACTION_CREATE_DOCUMENT` | document URI | 1회용 |
| 트리 내 탐색 | `DocumentFile.fromTreeUri(uri)` → `listFiles()`, `findFile()`, `createFile()` | child URI | tree 권한 따라감 |
| 바이트 읽기/쓰기 | `ContentResolver.openInputStream/openOutputStream(uri)` | `FileDescriptor`/stream | — |
| 공유 받기 | `ACTION_SEND` / `ACTION_SEND_MULTIPLE` (`EXTRA_STREAM`) | content URI(1회용) | 비영속 |
| 미디어 저장 | `MediaStore.<col>.EXTERNAL_CONTENT_URI` + `ContentValues(RELATIVE_PATH)` | inserted URI | 영속(컬렉션 소유) |

핵심: SAF는 **경로가 아니라 URI** 다. POSIX `open("/sdcard/...")` 같은 건
존재하지 않는다. 게스트는 POSIX 경로로 파일을 여는데, 그 경로를 URI+스트림으로
번역하는 어댑터가 이 브리지다.

## 2. 대안 비교

### (a) 복사 in/out (copy-on-open / copy-on-save)
사용자가 SAF로 연 파일을 rootfs의 임시 위치로 **복사**해 게스트에 넘기고,
게스트가 저장하면 다시 SAF URI로 **복사**해 돌려보낸다.

- 장점: 단순. 게스트는 평범한 rootfs 파일만 본다. 런타임 path 중재(WS-1) 무관.
  부분 읽기/seek/mmap 전부 로컬 파일이라 100% 호환. 구현 위험 최소.
- 단점: **양방향 동기화 문제**. 게스트가 저장 후에도 원본은 안 바뀜(명시적
  export 필요). 큰 파일 2배 저장공간 + 복사 지연. "진짜 파일을 편집하는 느낌"
  부족. 외부에서 원본이 바뀌어도 게스트는 모름(stale).

### (b) 런타임이 게스트 open을 SAF로 프록시 (virtual mount)
게스트가 `/mnt/android/<label>/...` 를 open하면 WS-1 path 중재가 그걸 가로채
SAF DocumentFile/스트림으로 프록시한다. 게스트엔 **진짜 마운트처럼** 보인다.

- 장점: 진짜 마운트感. 복사·중복저장 없음. 저장 즉시 원본 반영(동기화 문제 없음).
  데스크탑 UX에 가장 근접.
- 단점: **복잡**. SAF는 스트림(순차 I/O) 중심 — `mmap`, 임의 `seek`, `O_APPEND`,
  `rename`, 권한 비트, inode 의미를 완벽히 못 준다. content URI는 `pread`/`pwrite`가
  ParcelFileDescriptor로 가능하나, SAF document는 일반적으로 stream만 보장.
  WS-1 path 중재(seccomp openat trap → in-place rewrite)는 **경로를 다른 경로로
  바꾸는** 모델이라, "경로를 fd/스트림으로 바꾸는" 프록시는 trap 핸들러가
  openat 결과 fd를 ALR가 만든 fd로 치환해야 함(별도 메커니즘 = §5-F).

### (c) 하이브리드 (권장)
- 기본 = **(a) 복사**, 단 SAF tree 마운트 라벨(`/mnt/android/<label>`)을 게스트에
  보여 주고, 그 아래 접근은 **열 때 copy-in / 저장 시 copy-out(write-back)** 으로
  처리. 게스트엔 디렉토리가 보이고(listing은 DocumentFile로 즉시), 실제 파일
  바이트는 처음 open 시 lazy copy-in, close/fsync 시 write-back.
- 고급 = 특정 마운트를 (b) 직통 프록시로 승격(읽기 전용 미디어처럼 seek-light한
  경우). 런타임이 §5-F 계약으로 "이 경로는 프록시"를 받으면 fd 치환.

→ **권장: (c). 1단계는 (a)로 출시(위험 최소·UX 충분), 마운트 라벨 + write-back으로
"디렉토리가 보이고 저장이 원본에 반영되는" 느낌까지 확보. 직통 프록시(b)는
seek/mmap 의존 앱이 실제로 깨질 때만 마운트 단위로 켠다.**

## 3. 경로공간 매핑 모델

```
안드로이드(SAF tree URI)                게스트 rootfs 경로공간
content://…/tree/primary%3ADownload  ⇄  /mnt/android/downloads   (rw)
content://…/tree/primary%3ADCIM      ⇄  /mnt/android/dcim        (ro)
content://…/tree/primary%3ADocuments ⇄  /mnt/android/docs        (rw)
ACTION_SEND 로 받은 1회용 파일        →  /root/Android-Share/<name>  (in, 단방향)
게스트 출력 파일                      →  MediaStore.<col> insert       (out, 단방향)
```

- 게스트가 보는 마운트 루트 = `SAF_MOUNT_ROOT = /mnt/android`. 그 아래 한 단계가
  `label`(사용자가 고른 트리의 사람이 읽는 이름).
- `label` 아래 **상대 경로(rel)** 컴포넌트만 SAF DocumentFile 트리로 내려간다:
  `DocumentFile.fromTreeUri(uri)` 에서 `rel_components` 를 `findFile`/`createFile`로
  따라간다.
- 디렉토리 정책: `READ_ONLY`(사진 등 보호) / `READ_WRITE`(작업 폴더).

이 매핑·정책·안전성 검사가 `tools/saf_bridge_model.py` 의 순수 로직이다(아래 §4).

## 4. 순수 모델 (`tools/saf_bridge_model.py`)

런타임/UI와 독립적인 **번역·정책·안전성** 로직. Android API 호출 0.

- `SafMount(tree_uri, label, mode)`: 마운트 1개. `label` 형식 검증
  (`[A-Za-z0-9._-]+`, 경로 컴포넌트 안전), `tree_uri`가 SAF tree URI인지 검증.
- `SafBridge`: `label→SafMount` 등록부.
  - `resolve(guest_path, op) -> ResolvedPath`: 게스트 절대경로 + 연산 의도를
    `(tree_uri, label, mode, rel_components)` 로 번역.
    - `/mnt/android` 밖 → `BridgeError(NOT_MAPPED)` ⇒ 런타임은 **기존 rootfs 중재로
      폴백**(여기가 핵심 분기점).
    - 없는 label → `UNKNOWN_LABEL`.
    - `..` 로 마운트/루트 밖 탈출 → `ESCAPE` 또는 `NOT_MAPPED`(정규화 결과 마운트
      밖이면).
    - `ro` 마운트 + 변형 연산(write/create/delete) → `READ_ONLY_VIOLATION`.
  - `is_saf_path(p)`: 분류용(정책 무시) — 런타임이 "이 경로 내가 처리?"를 싸게 판단.
- `normalize_guest_path`: `.` 제거, `..` pop, 루트 위 탈출은 `ESCAPE`로 거부
  (런타임 `alr_path` 의 clamp-at-guest-root 와 같은 의미, 단 마운트 경계 보호를
  위해 명시적 에러).
- share intent: `route_incoming_share(IncomingShare)` → `/root/Android-Share/<safe>`.
  `sanitize_share_name` 가 경로 구분자/`..`/위험문자 제거(traversal 불가).
- MediaStore export: `export_to_mediastore(...)` → `MediaStoreTarget`(컬렉션 화이트
  리스트 + display_name 안전화).

**안전성 불변식(테스트로 고정, §test):**
1. 게스트가 준 어떤 경로도 `/mnt/android/<label>` 밖(rootfs/Android FS)을 만질 수
   없다(NOT_MAPPED로 SAF 밖, ESCAPE로 traversal).
2. `ro` 마운트는 어떤 변형 연산도 거부.
3. share/export 파일명은 traversal·구분자 제거 후 고정 디렉토리 한 단계에만 떨어짐.

## 5. 트레이드오프 요약

| 축 | (a) 복사 | (b) 직통 프록시 | (c) 하이브리드 |
|----|---------|----------------|----------------|
| 구현 난이도 | 낮음 | 높음(fd 치환·mmap) | 중간 |
| 동기화 일관성 | 약함(명시 export) | 강함(즉시) | write-back으로 강함 |
| 호환성(seek/mmap) | 완벽(로컬) | 부분(SAF 제약) | 완벽(로컬) + 옵트인 프록시 |
| 성능(대용량) | 복사 비용 2× | 스트림 직통 | 첫 open만 복사 |
| "진짜 마운트感" | 약함 | 강함 | 중간~강함 |
| 비root 안전성 | ✅ | ✅ | ✅ |

## 6. 권장안 + 단계

1. **1단계(출시 최소, 위험 최소)** — copy-in/out + 마운트 라벨.
   - 설정에 "폴더 추가"(`ACTION_OPEN_DOCUMENT_TREE`) + `takePersistableUriPermission`.
   - 라벨/모드를 `SafBridge` 에 등록(영속: persisted URI 목록을 앱이 저장).
   - 게스트 파일 선택 UI(또는 앱 런처의 "파일 열기")에서 SAF 파일 1개 선택 →
     `route`로 rootfs 임시 경로에 copy-in → 게스트에 그 경로 전달.
   - 게스트 저장 결과 → write-back(원래 tree URI) 또는 MediaStore export.
2. **2단계** — 디렉토리 listing을 게스트에 노출(DocumentFile.listFiles → 게스트가
   `/mnt/android/<label>` 를 `ls` 하면 가상 디렉토리 엔트리 제공). 여전히 파일
   바이트는 lazy copy-in/write-back.
3. **3단계(옵트인)** — §5-F 계약으로 WS-1 path 중재와 결합해, 특정 마운트를 직통
   프록시(읽기 위주)로 승격. seek/mmap 의존 앱이 깨질 때만.
4. share/export는 1단계부터: `ACTION_SEND` 수신 인텐트필터(§8 제안) +
   "안드로이드로 내보내기"(MediaStore).

## 7. 런타임 인터페이스 계약 (§5-F)

WS-1 path 중재(seccomp openat trap → 부모 supervisor가 게스트 경로를 rootfs host
경로로 **in-place rewrite**: `runtime_report.cpp` `alr sc path_rewrites`,
`__NR_openat`/`openat2` trap)와 SAF 브리지의 접점. §5(인터페이스 계약) 형식.

### F. `SafPathBridge` (UI/앱 레이어 제공 → WS-1 런타임 소비)

게스트 openat 경로를 받아 **(1) rootfs 경로로 rewrite 하거나, (2) ALR가 연
fd로 치환** 하라고 알려 주는 결정 훅. 헤더 1개로 고정, 변경은 통합 세션 승인.

제안 시그니처(C++ 측, runtime_report supervisor가 trap 핸들러에서 호출):

```cpp
// 게스트가 openat(path, flags) 를 trap 했을 때의 결정.
enum class SafDecision {
    kFallthrough,   // SAF 무관: 기존 rootfs path rewrite 로 처리(현행 동작)
    kRewritePath,   // SAF지만 copy-in 완료: host_path 로 in-place rewrite
    kSubstituteFd,  // 직통 프록시: ALR가 연 fd 를 게스트 fd 로 치환
    kDeny,          // 정책 위반(ro 위반/escape): EACCES/EROFS 로 실패시킴
};
struct SafResolveResult {
    SafDecision decision;
    std::string host_path;  // kRewritePath 일 때만(rootfs 안 절대경로)
    int fd;                 // kSubstituteFd 일 때만(ALR 소유, dup 해 넘김)
    int deny_errno;         // kDeny 일 때(EACCES=13, EROFS=30)
};
// flags: O_WRONLY/O_RDWR/O_CREAT 등으로 read/write 의도 판별.
SafResolveResult alr_saf_resolve(const char* guest_path, int flags);
```

보장/책임 경계:
- **런타임(WS-1)**: openat trap 시점에 `alr_saf_resolve` 를 부르고, 반환 decision대로
  처리. `kRewritePath` 면 기존 in-place rewrite 경로 재사용(이미 구현됨 —
  `path_rewrites`). `kSubstituteFd`/`kDeny`는 후속(현 trap은 경로 rewrite만 하므로
  3단계에서 trap 핸들러 확장 — runtime_report `loader-feature-gaps` 항목).
  런타임은 SAF/URI/ContentResolver를 **모른다**.
- **브리지(이 레이어)**: `alr_saf_resolve` 안에서 `SafBridge.resolve(path, op)` 로
  분류·정책 강제. SAF면 copy-in(JNI로 Android ContentResolver 호출 →
  `ParcelFileDescriptor`/스트림 → rootfs 임시파일 또는 fd) 후 decision 구성. 아니면
  `kFallthrough`. URI↔fd↔copy 모두 브리지 책임.
- **decision은 1차로 copy 모델(`kRewritePath`)만 사용** — WS-1 trap 핸들러 변경 0.
  `kSubstituteFd`는 trap 핸들러가 fd 주입을 지원하게 된 뒤(별도 ADR/큰 변경).

매핑 규칙(모델 ↔ decision):
| `SafBridge.resolve` 결과 | flags | decision |
|---|---|---|
| `BridgeError(NOT_MAPPED)` | any | `kFallthrough` |
| ok, ro 마운트, O_WRONLY/O_RDWR/O_CREAT | write | `kDeny(EROFS)` |
| ok, copy 모드 | any | copy-in 후 `kRewritePath(host_path)` |
| ok, 프록시 모드(3단계) | read-heavy | `kSubstituteFd(fd)` |
| `ESCAPE`/`READ_ONLY_VIOLATION` | — | `kDeny(EACCES/EROFS)` |

이 경계 덕에 WS-1은 "경로를 경로로 바꾸는" 현행 모델을 그대로 두고(1단계),
브리지는 host에서 모델/카피 로직을 독립 개발·검증할 수 있다(현 PR =
`saf_bridge_model.py` 가 그 host 절반).

## 8. UI/Manifest 변경 제안 (직접 수정 금지 — 통합 세션 결정)

브리지 동작에 필요한, 다른 트랙 소유 파일의 변경 **제안**:

1. `AndroidManifest.xml` — share 수신 인텐트필터(`ACTION_SEND`/`SEND_MULTIPLE`,
   `mimeType="*/*"`) 를 게스트 앱 런처/파일 핸들러 액티비티에 추가.
   `MANAGE_EXTERNAL_STORAGE` 는 **추가하지 않음**(SAF/MediaStore로 충분, 정책 위험).
   READ_MEDIA_* 도 불필요(SAF가 사용자 동의로 대체).
2. `MainActivity`(또는 신규 SettingsActivity) — `ACTION_OPEN_DOCUMENT_TREE` 런처
   (registerForActivityResult) + `takePersistableUriPermission` + 라벨/모드 입력 →
   `SafBridge` 등록. 영속: persisted URI 목록(SharedPreferences/파일).
3. 런타임 JNI — `alr_saf_resolve`(§7-F) C++ 훅 + Android측 copy-in/out JNI 콜백
   (ContentResolver). 1단계는 copy 모델만.

## 9. 비root/안전성 점검
- 마운트/FUSE 0. 전부 public SAF(`DocumentFile`, `ContentResolver`) + MediaStore.
- 권한: persistable URI permission(사용자가 고른 트리에 한정). 광역 저장소 권한 없음.
- traversal·escape: 모델이 `..`/구분자/심볼릭 이스케이프를 거부(§4 불변식, 32 test).
- W^X 무관(파일 I/O만).
