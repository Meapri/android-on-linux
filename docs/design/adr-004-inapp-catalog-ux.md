> 생성 경위: T1(제품 UX 전체 아키텍처) 설계 산출. 격리 브랜치 `research/product-ux`(base `main` d749073), 읽기 전용으로 현 프론트(`MainActivity.kt`·`AndroidManifest.xml`·`RootfsInstaller.kt`·`RootfsInstallPlan.kt`·`NativeCommandRunner.kt`)를 확인하고 작성. `MainActivity.kt`/`AndroidManifest.xml`/`RootfsInstaller.kt`는 **수정 금지** — 본 ADR은 그 변경을 _제안_으로만 기술한다. 코드 사실은 위 파일 직접 독해, 패키징 결정(런타임+인앱 카탈로그)은 찬우 확정. 리뷰 대상: UX 4트랙(매니페스트/런처/권한/SAF), WS-1(런타임 §5 계약), 통합 세션. ADR-002/003 톤 계승(device 게이트·정직 섹션·constraint 불변).
>
> **개정(R1, 2026-06-02 — 찬우 확정 5결정 명문화)**: 멀티앱 정책·UI 스택·카탈로그 단계·파일연동·백그라운드의 5개 열린 질문을 찬우가 확정. 본 개정은 그 결정을 **§0(결정 기록)**으로 명문화하고, (1)을 **§5-F AppSession 상태기계**의 `RENDERING 0..1` 불변식으로, (3)을 **§7 흐름**의 v1(stage-tar)/v2(인-게스트 apt) 로드맵으로, (4)를 **§7-SAF**의 프록시(목표)+copy(폴백) 모델로 반영. 전면 재작성 아님 — 기존 §1~§12는 결정과 정합하므로 보존, §0이 결정의 SSOT가 되고 본문은 그 구현 형태를 기술한다.

# ADR-004 — 인앱 카탈로그 UX 아키텍처: "검증 하니스 MainActivity"를 RunningSurface로 강등하고 그 앞에 Launcher/Catalog를 둔다

- 상태: **Accepted(결정 기록) / Proposed(구현)** — §0의 5결정은 찬우 확정(Accepted). 코드 변경은 없음(스켈레톤은 별도 UX 트랙이 신규 파일로) → 구현은 Proposed. device 게이트 미통과.
- 워크스트림: **UX**(신규 product-ux). 의존 공개: 런타임 §5 계약(이 ADR이 정의, WS-1이 구현 측 수락), 매니페스트 스키마(T2)·런처/카탈로그 스켈레톤(T3)·권한 플로우(T4)·SAF 임포트(T5)가 본 ADR을 SSOT로 참조.
- 작성: 2026-06-02. 선행: 찬우 패키징 결정("런타임 앱 1개 설치 → 인앱 카탈로그/apt로 리눅스 앱 설치·런처 실행, 데스크탑형"), `docs/research/orchestration-5session-plan.md` §5(레이어 인터페이스 계약)·§5-E(stage tar 규약), `docs/design/adr-003-multiprocess-exec-reentry.md`(exec/멀티프로세스 한계).
- HARD CONSTRAINTS(불변): 비root(untrusted_app), public Android API only, W^X-safe, in-process(map+jump + fork/ptrace supervisor; PRoot fallback-only), 단일 APK + 인앱 rootfs, version stamp 불변, **소유 밖 파일은 읽기만**.

---

## 0. 확정 결정 (찬우 확정 — 본 ADR의 결정 기록, SSOT)

ADR의 핵심은 _결정의 기록_이다. 아래 5개는 본 UX 레이어의 열린 질문에 대한 찬우의 확정 답이며, 본문(§1~§12)은 이 결정의 _구현 형태_를 기술한다. 각 결정 = **채택안 · 근거 · v1/v2 단계 · 좌우범위(누가 닫는가) · 미해결(device/후속)**. 결정 간 충돌 시 본 §0이 우선한다.

### D1 — 멀티앱: v1 단일 포그라운드 (동시-멀티는 v2+ 별 작업)
- **채택안**: v1은 _화면에 리눅스 앱 1개_. `RENDERING` 상태인 세션은 **항상 0 또는 1**(불변식). 두 번째 앱 실행 = 이전 앱을 `BACKGROUND`(suspend 가능 시)로 내리고 새 앱이 포그라운드 점유. 동시-멀티(데스크탑 멀티윈도우)는 v2+ 별 작업으로 승계.
- **근거**: 현 컴포지터·입력 인젝션이 _프로세스 1 · Surface 1 · 포커스 1_ 가정 위에 짜여 있음(§2·§4-C 코드 독해). 동시 2 독립 앱을 각자 윈도우로 보이려면 런타임 측 per-app surface 라우팅 + 멀티-toplevel 합성/z-order + 입력 포커스 모델이 _신규로_ 필요 → UI 재배치만으로 못 닫음. v1을 단일 포그라운드로 그으면 "재배치만으로 프로덕트가 선다"(§1)가 성립.
- **v1**: `AlrRuntime` 구현이 `RENDERING ≤ 1` 불변식을 강제(§5-F). UI는 처음부터 `sessions: List`를 다뤄, 멀티 해금이 _UI 재작성이 아니라 불변식 완화_가 되게.
- **v2+**: WS-3 멀티윈도우(M3)가 device-증명된 뒤 별 ADR로 동시-멀티 승계. §5-F가 `sessions: List`로 _표현은 미리_ 열어둠 — v1은 리스트 길이를 0..1로 제약할 뿐.
- **좌우범위**: v1 불변식 강제 = 런타임 트랙(WS-1) + 본 ADR §5-F. 동시-멀티 = WS-3 + 별 ADR(본 ADR 범위 밖).
- **미해결**: 게스트 suspend(백그라운드 보류) 가부는 device 측정(SIGSTOP/cgroup-free 환경) — D5와 함께 M-UX device 게이트로(§10).

### D2 — UI 스택: 하이브리드 (신규 화면 Compose, 실행화면 View 유지)
- **채택안**: 신규 일반 화면(Launcher/Catalog/AppDetail/Settings)은 **Jetpack Compose**. 실행화면 `RunningSurface`는 **기존 `SurfaceView`/View 유지**(컴포지터 호스트 블록을 그대로 옮김).
- **근거**: 현 프로젝트는 순수 View 기반(§4)·Compose 미사용 — 신규 화면은 Compose가 생산성↑이나, `RunningSurface`의 입력 인젝션(`nativeWaylandInject*`)이 View 콜백(`setOnTouchListener`/`setOnKeyListener`/`setOnGenericMotionListener`)에 강결합되어 있어 Compose로 옮기면 입력 경로를 재작성해야 함 → 회귀 위험. `SurfaceView`를 Compose `AndroidView`로 _감쌀 수는_ 있으나 v1은 순수 Activity+View가 안전(§4 단서).
- **v1**: Compose 신규 화면 + View `RunningSurface`. 둘은 `LaunchRequest`(§5-F)로만 만남 — UI 스택 경계가 곧 §5-F 경계.
- **v2+**: 입력 모델이 §5-F `requestForeground`로 추상화되면 `RunningSurface`도 `AndroidView` 래핑 검토 가능(비결정, 비v1).
- **좌우범위**: Compose 화면 = T3(런처/카탈로그/설정). View `RunningSurface` = 본 ADR(경계) + 통합/런타임 트랙(블록 이전, §8-개선-3).
- **미해결**: 없음(스택 결정은 host-닫힘 — 코드 위치/래핑은 §8 이전 계획에서).

