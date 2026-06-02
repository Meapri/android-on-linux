from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"


def test_native_runtime_report_has_gpu_boundary_probe():
    text = CPP.read_text()
    assert "nativeAlrGpuBoundaryProbe" in text
    assert "build_gpu_boundary_probe" in text
    assert "ALR GPU BOUNDARY PROBE: " in text
    # Three boundary mechanisms are measured.
    assert "inproc dispatch ns/op" in text
    assert "socket per-cmd ns/op" in text
    assert "shmem-ring ns/op" in text
    assert "MAP_SHARED | MAP_ANONYMOUS" in text
    assert "socketpair" in text
    # Reported as commands that fit a 60fps frame budget.
    assert "cmds per 60fps frame" in text


def test_main_activity_reports_gpu_passthrough_boundary_verdict():
    text = MAIN.read_text()
    assert "nativeAlrGpuBoundaryProbe" in text
    assert "alrGpuBoundaryProbe" in text
    assert "ALR GPU PASSTHROUGH BOUNDARY: " in text
    assert "gpuPassthroughBoundaryViable" in text
    assert "ALR guest->host GPU boundary cost probe:" in text
    assert "build: 0.4.149-cr1-v149" in text
