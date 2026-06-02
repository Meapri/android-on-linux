# RUNBOOK — product-ux Compose UI: 통합 머지 검수 + device 실행 절차

문서 전용 (device 미사용). product-ux 는 이미 `main` 에 머지됨(현 통합 트리 HEAD `839315e`,
머지 커밋 `0ceaa01` "Merge PR #1 research/product-ux"). 검수 시점: 2026-06-03.

> product-ux 는 런타임 없이 Compose UI 4화면 + MVVM + FakeAlrRuntime(목) 으로 흐름이 동작하도록
> 만든 트랙이다. **머지 후 통합 트리에서 `assembleDebug` host 빌드 PASS — 산출 APK 의 LAUNCHER
> 진입점이 `.ui.LauncherActivity` 로 확정됨(아래 §0 에서 aapt2 로 실증)**, **device 화면은 미검증**.
> 이 런북은 (0) 머지 후 host 빌드 실증, (1) main 머지 충돌 검수(이력), (2) device 실행 절차,
> (3) device 게이트 / DEVICE-REQ, (4) 통합 머지 주의(stamp · 진입점)를 다룬다.

---

## 0. 머지 후 host 빌드 실증 — 결론: **PASS (LAUNCHER=LauncherActivity, versionCode 163)**

통합 트리(`839315e`, product-ux 머지 반영)에서 직접 `assembleDebug` 를 돌려 검증함(device 불요).

### 0-A. 빌드 결과
```
JAVA_HOME=/opt/homebrew/opt/openjdk@17   ANDROID_HOME=~/Library/Android/sdk
./gradlew :app:assembleDebug --console=plain
  → BUILD SUCCESSFUL in 12s   (GRADLE_EXIT=0)
  → app/build/outputs/apk/debug/app-debug.apk  (≈276 MB; tar noCompress 로 큼)
  → buildCMakeDebug[arm64-v8a/armeabi-v7a/x86/x86_64] + Compose 모두 통과
```

### 0-B. 산출 APK 의 진입점/스탬프 실증 (aapt2 — 소스가 아닌 *컴파일된 매니페스트*)
`build-tools/34.0.0/aapt2 dump badging|xmltree` 결과:
```
package: name='dev.chanwoo.androlinux' versionCode='163' versionName='0.4.163-sd-v163'
application-label: 'AndroLinux Runtime Lab'
launchable-activity: name='dev.chanwoo.androlinux.ui.LauncherActivity'   ← LAUNCHER = LauncherActivity (★)
  .ui.LauncherActivity      exported=true   + action.MAIN + category.LAUNCHER
  .ui.RunningSurfaceActivity exported=false
  .MainActivity              exported=false  (LAUNCHER 강등 — intent-filter 없음)
```
→ **머지 매니페스트의 MAIN/LAUNCHER = `.ui.LauncherActivity` 가 빌드 산출물에서 확정**(task 요구 충족).
   probe 하니스 `.MainActivity` 는 `exported=false` 로 살아 있어 명시 기동만 가능(§4-B).

### 0-C. host 빌드 전제(이 host 에서 확인된 사실)
- JDK: **`/opt/homebrew/opt/openjdk@17`** (Homebrew, 17.0.19) 사용. SDK: `~/Library/Android/sdk`
  (`local.properties` `sdk.dir` 로 지정됨).
- **주의(런북 절차 보정)**: 이 host 에는 `/usr/libexec/java_home -v 17` 이 듣지 않는다
  ("Unable to locate a Java Runtime"). 시스템 JDK 가 없으므로 **JAVA_HOME 은 Homebrew 경로를
  직접 지정**해야 한다(§2-0 갱신됨). JAVA_HOME 누락 시 stale APK (MEMORY: build-needs-java-home).

---

## 1. main 머지 충돌 검수 (이력) — 결론: **충돌 없음 (clean), 머지 완료**

> 이 절은 머지 *전* 검수 이력이다. 실제로 `0ceaa01` 로 `main` 에 머지 완료됐고 §0 에서 머지 후
> 빌드까지 PASS 확인함. 아래 시뮬레이션 결과는 그 머지가 무손실이었음을 뒷받침한다.

