# build.gradle(.kts) — Compose 의존성 추가 제안 (product-ux 통합)

**상태: 제안만(문서 only).** `app/build.gradle.kts` 직접 수정 금지 — 통합/본체 세션 소유 파일.
이 문서는 product-ux 트랙의 Compose 화면(`ui/` 4화면 + `RunningSurfaceActivity` +
`runtime/`)이 **컴파일·실행되도록** `app/build.gradle.kts` 에 추가할 **정확한 좌표/버전/
블록**을 제시한다. 모든 좌표는 현 빌드 환경(아래 §0)에 정합한다.

연계 문서:
- 단계별 통합 절차 = `docs/design/product-ux-integration-guide.md`
- 화면 계약(SSOT) = `app/src/main/java/dev/chanwoo/androlinux/ui/AlrApp.kt`
- 매니페스트 등록 = 본 문서 §6 + integration-guide §2

---

## 0. 현 빌드 환경 (있는 그대로 — 정합 기준)

| 항목 | 값 | 출처 |
|---|---|---|
| AGP | `8.7.3` | 루트 `build.gradle.kts` |
| Kotlin | `2.0.21` | 루트 `build.gradle.kts` |
| Gradle wrapper | `8.10.2` | `gradle/wrapper/gradle-wrapper.properties` |
| compileSdk / targetSdk | `35` / `35` | `app/build.gradle.kts` |
| minSdk | `26` | `app/build.gradle.kts` |
| JVM target | `17` | `app/build.gradle.kts` (`compileOptions`/`kotlinOptions`) |

**★ 결정적 사실 (Kotlin 2.0):** Kotlin **2.0 부터 Compose 컴파일러가 Kotlin 배포에
포함**된다. 따라서 구식의 `composeOptions { kotlinCompilerExtensionVersion = "..." }`
는 **쓰지 않는다** — 대신 **Compose Compiler Gradle 플러그인**
(`org.jetbrains.kotlin.plugin.compose`)을 적용하고, 그 버전은 **Kotlin 버전과 동일
(2.0.21)** 이다. (옛 매핑 표에서 Kotlin 버전 → kotlinCompilerExtensionVersion 을 찾는
방식은 1.9.x 까지의 것이며 이 프로젝트엔 부적합.)

---

## 1. 루트 `build.gradle.kts` — 플러그인 별칭 추가 (제안)

루트 plugins 블록에 Compose Compiler 플러그인을 `apply false` 로 추가(버전 = Kotlin
버전과 동일). 이미 있는 두 줄 아래에 한 줄을 더한다.

```kotlin
// 루트 build.gradle.kts — 제안 diff
plugins {
    id("com.android.application") version "8.7.3" apply false
    id("org.jetbrains.kotlin.android") version "2.0.21" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.0.21" apply false   // ← 추가
}
```

> 플러그인은 `gradlePluginPortal()` 에서 해석된다 — `settings.gradle.kts` 의
> `pluginManagement.repositories` 에 이미 `gradlePluginPortal()` 이 있어 추가 저장소
> 설정은 불필요하다(확인됨).

---

## 2. `app/build.gradle.kts` — plugins 블록 (제안 diff)

```kotlin
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")   // ← 추가 (버전은 루트에서 고정)
}
```

---

## 3. `android { }` 블록 — buildFeatures.compose 활성화 (제안 diff)

`composeOptions` 는 **추가하지 않는다**(§0 참고 — Kotlin 2.0 에선 불필요·deprecated).
`buildFeatures { compose = true }` 한 줄만 켠다. 기존 `compileOptions`/`kotlinOptions`/
`androidResources`/`packaging` 등은 그대로 둔다.

```kotlin
android {
    namespace = "dev.chanwoo.androlinux"
    compileSdk = 35

    // ... (기존 androidResources / compileOptions / kotlinOptions / packaging 유지) ...

    buildFeatures {
        compose = true            // ← 추가
    }

    // ❌ composeOptions { kotlinCompilerExtensionVersion = ... }  ← 넣지 말 것 (Kotlin 2.0)

    defaultConfig {
        // ... 기존 그대로 ...
        // ★ integration-guide §4(런타임 정보)에서 BuildConfig.VERSION_NAME 을 SettingsScreen 에
        //   주입하려면 buildFeatures.buildConfig = true 가 필요할 수 있다(AGP 8 부터 기본 off).
        //   필요 시 위 buildFeatures 블록에 buildConfig = true 를 함께 켠다.
    }
}
```

> **buildConfig 주의:** AGP 8 부터 `BuildConfig` 생성은 기본 비활성이다. SettingsScreen 의
> "빌드" 정보 행에 `BuildConfig.VERSION_NAME` 을 쓰려면 `buildFeatures { buildConfig = true }`
> 도 함께 켠다. (version stamp 자체는 본 트랙이 건드리지 않는다 — `versionName` 은 본체 소유.)

