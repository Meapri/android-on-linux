# ALR 권한 매핑 모델 (T4)

대상: ALR 런타임 앱(`dev.chanwoo.androlinux`) 이 게스트 Linux 앱을 호스팅할 때
필요한 Android 권한을 **어떻게 매핑·요청·강등(graceful degrade)** 하는지의 설계.

상태: **모델/제안**. 현재 manifest(INTERNET/ACCESS_NETWORK_STATE 2개)는 바꾸지
않는다. 순수 매핑 로직 = `tools/permission_map.py`(host pytest 검증,
`tests/test_permission_map.py` 21 통과). manifest 변경 제안은
`docs/design/androidmanifest-permissions-proposal.md` 참고.

---

## 1. 패키징 모델이 권한에 주는 함의

확정 패키징 = **"런타임 + 인앱 카탈로그"**: ALR 런타임 앱 하나가 모든 게스트
Linux 앱을 in-process 로 실행한다. 게스트 앱은 별도 APK 가 아니다. 따라서:

* **Android 권한은 런타임 앱 단위로만 존재한다.** 게스트 GIMP/터미널/브라우저가
  각자 카메라·위치 권한을 따로 가질 수 없다. ALR 런타임 manifest 의 권한 집합 =
  "런타임이 게스트들에게 빌려줄 수 있는 능력의 상한선".
* **게스트 앱별 권한 게이트는 ALR 런타임이 직접 한다.** 게스트가 카메라
  syscall/`/dev` 접근을 시도하면, 그 게스트에게 사용자가 허용했는지를 ALR 가
  판단해 통과/차단한다. 이 enforcement 는 **WS-1 (path/syscall 중재) 런타임에
  의존**한다 → §5 계약(아래 6장)으로 분리.
* 결과적으로 권한 모델은 2계층이다:
  1. **OS 계층** — Android 가 ALR 런타임 앱에게 부여(install-time/runtime/SAF).
  2. **앱-카탈로그 계층** — ALR 가 개별 게스트 앱에게 그 능력을 재배분(런타임 게이트).

이 문서/`permission_map.py` 는 **(1) OS 계층** 의 매핑을 정의한다. (2) 의 정책
(어느 게스트에게 무엇을 허용)은 카탈로그 메타데이터 + ALR 런타임의 몫이다.

---

## 2. 게스트 권한 부류 → Android 매핑 표

| 게스트 부류 | API≥33 (TIRAMISU) | API≤32 | 부여 방식 | 비고 |
|---|---|---|---|---|
| `network` | `INTERNET` | 동일 | **install-time** | normal 권한, 프롬프트 없음. apt/카탈로그/게스트 net |
| `storage-read` (SAF) | — (권한 불요) | — | **SAF** | 기본·권장. ACTION_OPEN_DOCUMENT/TREE |
| `storage-read` (SAF 미사용) | `READ_MEDIA_IMAGES`/`VIDEO`/`AUDIO` | `READ_EXTERNAL_STORAGE` | runtime | 라이브러리-열람형 잔여 케이스 한정 |
| `storage-write` | — (항상 SAF 강제) | — (항상 SAF 강제) | **SAF** | scoped storage 에서 외부쓰기 권한 무력 |
| `camera` | `CAMERA` | 동일 | runtime | dangerous |
| `microphone` | `RECORD_AUDIO` | 동일 | runtime | dangerous |
| `location` | `ACCESS_COARSE_LOCATION` + `ACCESS_FINE_LOCATION` | 동일 | runtime | 사용자가 정밀/거친 선택 |
| `notifications` | `POST_NOTIFICATIONS` | — (권한 불요) | runtime / 없음 | API33+ 만 런타임 권한 |

