# product-ux 통합 가이드 (본체/통합 세션용)

**상태: 가이드 + 제안 diff(문서 only).** 본 문서는 product-ux 트랙이 만든 Compose UI
(`ui/` 4화면 + `RunningSurfaceActivity` + `runtime/{AlrRuntime,AppModels,FakeAlrRuntime}` +
`ui/viewmodel/` + `ui/AlrApp.kt` + `ui/theme/Theme.kt`)를 **빌드·실행 통합**하기 위한
정확한 단계·파일·심볼을 제시한다. product-ux 트랙은 다음 파일을 **직접 건드리지 않는다**:
`app/build.gradle.kts`·`MainActivity.kt`·`AndroidManifest.xml`·version stamp. 그래서 이
문서가 **통합 세션이 그 파일들에 무엇을·왜 넣을지** 의 단일 안내서다.

선행 문서: **`docs/design/build-gradle-compose-proposal.md`**(Compose 의존성/플러그인 좌표).
화면 계약 SSOT: **`ui/AlrApp.kt`**(Route 시그니처). 데이터 SSOT: **`runtime/AppModels.kt`** + ADR-004 §5-F.

---

## 0. 통합 후 최종 구조 (목표 상태)

```
LAUNCHER intent  ──▶  LauncherActivity (신규, ComponentActivity)
                         └ setContent { AlrApp(runtime = <실 런타임 or Fake>, onLaunchApp = …) }
                              ├ NavHost: launcher / catalog / appDetail/{id} / settings
                              └ onLaunchApp(LaunchRequest)  ──▶  RunningSurfaceActivity (View + SurfaceView)
                                                                     └ provideRuntime() = 프로세스 공유 런타임
MainActivity (기존 probe 하니스)  ──▶  Settings ▸ 진단(Diagnostics) 으로 강등(LAUNCHER 제거)
```

다섯 작업:
1. **Compose 의존성 추가** (build-gradle-compose-proposal.md 반영)
2. **AndroidManifest 등록** — `LauncherActivity`(MAIN/LAUNCHER) + `RunningSurfaceActivity`; MainActivity 는 LAUNCHER 강등
3. **AlrApp.kt placeholder Route 4개 정리** — redeclaration 충돌 해소(v1 부터의 빌드 이슈)
4. **FakeAlrRuntime → 실 AlrRuntime 배선** — 컴포지터 bindSurface(WS-3) + apt 설치 install()
5. **아이콘 Coil 로더 + SAF picker(ACTION_OPEN_DOCUMENT_TREE) Activity 배선**

> 각 단계는 독립적으로 빌드 가능하도록 설계했다. 1·2·3 만 해도 **FakeAlrRuntime 로 전 화면이
> 실기기에서 동작**(목 데이터)한다. 4·5 는 실 런타임/파일연동을 점진 결선한다.

---

## 1. 단계 ① — Compose 의존성 추가

`docs/design/build-gradle-compose-proposal.md` 를 그대로 반영한다. 요지만:

- 루트 + `app/build.gradle.kts` `plugins` 에 `org.jetbrains.kotlin.plugin.compose`(버전
  `2.0.21`, **Kotlin 버전과 동일**). **`composeOptions { kotlinCompilerExtensionVersion }`
  은 쓰지 않는다**(Kotlin 2.0 부터 컴파일러가 Kotlin 에 포함됨).
- `android.buildFeatures { compose = true }` (BuildConfig 사용 시 `buildConfig = true` 도).
- `dependencies` 에 Compose BOM(`2024.09.00`) + `material3`/`foundation`/`navigation-compose`/
  `activity-compose`/`lifecycle-viewmodel-compose`/`lifecycle-runtime-ktx`/`material-icons-core`/
  `kotlinx-coroutines-android` (+ 선택 `coil-compose`).

검증: `./gradlew :app:compileDebugKotlin` 가 `ui/`·`runtime/` import 를 전부 해소해야 한다
(단, 단계 ③ 을 함께 해야 redeclaration 없이 통과 — 아래).