### 1-A. 권위 있는 검증 (실제 `git merge` 시뮬레이션)

별도 worktree 에서 `git merge --no-commit --no-ff research/product-ux` 를 `main` 위에 실행한 결과:

```
Auto-merging app/build.gradle.kts
Automatic merge went well; stopped before committing as requested
MERGE EXIT=0
git diff --name-only --diff-filter=U  →  (빈 결과: 충돌 파일 0)
```

`git merge-tree --write-tree main research/product-ux` 도 깨끗한 tree OID 만 출력(`CONFLICT` 줄 없음).

> 주의(거짓 양성): 단독 `git merge-file` (또는 `--diff-algorithm=histogram`) 로 `app/build.gradle.kts`
> 3-way 머지를 돌리면 **4개(또는 12개) 충돌 마커**가 보고된다. 이는 `merge-file` 의 순진한 라인 단위
> 알고리즘 탓이며, **실제 `git merge` 는 자동 해소**한다(아래 1-C 참조). 검수 시 `git merge-file`
> 결과를 믿지 말 것 — `git merge --no-commit` 또는 `git merge-tree --write-tree` 가 정답.

### 1-B. 양측이 건드린 파일 (머지 베이스 `d749073` 기준)

| 파일 | main 변경? | product-ux 변경? | 겹침 |
|---|---|---|---|
| `app/build.gradle.kts` | YES (versionCode 137→163) | YES (compose 플러그인/의존성 +36줄) | **양측** — 자동 해소됨 |
| `build.gradle.kts` (루트) | NO | YES (+1줄 compose 플러그인 선언) | 단측 |
| `gradle.properties` | NO | YES (+2줄 `android.useAndroidX=true`) | 단측 |
| `app/src/main/AndroidManifest.xml` | NO | YES (진입점 교체 +17줄) | 단측 |
| `app/.../MainActivity.kt` | YES (probe 하니스, versionName) | NO | 단측 |

→ **양측이 동시에 건드린 파일은 `app/build.gradle.kts` 하나뿐**. 나머지 product-ux 변경(신규 `ui/`,
`runtime/`, `viewmodel/`, `docs/design/`, `tests/`, `tools/` 49개 파일)은 전부 **신규 추가**라 충돌 불가.

### 1-C. `app/build.gradle.kts` 가 자동 해소되는 이유 (서로 다른 hunk)

- **main 의 변경**: `versionCode = 163` / `versionName = "0.4.163-sd-v163"` (defaultConfig, ~L44).
- **product-ux 의 변경**: `plugins{}` 에 `id("org.jetbrains.kotlin.plugin.compose")` (L4),
  `buildFeatures { compose = true }` 블록 (L27 부근), `dependencies{}` 에 Compose BOM + UI/활동/
  네비/코루틴/coil 의존성 (~L139 이후, 파일 끝).
- 세 hunk가 **물리적으로 떨어진 영역**이라 git 이 3-way 로 무손실 병합.

병합 결과 파일에 양측이 **모두** 들어옴(검증됨):
```
versionCode = 163                                    ← main
versionName = "0.4.163-sd-v163"                      ← main
id("org.jetbrains.kotlin.plugin.compose")            ← product-ux
compose = true                                       ← product-ux
platform("androidx.compose:compose-bom:2024.09.00")  ← product-ux
```
잔여 충돌 마커: **없음** (`app/build.gradle.kts`, `build.gradle.kts`, `gradle.properties`, Manifest 전수 확인).

### 1-D. 빌드 전제(머지 후 한 번 확인) — 이미 충족

- 루트 `kotlin.android` = **2.0.21**, product-ux 가 추가하는 `kotlin.plugin.compose` = **2.0.21** → **일치**.
  (Kotlin 2.0 Compose 컴파일러는 Kotlin 과 동일 버전이어야 함. `composeOptions{kotlinCompilerExtensionVersion}` 은 쓰지 않음 — 의도된 설계.)