### D3 — 카탈로그: apt 인덱스 기반 (v1 stage-tar 풀기, v2 인-게스트 apt)
- **채택안**: 카탈로그 = **apt 저장소 인덱스**로 목록 구성. **v1 = 인덱스로 목록을 보이되, 의존성 closure를 host(또는 빌드 시) 풀어 `stage-tar` overlay로 설치**(dpkg fork-exec 우회) — 인-게스트 dpkg/apt가 요구하는 exec re-entry 벽(ADR-003)을 _회피_. **v2 = exec re-entry가 풀린 뒤 인-게스트 apt/dpkg**로 직접 설치.
- **근거**: ADR-003이 fresh-execve 자식(dpkg가 부르는 maintainer script·`ldconfig` 등)이 device 미검증 벽임을 기록. v1에서 인-게스트 dpkg를 돌리면 그 벽에 정면 충돌 → 대신 `deb_closure`(`tools/deb_closure.py`)가 이미 `.deb` closure를 base-subtract해 **§5-E `./`-rooted overlay tar**로 평탄화하므로, "설치" = 그 stage-tar를 `RootfsInstaller.extractOverlayTar`로 푸는 것(fork-exec 0)으로 닫힘. 인덱스(apt `Packages`)는 _목록·메타·버전·크기_의 출처로만 v1에 쓰고, 트랜잭션(dpkg)은 v2로 미룸.
- **v1**: 카탈로그 인덱스 → 후보 목록. 설치 = `InstallSpec(stage-tar)` → `extractOverlayTar` + `.{name}-staged-<size>` 마커. closure 풀기는 host/빌드 측(`deb_closure`), device는 푼 tar만 받음. → **B 트랙(카탈로그 파이프라인) 신규 문서**가 인덱스→closure→stage-tar 파이프라인을 SSOT로 소유(상호참조 §13).
- **v2**: ADR-003 exec re-entry가 device-증명(M-R4-*)되면 인-게스트 `apt-get install`을 트랜잭션으로 래핑(`runProotRootfsAptGet*` 경로, `NativeCommandRunner` L72~). v1 stage-tar 경로는 오프라인/번들 설치로 잔존.
- **좌우범위**: v1 closure→stage-tar 파이프라인 = B 트랙 + WS-4(overlay 빌드). 인덱스 파싱/카탈로그 목록 = T2(매니페스트·카탈로그 모델)·T3(화면). v2 인-게스트 apt = WS-1 exec re-entry(ADR-003) 선결.
- **미해결**: v2 인-게스트 apt는 ADR-003 §4 미검증 가정(envp 전파·TRACEEXEC 자동추적)이 device로 닫혀야 착수. v1은 그 벽과 _무관_(fork-exec 0).

### D4 — 파일연동: SAF 직통 프록시(최종 목표) + copy-in/out(중간 폴백)
- **채택안**: 최종 목표 = **SAF 직통 프록시**(`ContentResolver` fd를 게스트 경로에 _진짜 마운트_처럼 노출, 무복사). 중간단계 폴백 = **copy-in/out**(SAF로 고른 문서를 앱-private로 복사 후 게스트가 사본을 봄, 변경분은 명시 export).
- **근거**: 진짜 마운트(FUSE 등)는 비root·public-API 제약에서 직접 불가하나, ALR은 게스트 파일 IO를 _이미 매개_(seccomp-trace path 재작성, 메모리: ALR-path-mediation-proven)하므로 게스트의 open(rootfs 경로)을 host SAF fd로 _프록시_할 여지가 있음 — 이게 최종형. 그 프록시가 device-증명되기 전까지는 무손실·즉시 가능한 copy-in/out이 폴백. SAF는 `ACTION_OPEN_DOCUMENT`라 _권한 선언 불요_(picker 위임, §9).
- **v1**: copy-in(SAF 문서 → 앱-private → 게스트 사본) + copy-out(게스트 산출물 → SAF 저장). 동시에 SAF는 "앱 추가(.tar)"의 한 source로도(§7). → **C 트랙(SAF 프록시 설계) 신규 문서**가 프록시↔copy 경계·승격 조건을 SSOT로(상호참조 §13). 현 `tools/saf_bridge_model.py`(URI↔경로 모델)가 그 host-검증 기반.
- **v2+**: seccomp-trace path-mediation 위에 SAF-fd 프록시를 얹어 _무복사_ 직통(게스트 open → host fd). device로 프록시 정합성(쓰기 가시성·동시성·권한)이 증명되면 폴백을 졸업.
- **좌우범위**: copy 폴백 = T5(SAF 임포트)·`saf_bridge_model.py`. 프록시 최종형 = C 트랙 + WS-1 path-mediation(seccomp-trace). 마운트류 시스템콜 신규 0(프록시는 기존 path-rewrite 확장).
- **미해결**: 프록시의 쓰기 가시성/동시 접근/SAF 권한 만료 처리는 device-only — C 트랙 device 게이트.

### D5 — 백그라운드: device 측정(M-UX) 후 확정 (이번 범위 밖)
- **채택안**: 백그라운드(suspend) 정책은 **device 측정 결과로 확정** — 이번 ADR 범위 밖. v1 잠정 = "포그라운드 1앱, 그 외는 보류(suspend) 또는 정지"(§6).
- **근거**: 게스트 suspend 가부는 비root 환경의 SIGSTOP/cgroup-free 거동·OOM 압력에 의존 → host(darwin)로 못 닫음. D1 단일 포그라운드 불변식은 _백그라운드 정책과 독립_으로 성립(2번째 앱은 항상 비-RENDERING).
- **v1**: §6 잠정 정책으로 출하 가능(보류=프로세스 정지 fallback). suspend가 device로 가능 확인되면 "보류=상태 보존 suspend"로 업그레이드.
- **좌우범위**: 측정·확정 = M-UX device 게이트(§10) + 런타임 트랙. 본 ADR은 _정책 슬롯_만 열어둠.
- **미해결**: suspend 가부·복귀 시 컴포지터 재바인드 비용·OOM 시 CRASHED 신호 — 전부 §10 device-only.

> **결정 요약표**

| # | 질문 | v1 채택 | v2+ | 닫는 트랙 |
|---|---|---|---|---|
| D1 | 멀티앱 | 단일 포그라운드(RENDERING 0..1) | 동시-멀티 데스크탑(별 ADR) | WS-1/§5-F; v2=WS-3 |
| D2 | UI 스택 | Compose 신규화면 + View RunningSurface | (RunningSurface AndroidView 검토) | T3 + 통합 |
| D3 | 카탈로그 | apt-인덱스 목록 + stage-tar 설치 | 인-게스트 apt/dpkg | B 트랙·WS-4; v2=ADR-003 |
| D4 | 파일연동 | copy-in/out 폴백 | SAF-fd 직통 프록시 | T5; v2=C 트랙·WS-1 |
| D5 | 백그라운드 | 보류/정지(잠정) | suspend 확정(측정 후) | M-UX 게이트(범위 밖) |

---

## 1. 한 줄 결론

**현 프론트는 "프로덕트"가 아니라 "검증 하니스"다 — `MainActivity.onCreate`가 약 40개의 네이티브 probe를 동기 실행하고 그 PASS/FAIL을 `TextView`에 덤프하며, 마지막에 단 하나의 `SurfaceView` + Wayland 컴포지터를 띄워 하드코딩된 게스트(GIMP/foot)를 그린다. UX 최적화의 핵심 동작은 _코드를 갈아엎는 게 아니라 화면을 재배치_하는 것이다: 그 `SurfaceView`+컴포지터 블록을 `RunningSurface`(앱 실행 화면)라는 _독립 Activity_로 떼어내고, 앱 시작점을 probe-덤프가 아니라 `LauncherActivity`(설치된 리눅스 앱 아이콘 그리드)로 바꾼 뒤, `Launcher → Catalog → AppDetail → RunningSurface`의 4-화면 네비게이션을 깐다. 런타임(게스트 실행/생애주기/렌더 콜백)과 UI는 _이미_ 코드 안에 사실상 존재하는 경계(`NativeCommandRunner`의 program-spec 실행 + `nativeWaylandCompositorStart`/`SurfaceHolder.Callback` 렌더 콜백)를 §5-F 인터페이스로 _명문화_해서 만난다. 그 계약만 고정하면 런타임이 device에서 덜 익어도 UI 4트랙은 mock 런타임으로 _지금_ 전부 만들 수 있다.** 미해결의 급소는 단 하나로 수렴한다: **현 컴포지터/`SurfaceView`/네이티브 입력 인젝션이 _프로세스당 1개 Surface_를 _순차_ 사용하도록 짜여 있어(§4-C), "동시 여러 리눅스 앱"은 런타임 측 멀티-surface 작업 없이는 못 준다 — 그래서 v1 멀티앱 정책은 "단일 포그라운드 실행 + 백그라운드 보류(suspend)"로 긋고, 동시-멀티는 device 측정 후 별 ADR로 승계한다.**