---

## 2. 단계 ② — AndroidManifest 등록 (LauncherActivity + RunningSurfaceActivity, MainActivity 강등)

### 2-A. 신규 진입점 `LauncherActivity` (통합 세션이 신규 .kt 로 추가)

`AlrApp(...)` 를 `setContent` 로 띄울 **ComponentActivity** 가 필요하다. 현 `MainActivity`
는 `class MainActivity : Activity()`(plain Activity)라 `setContent` 를 못 쓴다 — 그래서
**새 Activity** 를 만든다(MainActivity 는 본체 소유라 본 트랙이 못 만든다; 통합 세션이 생성).

```kotlin
// app/src/main/java/dev/chanwoo/androlinux/ui/LauncherActivity.kt  (신규 — 통합 세션 소유)
package dev.chanwoo.androlinux.ui

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import dev.chanwoo.androlinux.runtime.AlrRuntime
import dev.chanwoo.androlinux.runtime.FakeAlrRuntime
import dev.chanwoo.androlinux.runtime.LaunchRequest

class LauncherActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // v1: FakeAlrRuntime. 단계 ④ 에서 프로세스 공유 실 런타임으로 교체(AlrRuntimeHolder).
        val runtime: AlrRuntime = AlrRuntimeHolder.get(applicationContext)   // 단계 ④ 참고
        setContent {
            AlrApp(
                runtime = runtime,
                onLaunchApp = { req -> startRunningSurface(req) },
            )
        }
    }

    /** AlrApp.onLaunchApp 위임 — LaunchRequest → RunningSurfaceActivity Intent extra. */
    private fun startRunningSurface(req: LaunchRequest) {
        val i = Intent(this, RunningSurfaceActivity::class.java).apply {
            putExtra(RunningSurfaceActivity.EXTRA_APP_ID, req.appId)
            putExtra(RunningSurfaceActivity.EXTRA_ENTRY_PATH, req.entryPath)
            putExtra(RunningSurfaceActivity.EXTRA_ARGS, req.args.toTypedArray())
            putExtra(RunningSurfaceActivity.EXTRA_PROTOCOL, req.protocol.name)
        }
        startActivity(i)
    }
}
```

> `RunningSurfaceActivity` 의 extra 키/프로토콜 문자열은 **이미 정의돼 있다**(읽기 검증):
> `EXTRA_APP_ID`/`EXTRA_ENTRY_PATH`/`EXTRA_ARGS`(String[])/`EXTRA_PROTOCOL`(`"WAYLAND"|"X11"`),
> 그리고 `RunningSurfaceActivity.parseRequest()` 가 이 키들을 그대로 복원한다 — 위 매핑이 정확하다.

### 2-B. 매니페스트 제안 diff

```xml
<!-- AndroidManifest.xml — 제안 diff (통합 세션 반영) -->
<application ...>

    <!-- ① 새 런처 진입점: MAIN/LAUNCHER 를 LauncherActivity 로 이전 -->
    <activity
        android:name=".ui.LauncherActivity"
        android:exported="true">
        <intent-filter>
            <action android:name="android.intent.action.MAIN" />
            <category android:name="android.intent.category.LAUNCHER" />
        </intent-filter>
    </activity>

    <!-- ② 게스트 실행 화면 (내부 진입 — exported=false, LauncherActivity 가 띄움) -->
    <activity
        android:name=".ui.RunningSurfaceActivity"
        android:exported="false"
        android:configChanges="orientation|screenSize|keyboardHidden|density"
        android:theme="@style/AppTheme" />

    <!-- ③ 기존 MainActivity: LAUNCHER 강등 (probe 하니스 → 진단 전용) -->
    <activity
        android:name=".MainActivity"
        android:exported="false" />
        <!--  ↑ <intent-filter> MAIN/LAUNCHER 제거. exported 도 false 로
              (외부 진입 불요). 진단은 Settings ▸ 진단 에서 호출(단계 ④-D). -->

</application>
```