- `gradle.properties` 에 `android.useAndroidX=true` 추가됨 (Compose/AndroidX 필수).
- `@style/AppTheme` 는 base `app/src/main/res/values/styles.xml` 에 이미 정의(`Theme.Material.Light.NoActionBar`)
  → 신규 `LauncherActivity`/`RunningSurfaceActivity` 의 테마 참조 정상 해소. 신규 res 추가 없음.

---

## 2. device 실행 절차 (install APK → am start → LauncherActivity → 4화면 네비 → 그리드 렌더)

> 전제: 이 런북은 device 를 쓰지 않는다(문서). 아래는 **device 게이트 담당이 그대로 실행할 절차**다.

### 2-0. 빌드 (host) — §0 에서 검증된 절차
```
# 이 host: 시스템 JDK 없음 → java_home 대신 Homebrew openjdk@17 직접 지정(§0-C).
export JAVA_HOME=/opt/homebrew/opt/openjdk@17     # java_home -v 17 이 듣는 host 면 그걸 써도 됨
cd "<repo>"
./gradlew :app:assembleDebug
# 산출물: app/build/outputs/apk/debug/app-debug.apk  (versionCode 163, 0.4.163-sd-v163)
# 검증(선택): build-tools/aapt2 dump badging <apk> | grep launchable-activity
#   → name='dev.chanwoo.androlinux.ui.LauncherActivity' 이어야 함(§0-B).
```
> MEMORY: JAVA_HOME 미설정 시 빌드가 조용히 stale APK 를 남긴다. 설치 후 device 에서 versionCode 163 확인.

### 2-1. 설치 + 강제정지(재설치 시 필수)
```
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am force-stop dev.chanwoo.androlinux     # 재실행 전 항상 force-stop (overlays/probe onCreate 스킵 방지)
adb shell dumpsys package dev.chanwoo.androlinux | grep versionCode   # → versionCode=163 확인
```

### 2-2. 런처 진입점 확인 + 기동
머지 후 LAUNCHER 진입점은 **`.ui.LauncherActivity`** (MainActivity 아님 — §4 주의):
```
adb shell cmd package resolve-activity -c android.intent.category.LAUNCHER dev.chanwoo.androlinux
#   → dev.chanwoo.androlinux/.ui.LauncherActivity 여야 함
adb shell am start -n dev.chanwoo.androlinux/.ui.LauncherActivity
```
홈 런처 아이콘 탭으로도 동일하게 LauncherActivity 가 떠야 한다.

### 2-3. 4화면 네비게이션 (Compose NavHost: `AlrApp.kt`)
경로: `launcher` → `catalog` → `appDetail/{appId}` → `settings`.

1. **Launcher (`LauncherScreen`)** — 설치앱 그리드 + 실행 진입. FakeAlrRuntime `seedInstalledApps()` 가
   시드: **GIMP (`org.gimp.GIMP`) · foot (`org.foot.foot`) · NetSurf (`org.netsurf.netsurf`)** 타일.
   → 그리드에 3개 타일이 카테고리 배지/아이콘과 함께 렌더되는지 확인 (**M-UX-launcher-render** 의 핵심 화면).
2. **Catalog (`CatalogScreen`)** — `seedCatalog()`: 위 3개 + **GTK3 Demo · SDL2 Demo · nano · htop**.
   검색/설치(install) progress, 오프라인 토글 동작.
3. **AppDetail (`AppDetailScreen`)** — 타일 탭 → 상세(스크린샷/권한/설치·실행 버튼). `appDetail/{appId}` 라우트.
4. **Settings (`SettingsScreen`)** — 설치앱 목록/진단. (통합 단계 ④-D 에서 MainActivity probe 결과를 여기로 주입 예정.)

### 2-4. 그리드 → 실행화면(RunningSurfaceActivity) 렌더
타일/상세에서 **실행(launch)** → `LauncherActivity.startRunningSurface(req)` 가 `RunningSurfaceActivity`
(SurfaceView + `SurfaceHolder.Callback`) 를 띄움. `onCreate` 가 `AlrRuntime.launch(req)` → `AppSession`,
`surfaceCreated → session.bindSurface(holder)`.

