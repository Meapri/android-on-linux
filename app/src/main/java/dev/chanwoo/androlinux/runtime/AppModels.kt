/*
 * AppModels — ALR 인앱 카탈로그/런처 UI 가 다루는 순수 데이터 모델(Kotlin).
 *
 * host-검증 파이썬 모델(tools 디렉터리)과 1:1 대응 — Kotlin 측은 표시/상태 만 들고,
 * 파싱·검증·closure 해결은 런타임(또는 빌드 파이프라인)이 그 파이썬 규칙으로 한다:
 *   InstalledApp / CatalogApp        ← tools/alr_manifest.py  AppManifest
 *   LaunchEntry                      ← tools/desktop_entry.py LauncherEntry / AppEntry
 *   AppPermission(enum 7종)          ← tools/permission_map.py GuestPermission
 *   InstallProgress                  ← tools/install_plan.py 진행 단계(stage-tar 설치)
 *   AppCategory(enum 10종)           ← tools/alr_manifest.py CATEGORIES
 *   RootfsDep / RootfsDepKind        ← tools/alr_manifest.py RootfsDep
 *
 * 런타임 독립: 이 파일은 Android/네이티브 의존이 없다(순수 코틀린 data class/enum).
 * UI 4트랙(Launcher/Catalog/AppDetail/Settings)과 FakeAlrRuntime 가 이 모델만 본다.
 *
 * 빌드 미통합: Compose 의존성은 통합 세션이 build.gradle 에 추가. 단 이 파일 자체는
 * Compose 비의존(순수 모델)이라 의존성과 무관하게 컴파일 가능. build.gradle·
 * MainActivity.kt·AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * 소유: 기반 트랙(runtime/ 신규 파일). ADR-004 §5-F 계약의 데이터 절반.
 */
package dev.chanwoo.androlinux.runtime

/**
 * 카탈로그 카테고리 — tools/alr_manifest.py 의 닫힌 CATEGORIES(10종)와 1:1.
 *
 * `id` 는 파이썬 문자열 값(예: "graphics")과 동일 — 매니페스트 JSON round-trip 키.
 * `label` 은 런처/카탈로그의 한글 표시명(필터 칩·섹션 헤더). 닫힌 enum 이라 알 수
 * 없는 카테고리는 표현 불가 → 카탈로그 빌더(파이썬)가 utility 로 폴백해 넘긴다.
 */
enum class AppCategory(val id: String, val label: String) {
    GRAPHICS("graphics", "그래픽"),
    DEVELOPMENT("development", "개발"),
    OFFICE("office", "오피스"),
    INTERNET("internet", "인터넷"),
    MULTIMEDIA("multimedia", "미디어"),
    GAMES("games", "게임"),
    SYSTEM("system", "시스템"),
    UTILITY("utility", "유틸리티"),
    EDUCATION("education", "교육"),
    TERMINAL("terminal", "터미널");

    companion object {
        /** 파이썬 id 문자열 → enum. 모르는 값은 UTILITY 폴백(파서가 이미 보장하나 방어). */
        fun fromId(id: String): AppCategory =
            entries.firstOrNull { it.id == id } ?: UTILITY
    }
}

/**
 * 게스트 앱이 요구하는 권한 부류 — tools/permission_map.py GuestPermission(7종)과 1:1.
 *
 * `id` 는 파이썬 문자열 값(예: "storage-read"). T4(권한 매핑) 트랙이 각 부류를 실제
 * Android 권한/SAF/no-op 으로 사상한다 — UI는 *어떤 부류를 요구하는지* 표시만 하고,
 * 실제 매핑/프롬프트 여부는 런타임이 permission_map 규칙으로 결정한다.
 */
enum class AppPermission(val id: String, val label: String) {
    STORAGE_READ("storage-read", "저장소 읽기"),
    STORAGE_WRITE("storage-write", "저장소 쓰기"),
    CAMERA("camera", "카메라"),
    MICROPHONE("microphone", "마이크"),
    LOCATION("location", "위치"),
    NETWORK("network", "네트워크"),
    NOTIFICATIONS("notifications", "알림");

    companion object {
        /** 파이썬 id 문자열 → enum, 모르면 null(알 수 없는 권한은 표시에서 제외). */
        fun fromId(id: String): AppPermission? = entries.firstOrNull { it.id == id }
    }
}

/** rootfs 의존 종류 — tools/alr_manifest.py DEP_KINDS(stage-tar | apt)와 1:1. */
enum class RootfsDepKind(val id: String) {
    STAGE_TAR("stage-tar"),
    APT("apt");

    companion object {
        fun fromId(id: String): RootfsDepKind =
            entries.firstOrNull { it.id == id } ?: STAGE_TAR
    }
}