---

## 2. 현 프론트의 사실(코드 독해, load-bearing)

UX 설계는 _있는 것_ 위에 얹어야 한다. `MainActivity.kt`(2405줄) 독해 사실:

- **시작점 = probe 하니스.** `onCreate`가 `RootfsInstaller(this).prepareBundledTinyRootfs()`로 인앱 rootfs를 풀고(`RootfsInstaller.kt` L27~), `/data/local/tmp/*-stage.tar` overlay들을 백그라운드 스레드로 스테이징(L59~207), 이어 `nativeCommandRunner.run*`/`nativeAlr*Probe(...)` 수십 개를 **동기**로 돌려(L221~566) 큰 `executionSummary` 문자열(L734~)을 만들고 `ScrollView(TextView)`에 붙인다.
- **유일한 표면 = 단일 `SurfaceView`.** L1138 `SurfaceView(this).apply{…}` 하나뿐. 그 `holder.addCallback(SurfaceHolder.Callback)`의 `surfaceCreated`(L1205~)에서 GPU 큐브 → `nativeWaylandCompositorStart(...)`(L1282) → 게스트 wl_shm 클라이언트 본드/present를 _그 콜백 안에서 순차_ 수행한다. 주석(L1213-1215)이 자인: "단일 Surface는 순차로 사용된다."
- **입력 = 네이티브 인젝션.** `setOnTouchListener`/`setOnKeyListener`/`setOnGenericMotionListener`가 터치·키·스크롤을 `nativeWaylandInjectTouch/Key/Scroll`로 _컴포지터에 직접_ 밀어넣는다(L1146~1203). 즉 입력은 "포커스된 Wayland 클라이언트" 1개를 향한다(L1156-1158 주석).
- **게스트 실행 인터페이스 = program-spec 문자열.** 네이티브 로더 진입점은 전부 `nativeAlrNativeLoaderProbe(pkg, nativeLibDir, filesDir, cacheDir, rootfsName, programSpec)` 형태이고, `programSpec`은 **개행-구분 argv**다(L423 `"/bin/dash\n-c\necho alr-shell-ok"`). 즉 "어떤 게스트를 어떤 인자로 띄울지"는 _이미_ 문자열 한 줄로 표현된다 — UX의 "앱 실행요청"이 여기에 그대로 매핑된다.
- **권한 = 2개뿐.** `AndroidManifest.xml`: `INTERNET`, `ACCESS_NETWORK_STATE`. 런처/카탈로그/실행은 _추가 권한 0_으로 가능(rootfs는 앱-private `filesDir`, overlay는 앱-private). 권한 프롬프트 UX(T4)는 _현재 불필요한 것을 정직히_ 다뤄야 한다(§9).
- **설치 단위 = stage tar overlay.** `RootfsInstaller.extractOverlayTar(tar, rootfsDir)`(L236~)가 base 다운그레이드 가드(M1) 하에 overlay를 풀고 `.{name}-staged-<size>` 마커를 남긴다(`MainActivity` L170-176). **"리눅스 앱 설치" = overlay tar 적용 + 카탈로그 엔트리 등록**이 자연 모델이다.

→ 결론: UX는 (a) `SurfaceView`+컴포지터를 `RunningSurface`로 외화, (b) program-spec 실행을 §5-F `LaunchRequest`로 명문화, (c) overlay 스테이징을 "설치"로 승격, (d) probe-덤프를 `Settings>Diagnostics`로 격리 — 이 4동작이면 큰 코드 재작성 없이 프로덕트 골격이 선다.

---

## 3. 화면 플로우

데스크탑형 "런타임+인앱 카탈로그"의 사용자 여정:

```
        ┌───────────────────────── ALR Runtime (단일 APK) ─────────────────────────┐
        │                                                                          │
첫실행 → [Onboarding] → [Launcher(홈)]  ⇄  [Catalog]  →  [AppDetail]               │
 (1회)    rootfs 준비      설치된 앱        설치가능 앱      설치/제거/실행            │
          진행 표시        아이콘 그리드     탐색·검색       메타+스크린샷             │
                              │  ▲                              │                   │
                       실행 탭 │  │ 복귀/종료                   실행 │                 │
                              ▼  │                              ▼                   │
                          [RunningSurface] ◀──────────────────────                  │
                          풀스크린 컴포지터(SurfaceView)                              │
                          앱 전환(Recents) · 키보드 · 종료                            │
                              │                                                     │
                       설정 ──┴──→ [Settings] (Storage / Permissions / Diagnostics) │
        └──────────────────────────────────────────────────────────────────────────┘
```

1. **Onboarding(첫 실행 1회)** — rootfs 추출(`prepareBundledTinyRootfs`, 현재 `onCreate`에서 동기 실행 중)을 _진행 표시와 함께 백그라운드_로 옮긴다. "리눅스 환경 준비 중 (1/1)" 같은 결정적 진행. 완료 시 Launcher로. (현 코드는 이걸 침묵하며 UI 스레드에서 한다 — 제안 §8-개선-1.)
2. **Launcher(홈)** — 설치된 리눅스 앱을 아이콘 그리드로. 각 타일 = 매니페스트(T2) 엔트리. 탭 → RunningSurface로 실행. 빈 상태 → "카탈로그에서 앱 받기" CTA. 우상단 = Catalog/Settings 진입.
3. **Catalog(탐색)** — 설치 가능한 앱(번들 카탈로그 + apt 기반)을 카드 리스트로. 검색/카테고리. 카드 탭 → AppDetail.
4. **AppDetail** — 앱 메타(이름/설명/크기/출처/스크린샷) + [설치]/[열기]/[제거]. 설치 = overlay tar 스테이징(번들) 또는 apt 트랜잭션(네트워크). 진행/실패를 인라인으로.
5. **RunningSurface(앱 실행)** — **현 `SurfaceView`+Wayland 컴포지터 블록을 그대로 담는 풀스크린 Activity.** 실행 중 앱이 여기 그려진다. 시스템 back/제스처 = "홈으로(백그라운드 보류)" 또는 "종료" 정책(§5). 키보드/입력은 현 인젝션 경로 재사용.
6. **멀티태스킹/복귀/종료** — Launcher↔RunningSurface 토글이 "최근 앱" 역할. v1은 단일 포그라운드(§5). Android Recents에는 RunningSurface가 ALR의 한 task로 보인다.
7. **Settings** — Storage(rootfs/overlay 용량·정리), Permissions(현 2개 + 향후 SAF/알림 토글), **Diagnostics(현 probe-덤프를 여기로 격리** — 개발자/지원용, 일반 사용자엔 숨김).

---

## 4. 컴포넌트 분해 (Activity/Compose 매핑)

현 프로젝트는 **순수 View(코드) 기반**(`SurfaceView`/`TextView`/`LinearLayout`, Compose 미사용). 권고: **신규 일반 UI(Launcher/Catalog/AppDetail/Settings)는 Compose로, `RunningSurface`만 View 기반**(`SurfaceView`는 Compose의 `AndroidView`로 감싸도 되나, 입력 인젝션이 View 콜백에 강결합되어 있어 v1은 순수 Activity+View가 안전).

