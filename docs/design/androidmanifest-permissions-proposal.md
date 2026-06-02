# AndroidManifest 권한 추가 제안 (T4)

**상태: 제안만.** AndroidManifest.xml 직접 수정 금지(다른 트랙 소유 파일).
이 문서는 UX 레이어가 활성화될 때 *어떤 권한을 어떤 순서로 켤지* 의 청사진이다.
실제 `<uses-permission>` 추가는 통합 세션이 결정·반영한다.

배경 모델 = `docs/design/permission-mapping.md` + `tools/permission_map.py`.

---

## 0. 현재 Manifest (있는 그대로)

```xml
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
```

현 단계는 "검증 하니스" — UI/런처/권한프롬프트 0. 그리고
`tests/test_android_permission_model.py` 가 **광범위 저장소·기타 dangerous 권한
미선언** 을 적극적으로 강제한다(`MANAGE_EXTERNAL_STORAGE`,
`READ/WRITE_EXTERNAL_STORAGE`, `SYSTEM_ALERT_WINDOW`,
`REQUEST_INSTALL_PACKAGES` 가 manifest 에 있으면 테스트 실패). 즉 **지금 이
제안의 어떤 항목도 곧바로 manifest 에 넣으면 기존 host 테스트가 깨진다.** 아래
"도입 게이트"를 반드시 거친다.

---

## 1. 분류별 제안

### A. 유지 (이미 있음, 변경 없음)
| 권한 | 부여 | 정당화 |
|---|---|---|
| `INTERNET` | install-time | apt/카탈로그 다운로드 + 게스트 네트워크. 이미 선언, 유지 |
| `ACCESS_NETWORK_STATE` | install-time | 네트워크 가용성 점검(다운로드 전 오프라인 안내) |

### B. 게스트 능력이 실제로 필요해질 때 추가 (런타임 dangerous)
각 항목은 **해당 능력을 쓰는 게스트 앱이 카탈로그에 처음 들어올 때** 추가.
선언 후에도 부여는 사용자 첫 사용 시 프롬프트(자동 부여 아님).

| 권한 | 부여 | 게스트 부류 | 정당화 / 도입 조건 |
|---|---|---|---|
| `CAMERA` | runtime | camera | 카메라 쓰는 게스트(영상통화/스캐너)가 카탈로그에 생길 때만 |
| `RECORD_AUDIO` | runtime | microphone | 녹음/음성입력 게스트가 생길 때만 |
| `ACCESS_COARSE_LOCATION` | runtime | location | 위치 게스트(지도 등). 거친 위치 |
| `ACCESS_FINE_LOCATION` | runtime | location | 정밀 위치. coarse 와 쌍으로 선언 |
| `POST_NOTIFICATIONS` | runtime (API33+) | notifications | 알림 게시 게스트. 대상 기기 Android16 이라 사실상 항상 런타임 |
| `READ_MEDIA_IMAGES` | runtime (API33+) | storage-read (SAF 불가) | **SAF 로 못 덮는 라이브러리-열람형 게스트** 한정. 기본은 SAF 라 보통 불필요 |
| `READ_MEDIA_VIDEO` | runtime (API33+) | storage-read (SAF 불가) | 위와 동일, 영상 |
| `READ_MEDIA_AUDIO` | runtime (API33+) | storage-read (SAF 불가) | 위와 동일, 오디오 |

> 미디어 3종은 **마지막 수단**이다. 저장소 접근의 기본·권장 경로는 SAF(C 항).
> 미디어 권한은 "사용자 미디어 라이브러리 전체를 인덱싱해야 하는" 소수 게스트만.

### C. SAF 로 회피 (manifest 권한 추가 안 함)
| 게스트 부류 | 회피 메커니즘 | 정당화 |
|---|---|---|
| `storage-read` | `ACTION_OPEN_DOCUMENT` / `ACTION_OPEN_DOCUMENT_TREE` | 사용자가 고른 문서/폴더 URI 만 게스트에 노출 → 최소권한, manifest 저장소 권한 0 |
| `storage-write` | SAF 쓰기 가능 URI (`ACTION_CREATE_DOCUMENT` / 트리) | scoped storage(API29+)에서 `WRITE_EXTERNAL_STORAGE` 는 무력 → SAF 가 유일한 정상 경로 |

SAF 는 권한이 아니라 인텐트라 manifest 변경이 없다. 받은 URI 는 §5 계약 P-3
(`bindSafUri`)으로 런타임 path 중재에 연결.