> **중요 — v1 은 목(mock)이다.** `AlrRuntimeHolder.buildRuntime()` 가 **`FakeAlrRuntime()`** 를 반환한다
> (프로세스 단일 인스턴스). FakeAlrRuntime 은 "실제 게스트를 실행하지 않는다"(소스 주석 명시). 따라서:
> - 그리드 GIMP/foot/NetSurf 타일 = **시드된 목 데이터**(실 rootfs 패키지 인덱스 아님).
> - 실행화면의 present/상태 전이(STARTING→RENDERING→CRASHED) = **목 상태머신**. **실 Wayland 프레임이 아님.**
> - 즉 이 머지로 검증되는 것은 **UI 흐름 + 4화면 네비 + 그리드/실행화면 렌더(목)** 까지다.
>   실 GIMP/foot/netsurf 게스트 렌더는 단계 ④-B(`NativeAlrRuntime`) 결선 **이후** 별도 게이트.

### 2-5. 로그 수집
```
adb logcat -d | grep -iE 'androlinux|AndroidRuntime|LauncherActivity|RunningSurface'   # 크래시/예외 0 확인
adb exec-out screencap -p > /tmp/M-UX-launcher-render.png                              # 그리드 캡처(증거)
```

---

## 3. device 게이트 + DEVICE-REQ

### 3-A. DEVICE-REQ: `M-UX-launcher-render` (이 머지의 device 게이트)
- **조건(PASS)**: 강제정지 후 `am start .ui.LauncherActivity` → 크래시/ANR 없이 **Launcher 4화면 네비 동작**,
  Launcher 그리드에 **GIMP·foot·NetSurf 3개 시드 타일이 시각적으로 렌더**, 타일 탭 → AppDetail,
  실행 → RunningSurfaceActivity SurfaceView 가 뜨고 상태머신이 전이(목). logcat 예외 0.
- **증거**: 위 screencap PNG + logcat 발췌(예외 없음) + `versionCode=163` dumpsys 라인.
- **범위 제한**: **목 런타임 한정.** 실 게스트 프레임은 이 게이트의 PASS 조건이 **아니다**.
- **probe 와의 관계(중요)**: 이 게이트는 `LauncherActivity` 만 띄우므로 `MainActivity.onCreate` 의
  네이티브 probe 시퀀스(아래 §5)는 **돌지 않는다 — 그게 정상이다**. probe 검증은 별도 게이트
  (`am start -n .../.MainActivity`, §5-A)로 분리 실행한다. 즉 M-UX-launcher-render 는 probe 결과를
  요구하지 않으며, probe 게이트(GPU/loader/aptdrain/chromium)와 **상호 독립**이다.

### 3-B. 후속 device 게이트(이 머지 범위 밖, ④-B 결선 후)
- **DEVICE-REQ: `M-UX-native-runtime`** — `AlrRuntimeHolder.buildRuntime()` 를 `NativeAlrRuntime` 로 교체
  후, RunningSurfaceActivity 에서 **실 GIMP/foot/netsurf 가 Wayland 로 실제 프레임** 표시(WS-1 exec
  re-entry + WS-3 compositor 결선). 이는 기존 device 증거(v111 GIMP 터치, v82 compositor)와 합류 지점.

### 3-C. host 회귀(머지 직후, device 불요)
```
uvx pytest tests/test_android_visible_build_stamp.py \
           tests/test_ui_viewmodel_layer_sources.py \
           tests/test_running_surface_activity_sources.py -q
```

---

## 4. 통합 머지 주의 (stamp · 진입점)

### 4-A. 버전 stamp — 충돌 없음, 그러나 SSOT 동기 확인
- main 의 `app/build.gradle.kts` versionCode **163** / `0.4.163-sd-v163` 이 머지 결과에 그대로 보존됨(검증).
- `tests/test_android_visible_build_stamp.py` 는 **`MainActivity.kt` 소스 텍스트**를 읽어
  `versionCode = 163`, `build: 0.4.163-sd-v163`, `"build:..." < "execution summary"` 를 assert 한다
  — **Manifest 의 LAUNCHER 진입점은 보지 않는다**. 따라서 진입점 강등(§4-B)이 이 테스트를 **깨지 않음**.