| UX 컴포넌트 | Android 매핑 | 소유 트랙 | 현 코드와의 관계 |
|---|---|---|---|
| **Onboarding** | `OnboardingActivity` 또는 Launcher 내 상태 | T3(런처) | `prepareBundledTinyRootfs`를 백그라운드로(제안 §8) |
| **Launcher(홈)** | `LauncherActivity`(Compose) + `LAUNCHER` intent-filter | **T3** | 신규. 현 `MainActivity`의 MAIN/LAUNCHER 필터를 _이리로 이전_(제안 §8-개선-2) |
| **Catalog** | `CatalogActivity` 또는 Launcher 내 탭(Compose) | **T3** | 신규. 데이터=매니페스트 카탈로그(T2) |
| **AppDetail** | `AppDetailActivity`/바텀시트(Compose) | **T3** | 신규 |
| **RunningSurface** | `RunningSurfaceActivity`(View) | 본 ADR(경계) / 런타임 통합 | **현 `MainActivity`의 `SurfaceView`+컴포지터 블록(L1138~)을 이리로 이동** |
| **Settings/Diagnostics** | `SettingsActivity`(Compose) | T3/T4 | **현 `executionSummary` TextView 덤프를 Diagnostics 탭으로 격리** |
| **권한 플로우** | runtime permission 헬퍼 + 설명 다이얼로그 | **T4** | 현 2권한 + 향후(§9) |
| **앱 import(SAF)** | `ACTION_OPEN_DOCUMENT` 결과 → overlay 스테이징 | **T5** | `extractOverlayTar` 재사용(읽기 전용 호출) |

### MainActivity 재배치(제안 — 코드 미수정, §8에 패치 형태 기술)
- 현 `MainActivity`는 **두 역할이 한 클래스에 엉켜** 있다: (1) probe 하니스, (2) 컴포지터 호스트. UX는 (2)를 `RunningSurfaceActivity`로 떼고, (1)을 `SettingsActivity > Diagnostics`로 격리한 뒤, **앱 시작점을 `LauncherActivity`로 교체**한다.
- _이 ADR은 `MainActivity.kt`/`AndroidManifest.xml`을 수정하지 않는다._ §8이 "어떤 라인이 어디로 가는지"의 이전 계획을 제시하고, 실제 이동은 통합 세션 또는 런타임 트랙이 수행한다(소유권 §2).

### (C) 단일-Surface 제약 — 멀티앱 정책의 물리적 근거
현 컴포지터는 입력(`nativeWaylandInject*`)을 _포커스된 1 클라이언트_에 보내고, present를 _1 SurfaceView_에 한다. 동시 2 리눅스 앱을 각자 윈도우로 보이게 하려면 런타임 측 (i) per-app surface 라우팅, (ii) 멀티-toplevel 합성/z-order, (iii) 입력 포커스 모델이 필요하다. **GIMP 다이얼로그(child toplevel) 합성은 이미 device-증명**(메모리: ALR-GUI-Android-native-polish — zorder_top 라우팅)됐으나 그건 _한 앱 내 멀티 toplevel_이지 _다중 독립 앱_이 아니다. → §5의 §5-F 계약은 멀티앱을 _표현은 가능_하게 두되(`appId`별 세션), v1 정책은 단일 포그라운드로 못 박는다(§7-정직).

---

## 5. UI↔런타임 §5 인터페이스 계약 (§5-F, 신규)

이 계약이 본 ADR의 _핵심 산출물_이다. UI 4트랙은 이 계약의 **mock 구현**에 대고 지금 개발하고, 런타임(WS-1/WS-3)이 device-ready되면 실 구현으로 교체한다. 계약은 §5-A~E(orchestration-5session-plan)의 후속 슬롯 **§5-F**로 등록.

```kotlin
// §5-F: UI ↔ 런타임 경계. UI는 이 인터페이스만 안다(컴포지터/로더 내부 불투명).
// 소유: 본 ADR 정의 → 런타임 트랙(WS-1) 구현 수락. UI 트랙(T3/T5)은 mock으로 선개발.

/** 앱 실행요청. program-spec(현 MainActivity의 개행-argv L423)을 구조화한 것. */
data class LaunchRequest(
    val appId: String,           // 매니페스트(T2) 안정 id. 세션 키.
    val entryPath: String,       // 게스트 절대경로, 예 "/usr/bin/gimp"
    val args: List<String>,      // argv[1..]
    val env: Map<String, String> = emptyMap(),  // 추가 게스트 env(런타임이 base env에 머지)
    val protocol: Surface = Surface.WAYLAND,     // WAYLAND | X11(Xwayland)
)
enum class Surface { WAYLAND, X11 }

/** 실행 세션 핸들. UI는 이걸로 생애주기를 제어. */
interface AppSession {
    val appId: String
    val state: StateFlow<SessionState>   // 관찰 가능한 상태
    fun bindSurface(holder: SurfaceHolder)   // RunningSurface가 자기 SurfaceView를 제공
    fun unbindSurface()                      // 백그라운드 전환 시
    fun requestForeground()                  // 포커스/입력 라우팅 획득
    fun requestBackground()                  // suspend(가능 시) 또는 보류
    fun stop(reason: StopReason)             // 정상 종료 요청
}

enum class SessionState { STARTING, RENDERING, BACKGROUND, STOPPING, STOPPED, CRASHED }
enum class StopReason { USER, SYSTEM_MEMORY, RUNTIME_ERROR }

// ── §5-F AppSession 상태기계 (D1 단일 포그라운드 불변식 명문화) ──────────────
//
//   launch()                bindSurface + 첫 present
//      │                          │
//      ▼          ┌───────────────┘
//  ┌────────┐     ▼              requestForeground() / bindSurface()
//  │STARTING│─▶┌─────────┐◀──────────────────────────┐
//  └────────┘  │RENDERING│                            │
//      │       └─────────┘──requestBackground()──▶┌──────────┐
//      │            │         / unbindSurface()    │BACKGROUND│
//      │            │ stop()/exit                  └──────────┘
//      │            ▼                                   │ stop()/exit
//      │       ┌────────┐                               │
//      └─crash─│STOPPING│◀──────────────────────────────┘
//        ▼     └────────┘
//   ┌───────┐      │ 정상 종료        ┌───────┐
//   │CRASHED│      └────────────────▶│STOPPED│
//   └───────┘  (비정상: SIGSEGV 등)   └───────┘
//
// ★ 단일 포그라운드 불변식 (찬우 결정 D1):
//   INV-1: ∀ 시점, |{ s ∈ sessions : s.state == RENDERING }| ≤ 1.
//          (v1: 화면에 리눅스 앱은 0개 또는 1개. 두 세션이 동시에 RENDERING 불가.)
//   INV-2: 새 세션이 RENDERING으로 진입하기 전에, 기존 RENDERING 세션은
//          반드시 BACKGROUND(suspend 가능 시) 또는 STOPPING으로 먼저 전이한다.
//          → AlrRuntime.launch()/requestForeground()는 이 선행 전이를
//            원자적으로 보장(포커스·입력·present 표면을 새 세션으로 양도).
//   INV-3: BACKGROUND·STOPPING·STOPPED·CRASHED·STARTING 세션 수에는 상한 없음
//          (멀티앱 "표현"은 열림). 제약은 오직 RENDERING ≤ 1.
//
//   D5(백그라운드)는 BACKGROUND의 _의미_만 device-측정으로 확정한다:
//     - suspend 가능 ⇒ BACKGROUND = 상태 보존 정지(복귀 시 재바인드).
//     - 불가 ⇒ BACKGROUND = STOPPING 경유 종료로 폴백(잠정 v1, §6).
//   어느 쪽이든 INV-1~3은 불변(백그라운드 정책과 독립).
//
//   동시-멀티(v2+)는 INV-1을 "RENDERING ≤ N(가시 윈도우 수)"로 _완화_할 뿐,
//   상태기계·sessions:List·UI는 그대로 — UI 재작성 없이 불변식만 푼다(D1 v2).

/** 런타임 진입점. 현 nativeWaylandCompositorStart + nativeAlrNativeLoaderProbe를 감싼다. */
interface AlrRuntime {
    /** 세션 시작. 즉시 핸들 반환, 상태는 콜백/StateFlow로. */
    fun launch(req: LaunchRequest): AppSession
    /** 현재 활성 세션들(멀티앱 표현; v1은 0..1 RENDERING). */
    val sessions: StateFlow<List<AppSession>>
    /** 설치 트랜잭션(overlay/apt). UI는 진행률만 본다. */
    fun install(spec: InstallSpec): Flow<InstallProgress>
    fun uninstall(appId: String): Flow<InstallProgress>
    /** 설치된 앱 목록(매니페스트 T2가 채움). */
    fun installedApps(): List<AppManifestEntry>
    fun catalog(): Flow<List<CatalogEntry>>
}

// 상태 콜백 계약(요구사항 (3)): 렌더 시작/종료/크래시.
//  - STARTING → RENDERING: 첫 프레임이 SurfaceView에 present된 시점(현 코드의
//    surfaceReport "frames rendered>0" / wlFramePresented L1313에 대응).
//  - RENDERING → STOPPED: 게스트 프로세스 exit=0(현 NativeCommandResult.exitCode).
//  - * → CRASHED: 게스트 비정상 종료/SIGSEGV(현 supervisor가 SIGSYS 외 시그널 포착,
//    ADR-003 §2 EVENT 처리). UI는 "앱이 종료됨" + 재시작/로그 보기.
```