---

## 4. `dependencies { }` — Compose BOM + 화면이 실제로 쓰는 좌표 (제안 diff)

아래 좌표는 **소스 파일이 실제로 import 하는 심볼**에 1:1로 맞췄다(불필요한 의존 없음).
버전은 BOM(`androidx.compose:compose-bom`)이 일괄 고정하므로 **개별 compose 좌표엔 버전을
적지 않는다**. BOM `2024.09.00` 은 Compose Compiler 2.0.21 / AGP 8.7 과 정합한다.

```kotlin
dependencies {
    // ── 기존 (유지) ──────────────────────────────────────────────
    implementation("org.apache.commons:commons-compress:1.26.2")

    // ── Compose BOM (버전 일괄 고정) ─────────────────────────────
    val composeBom = platform("androidx.compose:compose-bom:2024.09.00")
    implementation(composeBom)
    androidTestImplementation(composeBom)

    // ── Compose 코어/UI (버전은 BOM 이 고정 — 좌표만) ────────────
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")             // ColorPainter/Painter (AppDetailScreen)
    implementation("androidx.compose.ui:ui-tooling-preview")      // @Preview (모든 화면)
    implementation("androidx.compose.foundation:foundation")      // LazyVerticalGrid/LazyRow/combinedClickable
    implementation("androidx.compose.material3:material3")        // Material3 (Scaffold/Card/Chip/…)
    implementation("androidx.compose.material:material-icons-core") // Icons.Filled.Add/Delete/Info/Search/Settings

    // ── Activity / Lifecycle / Navigation / ViewModel(Compose) ──
    implementation("androidx.activity:activity-compose:1.9.2")    // setContent / ComponentActivity (LauncherActivity)
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")          // lifecycleScope/repeatOnLifecycle (RunningSurfaceActivity)
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")    // viewModel(factory=…) (AlrApp 라우트)
    implementation("androidx.navigation:navigation-compose:2.8.1")            // NavHost/composable/rememberNavController (AlrApp)

    // ── 코루틴 (StateFlow/Flow — runtime/ 전체가 의존) ───────────
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    // ── (선택) 아이콘 비트맵 로더 Coil — integration-guide §5 ────
    //   현 화면은 아이콘을 카테고리 머리글자 배지로 폴백한다(Coil 없이 컴파일/동작).
    //   rootfs 의 실제 .png 아이콘(InstalledApp.iconPath)을 디코드하려면 추가:
    implementation("io.coil-kt:coil-compose:2.7.0")

    // ── (선택) Compose 디버그 도구 — Preview 렌더/레이아웃 인스펙터 ─
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
```

### 4-A. 좌표 ↔ 사용처 매핑 (검증 가능한 근거)

| 좌표 | 어디서 쓰는가 (파일·심볼) |
|---|---|
| `compose.ui` / `ui-graphics` | 모든 화면 `Modifier`/`Color`; `AppDetailScreen.kt` `ColorPainter`/`Painter` |
| `ui-tooling-preview` | 모든 화면의 `@Preview` (`androidx.compose.ui.tooling.preview.Preview`) |
| `compose.foundation` | `LauncherScreen.kt` `LazyVerticalGrid`/`combinedClickable`; `CatalogScreen.kt` `LazyRow`; `SettingsScreen.kt` `verticalScroll` |
| `material3` | 전 화면 `Scaffold`/`TopAppBar`/`Card`/`FilterChip`/`AssistChip`/`OutlinedTextField`/`LinearProgressIndicator` 등 |
| `material-icons-core` | `LauncherScreen.kt` `Icons.Filled.{Add,Delete,Info,Search,Settings}` |
| `activity-compose` | 신규 `LauncherActivity`(integration-guide §2)가 `setContent { AlrApp(...) }` 호출 |
| `lifecycle-runtime-ktx` | `RunningSurfaceActivity.kt` `lifecycleScope`/`repeatOnLifecycle` |
| `lifecycle-viewmodel-compose` | `AlrApp.kt` 각 destination 의 `viewModel(factory = …)` |
| `navigation-compose` | `AlrApp.kt` `NavHost`/`composable`/`rememberNavController`/`NavHostController` |
| `kotlinx-coroutines-android` | `runtime/AlrRuntime.kt`·`FakeAlrRuntime.kt`(`StateFlow`/`Flow`/`Mutex`); `SettingsScreen.kt`(`MutableStateFlow`) |
| `coil-compose` (선택) | `LauncherScreen.AppIcon`·`CatalogScreen` 아이콘 디코드(현재 주석 처리된 폴백 자리) |