- 다음 stamp 범프 시: product-ux 는 stamp 를 건드리지 않으므로 main 의 stamp 핀 사이트만 따르면 됨
  (MEMORY: version-stamp-pin-sites). product-ux 머지로 핀 사이트가 늘지 않음.

### 4-B. 진입점 전환: probe 하니스(MainActivity) → Launcher(LauncherActivity) — **가장 중요한 주의**
머지 후 Manifest 가 이렇게 바뀐다:
- `.ui.LauncherActivity` = **MAIN/LAUNCHER** (신규 Compose 진입점, `exported=true`).
- `.ui.RunningSurfaceActivity` = `exported=false` (LauncherActivity 가 띄움).
- **`.MainActivity` = `exported=false`, intent-filter 제거 → LAUNCHER 강등.**

함의(통합 담당 필독):
1. **앱 아이콘 탭 / `monkey -p ... 1` / 단순 `am start <pkg>` 는 이제 LauncherActivity(목 UI)로 진입**한다.
   기존에 "앱 켜면 자동으로 ~40개 네이티브 probe 가 돌고 Android-visible 리포트가 떴던" 동작은
   **더 이상 자동 실행되지 않는다**. MainActivity 는 `am start -n dev.chanwoo.androlinux/.MainActivity`
   로 **명시 기동**해야만 probe 하니스가 돈다.
2. 기존 device 검증 워크플로(probe 리포트로 GPU/loader/compositor 확인)를 쓰는 게이트는
   **명시적으로 MainActivity 를 띄우도록** 절차를 갱신해야 한다. (force-stop 후
   `am start -n .../.MainActivity` — onCreate 가 `System.loadLibrary("alr_loader")` + 전 probe 실행.)
3. 통합 계획(integration-guide §4-D)은 궁극적으로 MainActivity 의 probe 결과를 Settings 의 진단 화면에
   주입하려 한다 — 그 결선 전까지는 **두 진입점이 공존**(Launcher=제품 UX, MainActivity=진단 명시기동).
4. CI/스크립트가 `resolve-activity ... LAUNCHER` 결과나 "런처 = MainActivity" 를 가정한다면 **갱신 필요**
   (이제 LauncherActivity). 단, 위 build-stamp host 테스트는 영향 없음(§4-A).

> **probe 트리거가 정확히 어디로 갔는지 + 공존 방안 비교/권고는 §5 에 소스 L번호로 실증**한다(이 머지의
> 핵심 질문). 요지: probe 는 전부 `MainActivity.onCreate` 안 → 자동 트리거는 사라지고, `am start -n
> …/.MainActivity` 명시 기동이 유일 경로(권고 = 코드 변경 0 현행 유지).

### 4-C. 런타임 결선 미완 표식
- `AlrRuntimeHolder.buildRuntime()` = `FakeAlrRuntime()` (TODO: ④-B 에서 `NativeAlrRuntime` 교체).
  머지해도 **실 게스트는 안 돈다** — 이는 의도된 v1 상태(흐름 검증용). device 게이트 §3-A 가 이 전제를 명시.

---

## 5. probe 하니스 트리거 정합 — **핵심 질문: 머지 후 probe 시퀀스는 어디서 도나?**

> 통합 머지의 최대 리스크. 이 절은 (5-0) 소스 실증으로 트리거 경로를 못박고, (5-A) device 에서
> probe 를 도는 정확한 절차, (5-B) "제품 모드 ↔ 테스트-하니스 모드" 공존 방안 비교 + **권고**(코드
> 미변경, 변경 지점만 명시)를 담는다. STEP 2 요구의 정합 산출물이다.

### 5-0. 소스 실증 — probe 는 전부 `MainActivity.onCreate` 안에만 있다 (LauncherActivity 엔 0개)

`app/src/main/java/dev/chanwoo/androlinux/` 직접 확인(2026-06-03):

