/*
 * Theme — ALR 인앱 UI 의 Material3 테마(다크/라이트 + 컬러 스킴).
 *
 * 신규 Compose 화면(Launcher/Catalog/AppDetail/Settings)의 공통 테마 루트. 실행화면
 * RunningSurface(View)는 풀스크린 컴포지터라 테마 비대상(ADR-004 §4-D2).
 *
 * 빌드 미통합: Compose/Material3 의존성은 통합 세션이 build.gradle 에 추가한다.
 * 본 파일은 *소스 골격* 이며 의존성 추가 전까지 import 미해소는 의도된 상태.
 * build.gradle·MainActivity.kt·AndroidManifest.xml 은 본 트랙이 건드리지 않는다.
 *
 * dynamicColor(Android 12+ Material You)는 통합 세션이 빌드/정책에 맞춰 켤 수 있도록
 * 파라미터로 노출(기본 false — 브랜드 색을 우선, 결정적 Preview 보장).
 *
 * 소유: 기반 트랙(ui/theme 신규).
 */
package dev.chanwoo.androlinux.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

// --------------------------------------------------------------------------- //
// 브랜드 컬러 — "AndroLinux": 리눅스/터미널 그린 + 안드로이드 친화 톤
// --------------------------------------------------------------------------- //

private val BrandGreen = Color(0xFF2E7D32)
private val BrandGreenLight = Color(0xFF60AD5E)
private val BrandGreenDark = Color(0xFF005005)
private val AccentAmber = Color(0xFFFFB300)
private val AccentAmberDark = Color(0xFFC68400)
private val SurfaceDark = Color(0xFF121212)
private val SurfaceLight = Color(0xFFFAFAF7)

private val LightColors = lightColorScheme(
    primary = BrandGreen,
    onPrimary = Color.White,
    primaryContainer = BrandGreenLight,
    onPrimaryContainer = Color(0xFF00210B),
    secondary = AccentAmber,
    onSecondary = Color(0xFF3A2A00),
    background = SurfaceLight,
    onBackground = Color(0xFF1A1C19),
    surface = SurfaceLight,
    onSurface = Color(0xFF1A1C19),
    error = Color(0xFFBA1A1A),
    onError = Color.White,
)

private val DarkColors = darkColorScheme(
    primary = BrandGreenLight,
    onPrimary = Color(0xFF00390C),
    primaryContainer = BrandGreenDark,
    onPrimaryContainer = Color(0xFFC8E6C9),
    secondary = AccentAmber,
    onSecondary = Color(0xFF3A2A00),
    secondaryContainer = AccentAmberDark,
    background = SurfaceDark,
    onBackground = Color(0xFFE2E3DE),
    surface = SurfaceDark,
    onSurface = Color(0xFFE2E3DE),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
)

/**
 * ALR Compose 테마 루트. 모든 신규 화면을 이걸로 감싼다(AlrApp 이 적용).
 *
 * @param darkTheme 다크 모드 여부(기본: 시스템 설정 추종).
 * @param dynamicColor Android 12+ Material You 동적 색 사용(기본 false — 브랜드 색 우선).
 */
@Composable
fun AlrTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit,
) {
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context)
            else dynamicLightColorScheme(context)
        }
        darkTheme -> DarkColors
        else -> LightColors
    }
    MaterialTheme(
        colorScheme = colorScheme,
        content = content,
    )
}