### 계약↔현 코드 매핑(구현 시 무엇을 감싸는가)
| §5-F 요소 | 현 코드 백킹 |
|---|---|
| `LaunchRequest.entryPath+args` | `nativeAlrNativeLoaderProbe(...,"path\narg1\narg2")`(L423 개행-argv) |
| `AppSession.bindSurface` | `SurfaceHolder.Callback.surfaceCreated`(L1205) + `nativeWaylandCompositorStart`(L1282) |
| `state RENDERING` | `surfaceReport` "frames rendered>0"(L1229) / `wlFramePresented`(L1313) |
| 입력 라우팅(`requestForeground`) | `nativeWaylandInject*`가 포커스 클라이언트로(L1146~) |
| `state STOPPED/CRASHED` | `NativeCommandResult.exitCode`(`NativeCommandRunner` L230~) / supervisor 시그널 |
| `install(overlay)` | `RootfsInstaller.extractOverlayTar`(L236) + `.{name}-staged-<size>` 마커 |
| `install(apt)` | `runProotRootfsAptGet*` 경로(`NativeCommandRunner` L75~, 트랜잭션 래핑 필요) |

### Mock 런타임(UI 선개발용 — UX 트랙 소유, host 검증 가능)
- `FakeAlrRuntime : AlrRuntime` — `launch`가 타이머로 `STARTING→RENDERING`을 흘리고, `bindSurface`가 `SurfaceView`에 단색/테스트 패턴을 그린다. `install`은 가짜 진행률. `catalog/installedApps`는 매니페스트(T2) 파서가 읽은 JSON 픽스처.
- 이 mock의 _순수 로직 부분_(매니페스트 파싱→`AppManifestEntry`, 설치 상태 전이, 카탈로그 필터/검색)은 **host pytest로 검증 가능**(T2/T3가 소유). 본 ADR은 문서-only(host_verified=false)이고, 검증은 T2/T3 트랙이 자기 파서/모델로 green을 낸다.

---

## 6. 멀티앱 정책 (v1 결정 + 승계)

> 본 절은 **§0-D1**(찬우 확정)의 운영 형태다. 불변식은 §5-F의 INV-1(`RENDERING ≤ 1`)·INV-2(양도 선행)·INV-3(비-RENDERING 무상한)에 코드 계약으로 박혀 있다.

| 정책 | v1(지금) | 근거 / 승계 |
|---|---|---|
| 동시 실행 | **단일 포그라운드** 1앱(RENDERING) | §4-C 단일-Surface·단일-입력-포커스. 동시-멀티는 런타임 멀티-surface 작업 필요 → 별 ADR |
| 백그라운드 | **보류(suspend) 또는 정지** | 게스트 suspend 가능성은 device-측정(SIGSTOP/cgroup-free 환경) — §7 |
| 앱 전환 | Launcher 경유(홈→다른 앱) | 동시 2 RENDERING 없음 → 전환 시 이전 앱 background |
| 한 앱 내 멀티창 | **지원**(이미 device-증명) | GIMP child toplevel 합성(메모리: zorder_top) — 새 작업 아님 |
| 장기 목표 | 동시-멀티(데스크탑 멀티윈도우) | §5-F가 `sessions: List`로 _표현은 미리_ 열어둠. 실현은 WS-3 멀티윈도우(M3) device 후 |

→ `§5-F`의 `AppSession`/`sessions:List`는 멀티를 _표현_하되, v1 `AlrRuntime` 구현이 "RENDERING은 항상 0..1"을 _불변식_으로 강제. UI는 처음부터 리스트를 다루게 짜서, 멀티 해금이 UI 재작성이 아니라 불변식 완화가 되게 한다.

---

## 7. 4트랙(매니페스트/런처/권한/SAF) 데이터·제어 흐름

본 ADR이 SSOT인 큰 그림. 각 트랙은 자기 소유 파일만 만들고 이 흐름으로 만난다.

```
   [T2 매니페스트 스키마]                 [T5 SAF 임포트]
   AppManifestEntry/CatalogEntry          ACTION_OPEN_DOCUMENT → .tar
   (JSON 파서, host-검증)                   → InstallSpec(overlay)
        │  installedApps()/catalog()             │
        ▼                                         ▼
   ┌──────────────────── AlrRuntime (§5-F) ───────────────────┐
   │  installedApps()  catalog()  install()  launch()  sessions │
   └───┬───────────────┬───────────────┬──────────┬───────────┘
       │ 목록           │ 카탈로그       │ 진행률    │ 세션상태
       ▼               ▼               ▼          ▼
   [T3 Launcher]   [T3 Catalog]   [T3 AppDetail/진행]  [RunningSurface]
       │ 탭 실행 → LaunchRequest ───────────────────────▶ bindSurface
       │
   [T4 권한] ── 설치/네트워크/SAF 시점에 필요한 권한만 just-in-time 요청
```

- **제어 흐름(설치)**: T3 AppDetail [설치] → `AlrRuntime.install(InstallSpec)` → (v1=stage-tar면 `extractOverlayTar`, v2=apt면 인-게스트 트랜잭션) → `InstallProgress` Flow → 완료 시 T2 매니페스트에 엔트리 등록 → Launcher 그리드 갱신. **v1/v2 분기는 §7-A 카탈로그 로드맵 참조**(D3).
- **제어 흐름(실행)**: T3 Launcher 탭 → `LaunchRequest(appId, entryPath, args)` → `AlrRuntime.launch` → `AppSession` → `RunningSurfaceActivity` 띄움 → `bindSurface(holder)` → `STARTING→RENDERING` → 사용자 상호작용(입력 인젝션) → back → `requestBackground`/`stop`. **launch는 §5-F INV-2를 지킴**(기존 RENDERING 세션을 먼저 BACKGROUND/STOPPING로 양도, D1).
- **데이터 흐름(매니페스트)**: T2가 `AppManifestEntry`(id/name/icon/entryPath/args/protocol/source)와 `CatalogEntry`(설치가능 메타)의 JSON 스키마+파서를 소유. RunningSurface의 `entryPath`/`args`/`protocol`은 _전부 매니페스트에서_ 온다(하드코딩 GIMP 경로 제거).
- **SAF(T5)**: §7-B 파일연동 참조(D4). 외부 `.tar`(또는 미래 `.alrpkg`) 임포트는 _설치 source_이고, 사용자 문서 입출력은 _런타임 파일연동_으로 별 경로.