**근거/주의:**
- `RunningSurfaceActivity` 는 `configChanges` 로 회전/리사이즈를 **자체 처리**하게 둔다 —
  SurfaceView + 컴포지터가 surface 재생성에 민감하므로 Activity 재생성을 피한다.
- MainActivity 의 `<intent-filter>` 를 제거하면 **런처 아이콘이 MainActivity → LauncherActivity
  로 바뀐다**. MainActivity 를 완전히 죽이지 않는 이유: 약 40개 네이티브 probe 하니스를
  진단(Diagnostics) 소스로 재활용하기 위함(단계 ④-D). 외부에서 더 이상 직접 안 띄우므로
  `exported="false"`.
- `android:label`/`android:theme` 는 `<application>` 레벨 것을 상속한다 — 별도 지정 불필요.
  (단 `RunningSurfaceActivity` 는 풀스크린 컴포지터라 통합 시 immersive/no-action-bar 테마를
  따로 줄 수 있다 — v1 은 상속 `@style/AppTheme` 로 충분.)
- **host 테스트 영향:** `tests/test_android_visible_build_stamp.py` 등 매니페스트/런처
  가시성 테스트가 있으면, LAUNCHER 가 `MainActivity` → `LauncherActivity` 로 옮겨간 것을
  반영하도록 통합 세션이 그 테스트의 기대값을 함께 갱신해야 한다(본 트랙 미접촉).

---

## 3. 단계 ③ — AlrApp.kt placeholder Route 4개 정리 (redeclaration 충돌 해소)

**이것이 v1 부터의 빌드 이슈다.** `ui/AlrApp.kt` 하단(L211–276)에는 4개 Route 의 *placeholder
구현*이 있고, Screens 트랙의 실제 화면 파일이 **같은 package·같은 함수명**으로 구현을 또
선언한다 → **redeclaration 컴파일 충돌**. 단 해소 방식이 **화면마다 다르다** — 아래 표가 핵심.

| Route | AlrApp.kt placeholder | 실 구현 파일·심볼 | 충돌? | 해소 |
|---|---|---|---|---|
| `LauncherRoute(installedApps, onLaunch, onOpenCatalog, onOpenSettings, onOpenAppDetail)` | AlrApp.kt **L230–239** | `LauncherScreen.kt:106` `fun LauncherRoute(…동일 시그니처…)` | **예** (동일명) | AlrApp.kt placeholder **삭제** |
| `CatalogRoute(catalog, installedAppIds, onOpenAppDetail, onBack)` | AlrApp.kt **L242–250** | `CatalogScreen.kt:140` `fun CatalogRoute(…동일…)` | **예** | AlrApp.kt placeholder **삭제** |
| `AppDetailRoute(appId, catalogApp, installedApp, installProgress, uninstallProgress, onOpen, onBack)` | AlrApp.kt **L256–267** | `AppDetailScreen.kt:116` `fun AppDetailRoute(…동일…)` | **예** | AlrApp.kt placeholder **삭제** |
| `SettingsRoute(installedApps, onBack)` | AlrApp.kt **L270–276** | `SettingsScreen.kt:157` `fun SettingsScreenRoute(…)` — **다른 이름** | **아니오** | placeholder **유지**, 바디만 `SettingsScreenRoute(...)` 호출로 교체 |

> 즉 **Launcher/Catalog/AppDetail 은 placeholder 를 통째로 지우고**(실 구현이 같은 이름을
> 이미 들고 있으므로 AlrApp 의 NavHost 가 자동으로 그 실 구현을 호출), **Settings 만은
> placeholder 함수 껍데기를 남기고 바디를 한 줄 교체**한다(실 구현이 `SettingsScreenRoute`
> 라는 다른 이름이라 충돌이 없고, 위임이 필요).

### 3-A. 정확한 편집 (제안 diff)