> `material-icons-extended` 는 **불필요**하다 — 현 화면이 쓰는 아이콘은 전부
> `material-icons-core` 의 `Icons.Filled.*` 다. (확장 아티팩트는 메서드 수가 매우 커서
> 굳이 넣지 않는다.) `SettingsScreen.kt` 의 백 글리프는 텍스트("‹")라 아이콘 의존이 없고,
> 통합 시 `Icons.AutoMirrored.Filled.ArrowBack` 으로 바꾸려면 `material-icons-core` 로 충분하다.

---

## 5. 버전 정합표 (왜 이 버전인가)

| 의존 | 버전 | 정합 근거 |
|---|---|---|
| Compose Compiler 플러그인 | `2.0.21` | **Kotlin 버전과 동일해야 함**(2.0 부터 컴파일러가 Kotlin 에 포함) |
| `compose-bom` | `2024.09.00` | Compose Compiler 2.0.x / AGP 8.5+ 와 정합; Material3 1.3.x·Foundation 1.7.x 일괄 고정 |
| `activity-compose` | `1.9.2` | compileSdk 35 / Compose 1.7 정합, minSdk 26 OK |
| `navigation-compose` | `2.8.1` | Compose 1.7 / Kotlin 2.0 정합 |
| `lifecycle-*` | `2.8.6` | `viewmodel-compose`·`runtime-ktx` 동일 버전(라이브러리 정합) |
| `kotlinx-coroutines` | `1.8.1` | Kotlin 2.0.x 정합 |
| `coil-compose` | `2.7.0` | Compose 1.7 / minSdk 26 정합(선택) |

> BOM 을 올리면(`2024.09.00` → 상위) 개별 compose 좌표는 그대로 두고 BOM 한 줄만 바꾸면
> 된다. 단 BOM 상향 시 Compose Compiler(=Kotlin) 버전도 함께 봐야 한다.

---

## 6. 매니페스트 `<application android:theme>` 정합 (빌드는 아니지만 함께 봐야 함)

현 매니페스트는 `android:theme="@style/AppTheme"` 이고 `res/values/styles.xml` 의 `AppTheme`
는 `android:style/Theme.Material.Light.NoActionBar` 를 부모로 한다. Compose 화면은 자체
`AlrTheme`(Material3, `ui/theme/Theme.kt`)로 색/타이포를 잡으므로 **플랫폼 테마는 그대로
둬도 동작**한다. 다만 Compose Activity 의 시스템 바/edge-to-edge 를 깔끔히 하려면 통합 시
`Theme.Material3.*` 또는 `Theme.AppCompat.*.NoActionBar` 계열로 바꾸는 것을 고려할 수 있다
(필수 아님 — 별도 androidx.appcompat 의존 유발하므로 v1 은 현 `AppTheme` 유지 권장).

---

## 7. 빌드 검증 체크리스트 (통합 세션이 반영 후)

1. 루트 + app `plugins` 에 `org.jetbrains.kotlin.plugin.compose` 적용됐는가.
2. `android.buildFeatures.compose = true`.
3. `composeOptions` 블록이 **없는가**(있으면 Kotlin 2.0 에서 경고/혼선).
4. `./gradlew :app:compileDebugKotlin` 통과 — `ui/`·`runtime/` import 전부 해소.
5. `AlrApp.kt` 의 4개 placeholder Route 정리 완료(integration-guide §3) — 안 하면
   `LauncherRoute`/`CatalogRoute`/`AppDetailRoute` **redeclaration** 으로 컴파일 실패.
6. (BuildConfig 사용 시) `buildFeatures.buildConfig = true`.

---

## 부록 A. 한 번에 적용하는 통합 패치 미리보기 (참고용 — 본체가 반영)

```kotlin
// app/build.gradle.kts (발췌 — 통합 세션이 반영)
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    // ... 기존 ...
    buildFeatures {
        compose = true
        buildConfig = true   // SettingsScreen 빌드 정보 주입 시
    }
}

dependencies {
    implementation("org.apache.commons:commons-compress:1.26.2")

    val composeBom = platform("androidx.compose:compose-bom:2024.09.00")
    implementation(composeBom)
    androidTestImplementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.activity:activity-compose:1.9.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.6")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.6")
    implementation("androidx.navigation:navigation-compose:2.8.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    implementation("io.coil-kt:coil-compose:2.7.0")             // 선택
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
```

> 이 패치는 **제안**이다. 실제 반영(plugins/buildFeatures/dependencies 편집 + 매니페스트
> 등록)은 통합/본체 세션이 수행한다. product-ux 트랙은 `build.gradle(.kts)`·`MainActivity.kt`·
> `AndroidManifest.xml`·version stamp 를 직접 건드리지 않는다.