부류 별칭(메타데이터 표기 흔한 변형)도 받는다: `storage`/`read-storage`→read,
`write-storage`→write, `mic`/`record-audio`→microphone, `gps`/`geolocation`→location,
`internet`/`net`→network, `notify`/`notification`→notifications, `webcam`→camera.
알 수 없는 토큰은 `UnknownGuestPermission` 으로 **거부**(허용 목록 방식).

---

## 3. API 레벨 분기 (Build.VERSION.SDK_INT)

* **API 33 (Android 13, TIRAMISU)** 기준으로 두 가지가 바뀐다:
  - 외부 미디어 읽기 권한이 `READ_EXTERNAL_STORAGE` → `READ_MEDIA_IMAGES` /
    `READ_MEDIA_VIDEO` / `READ_MEDIA_AUDIO` 로 세분화.
  - 알림 게시가 권한 불필요 → `POST_NOTIFICATIONS` **런타임 권한**으로 승격.
* **API 34 (Android 14)** 에서 `READ_MEDIA_VISUAL_USER_SELECTED`(부분 선택)도
  존재하나, ALR 의 기본 저장소 경로는 SAF 라 보통 불필요. 상수만 모듈에 둠.
* 대상 기기 SM-X236N = **Android 16** → 항상 API≥33 분기. API≤32 분기는
  하위호환/이론적 완결성용(테스트로 회귀 방지).

런타임은 `required_permissions(guest_set, api_level=Build.VERSION.SDK_INT,
storage_via_saf=...)` 한 번 호출로 (a) manifest 선언 후보, (b) 첫 사용 시
런타임 프롬프트 목록, (c) SAF 사용 여부를 모두 얻는다.

---

## 4. 권한 요청 플로우

```
앱(게스트) 첫 실행
   │
   ├─ 카탈로그 메타데이터에서 게스트 권한 부류 집합 읽기
   │     (예: ["network","storage-read","notifications"])
   │
   ├─ required_permissions(set, api_level=SDK_INT, storage_via_saf=true)
   │     → install_time / runtime / SAF 분류
   │
   ├─ install-time(INTERNET): 이미 manifest 선언 → 부여됨, 아무 동작 없음
   │
   ├─ runtime 권한들: 아직 미허용분만 ActivityResultContracts.RequestMultiplePermissions
   │     로 일괄 프롬프트 (게스트 앱 컨텍스트를 설명하는 rationale UI 선행)
   │
   ├─ SAF(storage): 게스트가 실제로 파일 열기를 시도하는 그 순간
   │     ACTION_OPEN_DOCUMENT / OPEN_DOCUMENT_TREE 인텐트로 사용자 선택
   │     → 받은 URI 를 ALR 가 게스트 경로로 중재(런타임 의존, §5)
   │
   └─ 거부 시 graceful degrade (5장)
```

핵심 원칙:
* **권한은 게스트 앱 첫 사용 시점에 요청**(설치 시 X). 런타임 앱 자체는 권한
  스플래시를 안 띄운다 — 데스크탑형 UX 라, 앱을 실제로 켤 때 맥락과 함께 묻는다.
* runtime 권한은 항상 **rationale 먼저**(왜 이 게스트가 카메라를 원하는지) →
  그 다음 시스템 프롬프트. `shouldShowRequestPermissionRationale` 활용.
* storage 는 권한이 아니라 **SAF 선택 인텐트**로 가는 게 기본. 사용자가 고른
  문서/트리 URI 만 게스트에 노출 → 최소권한.

---

## 5. 거부 시 graceful degrade