**(a) Launcher/Catalog/AppDetail placeholder 3개 삭제** — AlrApp.kt L211~267 중 해당 함수와
계약 주석을 제거한다(시그니처 SSOT 주석은 남겨도 무방하나, `fun` 본문은 삭제). 삭제 대상:

```kotlin
// ── AlrApp.kt 에서 삭제 ──
@Composable
fun LauncherRoute(installedApps: …, onLaunch: …, onOpenCatalog: …, onOpenSettings: …, onOpenAppDetail: …) {
    ScreenPlaceholder("Launcher", "${installedApps.size}개 설치됨")
}
@Composable
fun CatalogRoute(catalog: …, installedAppIds: …, onOpenAppDetail: …, onBack: …) {
    ScreenPlaceholder("Catalog", "${catalog.size}개 / 설치 ${installedAppIds.size}")
}
@Composable
fun AppDetailRoute(appId: …, catalogApp: …, installedApp: …, installProgress: …, uninstallProgress: …, onOpen: …, onBack: …) {
    ScreenPlaceholder("AppDetail", catalogApp?.name ?: installedApp?.name ?: appId)
}
```

삭제 후에도 `launcherDestination`/`catalogDestination`/`appDetailDestination`(AlrApp.kt
L123–179)이 `LauncherRoute(...)`/`CatalogRoute(...)`/`AppDetailRoute(...)` 를 호출하는 코드는
**그대로 둔다** — 이제 그 호출은 `LauncherScreen.kt`/`CatalogScreen.kt`/`AppDetailScreen.kt`
의 실 구현으로 해석된다.

**(b) SettingsRoute 는 유지하되 바디를 위임으로 교체** — AlrApp.kt L270–276:

```kotlin
// ── AlrApp.kt — 교체 전 (placeholder) ──
@Composable
fun SettingsRoute(installedApps: List<InstalledApp>, onBack: () -> Unit) {
    ScreenPlaceholder("Settings", "${installedApps.size}개 앱")
}

// ── AlrApp.kt — 교체 후 (실 화면 위임) ──
@Composable
fun SettingsRoute(installedApps: List<InstalledApp>, onBack: () -> Unit) {
    SettingsScreenRoute(
        installedApps = installedApps,
        onBack = onBack,
        // diagnostics = …  ← 단계 ④-D 에서 executionSummary 주입
        // onPickFolder = … ← 단계 ⑤ 에서 SAF 런처 결선
    )
}
```

`SettingsScreenRoute` 의 `diagnostics`/`uiState`/`onPickFolder` 는 **기본값이 있어**(빈
리스트·새 `SettingsUiState`·no-op) 위처럼 2-인자로만 불러도 컴파일/동작한다. 진단·SAF 결선은
④-D/⑤ 에서 인자를 더해 붙인다.

**(c) ScreenPlaceholder 헬퍼 처리** — Launcher/Catalog/AppDetail 삭제 후 `ScreenPlaceholder`
(AlrApp.kt L282–288)의 마지막 사용처가 사라진다. `SettingsRoute` 도 위임으로 바뀌면
`ScreenPlaceholder` 는 미사용이 되므로 함께 삭제한다(미사용 private 함수 경고 방지).
`RunningSurfacePlaceholder`(L290–295)는 `RUNNING_SURFACE` 라우트(L113)가 디자인 Preview 용으로
계속 쓰므로 **남긴다**.

### 3-B. 정리 후 컴파일 불변식

- `dev.chanwoo.androlinux.ui` 패키지에 `LauncherRoute`/`CatalogRoute`/`AppDetailRoute` 가
  **정확히 1개씩**(각 Screen 파일)만 존재.
- `SettingsRoute`(AlrApp.kt, 위임) + `SettingsScreenRoute`(SettingsScreen.kt, 구현) 공존 — 이름이 달라 OK.
- `AlrApp.kt` 의 destination 함수들은 그대로 — Route 호출 대상만 실 화면으로 해석.