| 항목 | 위치 | 트리거 조건 |
|---|---|---|
| `System.loadLibrary("alr_loader")` + rootfs 설치 | `MainActivity.onCreate` L26-43 | MainActivity 기동 시 **무조건** |
| GPU/loader/compositor 네이티브 probe (~40개: `nativeAlrGpu*`, `nativeJitWx`, `nativeHostGpu`, loader/proot exec, Android-visible `executionSummary`) | `MainActivity.onCreate` (L43~1799 인라인) | MainActivity 기동 시 **무조건** |
| `launchToolkitProbes` (sdl2/qt6/netsurf 등) · 오버레이 스테이징(chromium/foot/gtk3/gpushim/glmark2…) | `MainActivity.onCreate` L298 등 | MainActivity 기동 시 **무조건**(스테이지 tar 존재 시) |
| **aptdrain v2** (`launchAptDrainProbe`) | `MainActivity.onCreate` L294 호출 → 본문 L1972 | **+ 마커** `/data/local/tmp/.alr-aptdrain` (없으면 `return`, L1975). 마커 **내용**=설치 pkg(기본 `hello`; `echo galculator > …` 로 전환) |
| **chromium CR-1** (single-process headless render) | `MainActivity.onCreate` L101 | **+ 마커** `/data/local/tmp/.alr-cr1` |
| **chromium CR-2** (network https fetch+render) | `MainActivity.onCreate` L131 | **+ 마커** `/data/local/tmp/.alr-cr2` |
| `LauncherActivity.onCreate` | `ui/LauncherActivity.kt` L26-35 | **probe 0개.** `AlrRuntimeHolder.get()` + `setContent { AlrApp(...) }` 뿐 |

**구조 확정**: `MainActivity.onCreate` 는 L26~L1799(다음 멤버 `onWindowFocusChanged` 가 L1800)까지 약
1,774줄이 전부 onCreate 본문이고, 모든 probe(무조건 probe + 마커-게이트 probe)가 그 안에 인라인돼 있다.
`LauncherActivity.kt` 는 47줄 전체에 `System.loadLibrary`/`native*`/probe 호출이 **하나도 없다**.

> 마커 게이트의 의미(설계): `.alr-aptdrain`/`.alr-cr1`/`.alr-cr2` 는 **MainActivity 가 이미 돌고 있다는
> 전제 위에서** 무거운 추가 probe 를 켜는 *2차 게이트*다. 이 마커들은 `MainActivity.onCreate` 가
> **실행될 때만** 평가된다. 즉 진입점이 LauncherActivity 면 마커를 push 해도 **아무 일도 안 일어난다**.

### 5-0-결론 (사실)

- **머지 후 앱 아이콘 탭 / `monkey -p … 1` / 단순 `am start <pkg>` → LauncherActivity(목 UI)만 뜬다.**
  `MainActivity.onCreate` 가 호출되지 않으므로 **무조건 probe·마커 게이트 probe 가 전부 자동 트리거되지
  않는다.** 기존 "앱 켜면 ~40개 probe 가 돌고 Android-visible 리포트가 뜨던" device 워크플로는
  **자동으로는 죽는다**(설계상 의도된 강등 — integration-guide §2-B/§4-D).
- **유일한 트리거 경로 = `am start -n dev.chanwoo.androlinux/.MainActivity` 명시 기동**(§5-A). MainActivity
  는 `exported=false` 라 외부 앱은 못 띄우지만 `adb shell am start -n …` 의 동일 셸/디버그 경로는 가능.
- 따라서 **device 검증이 "깨지는" 게 아니라 "진입점이 분리"된 것**이다. probe 절차에 한 줄(`-n …/.MainActivity`)
  만 추가하면 머지 전과 동일하게 전 probe + 마커 게이트가 돈다. **추정 아님 — 소스 L번호로 실증됨.**

### 5-A. device 에서 probe 하니스를 도는 정확한 절차 (머지 후 갱신판)