| 부류 | 거부 시 ALR 동작 |
|---|---|
| `network` | install-time 이라 거부 불가(선언만으로 부여). 비행기모드 등은 게스트의 `connect()` 가 자연스럽게 실패 → 게스트 앱이 처리 |
| `storage`(SAF 취소) | 게스트의 해당 `open()` 에 EACCES/ENOENT 반환(런타임 중재) → 게스트 앱은 "파일 없음"으로 인지, 크래시 없이 진행 |
| `camera` | 게스트의 `/dev/video*` open / V4L2 ioctl 에 EACCES → 카메라 기능만 비활성, 앱 본체 동작 |
| `microphone` | 오디오 캡처 경로에 EACCES → 녹음만 비활성 |
| `location` 정밀 거부 | 거친 위치만 제공(부분 허용); 전체 거부 시 위치 API 에 권한오류 → 게스트가 기본/마지막 위치로 폴백 |
| `notifications` | POST 시 무시(시스템이 드롭) → 게스트는 게시 성공으로 간주, 사용자에게만 안 보임 |

**불변식**: 권한 거부가 게스트 프로세스를 죽이면 안 된다. ALR 의 syscall 중재는
권한 없는 능력 호출에 대해 **권한 오류 코드를 합성해 반환**(SIGKILL/차단 아님)해
게스트의 정상 에러 경로를 타게 한다. 이게 graceful degrade 의 메커니즘이고,
그 enforcement 는 런타임(§6) 책임.

---

## 6. 런타임 의존 경계 — §5 계약 (UI ↔ 런타임)

UX 레이어(이 트랙)는 **무엇을 요청/표시**할지를 정한다. **실제 능력 게이트**
(게스트 syscall 이 권한을 만났을 때의 enforcement)는 WS-1(path/syscall 중재)
런타임에 있다. 경계 계약:

```
계약 P-1 (UI → 런타임): 게스트별 허용 능력 비트셋 전달
  fun setGuestCapabilities(guestId: String, caps: GuestCapabilitySet)
  - caps = {network, storageUris: List<SafUri>, camera, microphone,
            location: {coarse,fine}, notifications}
  - UI 가 권한 프롬프트/SAF 결과를 모아 호출. 런타임은 이 비트셋을 게스트
    프로세스의 중재 정책으로 적용.

계약 P-2 (런타임 → UI): 게스트가 미허용 능력을 시도했을 때 콜백(선택)
  interface CapabilityDemandListener {
      fun onGuestDemandsCapability(guestId: String, cap: GuestPermission)
  }
  - 런타임이 게스트의 능력 호출을 가로채 미허용이면 (a) 권한오류 합성 반환 +
    (b) 이 콜백으로 UI 에 통지 → UI 가 "지금 허용?" 사후 프롬프트를 띄울 수 있음.
  - mock: 콜백 없이 항상 EACCES 합성하는 stub 으로 UI 단독 개발/테스트 가능.

계약 P-3 (UI → 런타임): SAF URI 를 게스트 경로로 바인드
  fun bindSafUri(guestId: String, safUri: Uri, guestPath: String)
  - 사용자가 SAF 로 고른 문서/트리를 게스트 파일시스템의 한 경로로 노출.
  - 런타임의 path 중재(이미 존재: openat 재작성)가 이 바인딩을 따른다.
```

이 계약들은 **미리 정의만** 한다. 실제 구현은 통합 세션에서 WS-1 중재 코드와
연결. UX 레이어는 P-1/P-3 를 호출하는 쪽, P-2 를 받는 쪽으로 골격을 짤 수 있고,
런타임이 없을 때는 위 mock 으로 단독 검증 가능.

---

## 7. 순수 로직 산출물

* `tools/permission_map.py` — `required_permissions(guest_set, api_level,
  storage_via_saf)` → `PermissionPlan`. install-time/runtime/SAF 분류,
  API 분기, 별칭/미지 토큰 처리. **런타임 의존 없음, host 검증됨.**
* `tests/test_permission_map.py` — 21 케이스(매핑 정확성, API33 세분화,
  미지 거부, SAF→권한불요, network→install-time, 결합/중복/정렬).

이 로직은 그대로 Kotlin 으로 옮기거나(같은 표를 코드 생성), 빌드 시 manifest
제안을 자동 산출하는 데 쓸 수 있다. 현재는 모델/검증 단계라 Kotlin 미생성.