---

## 4. 단계 ④ — FakeAlrRuntime → 실 AlrRuntime 배선

### 4-A. 프로세스 공유 런타임 홀더 (INV-1~3 전역 강제의 전제)

`AlrRuntime` 의 단일 포그라운드 불변식(INV-1~3, `AlrRuntime.kt` 주석)은 **프로세스에 런타임
인스턴스가 하나**일 때만 전역으로 성립한다. `RunningSurfaceActivity.provideRuntime()` 와
`LauncherActivity` 가 **같은 인스턴스**를 봐야 한다. 통합 세션이 작은 홀더(또는 Application
싱글턴/DI)를 둔다:

```kotlin
// app/src/main/java/dev/chanwoo/androlinux/runtime/AlrRuntimeHolder.kt  (신규 — 통합 세션 소유)
package dev.chanwoo.androlinux.runtime

import android.content.Context

object AlrRuntimeHolder {
    @Volatile private var instance: AlrRuntime? = null
    fun get(context: Context): AlrRuntime =
        instance ?: synchronized(this) {
            instance ?: buildRuntime(context.applicationContext).also { instance = it }
        }
    // v1: FakeAlrRuntime. 단계 ④-B 에서 실 런타임(NativeAlrRuntime)으로 교체.
    private fun buildRuntime(appContext: Context): AlrRuntime = FakeAlrRuntime()
}
```

그리고 `RunningSurfaceActivity.provideRuntime()` 를 홀더로 가리키게 한다. **두 방법:**
- (권장) `RunningSurfaceActivity` 의 `provideRuntime()` 본문을
  `AlrRuntimeHolder.get(applicationContext)` 로 바꾼다. 단 `RunningSurfaceActivity` 는
  product-ux 트랙 소유 파일이라 **본 트랙이 직접 편집 가능**(본체 파일 아님). 통합과 합의 시
  본 트랙이 이 한 줄을 커밋하거나, 통합 세션이 서브클래스로 override 한다.
- (대안) `RunningSurfaceActivity` 를 상속한 `class RealRunningSurfaceActivity :
  RunningSurfaceActivity() { override fun provideRuntime() = AlrRuntimeHolder.get(applicationContext) }`
  를 만들고 매니페스트에 그쪽을 등록. (현 설계가 `provideRuntime()` 를 `protected open` 으로
  열어둔 의도가 이것 — `RunningSurfaceActivity.kt:273`.)

> 권장안(직접 한 줄 교체)이 단순하다. `provideRuntime()` 시그니처/가시성이 이미 그 교체를
> 전제로 설계됐다.

### 4-B. 실 런타임 `NativeAlrRuntime` 구현 계약 (WS-1/WS-3 결선점)

실 런타임은 `AlrRuntime` 인터페이스(`AlrRuntime.kt`)를 구현하고 **FakeAlrRuntime 의 INV-1~3
전이 로직을 그대로 따른다**(`FakeAlrRuntime.promoteToForeground` 가 레퍼런스). 결선점:

| 인터페이스 멤버 | 실 결선 (네이티브/WS) |
|---|---|
| `installedApps: StateFlow<List<InstalledApp>>` | 매니페스트 스토어(설치 마커 `.{name}-staged-<size>`) 스캔 → StateFlow |
| `catalog(): Flow<List<CatalogApp>>` | 번들 stage-tar 매니페스트 + apt 인덱스(catalog-apt-v1.md) |
| `launch(req): AppSession` | INV-2 양도 후 새 세션 STARTING; 세션이 `bindSurface(holder)` 에서 **WS-3 컴포지터**에 SurfaceHolder 양도 |
| `install(appId): Flow<InstallProgress>` | `RootfsInstaller.extractOverlayTar`(stage-tar v1) / 인-게스트 apt(v2); 진행 단계 = `InstallStage` |
| `uninstall(appId)` | overlay/마커 제거 |