### 7-A. 카탈로그 v1(stage-tar) / v2(인-게스트 apt) 로드맵 (D3 반영)

카탈로그는 **apt 저장소 인덱스**(`Packages`)를 _목록·메타·버전·크기·아이콘_의 출처로 쓴다. 설치 _실행_은 단계별로 다르다:

```
   apt 저장소 인덱스 (Packages.{gz,xz})
        │  파싱 → CatalogEntry 목록 (T2 모델, host-검증)
        ▼
   ┌──────────────── v1: stage-tar 설치 (지금) ────────────────┐
   │ deb_closure(tools/deb_closure.py):                          │
   │   targets → 런타임 Depends closure 해결 → 각 .deb 추출      │
   │   → base-owned SONAME drop(overlay_guard 가드)             │
   │   → §5-E ./-rooted overlay tar 평탄화                       │
   │   = <name>-stage.tar                                        │
   │        │  (host/빌드 측에서 풀림 — device는 푼 tar만 받음)  │
   │        ▼                                                    │
   │ device: RootfsInstaller.extractOverlayTar(tar, rootfsDir)   │
   │   + .{name}-staged-<size> 마커  ← fork-exec 0, dpkg 우회    │
   └─────────────────────────────────────────────────────────────┘
        │   exec re-entry(ADR-003 M-R4-*) device-증명 시 졸업
        ▼
   ┌──────────── v2: 인-게스트 apt/dpkg (exec 벽 해소 후) ───────┐
   │ device 게스트 내: apt-get install <pkg>                     │
   │   (dpkg maintainer script·ldconfig = fresh-execve 자식)     │
   │   ← ADR-003 §4 envp전파/TRACEEXEC 가정이 device로 닫혀야    │
   │   runProotRootfsAptGet* 경로(NativeCommandRunner L72~)를    │
   │   InstallSpec(apt) 트랜잭션으로 래핑                         │
   └─────────────────────────────────────────────────────────────┘
```

- **v1이 dpkg를 우회하는 이유**: 인-게스트 dpkg는 maintainer script·`ldconfig`를 `fork+execve`로 부르는데, 이게 ADR-003이 기록한 _device 미검증 exec 벽_이다. `deb_closure`가 이미 closure→§5-E stage-tar 평탄화를 수행하므로, "설치 = overlay 풀기"로 fork-exec 0에 도달 → v1은 그 벽과 **무관**.
- **인덱스의 역할 분리**: v1에서 apt 인덱스는 _목록·의존성 그래프·메타_까지만 쓰고(closure 해결 입력), 실제 _트랜잭션_(설정·트리거·스크립트)은 v2로 미룬다. 따라서 v1 카탈로그는 "apt 기반"이되 "apt 실행"은 아니다.
- **소유**: 인덱스→closure→stage-tar 파이프라인 = **B 트랙(카탈로그 파이프라인) 신규 문서**가 SSOT(상호참조 §13), `deb_closure`/`overlay_guard`/§5-E가 그 엔진. 카탈로그 _목록·검색·정렬_ 모델 = T2/T3(host-검증). v2 인-게스트 apt = WS-1 exec re-entry(ADR-003) 선결.

### 7-B. 파일연동: SAF 프록시(목표) + copy-in/out(폴백) (D4 반영)

설치 source로서의 SAF(.tar 임포트)와 **별개로**, 게스트 앱이 사용자 문서를 읽고 쓰는 _런타임 파일연동_은 다음 단계를 탄다:

```
  v1 (지금, 무손실 폴백) — copy-in/out
    열기:  ACTION_OPEN_DOCUMENT → ContentResolver fd → 앱-private 사본
           → 게스트는 rootfs 경로의 _사본_을 봄
    저장:  게스트 산출물(rootfs) → ACTION_CREATE_DOCUMENT/SAF로 export
    특성:  단순·즉시·device 불요 로직(tools/saf_bridge_model.py host-검증);
           단 _사본_이라 외부 원본과 실시간 동기화 아님(명시 export 필요)

  v2+ (최종 목표) — SAF 직통 프록시 (무복사)
    게스트 open("/mnt/saf/<doc>") → seccomp-trace path-mediation이
      그 open을 host ContentResolver fd로 _프록시_ (진짜 마운트처럼)
    근거:  ALR은 게스트 파일 IO를 이미 매개(ALR-path-mediation-proven);
           프록시는 마운트류 신규 syscall 0 — 기존 path-rewrite 확장
    졸업조건(device): 쓰기 가시성·동시 접근·SAF 권한 만료 처리 정합
```

- **왜 프록시가 최종형인가**: 비root·public-API에서 FUSE식 진짜 마운트는 직접 불가하나, 게스트의 `open(rootfs 경로)`을 이미 seccomp-trace로 재작성하므로 그 자리에 host SAF fd를 끼우면 _무복사 직통_이 된다. 그러나 쓰기 가시성·동시성·권한 만료가 device-only 미해결 → 그때까지 copy가 폴백.
- **소유**: copy 폴백 = T5(`saf_bridge_model.py` URI↔경로 모델, host-검증). 프록시 최종형 + 졸업 조건 = **C 트랙(SAF 프록시) 신규 문서**가 SSOT(상호참조 §13) + WS-1 path-mediation. SAF는 `ACTION_OPEN_DOCUMENT`라 _권한 선언 불요_(§9, picker 위임).

---

## 8. MainActivity 재배치 제안 (코드 미수정 — 이전 계획)

본 ADR은 `MainActivity.kt`/`AndroidManifest.xml`/`RootfsInstaller.kt`를 _건드리지 않는다_. 아래는 통합 세션/런타임 트랙이 수행할 이전 계획(소유권 §2). 각 항목은 _최소 침습_(블록 이동·필터 이전)이고 동작 로직은 보존.

- **개선-1(Onboarding 비차단화)**: `prepareBundledTinyRootfs()`(현 `onCreate` L42 동기)를 워커로 옮기고 진행 콜백 추가. 현재 UI 스레드에서 rootfs 추출이 끝날 때까지 침묵(콜드스타트 ANR 위험). 단 이건 _런타임/통합 트랙_ 작업이지 UX 트랙이 `MainActivity`를 고치는 게 아님.
- **개선-2(시작점 교체)**: `AndroidManifest.xml`의 MAIN/LAUNCHER intent-filter를 `MainActivity`→`LauncherActivity`로 이전(L8-15). `MainActivity`는 `exported=false`의 내부 Diagnostics 호스트로 남기거나 `SettingsActivity`에 흡수. **이 매니페스트 편집은 통합 세션 소유**(UX 트랙은 제안만).
- **개선-3(RunningSurface 추출)**: `SurfaceView`+`SurfaceHolder.Callback`+`nativeWaylandCompositorStart`+입력 인젝션 블록(L1138~끝)을 `RunningSurfaceActivity`로 _그대로_ 이동. 진입 시 `LaunchRequest`를 Intent extra로 받아 하드코딩 GIMP/foot 대신 그 program-spec을 실행.
- **개선-4(Diagnostics 격리)**: `executionSummary` 빌드(L221~566, L734~)와 `TextView` 덤프를 `SettingsActivity > Diagnostics`로. 일반 사용자 경로에서 probe는 안 돈다(콜드스타트 가속 + UX 정돈).
- **개선-5(stage-tar 스테이징의 명시화)**: 현 `/data/local/tmp/*-stage.tar` 자동 스테이징(L59~207)은 _개발 편의_(adb push)다. 프로덕트에선 "설치"가 카탈로그/SAF에서 와야 하므로, 이 자동 경로는 Diagnostics/dev 모드로 게이트(런타임 트랙).