/**
 * 하나의 rootfs 요구 — overlay stage-tar 또는 apt 패키지.
 * tools/alr_manifest.py RootfsDep 와 1:1(kind/ref/install_size_bytes).
 *
 * kind==STAGE_TAR: `ref` 는 overlay basename(예 "gimp-stage.tar"),
 *   RootfsInstaller.extractOverlayTar 가 풀고 `.{name}-staged-<size>` 마커를 남김.
 * kind==APT: `ref` 는 Ubuntu noble 패키지명(v2 인-게스트 apt 설치 대상).
 */
data class RootfsDep(
    val kind: RootfsDepKind,
    val ref: String,
    val installSizeBytes: Long = 0,
)

/**
 * 런처가 실행할 진입점 — tools/desktop_entry.py LauncherEntry / alr_manifest.py AppEntry 대응.
 *
 * kind==EXEC: `target` 은 rootfs 절대 바이너리 경로(예 "/usr/bin/gimp"), `args` 는 argv[1..].
 * kind==DESKTOP: `target` 은 rootfs `.desktop` 경로 — 런타임이 Exec=/Icon= 을 launch 시 해석,
 *   `args` 는 비어 있어야 함(.desktop 이 커맨드라인 소유).
 *
 * UI는 이 entry 로 LaunchRequest 를 만든다(하드코딩 GIMP 경로 제거 — ADR-004 §7).
 */
data class LaunchEntry(
    val kind: EntryKind,
    val target: String,
    val args: List<String> = emptyList(),
) {
    enum class EntryKind(val id: String) {
        EXEC("exec"),
        DESKTOP("desktop");

        companion object {
            fun fromId(id: String): EntryKind =
                entries.firstOrNull { it.id == id } ?: EXEC
        }
    }
}

/**
 * 표면 표시 힌트 — tools/alr_manifest.py DisplaySpec 대응(mode/width/height).
 * width/height==0 은 미설정(컴포지터가 device surface 로 클램프).
 */
data class DisplaySpec(
    val mode: DisplayMode = DisplayMode.WINDOWED,
    val width: Int = 0,
    val height: Int = 0,
) {
    enum class DisplayMode(val id: String) {
        WINDOWED("windowed"),
        FULLSCREEN("fullscreen");

        companion object {
            fun fromId(id: String): DisplayMode =
                entries.firstOrNull { it.id == id } ?: WINDOWED
        }
    }
}

/**
 * 카탈로그에 보이는 *설치 가능* 앱 — tools/alr_manifest.py AppManifest 와 1:1.
 *
 * appId 는 reverse-DNS 안정 id(예 "org.gimp.GIMP" 또는 apt 합성 "org.debian.foot").
 * 카탈로그 화면(T3)이 이 모델로 카드를 그리고, [설치] 시 appId 로 install() 을 부른다.
 * 설치 후에는 같은 appId 의 InstalledApp 으로 런처 그리드에 등장한다.
 */
data class CatalogApp(
    val appId: String,
    val name: String,
    val summary: String,
    val entry: LaunchEntry,
    val category: AppCategory = AppCategory.UTILITY,
    val description: String = "",
    val iconPath: String? = null,
    val rootfsDeps: List<RootfsDep> = emptyList(),
    val requiredPermissions: List<AppPermission> = emptyList(),
    val display: DisplaySpec = DisplaySpec(),
    val minRuntime: String = "0.1",
    val installSizeBytes: Long = 0,
    /** 스크린샷 rootfs/원격 경로(AppDetail 갤러리). 빈 리스트면 미제공. */
    val screenshots: List<String> = emptyList(),
    val source: AppSource = AppSource.BUNDLED,
    /**
     * X11 전용 앱 라우팅 플래그(TASK-A). true 면 이 앱은 libwayland-client 를 링크하지
     * 않고 libX11/libxcb 만 링크하는 **X11-only** 클라이언트라, ALR 의 네이티브 Wayland
     * 컴포지터에 직접 붙을 수 없다(DISPLAY 없음 → "cannot open display"). 런치 경로
     * (NativeAppSession.XwaylandLaunch)가 이 앱에 한해 ROOTFUL Xwayland :0 을 띄우고
     * DISPLAY=:0 를 게스트 환경에 주입해, X11 클라이언트가 Xwayland→wl_shm→SurfaceView
     * 로 렌더되게 한다(xcalc 가 device-proven 으로 검증한 경로). Wayland-가능 앱(GTK3/GTK4/
     * Qt-wayland 등)은 false 로 두어 종전과 바이트 동일하게 컴포지터에 직접 붙는다.
     *
     * 판별 근거(host): tools/elf_needed.needed_of 로 EXEC 바이너리의 DT_NEEDED 를 보면
     * X11-only 앱은 libX11.so.6(±libgtk-x11-2.0)만 있고 libwayland-client.so.0 가 없다
     * (xzgv/xli device-host-audited). 카탈로그 엔트리에 명시하여 런치 라우팅을 결정한다.
     */
    val needsXwayland: Boolean = false,
) {
    /**
     * 카탈로그 UI 의 "다운로드 X MB" 단일 숫자 — alr_manifest.total_install_size_bytes 미러:
     * 명시 installSizeBytes 가 있으면 그것, 없으면 deps 크기 합.
     */
    val totalInstallSizeBytes: Long
        get() = if (installSizeBytes > 0) installSizeBytes
        else rootfsDeps.sumOf { it.installSizeBytes }
}