**`AppSession.bindSurface(holder)` (WS-3 결선의 핵심):** `FakeAppSession.bindSurface` 는
no-op(holder 기록만, `FakeAlrRuntime.kt:510`)이다. 실 세션은 여기서 WS-3 컴포지터에
`SurfaceHolder.surface` 를 넘겨 `nativeWaylandCompositorStart` + present 를 시작하고, 첫
프레임 present 시 세션 state 를 `STARTING → RENDERING` 으로 올린다(메모리 WS-3 §5-C
PresentSource 와 정합). `RunningSurfaceActivity` 의 `surfaceCreated → session.bindSurface(holder)`
경로는 **이미 구현돼 있다**(`RunningSurfaceActivity.kt:227`) — 실 세션만 끼우면 된다.

**`install()` apt 결선:** v1 은 `RootfsInstaller.extractOverlayTar` 로 번들 stage-tar 를
풀고, v2 는 인-게스트 apt 다. 진행률 매핑은 `InstallStage`(RESOLVING/DOWNLOADING/EXTRACTING/
REGISTERING/REMOVING, `AppModels.kt:233`) 그대로 — UI 는 진행률만 본다(ADR-004 §7).

> **선택 능력 인터페이스:** ViewModel 들이 `runtime as? CatalogExtras`/`as? InstallQueueInfo`/
> `as? CrashSimulator` 로 안전 다운캐스트한다(`FakeAlrRuntime.kt:45–66`). 실 런타임은 이들을
> **구현하지 않아도 graceful 폴백**한다(데모용 오프라인/큐/크래시 주입은 Fake 전용).

### 4-C. 교체 시점

`AlrRuntimeHolder.buildRuntime` 한 곳만 `FakeAlrRuntime()` → `NativeAlrRuntime(appContext)` 로
바꾸면 전 화면이 실 런타임으로 전환된다(나머지 UI 코드 불변 — 단방향 경계의 이점).

### 4-D. MainActivity probe → Settings 진단 주입

MainActivity 의 ~40개 네이티브 probe 결과(`executionSummary`, MainActivity 내)를
`List<DiagnosticLine>`(`SettingsScreen.kt:184` `DiagnosticLine(label, status, detail)`)로
매핑해 `SettingsScreenRoute(diagnostics = …)` 로 주입한다. 매핑 규칙:

- PASS/FAIL/SKIP/INFO 문자열 → `DiagnosticStatus`(`SettingsScreen.kt:190`).
- 한 probe 라인 → `DiagnosticLine(label = probe명, status = …, detail = "v76"/"device-pending" 등)`.

통합 세션이 probe 수집을 함수로 빼서(또는 MainActivity 가 결과를 공유 홀더에 적재)
`SettingsRoute` 가 그것을 읽어 `SettingsScreenRoute(diagnostics = collected)` 로 넘긴다.
진단은 **개발자/지원 경로**라 일반 실행에선 수집 안 함(빈 리스트 → "수집 안 됨" 안내,
`SettingsScreen.DiagnosticsSection`).

---

## 5. 단계 ⑤ — 아이콘 Coil 로더 + SAF picker(ACTION_OPEN_DOCUMENT_TREE) 결선

### 5-A. 아이콘 Coil 로더

현 화면은 아이콘을 **카테고리 머리글자 원형 배지**로 폴백한다(`LauncherScreen.AppIcon`,
`LauncherScreen.kt:290`; CatalogScreen 도 동일 주석 자리 `CatalogScreen.kt:536`). rootfs 의
실제 `.png`(`InstalledApp.iconPath`/`CatalogApp.iconPath`, 예
`/usr/share/icons/hicolor/256x256/apps/gimp.png`)를 디코드하려면:

1. `build.gradle` 에 `io.coil-kt:coil-compose:2.7.0`(proposal §4, 선택) 추가.
2. `AppIcon` 의 폴백 배지를 Coil 로 감싸 **경로 존재 시 비트맵, 실패/`null` 시 배지 폴백**:

```kotlin
// LauncherScreen.AppIcon 결선 예 (통합/본 트랙)
@Composable
private fun AppIcon(app: InstalledApp) {
    val iconPath = app.iconPath
    if (iconPath != null) {
        AsyncImage(
            model = java.io.File(iconPath),     // rootfs 절대경로 → File
            contentDescription = app.name,
            modifier = Modifier.size(64.dp).clip(CircleShape),
            error = painterResource(/* 폴백 배지 or ColorPainter */),
        )
    } else {
        // 기존 머리글자 배지 폴백 유지
    }
}
```

> **주의:** `iconPath` 는 **게스트 rootfs 안의 절대경로**(예 `/usr/.../gimp.png`)다. 안드로이드
> 파일시스템에서 바로 못 읽을 수 있으니, 통합 세션이 (a) rootfs 마운트/추출 경로로 변환하거나
> (b) 설치 시 아이콘을 앱-private(`filesDir/icons/<appId>.png`)로 복사해 그 경로를 `iconPath`
> 로 채우는 편이 안전하다. 후자가 권장(Coil 이 그대로 디코드). `coil-compose` 미추가 시
> 현 배지 폴백으로 **그대로 동작**한다(아이콘 결선은 선택 단계).

### 5-B. SAF picker(ACTION_OPEN_DOCUMENT_TREE) Activity 결선

`SettingsScreen` 의 SAF 마운트 등록은 picker 진입을 `onPickFolder: () -> Unit` 콜백으로
**위임만** 한다(`SettingsScreen.kt:162`, `SettingsScreenRoute`). 실제 시스템 picker 결선은
통합 세션이 Compose `rememberLauncherForActivityResult` 로 한다:

```kotlin
// SettingsRoute 결선부 (AlrApp.kt 의 settingsDestination 또는 래퍼에서)
@Composable
fun SettingsRouteWired(installedApps: List<InstalledApp>, onBack: () -> Unit) {
    val uiState = remember { SettingsUiState() }
    val context = LocalContext.current
    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { treeUri: Uri? ->
        if (treeUri != null) {
            // 영속 권한 취득(프로세스 재시작 후에도 유효).
            context.contentResolver.takePersistableUriPermission(
                treeUri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
            )
            val label = deriveLabel(treeUri)            // 폴더명 → ^[A-Za-z0-9._-]+$ 정규화
            uiState.register(SafMountEntry(label = label, displayName = label, treeUri = treeUri.toString()))
            // 런타임/path-mediation 결선(WS-1·C 트랙): saf_bridge_model 의 /mnt/android/<label>.
        }
    }
    SettingsScreenRoute(
        installedApps = installedApps,
        onBack = onBack,
        uiState = uiState,
        onPickFolder = { picker.launch(null) },
        // diagnostics = …  (단계 ④-D)
    )
}
```

**근거/정합:**
- `OpenDocumentTree` 는 **권한 선언 불요**(`androidmanifest-permissions-proposal.md` / ADR-004
  §9). 그래서 매니페스트에 storage 권한을 더할 필요가 없다 — picker 가 SAF 트리 URI 를 돌려준다.
- 등록된 마운트는 `SafMountEntry(label, displayName, treeUri)` 로 `SettingsUiState.register`
  에 들어가고, 게스트 경로는 UI 가 `/mnt/android/<label>`(`SettingsScreen.SAF_MOUNT_ROOT`)로
  합성 표시한다 — `tools/saf_bridge_model.py` 의 `SafMount.guest_mount_point` 와 1:1.
- `treeUri` 영속(`takePersistableUriPermission`)과 게스트 path-mediation(SAF 프록시/copy 폴백,
  `docs/design/saf-proxy.md`) 결선은 WS-1/C 트랙 소관 — 본 화면은 등록/표시만.
- `SettingsScreenRoute` 가 `uiState`/`onPickFolder` 를 파라미터로 받으므로(기본값 존재), 위처럼
  **외부에서 picker 를 주입**하면 화면은 그대로 둔 채 실 SAF 가 붙는다.