→ 이 5개는 전부 _기존 로직 보존·위치 이동_이라 device 회귀 위험이 낮다. 그러나 **UX 트랙은 이 중 어느 것도 직접 커밋하지 않는다** — 제안으로 남기고, 통합 세션이 device 게이트(§10) 하에 수행한다.

---

## 9. 권한 UX (T4 SSOT 입력 — 정직히 "지금 거의 필요 없음")

- **현 사실**: 설치(overlay·apt 네트워크 제외)·실행·렌더는 _추가 권한 0_. rootfs는 `filesDir`(앱-private), overlay도 앱-private. 카탈로그 네트워크 다운로드만 `INTERNET`(이미 있음).
- **just-in-time만**: 권한은 _그 기능을 처음 쓸 때_ 설명 다이얼로그와 함께. 후보:
  - `INTERNET`(이미 선언, 런타임 권한 아님) — 카탈로그/apt 다운로드. 프롬프트 불필요.
  - **SAF(T5)** — `ACTION_OPEN_DOCUMENT`는 _권한 선언 불요_(시스템 picker가 위임). T4는 "SAF는 권한 프롬프트가 아니라 picker UX"임을 정직히.
  - **알림(POST_NOTIFICATIONS, Android 13+)** — 설치/실행 진행을 알림으로 보일 때만. v1 옵션.
  - **저장소 광범위(MANAGE_EXTERNAL_STORAGE)** — `MainActivity`가 _이미 "선언 안 됨"을 체크_(L570 `broadStoragePermissionDeclared`)하며 **안 쓰는 게 정책**. T4는 이걸 _거부 목록_으로 문서화(비root·최소권한 원칙).
- → **T4의 진짜 산출물은 "권한 프롬프트 난사"가 아니라 "왜 ALR이 권한을 거의 안 요구하는지"의 설명 UX + 미래 권한의 just-in-time 패턴**이다. 이게 비root/최소권한이라는 제품 차별점과 정합.

---

## 10. device 게이트 + DEVICE-REQ 마커

UI 골격·매니페스트 파서·mock 런타임은 host에서 끝나지만, _실 런타임 결선_은 device가 답한다.

### M-UX-runningsurface (1순위 — RunningSurface 추출이 무회귀)
- **무엇**: 개선-3(컴포지터 블록을 별 Activity로 이동) 후, 기존 GIMP/foot 렌더·입력이 _그대로_ 동작하는지. UX 재배치가 device 동작을 깨지 않음을 확정.
- **DEVICE-REQ**: `DEVICE-REQ: ALR-M-UX-runningsurface — SM-X236N (am force-stop first), Launcher→앱 탭→RunningSurface에서 GIMP File>New>OK 캔버스 + 브러시 1획; gate = 렌더 present AND 터치 입력 도달(현 메모리 v111 동등). probe-덤프는 Diagnostics에만.`

### M-UX-launch-contract (2순위 — §5-F 실 결선)
- **무엇**: `AlrRuntime.launch(LaunchRequest)` 실 구현이 program-spec 경로(L423)를 감싸 `STARTING→RENDERING` 콜백을 정확한 시점(첫 present)에 낸다.
- **DEVICE-REQ**: `DEVICE-REQ: ALR-M-UX-launch — SM-X236N, LaunchRequest(appId=gimp, entryPath=/usr/bin/gimp)로 실행; gate = state RENDERING이 첫 프레임 present와 ±1프레임 내 발화 AND exit=0이 STOPPED로.`

### M-UX-install-overlay (3순위 — 설치 파이프라인)
- **무엇**: AppDetail [설치]가 `extractOverlayTar`를 호출해 한 앱(예: foot)을 카탈로그→Launcher 그리드로 올리고 실행까지.
- **DEVICE-REQ**: `DEVICE-REQ: ALR-M-UX-install — SM-X236N, Catalog>foot>설치→Launcher에 타일 등장→실행 렌더; gate = overlay extracted>0(가드 skip 정상) AND 실행 RENDERING.`

(주의: M-UX-*는 _UI 결선_ 게이트로, 런타임 내부(supervisor/컴포지터)는 ADR-001/002/003·WS 게이트가 별도 담당. 멀티앱 동시-실행은 본 ADR _범위 밖_ — §6대로 별 ADR로 승계, device-REQ 미발급.)

---

## 11. host 프로토타입 가능범위 (device 불요)

본 ADR은 문서-only이나, 의존 트랙이 _지금_ host-검증할 수 있는 순수 로직을 짚는다(각 트랙 소유):
- **(T2)** 매니페스트/카탈로그 JSON 파서 → `AppManifestEntry`/`CatalogEntry` 모델. pytest로 round-trip·검색·필터·버전정렬·악성입력 거부.
- **(T3)** 설치 상태 머신(`NOT_INSTALLED→INSTALLING→INSTALLED→…`) + 카탈로그 필터/정렬 + Launcher 그리드 정렬 규칙 = 순수 함수. pytest로.
- **(T5)** SAF로 받은 tar의 사전검증(엔트리 traversal/`..`/절대경로/디바이스노드 거부)을 `RootfsInstaller.validateTarEntry`(L393) 규칙의 _순수 미러_로 재구현·테스트(실제 추출은 런타임).
- **(공통/mock)** `FakeAlrRuntime`의 상태 전이(STARTING→RENDERING→STOPPED, install 진행률 단조성, sessions 0..1 불변식)를 pytest 모델로.

→ **단 host(darwin)는 실 컴포지터/seccomp/Mali 거동 불가**(§10). UI 골격의 _상호작용_·_렌더_는 device-only. 본 ADR이 host로 닫는 건 _계약의 형태와 순수 로직_이지 _런타임 효과_가 아니다.

---

## 12. 정직 섹션

**지금 미리 가능 vs 런타임 의존:**
- **미리 가능(런타임 무관)**: 4-화면 네비게이션, Launcher/Catalog/AppDetail/Settings UI 골격, §5-F 계약 정의, mock 런타임, 매니페스트/카탈로그 파서(T2), 설치 상태머신(T3), SAF 사전검증(T5) — 전부 host pytest + mock으로 _오늘_ 만든다.
- **런타임 의존(device-only)**: 실 `launch`→첫-프레임 RENDERING 타이밍, 입력 인젝션 도달, overlay 설치 후 실 실행, 게스트 suspend(백그라운드) 가부, CRASHED 신호 정확도. 전부 §10 DEVICE-REQ로 격리.

**자가 적대검증(핵심 주장 1개 자기공격)**: §1 주장 — "코드 재작성 없이 화면 재배치로 프로덕트가 선다" — 를 공격한다. _constraint는 안 깨진다_(새 권한 0, 새 네이티브 진입점 0; `RunningSurface`는 기존 `SurfaceView`+`nativeWaylandCompositorStart`를 _그대로_ 옮길 뿐, 비root/W^X/in-process 불변). _그러나 "재배치만으로 충분"의 급소는 §4-C다_: 현 컴포지터·입력 인젝션이 _프로세스 1개·Surface 1개·포커스 1개_ 가정 위에 짜여 있어, "데스크탑형 멀티윈도우"라는 제품 비전의 _완성형_은 재배치가 아니라 **런타임 멀티-surface 신작업**을 요구한다. 즉 _v1(단일 포그라운드)은 재배치로 100% 도달하지만, 멀티앱 동시 데스크탑은 UI 트랙만으로 못 닫는다_ — 그래서 §6이 그걸 v1 밖으로 정직히 긋고 §5-F가 표현만 미리 연다. 또 하나: §8의 MainActivity 이전(특히 MAIN/LAUNCHER 필터 교체·rootfs 비차단화)은 _UX 트랙 소유 밖_이라, 통합/런타임 트랙이 수행하기 전까지 UX 트랙의 Launcher는 "설치는 됐으나 시작점이 아직 probe 하니스"인 _과도기_를 산다. 이건 결함이 아니라 _소유권 분리의 정직한 비용_이고, §10 M-UX-runningsurface device 게이트가 그 이전의 무회귀를 확정하는 순간 해소된다.

