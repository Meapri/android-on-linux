from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = ROOT / "app/src/main/assets/rootfs/payloads/tiny-rootfs.tar"
MAIN = ROOT / "app/src/main/java/dev/chanwoo/androlinux/MainActivity.kt"
RUNNER = ROOT / "app/src/main/java/dev/chanwoo/androlinux/NativeCommandRunner.kt"
CPP = ROOT / "app/src/main/cpp/runtime_report.cpp"


def test_rootfs_contains_guest_gpu_ipc_client_and_gles_shim():
    with tarfile.open(PAYLOAD) as archive:
        names = set(archive.getnames())
        for name in [
            "./usr/bin/alr-gpu-client",
            "./usr/bin/alr-gles-shim-smoke",
            "./usr/lib/androlinux/libalr_gles_shim.so",
            "./usr/bin/alr-wayland-gpu-client",
            "./usr/bin/alr-x11-gpu-client",
        ]:
            assert name in names
            assert archive.getmember(name).mode & 0o111
        payload = archive.extractfile("./usr/share/androlinux/gpu-bridge.txt").read().decode()
        assert "tcp-loopback" in payload
        assert "multi-frame" in payload
        assert "gles-shim-smoke" in payload
        gui_payload = archive.extractfile("./usr/share/androlinux/gui-gpu-bridge.txt").read().decode()
        assert "Wayland/X11" in gui_payload
        assert "ALR_GUI_FRAME" in gui_payload


def test_android_runs_loopback_ipc_bridge_and_reports_loss_metrics():
    runner = RUNNER.read_text()
    assert "runProotRootfsGuestGpuClientIpc" in runner
    assert "ALR_GPU_BRIDGE_PORT" in runner
    text = MAIN.read_text()
    assert "ServerSocket(0, 1, InetAddress.getByName(host))" in text
    assert "GUEST GPU IPC BRIDGE EXECUTION" in text
    assert "GUEST GLES SHIM SMOKE EXECUTION" in text
    assert "GUEST GPU MULTI-FRAME SURFACE EXECUTION" in text
    assert "guest gpu ipc received frames" in text
    assert "guest gpu ipc lossless" in text
    assert "Linux guest Wayland/X11 GUI GPU surface renderer" in text
    assert "GUEST WAYLAND GUI GPU BRIDGE EXECUTION" in text
    assert "GUEST X11 GUI GPU BRIDGE EXECUTION" in text
    assert "GUEST GUI GPU SURFACE EXECUTION" in text
    assert "runGuestGuiBridge" in text
    assert "ALR_GUI_IPC_ACK" in text
    assert "guest wayland gui ipc seq gaps" in text
    assert "guest x11 gui ipc seq gaps" in text


def test_native_surface_renderer_accepts_multi_frame_stream_and_reports_bridge():
    text = CPP.read_text()
    assert "parse_surface_frames" in text
    assert "surface requested frames=" in text
    assert "surface wayland frames rendered=" in text
    assert "surface x11 frames rendered=" in text
    assert "surface gui total frames rendered=" in text
    assert "surface frames rendered=" in text
    assert "surface frames dropped=" in text
    assert "surface frame lossless=" in text
    assert "guest gpu ipc bridge hardware render=" in text
    assert "guest gui gpu compositor hardware render=" in text
    assert "guest wayland/x11 gui gpu surface hardware render=" in text
    assert "eglSwapBuffers" in text