> `SettingsRoute`(AlrApp.kt)에서 위 `SettingsRouteWired` 패턴을 인라인해도 되고, 별도 결선
> Composable 로 빼도 된다 — 화면(`SettingsScreen`) 자체는 불변.

---

## 6. 통합 순서 권장 + 단계별 동작 보증

| 순서 | 단계 | 완료 시 동작 | 위험/주의 |
|---|---|---|---|
| 1 | ①Compose 의존성 + ③placeholder 정리 | `compileDebugKotlin` 통과 | ③ 없이 ① 만 하면 redeclaration 으로 실패 — **①③ 동시** |
| 2 | ②매니페스트(LauncherActivity/RunningSurface, MainActivity 강등) | 앱 실행 → 런처 그리드(목 데이터) → 카탈로그/상세/설정 네비 + 실행화면 로딩 오버레이까지 **실기기 동작**(FakeAlrRuntime) | LAUNCHER 이전으로 매니페스트 host 테스트 기대값 갱신 필요 |
| 3 | ④-A 홀더 + ④-D 진단 | 진단 화면에 probe 결과 노출; 런타임 단일 인스턴스 보장 | provideRuntime 한 줄 교체(또는 서브클래스) 합의 |
| 4 | ④-B/C 실 런타임 교체 | `AlrRuntimeHolder.buildRuntime` 한 줄로 실 게스트 실행(WS-3 컴포지터 present) | bindSurface 결선·STARTING→RENDERING 전이가 WS-3 PresentSource 와 맞물려야 |
| 5 | ⑤ Coil + SAF | 실제 아이콘 + 안드로이드 폴더 연동 | iconPath 는 앱-private 복사 권장; treeUri 영속/path-mediation 은 WS-1/C |

**①③ 만으로도 빌드가 서고, ② 까지면 FakeAlrRuntime 로 전 화면이 실기기에서 흐른다.**
④⑤ 는 실 런타임/파일연동을 점진 결선하는 추가 작업이다.

---

## 7. 본 트랙 미접촉 파일 / 통합 세션 소유 작업 (요약)

product-ux 트랙이 **직접 안 건드리는** 파일(통합/본체 세션이 본 가이드대로 반영):
- `app/build.gradle.kts` (단계 ①) — Compose 플러그인/buildFeatures/dependencies
- 루트 `build.gradle.kts` (단계 ①) — Compose Compiler 플러그인 별칭
- `AndroidManifest.xml` (단계 ②) — LauncherActivity/RunningSurfaceActivity 등록, MainActivity 강등
- `MainActivity.kt` (단계 ④-D) — probe 결과를 진단 소스로 공유(필요 시)
- version stamp — 불변(본체 소유)

통합 세션이 **신규로 만드는** 파일:
- `ui/LauncherActivity.kt` (단계 ②-A)
- `runtime/AlrRuntimeHolder.kt` (단계 ④-A)
- `runtime/NativeAlrRuntime.kt` (단계 ④-B, WS-1/WS-3 결선)

product-ux 트랙이 **소유·편집 가능**(본체 파일 아님):
- `ui/AlrApp.kt` (단계 ③ placeholder 정리) — 단, 빌드 통합과 함께 가야 하므로 통합 세션과
  순서 합의 권장(③ 단독 커밋 시 실 화면 파일과 동시에 가야 컴파일 성립).
- `ui/RunningSurfaceActivity.kt` (단계 ④-A provideRuntime 한 줄) — `protected open` 설계대로.

> 모든 좌표·라인·심볼은 현 worktree(`research/product-ux`) 소스를 직접 읽어 확정했다. 라인
> 번호는 편집에 따라 이동할 수 있으니 **심볼명**(`LauncherRoute`/`SettingsScreenRoute`/
> `provideRuntime`/`bindSurface` 등)을 기준으로 적용한다.