### D. 명시적 비채택 (넣지 않음)
| 권한 | 비채택 사유 |
|---|---|
| `MANAGE_EXTERNAL_STORAGE` | 광범위 저장소. SAF 로 대체 가능 + Play 정책 위험 + 기존 테스트가 금지 |
| `READ_EXTERNAL_STORAGE` / `WRITE_EXTERNAL_STORAGE` | API33+ 에서 미디어 세분화/scoped 로 대체. 레거시 |
| `SYSTEM_ALERT_WINDOW` | 오버레이 불필요(데스크탑형 UX 는 자체 컴포지터 SurfaceView 안에서 그림) |
| `REQUEST_INSTALL_PACKAGES` | 게스트는 APK 가 아니라 인앱 카탈로그/apt 로 설치 → 불필요 |
| `QUERY_ALL_PACKAGES` | 타 앱 조회 불필요 |
| `FOREGROUND_SERVICE` | (미래) 백그라운드 게스트 유지가 필요해지면 재검토. 현재 범위 밖 |

---

## 2. 제안 manifest 스니펫 (참고용, 즉시 적용 금지)

UX 레이어가 켜지고 도입 게이트를 통과한 뒤, **게스트 능력이 실제 필요한 만큼만**
점진 추가하는 형태. 한 번에 다 넣지 않는다.

```xml
<!-- 유지(현행) -->
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />

<!-- 아래는 해당 능력을 쓰는 게스트가 카탈로그에 들어올 때 비로소 추가 -->
<!-- <uses-permission android:name="android.permission.CAMERA" /> -->
<!-- <uses-permission android:name="android.permission.RECORD_AUDIO" /> -->
<!-- <uses-permission android:name="android.permission.ACCESS_COARSE_LOCATION" /> -->
<!-- <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" /> -->
<!-- <uses-permission android:name="android.permission.POST_NOTIFICATIONS" /> -->
<!-- SAF 로 못 덮는 라이브러리-열람형 게스트 한정: -->
<!-- <uses-permission android:name="android.permission.READ_MEDIA_IMAGES" /> -->
<!-- <uses-permission android:name="android.permission.READ_MEDIA_VIDEO" /> -->
<!-- <uses-permission android:name="android.permission.READ_MEDIA_AUDIO" /> -->
```

저장소(storage-read/write)는 위 목록에 **없다** — 의도된 것(SAF 로 처리).

---

## 3. 도입 게이트 (반드시 선행)

manifest 에 B 항 권한을 실제로 넣으려면 통합 세션이 다음을 함께 처리해야 한다.

1. **기존 테스트 갱신**: `tests/test_android_permission_model.py` 는 현재
   - `permission runtime dangerous requested=false`
   - 광범위/미디어 권한 부재
   를 강제한다. dangerous 권한을 선언하는 순간 이 테스트가 깨지므로,
   해당 단언을 "선언된 dangerous 권한이 매핑 표(§permission-mapping)에 정당화돼
   있을 것"으로 **재정의**해야 한다. (이 트랙은 그 테스트를 소유하지 않으므로
   직접 수정하지 않음 — 통합 세션 결정.)
2. **MainActivity 권한 요청 코드**: 현재 `requestedPermissionNames()` 는 선언된
   권한을 **읽기만** 한다. 실제 런타임 프롬프트(`RequestMultiplePermissions`) +
   rationale UI 는 아직 없다 → UX 레이어 통합 시 추가.
3. **런타임 enforcement 연결**: §5 계약 P-1/P-2/P-3 을 WS-1 중재에 wire.
   이게 없으면 "권한은 받았지만 게스트가 그 능력을 실제로 못 쓰는" 반쪽 상태.
4. **Play 정책 점검**: CAMERA/RECORD_AUDIO/LOCATION 은 데이터 안전 섹션 +
   민감 권한 정당화가 필요. 카탈로그에 그 능력 게스트가 실제 있을 때만 선언해
   "선언했는데 안 쓰는" 정책 위반을 피한다.

---

## 4. 요약

* 지금 추가할 것: **없음**(현 단계는 하니스, 기존 테스트가 dangerous 권한
  부재를 강제).
* 매핑 모델은 `permission_map.py` 로 미리 확정·검증(21 테스트 통과).
* 향후 추가는 **게스트 능력 출현 시점에 한 권한씩**, SAF 우선, 미디어 권한은
  최후수단. 저장소는 영구적으로 SAF.
* 모든 dangerous 권한은 런타임 enforcement(§5 계약) 와 함께만 의미가 있다.