/** 카탈로그 엔트리의 출처 — 번들(APK 동봉 stage-tar) vs apt 인덱스 합성. */
enum class AppSource(val label: String) {
    BUNDLED("번들"),
    APT("apt 저장소"),
}

/**
 * *설치 완료* 되어 런처 그리드에 보이는 앱 — AppManifest 의 설치-후 뷰.
 *
 * CatalogApp 의 부분집합 + 설치 상태. Launcher(T3)가 이 모델로 타일을 그리고,
 * 탭 시 entry 로 LaunchRequest 를 만들어 AlrRuntime.launch 를 부른다.
 * ui/LauncherScreen.kt 의 AppEntry(표시 전용)와 필드가 대응한다(appId/name/icon/category).
 */
data class InstalledApp(
    val appId: String,
    val name: String,
    val summary: String,
    val entry: LaunchEntry,
    val category: AppCategory = AppCategory.UTILITY,
    val iconPath: String? = null,
    val requiredPermissions: List<AppPermission> = emptyList(),
    val display: DisplaySpec = DisplaySpec(),
    val installedSizeBytes: Long = 0,
    val version: String = "",
)

/**
 * 설치/제거 트랜잭션의 관찰 가능한 진행 — install()/uninstall() 가 흘리는 Flow 원소.
 *
 * tools/install_plan.py 의 단계(closure 해결 → stage-tar 추출 → 마커)와 의미 대응하되,
 * UI 는 *진행률 0..100 + 단계 라벨* 만 본다(ADR-004 §7: "UI는 진행률만 본다").
 * percent 는 단조 증가(FakeAlrRuntime 가 보장), 완료 시 Done, 실패 시 Failed.
 */
sealed interface InstallProgress {
    val appId: String

    /** 진행 중 — percent(0..100) + 사람이 읽는 단계 라벨(예 "오버레이 적용 중"). */
    data class Running(
        override val appId: String,
        val percent: Int,
        val stage: InstallStage,
    ) : InstallProgress

    /** 정상 완료 — 이 시점에 InstalledApp 이 installedApps 에 등장. */
    data class Done(override val appId: String) : InstallProgress

    /** 실패 — message 는 진단용(가드 충돌/네트워크/closure 미해결 등). */
    data class Failed(
        override val appId: String,
        val message: String,
    ) : InstallProgress
}

/**
 * 설치 진행 단계 — v2(인-게스트 dpkg/apt) 파이프라인의 사용자-가시 페이즈.
 * RESOLVING(의존성/closure 해결) → DOWNLOADING(.deb/stage-tar 수신) →
 * UNPACKING(dpkg unpack: `.dpkg-new` 전개 → unpacked=true) →
 * CONFIGURING(dpkg configure: maintainer script "Setting up …" → configured=true) →
 * REGISTERING(설치 완료 등록 → installed; status="install ok installed").
 * 제거는 REMOVING 단일.
 *
 * v2 device 달성(ws-1 `7f45def`, v163): 비root `dpkg -i hello.deb`가
 * unpacked=true configured=true installed=true 도달 — UNPACKING/CONFIGURING 페이즈가
 * 실제 dpkg unpack/configure 단계에 대응한다. v1(stage-tar) 폴백 경로는 UNPACKING을
 * extractOverlayTar 로, CONFIGURING 을 no-op(설정 불요)로 흘린다. (EXTRACTING 은
 * UNPACKING 으로 명칭 통합 — v1 stage-tar 의 "오버레이 적용"도 unpack 의미.)
 */
enum class InstallStage(val label: String) {
    RESOLVING("의존성 해결 중"),
    DOWNLOADING("내려받는 중"),
    UNPACKING("푸는 중"),
    CONFIGURING("설정 중"),
    REGISTERING("등록 중"),
    REMOVING("제거 중"),
}