**constraint_violations**: 없음(확정). 새 syscall/권한/ptrace op/execmem 매핑 0. RunningSurface는 기존 표면·진입점 재배치. 카탈로그 v1(stage-tar, D3)은 fork-exec 0(dpkg 우회), 파일연동 v1(copy, D4)은 신규 syscall 0, 프록시 v2(D4)는 기존 path-rewrite 확장(마운트류 syscall 0). 깨질 위험이 있는 것(멀티-surface=D1 v2, MainActivity 이전=§8, suspend 거동=D5, 인-게스트 apt=D3 v2, SAF 프록시=D4 v2)은 전부 §0/§6/§8/§10에 _범위 밖_ 또는 _device-only_로 격리.

**R1 개정의 정직(찬우 5결정 반영의 비용)**: §0의 5결정은 _열린 질문을 닫은_ 것이지 _새 능력을 증명한_ 게 아니다. D1·D2는 host로 닫힘(불변식·스택은 코드 계약). D3 v1·D4 v1은 _이미 있는 엔진_(`deb_closure`/`extractOverlayTar`/`saf_bridge_model`) 위에 서므로 오늘 형태가 확정. **그러나 D3 v2(인-게스트 apt)·D4 v2(SAF 프록시)·D5(suspend)는 device·후속 ADR에 매달려 있다** — 본 ADR은 그 3개를 _로드맵 슬롯_으로만 열고(§7-A/§7-B/§6), 실현은 ADR-003 exec re-entry(D3 v2)·WS-1 path-mediation(D4 v2)·M-UX 측정(D5)에 위임한다. 즉 R1은 "v1을 100% 확정 + v2를 정직히 미결"로 긋는다 — ADR-002/003의 "1차 기준선은 확정, 상위 목표는 device-best-effort+한계 문서화" 톤과 일관.

---

## 13. 상호참조 (B/C 트랙 신규 문서 + 선행 ADR)

본 ADR(§0)이 결정의 SSOT이고, 결정별 _구현 파이프라인_은 병렬 트랙(B=카탈로그, C=SAF 프록시)의 산출물이 소유한다. 아래 링크는 같은 worktree에 실재하는 산출물을 가리킨다(각 트랙 소유 — 본 ADR은 _참조_만).

- **B 트랙 — 카탈로그 파이프라인(D3 구현)**: `tools/apt_catalog.py`(apt 인덱스 → 카탈로그 목록 v1) + `tools/install_plan.py`(선택 패키지 → closure → §5-E stage-tar 설치 플랜)이 SSOT. apt 인덱스 파싱 → `deb_closure` Depends-closure 해결 → base-subtract(`overlay_guard`) → §5-E `<name>-stage.tar` 평탄화 → `extractOverlayTar` 설치까지의 v1 파이프라인. 본 ADR §7-A가 그 큰 그림, B 트랙이 인덱스 포맷·closure 정책·캐시·번들 vs 네트워크 분기·v2 인-게스트 apt 트랜잭션 래핑을 상세화. 엔진: `tools/deb_closure.py`·`tools/overlay_guard.py`·`tools/build_stage_tar.py`.
- **C 트랙 — SAF 파일 프록시(D4 구현)**: `docs/design/saf-proxy.md`(T5-C, 프록시 최종형 + copy 폴백 SSOT) + `docs/design/file-bridge-saf.md`(T5, 파일 브리지 기반). copy-in/out 폴백 ↔ seccomp-trace SAF-fd 직통 프록시의 경계·승격 조건·졸업 게이트. 본 ADR §7-B가 두 단계의 큰 그림, C 트랙이 URI 수명·쓰기 가시성·동시성·권한 만료·path-mediation 결선을 상세화. 모델: `tools/saf_bridge_model.py`.
- **선행 ADR(결정의 제약 출처)**:
  - `docs/design/adr-003-multiprocess-exec-reentry.md` — D3 v2(인-게스트 apt)가 의존하는 exec re-entry 벽·envp 전파·TRACEEXEC 가정. v2 착수 게이트(M-R4-*).
  - `docs/design/adr-002-chromium-cp6-roadmap.md` — "1차 기준선 확정 + 상위는 best-effort+한계 문서화" 톤의 선례(본 R1 §12가 계승).
  - `docs/design/adr-001-syscall-overhead-user-notif.md` — 중재 오버헤드 평가(런타임 비용 맥락).
- **동급 UX 트랙 문서(본 ADR이 SSOT로 참조됨)**: `docs/design/alr-app-manifest-schema.md`(T2), `docs/design/launcher-ui.md`(T3), `docs/design/permission-mapping.md`·`docs/design/androidmanifest-permissions-proposal.md`(T4), `docs/design/file-bridge-saf.md`(T5).

---

관련 파일(절대경로):
- 본 ADR: `/Users/naen/Documents/alr-product-ux/docs/design/adr-004-inapp-catalog-ux.md`
- 현 프론트(읽기 전용 — 본 ADR 미수정): `/Users/naen/Documents/alr-product-ux/app/src/main/AndroidManifest.xml`, `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt`(SurfaceView/컴포지터 L1138~, program-spec 실행 L337~/L423, executionSummary L734~), `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/RootfsInstaller.kt`(extractOverlayTar L236, validateTarEntry L393), `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/RootfsInstallPlan.kt`, `/Users/naen/Documents/alr-product-ux/app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt`(program-spec/argv L155~, apt 경로 L72~)
- 선행/계약: `/Users/naen/Documents/alr-product-ux/docs/research/orchestration-5session-plan.md` §5(레이어 계약)·§5-E(stage tar 규약 — 본 ADR이 §5-F를 후속 등록), `/Users/naen/Documents/alr-product-ux/docs/design/adr-003-multiprocess-exec-reentry.md`(exec/멀티프로세스 한계 — D3 v2 게이트), `/Users/naen/Documents/alr-product-ux/docs/design/adr-002-chromium-cp6-roadmap.md`(기준선+best-effort 톤 선례), `/Users/naen/Documents/alr-product-ux/docs/design/adr-001-syscall-overhead-user-notif.md`
- B/C 트랙 산출물(§13 상호참조): `/Users/naen/Documents/alr-product-ux/tools/apt_catalog.py`·`/Users/naen/Documents/alr-product-ux/tools/install_plan.py`(B, D3 — apt 인덱스→목록→closure→stage-tar 플랜), `/Users/naen/Documents/alr-product-ux/docs/design/saf-proxy.md`(C, D4 프록시 SSOT), `/Users/naen/Documents/alr-product-ux/docs/design/file-bridge-saf.md`(C, D4 copy 폴백 기반)
- D3 v1 엔진(host): `/Users/naen/Documents/alr-product-ux/tools/deb_closure.py`(closure→§5-E stage-tar), `/Users/naen/Documents/alr-product-ux/tools/overlay_guard.py`(base-subtract 가드), `/Users/naen/Documents/alr-product-ux/tools/alr_manifest.py`(매니페스트·카탈로그 모델·stage-tar 마커)
- 의존 UX 트랙(본 ADR이 SSOT): T2 매니페스트 스키마(`alr-app-manifest-schema.md`), T3 런처/카탈로그(`launcher-ui.md`), T4 권한(`permission-mapping.md`·`androidmanifest-permissions-proposal.md`), T5 SAF 임포트(`file-bridge-saf.md`)