```
adb shell am force-stop dev.chanwoo.androlinux                 # 재기동 전 항상(overlays/probe onCreate 스킵 방지; MEMORY: device-test-force-stop-first)
# (선택) 마커 게이트 무장 — MainActivity 기동 '전'에 push 해야 onCreate 가 평가함
adb shell touch /data/local/tmp/.alr-aptdrain                  # v2 apt-pipeline drain(기본 hello)
#   echo galculator > /data/local/tmp/.alr-aptdrain  로 galculator 드레인 전환(마커 내용=pkg)
adb shell touch /data/local/tmp/.alr-cr1                       # chromium single-process headless render
adb shell touch /data/local/tmp/.alr-cr2                       # chromium network https fetch
# ★ 핵심: LAUNCHER 가 아니라 MainActivity 를 '명시 기동' (이게 머지 후 유일한 probe 트리거)
adb shell am start -n dev.chanwoo.androlinux/.MainActivity
adb logcat -d | grep -iE 'alr_loader|execution summary|MARSHAL|aptdrain|chromium'   # probe 결과 수집
```

- 마커 없이 `…/.MainActivity` 만 기동하면 **무조건 probe(~40개 GPU/loader/compositor + Android-visible
  리포트)는 그대로 다 돈다**. 마커는 aptdrain/chromium 만 추가로 켠다.
- `am start -n …/.MainActivity` 가 먹히려면 디버그/adb 셸 경로면 충분(exported=false 는 *타 앱* 차단일 뿐).

### 5-B. 공존 방안(제품 UX ↔ 테스트 하니스) — 후보 비교 + 권고

세 후보를 평가한다. **목표: device probe 검증과 product UX 를 둘 다 살린다.**

| 방안 | 내용 | device probe | product UX | 코드 변경량 | 리스크 |
|---|---|---|---|---|---|
| **(A) 명시 기동 — 현행** | LAUNCHER=LauncherActivity 유지, probe 는 `am start -n …/.MainActivity` 로만 | ✅ 절차 한 줄(`-n …/.MainActivity`) | ✅ 무손상 | **0 (변경 없음)** | 없음(자동 트리거만 사라짐; 의도됨) |
| **(B) activity-alias 분기** | manifest 에 `<activity-alias>` 추가, 별도 컴포넌트명으로 MainActivity 를 *또 하나의* 진입점으로 노출(예: `…/.ProbeHarness`) | ✅ alias 컴포넌트 기동 | ✅ 무손상 | manifest +alias 1블록(읽기전용 트랙은 제안만) | 낮음. 단 alias 가 LAUNCHER category 를 또 들면 런처 아이콘 2개 → product UX 흐림 |
| **(C) LauncherActivity 가 마커 보고 probe 위임** | `LauncherActivity.onCreate` 에서 `/data/local/tmp/.alr-harness` 존재 시 `MainActivity` 로 `startActivity` 위임(또는 probe 수집 함수 호출) | ✅ 마커 push→아이콘 탭으로도 probe | ⚠️ product UX 코드에 테스트 분기 침투 | LauncherActivity +몇 줄(**ui/ 코드 수정** — 이 트랙 권한 밖) | 중. 마커 잔존 시 사용자 기기에서 의도치 않게 probe 기동. ui/ 가 본체 동작에 결합 |

**권고 = (A) 현행 유지(코드 변경 0).** 근거:
1. **이미 정합이 성립**한다 — probe 는 `…/.MainActivity` 명시 기동으로 100% 재현되고(§5-A, 소스 실증),
   product UX 는 LauncherActivity 로 무손상. **추가 코드 없이 두 모드가 공존**한다.
2. integration-guide **§4-D 가 이미 정식 통합 경로**(MainActivity probe `executionSummary` → Settings ▸
   진단 주입)를 정의했다. probe 를 product UX 안에서 보고 싶으면 (C) 의 임시 분기가 아니라 **§4-D 결선**으로
   가야 한다(통합 세션 소유, ui/ 와 MainActivity 양쪽 협의 필요). (C) 는 §4-D 의 하위호환 임시방편일 뿐이라
   중복.
