from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_main_activity_places_execution_summary_before_verbose_report():
    text = MAIN.read_text()
    summary_index = text.index("execution summary")
    verbose_index = text.index("nativeRuntimeReport(")
    assert summary_index < verbose_index
    assert "rootfs verified=${rootfsStatus.verified}" in text
    assert "proot --version exit=${prootCandidateResult.exitCode}" in text
    assert "ROOTFS EXECUTION: ${if (rootfsExecutionPassed) \"PASS\" else \"FAIL\"}" in text
    assert "proot hello quiet exit=${prootHelloResult.exitCode}" in text


def test_main_activity_uses_scrollable_selectable_report_text():
    text = MAIN.read_text()
    assert "ScrollView" in text
    assert "setTextIsSelectable(true)" in text


def test_report_rejects_android_env_leaks_in_guest_userland_summary():
    text = MAIN.read_text()
    assert "val androidEnvLeakPrefixes = listOf(" in text
    assert "ANDROID_ROOT=" in text
    assert "BOOTCLASSPATH=" in text
    assert "DEX2OATBOOTCLASSPATH=" in text
    assert "prootDashResult.stdout.lineSequence().any" in text
    assert "!guestEnvLeakedAndroidVars" in text