3. (B) activity-alias 는 device 자동화에서 "아이콘 없이 두 번째 진입점"이 필요할 때만 가치가 있는데, adb
   셸은 이미 exported=false 컴포넌트를 `-n` 으로 띄울 수 있어 **불필요**. alias 가 LAUNCHER 를 또 들면
   런처 아이콘이 2개가 돼 product UX 를 해친다.

> **만약** CI/자동화가 "아이콘/단순 am start 로 probe 가 자동으로 돌아야 한다"를 *반드시* 요구한다면 →
> 그때만 (B) 를 택하되 **alias 에 LAUNCHER category 를 주지 말고**(MAIN 만, 또는 커스텀 action) 별도
> 컴포넌트명으로 노출한다. 변경 지점: `app/src/main/AndroidManifest.xml` 에 `<activity-alias
> android:name=".ProbeHarness" android:targetActivity=".MainActivity" android:exported="true">` 1블록.
> **본 트랙은 manifest 를 수정하지 않음 — 변경 지점만 명시(STEP 2 제약).**

### 5-C. 변경 지점 요약(택일 시 — 실제 수정은 통합/본체 세션 몫)

- **(A) 현행 권고**: 변경 **없음**. device 절차 문서(이 §5-A)만 따르면 됨.
- (B) alias: `app/src/main/AndroidManifest.xml` 에 `<activity-alias>` 1블록 추가(위 인용).
  host 매니페스트 가시성 테스트가 있으면 alias 추가분 기대값 갱신(있다면).
- (C) 위임: `app/src/main/java/dev/chanwoo/androlinux/ui/LauncherActivity.kt` `onCreate` 에 마커 분기
  (**ui/ 수정 — 본 문서 트랙 권한 밖, 권고 안 함**). §4-D 결선으로 대체 권장.

---

## 부록 — 검증에 쓴 사실(재현용)

- 머지 베이스: `git merge-base main research/product-ux` → `d749073`.
- 충돌 0: `git merge --no-commit --no-ff research/product-ux` (별도 worktree) → EXIT 0, `--diff-filter=U` 빈 결과.
- 양측 공통 변경 파일: `app/build.gradle.kts` 1개(자동 해소; main=versionCode, product-ux=compose, 비겹침 hunk).
- product-ux 신규 49파일(`ui/ runtime/ viewmodel/ docs/design/ tests/ tools/`) = 전부 add, 충돌 불가.
- `FakeAlrRuntime` = 목(게스트 미실행); `AlrRuntimeHolder` 가 프로세스 단일 인스턴스로 공급.
- 시드 설치앱: GIMP·foot·NetSurf / 시드 카탈로그: +GTK3 Demo·SDL2 Demo·nano·htop.
- build-stamp 테스트는 `MainActivity.kt` 텍스트만 검사 → 진입점 강등에 안전.
- 플러그인 버전 정합: kotlin.android 2.0.21 == kotlin.plugin.compose 2.0.21; `android.useAndroidX=true` 추가; `@style/AppTheme` base 존재.
- **probe 트리거 실증(§5-0)**: `MainActivity.onCreate` = L26~L1799(다음 멤버 `onWindowFocusChanged`=L1800);
  무조건 probe + 마커 게이트(`launchAptDrainProbe` L294→본문 L1972 `if(!marker.isFile) return` L1975 /
  `.alr-cr1` L101 / `.alr-cr2` L131) 전부 그 안에 인라인. `ui/LauncherActivity.kt`(47줄)엔
  `System.loadLibrary`/`native*`/probe **0개**. → 아이콘/단순 am start = LauncherActivity 만,
  probe 자동 트리거 안 됨(추정 아님).
- 마커 게이트는 2차 게이트(MainActivity 가 돌아야 평가됨): LauncherActivity 진입 시 마커 push 해도 무효.
- 공존 권고 = 방안 (A) 현행 유지(코드 변경 0); probe 는 `am start -n …/.MainActivity` 로 재현; product
  UX 안 probe 노출은 integration-guide §4-D(Settings▸진단) 결선으로(임시 ui/ 분기 (C) 비권고).
